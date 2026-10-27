# OnePlus 6T (fajita) — notes de portage / handoff IA

> **But de ce fichier :** continuité entre sessions (le contexte se fait compacter). Si tu es une IA
> qui reprend ce portage, LIS ÇA EN PREMIER, puis `BRINGUP.md` (le drapeau 🚩 = où on en est). Garde
> ce fichier à jour à chaque mur franchi. Tout est poussé sur le fork `ferdza/hackios-xnu-kernel`,
> branche `oneplus-6t-port-scaffold`.

## Contexte

Porter HakiOS (XNU + userland iOS 16.5, booté en RAM via `fastboot boot`, rien de flashé) du
**POCO X5 Pro (redwood, SM7325)** vers le **OnePlus 6T (fajita, SDM845)**. L'utilisateur (jules,
GitHub ferdza) bosse en français, débutant, veut des étapes idiot-proof. Sa machine : Debian, tél 6T
sous LineageOS 17.1 avec `adb root` (daemon en root ; **pas de binaire `su`**).

## Où on en est (session 2026-10-27)

**L'UEFI + le loader HakiOS démarrent sur le tél.** Chaîne de boot actuelle, mur par mur :

1. ✅ **Build UEFI fajita** (`SILICIUM_UEFI.fd`, 3 Mo). Dérivé d'enchilada (OnePlus 6, même SDM845).
2. ✅ **ABL saute dans l'image** via `fastboot boot` — a nécessité le **vrai dtb 6T** (voir plus bas).
3. ✅ **UEFI boote** : splash « Project Silicium » à l'écran.
4. ✅ **Loader HakiOS démarre** : « == XNULOADER v3 == » + `CurrentEL=1 MIDR=517f803c ...`.
5. ✅ **Crash display corrigé** : le loader prenait fajita pour QEMU (ramfb → fw_cfg). Fix :
   `platform_fajita` + branche display sur le framebuffer `0x9D400000` (GOP direct). Écran confirme :
   `display 1080 x 2340 at 9d400000` / `platform fajita` / `usb: started`.
6. ✅ **USB énumère** : `fastboot devices` → `xnuloader   fastboot`. Mais `getvar product` renvoyait
   vide → `xl` ne détectait pas le loader. Cause : le contrôleur USB fajita (SuperSpeed, bulk max
   packet 1024) sur-rapporte la longueur d'un short packet → `cmd` reçu avec des octets stale →
   `str_eq(var,"product")` échoue. **Fix** : zéroter `usb_rx_buf` avant chaque réception de commande
   (usb_command_loop + usb_poll_command). Les commandes à préfixe (`oem run`, `download:`) marchaient
   déjà ; seules les égalités exactes (`getvar product`, `max-download-size`) cassaient.
7. 🚩 **EN COURS** : rebuild loader, re-tester. `./xl state` doit dire `loader` → `xl ramdisk` +
   `xl run` → **premier log du kernel** (le but V1).

## ⚡ LE POINT CLÉ : comment booter et VOIR l'écran (OnePlus fait autrement)

`make DEVICE=oneplus-6t phone` lance `xl` qui fait `fastboot boot` puis **attend le loader sur USB
~20 s et abandonne en silence** (« loader did not come up ») — on ne voit rien. Sur OnePlus, pour
débugger il faut **booter à la main et regarder l'écran du tél** :

```sh
cd ~/hackios-xnu-kernel
# tél en fastboot BOOTLOADER (pas fastbootd : fastboot getvar is-userspace -> "no")
fastboot --set-active=b
DEVICE=oneplus-6t xnuloader/mkpayload.py -o usb-payload.bin loader.efi=xnuloader/loader.efi --usb
DEVICE=oneplus-6t ./mkxnuimg.py -p usb-payload.bin -o xnu-usb.img
fastboot boot xnu-usb.img          # <-- REGARDE L'ÉCRAN DU TÉL
./xl state                         # "loader" = le loader répond sur USB (gagné) ; "fastboot" = pas up
./xl log                           # si loader up : récupère son log via fb oem log
```
Si `./xl state` dit `loader`, on peut enchaîner `./xl ramdisk userland/system.apfs` puis `./xl run`
(ou `make DEVICE=oneplus-6t phone ARGS="touchscreen=2"` qui fait tout et sauve dans `logs/`).

