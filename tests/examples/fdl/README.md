# Example SiLA 2 FDL Files

Real-world SiLA 2 Feature Definition Language (FDL) XML files collected from
open-source repositories, for use as codegen and parser test inputs.

## Sources

### `sila_base/` — SiLA2 Official Feature Definitions

Source: <https://gitlab.com/SiLA2/sila_base>

**Madisoft — Microplate Reader (com.madisoft)**

| File | Description |
|------|-------------|
| `AbsorbanceReaderService-v1_0.sila.xml` | Absorbance measurement |
| `FluorescenceReaderService-v1_0.sila.xml` | Fluorescence measurement |
| `LuminescenceReaderService-v1_0.sila.xml` | Luminescence measurement |
| `ReaderStatusProvider-v1_0.sila.xml` | Reader status / readiness |

**Unitelabs — Laboratory Robot (ch.unitelabs)**

| File | Description |
|------|-------------|
| `RobotController-v1_0.sila.xml` | Robot arm motion control |
| `GripController-v1_0.sila.xml` | Gripper open/close |
| `BatteryController-v1_0.sila.xml` | Battery status monitoring |
| `DeviceCalibrationService-v1_0.sila.xml` | Device calibration |
| `PlateCalibrationService-v1_0.sila.xml` | Plate position calibration |
| `RobotTeachingService-v1_0.sila.xml` | Position teaching |
| `InitializationController-v1_0.sila.xml` | Device initialization |
| `ProgramController-v1_0.sila.xml` | Program execution control |
| `StateService-v1_0.sila.xml` | Device state reporting |

**Tecan — Fluent Platform (com.tecan)**

| File | Description |
|------|-------------|
| `GatewayService-v1_0.sila.xml` | Script execution gateway |
| `ErrorRecovery-v0_1.sila.xml` | Error recovery handling |
| `LicensingService-v0_1.sila.xml` | License management |
| `Logging-v0_1.sila.xml` | Log streaming |
| `MoveInteractionCommands-v0_1.sila.xml` | Interactive move commands |
| `RestartService-v1_0.sila.xml` | Service restart |

**TU Berlin — Bioprocess Device (de.tuberlin.bioprocess)**

| File | Description |
|------|-------------|
| `DeviceInformationProvider-v1_0.sila.xml` | Device metadata provider |
| `MessagingController-v1_0.sila.xml` | Messaging / notifications |
| `StorageService-v1_0.sila.xml` | Data storage |

**SiLA Standard — Instrument Features (org.silastandard)**

| File | Description |
|------|-------------|
| `CoverController-v1_0.sila.xml` | Instrument cover open/close |
| `LabwareTransferManipulatorController-v1_0.sila.xml` | Labware transfer robot |
| `LabwareTransferSiteController-v1_0.sila.xml` | Transfer site management |

**diginbio — Example (de.diginbio)**

| File | Description |
|------|-------------|
| `WarpdriveService-v1_0.sila.xml` | Example sci-fi device |

### `cetoni/` — CETONI Laboratory Automation Modules

Syringe pumps, contiflow pumps, valves, I/O, motion control, PID controllers.
Source: <https://github.com/CETONI-Software> (`sila_cetoni_pumps`, `_valves`, `_io`, `_motioncontrol`, `_controllers`, `_core`)

