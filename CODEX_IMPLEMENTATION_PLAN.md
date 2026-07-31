# Codex Implementation Plan

## S5 Node-OTBR Factory-Test Firmware

### Target repository

Create and implement a dedicated repository named:

```text
theijattomoto/s5-node-otbr-factory-test-firmware
```

Reference repositories:

- Production hardware and firmware behavior:
  `theijattomoto/NODE-OTBR`
- Existing factory-test architecture and host workflow:
  `theijattomoto/s5-node-factory-test`

The new repository is a standalone ESP-IDF factory-test application. It must
not be implemented as a factory mode inside the production `NODE-OTBR`
firmware.

---

## 1. Mission

Build deterministic post-assembly factory-test firmware for the ESP32-C6-based
S5 Node-OTBR board.

The solution must:

1. Start all controllable outputs in verified safe states.
2. Expose a versioned, line-oriented JSON protocol over native USB
   Serial/JTAG.
3. Test the ESP32-C6 identity, board interfaces, ADC channels, PWM, ZCD, GPS,
   and EG912 modem.
4. Separate self-verifiable tests from station-authoritative measurements.
5. Maintain a fail-closed test manifest.
6. Generate traceable JSON reports through Raspberry Pi station software.
7. Never classify an incomplete or no-fixture test as production PASS.
8. Restore the board to safe state after success, failure, timeout,
   communication loss, malformed input, abort, or reset.
9. Support factory deployment using binaries without requiring operators to
   receive this source repository or an ESP-IDF development environment.

---

## 2. Mandatory engineering constraints

### 2.1 Safety

- Do not energize mains or connect a lamp load during the low-voltage PCB test.
- Do not assume that a GPIO register readback proves an assembled PCB net.
- Output results must be observed by approved fixture hardware or, for
  engineering-only LED checks, confirmed by the operator.
- Fixture drivers must default to high impedance.
- Inputs must never be driven above the measured 3.3 V I/O rail, below ground,
  or while the DUT is unpowered.
- Analog stimulus must remain inside the approved front-end and ESP32-C6 ADC
  limits.
- `safe` must be idempotent and callable at any time.
- Safety cleanup must not depend on an active factory-test session.
- Session timeout and serial failure must independently trigger safe cleanup.
- Do not finalize the `PSW_EN` test sequence or safe level until it is verified
  against the controlled schematic and board power design.

### 2.2 Security and privacy

- Do not copy production MQTT credentials, TLS material, Thread datasets,
  network keys, APNs, or deployment secrets from `NODE-OTBR`.
- Factory LTE/MQTT credentials belong to the Raspberry Pi station
  configuration, not the firmware source.
- Redact IMSI, ICCID, IMEI, SIM identifiers, credentials, and sensitive modem
  responses from normal logs and operator screens.
- Store only the identifiers explicitly approved for manufacturing
  traceability.
- Do not enable production OTA, MQTT, CoAP bridge, LittleFS configuration,
  scheduler, heartbeat, energy accumulation, or FTD registry services.

### 2.3 Determinism

- Use native USB Serial/JTAG as the fixture transport.
- Disable ANSI log colors.
- Keep normal ESP-IDF logs at warning level or lower.
- Prefix every fixture frame with exactly:

  ```text
  @S5OTBRFT 
  ```

- Encode one UTF-8 JSON object per line.
- Every request must contain numeric `seq` and string `cmd`.
- Every response must echo `seq` and `cmd`.
- Match replies by sequence number; ignore unrelated boot-ROM text.
- Enforce a documented maximum input frame size.
- Reject duplicate JSON keys, invalid types, unsupported fields where strict
  validation is required, oversized strings, NaN, and infinite numeric values.
- Never allow malformed input to leave an output energized.

---

## 3. Verified initial hardware profile

Use `theijattomoto/NODE-OTBR` as the software-visible baseline, but verify every
item against the controlled schematic before approving a production fixture.

