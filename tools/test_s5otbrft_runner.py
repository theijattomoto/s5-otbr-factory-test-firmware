"""Native unit tests for the partial station runner."""

from __future__ import annotations

import json
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
            "firmware": "0.1.0",
            "protocol": "1.0",
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
        elif command == "session.list":
            passed = {
                "device_identity",
                "usb_protocol",
                "base_mac",
                "thread_eui64",
                "firmware_identity",
            }
            all_tests = [
                "device_identity",
                "usb_protocol",
                "base_mac",
                "thread_eui64",
                "firmware_identity",
                "gps_uart_rx",
                "modem_uart",
                "modem_identity",
                "sim_presence",
                "rail_3v3",
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
                "session.list",
                "safe",
                "session.abort",
            ],
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

    def test_profile_is_nonproduction(self) -> None:
        profile, digest = load_profile(Path(DEFAULT_PROFILE))
        self.assertFalse(profile["production_release"])
        self.assertRegex(digest, r"^[0-9A-F]{64}$")

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
