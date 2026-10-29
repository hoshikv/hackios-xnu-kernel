#!/bin/bash
# Put the upstream combined userland into this tree and apply the Peridot Bluetooth patch.
# The old hackios-xnu-userland checkout is no longer maintained; upstream/main carries userland/.
# usage: common/use-upstream-userland.sh
#   FORCE=1 replaces an existing userland/ directory.
set -e
cd "$(dirname "$(readlink -f "$0")")/.."

P=common/patches/userland-peridot-bluetooth.patch
REF=upstream/main
if ! git rev-parse --verify "$REF^{commit}" >/dev/null 2>&1; then
	if git rev-parse --verify origin/main^{commit} >/dev/null 2>&1 &&
	    git cat-file -e origin/main:userland/build.sh 2>/dev/null; then
		REF=origin/main
	else
		echo "use-upstream-userland: no upstream/main or origin/main userland; run git fetch upstream first" >&2
		exit 1
	fi
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
	git archive "$REF" userland | tar -x
	git apply -p1 "$P"
	echo "use-upstream-userland: userland/ from $REF $(git rev-parse --short "$REF"), Bluetooth patch applied"
fi
echo "next: common/build.sh peridot BLUETOOTH=1; BT=1 common/run.sh peridot"
