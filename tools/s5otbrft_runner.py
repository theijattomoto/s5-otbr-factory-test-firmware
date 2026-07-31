#!/usr/bin/env python3
"""Phase 1 partial automation for S5 Node-OTBR factory-test firmware."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

PROTOCOL_PREFIX = "@S5OTBRFT "
TOOL_VERSION = "0.1.0"
DEFAULT_PROFILE = (
    Path(__file__).resolve().parent
    / "profiles"
    / "s5-otbr-phase1-partial-v1.json"
)


class S5OTBRFTError(RuntimeError):
    """Base station-runner error."""


class ProtocolError(S5OTBRFTError):
    """Serial framing or protocol-contract failure."""


class ValidationError(S5OTBRFTError):
    """DUT data does not satisfy the selected profile."""


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def load_profile(path: Path) -> tuple[dict[str, Any], str]:
    raw = path.read_bytes()
    try:
        profile = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValidationError(f"Invalid profile {path}: {exc}") from exc

    required = {
        "profile_id",
        "production_release",
        "expected",
        "required_passed_tests",
        "required_pending_tests",
    }
    missing = sorted(required - profile.keys())
    if missing:
        raise ValidationError(f"Profile is missing: {', '.join(missing)}")
    if profile["production_release"] is not False:
        raise ValidationError("Partial runner requires production_release=false")
    if not isinstance(profile["expected"], dict):
        raise ValidationError("Profile expected must be an object")
    for name in ("required_passed_tests", "required_pending_tests"):
        values = profile[name]
        if not isinstance(values, list) or not all(
            isinstance(value, str) and value for value in values
        ):
            raise ValidationError(f"Profile {name} must be a string array")
    return profile, hashlib.sha256(raw).hexdigest().upper()


class S5OTBRFTClient:
    """Persistent line accumulator and sequence-matched protocol client."""

    def __init__(
        self,
        serial_port: Any,
        port_name: str,
        monotonic: Callable[[], float] = time.monotonic,
    ) -> None:
        self.serial = serial_port
        self.port_name = port_name
        self._monotonic = monotonic
        self._rx_buffer = bytearray()
        self._next_sequence = 1
        self.transcript: list[dict[str, Any]] = []

    def _record(self, direction: str, line: str) -> None:
        self.transcript.append(
            {"timestamp_utc": utc_now(), "direction": direction, "line": line}
        )

    def _pop_line(self) -> str | None:
        positions = [
            position
            for delimiter in (b"\r", b"\n")
            if (position := self._rx_buffer.find(delimiter)) >= 0
        ]
        if not positions:
            return None
        end = min(positions)
        raw = bytes(self._rx_buffer[:end])
        del self._rx_buffer[: end + 1]
        while self._rx_buffer[:1] in (b"\r", b"\n"):
            del self._rx_buffer[:1]
        try:
            return raw.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise ProtocolError("DUT returned invalid UTF-8") from exc

    def _read_frame(self, timeout_s: float) -> dict[str, Any]:
        deadline = self._monotonic() + timeout_s
        while self._monotonic() < deadline:
            line = self._pop_line()
            if line is not None:
                if not line:
                    continue
                self._record("rx", line)
                if not line.startswith(PROTOCOL_PREFIX):
                    continue
                try:
                    frame = json.loads(line[len(PROTOCOL_PREFIX) :])
                except json.JSONDecodeError as exc:
                    raise ProtocolError(f"Malformed DUT JSON: {line}") from exc
                if not isinstance(frame, dict):
                    raise ProtocolError("DUT payload must be a JSON object")
                return frame

            waiting = int(getattr(self.serial, "in_waiting", 0) or 0)
            chunk = self.serial.read(max(1, waiting))
            if chunk:
                self._rx_buffer.extend(chunk)
            else:
                time.sleep(0.005)
        raise ProtocolError(f"Timed out waiting for DUT on {self.port_name}")

    def command(
        self, command: str, *, timeout_s: float = 3.0, **fields: Any
    ) -> dict[str, Any]:
        sequence = self._next_sequence
        self._next_sequence += 1
        request = {"seq": sequence, "cmd": command, **fields}
        line = PROTOCOL_PREFIX + json.dumps(
            request, separators=(",", ":"), ensure_ascii=True
        )
        self._record("tx", line)
        encoded = (line + "\n").encode("utf-8")
        written = self.serial.write(encoded)
        if written is not None and written != len(encoded):
            raise ProtocolError(
                f"Partial serial write: expected {len(encoded)}, wrote {written}"
            )
        if hasattr(self.serial, "flush"):
            self.serial.flush()

        deadline = self._monotonic() + timeout_s
        while True:
            remaining = deadline - self._monotonic()
            if remaining <= 0:
                raise ProtocolError(
                    f"Timed out waiting for seq={sequence} cmd={command}"
                )
            response = self._read_frame(remaining)
            if response.get("seq") == 0:
                if response.get("cmd") in {"ready", "session.timeout"}:
                    continue
                raise ProtocolError(
                    f"Unexpected unsolicited frame: {response!r}"
                )
            if response.get("seq") != sequence:
                raise ProtocolError(
                    f"Sequence mismatch: expected {sequence}, "
                    f"received {response.get('seq')!r}"
                )
            if response.get("cmd") != command:
                raise ProtocolError(
                    f"Command mismatch: expected {command}, "
                    f"received {response.get('cmd')!r}"
                )
            validate_envelope(response)
            return response

    def observe_ready(self, timeout_s: float = 0.5) -> bool:
        deadline = self._monotonic() + timeout_s
        while self._monotonic() < deadline:
            try:
                frame = self._read_frame(max(0.001, deadline - self._monotonic()))
            except ProtocolError as exc:
                if str(exc).startswith("Timed out waiting"):
                    return False
                raise
            if frame.get("seq") == 0 and frame.get("cmd") == "ready":
                validate_envelope(frame)
                return True
        return False


def validate_envelope(response: dict[str, Any]) -> None:
    required = {
        "seq",
        "cmd",
        "status",
        "code",
        "firmware",
        "protocol",
        "product",
        "board",
        "data",
    }
    missing = sorted(required - response.keys())
    if missing:
        raise ProtocolError(f"Response envelope is missing: {', '.join(missing)}")
    if response["status"] not in {"ok", "error"}:
        raise ProtocolError(f"Invalid response status: {response['status']!r}")
    if not isinstance(response["data"], dict):
        raise ProtocolError("Response data must be an object")


def require_ok(response: dict[str, Any], command: str) -> dict[str, Any]:
    if response.get("status") != "ok":
        raise ValidationError(
            f"{command} failed: code={response.get('code')!r}"
        )
    data = response.get("data")
    if not isinstance(data, dict):
        raise ProtocolError(f"{command} response data must be an object")
    return data


def validate_identity(
    response: dict[str, Any], profile: dict[str, Any]
) -> dict[str, Any]:
    data = require_ok(response, "identity")
    expected = profile["expected"]
    actual = {
        "product": response.get("product"),
        "board": response.get("board"),
        "firmware": response.get("firmware"),
        "protocol": response.get("protocol"),
        "target": data.get("target"),
        "flash_size_bytes": data.get("flash_size_bytes"),
    }
    mismatches = [
        f"{name}: expected {expected[name]!r}, received {actual[name]!r}"
        for name in expected
        if actual.get(name) != expected[name]
    ]
    if mismatches:
        raise ValidationError("Identity mismatch: " + "; ".join(mismatches))

    mac = data.get("base_mac")
    if not isinstance(mac, str) or re.fullmatch(
        r"(?:[0-9A-F]{2}:){5}[0-9A-F]{2}", mac
    ) is None:
        raise ValidationError("Identity has no valid uppercase base MAC")
    eui64 = data.get("thread_eui64")
    if not isinstance(eui64, str) or re.fullmatch(
        r"(?:[0-9A-F]{2}:){7}[0-9A-F]{2}", eui64
    ) is None:
        raise ValidationError("Identity has no valid uppercase IEEE EUI-64")
    return data


def derive_unit_id(identity: dict[str, Any]) -> str:
    compact = re.sub(r"[^0-9A-F]", "", identity["base_mac"])
    return f"ENG-{compact}"


def summarize_manifest(data: dict[str, Any]) -> dict[str, list[str]]:
    tests = data.get("tests")
    if not isinstance(tests, list):
        raise ProtocolError("session.list response has no tests array")
    summary = {"passed": [], "pending": [], "failed": []}
    for test in tests:
        if not isinstance(test, dict) or not isinstance(test.get("id"), str):
            raise ProtocolError("Malformed manifest entry")
        bucket = {"pass": "passed", "pending": "pending", "fail": "failed"}.get(
            test.get("status")
        )
        if bucket is None:
            raise ProtocolError(
                f"Unknown manifest status: {test.get('status')!r}"
            )
        summary[bucket].append(test["id"])
    return summary


def best_effort_command(
    client: S5OTBRFTClient,
    command: str,
    errors: list[str],
    **fields: Any,
) -> bool:
    try:
        require_ok(client.command(command, **fields), command)
        return True
    except Exception as exc:  # Cleanup must continue after one failure.
        errors.append(f"{command}: {exc}")
        return False


def run_partial_self_test(
    client: S5OTBRFTClient,
    profile: dict[str, Any],
    profile_hash: str,
    requested_unit_id: str | None = None,
) -> dict[str, Any]:
    errors: list[str] = []
    report: dict[str, Any] = {
        "schema_version": 1,
        "tool": {"name": "s5otbrft_runner", "version": TOOL_VERSION},
        "operation": "self-test",
        "profile": {
            "id": profile["profile_id"],
            "sha256": profile_hash,
            "production_release": False,
        },
        "production_release": False,
        "port": client.port_name,
        "started_utc": utc_now(),
        "ended_utc": None,
        "result": "RUNNING",
        "unit_id": requested_unit_id,
        "identity": None,
        "manifest": {"passed": [], "pending": [], "failed": []},
        "errors": errors,
        "safety_cleanup": {"safe": False, "abort": False},
        "raw_exchange": client.transcript,
    }

    try:
        identity_response = client.command("identity")
        identity = validate_identity(identity_response, profile)
        report["identity"] = {
            **identity,
            "product": identity_response["product"],
            "board": identity_response["board"],
            "firmware": identity_response["firmware"],
            "protocol": identity_response["protocol"],
        }
        unit_id = requested_unit_id or derive_unit_id(identity)
        report["unit_id"] = unit_id

        require_ok(
            client.command("session.start", unit_id=unit_id),
            "session.start",
        )
        manifest_data = require_ok(
            client.command("session.list"), "session.list"
        )
        report["manifest"] = summarize_manifest(manifest_data)

        passed = set(report["manifest"]["passed"])
        pending = set(report["manifest"]["pending"])
        missing_passed = sorted(
            set(profile["required_passed_tests"]) - passed
        )
        missing_pending = sorted(
            set(profile["required_pending_tests"]) - pending
        )
        if missing_passed:
            raise ValidationError(
                "Required automatic tests not passed: "
                + ", ".join(missing_passed)
            )
        if missing_pending:
            raise ValidationError(
                "Expected future tests are not pending: "
                + ", ".join(missing_pending)
            )
        if report["manifest"]["failed"]:
            raise ValidationError(
                "Manifest contains failures: "
                + ", ".join(report["manifest"]["failed"])
            )
        if not report["manifest"]["pending"]:
            raise ValidationError("Partial run must retain pending tests")
        report["result"] = "PARTIAL"
    except Exception as exc:
        errors.append(str(exc))
        report["result"] = "FAIL"
    finally:
        report["safety_cleanup"]["safe"] = best_effort_command(
            client, "safe", errors
        )
        report["safety_cleanup"]["abort"] = best_effort_command(
            client, "session.abort", errors
        )
        if not all(report["safety_cleanup"].values()):
            report["result"] = "FAIL"
        report["ended_utc"] = utc_now()
        report["raw_exchange"] = list(client.transcript)
    return report


def write_report(report: dict[str, Any], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def default_report_path(unit_id: str | None) -> Path:
    safe_unit = re.sub(r"[^A-Za-z0-9_.-]", "_", unit_id or "UNKNOWN")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return Path("reports") / f"{stamp}_{safe_unit}.json"


def serial_modules() -> tuple[Any, Any]:
    try:
        import serial
        from serial.tools import list_ports
    except ImportError as exc:
        raise S5OTBRFTError(
            "pyserial is required; run: python -m pip install -r "
            "tools/requirements.txt"
        ) from exc
    return serial, list_ports


def open_client(port: str) -> tuple[Any, S5OTBRFTClient]:
    serial, _ = serial_modules()
    handle = serial.Serial(
        port=port,
        baudrate=115200,
        bytesize=8,
        parity="N",
        stopbits=1,
        timeout=0.05,
        write_timeout=1.0,
    )
    time.sleep(0.15)
    return handle, S5OTBRFTClient(handle, port)


def discover_devices(
    profile: dict[str, Any], candidate_port: str | None = None
) -> list[dict[str, Any]]:
    _, list_ports = serial_modules()
    ports = [candidate_port] if candidate_port else [
        item.device for item in list_ports.comports()
    ]
    devices: list[dict[str, Any]] = []
    for port in ports:
        handle = None
        try:
            handle, client = open_client(port)
            ready_seen = client.observe_ready()
            response = client.command("identity", timeout_s=2.5)
            identity = validate_identity(response, profile)
            devices.append(
                {
                    "port": port,
                    "ready_seen": ready_seen,
                    "product": response["product"],
                    "board": response["board"],
                    "firmware": response["firmware"],
                    "protocol": response["protocol"],
                    **identity,
                }
            )
        except Exception:
            if candidate_port:
                raise
        finally:
            if handle is not None:
                handle.close()
    return devices


def select_port(profile: dict[str, Any], explicit_port: str | None) -> str:
    if explicit_port:
        return explicit_port
    devices = discover_devices(profile)
    if not devices:
        raise S5OTBRFTError("No matching S5 Node-OTBR factory DUT found")
    if len(devices) > 1:
        ports = ", ".join(device["port"] for device in devices)
        raise S5OTBRFTError(
            f"Multiple matching DUTs found; specify --port: {ports}"
        )
    return devices[0]["port"]


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="S5 Node-OTBR Phase 1 partial factory automation"
    )
    parser.add_argument(
        "--profile",
        type=Path,
        default=DEFAULT_PROFILE,
        help="Versioned non-production validation profile",
    )
    subparsers = parser.add_subparsers(dest="operation", required=True)

    discover = subparsers.add_parser("discover", help="Find matching DUTs")
    discover.add_argument("--port", help="Probe only this serial port")

    self_test = subparsers.add_parser(
        "self-test", help="Run automatic no-fixture partial test"
    )
    self_test.add_argument("--port", help="Serial port; auto-detected if omitted")
    self_test.add_argument("--unit-id", help="Barcode; defaults to base MAC")
    self_test.add_argument("--output", type=Path, help="JSON report path")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        profile, profile_hash = load_profile(args.profile)
        if args.operation == "discover":
            devices = discover_devices(profile, args.port)
            print(json.dumps({"devices": devices}, indent=2, sort_keys=True))
            return 0 if devices else 2

        port = select_port(profile, args.port)
        handle, client = open_client(port)
        try:
            client.observe_ready()
            report = run_partial_self_test(
                client, profile, profile_hash, args.unit_id
            )
        finally:
            handle.close()

        output = args.output or default_report_path(report["unit_id"])
        write_report(report, output)
        print(f"{report['result']}: {output}")
        if report["result"] == "FAIL":
            for error in report["errors"]:
                print(f"  - {error}", file=sys.stderr)
        return 0 if report["result"] == "PARTIAL" else 1
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
