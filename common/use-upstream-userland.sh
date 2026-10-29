#!/bin/bash
# Put the upstream combined userland into this tree and apply the Peridot Bluetooth patch.
# The old hackios-xnu-userland checkout is no longer maintained; upstream/main carries userland/.
# usage: common/use-upstream-userland.sh
#   FORCE=1 replaces an existing userland/ directory.
set -e
cd "$(dirname "$(readlink -f "$0")")/.."

P=common/patches/userland-peridot-bluetooth.patch
if ! git rev-parse --verify upstream/main >/dev/null 2>&1; then
	echo "use-upstream-userland: no upstream/main; run git fetch upstream first" >&2
	exit 1
fi
if [ -e userland ] && [ "${FORCE-0}" != 1 ]; then
	if git apply --reverse --check "$P" >/dev/null 2>&1; then
		echo "use-upstream-userland: userland/ is already the patched upstream tree"
	elif git apply --check "$P" >/dev/null 2>&1; then
		git apply -p1 "$P"
		echo "use-upstream-userland: applied the Bluetooth patch to the existing userland/"
	else
		echo "use-upstream-userland: userland/ exists but is not the upstream tree; FORCE=1 replaces it" >&2
		exit 1
	fi
else
	rm -rf userland
	git archive upstream/main userland | tar -x
	git apply -p1 "$P"
	echo "use-upstream-userland: userland/ from upstream/main $(git rev-parse --short upstream/main), Bluetooth patch applied"
fi
echo "next: common/build.sh peridot BLUETOOTH=1; boot XNU with bt=1"
