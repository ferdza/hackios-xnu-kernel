# OnePlus 6T — bring-up pas à pas (le tuto de con)

> **À lire d'abord.** Personne n'a encore booté HakiOS sur la 6T. Ce tuto t'amène au point où tu
> peux lancer un kernel sur le tél et **lire un log**. Il ne va probablement **pas** marcher du
> premier coup — le but de la première fois, c'est de récupérer le log série et de me l'envoyer.
> On ne flashe **rien** : `fastboot boot` charge en RAM, un reboot te ramène sur LineageOS.

Tout se fait sur ton PC Linux, la 6T branchée en USB. Commandes à copier-coller.

---

## 📍 OÙ TU EN ES (mis à jour le 2026-10-27)

```
[x] 0. Outils installés (apt + theos)   <- ajoute gawk ! (voir étape 0)
[x] 2. make images .......... OK
[x] 3. make loader .......... OK (loader.efi)
[x] 3. setup.sh --stage1 .... OK (tcsh buildé, availability.pl créé)
[~] 3. make kernel ........... OPTIONNEL pour le 1er boot : xl prend prebuilt/mach.* tout seul si
                             aucun kernel n'est buildé. Le kernel custom n'ajoute QUE le driver
                             touch (= milestone 4). Linker XNU veut ~13 Go RAM -> sur 16 Go avec un
                             bureau ouvert ça passe pas (« ld64: Cannot fork »). Pour le builder
                             plus tard : TTY nu (bureau tué) + make kernel JOBS=1, ou linker lld.
[x] 4a. make-fajita-pkg + add-xnuloader ... OK (fajitaPkg généré, couche XnuLoader appliquée)
[x] 4b. mono-devel installé ... OK (l'erreur Mono a disparu)
[x] 4c. nasm : NuGet KO -> contournement nasm d'apt (bloc 🩹🩹). OK.
[x] 4d. dossiers `fajita` manquants dans submodules (ACPI + Binaries) -> 2 x cp (bloc 🩹🩹🩹). OK.
[x] 4e. build_uefi.py ......... OK -> SILICIUM_UEFI.fd (3 Mo) buildé. 🎉
[-] 5. firmware ............. OPTIONNEL pour le 1er boot (a630/wifi/modem pas dans le chemin critique
                             avec touchscreen=2). Tél en Android, depuis la RACINE.
[x] 6a. dtb 6T réel ......... OBLIGATOIRE. Sans lui l'ABL refuse de sauter (fastboot boot = OKAY mais
                             tél figé). Tiré via `adb root` + cat /sys/firmware/fdt (~500 Ko). OK.
[x] 6b. fastboot boot ....... OK ! l'ABL saute enfin dans l'UEFI.
🚩 6c. UEFI s'exécute ....... mais plante : « Synchronous Exception at 0x... » (crash EDK2/ArmPkg ->
                             le cœur UEFI tourne). EN COURS : lire le dump (PC/module/ESR/FAR) pour
                             savoir quel driver faute. Blobs Qualcomm empruntés à l'OnePlus 6 =
                             suspect n°1 (un DXE device-specific qui tape un registre différent).
[ ] 7. loader + kernel + log (une fois l'UEFI passé)
```

**Ta prochaine étape :** lire le dump de l'exception UEFI à l'écran (photo) — PC, nom de module, ESR,
FAR. C'est ce qui dit quel composant faute. On exécute enfin du code sur le tél ; reste à passer
l'UEFI, puis on atteint le loader HakiOS et XNU.

> ⚠️ **Deux pièges qui t'ont déjà mordu :**
> 1. **`make ...` se lance depuis la RACINE du dépôt** (`~/hackios-xnu-kernel`), **jamais** depuis
>    `Mu-Silicium/`. Les cibles `firmware`/`phone` sont dans le Makefile du haut ; depuis `Mu-Silicium/`
>    tu as « Aucune règle pour fabriquer la cible ». Entre les deux : `cd ~/hackios-xnu-kernel`.
> 2. Le build UEFI (`build_uefi.py`) va chercher `nasm` via NuGet, qui tourne via **Mono**. Sans Mono :
>    « Mono is required to run NuGet ... Failed to fetch NugetDependecy: mu_nasm ». Fix à l'étape 4.

