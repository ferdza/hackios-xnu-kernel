# UEFI for the OnePlus 6T (fajita)

HakiOS boots XNU over a per-phone UEFI, the role the POCO's Mu-Silicium port plays in
`../../poco-x5-pro/uefi/`. The 6T needs its own. Mu-Silicium has no fajita target upstream, **but it
has `enchiladaPkg` (the OnePlus 6)** — the same SoC (SDM845, Qualcomm "Napali", `NapaliPkg`) and
near-identical board. So the fajita port is the OnePlus 6 package with the 6T's panel size, board id
and names changed. `make-fajita-pkg.sh` here does exactly that derivation (this is the same approach
UniXNU-fajita takes).

## Step 1 — generate `fajitaPkg` (done by the script, verified)

```sh
devices/oneplus-6t/uefi/make-fajita-pkg.sh
```

It clones Mu-Silicium to `Mu-Silicium/` at the repo root (shared with the POCO port; it is
git-ignored), copies `Platforms/OnePlus/enchiladaPkg` → `fajitaPkg`, and applies the 6T's values.
What it changes, from the real device tree (`../HARDWARE.md`):

| In `fajita.dsc` | enchilada (OnePlus 6) | fajita (6T) |
|---|---|---|
| `PLATFORM_NAME` / GUID | enchilada / `F8A6C376-…` | fajita / `F5859963-A198-40FE-BA5E-20C2B1B07AC7` |
| `PcdFrameBufferHeight` | 2280 (`sofef00`) | **2340** (`s6e3fc2x01`) |
| `PcdFrameBufferWidth` | 1080 | 1080 (unchanged) |
| `PcdSmbiosSystemModel` | `"6"` | `"6T"` |
| `PcdSmbiosSystemRetailModel` | `"enchilada"` | `"fajita"` |
| `PcdSmbiosSystemRetailSku` | `"A6003"` | `"A6013"` |
| `PcdSmbiosSystemBoardModel` | `"17819"` | **`"18801"`** (matches `androidboot.project_name`) |

Unchanged because the 6 and 6T share them (confirmed in the OnePlus 6 memory map): the display/splash
region `0x9D400000`, the FD region `0x9FC00000`, `NapaliPkg` as the SoC package. `DeviceBuild.py`
and the `.dec`/`.fdf` are renamed and re-scoped to `fajita` automatically. The 6T's own device tree
(`../prebuilt/device-tree.dtb` if present, or pass one as an argument) is shipped as the UEFI DTB.

## Step 2 — build it

Packages as in Mu-Silicium's `setup_env.sh` (Arch: `git base-devel lld nasm clang llvm
aarch64-linux-gnu-gcc python python-pip python-virtualenv`).

```sh
cd Mu-Silicium
python3 -m venv venv && venv/bin/pip install -r pip-requirements.txt
source venv/bin/activate
python3 build_uefi.py -d fajita -r RELEASE    # -> Build/fajitaPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd
```

(The POCO README's `mu_nasm`/Mono note applies here too if the build complains about nasm.)

## Step 3 — the HakiOS XnuLoader layer

The base `fajitaPkg` boots UEFI; it does not yet start XNU. That is the job of the HakiOS layer:
- `BootShim/BootShim.S`: copies the "XNUP" payload that sits behind the FD to `PAYLOAD_BASE`.
- `Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/{BootManager.c,BootManagerLib.inf}` +
  `XnuLoader.[ch]`: starts the XNU loader from that payload instead of showing a boot menu.

Every one of these is a **shared SiliciumPkg/BootShim file with no device-specific address** (it uses
the `PAYLOAD_BASE` symbol and the `XNUP` magic, all memory-map-relative), so fajita reuses the POCO's
layer verbatim — verified: the POCO's `mu-silicium.patch` applies cleanly to current Mu-Silicium.

```sh
devices/oneplus-6t/uefi/add-xnuloader.sh      # applies ../../poco-x5-pro/uefi/{mu-silicium.patch,XnuLoader.[ch]}
```

The script is idempotent and reverts with `--revert`. It reuses the POCO's files as the single source
of this shared layer rather than duplicating the 600-line patch here. After it, build Step 2's command
produces an XNU-booting UEFI.

## Step 4 — the `fastboot boot` image (handled for you by `DEVICE=oneplus-6t`)

The top-level `mkxnuimg.py` wraps `SILICIUM_UEFI.fd` + the BootShim + the dtb into the Android boot
image that `xl`/`make phone` upload with `fastboot boot`. It is device-aware: with `DEVICE=oneplus-6t`
it reads `uefi.conf` here for the package stem (`fajita`) and the **FD base/size** the BootShim must
copy the firmware volume to. Those are the 6T's, taken from Mu-Silicium's `fajita.toml` `[uefi_fd]`
(`base = 0x9FC00000`, `size = 0x300000`) — and they differ from the POCO's (`0x9FD00000` / `0x200000`)
on **both** base and size, so booting the 6T with the POCO's defaults would copy the FD to the wrong
address at the wrong length and die. Nothing to run by hand; just keep `DEVICE=oneplus-6t` on the
`make` line.

## ABL `fastboot boot` quirks (both the base and the HakiOS build)

Per UniXNU-fajita, the 6T's stock ABL:
- **refuses `fastboot boot`** if an image sits on the active slot's boot partition, or if the active
  slot's retry count is 0 → `fastboot --set-active=b` before each boot;
- **rejects large ramdisks** → HakiOS uploads its ramdisk through `xl ramdisk` rather than baking it
  into the boot image, which should sidestep this; verify on first bring-up.

Nothing is flashed: the loader is RAM-booted with `xl`, and a reboot returns the phone to Android.
Charging does not run while UEFI/XNU is up — keep the battery topped up during bring-up.
