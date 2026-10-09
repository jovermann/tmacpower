# tmacpower

`tmacpower` is a terminal UI app showing power flowing into a MacBook, power consumed, and power charging or discharging its battery.

## Build

Requirements: C++23 compiler, and the macOS IOKit and CoreFoundation frameworks.

```sh
make
```

## Usage

```sh
./tmacpower                 # Live TUI; press q or Ctrl-C to quit
./tmacpower --interval=2    # Refresh every two seconds
./tmacpower --no-ui         # Print data to terminal and exit
./tmacpower --help
```

## Readings

| Label | Meaning |
| --- | --- |
| Supply delivered | Instantaneous external input reported by `PowerTelemetryData.SystemPowerIn`, in W. Zero when disconnected. |
| Machine consumed | Instantaneous `PowerTelemetryData.SystemLoad`, in W. |
| Battery charging | Positive battery power entering the battery, in W. |
| Battery discharging | Positive battery power leaving the battery, in W. |
| Adapter capacity | Adapter's rated or negotiated wattage. This is the available limit, not the current draw. |

The battery power reading comes from `PowerTelemetryData.BatteryPower`. If that field is absent, the tool uses battery voltage multiplied by battery current. Other battery details come from the `AppleSmartBattery` IOKit service. Adapter capacity comes from the battery's adapter details or `IOPSCopyExternalPowerAdapterDetails`.

These are controller telemetry estimates. They do not include every conversion loss or peripheral draw, so the displayed flows need not balance exactly. A value shown as `unavailable` was not reported by the Mac. The tool does not require administrator privileges.

## Tests

Run `make test`. Tests require a Mac with a built-in battery.

## License

Boost Software License 1.0; see [LICENSE](LICENSE).