| Function | ESP32-C6 resource | Factory-test concern |
| --- | ---: | --- |
| Lamp control | GPIO0, active-low | Safe state is logic high |
| EG912 power key | GPIO1 | Timed pulse only |
| Status LED | GPIO2, active-low | Optical or current observation |
| `PSW_EN` | GPIO3 | Safe behavior requires hardware approval |
| VRMS | ADC channel 4 | Two-point fixture stimulus |
| IRMS | ADC channel 5 | Two-point fixture stimulus |
| 5 V monitor | ADC channel 6 | Rail check plus separate ADC validation |
| Lamp PWM | GPIO7 | 1 kHz and allowed duty points |
| Control LED | GPIO8, active-low | Optical or current observation |
| EG912 reset | GPIO9, active-low | Controlled pulse |
| Native USB | GPIO12/GPIO13 | Framed protocol |
| AC ZCD | GPIO14 | Static and frequency stimulus |
| EG912 UART | GPIO16/GPIO17, UART1 | Bidirectional AT communication |
| GPS UART | GPIO18/GPIO19, UART0 | Bidirectional UART test |
| IEEE 802.15.4 identity | Native radio EUI-64 | Identity readout only; no Thread network or RF test |

Do not copy the S5 Node V5 WSEN SPI or fixture I2C tests unless those circuits
are confirmed on the Node-OTBR schematic.

---

## 4. Required repository structure

Create this structure incrementally. Do not generate empty placeholder modules
that have no defined interface or test.

```text
s5-node-otbr-factory-test-firmware/
├── CMakeLists.txt
├── sdkconfig.defaults
├── README.md
├── LICENSE
├── main/
│   ├── CMakeLists.txt
│   ├── app_main.c
│   ├── factory_protocol.c
│   ├── factory_session.c
│   ├── factory_manifest.c
│   ├── factory_identity.c
│   ├── factory_safety.c
│   ├── test_gpio.c
│   ├── test_adc.c
│   ├── test_pwm.c
│   ├── test_zcd.c
│   ├── test_gps.c
│   ├── test_modem.c
│   └── include/
│       ├── factory_protocol.h
│       ├── factory_session.h
│       ├── factory_manifest.h
│       ├── factory_identity.h
│       ├── factory_safety.h
│       ├── factory_board.h
│       ├── test_gpio.h
│       ├── test_adc.h
│       ├── test_pwm.h
│       ├── test_zcd.h
│       ├── test_gps.h
│       └── test_modem.h
├── test/
│   ├── test_protocol.c
│   ├── test_session.c
│   ├── test_manifest.c
│   ├── test_validation.c
│   └── test_safety.c
├── tools/
│   ├── requirements.txt
│   ├── pyproject.toml
│   ├── s5otbrft_runner/
│   │   ├── __init__.py
│   │   ├── __main__.py
│   │   ├── cli.py
│   │   ├── protocol.py
│   │   ├── discovery.py
│   │   ├── session.py
│   │   ├── profiles.py
│   │   ├── report.py
│   │   └── validation.py
│   └── profiles/
│       ├── engineering-partial.json
│       └── schema.json
├── docs/
│   ├── factory_test_plan.md
│   ├── factory_protocol.md
│   ├── fixture_contract.md
│   ├── raspberry_pi_gui_specification.md
│   ├── operator_work_instruction.md
│   ├── engineering_maintenance_guide.md
│   └── release_and_traceability.md
└── .github/
    └── workflows/
        ├── firmware-build.yml
        └── host-tests.yml
```

If the repository will remain private and no license has been chosen, omit
`LICENSE` rather than inventing one.

---

## 5. Architecture requirements

### 5.1 Layering

Keep these responsibilities separate:

1. `factory_board.h`
   - Pin names
   - Active levels
   - Safe levels
   - UART assignments
   - ADC channels
   - Board and product identifiers

2. `factory_safety`
   - Initialize outputs safely before starting the parser
   - Stop PWM
   - Restore all outputs
   - Abort active peripheral operations
   - Handle timeout and cleanup

3. `factory_protocol`
   - Input framing
   - JSON parsing
   - Request validation
   - Dispatch
   - Response envelope

4. `factory_session`
   - Start, timeout, abort, finish
   - Unit ID binding
   - Session timestamps and state

5. `factory_manifest`
   - Required test definitions
   - `pending`, `pass`, and `fail` state
   - Result details
   - Fail-closed completion decision

6. `test_*`
   - Own only the relevant peripheral
   - Validate every request parameter
   - Return measurements and diagnostic details
   - Restore temporary configuration after the command

7. Host runner
   - Own test sequencing and station policy
   - Apply versioned limit profiles
   - Submit fixture-authoritative verdicts
   - Save raw traffic and structured reports
   - Always perform cleanup in a `finally` path

### 5.2 No production-module reuse by copy

