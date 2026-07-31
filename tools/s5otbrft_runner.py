#!/usr/bin/env python3
"""Partial automation for S5 Node-OTBR factory-test firmware."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

PROTOCOL_PREFIX = "@S5OTBRFT "
TOOL_VERSION = "0.3.0"
PWM_STEP_NOISE_TOLERANCE_A = 0.010
PWM_MIN_TOTAL_RISE_A = 0.020
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
        "criteria",
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
        raise ValidationError("GPS did not receive a checksum-valid NMEA sentence")
    count = data.get("valid_nmea_sentences")
    if isinstance(count, bool) or not isinstance(count, int) or count < 1:
        raise ValidationError("GPS response has no valid NMEA sentence count")
    if not isinstance(data.get("sample"), str) or not data["sample"].startswith(
        "$"
    ):
        raise ValidationError("GPS response has no valid NMEA sample")
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
    if data.get("engineering_units_approved") is not True:
        raise ValidationError("ADC response has no approved engineering units")
    return data


def validate_wsen(
    response: dict[str, Any], profile: dict[str, Any]
) -> dict[str, Any]:
    data = require_ok(response, "spi.sensor")
    expected = profile["criteria"]["wsen_who_am_i"]
    if data.get("who_am_i") != expected:
        raise ValidationError(
            f"WSEN identity expected 0x{expected:02X}, "
            f"received {data.get('who_am_i')!r}"
        )
    for axis in ("x", "y", "z"):
        value = data.get(axis)
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise ValidationError(f"WSEN response has no numeric {axis}")
    return data


def prompt_float(
    prompt: str,
    input_func: Callable[[str], str] = input,
    output_func: Callable[[str], None] = print,
) -> float:
    while True:
        try:
            value = float(input_func(prompt).strip())
        except (EOFError, ValueError):
            output_func("Please enter a numeric value.")
            continue
        if math.isfinite(value):
            return value
        output_func("Please enter a finite numeric value.")


def record_fixture(
    client: S5OTBRFTClient,
    test_id: str,
    passed: bool,
    detail: str,
    value: float | None = None,
    unit: str | None = None,
) -> None:
    fields: dict[str, Any] = {
        "test_id": test_id,
        "pass": passed,
        "detail": detail,
    }
    if value is not None:
        fields["value"] = value
    if unit is not None:
        fields["unit"] = unit
    require_ok(client.command("fixture.record", **fields),
               f"fixture.record {test_id}")


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
    sleep_func: Callable[[float], None] = time.sleep,
) -> dict[str, Any]:
    guided = operator_id is not None
    if guided and re.fullmatch(r"[A-Za-z0-9_.-]{1,20}", operator_id) is None:
        raise ValidationError(
            "operator-id must use 1..20 letters, digits, period, underscore "
            "or hyphen"
        )
    errors: list[str] = []
    emit = progress if progress is not None else (lambda _: None)

    def begin_test(name: str) -> None:
        emit(f"[TEST] {name}")

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
        "test_results": {},
        "manifest": {"passed": [], "pending": [], "failed": []},
        "errors": errors,
        "safety_cleanup": {
            "pwm_zero": False,
            "lamp_ctrl_off": False,
            "safe": False,
            "abort": False,
        },
        "raw_exchange": client.transcript,
    }

    try:
        begin_test("Device identity and USB protocol")
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
        report["test_results"]["device_identity"] = "PASS"
        emit(
            f"[PASS] Identity: {identity['base_mac']} / "
            f"{identity['thread_eui64']}"
        )

        begin_test(f"Start session for {unit_id}")
        require_ok(
            client.command("session.start", unit_id=unit_id),
            "session.start",
        )
        emit("[PASS] Factory session started")

        peripheral_failures = 0

        if guided:
            criteria = profile["criteria"]
            begin_test(
                "3.3 V rail with multimeter "
                f"(allowed {criteria['rail_3v3_min_v']:.1f}-"
                f"{criteria['rail_3v3_max_v']:.1f} V)"
            )
            measured_3v3 = prompt_float(
                "Enter measured 3.3 V rail voltage: ",
                input_func,
                output_func,
            )
            rail_3v3_passed = (
                criteria["rail_3v3_min_v"] <= measured_3v3 <=
                criteria["rail_3v3_max_v"]
            )
            emit(
                f"[{'PASS' if rail_3v3_passed else 'FAIL'}] "
                f"3.3 V rail: {measured_3v3:.3f} V"
            )
            record_fixture(
                client, "rail_3v3", rail_3v3_passed,
                f"operator={operator_id};DMM", measured_3v3, "V"
            )
            if not rail_3v3_passed:
                peripheral_failures += 1
                errors.append("rail_3v3: measurement outside 3.0-3.7 V")

        begin_test("WSEN SPI sensor")
        try:
            report["wsen"] = validate_wsen(
                client.command("spi.sensor", timeout_s=5.0), profile
            )
            emit(
                f"[PASS] WSEN SPI: "
                f"WHO_AM_I=0x{report['wsen']['who_am_i']:02X}"
            )
        except ProtocolError:
            raise
        except Exception as exc:
            peripheral_failures += 1
            errors.append(f"spi.sensor: {exc}")
            emit(f"[FAIL] WSEN SPI: {exc}")

        begin_test("GPS UART checksum-valid NMEA")
        gps_timeout_ms = int(profile["gps_timeout_ms"])
        try:
            report["gps"] = validate_gps(
                client.command(
                    "gps.check",
                    timeout_s=(gps_timeout_ms / 1000.0) + 3.0,
                    timeout_ms=gps_timeout_ms,
                )
            )
            report["test_results"]["gps"] = "PASS"
            emit(
                f"[PASS] GPS UART: "
                f"{report['gps'].get('valid_nmea_sentences', 0)} valid NMEA"
            )
        except ProtocolError:
            raise
        except Exception as exc:
            peripheral_failures += 1
            report["test_results"]["gps"] = "FAIL"
            errors.append(f"gps.check: {exc}")
            emit(f"[FAIL] GPS UART: {exc}")

        begin_test("EG912 modem UART")
        modem_timeout_ms = int(profile["modem_timeout_ms"])
        try:
            report["modem"] = validate_modem(
                client.command(
                    "modem.check",
                    timeout_s=(modem_timeout_ms * 3 / 1000.0) + 3.0,
                    timeout_ms=modem_timeout_ms,
                )
            )
            report["test_results"]["modem"] = "PASS"
            emit(
                f"[PASS] EG912 modem UART and SIM: "
                f"{report['modem']['identity']}"
            )
        except ProtocolError:
            raise
        except Exception as exc:
            peripheral_failures += 1
            report["test_results"]["modem"] = "FAIL"
            errors.append(f"modem.check: {exc}")
            emit(f"[FAIL] EG912: {exc}")

        begin_test("VRMS waveform (allowed 220-260 VAC)")
        try:
            response = client.command(
                "adc.waveform", channel="vrms", samples=1000,
                sample_interval_us=50,
            )
            vrms = response.get("data", {})
            report["adc_snapshots"]["vrms"] = vrms
            measured_vrms = float(vrms.get("engineering_value"))
            vrms_passed = vrms.get("within_range") is True
            emit(
                f"[{'PASS' if vrms_passed else 'FAIL'}] "
                f"VRMS: {measured_vrms:.2f} VAC"
            )
            record_fixture(
                client, "adc_vrms", vrms_passed,
                f"waveform;samples=1000;operator={operator_id or 'AUTO'}",
                measured_vrms, "VAC",
            )
            require_ok(response, "adc.waveform vrms")
        except ProtocolError:
            raise
        except Exception as exc:
            peripheral_failures += 1
            errors.append(f"adc.waveform vrms: {exc}")
            emit(f"[FAIL] VRMS waveform: {exc}")

        begin_test("AC ZCD frequency (allowed 45-55 Hz)")
        try:
            response = client.command(
                "zcd.capture", expected_hz=50, duration_ms=1000,
                timeout_s=4.0,
            )
            zcd = response.get("data", {})
            measured_hz = float(zcd.get("frequency_hz", 0.0))
            zcd_passed = zcd.get("within_tolerance") is True
            emit(
                f"[{'PASS' if zcd_passed else 'FAIL'}] AC ZCD captured: "
                f"{measured_hz:.2f} Hz, edges={zcd.get('edges', 0)}"
            )
            record_fixture(
                client, "zcd_50hz", zcd_passed,
                f"capture;operator={operator_id or 'AUTO'}",
                measured_hz, "Hz",
            )
            record_fixture(
                client, "zcd_gpio", zcd.get("edges", 0) > 0,
                "transitions observed",
            )
            require_ok(response, "zcd.capture 50 Hz")
        except ProtocolError:
            raise
        except Exception as exc:
            peripheral_failures += 1
            errors.append(f"zcd.capture 50 Hz: {exc}")
            emit(f"[FAIL] AC ZCD: {exc}")

        if guided:
            begin_test("LAMP_CTRL + IRMS functional test")
            authorized = prompt_yes_no(
                "Approved isolated load is connected and safe to energize? "
                "[Y/N] ", input_func, output_func
            )
            if not authorized:
                peripheral_failures += 1
                errors.append("lamp current test: not authorized by operator")
                emit("[FAIL] Lamp current test not authorized")
            else:
                observations: dict[str, float] = {}

                def capture_irms(label: str) -> float:
                    begin_test(f"IRMS {label}")
                    irms_response = client.command(
                        "adc.waveform", channel="irms", mode="observe",
                        samples=1000, sample_interval_us=50,
                    )
                    irms_data = require_ok(
                        irms_response, f"adc.waveform irms {label}"
                    )
                    current = float(irms_data["engineering_value"])
                    emit(f"[MEASURE] IRMS {label}: {current:.4f} A")
                    return current

                try:
                    begin_test("Set LAMP_CTRL OFF")
                    require_ok(
                        client.command(
                            "gpio.write", name="lamp_ctrl", level=1
                        ), "LAMP_CTRL OFF"
                    )
                    emit("[PASS] LAMP_CTRL commanded OFF")
                    sleep_func(0.5)
                    observations["off_before"] = capture_irms(
                        "OFF before load"
                    )
                    begin_test(
                        "Keep LAMP_CTRL OFF for 5.0 seconds before ON"
                    )
                    sleep_func(5.0)
                    begin_test("Set LAMP_CTRL ON")
                    require_ok(
                        client.command(
                            "gpio.write", name="lamp_ctrl", level=0
                        ), "LAMP_CTRL ON"
                    )
                    begin_test("Load ON; settling for 10.0 seconds")
                    sleep_func(10.0)
                    observations["on"] = capture_irms("ON with load")
                finally:
                    emit("[SAFE] Set LAMP_CTRL OFF")
                    best_effort_command(
                        client, "gpio.write", errors,
                        name="lamp_ctrl", level=1
                    )
                sleep_func(0.5)
                observations["off_after"] = capture_irms("OFF after load")
                off1 = observations["off_before"]
                on = observations["on"]
                off2 = observations["off_after"]
                baseline = (off1 + off2) / 2.0
                lamp_passed = (
                    abs(off1 - off2) <= 0.002 and
                    on - baseline >= 0.002 and on <= 1.000
                )
                begin_test(
                    f"Current transition: OFF {off1:.4f} A -> "
                    f"ON {on:.4f} A -> OFF {off2:.4f} A"
                )
                emit(
                    f"[{'PASS' if lamp_passed else 'FAIL'}] "
                    "LAMP_CTRL baseline/current-response verification"
                )
                detail = (
                    f"op={operator_id};off1={off1:.4f};"
                    f"on={on:.4f};off2={off2:.4f}"
                )
                record_fixture(
                    client, "adc_irms", lamp_passed, detail, on, "A"
                )
                record_fixture(
                    client, "gpio_lamp_ctrl", lamp_passed, detail
                )
                if not lamp_passed:
                    peripheral_failures += 1
                    errors.append(
                        "lamp current test: OFF-ON-OFF criteria failed"
                    )

                begin_test("PWM current sweep: 0% to 100% in 10% steps")
                points: list[float] = []
                try:
                    require_ok(
                        client.command("pwm.set", duty_percent=100),
                        "PWM zero brightness",
                    )
                    require_ok(
                        client.command(
                            "gpio.write", name="lamp_ctrl", level=0
                        ), "LAMP_CTRL ON for PWM"
                    )
                    for brightness in range(0, 101, 10):
                        duty = 100 - brightness
                        begin_test(
                            f"Brightness {brightness}% "
                            f"(hardware PWM duty {duty}%)"
                        )
                        require_ok(
                            client.command(
                                "pwm.set", duty_percent=duty
                            ), f"PWM {duty}%"
                        )
                        sleep_func(10.0)
                        value = capture_irms(
                            f"at brightness {brightness}%"
                        )
                        points.append(value)
                        emit(
                            f"[MEASURE] Brightness {brightness:3d}% / "
                            f"duty {duty:3d}%: {value:.4f} A"
                        )
                finally:
                    emit(
                        "[SAFE] Brightness 0% (PWM duty 100%) "
                        "and LAMP_CTRL OFF"
                    )
                    best_effort_command(
                        client, "pwm.set", errors, duty_percent=100
                    )
                    best_effort_command(
                        client, "gpio.write", errors,
                        name="lamp_ctrl", level=1
                    )
                pwm_passed = (
                    len(points) == 11 and
                    all(
                        points[i] + PWM_STEP_NOISE_TOLERANCE_A >=
                        points[i - 1]
                        for i in range(1, len(points))
                    ) and points[-1] - points[0] >= PWM_MIN_TOTAL_RISE_A
                )
                emit(
                    "[TEST] PWM IRMS sequence: "
                    + " -> ".join(f"{value:.4f}" for value in points)
                    + " A"
                )
                emit(
                    f"[{'PASS' if pwm_passed else 'FAIL'}] "
                    "Current rises with brightness "
                    f"(step noise allowance "
                    f"{PWM_STEP_NOISE_TOLERANCE_A:.3f} A)"
                )
                record_fixture(
                    client, "pwm", pwm_passed,
                    f"op={operator_id};11-point IRMS sweep",
                    points[-1] if points else 0.0, "A"
                )
                if not pwm_passed:
                    peripheral_failures += 1
                    errors.append("pwm current sweep: trend criteria failed")

        try:
            sample = validate_adc(
                client.command(
                    "adc.sample", channel="dc5v",
                    samples=int(profile["adc_samples"]),
                ), "dc5v"
            )
            report["adc_snapshots"]["dc5v"] = sample
            measured_5v = float(sample["estimated_input_mv"]) / 1000.0
            minimum = profile["criteria"]["rail_5v_min_mv"] / 1000.0
            maximum = profile["criteria"]["rail_5v_max_mv"] / 1000.0
            rail_5v_passed = minimum <= measured_5v <= maximum
            emit(
                f"[{'PASS' if rail_5v_passed else 'FAIL'}] "
                f"5 V rail: {measured_5v:.3f} V"
            )
            record_fixture(
                client, "rail_5v", rail_5v_passed,
                "onboard calibrated ADC", measured_5v, "V"
            )
            record_fixture(
                client, "adc_5v", rail_5v_passed,
                "same calibrated ADC reading", measured_5v, "V"
            )
            if not rail_5v_passed:
                peripheral_failures += 1
                errors.append("rail_5v: measurement outside 4.75-5.25 V")
        except ProtocolError:
            raise
        except Exception as exc:
            peripheral_failures += 1
            errors.append(f"adc.sample dc5v: {exc}")
            emit(f"[FAIL] 5 V rail: {exc}")

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
                try:
                    passed = True
                    begin_test(f"{display_name} OFF-ON-OFF visual check")
                    for level, expected in led_states:
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
                    report["test_results"][test_id] = (
                        "PASS" if passed else "FAIL"
                    )
                    if not passed:
                        peripheral_failures += 1
                        message = (
                            f"{display_name} did not match the expected state"
                        )
                        errors.append(f"{test_id}: {message}")
                        emit(f"[FAIL] {message}")
                    else:
                        emit(f"[PASS] {display_name} visual check")
                except ProtocolError:
                    raise
                except Exception as exc:
                    peripheral_failures += 1
                    report["test_results"][test_id] = "FAIL"
                    errors.append(f"{test_id}: {exc}")
                    emit(f"[FAIL] {display_name}: {exc}")

        begin_test("Review factory-test manifest")
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
            required_guided = required_leds | {
                "rail_3v3", "rail_5v", "gpio_lamp_ctrl",
                "adc_vrms", "adc_irms", "adc_5v", "pwm",
                "zcd_gpio", "zcd_50hz",
            }
            missing_passed.extend(sorted(required_guided - passed))
        manifest_problems: list[str] = []
        if missing_passed:
            manifest_problems.append(
                "required tests not passed: " + ", ".join(missing_passed)
            )
        if report["manifest"]["failed"]:
            manifest_problems.append(
                "manifest failures: "
                + ", ".join(report["manifest"]["failed"])
            )
        if not report["manifest"]["pending"]:
            manifest_problems.append("partial run has no pending tests")
        if manifest_problems:
            errors.extend(f"manifest: {problem}" for problem in manifest_problems)

        report["result"] = (
            "FAIL"
            if peripheral_failures or manifest_problems
            else "PARTIAL"
        )
    except Exception as exc:
        errors.append(str(exc))
        report["result"] = "FAIL"
        emit(f"[FAIL] {exc}")
    finally:
        emit("[SAFE] Restoring all outputs and aborting session")
        report["safety_cleanup"]["pwm_zero"] = best_effort_command(
            client, "pwm.set", errors, duty_percent=100
        )
        report["safety_cleanup"]["lamp_ctrl_off"] = best_effort_command(
            client, "gpio.write", errors, name="lamp_ctrl", level=1
        )
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
        passed_count = len(report["manifest"]["passed"])
        failed_count = len(report["manifest"]["failed"])
        executed_count = passed_count + failed_count
        pending_count = len(report["manifest"]["pending"])
        manifest_total = executed_count + pending_count
        report["summary"] = {
            "executed": executed_count,
            "passed": passed_count,
            "failed": failed_count,
            "pending": pending_count,
            "manifest_total": manifest_total,
        }
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
        operation_name = (
            "Guided factory test"
            if args.operation in {"guided-test", "led-check"}
            else "Factory self-test"
        )
        print(f"[START] {operation_name} on {port}", flush=True)
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
        print(f"{report['result']}: {output}", flush=True)
        if report["result"] == "FAIL":
            print("Failure reasons:", flush=True)
            for error in report["errors"]:
                print(f"  - {error}", flush=True)
        if report["manifest"]["failed"]:
            print(
                "Failed tests: "
                + ", ".join(report["manifest"]["failed"]),
                flush=True,
            )
        if report["manifest"]["pending"]:
            print(
                "Pending tests: "
                + ", ".join(report["manifest"]["pending"]),
                flush=True,
            )
        summary = report["summary"]
        print(
            f"[SUMMARY] Passed {summary['passed']}/"
            f"{summary['executed']} executed tests"
            f" | Failed {summary['failed']}"
            f" | Pending {summary['pending']}"
            f" | Manifest total {summary['manifest_total']}",
            flush=True,
        )
        return 0 if report["result"] == "PARTIAL" else 1
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
