#!/bin/bash
# Build everything a device needs, in order, skipping what cannot be built on this machine.
#
# usage: common/build.sh <device> [userland/build.sh variables...]
#   common/build.sh qemu-virt
#   common/build.sh redwood GL=1
#
#   1. kernel   xnu/apply.sh, xnu-deps/setup.sh, xnu-build.sh   -- only with theos + iPhoneOS16.5.sdk,
#                                                                  otherwise prebuilt/ is used
#   2. loader   xnuloader/build.sh                              -- only with clang + lld-link
#   3. images   userland/build.sh (REST=1 and the profile's USERLAND_VARS)
#                                                               -- needs ios165/fetch.sh to have run
#   4. boot     mkxnuimg.py -> out/<device>-boot.img             -- phones: the UEFI image for `fastboot boot`
#
#   SKIP="kernel loader images boot"   leave steps out
#   METAL=1                            build metal/ and put its GPU probe in the image
. "$(dirname "$(readlink -f "$0")")/lib.sh"
load_device "$1"; shift
skip() { [[ " $SKIP " == *" $1 "* ]]; }
mkdir -p out

if ! skip kernel; then
	if [ -d "${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk" ] && command -v clang >/dev/null; then
		[ -d xnu-src ] || xnu/apply.sh
		[ -x xnu-deps/host/bin/ld64 ] || xnu-deps/setup.sh
		# Wi-Fi radio driver (phones with a WCN6750): fetched and built once
		[ "$WIFI_ATH11K" = 1 ] && [ ! -f xnu-deps/build/ath11k/out/ath11k.o ] && { tools/mk-ath11k.sh && tools/mk-wlfw.sh; }
		# the export check fails after the kernel is linked, on purpose (xnu/README.md): judge by the file
		touch out/.kernel-start
		./xnu-build.sh -j"$(nproc)" -k > out/kernel-build.log 2>&1 || true
		K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
		[ -f $K ] && { [ $K -nt out/.kernel-start ] || ! grep -q "error:" out/kernel-build.log; } &&
			echo "build: kernel $(stat -c %s $K) bytes" || die "kernel build failed, see out/kernel-build.log"
	else
		echo "build: no theos/iPhoneOS16.5.sdk: using prebuilt/mach.development.bcm2837"
	fi
fi

if ! skip loader; then
	if command -v clang >/dev/null && command -v lld-link >/dev/null; then
		xnuloader/build.sh && echo "build: loader xnuloader/loader.efi"
	else
		echo "build: no clang/lld-link: using prebuilt/loader.efi"
	fi
fi

if ! skip images; then
	have_userland
	[ -d ios165/rootfs ] || die "no iOS files yet: run ios165/fetch.sh first (it downloads them from Apple to this machine)"
	if [ "${METAL-0}" = 1 ]; then
		./metal/build.sh || die "the Metal probe build failed"
		EXTRA="${EXTRA:+$EXTRA }../metal/out/ios/hakios-metal-probe"
		RC_JOBS="${RC_JOBS:-../metal/rc.jobs.metalprobe}"
		export EXTRA RC_JOBS
	fi
	# The Peridot branch carries firmware packaging and Bluetooth fixes until they are upstream.
	# The old userland checkout and the combined upstream/main tree have different build.sh
	# contexts, so apply whichever patch fits this tree.
	P=
	USERLAND_PATCH_APPLIED=0
	for patch in common/patches/userland-peridot-firmware.patch \
	    common/patches/userland-peridot-bluetooth.patch; do
		if git apply --reverse --check "$patch" >/dev/null 2>&1; then
			P=$patch
			break
		fi
	done
	if [ -z "$P" ]; then
		for patch in common/patches/userland-peridot-firmware.patch \
		    common/patches/userland-peridot-bluetooth.patch; do
			if git apply --check "$patch" >/dev/null 2>&1; then
				git apply "$patch"
				P=$patch
				USERLAND_PATCH_APPLIED=1
				break
			fi
		done
		[ -n "$P" ] || die "cannot apply a Peridot userland patch"
	fi
	if [ "${BLUETOOTH-0}" = 1 ] && [ "$P" = common/patches/userland-peridot-firmware.patch ]; then
		die "BLUETOOTH=1 needs the upstream userland: run common/use-upstream-userland.sh"
	fi
	# Do not leave the separately versioned userland checkout dirty after its build, including
	# when that build exits early.
	if [ "$USERLAND_PATCH_APPLIED" = 1 ]; then
		trap 'git apply --reverse "$P"' EXIT
	fi
	env REST=1 WIFI_BDF="${WIFI_BDF:-m20gl}" $USERLAND_VARS "$@" userland/build.sh
	if [ "$USERLAND_PATCH_APPLIED" = 1 ]; then
		git apply --reverse "$P"
		trap - EXIT
	fi
fi

if ! skip boot && [ "$KIND" = phone ]; then
	# the UEFI with the loader inside, the way `xl enter` makes it: the loader then brings up its USB link
	L=xnuloader/loader.efi; [ -f $L ] || L=prebuilt/loader.efi
	python3 xnuloader/mkpayload.py -o out/usb-payload.bin loader.efi=$L --usb > /dev/null
	FD_BASE=$FD_BASE FD_SIZE=$FD_SIZE PAYLOAD_BASE=$PAYLOAD_BASE UEFI_FD=$UEFI_FD DTB=$DTB UEFI_TARGET=$UEFI_TARGET \
		python3 mkxnuimg.py -p out/usb-payload.bin -o out/$DEVICE-boot.img
	echo "build: out/$DEVICE-boot.img -- boot it with: fastboot boot out/$DEVICE-boot.img (or common/run.sh $DEVICE)"
fi
