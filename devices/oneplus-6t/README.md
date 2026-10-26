# Device: OnePlus 6T (fajita)  — PORT IN PROGRESS

Everything in HakiOS that belongs to the OnePlus 6T goes here, the same way `devices/poco-x5-pro/`
holds the first phone. The kernel, loader and user space know nothing about the phone: addresses,
interrupts, power rails and firmware come from its device tree and its Android. This directory is
that map for the 6T.

> **Status: device characterised; no boot yet.** The real device tree and firmware listing have been
> pulled from a fajita 6T (LineageOS) — `soc.args` and `HARDWARE.md` carry the verified numbers
> (panel, touch, UFS, display, SoC addresses). Still to do: the fajita UEFI build and rewriting the
> per-phone drivers (display → UFS → touch first). Build with `make DEVICE=oneplus-6t ...`.

| What | Where / value |
|---|---|
| Model | OnePlus 6T, codename **fajita** (the OnePlus 6 is **enchilada**; they share a lot) |
| SoC | Qualcomm **Snapdragon 845 (SDM845)**, `sdm845` in the device tree |
| Display panel | **`samsung,s6e3fc2x01`**, 1080×2340 (the OnePlus 6 is `samsung,sofef00`, 1080×2280) |
| GPU | **Adreno 630** (`a630_*` firmware), vs the POCO's Adreno 642L (`a660_*`) |
| Wi-Fi / BT | **WCN3990** (integrated, `ath10k`/remoteproc `wlanmdsp`), vs the POCO's WCN6750 (`wpss`) |
| Touch | in-display; see `tools`/`NOTES-touch` once pulled — **not** the POCO's Goodix |
| Audio DSP | SDM845 ADSP (`adsp.*`) |
| Modem | SDM845 MPSS — **different IPA version from the POCO**, see "Cellular" below |
| Mainline Linux | well supported (postmarketOS, kernel 6.16): freedreno GPU, display, Wi-Fi all work there |

The 6 and 6T share `sdm845-oneplus-common.dtsi` in mainline Linux, so most of the memory map and
reserved regions are common; the panel, the ABL board-id and a few rails differ.

## Why this is a real port and not a config tweak

HakiOS runs XNU with its own loader over a per-phone **UEFI** (a Mu-Silicium port), and the kernel's
per-phone C drivers (`xnu/files/bsd/dev/hakios.*.c`, `xnu/files/iokit/Kernel/hakios.*.cpp`) carry
the first phone's register addresses, panel timings and firmware names. For the 6T every one of
those has to be re-pointed at SDM845/fajita numbers. The head of each driver file says where its
numbers come from — that is the checklist.

### Milestones (in order; each is a real sub-project)

1. **UEFI for fajita.** The POCO uses a Mu-Silicium port (`uefi/`). Mu-Silicium has no upstream
   fajita target. There are two paths: (a) derive one from the OnePlus 6 (`enchilada`) Mu-Silicium
   port, changing the panel to `s6e3fc2x01`, the ABL board-id to `<8 0 18801 41>` and the splash
   region; or (b) use the mature **edk2-sdm845** (Renegade Project) build that already runs on the
   6T. See `uefi/README.md`.
2. **Boot to the loader + framebuffer.** Get XNU to the point the POCO reaches: VM, scheduler,
   IOKit, AMFI, Sandbox up, and a framebuffer on the panel. Gating drivers: display
   (`hakios.*framebuffer*`), then the SDM845 **SoC addresses** below.
3. **Storage (UFS).** The 6T's UFS so the APFS root mounts. This is the wall other XNU-on-fajita
   efforts hit first; prioritise it.
4. **Touch**, then home screen interactive.
5. **Wi-Fi** (WCN3990 — a different radio subsystem from the POCO's; `wifi/lx/` is the shared stack).
6. **Cellular** — the largest piece, see below.
7. GPU/Metal, audio, haptics, cameras — all later.

## The SoC addresses (`soc.args`)

Same mechanism as the POCO (see the top-level README and `../poco-x5-pro/README.md`): the modem/IPA
drivers carry defaults and take `soc_*` boot-args for anything that differs. Generate the 6T's from
the phone's own device tree:

```sh
devices/oneplus-6t/pull-soc.sh      # adb pull /sys/firmware/fdt, then tools/soc-args.py -> soc.args
```

**`soc.args` is now populated from a real fajita device tree** (see `HARDWARE.md` for every value and
where it came from). `soc.args.sdm845-reference` is kept only as the earlier best-guess; the live
file supersedes it (and corrected `soc_rsc` from `0x179c0000` to the real `0x179e0000`). Note the run
scripts auto-pick `devices/*/soc.args` alphabetically, and `oneplus-6t` sorts before `poco-x5-pro`;
if you ever build for the POCO from this fork, pass `SOC_ARGS=devices/poco-x5-pro/soc.args` (or
`DEVICE=poco-x5-pro`) so you don't get the 6T's addresses.

### Known SDM845 vs POCO (SM7325) divergences — read before trusting any address

- **`soc_ipcc`**: SDM845 has **no `qcom,ipcc`** block (IPCC arrived with SM8150). The POCO's modem
  path signals through IPCC; on SDM845 the equivalent is the APCS/GLINK mailbox. `soc-args.py` will
  emit no `soc_ipcc` for fajita — the modem driver needs code work here, not just an address.
- **IPA version**: SDM845 is IPA **v3.5.1**; the POCO is **v4.11**. The porting README notes the
  SRAM layout, the SMEM filter-table item and the Qtime/resource-group values are IPA-version
  specific and hardcoded for 4.11. A different IPA version is explicitly called "a larger port."
  **Cellular on the 6T is a driver rewrite, not an address change.**
- **`soc_rsc`** (apps RPMh RSC) and **`soc_smem`** differ from the POCO; `pull-soc.sh` will produce
  the right values from the device tree.

## Firmware

```sh
devices/oneplus-6t/pull-firmware.sh      # the phone in Android, on adb
```

Pulls the GPU (`a630_*`), Wi-Fi (WCN3990 `wlanmdsp` + board data), the ADSP, and the modem. **The
exact `/vendor` paths vary by OxygenOS build** — the script warns on anything missing so you can
adjust. All of it is the vendor's and stays out of git (covered by the top-level `.gitignore`).

## Prior art for this exact device

- **UniXNU-fajita** (github.com/johnpanos/UniXNU-fajita): a different XNU-on-Android project that
  already boots an XNU kernelcache on the 6T to the root-mount point and documents the fajita UEFI
  (derived from enchilada), the ABL `fastboot boot` quirks (`fastboot set_active b` before each
  boot; ABL rejects large ramdisks so the payload goes to `system_a`), and the panel/board-id
  numbers used above. Invaluable reference for milestones 1–3; the driver code is not reusable
  as-is (different kernel plumbing) but the hardware facts are.
- postmarketOS / sdm845-mainline: the authoritative source for every SDM845 address and compatible.
