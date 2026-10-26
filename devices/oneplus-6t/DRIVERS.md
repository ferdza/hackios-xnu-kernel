# OnePlus 6T (fajita) — kernel driver port map

How each HakiOS kernel driver fares on fajita, grounded in the real device tree (`HARDWARE.md`) and
in reading the POCO sources. **Every peripheral driver in HakiOS is bare-metal**: it brings up its
own GENI serial engine, GCC clocks, TLMM pinmux and RPMh power rails with per-SoC addresses — there
is no generic bus/clock/regulator framework to lean on. So "porting a driver" means re-pointing all
of that at fajita's numbers and then debugging it **on the phone** (serial/loader log), which cannot
be done off-device.

Legend: ✅ works as-is · 🟡 needs re-pointing · 🔴 new driver / structural · ⬛ much later

| Driver (file) | POCO hardware | fajita hardware | Status | What the port needs |
|---|---|---|---|---|
| Framebuffer (`MiniIOMobileFramebuffer.cpp`) | loader's fb | loader's fb | ✅ | Device-agnostic — consumes the framebuffer UEFI leaves. Display bring-up is the **UEFI's** job (panel `s6e3fc2x01`, splash `0x9d400000`), not a kernel driver. |
| UFS host (`hakios.ufshc.c`) | `ufshc@1d84000` | `ufshc@1d84000` | 🟡→✅ | **Register base already matches** (`UFSHC_PHYS 0x1d84000`). Qualcomm shares it SM7325↔SDM845. Link-startup/PHY/clock tuning may still differ, but no rewrite — this is the storage wall other ports hit, and HakiOS starts it half-solved. |
| UFS stack (`hakios.ufs.c`) | — | — | ✅ | SoC-agnostic (block layer / APFS plumbing above the host). |
| Touch (`hakios.goodix.c` → new `hakios.synaptics.c`) | Goodix, GENI **SPI** @`0xa94000` | **Synaptics S3320**, GENI **I²C** @`0xa90000`, addr `0x20`, IRQ GPIO125, reset GPIO99 | 🔴 | New driver. HakiOS has no GENI-**I²C** code (goodix is GENI-SPI), so: GENI-I²C transport + GCC vote for the `a90000` SE + TLMM i2c pinmux + RPMh rail votes + RMI4 (PDT scan, F01, F11/F12 2D). Wire into `hakios_input_init()`. |
| GPU (`hakios.adreno.c`, `hakios.gpu.cpp`) | Adreno 642L (`a660_*` fw) | Adreno 630 (`a630_*` fw) | 🔴 | Different GPU generation. Firmware names, GMU sequences and register deltas differ. Big; Metal is far off regardless. |
| Wi-Fi (`hakios.wifi.c`, `hakios.wlfw.c`, `wifi/lx/`) | WCN6750 (`wpss`) | WCN3990 (`wlanmdsp` + ath10k board data) | 🔴 | Different radio subsystem (remoteproc image + board-data model differs from the POCO's wpss). |
| Modem (`hakios.mpss.c`, `hakios.qmi.c`, `hakios.cell.c`) | SM7325, IPA v4.11, **has IPCC** | SDM845, IPA **v3.5.1**, **no IPCC** | 🔴 | Largest piece. No `qcom,ipcc` on SDM845 (different signalling), and the IPA SRAM layout / filter-table / Qtime values are hardcoded for v4.11. Structural. |
| IPA (`hakios.ipa.c`) | IPA v4.11 | IPA v3.5.1 | 🔴 | See modem — version-specific internals must be redone. |
| Audio/ADSP (`hakios.adsp.c`, `hakios.sound.c`, `hakios.audio.c`, `hakios.speaker.c`, `hakios.mic.c`) | SM7325 ADSP | SDM845 ADSP (`adsp.*`) | ⬛ | Re-point ADSP load addresses/rails; after storage+touch+UI. |
| Battery (`hakios.battery.c/.cpp`) | via ADSP | via ADSP (SDM845) | ⬛ | Depends on ADSP. UEFI also doesn't charge during a session — keep the phone topped up. |
| Haptics (`hakios.haptic.c`) | Awinic part | plain vibration motor (no fw) | ⬛ | Different/absent part; low priority. |
| Buttons (`hakios.buttons.c`) | PMIC GPIO | PMIC GPIO (SDM845 PM8998) | 🟡 | Re-point PMIC/PON addresses. |
| Camera (`hakios.camera.c`) | SM7325 ISP | SDM845 ISP | ⬛ | Very late. |

## Suggested order (matches the POCO bring-up)
1. **UEFI (fajita)** — gates everything; gives the framebuffer and brings up UFS PHY/clocks. *Must be built and tested on the device.*
2. **UFS** — confirm the base-matched host driver links on fajita → APFS root mounts (the big milestone).
3. **Touch** (`hakios.synaptics.c`) — first interactive input.
4. Wi-Fi → GPU/Metal → modem → audio/camera, as endurance allows.

## Why the rest can't be finished from a cloud box
Steps 1–3 each need the physical 6T in the loop: build → `fastboot boot` → read the loader/serial
log → fix addresses/sequences → repeat. Parameters and draft code can be prepared here (this file
and `HARDWARE.md` are that); a *booting* port cannot be produced without the device.