Review production modules for pin assignments and known hardware behavior.
Do not copy large production modules into the factory repository.

Extract or reimplement only the smallest hardware transaction required for a
test. For example:

- The modem test needs safe GPIO initialization, UART setup, bounded AT
  commands, and redacted response parsing.
- It does not need PPP, MQTT, APN selection, SNTP, recovery supervision, or
  production credentials.
- Thread network and RF testing are explicitly outside this factory-test
  repository. Reading the IEEE 802.15.4 EUI-64 for identity and traceability is
  permitted, but the firmware must not form, join, commission, or exchange
  traffic on a Thread network.

---

## 6. Factory protocol

### 6.1 Response envelope

Every response must include:

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

For errors:

```json
{
  "seq": 1,
  "cmd": "adc.sample",
  "status": "error",
  "code": "invalid_parameter",
  "firmware": "0.1.0",
  "protocol": "1.0",
  "product": "S5-NODE-OTBR",
  "board": "TBD",
  "data": {
    "field": "samples"
  }
}
```

Do not expose unstable internal ESP-IDF error strings as the only machine
readable error. Use stable protocol codes and add bounded diagnostic detail
when useful.

### 6.2 Initial command set

Implement and document:

| Command | Purpose |
| --- | --- |
| `identity` | Read target, silicon, flash, MAC, EUI-64 and reset identity |
| `session.start` | Open a traceable unit session |
| `session.list` | Return every manifest entry |
| `session.finish` | PASS only when every required item passed |
| `session.abort` | Close session without release |
| `safe` | Immediately restore safe hardware state |
| `fixture.record` | Store a station-authoritative verdict |
| `gpio.write` | Drive an allow-listed output |
| `gpio.read` | Sample an allow-listed input |
| `adc.sample` | Sample an allow-listed ADC path |
| `pwm.set` | Generate an approved frequency/duty setting |
| `zcd.capture` | Validate simulated 50 Hz or 60 Hz half-cycle input |
| `gps.check` | Check bounded GPS UART receive/transaction behavior |
| `modem.check` | Check EG912 GPIO, UART, AT, model and SIM readiness |

Do not expose a generic unrestricted modem command in the production factory
profile. If an engineering-only `modem.at` command is implemented, restrict it
to an allow-list and compile or configure it out of release artifacts.

---

## 7. Test manifest

### 7.1 Automatic or firmware-verifiable

- `device_identity`
- `usb_protocol`
- `base_mac`
- `thread_eui64`
- `firmware_identity`
- `gps_uart_rx`
- `modem_uart`
- `modem_identity`
- `sim_presence`

### 7.2 Station-authoritative

- `rail_3v3`
- `rail_5v`
- `gpio_lamp_ctrl`
- `gpio_psw_en`
- `gpio_modem_pwrkey`
- `gpio_modem_reset`
- `gpio_status_led`
- `gpio_ctrl_led`
- `adc_vrms`
- `adc_irms`
- `adc_5v`
- `pwm`
- `zcd_gpio`
- `zcd_50hz`
- `zcd_60hz`
- `gps_uart_tx`
- `modem_uart_tx_rx`

The manifest is provisional until the controlled schematic, fixture design,
approved limits, and factory station scope are available.

`session.finish` must reject:

- Any pending item
- Any failed item
- Missing unit identity
- Expired session
- Incompatible firmware or protocol identity
- Cleanup failure

---

## 8. Host runner and report requirements

Implement these initial commands:

```text
python -m tools.s5otbrft_runner discover
python -m tools.s5otbrft_runner self-test
python -m tools.s5otbrft_runner guided-test --operator-id <ID>
python -m tools.s5otbrft_runner validate-report <REPORT>
```

Support explicit `--port`, `--unit-id`, `--station-id`, and `--profile`.

The runner must:

- Reject ambiguous discovery when multiple DUTs are attached.
- Validate product, board, firmware, and protocol before testing.
- Match every response by sequence number.
- Preserve raw request/response frames.
- Record monotonic and wall-clock timing.
- Continue safe independent checks after a peripheral failure when transport
  remains trustworthy.
- Stop active testing when framing, sequence, transport, or identity becomes
  untrustworthy.
- Run `safe` and `session.abort` in cleanup.
- Return a nonzero exit status for FAIL or infrastructure error.
- Report successful incomplete testing as `PARTIAL`, not PASS.
- Keep validation profiles versioned and hash them into each report.
- Write reports atomically.