| File | Description |
|------|-------------|
| `PumpFluidDosingService.sila.xml` | Fluid dosing commands |
| `PumpDriveControlService.sila.xml` | Drive motor control |
| `SyringeConfigurationController.sila.xml` | Syringe geometry / calibration |
| `ForceMonitoringService.sila.xml` | Force sensor monitoring |
| `ContinuousFlowDosingService.sila.xml` | Contiflow dosing |
| `ContinuousFlowConfigurationService.sila.xml` | Contiflow pump configuration |
| `ValvePositionController.sila.xml` | Valve position switching |
| `ValveGatewayService.sila.xml` | Valve gateway routing |
| `AnalogInChannelProvider.sila.xml` | Analog input reading |
| `AnalogOutChannelController.sila.xml` | Analog output control |
| `DigitalInChannelProvider.sila.xml` | Digital input reading |
| `DigitalOutChannelController.sila.xml` | Digital output control |
| `AxisPositionController.sila.xml` | Single axis positioning |
| `AxisSystemControlService.sila.xml` | Multi-axis system control |
| `AxisSystemPositionController.sila.xml` | Multi-axis positioning |
| `ControlLoopService.sila.xml` | Control loop configuration & execution |
| `ShutdownController.sila.xml` | Device shutdown |
| `SystemStatusProvider.sila.xml` | System status reporting |

### `panda/` — Franka Emika Panda Robot Arm

7-DOF collaborative robot arm for lab plate handling.
Source: <https://github.com/FlorianBauer/panda-controller>

| File | Description |
|------|-------------|
| `RobotController.sila.xml` | Robot motion control |
| `PlateTypeManager.sila.xml` | Plate type definitions |
| `SiteManager.sila.xml` | Worktable site management |

### `ot2/` — Opentrons OT-2 Liquid Handling Robot

Open-source benchtop liquid handler.
Source: <https://github.com/FlorianBauer/ot2-controller>

| File | Description |
|------|-------------|
| `Ot2Controller.sila.xml` | Protocol execution & pipetting |

### `bioshake-qx/` — QInstruments BioShake QX Plate Shaker

Microplate shaker with temperature control.
Source: <https://gitlab.com/sila-driver-group/bioshake-qx>

| File | Description |
|------|-------------|
| `ShakeController.sila.xml` | Shaking control |
| `TemperatureController.sila.xml` | Temperature control |
| `SettingsProvider.sila.xml` | Device settings |

### `teleshake/` — ThermoScientific Teleshake 1536 Plate Shaker

High-throughput microplate shaker.
Source: <https://gitlab.com/sila-driver-group/teleshake>

| File | Description |
|------|-------------|
| `ShakeController.sila.xml` | Shaking control |
| `Settings.sila.xml` | Device settings |

### `hiperistatlic/` — HiPeristaltic Open-Source Peristaltic Pump

Multi-channel peristaltic pump for self-driving labs.
Source: <https://github.com/gunakkoc/HiPeristaltic>

| File | Description |
|------|-------------|
| `HiPeristaltic.sila.xml` | Pump channel control |

### `sila_base/valid-fdl/` — XSD Validation Positive Cases

22 files from the official XSLT test suite that must parse successfully.
Source: <https://gitlab.com/SiLA2/sila_base> `xslt/test/valid-fdl/`

### `sila_base/invalid-fdl/` — XSD Validation Negative Cases

31 intentionally malformed FDL files (duplicate identifiers, invalid dates,
nested lists, cyclic references, etc.) for testing parser rejection.
Source: <https://gitlab.com/SiLA2/sila_base> `xslt/test/invalid-fdl/`

## License

| Source | License |
|--------|---------|
| [SiLA2/sila_base](https://gitlab.com/SiLA2/sila_base) | MIT |
| [CETONI-Software](https://github.com/CETONI-Software) (all repos) | BSD-3-Clause |
| [FlorianBauer/panda-controller](https://github.com/FlorianBauer/panda-controller) | Apache-2.0 |
| [FlorianBauer/ot2-controller](https://github.com/FlorianBauer/ot2-controller) | Apache-2.0 |
| [sila-driver-group/bioshake-qx](https://gitlab.com/sila-driver-group/bioshake-qx) | MIT |
| [sila-driver-group/teleshake](https://gitlab.com/sila-driver-group/teleshake) | MIT |
| [gunakkoc/HiPeristaltic](https://github.com/gunakkoc/HiPeristaltic) | Apache-2.0 |
