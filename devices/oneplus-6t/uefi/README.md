# UEFI for the OnePlus 6T (fajita)

HakiOS boots XNU over a per-phone UEFI, the same role the POCO's Mu-Silicium port plays in
`../../poco-x5-pro/uefi/`. The 6T needs its own. There is no upstream Mu-Silicium fajita target, so
there are two realistic paths:

## Path A — Mu-Silicium, derived from the OnePlus 6 (enchilada)

The 6 and 6T are near-identical SDM845 boards sharing `sdm845-oneplus-common.dtsi`. Start from the
`enchilada` Mu-Silicium device and change only what the 6T differs in:

- **Panel**: `samsung,sofef00` (1080×2280) → **`samsung,s6e3fc2x01`** (1080×2340).
- **ABL board id**: `<8 0 17819 22>` → **`<8 0 18801 41>`**.
- **Splash / framebuffer region**: confirm the 6T splash base (UniXNU uses `0x9d400000`) and the
  FD base/size (`0x9fc00000` / `0x300000`, unchanged from the 6).

This mirrors what UniXNU-fajita's `scripts/make-fajita-uefi.py` does (it derives a fajita UEFI from
the enchilada port). Keep this directory's layout parallel to the POCO's: an `apply.sh` that clones
Mu-Silicium and applies a `mu-silicium.patch`, an `export.sh` that saves it back, and the built
`SILICIUM_UEFI.fd` + `BootShim.bin` landing in `../prebuilt/`.

## Path B — edk2-sdm845 (Renegade Project)

The mature community UEFI for SDM845 phones (including the 6T) is **edk2-sdm845**. It already boots
on fajita and exposes standard UEFI (GOP framebuffer, block IO). If the HakiOS loader only needs a
conformant UEFI + a device tree handoff, this is the lower-risk way to first light. The cost is that
it is a different edk2 base than the rest of HakiOS's device tooling assumes, so the prebuilt/boot
plumbing (`mkbootimg.py`, the BootShim) may need adapting.

## ABL `fastboot boot` quirks (both paths)

The 6T's stock ABL is fussy, per UniXNU-fajita:

- It **refuses `fastboot boot`** if an image already sits on the active slot's boot partition, or if
  the active slot's retry count has hit 0. Workaround: `fastboot --set-active=b` (or `set_active b`)
  before each boot.
- It **rejects large ramdisks**. UniXNU ships an empty ramdisk in the boot image and puts the real
  payload on a data volume (`system_a`). HakiOS uploads its ramdisk through `xl ramdisk` rather than
  baking it into the boot image, which should sidestep this — verify on first bring-up.

Nothing here is flashed in the HakiOS flow: the loader is RAM-booted with `xl`, and a reboot returns
the phone to Android. Charging does **not** run while UEFI/XNU is up (inherited OnePlus-6 charger
config is unverified on the 6T) — keep the battery topped up during bring-up sessions.

## Files to add here (todo)

- `apply.sh`, `export.sh`, `mu-silicium.patch` (Path A), or a thin `fetch-edk2.sh` (Path B)
- built artifacts exported to `../prebuilt/`: `SILICIUM_UEFI.fd`, `BootShim.bin`, `device-tree.dtb`
  (the 6T's own, from `/sys/firmware/fdt` or mainline `sdm845-oneplus-fajita.dts`), `ramdisk`,
  `mkbootimg.py`
