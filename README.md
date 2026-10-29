# HackiOS — XNU kernel

**HackiOS** boots Apple's open-source **XNU kernel** (`xnu-8796.121.2`, the kernel of iOS 16.5) on Android phone
hardware and runs the **unmodified iOS 16.5 user space** on top of it: launchd → backboardd → SpringBoard →
apps. Our own EFI loader, our own replacements for Apple's closed drivers, a UEFI port, and a Linux Wi-Fi driver
on a compatibility layer make that possible.

This checkout targets the **Xiaomi POCO F6 / Redmi Turbo 3 (codename `peridot`, Qualcomm SM8635 / Snapdragon 8s Gen 3)**.
The phone boots through stock Fastboot with `fastboot boot`; XNULoader then receives the kernel and system image over USB.
The current workflow keeps the boot and system images in RAM. Rebooting returns to Android. QEMU (`virt`, aarch64)
remains available for host-side development.

This repository is half of the project:

| Repository | What |
|---|---|
| **hackios-xnu-kernel** (this one) | XNU patches, the EFI loader, the UEFI port, Wi-Fi, QEMU scripts, `xl`, prebuilt kernel/loader/UEFI, `common/` scripts |
| **upstream/main userland** | our init, injected libraries, launchd jobs, the APFS image builder, the scripts that take iOS out of Apple's IPSW, Metal/Mesa work. The old `hackios-xnu-userland` repository is retired. |

> **No Apple files are in either repository.** `ios165/fetch.sh` downloads the iOS 16.5 IPSW from Apple's own CDN
> to *your* machine and extracts what is needed there. HackiOS is an independent research project by OOSEX GANG and
> is not affiliated with, authorised or endorsed by Apple Inc. iOS, iPhone and Apple are trademarks of Apple Inc.

Peridot support is an active bring-up. Results below distinguish what has been seen on the phone from changes
that are only built and still need another device run.

## Peridot status

| Area | Current status |
|---|---|
| Boot and userland | UEFI has been booted from stock Fastboot and iOS userland has reached SpringBoard on the POCO F6. |
| GPU / UI | Adreno A735 accelerated rendering has appeared to work, including a smooth run. Other runs showed frame stalls, black surfaces, missing widgets, and a blue/black wallpaper. XNU framebuffer/GPU recovery changes and userland packed-16-bit/PosterBoard startup changes are prepared; these latest changes have not been validated on the phone yet. The system UI uses the Mesa/Freedreno OpenGL ES path. Full Metal acceleration is experimental and is not established as working. |
| Touch | Touch input works, but can be delayed or unresponsive. The current profile requests a 4 ms poll interval and 480 Hz; this tuning is not yet verified on-device. |
| Battery | Battery reporting was observed working in a previous userland run. |
| Thermal | `thermal=1` enables the kernel frequency governor and `ddr=0` avoids a previously crashing DDR RPMh vote. The phone was reported to get very hot; effective thermal sensor feedback and throttling have not yet been confirmed. |
| Volume and haptics | Volume up/down and vibration were reported not working. |
| Wi-Fi and audio | Not confirmed working on Peridot; treat as unfinished. |
| Bluetooth | The WCN6750 UART transport, firmware download, and upstream-userland bridge are implemented but not yet boot-tested. Build the bridge with `BLUETOOTH=1`; `bt=1` is opt-in for testing (`BT=1 common/run.sh peridot`). |

These results are specific to the Peridot port. Redwood screenshots, firmware names, and hardware claims do not describe this target.

It is a research bench, not a ROM: a session lasts a given number of seconds, then the kernel returns to the
loader and hands over its log.

## Quick start: QEMU, no kernel build

`prebuilt/` holds the kernel, loader and our programs built from these repositories, so all you need is the system
image, which is made on your machine from Apple's IPSW.

Needs: Linux x86-64, ~12 GB free disk, 4 GB free RAM. Packages (Arch):

```sh
sudo pacman -S --needed git python 7zip lz4 gcc qemu-system-aarch64 qemu-ui-gtk edk2-aarch64
```

