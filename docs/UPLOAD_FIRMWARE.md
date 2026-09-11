# Upload du Firmware sur macOS

Guide concis pour uploader le firmware sur le XIAO ESP32-C3/S3 depuis macOS.

---

## Méthode 0 : OTA par l'interface web (flux de prod, sans câble) ⭐

C'est la méthode utilisée en atelier/prod : on met à jour le firmware **par Wi-Fi**,
via la page web servie par la carte, **sans câble série**.

### Étapes

1. **Builder l'image** (voir distinctions ci-dessous — la commande est `build`, PAS `compile`, et il faut `--ota`) :

   ```bash
   ./scripts/nidmi.sh build --board s3 --variant on --ota
   ```

2. **Récupérer l'image app** dans `bin/` :
   - Variante prod : **`bin/nidmi-s3-usbmidi-on.bin`** (= copie de `bin/nidmi_basic.ino.bin`).
   - ⚠️ Prendre l'**image app `.bin`**, surtout **PAS** le `.merged.bin` (bootloader+partitions+app, réservé au flash série complet / ESP Web Tools).
   - Vérifier le `mtime` du fichier pour être sûr de charger le build du jour.

3. **Flasher depuis l'interface** : ouvrir `http://192.168.4.1`, onglet/section **« Firmware (OTA) »** →
   bouton **« Flasher »** → choisir le `.bin`. La carte écrit dans le slot OTA libre puis **redémarre** dessus.

### Trois pièges à connaître (sources d'erreurs réelles)

