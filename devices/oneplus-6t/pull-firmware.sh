#!/bin/bash
# Pull the OnePlus 6T's own firmware out of its Android into the fw-*/ directories at the top of the
# repository (never committed: it is the vendor's). The phone is on adb, in Android. Paths below were
# confirmed against a fajita LineageOS /vendor listing (see HARDWARE.md); other builds may differ, so
# missing files are reported, not fatal. SDM845: GPU Adreno 630 (a630_*), Wi-Fi WCN3990 (wlanmdsp +
# bdwlan board data), modem MPSS + MBA.
set -e
cd "$(dirname "$0")/../.."
pull() { # DIR SOURCE...  (missing files are told, not fatal)
	local dir=$1; shift
	mkdir -p "$dir"
	for f in "$@"; do adb pull "$f" "$dir/" >/dev/null 2>&1 || echo "pull-firmware.sh: $f: not on the phone"; done
	echo "pull-firmware.sh: $dir: $(ls -1 "$dir" | wc -l | tr -d ' ') files"
}
adb get-state >/dev/null
FW=/vendor/firmware                    # a630_*, ipa_fws.*, wlan/
IMG=/vendor/firmware_mnt/image         # adsp.*, modem.*, mba.mbn, wlanmdsp.mbn, bdwlan.*
# GPU: Adreno 630 (zap is .mdt + .b00/.b01/.b02).
pull fw-gpu $FW/a630_gmu.bin $FW/a630_sqe.fw $FW/a630_zap.mdt $FW/a630_zap.b00 $FW/a630_zap.b01 $FW/a630_zap.b02
# IPA firmware (lives in /vendor/firmware on fajita, not in firmware_mnt).
pull fw-ipa $FW/ipa_fws.mdt $(for i in b00 b01 b02 b03 b04; do echo $FW/ipa_fws.$i; done)
# Wi-Fi: WCN3990 -- remoteproc image + ath10k board data.
pull fw-wifi $IMG/wlanmdsp.mbn $IMG/data.msc $(adb shell 'ls '"$IMG"'/bdwlan* 2>/dev/null' | tr -d '\r')
mkdir -p fw-wifi/wlan && adb pull $FW/wlan fw-wifi/ >/dev/null 2>&1 && echo "pull-firmware.sh: fw-wifi/wlan: board files"
# ADSP (audio/sensors; battery/charging reporting rides here on SDM845 too).
pull fw-audio $IMG/adsp.mdt $IMG/adspr.jsn $IMG/adspua.jsn $(for i in $(seq -w 0 13); do echo $IMG/adsp.b$i; done)
# Modem (MPSS) + MBA boot image + the QDSP blob.
pull fw-modem $IMG/modem.mdt $IMG/mba.mbn $IMG/qdsp6m.qdb $IMG/modemr.jsn $IMG/modemuw.jsn $(for i in $(seq -w 0 28); do echo $IMG/modem.b$i; done)
mkdir -p fw-modem/image && adb pull $IMG fw-modem/ >/dev/null 2>&1 && echo "pull-firmware.sh: fw-modem/image: the modem's own files (TFTP)"
echo "pull-firmware.sh: done. WCN3990 Wi-Fi also wants fw-wifi/mac (the radio's address) and"
echo "pull-firmware.sh: fw-wifi/network (SSID + passphrase) -- add by hand."
