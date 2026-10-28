# Thermal service and sensors

## Thermal service (kernel: `bsd/dev/hakios.thermal.c`, userland: `thermald`)

With `cpufreq=1` every core this kernel uses runs at the top of its frequency table, and nothing else would ever
slow it down. The thermal service is the governor that Android's thermal engine would otherwise be:

| What | How |
|---|---|
| Sensors | all 81 zones the device tree names, every 250 ms; 35 TSENS (three blocks: 14 + 16 + 5; cores, GPU, DSPs, memory, modem area, camera, video) have readers today. The other 46 are enumerated: 19 PMIC/ADC temperatures (10 PMIC die, 9 board thermistors) and 27 non-temperature QMI/BCL/current/SOCD zones |
| Limits | from the phone's own device tree; slowing starts at 95 C (cores, GPU, modem area) or 100 C (the rest), lower than Xiaomi's 108..112 C because nothing else governs here. PMIC/ADC zones use 100 C/115 C; raw zones have no limits |
| Raw zones | QMI, BCL, current and SOCD values are not temperatures; the governor skips them and the table prints them as `raw` |
| Slowing | one row of the frequency table a tick, three when 5 C past the limit; heat outside the cores slows every group |
| Speeding up | one row a second, only after a second of being 5 C under the limit (no see-saw) |
| Battery | from 45 C the pressure is at least moderate, from 50 C the cores are held at half speed, from 58 C at the lowest |
| Emergency | past 115 C everything at the lowest row at once; two seconds there and the run ends (a panic: the loader resets the phone and keeps the log) |
| Sensors silent | five seconds without any CPU reading: the cores are held at half speed |
| iOS | `thermald` posts the level as `com.apple.system.thermalpressurelevel`, so `ProcessInfo.thermalState` changes in apps |

```sh
sysctl kern.hakios.thermal            # the table: every sensor, every group of cores
sysctl kern.hakios.thermal_pressure   # 0 nominal, 1 moderate, 2 heavy, 3 trapping, 4 sleeping
thermald show                          # the same table, from the userland
```

Boot arguments: on by default with `cpufreq=1`; `thermal=0` off; `thermal=2` watch and report without `cpufreq=1`.

The governor's decisions are tested on the computer in 11 scenarios (`tools/thermal-test/run.sh`, in CI): heat-up,
cool-down without see-saw, a single hot group, package heat, near-critical, a one-tick spike, a sustained emergency,
silent sensors, a hot battery, a configuration with one group of cores. The kernel builds with it and boots in QEMU
(where it stays off: QEMU has no TSENS). The redwood bring-up (POCO X5 Pro, 22 October 2026, `cpufreq=1 cpus=8`, the
cores at 1.8 / 2.4 / 2.4 GHz, one core kept busy for a minute and a half) had all 27 of its sensors answer at 33..38 C,
pressure nominal, nothing slowed. Peridot's table names all 81 zones from its device tree; the 35 TSENS have readers,
the 19 PMIC/ADC temperature zones are mapped but not read yet, and the 27 QMI/BCL/current/SOCD zones are raw. The
host tests pass for all 81 zones, but the table has not yet been exercised on the phone; the governor slowing the
cores still needs a load that heats them past 95 C.

## Sensors (accelerometer, gyroscope, compass, light, proximity, barometer)

On this phone they are not wired to the application processor: they hang off the audio DSP, behind its sensor
framework (Qualcomm's "SEE"), reached through the QMI service 400 over QRTR. What is there and what is missing:

| Step | State |
|---|---|
| the DSP started, GLINK, QRTR hello, the DSP's services listed | done (`adsp=1`, `bsd/dev/hakios.adsp.c`) |
| which services it offers and which it looks for on our side | `sysctl kern.hackios.adsp_services` (new) |
| QRTR ports of our own on the DSP's channel (for any QMI client) | to do |
| the service locator and notifier the DSP looks for (its sensor domain waits for them) | to do |
| the sensor registry the DSP reads from the phone's Android partitions | to do |
| a SEE client (service 400): find the sensors, stream their samples | to do |
| the samples to iOS as the HID events CoreMotion reads | to do |

The next run on the phone with `adsp=1` answers the first open question: `sysctl kern.hackios.adsp_services` shows
whether service 400 appears and what the DSP is waiting for.

## Location (GPS)

The GNSS receiver belongs to the modem (`qcom,mss@4080000`), which this port does not start yet; location comes
with the modem, through its QMI location service (16).
