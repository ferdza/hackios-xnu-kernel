#!/bin/bash
# Apply the HakiOS XnuLoader layer to the Mu-Silicium tree so the fajita UEFI starts XNU instead of
# showing a boot menu. This layer is SoC/loader-level and identical to the POCO's -- it touches only
# shared SiliciumPkg/BootShim files (BootShim.S copies the "XNUP" payload that sits behind the FD to
# PAYLOAD_BASE; BootManagerLib boots the XNU loader from it), with no device-specific addresses --
# so fajita reuses the POCO's patch and XnuLoader.[ch] verbatim (verified: the patch applies cleanly
# to current Mu-Silicium). Run make-fajita-pkg.sh first.
#
# usage: devices/oneplus-6t/uefi/add-xnuloader.sh [--revert]
set -e
cd "$(dirname "$(readlink -f "$0")")"                   # devices/oneplus-6t/uefi
MU=../../../Mu-Silicium
POCO=../../poco-x5-pro/uefi                              # the single source of the shared layer
PATCH=$POCO/mu-silicium.patch
XNU_SRC=$POCO/files/Silicon/Silicium/SiliciumPkg/Library/BootManagerLib
XNU_DST=$MU/Silicon/Silicium/SiliciumPkg/Library/BootManagerLib

[ -d "$MU" ] || { echo "add-xnuloader.sh: no $MU -- run make-fajita-pkg.sh first" >&2; exit 1; }
[ -f "$PATCH" ] || { echo "add-xnuloader.sh: no $PATCH" >&2; exit 1; }

if [ "$1" = "--revert" ]; then
	git -C "$MU" apply -R --whitespace=nowarn "$PWD/$PATCH" && rm -f "$XNU_DST/XnuLoader.c" "$XNU_DST/XnuLoader.h"
	echo "add-xnuloader.sh: reverted"
	exit 0
fi

# already applied? (BootManager.c carries the XNU hook once patched)
if grep -q "XnuLoader" "$MU/Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/BootManager.c" 2>/dev/null; then
	echo "add-xnuloader.sh: already applied"
	exit 0
fi

git -C "$MU" apply --check --whitespace=nowarn "$PWD/$PATCH" || {
	echo "add-xnuloader.sh: the shared patch no longer applies to this Mu-Silicium; regenerate it" >&2
	exit 1
}
git -C "$MU" apply --whitespace=nowarn "$PWD/$PATCH"
cp "$XNU_SRC/XnuLoader.c" "$XNU_SRC/XnuLoader.h" "$XNU_DST/"
echo "add-xnuloader.sh: applied the XnuLoader layer (BootShim + BootManagerLib + XnuLoader.[ch])"
echo "add-xnuloader.sh: now build -- cd $MU && python3 build_uefi.py -d fajita -r RELEASE"