---

## 0. Les outils (une seule fois)

**Arch :**
```sh
sudo pacman -S --needed base-devel git curl clang lld python perl xz nasm llvm aarch64-linux-gnu-gcc python-pip python-virtualenv android-tools
```
**Debian/Ubuntu :**
```sh
sudo apt install build-essential git curl clang lld python3 perl xz-utils nasm llvm \
  gcc-aarch64-linux-gnu python3-pip python3-venv android-tools-adb android-tools-fastboot \
  bison flex m4 autoconf automake libtool pkg-config uuid-dev lzfse libncurses-dev libcrypt-dev gawk
```
> `gawk` est important sur Debian : l'`awk` par défaut (mawk) casse la génération de la table
> d'appels système (`init_sysent.c` sort `\(sy_call_t *\)` au lieu de `(sy_call_t *)`). Installer
> `gawk` le met par défaut et règle ça. (Le dépôt a aussi un fix dans `hakios.patch` pour que mawk
> passe, mais il ne s'applique qu'à un arbre `xnu-src/` **frais** — si le tien est déjà déplié,
> `gawk` est la voie rapide.)
> Les derniers sont indispensables au build du kernel/loader : sans eux tu auras `bison: Aucun
> fichier`, `'uuid/uuid.h' file not found`, `no LZFSE decoder`, ou `tcsh did not build` (ça =
> `libncurses-dev` manquant, puisque `setup.sh` compile tcsh et s'arrête dessus). Si tu as lancé un
> build avant de les installer, relance **`xnu-deps/setup.sh --stage1`** à la main ensuite (il
> complète le SDK, dont `availability.pl`) — `make` ne le relance pas tout seul une fois `xnu-clang`
> construit.

Il te faut aussi **theos** dans `~/theos` (pour les outils `ld64`/`otool` et le SDK `iPhoneOS16.5.sdk`) — voir https://theos.dev/docs/installation-linux.

---

## 1. Récupérer ton dépôt

```sh
git clone https://github.com/ferdza/hackios-xnu-kernel.git
cd hackios-xnu-kernel
git checkout oneplus-6t-port-scaffold
```

---

## 2. Les trucs lourds d'Apple (une seule fois)

HakiOS pose le kernel XNU sous l'espace utilisateur d'iOS 16.5. Il te faut l'**IPSW iOS 16.5**
(celui d'un iPhone, à télécharger chez toi — c'est du logiciel Apple, pas dans le dépôt) :

```sh
make fetch        # extrait les fichiers OS depuis l'IPSW dans sysimage/ (il te demande le chemin de l'IPSW)
make images       # construit userland/root.apfs et userland/system.apfs
```

---

## 3. Construire le kernel + le loader

> **Raccourci :** le kernel est **optionnel** pour un premier boot. `xl` retombe sur
> `prebuilt/mach.development.bcm2837` si tu n'en builds pas un. Le kernel custom n'ajoute que le
> driver touch (`hakios.oneplus6t.c`, milestone 4). Linker XNU demande ~13 Go de RAM : sur une
> machine 16 Go avec un bureau ouvert, le link échoue (« ld64: Cannot fork »). Pour le 1er boot,
> **saute cette étape** (garde juste `make loader`) et va à l'UEFI. Pour builder le kernel plus tard :
> un TTY nu (bureau fermé) libère assez de RAM pour `make kernel JOBS=1`, ou passe le link sur lld.

**D'abord, une fois, cette commande** (elle finit de préparer le SDK — crée `availability.pl`) :
```sh
xnu-deps/setup.sh --stage1
```
> ⚠️ Piège : `make kernel` ne relance **pas** `setup.sh` tout seul une fois `xnu-clang` construit.
> Si un premier build a laissé un SDK incomplet, tu auras `availability.pl ... not found`. Cette
> commande règle ça. Elle est sans risque à relancer. Vérifie après :
> `ls xnu-deps/sdk/iPhoneOS16.5.xnu.sdk/usr/local/libexec/availability.pl`

Ensuite :
```sh
make kernel       # applique les sources XNU d'Apple + HakiOS, puis compile (xnu-build.sh)
make loader       # le bootloader HakiOS  (déjà fait chez toi -> loader.efi)
```
> Normal : le build kernel finit **toujours** sur une erreur `check_all_exports` **après** avoir lié
> le kernel. Regarde la date du fichier, pas le code de sortie. C'est pour ça que `-k` est là.

Ton driver touch (`hakios.oneplus6t.c`) est compilé automatiquement dans le kernel.

---

## 4. Construire l'UEFI fajita

**Avant tout, Mono** (le build EDK2 tire `nasm` via NuGet, qui tourne via Mono — c'est ce qui a
planté chez toi avec « Failed to fetch NugetDependecy: mu_nasm »). C'est le paquet que le propre
`setup_env.sh` de Mu-Silicium installe sur Debian :
```sh
sudo apt install mono-devel
# si NuGet râle ensuite sur les certificats TLS :  sudo cert-sync /etc/ssl/certs/ca-certificates.crt
```

**⚠️ TON VRAI DTB 6T — PAS OPTIONNEL.** Sans lui, l'image embarque le dtb de l'OnePlus 6
(board-id 17819). L'ABL du 6T **valide le board-id du dtb appendé et refuse de sauter** s'il ne
matche pas (18801) : `fastboot boot` répond `OKAY`/`Booting` mais le tél **reste figé sur l'écran
fastboot** (vécu). Avec le bon dtb, l'ABL saute enfin dans l'UEFI. Tire-le du tél, en Android, via
**`adb root`** (ton root c'est le daemon en root — la commande `su` n'existe pas ici) :
```sh
adb wait-for-device && adb shell getprop sys.boot_completed   # attends "1"
adb root                                                      # "restarting adbd as root" ; attends ~3 s
adb shell id                                                  # doit montrer uid=0(root)
adb shell 'cat /sys/firmware/fdt' > devices/oneplus-6t/prebuilt/device-tree.dtb   # doit faire ~500 Ko, pas 0
```
> Le dtb vit au final dans `Mu-Silicium/Resources/DTBs/fajita.dtb` (ce que `mkxnuimg` appende). Soit
> tu le poses dans `devices/oneplus-6t/prebuilt/device-tree.dtb` **avant** `make-fajita-pkg.sh` (il le
> prend), soit tu l'écris direct dans `Mu-Silicium/Resources/DTBs/fajita.dtb` et tu reconstruis juste
> l'image (`mkxnuimg`), sans rebuilder l'UEFI (le `.fd` n'embarque pas ce dtb).

Puis le paquet et le build :
```sh
devices/oneplus-6t/uefi/make-fajita-pkg.sh     # génère le fajitaPkg (ton écran, ton board-id, ton dtb)
devices/oneplus-6t/uefi/add-xnuloader.sh       # ajoute la couche qui démarre XNU
cd Mu-Silicium
python3 -m venv venv && venv/bin/pip install -r pip-requirements.txt
source venv/bin/activate
python3 build_uefi.py -d fajita -r RELEASE     # -> Build/fajitaPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd
cd ..                                          # <- RETOUR À LA RACINE avant les étapes 5 et 6 !
```

> 🩹 **Si un premier `build_uefi.py` a déjà planté** (typiquement : il a tourné *avant* que Mono soit
> installé et a échoué sur `mu_nasm`). Il a quand même créé le dossier `Build/fajitaPkg/…`, et
> `build_uefi.py` **saute la phase de téléchargement des dépendances** quand ce dossier existe déjà et
> que tu ne passes pas `-u` → tu te retrouves avec « Dependency 'mu_nasm' is not met! ... run
> stuart_update ». Mono est prêt mais n'a jamais été appelé. Force le téléchargement **une fois**
> (surtout **pas** `-u` : il ferait un `git pull` sur Mu-Silicium qui casserait ton arbre patché) :
> ```sh
> cd Mu-Silicium && source venv/bin/activate
> python3 Platforms/OnePlus/fajitaPkg/DeviceBuild.py --setup  -t RELEASE
> python3 Platforms/OnePlus/fajitaPkg/DeviceBuild.py --update -t RELEASE   # <- tire mu_nasm via NuGet/Mono
> python3 build_uefi.py -d fajita -r RELEASE                               # repart, dépendances OK
> cd ..
> ```

> 🩹🩹 **Si NuGet refuse quand même de télécharger `mu_nasm`** (« [Nuget] We failed to install this
> version » même avec Mono en place — TLS de Mono cassé, pas de `cert-sync`, réseau...). Pas la peine
> de s'acharner : **cette dépendance n'est que `nasm`**, que tu as déjà installé par apt, et le
> descripteur a le flag **`set_path`** = edk2 veut juste `nasm` dans le `PATH`. On neutralise le
> téléchargement NuGet et on pointe edk2 sur le `nasm` du système (méthode edk2 « hors-ligne ») :
> ```sh
> cd Mu-Silicium
> mv Mu_Basecore/BaseTools/Bin/nasm_ext_dep.yaml Mu_Basecore/BaseTools/Bin/nasm_ext_dep.yaml.disabled
> export NASM_PREFIX=/usr/bin/          # nasm d'apt est /usr/bin/nasm ; le prefix DOIT finir par /
> which nasm && nasm -v                 # sanity : /usr/bin/nasm + version 2.x
> source venv/bin/activate
> python3 build_uefi.py -d fajita -r RELEASE
> cd ..
> ```
> Rien d'autre à fetcher (le log ne liste qu'**une** dépendance en échec, `mu_nasm` ; les autres sont
> OK). `build_uefi.py` ne touche pas à ce `.yaml`, donc la neutralisation tient — sauf si tu refais un
> `git submodule update`, qui le restaurerait (re-fais le `mv` si ça arrive).

> 🩹🩹🩹 **Dossiers `fajita` manquants dans les submodules** (si ton `fajitaPkg` a été généré par une
> **ancienne** version du script). Le paquet renomme les *références* `enchilada→fajita` mais les
> *arbres* vivent dans des submodules indexés par appareil. Deux erreurs, dans l'ordre :
> 1. `Failed to convert Edk2Relative Path ... fajita/AcpiTables.inf` (PreBuild) → tables ACPI
>    (**Silicium-ACPI**).
> 2. `File/directory not found ... Binaries/fajita/QcomPkg/Drivers/EnvDxe/EnvDxe.inf` → blobs
>    Qualcomm (**Binaries**).
>
> Les 6/6T sont le même SoC Napali → on copie ceux de l'OnePlus 6 (copie exacte, aucun fichier texte
> ne contient le nom de l'appareil) :
> ```sh
> cd Mu-Silicium
> cp -r Silicium-ACPI/Platforms/OnePlus/enchilada Silicium-ACPI/Platforms/OnePlus/fajita
> cp -r Binaries/enchilada Binaries/fajita
> source venv/bin/activate && python3 build_uefi.py -d fajita -r RELEASE && cd ..
> ```
> (Plus propre à partir de maintenant : `make-fajita-pkg.sh --force` fait les deux + le reste. Mais il
> faut alors refaire `add-xnuloader.sh`, re-`mv` le `nasm_ext_dep.yaml`, et reposer ton dtb. Pour
> débloquer *ton* build en cours, les deux `cp` ci-dessus suffisent.)
>
> ⚠️ Réutiliser les blobs Qualcomm de l'OnePlus 6 est un amorçage : ça suffit pour un premier boot,
> mais si l'UEFI se comporte mal (écran, charge), c'est le premier suspect — les vrais blobs du 6T
> s'extraient de son OxygenOS (milestone ultérieur).

> Si tu as déjà lancé `make-fajita-pkg.sh` **avant** de poser ton dtb, relance-le avec `--force` pour
> réembarquer le bon. Tu n'as pas à retoucher `add-xnuloader.sh` (il est idempotent).
>
> Le reste de la chaîne (empaqueter `SILICIUM_UEFI.fd` + le BootShim + ton dtb dans l'image
> `fastboot boot`) connaît fajita tout seul dès que tu passes `DEVICE=oneplus-6t` à `make` : les
> adresses FD de la 6T (`0x9FC00000` / taille `0x300000`, différentes de la POCO) viennent de
> `devices/oneplus-6t/uefi/uefi.conf`. Rien à régler à la main.

---

## 5. Les firmwares de la 6T (OPTIONNEL pour le 1er boot — tél en Android/LineageOS)

> **Saute cette étape pour ta première tentative.** Les firmwares (GPU a630, Wi-Fi WCN3990, ADSP,
> modem) ne servent qu'aux drivers GPU/wifi/modem, qui ne sont **pas** activés par `touchscreen=2` et
> ne sont pas sur le chemin critique du premier boot (on vise le mur UFS). `make DEVICE=oneplus-6t
> firmware` échoue d'ailleurs si le tél n'est pas en **Android** sur adb (« no devices/emulators
> found » = tu étais en fastboot). À faire plus tard, tél en Android, **depuis la racine**.

```sh
make DEVICE=oneplus-6t firmware    # tire a630, wlanmdsp, adsp, modem... dans fw-*/
```

---

## 6. Booter sur le tél

Mets la 6T en **fastboot** (éteins, puis Volume Haut + Power), branchée en USB.

> ⚠️ **Le BON fastboot.** `fastboot boot` (charger en RAM) n'existe QUE dans le fastboot du
> **bootloader** (écran « FASTBOOT mode »). **fastbootd** (fastboot *userspace*, écran avec menu,
> atteint depuis le recovery) ne connaît pas `boot` → `FAILED (remote: unknown command)`. Vérifie :
> ```sh
> fastboot getvar is-userspace    # "yes" = fastbootd (MAUVAIS)  |  "no" = bootloader (BON)
> ```
> Si tu es en fastbootd : `fastboot reboot bootloader`. (Et `xl`/`make phone` partent d'Android et
> font `adb reboot bootloader` tout seuls, donc en les laissant conduire tu es toujours dans le bon.)

La 6T refuse `fastboot boot` sur un slot « usé » — d'abord :

```sh
fastboot --set-active=b
```

Puis, depuis la racine du dépôt. **Important :** le `ARGS` par défaut de `make phone` est réglé
pour la POCO (`touchscreen=1` = Goodix, plus modem/wifi/son qui n'existent pas pareil sur fajita et
peuvent faire planter). Pour une première fois, passe un `ARGS` **minimal** avec ton driver à toi :

```sh
make DEVICE=oneplus-6t phone ARGS="touchscreen=2"   # xl enter / ramdisk / run, puis retour au loader
```
`touchscreen=2` = ton driver Synaptics (le `=1` resterait le Goodix de la POCO). À la main, étape
par étape :
```sh
./xl enter                         # entre dans le loader (via fastboot boot de l'UEFI+kernel)
./xl ramdisk userland/system.apfs  # charge l'espace utilisateur iOS en RAM
XL_CMDLINE="debug=0x8 rd=md0 cpus=8 ufs=2 touchscreen=2" ./xl run   # lance le kernel
```

---

## 7. Le log (ce que je veux)

`make phone` et `./xl run` écrivent le log dans `logs/`. Récupère le dernier :

```sh
ls -t logs/*-run.log* | head -1        # le fichier le plus récent
```

**Colle-moi ce fichier** (ou ses 200 dernières lignes). C'est là-dedans qu'on voit où ça coince :
le lien UFS (`ufs:`), le montage APFS, et ton driver touch (lignes `oneplus6t:` / `tl:`).

Boot-args utiles à ajouter dans `ARGS="..."` de `make phone` :
- `touchscreen=2` → ton driver Synaptics (déjà par défaut si tu passes `ARGS="touchscreen=2 ..."`)
- `touch_lat=1` → une ligne par rapport touch lu, pour voir s'il parle
- `ufs=2` → le stockage via le loader (déjà mis par `modem-cycle`)

---

## À quoi s'attendre la première fois

- **Le mieux réaliste :** l'UEFI boote, le loader démarre, le kernel part et log jusqu'à un mur
  (souvent le stockage UFS, comme les autres portages XNU). C'est un **succès** pour une V1 : on a
  un log à débugger.
- **Pire cas :** écran noir, le tél reboot tout seul sur LineageOS après ~30 s (watchdog). Pas
  grave, rien de cassé. Renvoie quand même ce que `logs/` a attrapé.

Dès que tu as un log, balance-le-moi et on itère. 🔧