`DEVICE=oneplus-6t` est OBLIGATOIRE sur mkpayload/mkxnuimg/make : sinon `mkxnuimg` prend l'UEFI POCO
(il lit `devices/oneplus-6t/uefi/uefi.conf` pour l'UEFI fajita : FD 0x9FC00000 / 0x300000).

> ⚠️⚠️ **NE JAMAIS faire `fastboot --set-active=a`.** LineageOS est sur le slot **b**. Passer sur `a`
> (vide/périmé) met le tél en **bootloop sur le logo** (pas de brick, le RAM-boot ne touche rien, mais
> flippant). Récup : force l'extinction (Power ~12 s) → Power+Volume Haut → fastboot → `fastboot
> --set-active=b` → `fastboot reboot`. Toujours `--set-active=b`, jamais `a`.

> 💡 **Canal de log SANS USB** : le loader écrit son log (loader + kernel du dernier run) dans la
> partition GPT **"logdump"** (sur ce 6T : `/dev/block/sde71`, symlink dans `/dev/block/by-name/`).
> Après un boot, reboot en Android et : `adb root` puis
> `adb shell 'dd if=/dev/block/sde71 bs=4096 count=64 2>/dev/null' > ~/logdump.bin && strings ~/logdump.bin`.
> C'est le plan B quand l'USB bulk déconne (voir ci-dessous).

> 🔌 **USB bulk capricieux sur fajita.** L'énumération marche (`fastboot devices` → `xnuloader`), mais
> les transferts bulk (getvar, oem, upload ramdisk) sont intermittents : `getvar product` revient
> tantôt vide tantôt en hang. Depuis l'HÔTE (hors distrobox) l'énumération est fiable mais le bulk
> reste capricieux. Suspect : négociation SuperSpeed (le loader annonce HS 512 + SS 1024 ; bulk max
> packet SS = 1024). Piste si on y revient : forcer HighSpeed, ou fiabiliser usb_wait. En attendant,
> pour obtenir un **log de boot kernel**, passer par le boot AUTONOME (payload sans `--usb`, le loader
> lance run_kernel tout seul) + lecture logdump — pas besoin de l'USB bulk.

## Le vrai dtb 6T (sinon l'ABL refuse de sauter)

L'ABL du 6T valide le **board-id** du dtb appendé et **ne saute pas** s'il ne matche pas (6T=18801,
pas l'OnePlus 6=17819) : `fastboot boot` dit `OKAY`/`Booting` mais le tél reste figé sur fastboot.
Tirer le vrai dtb (tél en Android, `adb root`, **pas `su`**) :
```sh
adb root && adb shell id            # uid=0(root)
adb shell 'cat /sys/firmware/fdt' > Mu-Silicium/Resources/DTBs/fajita.dtb   # ~500 Ko
cp Mu-Silicium/Resources/DTBs/fajita.dtb devices/oneplus-6t/prebuilt/device-tree.dtb
```
`mkxnuimg` appende `Mu-Silicium/Resources/DTBs/fajita.dtb`. Le rebuild UEFI n'est PAS nécessaire pour
changer ce dtb (le `.fd` ne l'embarque pas) — juste reconstruire l'image.

## Pièges de build UEFI (tous contournés, voir BRINGUP.md § étape 4)

- **Mono/NuGet** pour `mu_nasm` : sur cette Debian, NuGet n'installe pas le paquet (TLS Mono, pas de
  `cert-sync`). Contournement retenu : neutraliser le ext_dep NuGet et utiliser le `nasm` d'apt
  (`mv Mu_Basecore/BaseTools/Bin/nasm_ext_dep.yaml{,.disabled}` + `export NASM_PREFIX=/usr/bin/`).
- **Dossiers `fajita` manquants** dans les submodules (ACPI, Binaries) : `make-fajita-pkg.sh` les
  dérive maintenant d'enchilada (helper `derive_sibling`). Trees générés par l'ancien script :
  `cp -r Silicium-ACPI/Platforms/OnePlus/enchilada .../fajita` et `cp -r Binaries/enchilada Binaries/fajita`.
- `build_uefi.py` **saute l'update** si `Build/` existe et pas de `-u` (⇒ « mu_nasm not met »). Ne PAS
  passer `-u` (il `git pull` Mu-Silicium et casse le patch XnuLoader). Forcer via
  `DeviceBuild.py --setup/--update` directement.

## Faits matériels (sources vérifiées : dtb réel, MemoryMapLib enchilada, sdm845.dtsi mainline)

- SoC **SDM845 "Napali"**, 8 cœurs (4 silver A55 cœurs 0-3 + 4 gold A75 cœurs 4-7), MPIDR Aff1=0..7.
- **GICv3** : dist `0x17A00000`, redist `0x17A60000` (identique à redwood/SM7325).
- **Framebuffer UEFI (GOP)** = région cont-splash « Display Reserved » **`0x9D400000`** (taille
  `0x02400000`). Panel `s6e3fc2x01` 1080×2340.
- **UEFI FD** (Mu-Silicium fajita.toml) : base `0x9FC00000`, size `0x300000` (≠ POCO 0x9FD00000/0x200000).
- PSCI par **SMC** (pas HVC), pas de PL011. USB device = **protocole UEFI** `QCOM_USB_DEVICE_GUID`
  (pas de MMIO DWC3 hardcodée → devrait marcher via l'UEFI).
- `soc.args` (modem/IPA) : voir `soc.args` et `HARDWARE.md` (tirés du vrai device tree).

## Divergences loader.c POCO→fajita (ce qui a été / reste à toucher)

- `xnuloader/loader.c` : `platform_fajita` ajouté (= redwood car même layout Qualcomm) ; `efi_main`
  branche sur le framebuffer `0x9D400000` pour utiliser le GOP direct au lieu de `ramfb_phone_screen()`
  (QEMU). `charger_start()`/`wdt_arm()` restent redwood-only → fajita les saute (OK pour bring-up).
- Suspects des prochains murs : UFS (le mur classique des portages XNU ; base `0x1d84000` — déjà la
  même que POCO d'après DRIVERS.md), puis le touch (driver `hakios.oneplus6t.c`, boot-arg
  `touchscreen=2`). Le kernel custom (avec le touch) n'est PAS encore buildé (linker XNU bloqué, voir
  BRINGUP) ; le prebuilt `prebuilt/mach.development.bcm2837` suffit pour le 1er boot.

## Workflow repo

- Remote utilisateur = `origin` (= son fork). Le stop-hook compare à `origin/oneplus-6t-port-scaffold`
  qui n'existe pas → **faux positif systématique « unpushed commits »**, ignorer (vérifier avec
  `git rev-list --count origin/oneplus-6t-port-scaffold..HEAD` quand dispo).
- Tout commit se termine par les lignes Co-Authored-By / Claude-Session de la session courante.
