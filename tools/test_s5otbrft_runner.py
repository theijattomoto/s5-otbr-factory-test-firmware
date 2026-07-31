"""Native unit tests for the partial station runner."""

from __future__ import annotations

import json
import contextlib
import io
import unittest
from pathlib import Path
from typing import Any

from tools.s5otbrft_runner import (
    DEFAULT_PROFILE,
    PROTOCOL_PREFIX,
    ProtocolError,
    S5OTBRFTClient,
    load_profile,
    run_partial_self_test,
)


class FakeSerial:
    def __init__(self) -> None:
        self.rx = bytearray()
        self.writes: list[bytes] = []
        self.closed = False
        self.recorded_tests: set[str] = set()
        self.pwm_duty = 100
        self.lamp_on = False

    @property
    def in_waiting(self) -> int:
        return len(self.rx)

    def read(self, size: int) -> bytes:
        if not self.rx:
            return b""
        chunk = bytes(self.rx[:size])
        del self.rx[:size]
        return chunk

    def write(self, data: bytes) -> int:
        self.writes.append(data)
        request = json.loads(
            data.decode("utf-8").strip()[len(PROTOCOL_PREFIX) :]
        )
        response = self._response(request)
        self.rx.extend(
            (
                PROTOCOL_PREFIX
                + json.dumps(response, separators=(",", ":"))
                + "\n"
            ).encode("utf-8")
        )
        return len(data)

    def flush(self) -> None:
        return None

    @staticmethod
    def _envelope(request: dict[str, Any], data: dict[str, Any]) -> dict[str, Any]:
        return {
            "seq": request["seq"],
            "cmd": request["cmd"],
            "status": "ok",
            "code": "ok",
            "firmware": "0.3.0",
            "protocol": "1.2",
            "product": "S5-NODE-OTBR",
            "board": "TBD",
            "data": data,
        }

    def _response(self, request: dict[str, Any]) -> dict[str, Any]:
        command = request["cmd"]
        if command == "identity":
            data = {
                "target": "esp32c6",
                "silicon_revision": 1,
                "cores": 1,
                "flash_size_bytes": 4194304,
                "base_mac": "98:88:E0:11:22:33",
                "thread_eui64": "98:88:E0:FF:FE:11:22:33",
                "reset_reason": 1,
            }
        elif command == "gps.check":
            data = {
                "nmea_received": True,
                "valid_nmea_sentences": 1,
                "valid_rmc_sentences": 1,
                "sample": "$GPRMC,000000.00,V,,,,,,,010100,,,N*7A",
            }
        elif command == "modem.check":
            data = {
                "at_ok": True,
                "identity_ok": True,
                "sim_ready": True,
                "identity": "Quectel EG912 OK",
                "sim_status": "+CPIN: READY OK",
            }
        elif command == "adc.sample":
            data = {
                "channel": request["channel"],
                "samples": request["samples"],
                "raw_average": 1000,
                "raw_min": 995,
                "raw_max": 1005,
                "raw_noise": 10,
                "adc_mv": 900,
                "estimated_input_mv": 4995,
                "engineering_units_approved": True,
            }
        elif command == "adc.waveform":
            current = (
                0.005
                if not self.lamp_on
                else 0.010 + (100 - self.pwm_duty) * 0.0004
            )
            data = {
                "channel": request["channel"],
                "samples": request["samples"],
                "raw_mean": 2000.0,
                "raw_rms": 450.0,
                "raw_min": 1000,
                "raw_max": 3000,
                "peak_to_peak": 2000,
                "clipped_samples": 0,
                "sample_rate_hz": 20000.0,
                "engineering_value": (
                    240.0 if request["channel"] == "vrms" else current
                ),
                "within_range": True,
            }
        elif command == "spi.sensor":
            data = {"who_am_i": 0x44, "x": 1, "y": 2, "z": 3}
        elif command == "zcd.capture":
            data = {
                "expected_hz": 50,
                "edges": 100,
                "frequency_hz": 50.0,
                "duration_ms": 1000.0,
                "within_tolerance": True,
            }
        elif command == "pwm.set":
            self.pwm_duty = request["duty_percent"]
            data = {
                "duty_percent": request["duty_percent"],
                "frequency_hz": 1000,
            }
        elif command == "gpio.write":
            if request["name"] == "lamp_ctrl":
                self.lamp_on = request["level"] == 0
            data = {
                "name": request["name"],
                "gpio": 2 if request["name"] == "status_led" else 8,
                "level": request["level"],
                "active_low": True,
            }
        elif command == "fixture.record":
            self.recorded_tests.add(request["test_id"])
            data = {}
        elif command == "session.list":
            passed = {
                "device_identity",
                "usb_protocol",
                "base_mac",
                "thread_eui64",
                "firmware_identity",
                "spi_wsen",
                "gpio_spi_cs1",
                "gpio_spi_sck",
                "gpio_spi_mosi",
                "gpio_spi_miso",
                "gps_uart_rx",
                "modem_uart",
                "modem_identity",
                "sim_presence",
                *self.recorded_tests,
            }
            all_tests = [
                "device_identity",
                "usb_protocol",
                "base_mac",
                "thread_eui64",
                "firmware_identity",
                "spi_wsen",
                "gpio_spi_cs1",
                "gpio_spi_sck",
                "gpio_spi_mosi",
                "gpio_spi_miso",
                "gps_uart_rx",
                "modem_uart",
                "modem_identity",
                "sim_presence",
                "rail_3v3",
                "rail_5v",
                "gpio_lamp_ctrl",
                "gpio_psw_en",
                "gpio_modem_pwrkey",
                "gpio_status_led",
                "gpio_ctrl_led",
                "adc_vrms",
                "adc_irms",
                "adc_5v",
                "pwm",
                "zcd_gpio",
                "zcd_50hz",
                "zcd_60hz",
                "gps_uart_tx",
                "modem_uart_tx_rx",
            ]
            data = {
                "tests": [
                    {
                        "id": test_id,
                        "status": "pass" if test_id in passed else "pending",
                    }
                    for test_id in all_tests
                ]
            }
        else:
            data = {}
        return self._envelope(request, data)