| Piège | Détail |
|-------|--------|
| **`compile` ≠ `build`** | `./scripts/nidmi.sh compile` ne fait que **vérifier** la compilation dans le cache Arduino (nettoyé juste après) → **rien n'est écrit dans `bin/`**. Seul **`build`** (`--output-dir bin`) produit l'image. Après un `compile`, le `bin/` contient encore l'**ancien** build (timestamp périmé). |
| **Variante USB-MIDI** | `--variant on` = `usb_mode=0` (TinyUSB/OTG, **requis pour le MIDI USB**, c'est la variante de **prod**). `--variant off` = `usb_mode=1` (HW CDC/JTAG, série stable pour **debug**). Par défaut le script suit le flag du header `UsbMidiManager.h` — **toujours préciser `--variant on` pour la prod**. |
| **`--ota` obligatoire** | L'OTA nécessite une table à **2 slots app** (`--ota` → `nidmi_s3_ota_dual_littlefs`). Sans `--ota` : la carte ne peut pas recevoir l'OTA (`Update.begin()` échoue), et flasher une image non-`--ota` fait **perdre la capacité OTA** pour la fois d'après. Cf. `src/api/OtaAPI.cpp`. |

### Robustesse

- Le corps est envoyé en **octet-stream brut** (Content-Length = taille exacte) ; si l'upload est
  tronqué (coupure Wi-Fi), la dernière frame n'arrive pas → la partition n'est pas marquée bootable
  → la carte **reste sur le firmware actuel** (pas de brick).
- Endpoint `POST /api/ota` **non authentifié** : usage atelier sur AP isolé uniquement.
  À protéger (token) si exposé sur un réseau ouvert.

---

## Prérequis (Installation)

### Installer Python 3 et pip3

```bash
# Installer Homebrew si ce n'est pas déjà fait
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Installer Python 3 (qui inclut pip3)
brew install python3
```

### Vérifier l'installation

```bash
python3 --version
pip3 --version
```

## Méthode 1 : Script automatique (Recommandé)

### Compiler et uploader automatiquement

```bash
cd /Users/patricecolet/repo/NiDMI
./scripts/nidmi.sh upload
```

### Reset NVS (sketch dédié)

Pour effacer la NVS, utiliser le sketch `nidmi_clear_nvs` via l’option `--clear-nvs`. **Reprend les mêmes options que pour un `upload` normal** (`--board`, `--split-fs`, `--no-large-app`, `--port`, etc.) : sans `--split-fs`, le profil par défaut est déjà aligné entre `nidmi_clear_nvs` et `nidmi_basic` (sur S3, partition Arduino par défaut ; sur C3, `--large-app` du script sauf `--no-large-app`). N’ajoute `--split-fs` que si tu l’utilises déjà pour le firmware.

```bash
./scripts/nidmi.sh upload --clear-nvs
```

Alternative côté PC (`pip install esptool`) :

```bash
./scripts/nidmi.sh erase-nvs --port /dev/cu.usbmodemXXXX
```

### Compiler et générer le fichier binaire

```bash
# Image OTA de prod (USB-MIDI ON, partition dual-OTA) :
./scripts/nidmi.sh build --board s3 --variant on --ota
```

Génère dans `bin/` :
- `bin/nidmi_basic.ino.bin` (+ copie étiquetée `bin/nidmi-s3-usbmidi-on.bin`) — **image app**, à charger en **OTA** (voir [Méthode 0](#méthode-0--ota-par-linterface-web-flux-de-prod-sans-câble-)).
- `bin/nidmi_basic.ino.merged.bin` (+ `bin/nidmi-s3-usbmidi-on.merged.bin`) — image complète bootloader+partitions+app, pour le **flash série complet** (esptool / ESP Web Tools).

> ⚠️ `build` écrit dans `bin/`. `compile` ne le fait **pas** (il vérifie seulement la compilation dans le cache).

## Méthode 2 : Arduino IDE

### Via l'interface graphique

1. Ouvrir Arduino IDE
2. Fichier > Ouvrir > Sélectionner le sketch dans `examples/nidmi_basic/`
3. Outils > Type de carte > XIAO_ESP32C3 (ou XIAO_ESP32S3)
4. Outils > Port > Sélectionner le port série
5. Bouton "Téléverser" (compilera puis uploadera automatiquement)

**Note** : Arduino IDE compile toujours avant d'uploader. Il n'y a pas d'option pour uploader sans compiler.

## Méthode 3 : esptool.py en ligne de commande

### Installer esptool

```bash
pip3 install esptool
```

### Trouver le port série

```bash
# Lister les ports série disponibles
ls /dev/cu.usbmodem* /dev/cu.SLAB_USBtoUART* 2>/dev/null
```

### Uploader le firmware

**Pour ESP32-C3 :**
```bash
esptool.py --chip esp32c3 \
  --port /dev/cu.usbmodem* \
  write_flash 0x0 bin/nidmi_basic.ino.merged.bin
```

**Pour ESP32-S3 :**
```bash
esptool.py --chip esp32s3 \
  --port /dev/cu.usbmodem* \
  write_flash 0x0 bin/nidmi_basic.ino.merged.bin
```

### Utiliser esptool.py inclus avec Arduino IDE

Si vous préférez utiliser l'esptool.py inclus avec Arduino IDE (sans installer Python globalement) :

```bash
# Trouver esptool.py
ESPTOOL=$(find ~/Library/Arduino15/packages/esp32/tools/esptool_py -name esptool.py 2>/dev/null | head -1)

# Uploader
python3 "$ESPTOOL" --chip esp32c3 --port /dev/cu.usbmodem* write_flash 0x0 bin/nidmi_basic.ino.merged.bin
```

## Méthode 4 : Format UF2 (non supporté nativement)

Le format UF2 est généré automatiquement par le script (`./scripts/nidmi.sh build`), mais **le XIAO ESP32-C3/S3 ne supporte pas nativement le drag-and-drop UF2** comme le RP2040.

Pour ESP32, utilisez les méthodes ci-dessus (Arduino IDE ou esptool.py).

## Résumé des commandes rapides

```bash
# 1. Compiler + uploader (tout automatique)
./scripts/nidmi.sh upload

# 2. Compiler uniquement (génère le .bin)
./scripts/nidmi.sh build

# 3. Uploader avec esptool (nécessite pip3 install esptool)
esptool.py --chip esp32c3 --port /dev/cu.usbmodem* write_flash 0x0 bin/nidmi_basic.ino.merged.bin
```

## Dépannage

### Port série non trouvé

```bash
# Vérifier que le XIAO est connecté
ls /dev/cu.*

# Si aucun port n'apparaît :
# - Vérifier le câble USB
# - Vérifier que le XIAO est allumé
# - Essayer un autre câble USB (certains ne transmettent que l'alimentation)
```

### Erreur de permissions

```bash
# Sur macOS, généralement pas nécessaire, mais si problème :
sudo chmod 666 /dev/cu.usbmodem*
```

### esptool.py non trouvé

```bash
# Vérifier si Python 3 est installé
python3 --version

# Installer esptool
pip3 install esptool

# Ou utiliser celui inclus avec Arduino IDE
find ~/Library/Arduino15/packages/esp32/tools/esptool_py -name esptool.py
```

## Variant `--usb-net` : interface web par le câble USB (S3)

```bash
./scripts/nidmi.sh upload --usb-net
```

Sert l'UI par le câble USB (CDC-NCM) **en parallèle** de l'USB-MIDI et du WiFi,
sur le même connecteur. Sans l'option, le firmware est inchangé. S3 seulement :
le C3 n'a pas d'USB-OTG.

L'UI n'a rien de spécial à faire — `AsyncWebServer` écoute sur `INADDR_ANY` et
`web/js/websocket.js` construit son URL depuis `window.location`.

| accès | adresse |
|---|---|
| câble USB | `http://192.168.7.1/` |
| AP WiFi | `http://192.168.4.1/` |
| STA | IP fournie par le réseau |
| mDNS | `http://nidmi.local/` — résout vers **toutes** les interfaces actives |

### Mesures (macOS 26.5, XIAO ESP32S3)

```
page          200, 15 077 o en 34 ms
bundle JS     200, 33 743 o en 64 ms — ~525 ko/s, stable sur 5 tirs
API           /api/pins/list 3878 o en 22 ms ; les autres 6-8 ms
WebSocket     connexion 9 ms, aller-retour 1.9-2.1 ms, fermeture propre
ping          1.3-1.8 ms
route         défaut inchangée : brancher l'instrument ne détourne rien
```

### Produire les binaires étiquetés

```bash
./scripts/nidmi.sh build --usb-net
```

```
bin/nidmi-s3-usbnet.bin           image applicative — pour l'OTA
bin/nidmi-s3-usbnet.merged.bin    bootloader + partitions + app — ESP Web Tools
```

Combinable avec `--variant` : `--variant on --usb-net` donne
`nidmi-s3-usbmidi-on-usbnet`. Sans aucune des deux options, aucun binaire
étiqueté n'est produit — le comportement d'avant est inchangé.

Le firmware s'identifie lui-même : `NIDMI_FW_VARIANT` vaut `usbmidi-on+usbnet`,
exposé par l'API et visible dans l'UI. Utile pour savoir ce qui tourne
réellement sur une carte sans avoir à la reflasher pour vérifier.

### Deux points à connaître

**Le lien met plus longtemps à monter qu'avec un firmware minimal.** Le boot de
NiDMI est lourd (WiFi, STA, MIDI, composants) et l'annonce de lien n'est émise
que depuis `nidmi_loop()`. Compter plusieurs dizaines de secondes avant que
l'hôte obtienne son bail DHCP, contre ~2 s sur un firmware nu.

**Les trois slots mDNS sont saturés** : `STA` + `AP` + `ETH` (le lien USB prend
la clé `ETH_DEF`). `CONFIG_MDNS_MAX_INTERFACES` vaut 3 dans les libs Arduino, il
n'y a pas de quatrième interface possible. Voir `nidmi-core/docs/USB_NET.md`.
