# S5 Node-OTBR PCB Factory-Test Firmware

Deterministic low-voltage factory-test firmware for assembled S5 Node-OTBR
PCBs based on the ESP32-C6. Phase 1 provides safe initialization, device
identity, a strict native-USB protocol, traceable sessions, and a fail-closed
test manifest.

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

## Phase 1 behavior

The firmware sends and accepts one JSON object per line with the exact prefix:

```text
@S5OTBRFT 
```

Implemented commands are `identity`, `session.start`, `session.list`,
`session.finish`, `session.abort`, `fixture.record`, and `safe`. See
[the protocol specification](docs/factory_protocol.md) for schemas and stable
error codes.

The provisional manifest contains future GPS and EG912 automatic tests that
Phase 1 cannot complete. Therefore, `session.finish` cannot return PASS in this
phase even if every station-owned item is recorded. This is intentional.

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

## Phase 1 partial automation

Install the pinned host dependency, discover the DUT, and run the automatic
no-fixture test:

```powershell
python -m pip install -r tools/requirements.txt
python -m tools.s5otbrft_runner discover
python -m tools.s5otbrft_runner self-test --port COM9 --unit-id BENCH-001
```

`--port` and `--unit-id` are optional when exactly one matching DUT is
connected. Without a unit ID, the runner derives an engineering identifier
from the base MAC. Reports are written atomically under `reports/`.

A successful run is always `PARTIAL`, never production PASS. It validates the
USB protocol envelope, product/board/firmware/protocol identity, ESP32-C6
target, 4 MB flash, base MAC, IEEE 802.15.4 EUI-64, traceable session, and
manifest state. GPS, EG912, rail, GPIO, ADC, PWM, and ZCD tests remain pending.
Every run finishes with `safe` and `session.abort`, including failed runs.

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
