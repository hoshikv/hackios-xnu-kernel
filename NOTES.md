# HackiOS peridot port — working notes

Target: Xiaomi peridot (POCO F6 / Redmi Turbo 3), Qualcomm SM8635 (Snapdragon 8s Gen 3).
Forks: hoshikv/hackios-xnu-kernel, hoshikv/hackios-xnu-userland. Branch: `peridot`.
Method: HackiOS docs/PORTING.md + common/new-device.sh. No local clones; remote edits only.

## Status

- STEP 0 recon: done.
- STEP 1 fork + branch: done (this commit adds notes).
- STEP 2 device support: in progress.
- STEP 3 kernel boot: pending.
- STEP 4 userland: pending.
- STEP 5 input: pending.
- STEP 6 vibration: pending.
- STEP 7 CI build: pending.

## Recon: how a device is added

Kernel repo has no `.github/workflows` upstream; all builds are scripts. A CI workflow must be
added (ASSUMPTION: allowed, because the task requires kvfsrl Actions builds).

Files that define a device:
- `common/devices/<codename>.conf` — profile read by `common/lib.sh`, `build.sh`, `run.sh`.
- `common/devices/<codename>.platform.c` — `struct platform` for `xnuloader/loader.c`; loader
  picks it by the GOP framebuffer base.
- `xnuloader/loader.c` — add `&platform_<codename>` to `platforms[]`.
- `uefi/files/Platforms/<Vendor>/<codename>Pkg/` — Mu-Silicium package (dsc/fdf/DeviceBuild.py,
  APRIORI/DXE/RAW, ConfigurationMapLib, MemoryMapLib).
- `uefi/files/Resources/Configs/<codename>.toml` — FD_BASE/FD_SIZE, boot image params.
- `uefi/files/Resources/DTBs/<codename>.dtb` — device tree from vendor_boot.
- `uefi/files/Binaries/<codename>/` — UEFI drivers from the phone's xbl.img (UEFIReader).
- `uefi/mu-silicium.patch` — BootShim copies the XNUP payload to PAYLOAD_BASE; BootManagerLib
  starts the loader from it.
- `xnu/redwood.patch` + `xnu/files/` — kernel drivers. Peridot reuses generic code; device
  drivers are named redwood_* and need peridot variants where hardware differs.
- `common/build.sh <codename>` and `common/run.sh <codename>` drive build/run.
- userland repo is checked out into the same tree by `common/setup.sh`; `userland/build.sh`
  makes root.apfs/system.apfs; peridot needs its own job list/preferences only if redwood's
  are device-specific.

Existing device profiles: `redwood.conf`, `qemu-virt.conf`, `_template.conf`.
redwood loader platform: io_base 0x00400000/0x3FC00000, gic 0x17A00000/0x17A60000,
cores 8, shift 8, small_cores 0x0f, fb_base 0xE1000000, psci_hvc 0.
redwood boot: FD_BASE 0x9FD00000, FD_SIZE 0x200000, PAYLOAD_BASE 0xF0000000.

## Peridot hardware facts

Source: Xiaomi peridot device tree (peridot.dts from vendor_boot / Mu-Silicium Resources/DTBs).
Cite: `peridot.dts` nodes.

- SoC: SM8635, 8 cores:
  cpu@0/100/200 = 3x Cortex-A520, capacity-dmips-mhz 0x400 -> small_cores 0x07
  cpu@300/400/500/600 = 4x Cortex-A720, 0x69a
  cpu@700 = 1x Cortex-X4, 0x733. MPIDR shift 8 (reg 0x100..0x700).
- GICv3: dist 0x17100000 size 0x10000; redist 0x17180000 size 0x200000;
  redistributor-stride 0x40000. `interrupt-controller@17100000`.
- Peripherals window: 0x00400000 + 0x3FC00000 (same convention as redwood; covers
  0xA94000 GENI UART, 0x17100000 GIC, 0x1D84000 UFS).
- UART: GENI0 at 0xA94000 (not PL011) -> platform.uart = 0.
- PSCI: `psci` node, SMC conduit on phones -> psci_hvc = 0.
- Framebuffer: 0xE3940000, panel 1220x2712, stride 4880, 120 Hz. GOP base from
  boot log "AppleCLCD ... at 0xe3940000" -> platform.fb_base = 0xE3940000.
- Boot chain: FD_BASE 0xA7100000, FD_SIZE 0x300000 (Mu peridot config). PAYLOAD_BASE
  0xF0000000 (free low DDR; redwood default).
- Touch: Goodix GT9916R BerlinD, SPI on GENI SE4 0xA90000; TLMM reset 128, IRQ 129
  (`xiaomi_touch@0`, goodix,reset-gpio/irq-gpio); rails ldob13 3.3V + ldob23 1.8V.
- Buttons: pwrkey = PMK8550 PON (`pwrkey`); vol-up = pmxr2230 SID1 GPIO6.
- Haptics: Awinic AW8697 (also SIH6887 alternate) on I2C, `aw8697_haptic@5A`
  (compatible awinic,awinic_haptic); QPNP vibrator-ldo also present.
- GPU: Adreno 735 (SM8635). Wi-Fi: WCN7850/6750-family, ath11k (needs confirmation).
- Display: 1220x2712; iOS surface can be 1080x2400 scaled (kernel already does this).

## Assumptions

- ASSUMPTION: peridot UEFI port is reused from the existing Mu peridot tree
  (`Platforms/Xiaomi/peridotPkg`, `Binaries/peridot`, `Resources/Configs/peridot.toml`),
  then HackiOS's XNUP payload patch is applied on top.
- ASSUMPTION: PAYLOAD_BASE 0xF0000000 is free on peridot; FD_BASE 0xA7100000 + 0x300000.
- ASSUMPTION: GOP framebuffer base is 0xE3940000 (matches cont_splash/AppleCLCD address).
- ASSUMPTION: no peridot-specific CI exists upstream; a build workflow will be added.

## Build

Upstream build (documented): `common/build.sh <device>` -> kernel (needs theos + iOS 16.5 SDK,
otherwise prebuilt), loader (clang+lld-link, otherwise prebuilt), images (needs ios165/fetch.sh),
boot image via `mkxnuimg.py`. Device test uses `common/run.sh` / `./xl`.

CI plan: add `.github/workflows/peridot.yml` that runs the script steps that do not need the
7.4 GB IPSW (kernel/loader/boot-image); use prebuilt kernel/loader when the toolchain is absent.
Artifacts: out/peridot-boot.img, loader.efi, kernel if built.

## CI (peridot)

- `.github/workflows/peridot-boot.yml` is dispatch-only and builds the UEFI, loader and boot
  image. Actual runs are dispatched from `kvfsrl/peridot-xnu-build` (workflow
  `.github/workflows/hackios-peridot.yml`), which checks out this fork at `peridot`.
- First run (hoshikv, 39797488644) failed in `Build UEFI`: `llvm-objcopy: No such file or
  directory`. Fixed by installing the `llvm` package next to clang/lld. The build now also
  uses the system `nasm` through the Mu extdep state.
- Run time is dominated by `uefi/apply.sh` (Mu-Silicium clone + recursive submodules, ~3 GB)
  and pip, not by kernel compilation.
- GIC redistributor window: peridot has 8 redistributor frames at stride 0x40000, so the ADT
  `/arm-io/gic` now advertises 0x200000 bytes (`platform.gic_redist_size`). The old fixed
  0x100000 only covered four cores and left boot CPU 7 without a redistributor.
- ASSUMPTION: `kvfsrl` runs the build because the user requires it; if the queue is stalled,
  do not fall back to hoshikv Actions.