(Debian/Ubuntu: `git python3 7zip liblz4-dev gcc qemu-system-arm qemu-efi-aarch64`; 7-Zip must be 23 or newer — it
reads APFS.)

```sh
git clone https://github.com/oosexgang/hackios-xnu-kernel.git hackios
cd hackios
common/setup.sh               # extracts upstream/main userland into this tree, checks the host
ios165/fetch.sh               # downloads 7.4 GB from Apple's CDN and extracts what is needed (5-15 min)
common/build.sh qemu-virt     # builds the APFS images (and the kernel, if the toolchain is there)
common/run.sh qemu-virt       # a 20-minute session in a QEMU window
```

The lock screen appears after 2–3 minutes.

- **Mouse with the left button held = finger.** Unlock / leave an app: swipe up from the very bottom edge; back:
  swipe from the left edge.
- Keys: `H` home, `L` lock/power, `↑`/`↓` volume, `F11` (hold) five fingers.
- From a script: `qemu/touch.py swipe 0.5 0.985 0.5 0.3 250`, `qemu/touch.py tap 0.615 0.71`,
  `qemu/touch.py key home`; screenshot: `qemu/screendump.sh shot.png`.
- Right after SpringBoard appears the system is still registering apps: give it 30–40 s.
- Debian/Ubuntu UEFI firmware: `QEMU_EFI=/usr/share/AAVMF/AAVMF_CODE.fd QEMU_VARS=/usr/share/AAVMF/AAVMF_VARS.fd`.

## On the phone (POCO F6 / Redmi Turbo 3)

You need a **POCO F6 / Redmi Turbo 3 (`peridot`, SM8635)** with an unlocked bootloader, `adb` and `fastboot`.
The normal bring-up uses stock Fastboot to boot UEFI, then `xl` to upload the kernel and userland to XNULoader.
It does not require flashing the phone's partitions.

**The phone's own firmware** is not in the repository. Collect it from the Peridot phone's Android vendor image.
The profile expects `fw-gpu`,
`fw-wifi`, `fw-audio`, and `fw-haptic` directories beside the workspace. The GPU is Adreno A735; its firmware
uses the `gen71100` family. Exact segment files can vary by vendor build, so copy all segments referenced by each
manifest rather than assuming a fixed list.

Peridot audio also needs the DSP device-tree image and every segment named by its header; without these,
the DSP may authenticate `adsp.mdt` but fail to announce itself to the application processor:

```sh
adb pull /vendor/firmware_mnt/image/adsp_dtb.mdt fw-audio/
# Pull each /vendor/firmware_mnt/image/adsp_dtb.bNN segment referenced by adsp_dtb.mdt into fw-audio/.
```

The Peridot input node is `/dev/hk-touch`; the legacy `/dev/redwood-touch` name remains as a compatibility alias.

The Peridot profile uses `GL=1` and the `gpu=1` boot argument for the Mesa/Freedreno OpenGL ES path. `metal=1`
only enables an experimental Metal registry switch; it does not mean the compositor or app Metal rendering is
working. Keep Metal plugin discovery opt-in (`metal_props on`).

(Put the `fw-*` directories next to the workspace. Wi-Fi also needs `fw-wifi/mac` — the radio's address on one
line — and `fw-wifi/network` — network name and password. `userland/build.sh` prints which firmware went in.)

```sh
common/build.sh peridot GL=1         # Peridot images and boot artifacts
common/build.sh peridot BLUETOOTH=1  # same, plus the Bluetooth bridge (needs the cross toolchain)
BT=1 common/run.sh peridot           # boot with bt=1 for a Bluetooth test
```

or step by step with `xl`:

```sh
# From stock Fastboot, run the prepared Peridot workflow (paths may be overridden via environment variables).
./runs.sh
./xl screenshot shot.png
./xl reboot           # return to Android
```

`./xl` with no arguments lists every command (logs, screen and sound recording, camera frames, stop, state...).

## Dual-boot support

