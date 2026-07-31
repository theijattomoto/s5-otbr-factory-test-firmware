# S5 Node-OTBR Factory Protocol 1.0

## Purpose

This protocol binds low-voltage PCB observations to a unit session. It does
not authorize mains, lamp-load, RF-performance, calibration, or production
release testing. Fixture-observed results must come from approved external
measurement; firmware GPIO readback alone is not assembly evidence.

## Transport and framing

- ESP32-C6 native USB Serial/JTAG on GPIO12/GPIO13.
- UTF-8, one JSON object per line.
- Exact prefix: `@S5OTBRFT `.
- Maximum request frame: 767 bytes from the `@` through the final JSON byte,
  excluding CR/LF.
- Request `seq` is an integer from 1 through 2147483647.
- Sequence 0 is reserved for unsolicited `ready` and `session.timeout` events.
- The station ignores non-prefixed ROM/log text and matches both `seq` and
  `cmd`.

An oversized frame is discarded through its line ending. Malformed, invalid,
oversized, or unknown input triggers `safe` before the error response.

## Envelope

Every response includes:

```json
{
  "seq": 1,
  "cmd": "identity",
  "status": "ok",
  "code": "ok",
  "firmware": "0.1.0",
  "protocol": "1.0",
  "product": "S5-NODE-OTBR",
  "board": "TBD",
  "data": {}
}
```

Frames reject duplicate keys, unknown fields, wrong types, oversized strings,
trailing JSON, malformed UTF-8, non-integral sequence values, NaN, infinity,
and unsupported commands.

## Commands

### `identity`

Request fields: `seq`, `cmd`.

Returns `target`, `silicon_revision`, `cores`, `flash_size_bytes`, `base_mac`,
`thread_eui64`, and numeric `reset_reason`. Identity failure returns an error;
zero or fabricated identifiers are never substituted.

### `session.start`

Additional field: `unit_id`, containing 1–47 ASCII letters, digits, `.`, `_`,
or `-`.

The command first verifies identity and reapplies safe state. It rejects an
already-active session, clears prior results only after cleanup succeeds, and
marks `device_identity`, `usb_protocol`, `base_mac`, `thread_eui64`, and
`firmware_identity` PASS.

### `session.list`

Requires an active session. Returns every manifest item with its `id`,
`description`, `owner`, `status`, and optional recorded value/detail.

### `fixture.record`

Required fields: `test_id`, Boolean `pass`. Optional fields are finite numeric
`value`, `unit` up to 15 bytes, and `detail` up to 95 bytes.

Only entries with owner `station` may be written. A pending entry can be
written exactly once; PASS and FAIL results are immutable. Recording FAIL
immediately reapplies safe state.

### `session.finish`

Reapplies safe state and returns `code:"pass"` only if every manifest entry is
PASS and cleanup succeeds. Pending items, failed items, or cleanup failure
prevent PASS. Phase 1 necessarily remains incomplete because GPS and modem
automatic tests are not implemented.

### `session.abort`

Idempotently marks the session aborted and reapplies safe state, including
when no session is active.

### `safe`

Idempotently restores all known software-safe outputs without changing the
session. GPIO3 remains high impedance because the `PSW_EN` policy is
unresolved.

## Session lifecycle

- Timeout: 300 seconds since the last valid active-session command.
- Duplicate start: rejected without resetting the active manifest.
- Incomplete finish: rejected; session remains active for listing and pending
  fixture work.
- Successful finish: state becomes `completed`.
- Timeout: state becomes `expired` and a sequence-0 event is emitted.
- Abort or cleanup failure: state becomes `aborted`.

## Stable error codes

`invalid_parameter`, `invalid_frame`, `invalid_json`, `invalid_request`,
`frame_too_long`, `unknown_command`, `unknown_test`, `invalid_state`,
`result_immutable`, `test_owner_mismatch`, `incomplete_or_failed`,
`session_timeout`, `cleanup_failed`, `hardware_error`, and `no_memory`.

## Provisional manifest

Automatic:

```text
device_identity, usb_protocol, base_mac, thread_eui64, firmware_identity,
gps_uart_rx, modem_uart, modem_identity, sim_presence
```

Station-authoritative:

```text
rail_3v3, rail_5v,
gpio_lamp_ctrl, gpio_psw_en, gpio_modem_pwrkey, gpio_modem_reset,
gpio_status_led, gpio_ctrl_led,
adc_vrms, adc_irms, adc_5v, pwm,
zcd_gpio, zcd_50hz, zcd_60hz,
gps_uart_tx, modem_uart_tx_rx
```

The manifest remains provisional until the controlled schematic, PCB
revision, fixture design, test-point map, and approved limits are available.

## Fixture contract

- Use common ground, current limiting, and high-impedance observations.
- Default stimulus outputs to high impedance.
- Never drive an unpowered DUT or exceed its measured I/O rails.
- Store unit, station, operator, fixture, firmware, protocol, board, profile,
  limits, measurements, manifest, cleanup result, and raw frames together.
- Never turn a partial or incomplete session into production PASS.
