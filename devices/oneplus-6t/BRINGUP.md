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
🚩 3. make kernel ........... si erreur « init_sysent.c ... \(sy_call_t » -> sudo apt install gawk
                             (mawk de Debian casse la génération ; gawk la répare) puis  make kernel
                             succès = ça finit sur  check_all_exports  (voir étape 3)
[ ] 4. UEFI fajita
[ ] 5. firmware (make DEVICE=oneplus-6t firmware)
[ ] 6. boot sur le tél
[ ] 7. m'envoyer le log
```

**Ta prochaine commande, juste celle-là :**
```sh
xnu-deps/setup.sh --stage1 && make kernel
```
Si `make kernel` finit sur `check_all_exports` → **c'est normal, c'est gagné** (voir étape 3).
Si ça meurt ailleurs → colle-moi la sortie.

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

```sh
devices/oneplus-6t/uefi/make-fajita-pkg.sh     # génère le fajitaPkg (ton écran, ton board-id, ton dtb)
devices/oneplus-6t/uefi/add-xnuloader.sh       # ajoute la couche qui démarre XNU
cd Mu-Silicium
python3 -m venv venv && venv/bin/pip install -r pip-requirements.txt
source venv/bin/activate
python3 build_uefi.py -d fajita -r RELEASE     # -> Build/fajitaPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd
cd ..
```

---

## 5. Les firmwares de la 6T (une seule fois, tél en Android/LineageOS)

```sh
make DEVICE=oneplus-6t firmware    # tire a630, wlanmdsp, adsp, modem... dans fw-*/
```

---

## 6. Booter sur le tél

Mets la 6T en **fastboot** (éteins, puis Volume Haut + Power), branchée en USB. La 6T refuse
`fastboot boot` sur un slot « usé » — d'abord :

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
