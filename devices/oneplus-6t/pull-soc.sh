#!/bin/bash
# Pull the OnePlus 6T's own device tree over adb and turn it into the SoC boot-args HakiOS needs
# (tools/soc-args.py). Unlike the POCO, several of the 6T's values differ from the kernel defaults,
# so this file matters here. Needs the phone in Android with adb (root not required:
# /sys/firmware/fdt is world-readable). Review the output against soc.args.sdm845-reference and the
# "divergences" note in this directory's README before renaming it to soc.args.
set -e
cd "$(dirname "$0")/../.."
PATH=$PWD/tools/macos-bin/platform-tools:$PATH
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
adb pull /sys/firmware/fdt "$tmp/fdt" >/dev/null || { echo "pull-soc.sh: cannot read /sys/firmware/fdt over adb"; exit 1; }
# Write to soc.args.generated, NOT soc.args: the run scripts auto-pick any devices/*/soc.args, so a
# 6T file must not appear until a human has confirmed it. Rename it by hand once verified.
tools/soc-args.py "$tmp/fdt" | tee "$(dirname "$0")/soc.args.generated"
echo "pull-soc.sh: wrote $(dirname "$0")/soc.args.generated"
echo "pull-soc.sh: NOTE SDM845 emits no soc_ipcc and is IPA v3.5.1 (not 4.11) -- see README 'divergences'."
echo "pull-soc.sh: review it, then 'mv soc.args.generated soc.args' to make it live."