Peridot is currently tested with temporary Fastboot booting and RAM-loaded images. The repository's generic
dual-boot design and QEMU checks are documented in [docs/INSTALL.md](docs/INSTALL.md); they do not establish that
dual boot is ready on Peridot.

## Prebuilt boot image

Build Peridot boot artifacts with `common/build.sh peridot GL=1`. Confirm the device/codename before using any
release image; a Redwood image is not suitable for Peridot. No Apple files are included in the boot image.

## Building from source

`common/build.sh <device>` runs every step that this machine can run and uses `prebuilt/` for the rest.

- **Kernel** — needs [theos](https://theos.dev/docs/installation-linux) in `~/theos` (cctools and
  `iPhoneOS16.5.sdk`) and `clang lld python perl`. Details: [xnu/README.md](xnu/README.md).
  ```sh
  xnu/apply.sh && xnu-deps/setup.sh && ./xnu-build.sh -j8 -k
  ```
- **Loader** — `xnuloader/build.sh` (clang + lld-link, an EFI application without EDK2).
- **Images** — `REST=1 userland/build.sh` (variables documented at the top of that file).
- **UEFI** — [uefi/README.md](uefi/README.md) (Project Silicium port); not needed to run, `prebuilt/uefi` has it.
- After rebuilding: `tools/update-prebuilt.sh` refreshes `prebuilt/`.

## Porting to another device

```sh
common/new-device.sh <codename> path/to/device.dtb   # profile + loader platform from the phone's device tree
```

Then follow [docs/PORTING.md](docs/PORTING.md): the UEFI port, the loader's platform description, the kernel's
drivers, the firmware. Device profiles live in `common/devices/` (`peridot.conf`, `qemu-virt.conf`, `_template.conf`).

## How it works

```
fastboot boot ──► UEFI (Mu-Silicium) ──► xnuloader (EFI application)
                                           │  stays resident: USB link (fastboot protocol),
                                           │  watchdog, receives the kernel's log
                                           ▼
            XNU (BCM2837 config as the base + our platform)
              ├─ GICv3, timer, video console, return to the loader
              ├─ APFS (system images uploaded to RAM for the current Peridot run), ramfs (/private/var, /tmp)
              ├─ minicrypto instead of corecrypto, AMFI and Sandbox stand-ins (MAC policies)
              ├─ driver replacements: IOSurface, IOMobileFramebuffer, AppleKeyStore, network stack
              ├─ input: /dev/hk-touch ◄─ Peridot touchscreen / virtio-input (QEMU)
              ├─ GPU: Adreno A735 (experimental Mesa/Freedreno path); Wi-Fi: WCN6750 (ath11k); sound: ADSP / virtio-sound
              ▼
   iOS 16.5 launchd → logd, notifyd, cfprefsd, runningboardd, backboardd, SpringBoard, apps
              ├─ our libraries are injected into backboardd, SpringBoard and mediaserverd
              │  (virtual HID services, a few ObjC method swaps — in memory only)
              └─ mediaserverd gets our CoreAudio output device (HKAudio.driver)
```

The iOS programs, frameworks and dyld shared cache are not changed by a single byte: what they miss is done by the
kernel (driver replacements) or by a library the kernel injects with `DYLD_INSERT_LIBRARIES` (`kern.exec_inject`).

## Layout

| Path | What |
|---|---|
| `common/` | `setup.sh` (workspace), `build.sh` / `run.sh` (per device), `new-device.sh`, `devices/*.conf`, `ugit` |
| `xl` | drives the phone over USB: loader, kernel runs, logs, screenshots, screen and sound recording |
| `xnuloader/` | the loader: `loader.c` (UEFI application), `mkpayload.py` (kernel + images + arguments container) |
| `xnu/` | our XNU changes: patch, new files, `apply.sh` / `export.sh` |
| `xnu-deps/` | everything for building XNU on Linux: `setup.sh`, tool wrappers, stubs |
| `xnu-build.sh`, `xnu-env.sh` | kernel build |
| `uefi/`, `mkxnuimg.py` | the UEFI port and packing the boot image for `fastboot boot` |
| `wifi/lx/` | the layer on which Linux's Wi-Fi driver builds for our kernel |
| `qemu/` | QEMU runs, touch input, screenshots, stack symbolication |
| `tools/` | recording (`recording.py`, `sound.py`), UFS/flash tools, `mk-ath11k.sh`, `new-workspace.sh`, host tests |
| `prebuilt/` | built kernel, loader and UEFI pieces (`NOTICES.md` for the third-party code in the kernel) |
| `docs/` | screenshots, [checks.md](docs/checks.md), [PORTING.md](docs/PORTING.md) |

