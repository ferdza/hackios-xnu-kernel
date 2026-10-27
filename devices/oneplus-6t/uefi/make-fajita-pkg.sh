#!/bin/bash
# Derive a fajitaPkg (OnePlus 6T) from enchiladaPkg (OnePlus 6) inside the Mu-Silicium tree. The two
# are the same SoC (SDM845, "Napali"), so the port is the OnePlus 6 package with the 6T's panel size,
# board id and names changed. This is the base UEFI port; the HakiOS XnuLoader/BootShim layer (the
# "XNUP" container and the loader that starts XNU) is applied on top afterwards -- see README.md.
#
# Run it from anywhere in the repo. It clones Mu-Silicium next to the other device packages (the same
# ../../../Mu-Silicium that devices/poco-x5-pro/uefi/apply.sh uses) if it is not there yet, then adds
# fajitaPkg to it. Idempotent only with --force (which removes an existing fajitaPkg first).
#
# usage: devices/oneplus-6t/uefi/make-fajita-pkg.sh [--force] [FAJITA.dtb]
#   FAJITA.dtb  the 6T's own device tree to ship as the UEFI's DTB (default: enchilada's, renamed).
#               devices/oneplus-6t/prebuilt/device-tree.dtb is used automatically if it exists.
set -e
cd "$(dirname "$(readlink -f "$0")")"                   # devices/oneplus-6t/uefi
HERE=$PWD
MU=../../../Mu-Silicium
FAJITA_GUID=F5859963-A198-40FE-BA5E-20C2B1B07AC7         # generated once for this port (enchilada is F8A6C376-...)

force=0; dtb=
for a in "$@"; do
	case "$a" in
	--force) force=1 ;;
	*) dtb=$a ;;
	esac
done
[ -z "$dtb" ] && [ -f "$HERE/../prebuilt/device-tree.dtb" ] && dtb=$HERE/../prebuilt/device-tree.dtb

# 1. the Mu-Silicium tree (shared with the POCO port)
if [ ! -d "$MU" ]; then
	echo "make-fajita-pkg.sh: cloning Mu-Silicium into $MU"
	git clone https://github.com/Project-Silicium/Mu-Silicium.git "$MU"
	git -C "$MU" submodule update --init --recursive
fi
E=$MU/Platforms/OnePlus/enchiladaPkg
F=$MU/Platforms/OnePlus/fajitaPkg
[ -d "$E" ] || { echo "make-fajita-pkg.sh: no enchiladaPkg in $MU (update Mu-Silicium?)" >&2; exit 1; }
if [ -d "$F" ]; then
	[ "$force" = 1 ] || { echo "make-fajita-pkg.sh: $F already exists (use --force to redo)" >&2; exit 1; }
	rm -rf "$F"
fi

# 2. copy the package and rename enchilada.* -> fajita.*
cp -r "$E" "$F"
for f in "$F"/enchilada.*; do mv "$f" "${f/enchilada./fajita.}"; done

# 3. rewrite every in-tree reference enchilada -> fajita (package name, file stems, scopes, names),
#    then set the new platform GUID (which the blanket replace must not touch).
grep -rl enchilada "$F" | while read -r f; do sed -i 's/enchilada/fajita/g' "$f"; done
sed -i "s/F8A6C376-C5D3-4F61-B911-CE778BCB5DEB/$FAJITA_GUID/" "$F/fajita.dsc"

# 4. the 6T's own values in fajita.dsc (panel s6e3fc2x01 is 1080x2340; board id 18801; SKU A6013).
#    (RetailModel "enchilada" already became "fajita" in step 3.) Edit only the matching PCD line.
D=$F/fajita.dsc
sed -i '/PcdFrameBufferHeight/   s/2280/2340/'     "$D"
sed -i '/PcdSmbiosSystemModel/   s/"6"/"6T"/'      "$D"
sed -i '/PcdSmbiosSystemRetailSku/ s/"A6003"/"A6013"/' "$D"
sed -i '/PcdSmbiosSystemBoardModel/ s/"17819"/"18801"/' "$D"

# 5. Resources: config, device tree, source dts (renamed; swap in the real 6T dtb if we have one)
for ext in toml dtb dts; do
	[ -f "$MU/Resources/Configs/enchilada.$ext" ] && cp "$MU/Resources/Configs/enchilada.$ext" "$MU/Resources/Configs/fajita.$ext" 2>/dev/null || true
	[ -f "$MU/Resources/DTBs/enchilada.$ext" ]    && cp "$MU/Resources/DTBs/enchilada.$ext"    "$MU/Resources/DTBs/fajita.$ext"    2>/dev/null || true
done
if [ -n "$dtb" ] && [ -f "$dtb" ]; then
	cp "$dtb" "$MU/Resources/DTBs/fajita.dtb"
	echo "make-fajita-pkg.sh: shipped the 6T's own device tree ($dtb) as the UEFI DTB"
fi

# 6. ACPI tables. They live in the Silicium-ACPI submodule, not in the package, and fajita.dsc/.fdf
#    reference them by device-relative path ("fajita/AcpiTables.inf" after step 3's rename). That path
#    resolves against Silicium-ACPI/Platforms/OnePlus, so a fajita/ dir MUST exist there or PreBuild's
#    OverrideValidation dies ("Failed to convert Edk2Relative Path ... fajita/AcpiTables.inf"). The 6
#    and 6T share the Napali SoC tables, so derive fajita's from enchilada's (the .inf carries only a
#    GUID and shared Napali/Common AML paths -- no device string -- so a copy is exact).
ACPI=$MU/Silicium-ACPI/Platforms/OnePlus
if [ -d "$ACPI/enchilada" ]; then
	[ "$force" = 1 ] && rm -rf "$ACPI/fajita"
	if [ ! -d "$ACPI/fajita" ]; then
		cp -r "$ACPI/enchilada" "$ACPI/fajita"
		grep -rl enchilada "$ACPI/fajita" 2>/dev/null | while read -r f; do sed -i 's/enchilada/fajita/g' "$f"; done
		echo "make-fajita-pkg.sh: derived fajita ACPI tables from enchilada ($ACPI/fajita)"
	fi
else
	echo "make-fajita-pkg.sh: WARNING: no $ACPI/enchilada (Silicium-ACPI submodule not initialised?)" >&2
	echo "make-fajita-pkg.sh:   run: git -C $MU submodule update --init --recursive" >&2
fi

echo "make-fajita-pkg.sh: wrote $F"
echo "make-fajita-pkg.sh: review fajita.dsc, then build:"
echo "    cd $MU && python3 build_uefi.py -d fajita -r RELEASE"
echo "make-fajita-pkg.sh: then apply the HakiOS XnuLoader layer -- see devices/oneplus-6t/uefi/README.md"
