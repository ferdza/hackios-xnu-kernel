#!/bin/bash
# Pull the OnePlus 6T's own firmware out of its Android (OxygenOS) into the fw-*/ directories at the
# top of the repository (never committed: it is the vendor's). The phone is on adb, in Android.
# Paths are the SDM845/fajita equivalents of the POCO's; OxygenOS builds vary, so missing files are
# reported, not fatal -- adjust the paths for your build. SDM845 GPU is Adreno 630 (a630_*), Wi-Fi
# is WCN3990 (wlanmdsp + board data, not the POCO's wpss).
set -e
cd "$(dirname "$0")/../.."
pull() { # DIR SOURCE...  (missing files are told, not fatal)
	local dir=$1; shift
	mkdir -p "$dir"
	for f in "$@"; do adb pull "$f" "$dir/" >/dev/null 2>&1 || echo "pull-firmware.sh: $f: not on the phone"; done
	echo "pull-firmware.sh: $dir: $(ls -1 "$dir" | wc -l | tr -d ' ') files"
}
adb get-state >/dev/null
# GPU: Adreno 630. Zap shader is often a630_zap.mbn/.mdt on OnePlus; grab both forms.
pull fw-gpu /vendor/firmware/a630_gmu.bin /vendor/firmware/a630_sqe.fw \
            /vendor/firmware/a630_zap.mdt /vendor/firmware/a630_zap.mbn \
            $(for i in b00 b01 b02; do echo /vendor/firmware/a630_zap.$i; done)
# Wi-Fi: WCN3990. Firmware comes up via remoteproc (wlanmdsp) plus ath10k board data.
pull fw-wifi /vendor/firmware/wlanmdsp.mbn \
             /vendor/firmware/bdwlan.elf /vendor/firmware/Data.msc \
             $(adb shell 'ls /vendor/firmware/bdwlan* 2>/dev/null' | tr -d '\r')
# ADSP (audio/sensors; battery/charging reporting rides here on SDM845 too).
pull fw-audio /vendor/firmware_mnt/image/adsp.mdt $(for i in $(seq -w 1 36); do echo /vendor/firmware_mnt/image/adsp.b$i; done)
# Modem (MPSS) + IPA firmware. IPA on SDM845 is v3.5.1 -- see README before relying on cellular.
pull fw-modem /vendor/firmware_mnt/image/modem.mdt $(for i in $(seq -w 0 30); do echo /vendor/firmware_mnt/image/modem.b$i; done)
pull fw-modem /vendor/firmware_mnt/image/ipa_fws.mdt $(for i in $(seq -w 0 3); do echo /vendor/firmware_mnt/image/ipa_fws.b$i; done)
mkdir -p fw-modem/image && adb pull /vendor/firmware_mnt/image fw-modem/ >/dev/null 2>&1 && echo "pull-firmware.sh: fw-modem/image: the modem's own files (TFTP)"
echo "pull-firmware.sh: done. Confirm GPU/Wi-Fi paths against your OxygenOS build; WCN3990 also wants"
echo "pull-firmware.sh: fw-wifi/mac (the radio's address) and fw-wifi/network (SSID + passphrase)."
