#!/bin/bash
# Build libhakios-metal.a and hakios-metal-probe for iOS 16.5.
# usage: metal/build.sh
#   SDK=...   the iPhoneOS16.5.sdk to use (default: $THEOS or xnu-deps/theos-mac)
set -e
cd "$(dirname "$(readlink -f "$0")")/.."
source ./xnu-env.sh
SDK=${SDK:-}
if [ -z "$SDK" ]; then
	for d in "${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk" \
	    "$PWD/xnu-deps/theos-mac/sdks/iPhoneOS16.5.sdk" \
	    "$HOME/theos/sdks/iPhoneOS16.5.sdk"; do
		if [ -d "$d" ]; then
			SDK=$d
			break
		fi
	done
fi
if [ ! -d "$SDK" ]; then
	echo "metal/build.sh: no iPhoneOS16.5.sdk; set SDK= or THEOS" >&2
	exit 1
fi
make -C metal ios SDK="$SDK" THEOS="$THEOS" HAKIOS="$PWD"
echo "metal/build.sh: metal/out/ios/hakios-metal-probe and libhakios-metal.a"