class RunnerTests(unittest.TestCase):
    def test_partial_success_and_cleanup(self) -> None:
        profile, profile_hash = load_profile(DEFAULT_PROFILE)
        serial = FakeSerial()
        client = S5OTBRFTClient(serial, "FAKE")

        report = run_partial_self_test(client, profile, profile_hash)

        self.assertEqual(report["result"], "PARTIAL")
        self.assertFalse(report["production_release"])
        self.assertEqual(report["unit_id"], "ENG-9888E0112233")
        self.assertTrue(report["safety_cleanup"]["safe"])
        self.assertTrue(report["safety_cleanup"]["abort"])
        self.assertEqual(
            report["adc_snapshots"]["vrms"]["engineering_value"], 240.0
        )
        commands = [
            json.loads(
                raw.decode("utf-8").strip()[len(PROTOCOL_PREFIX) :]
            )["cmd"]
            for raw in serial.writes
        ]
        self.assertEqual(
            commands,
            [
                "identity",
                "session.start",
                "spi.sensor",
                "gps.check",
                "modem.check",
                "adc.waveform",
                "fixture.record",
                "zcd.capture",
                "fixture.record",
                "fixture.record",
                "adc.sample",
                "fixture.record",
                "fixture.record",
                "session.list",
                "pwm.set",
                "gpio.write",
                "safe",
                "session.abort",
            ],
        )

    def test_live_output_has_operator_test_measure_and_summary_lines(self) -> None:
        profile, profile_hash = load_profile(DEFAULT_PROFILE)
        messages: list[str] = []

        report = run_partial_self_test(
            S5OTBRFTClient(FakeSerial(), "FAKE"),
            profile,
            profile_hash,
            requested_unit_id="BENCH-001",
            progress=messages.append,
        )

        output = "\n".join(messages)
        self.assertEqual(report["result"], "PARTIAL")
        self.assertIn("[TEST] Device identity and USB protocol", output)
        self.assertIn(
            "[PASS] Identity: 98:88:E0:11:22:33 / "
            "98:88:E0:FF:FE:11:22:33",
            output,
        )
        self.assertIn("[TEST] VRMS waveform", output)
        self.assertIn("[PASS] VRMS: 240.00 VAC", output)
        self.assertIn("[PASS] 5 V rail: 4.995 V", output)
        self.assertIn("[TEST] Review factory-test manifest", output)
        self.assertIn(
            "[SAFE] Restoring all outputs and aborting session", output
        )
        self.assertNotIn("[SUMMARY]", output)
        self.assertEqual(
            report["summary"],
            {
                "executed": 19,
                "passed": 19,
                "failed": 0,
                "pending": 11,
                "manifest_total": 30,
            },
        )

    def test_sequence_mismatch_is_rejected(self) -> None:
        class BadSequenceSerial(FakeSerial):
            def _response(
                self, request: dict[str, Any]
            ) -> dict[str, Any]:
                response = super()._response(request)
                response["seq"] += 1
                return response

        client = S5OTBRFTClient(BadSequenceSerial(), "FAKE")
        with self.assertRaises(ProtocolError):
            client.command("identity", timeout_s=0.1)

    def test_verbose_client_prints_frames(self) -> None:
        output = io.StringIO()
        client = S5OTBRFTClient(FakeSerial(), "FAKE", verbose=True)
        with contextlib.redirect_stdout(output):
            client.command("identity")
        text = output.getvalue()
        self.assertIn("[TX] @S5OTBRFT", text)
        self.assertIn("[RX] @S5OTBRFT", text)

    def test_peripheral_failure_does_not_stop_remaining_tests(self) -> None:
        class GpsFailureSerial(FakeSerial):
            def _response(
                self, request: dict[str, Any]
            ) -> dict[str, Any]:
                response = super()._response(request)
                if request["cmd"] == "gps.check":
                    response["status"] = "error"
                    response["code"] = "session_timeout"
                return response

        profile, profile_hash = load_profile(DEFAULT_PROFILE)
        serial = GpsFailureSerial()
        report = run_partial_self_test(
            S5OTBRFTClient(serial, "FAKE"),
            profile,
            profile_hash,
        )
        commands = [
            json.loads(
                raw.decode("utf-8").strip()[len(PROTOCOL_PREFIX) :]
            )["cmd"]
            for raw in serial.writes
        ]
        self.assertEqual(report["result"], "FAIL")
        self.assertEqual(report["test_results"]["gps"], "FAIL")
        self.assertIn("modem.check", commands)
        self.assertEqual(commands.count("adc.sample"), 1)
        self.assertIn("session.list", commands)
        self.assertEqual(commands[-2:], ["safe", "session.abort"])

    def test_profile_is_nonproduction(self) -> None:
        profile, digest = load_profile(Path(DEFAULT_PROFILE))
        self.assertFalse(profile["production_release"])
        self.assertRegex(digest, r"^[0-9A-F]{64}$")

    def test_guided_led_sequence(self) -> None:
        profile, profile_hash = load_profile(DEFAULT_PROFILE)
        serial = FakeSerial()
        answers = iter(["3.3", "Y", "Y", "Y", "Y", "Y", "Y", "Y"])
        report = run_partial_self_test(
            S5OTBRFTClient(serial, "FAKE"),
            profile,
            profile_hash,
            operator_id="OP-01",
            input_func=lambda _: next(answers),
            sleep_func=lambda _: None,
        )
        self.assertEqual(report["result"], "PARTIAL")
        self.assertTrue(
            {
                "rail_3v3",
                "rail_5v",
                "gpio_lamp_ctrl",
                "gpio_status_led",
                "gpio_ctrl_led",
                "adc_vrms",
                "adc_irms",
                "adc_5v",
                "pwm",
                "zcd_gpio",
                "zcd_50hz",
            }.issubset(serial.recorded_tests)
        )

    def test_failure_errors_are_available_to_cli(self) -> None:
        profile, profile_hash = load_profile(DEFAULT_PROFILE)

        class MissingManifestSerial(FakeSerial):
            def _response(
                self, request: dict[str, Any]
            ) -> dict[str, Any]:
                response = super()._response(request)
                if request["cmd"] == "session.list":
                    response["data"] = {"tests": []}
                return response

        report = run_partial_self_test(
            S5OTBRFTClient(MissingManifestSerial(), "FAKE"),
            profile,
            profile_hash,
        )
        self.assertEqual(report["result"], "FAIL")
        self.assertTrue(report["errors"])


if __name__ == "__main__":
    unittest.main()