Minimum report fields:

```text
schema_version
report_id
result
production_release
unit_id
operator_id
station_id
fixture_revision
profile_id
profile_sha256
firmware_version
protocol_version
product
board
base_mac
thread_eui64
start_time
end_time
duration
tests
manifest_before_cleanup
cleanup
raw_exchange
runner_version
```

Never set `production_release:true` unless the approved production fixture
profile completed every required station-authoritative test and
`session.finish` returned PASS.

---

## 9. Raspberry Pi GUI boundary

The initial firmware repository should define the GUI contract but should not
mix GUI code into the firmware modules.

The GUI will later:

- Discover the DUT automatically.
- Scan or accept a unit ID.
- Flash approved factory and production images.
- Run the selected controlled profile.
- Present one operator action at a time.
- Prevent required steps from being skipped.
- Display clear PASS, FAIL, PARTIAL, and station-error states.
- Save reports locally when offline.
- Support authorized export and synchronization.
- Reflash production firmware after PASS.
- Verify the production firmware identity after reboot.

The GUI must call the Python runner as a library or through a stable local API.
Do not duplicate serial protocol logic in the GUI.

---

## 10. Implementation phases

### Phase 0 — resolve blocking hardware inputs

Before final fixture acceptance:

- Obtain the Node-OTBR schematic.
- Record the exact PCB name and revision.
- Map every accessible connector and pogo-pin point.
- Confirm factory power input.
- Confirm GPIO0 relay-driver polarity electrically.
- Confirm `PSW_EN` safe behavior.
- Confirm EG912 power-key and reset timing.
- Confirm GPS and EG912 UART wiring.
- Decide whether LTE, GPS fix, mains, lamp load, and metering
  calibration occur at this station or at later QC/QA stations.
- Obtain approved measurement tolerances and limit-set ownership.

Codex may build the protocol and software skeleton before these inputs are
resolved, but must mark affected tests provisional and must not invent limits.

### Phase 1 — repository foundation

Deliver:

- ESP-IDF project targeting ESP32-C6
- Single-app factory partition configuration
- Native USB console
- Safe board initialization
- Protocol framing and validation
- Identity command
- Session lifecycle
- Manifest lifecycle
- `safe`
- Unit tests for parser, session, manifest and safety state transitions
- Initial README and protocol documentation

Acceptance:

- Clean build with the approved ESP-IDF version.
- Unit tests pass.
- Ready frame appears after boot.
- Malformed or oversized input does not crash the device.
- Every error path leaves outputs safe.

### Phase 2 — low-voltage partial test

Deliver:

- Allow-listed GPIO commands
- ADC sampling
- PWM control
- ZCD capture
- GPS receive check
- EG912 bounded AT check
- Python discovery, self-test and report generation
- Versioned engineering partial profile

Acceptance:

- One command produces one matching response.
- All parameter boundaries are tested.
- `self-test` produces only `PARTIAL` or `FAIL`.
- Reports validate against the report schema.
- Interrupting the host process restores safe state whenever communication
  remains possible.

### Phase 3 — fixture integration

Deliver:

- Approved fixture contract
- Fixture measurement adapters
- Station-authoritative `fixture.record` workflow
- Rails, GPIO, ADC, PWM and ZCD coverage
- Complete production manifest
- Atomic factory reports

Acceptance:

- A known-good golden board passes every required test repeatedly.
- Known injected open, short, stuck, out-of-range, and communication faults are
  rejected.
- No fixture measurement can be replaced by GPIO register readback.
- `session.finish` is demonstrably fail-closed.

### Phase 4 — LTE functional station

Deliver only if included in the approved station:

- Factory test SIM and network policy
- LTE registration and PPP checks
- Factory-only MQTT loopback
- Optional GPS fix/antenna check

Acceptance:

- Tests are bounded by explicit timeouts.
- No production credentials are present.
- Sensitive modem identifiers are redacted.
- Network infrastructure failure is distinguished from DUT failure.

### Phase 5 — production handoff

Deliver:

- Raspberry Pi GUI integration contract
- Reproducible Raspberry Pi package
- Approved firmware binaries and SHA-256 hashes
- Operator work instruction
- Engineering maintenance guide
- Release and rollback procedure
- Production-image reflash and identity verification

Acceptance:

