#!/usr/bin/env python3
"""Partial automation for S5 Node-OTBR factory-test firmware."""

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
TOOL_VERSION = "0.2.0"
DEFAULT_PROFILE = (
    Path(__file__).resolve().parent
    / "profiles"
    / "s5-otbr-partial-v2.json"
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
        "adc_samples",
        "gps_timeout_ms",
        "modem_timeout_ms",
        "guided_led_tests",
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
    for name in ("adc_samples", "gps_timeout_ms", "modem_timeout_ms"):
        value = profile[name]
        if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
            raise ValidationError(f"Profile {name} must be a positive integer")
    if not isinstance(profile["guided_led_tests"], list):
        raise ValidationError("Profile guided_led_tests must be an array")
    return profile, hashlib.sha256(raw).hexdigest().upper()


class S5OTBRFTClient:
    """Persistent line accumulator and sequence-matched protocol client."""

    def __init__(
        self,
        serial_port: Any,
        port_name: str,
        monotonic: Callable[[], float] = time.monotonic,
        verbose: bool = False,
    ) -> None:
        self.serial = serial_port
        self.port_name = port_name
        self._monotonic = monotonic
        self._rx_buffer = bytearray()
        self._next_sequence = 1
        self.verbose = verbose
        self.transcript: list[dict[str, Any]] = []

    def _record(self, direction: str, line: str) -> None:
        self.transcript.append(
            {"timestamp_utc": utc_now(), "direction": direction, "line": line}
        )
        if self.verbose:
            print(f"[{direction.upper()}] {line}", flush=True)

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


def validate_gps(response: dict[str, Any]) -> dict[str, Any]:
    data = require_ok(response, "gps.check")
    if data.get("nmea_received") is not True:
        raise ValidationError("GPS did not receive a checksum-valid RMC sentence")
    count = data.get("valid_rmc_sentences")
    if isinstance(count, bool) or not isinstance(count, int) or count < 1:
        raise ValidationError("GPS response has no valid RMC sentence count")
    if not isinstance(data.get("sample"), str) or not data["sample"].startswith(
        ("$GPRMC,", "$GNRMC,")
    ):
        raise ValidationError("GPS response has no valid RMC sample")
    return data


def validate_modem(response: dict[str, Any]) -> dict[str, Any]:
    data = require_ok(response, "modem.check")
    for field in ("at_ok", "identity_ok", "sim_ready"):
        if data.get(field) is not True:
            raise ValidationError(f"EG912 check failed: {field}=false")
    if not isinstance(data.get("identity"), str) or not data["identity"]:
        raise ValidationError("EG912 response has no model identity")
    return data


def validate_adc(response: dict[str, Any], channel: str) -> dict[str, Any]:
    data = require_ok(response, f"adc.sample {channel}")
    if data.get("channel") != channel:
        raise ValidationError(f"ADC returned wrong channel for {channel}")
    for field in ("samples", "raw_average", "raw_min", "raw_max", "raw_noise"):
        value = data.get(field)
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise ValidationError(f"ADC {channel} has no numeric {field}")
    if data.get("engineering_units_approved") is not False:
        raise ValidationError("ADC snapshot incorrectly claims approved units")
    return data


def prompt_yes_no(
    prompt: str,
    input_func: Callable[[str], str] = input,
    output_func: Callable[[str], None] = print,
) -> bool:
    while True:
        try:
            answer = input_func(prompt).strip().upper()
        except EOFError as exc:
            raise ValidationError(
                "Operator input ended before confirmation"
            ) from exc
        if answer == "Y":
            return True
        if answer == "N":
            return False
        output_func("Please enter Y or N.")


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
    operator_id: str | None = None,
    input_func: Callable[[str], str] = input,
    output_func: Callable[[str], None] = print,
    progress: Callable[[str], None] | None = None,
) -> dict[str, Any]:
    guided = operator_id is not None
    if guided and re.fullmatch(r"[A-Za-z0-9_.-]{1,20}", operator_id) is None:
        raise ValidationError(
            "operator-id must use 1..20 letters, digits, period, underscore "
            "or hyphen"
        )
    errors: list[str] = []
    emit = progress if progress is not None else (lambda _: None)
    report: dict[str, Any] = {
        "schema_version": 1,
        "tool": {"name": "s5otbrft_runner", "version": TOOL_VERSION},
        "operation": "guided-test" if guided else "self-test",
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
        "operator_id": operator_id,
        "identity": None,
        "gps": None,
        "modem": None,
        "adc_snapshots": {},
        "leds": {},
        "manifest": {"passed": [], "pending": [], "failed": []},
        "errors": errors,
        "safety_cleanup": {"safe": False, "abort": False},
        "raw_exchange": client.transcript,
    }

    try:
        emit("[TEST] Device identity and USB protocol")
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
        emit(
            f"[PASS] Identity: {identity['base_mac']} / "
            f"{identity['thread_eui64']}"
        )

        emit(f"[TEST] Start traceable session: {unit_id}")
        require_ok(
            client.command("session.start", unit_id=unit_id),
            "session.start",
        )
        emit("[PASS] Session started")

        emit("[TEST] GPS UART and checksum-valid RMC reception")
        gps_timeout_ms = int(profile["gps_timeout_ms"])
        report["gps"] = validate_gps(
            client.command(
                "gps.check",
                timeout_s=(gps_timeout_ms / 1000.0) + 3.0,
                timeout_ms=gps_timeout_ms,
            )
        )
        emit(
            f"[PASS] GPS UART: "
            f"{report['gps']['valid_rmc_sentences']} valid RMC"
        )

        emit("[TEST] EG912 UART, model identity and SIM readiness")
        modem_timeout_ms = int(profile["modem_timeout_ms"])
        report["modem"] = validate_modem(
            client.command(
                "modem.check",
                timeout_s=(modem_timeout_ms * 3 / 1000.0) + 3.0,
                timeout_ms=modem_timeout_ms,
            )
        )
        emit(f"[PASS] EG912: {report['modem']['identity']}")

        for channel in ("vrms", "irms", "dc5v"):
            emit(f"[TEST] ADC snapshot: {channel}")
            report["adc_snapshots"][channel] = validate_adc(
                client.command(
                    "adc.sample",
                    channel=channel,
                    samples=int(profile["adc_samples"]),
                ),
                channel,
            )
            sample = report["adc_snapshots"][channel]
            emit(
                f"[PASS] ADC {channel}: avg={sample['raw_average']} "
                f"min={sample['raw_min']} max={sample['raw_max']} "
                f"noise={sample['raw_noise']}"
            )

        if guided:
            led_states = (
                (1, "OFF"),
                (0, "ON"),
                (1, "OFF"),
            )
            for led in profile["guided_led_tests"]:
                command_name = led["command_name"]
                display_name = led["display_name"]
                test_id = led["test_id"]
                observations: list[dict[str, Any]] = []
                report["leds"][command_name] = observations
                passed = True
                for level, expected in led_states:
                    emit(f"[TEST] {display_name}: expected {expected}")
                    data = require_ok(
                        client.command(
                            "gpio.write", name=command_name, level=level
                        ),
                        f"gpio.write {command_name}",
                    )
                    confirmed = prompt_yes_no(
                        f"{display_name} should be {expected}. "
                        f"Is it {expected}? [Y/N] ",
                        input_func,
                        output_func,
                    )
                    observations.append(
                        {
                            "timestamp_utc": utc_now(),
                            "gpio": data.get("gpio"),
                            "level": level,
                            "expected_state": expected,
                            "confirmed": confirmed,
                        }
                    )
                    if not confirmed:
                        passed = False
                        break
                    emit(f"[PASS] {display_name}: observed {expected}")

                states = "".join(
                    "Y" if observation["confirmed"] else "N"
                    for observation in observations
                )
                require_ok(
                    client.command(
                        "fixture.record",
                        test_id=test_id,
                        **{"pass": passed},
                        detail=(
                            f"method=operator_visual;op={operator_id};"
                            f"states={states}"
                        ),
                    ),
                    f"fixture.record {test_id}",
                )
                if not passed:
                    raise ValidationError(
                        f"{display_name} did not match the expected state"
                    )

        emit("[TEST] Capture and validate manifest")
        manifest_data = require_ok(
            client.command("session.list"), "session.list"
        )
        report["manifest"] = summarize_manifest(manifest_data)

        passed = set(report["manifest"]["passed"])
        pending = set(report["manifest"]["pending"])
        missing_passed = sorted(
            set(profile["required_passed_tests"]) - passed
        )
        if guided:
            required_leds = {
                led["test_id"] for led in profile["guided_led_tests"]
            }
            missing_passed.extend(sorted(required_leds - passed))
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
        emit(
            f"[PARTIAL] passed={len(report['manifest']['passed'])} "
            f"pending={len(report['manifest']['pending'])} failed=0"
        )
    except Exception as exc:
        errors.append(str(exc))
        report["result"] = "FAIL"
        emit(f"[FAIL] {exc}")
    finally:
        emit("[CLEANUP] Restore safe outputs")
        report["safety_cleanup"]["safe"] = best_effort_command(
            client, "safe", errors
        )
        emit(
            "[PASS] Safe state restored"
            if report["safety_cleanup"]["safe"]
            else "[FAIL] Safe-state command failed"
        )
        emit("[CLEANUP] Abort partial session")
        report["safety_cleanup"]["abort"] = best_effort_command(
            client, "session.abort", errors
        )
        emit(
            "[PASS] Session aborted"
            if report["safety_cleanup"]["abort"]
            else "[FAIL] Session abort failed"
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


def open_client(port: str, verbose: bool = False) -> tuple[Any, S5OTBRFTClient]:
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
    return handle, S5OTBRFTClient(handle, port, verbose=verbose)


def discover_devices(
    profile: dict[str, Any],
    candidate_port: str | None = None,
    verbose: bool = False,
) -> list[dict[str, Any]]:
    _, list_ports = serial_modules()
    ports = [candidate_port] if candidate_port else [
        item.device for item in list_ports.comports()
    ]
    devices: list[dict[str, Any]] = []
    for port in ports:
        handle = None
        try:
            if verbose:
                print(f"[PROBE] {port}", flush=True)
            handle, client = open_client(port, verbose=verbose)
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


def select_port(
    profile: dict[str, Any], explicit_port: str | None, verbose: bool = False
) -> str:
    if explicit_port:
        return explicit_port
    devices = discover_devices(profile, verbose=verbose)
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
        description="S5 Node-OTBR partial factory automation"
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
    discover.add_argument(
        "--verbose", action="store_true", help="Print raw protocol frames"
    )

    self_test = subparsers.add_parser(
        "self-test", help="Run automatic no-fixture partial test"
    )
    self_test.add_argument("--port", help="Serial port; auto-detected if omitted")
    self_test.add_argument("--unit-id", help="Barcode; defaults to base MAC")
    self_test.add_argument("--output", type=Path, help="JSON report path")
    self_test.add_argument(
        "--verbose", action="store_true", help="Print raw protocol frames"
    )
    for operation, help_text in (
        ("guided-test", "Run automatic tests plus guided LED checks"),
        ("led-check", "Compatibility alias for guided-test"),
    ):
        guided = subparsers.add_parser(operation, help=help_text)
        guided.add_argument(
            "--port", help="Serial port; auto-detected if omitted"
        )
        guided.add_argument("--unit-id", help="Barcode; defaults to base MAC")
        guided.add_argument(
            "--operator-id",
            required=True,
            help="Operator identifier using 1..20 safe characters",
        )
        guided.add_argument("--output", type=Path, help="JSON report path")
        guided.add_argument(
            "--verbose", action="store_true", help="Print raw protocol frames"
        )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        profile, profile_hash = load_profile(args.profile)
        if args.operation == "discover":
            devices = discover_devices(profile, args.port, args.verbose)
            print(json.dumps({"devices": devices}, indent=2, sort_keys=True))
            return 0 if devices else 2

        port = select_port(profile, args.port, args.verbose)
        print(f"[START] {args.operation} on {port}", flush=True)
        handle, client = open_client(port, verbose=args.verbose)
        try:
            client.observe_ready()
            report = run_partial_self_test(
                client,
                profile,
                profile_hash,
                args.unit_id,
                operator_id=(
                    args.operator_id
                    if args.operation in {"guided-test", "led-check"}
                    else None
                ),
                progress=lambda message: print(message, flush=True),
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
