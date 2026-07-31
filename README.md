# S5 Node-OTBR PCB Factory-Test Firmware

Deterministic low-voltage factory-test firmware for assembled S5 Node-OTBR
PCBs based on the ESP32-C6. It provides safe initialization, device identity,
a strict native-USB protocol, traceable sessions, partial peripheral
automation, guided LED checks, and a fail-closed test manifest.

This image is a foundation for finding assembly faults such as missing,
misoriented, open, shorted, stuck, or incorrectly connected components. GPIO
register readback is not proof of PCB continuity. A production verdict
requires approved fixture measurements or optical/current observation.

## Safety boundary

- Do not connect mains or a lamp load during this low-voltage PCB test.
- Fixture drivers must default to high impedance and must not back-power the
  DUT.
- Do not apply signals above the measured 3.3 V I/O rail, below ground, or
  while the DUT is unpowered.
- No production electrical limits are defined by this repository.
- The PCB revision and `PSW_EN` safe behavior are unresolved. Phase 1 leaves
  GPIO3 as a floating input and never drives it.
- Known outputs are initialized before USB parsing: lamp control high/OFF,
  modem PWRKEY high/idle, status and control LEDs high/OFF, PWM low/stopped,
  and modem reset high/inactive.

GPIO latch readback during initialization confirms the ESP-IDF configuration,
not the assembled electrical net.

If safe initialization fails, the firmware logs the exact GPIO and ESP-IDF
operation and enters a stable fail-stop state. It does not continuously reboot.
The physical pad level is deliberately not used as proof that output
configuration succeeded because assembled board circuitry can load the pad;
the approved fixture remains authoritative.

## Protocol behavior

The firmware sends and accepts one JSON object per line with the exact prefix:

```text
@S5OTBRFT 
```

Implemented commands are `identity`, `session.start`, `session.list`,
`session.finish`, `session.abort`, `fixture.record`, and `safe`. See
[the protocol specification](docs/factory_protocol.md) for schemas and stable
error codes.

The provisional manifest still contains fixture-authoritative rail, GPIO,
calibration, PWM, ZCD, GPS transmit, and modem bidirectional tests.
`session.finish` cannot return PASS from the partial workflow. This is
intentional.

## Build

Use ESP-IDF 5.5.3 and its installed ESP32-C6 toolchain:

```powershell
idf.py set-target esp32c6
idf.py build
idf.py -p COMx flash monitor
```

The project uses 4 MB flash, the built-in single-app partition table, and the
ESP32-C6 native USB Serial/JTAG console on GPIO12/GPIO13.

## Native unit tests

The host tests compile the same request parser, manifest, session, safety, and
error-cleanup code used by the firmware:

```powershell
cmake -S test -B build-host-tests -G Ninja `
  -DIDF_PATH=C:/esp/v5.5.3/esp-idf
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
```

## Partial automation

Install the pinned host dependency, discover the DUT, and run the automatic
no-fixture test:

```powershell
python -m pip install -r tools/requirements.txt
python -m tools.s5otbrft_runner discover
python -m tools.s5otbrft_runner self-test --port COM9 --unit-id BENCH-001
python -m tools.s5otbrft_runner guided-test --port COM9 --unit-id BENCH-001 --operator-id OP-01
```

Development runs show `[START]`, `[TEST]`, `[PASS]`, `[PARTIAL]`, `[FAIL]`,
and `[CLEANUP]` progress directly in the terminal. Add `--verbose` to print
every transmitted and received protocol frame:

```powershell
python -m tools.s5otbrft_runner self-test --port COM9 --unit-id BENCH-001 --verbose
python -m tools.s5otbrft_runner guided-test --port COM9 --unit-id BENCH-001 --operator-id OP-01 --verbose
```

`--port` and `--unit-id` are optional when exactly one matching DUT is
connected. Without a unit ID, the runner derives an engineering identifier
from the base MAC. Reports are written atomically under `reports/`.

A successful run is always `PARTIAL`, never production PASS. It validates the
USB protocol envelope, product/board/firmware/protocol identity, ESP32-C6
target, 4 MB flash, base MAC, IEEE 802.15.4 EUI-64, checksum-valid GPS RMC
reception, EG912 AT/model/SIM readiness, raw VRMS/IRMS/5 V ADC snapshots,
traceable session, and manifest state.

`guided-test` adds active-low status and control LED OFF-ON-OFF observations.
The operator answers only `Y` or `N`; the visual verdicts are recorded in the
same JSON report. `led-check` is a compatibility alias.

Rail acceptance, ADC engineering-unit calibration, lamp/modem power outputs,
`PSW_EN`, PWM, ZCD frequency, mains, lamp load, and metering tests remain
pending. Every run finishes with `safe` and `session.abort`, including failed
runs.

Peripheral failures do not stop the remaining safe tests. The runner continues
through GPS, modem, every ADC channel, guided LEDs, and manifest capture when
the serial protocol remains trustworthy. A framing, sequence, or transport
failure stops active testing. Each run ends with a consolidated terminal
summary containing individual test results, manifest counts, cleanup status,
errors, overall result, and JSON report path.
Failure reasons are printed immediately and retained in the JSON report.

Run the station-runner unit tests without hardware:

```powershell
python -m unittest tools.test_s5otbrft_runner -v
```

## Hardware verification still required

Before production use, execute and retain hardware evidence for:

- ready frame after repeated resets and power cycles;
- fixture-observed safe levels at every controllable output;
- session timeout while a later active-output test is running;
- USB disconnect and host termination cleanup;
- controlled low/high stimulus and continuity checks;
- golden-board repeatability and known-fault rejection.

These tests require the controlled schematic, exact PCB revision, approved
fixture, and approved measurement limits.