- Operator requires no source repository or ESP-IDF environment.
- Every released image and station package is versioned and hashed.
- Reports identify all software, hardware, fixture and limit-set revisions.
- The station cannot release a `PARTIAL` unit.

---

## 11. Verification requirements

### Firmware tests

Test at minimum:

- Valid and invalid JSON framing
- Missing `seq` or `cmd`
- Wrong JSON types
- Oversized frame and strings
- Unknown commands
- Commands outside an active session
- Session timeout
- Duplicate session start
- Abort and repeated abort
- Safe and repeated safe
- Pending, failed and complete manifests
- Invalid `fixture.record` test IDs
- Attempts to overwrite immutable automatic results
- GPIO allow-list enforcement
- ADC channel and sample-count limits
- PWM duty limits
- ZCD frequency and timeout handling
- GPS timeout and malformed NMEA
- Modem timeout and oversized response
- Cleanup after every command failure

### Host tests

Use a fake serial transport to test:

- Discovery of zero, one, and multiple DUTs
- Boot noise before the ready frame
- Sequence mismatch
- Missing response
- Malformed response
- Wrong product or board
- Unsupported protocol version
- Peripheral failure with continued safe checks
- Transport failure with immediate stop
- Ctrl+C cleanup
- Atomic report creation
- Schema validation
- Profile hashing
- Prevention of production PASS from a partial profile

### Hardware-in-loop tests

Document and execute:

- Repeated power cycles
- Reset during every active output test
- USB disconnect during a test
- Host application termination
- Session timeout while an output is active
- Stuck modem and GPS UART
- Missing modem and GPS modules
- Missing or invalid ZCD stimulus
- Out-of-range ADC stimulus
- Golden-board repeatability
- Known-fault board rejection

---

## 12. Coding rules for Codex

While implementing:

1. Inspect repository instructions and existing files before editing.
2. Work in small, reviewable phases.
3. Do not modify `theijattomoto/NODE-OTBR` or
   `theijattomoto/s5-node-factory-test` unless separately instructed.
4. Preserve user changes and do not overwrite unrelated work.
5. Prefer explicit state machines over scattered booleans.
6. Keep hardware access behind narrow interfaces.
7. Use fixed-size buffers with checked lengths.
8. Check every ESP-IDF return value that can affect safety or test validity.
9. Use bounded timeouts for UART, USB, radio and fixture operations.
10. Do not use heap allocation in framing or critical safety paths unless
    justified and tested.
11. Do not log credentials or sensitive identifiers.
12. Do not invent electrical acceptance limits.
13. Update protocol and operator documentation whenever behavior changes.
14. Add or update tests with each implemented behavior.
15. Build and run relevant tests before reporting a task complete.

For each implementation phase, report:

- Files created or changed
- Behavior implemented
- Tests executed and results
- Hardware assumptions
- Unresolved blockers
- Exact next recommended task

---

## 13. Definition of done

The project is production-ready only when:

- The hardware revision and fixture map are controlled.
- Safe states are electrically verified.
- Every manifest item has an approved verification method.
- Limit sets are approved and versioned.
- Firmware and host tests pass.
- Hardware fault-injection results are documented.
- The complete fixture test can generate PASS and reject known faults.
- Partial tests cannot generate production PASS.
- Raspberry Pi packaging works without source-code access.
- Factory and production images are versioned and hashed.
- The production image is reflashed and verified after test.
- Reports contain complete traceability.
- Operator, engineering, fixture, protocol and release documentation are
  approved.

---

## 14. First Codex execution prompt

Use the following prompt after creating the empty target repository:

> Implement Phase 1 of `CODEX_IMPLEMENTATION_PLAN.md` in this repository.
> First inspect the repository and the reference designs
> `theijattomoto/NODE-OTBR` and `theijattomoto/s5-node-factory-test`. Do not
> modify either reference repository. Create the minimum deterministic ESP-IDF
> ESP32-C6 factory-test application with native USB framing, verified safe
> initialization, identity, session lifecycle, fail-closed manifest, `safe`,
> protocol validation, unit tests, README, and protocol documentation. Treat
> the board revision and `PSW_EN` safe behavior as unresolved inputs; isolate
> them in `factory_board.h` and do not invent production electrical limits.
> Build the firmware and run all available tests. Finish with a concise report
> of changed files, test results, assumptions, blockers, and the next task.