## Debugging

- Session log: QEMU's stdout or `logs/HHMMSS-run.log` from the phone.
- `RC_JOBS=rc.jobs.tour REST=1 userland/build.sh` — an image that unlocks and opens every app of the first page.
- `userabort:` in the log is the stack of a process that called `abort()`; `usertrap:` (with `systrace=1`) — crashes.
- Live stacks (QEMU): `qemu/touch.py stacks NAME`, then `qemu/symstack.py LOG NAME`
  (needs [`ipsw`](https://github.com/blacktop/ipsw)).
- How a check can lie to you: [docs/checks.md](docs/checks.md).

## Screenshots

The screenshots below are inherited project examples and are not Peridot validation evidence.

<p>
<img src="docs/homescreen-2.jpg" width="160" alt="Home screen, page 2">
<img src="docs/search.jpg" width="160" alt="Search">
<img src="docs/settings-general.jpg" width="160" alt="Settings > General">
<img src="docs/calendar.jpg" width="160" alt="Calendar">
<img src="docs/clock.jpg" width="160" alt="Clock">
</p>
<p>
<img src="docs/wallet.jpg" width="160" alt="Wallet">
<img src="docs/messages.jpg" width="160" alt="Messages">
<img src="docs/music.jpg" width="160" alt="Music">
<img src="docs/maps.jpg" width="160" alt="Maps">
<img src="docs/appstore-tabs.png" width="160" alt="App Store">
</p>

## Licenses

- No iOS files are in the repositories: `ios165/fetch.sh` downloads them from Apple's CDN to your machine.
- `xnu/` is a derivative work of XNU and is distributed under APSL 2.0, like XNU itself; `prebuilt/mach.*` is the
  kernel built from it. It also contains ath11k (BSD-3-Clause-Clear) and FreeBSD LinuxKPI code (BSD-2-Clause):
  see [prebuilt/NOTICES.md](prebuilt/NOTICES.md).
- `xnu/files/bsd/miscfs/apfs/libzbitmap/` and `xnu-deps/build/lzbitmap/` — libzbitmap (Corellium LLC, MIT here);
  `xnu/files/bsd/miscfs/apfs/libdeflate/` — libdeflate 1.24 (Eric Biggers, MIT, `COPYING` there).
- ath11k, Linux and FreeBSD sources are not in the repository: `tools/mk-ath11k.sh` fetches pinned releases.
  `wifi/lx/patches/*.patch` are our changes to FreeBSD headers; the rest of `wifi/lx/` is ours.
- `uefi/files/Binaries/` and `prebuilt/uefi/SILICIUM_UEFI.fd` contain UEFI drivers from device firmware, distributed
  the way Project Silicium distributes them for supported devices;
  `prebuilt/uefi/mkbootimg.py` — AOSP, Apache-2.0.
- This repository is licensed under the [Apple Public Source License 2.0](LICENSE): our changes to XNU are a derivative of XNU and must be, and our other code here (loader, scripts) is released under the same license. Third-party code keeps its own license, as listed above.

## Team

OOSEX GANG — [@oodze](https://t.me/oodze) (founder), [@netusd](https://t.me/netusd), [@wannqn](https://t.me/wannqn),
[@hxfuxyyXD](https://t.me/hxfuxyyXD). News: [t.me/oosexgangdevs](https://t.me/oosexgangdevs).
