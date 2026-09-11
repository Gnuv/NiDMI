#!/bin/bash

# Script unifié pour nidmi
# Usage: ./scripts/nidmi.sh [OPTIONS]
# 
# Options:
#   sync     - Synchroniser les fichiers seulement
#   compile  - Synchroniser + compiler
#   upload   - Synchroniser + compiler + uploader
#   flash    - Uploader seulement (sans sync ni compile ; dernier binaire arduino-cli)
#   all      - Tout faire (sync + compile + upload + test)
#   clean    - Nettoyer le cache seulement
#   help     - Afficher cette aide

set -e  # Arrêter en cas d'erreur

# Variables
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}" )/.." && pwd)"

# Détection de la plateforme (macOS, WSL, Linux)
PLATFORM="linux"
if grep -qi microsoft /proc/version 2>/dev/null; then
    PLATFORM="wsl"
elif [[ "$OSTYPE" == darwin* ]]; then
    PLATFORM="mac"
fi

# Tentative de détection automatique du sketchbook Arduino de l'IDE
detect_sketchbook_path() {
    local prefs_file
    case "$PLATFORM" in
        mac)
            prefs_file="$HOME/Library/Arduino15/preferences.txt"
            ;;
        wsl)
            prefs_file="$HOME/.arduino15/preferences.txt"
            ;;
        linux)
            prefs_file="$HOME/.arduino15/preferences.txt"
            ;;
        *)
            return 1
            ;;
    esac

    if [ -f "$prefs_file" ]; then
        local path
        path="$(grep -E '^sketchbook.path=' "$prefs_file" 2>/dev/null | sed 's/^sketchbook.path=//')"
        if [ -n "$path" ]; then
            printf "%s" "$path"
            return 0
        fi
    fi
    return 1
}

MAC_LIB_DEFAULT="$HOME/Documents/Arduino/libraries/NiDMI"
MAC_CACHE_DEFAULT="$HOME/Library/Caches/arduino/sketches"
WSL_LIB_DEFAULT="$HOME/Arduino/libraries/NiDMI"
WSL_CACHE_DEFAULT="$HOME/.arduino15/sketches"
LINUX_LIB_DEFAULT="$HOME/Arduino/libraries/NiDMI"
LINUX_CACHE_DEFAULT="$HOME/.arduino15/sketches"

SKETCHBOOK_FROM_PREFS="$(detect_sketchbook_path || true)"

case "$PLATFORM" in
    mac)
        if [ -n "$SKETCHBOOK_FROM_PREFS" ] && [ -z "${ARDUINO_LIB_DIR:-}" ]; then
            ARDUINO_LIB_DIR="$SKETCHBOOK_FROM_PREFS/libraries/NiDMI"
        fi
        ARDUINO_LIB_DIR="${ARDUINO_LIB_DIR:-$MAC_LIB_DEFAULT}"
        ARDUINO_CACHE_DIR="${ARDUINO_CACHE_DIR:-$MAC_CACHE_DEFAULT}"
        ;;
    wsl)
        if [ -n "$SKETCHBOOK_FROM_PREFS" ] && [ -z "${ARDUINO_LIB_DIR:-}" ]; then
            ARDUINO_LIB_DIR="$SKETCHBOOK_FROM_PREFS/libraries/NiDMI"
        fi
        ARDUINO_LIB_DIR="${ARDUINO_LIB_DIR:-$WSL_LIB_DEFAULT}"
        ARDUINO_CACHE_DIR="${ARDUINO_CACHE_DIR:-$WSL_CACHE_DEFAULT}"
        ;;
    linux)
        if [ -n "$SKETCHBOOK_FROM_PREFS" ] && [ -z "${ARDUINO_LIB_DIR:-}" ]; then
            ARDUINO_LIB_DIR="$SKETCHBOOK_FROM_PREFS/libraries/NiDMI"
        fi
        ARDUINO_LIB_DIR="${ARDUINO_LIB_DIR:-$LINUX_LIB_DEFAULT}"
        ARDUINO_CACHE_DIR="${ARDUINO_CACHE_DIR:-$LINUX_CACHE_DEFAULT}"
        ;;
    *)
        echo "⚠️  Plateforme non reconnue: $PLATFORM"
        exit 1
        ;;
esac


BOARD_TYPE="s3"  # Par défaut: S3
BOARD="esp32:esp32:XIAO_ESP32S3:PSRAM=opi"
# Le mode USB S3 (S3_USB_PROPS) et le flag USB-MIDI sont calculés APRÈS le parsing
# des arguments — car --variant peut forcer le mode (voir compute_usb_midi_mode plus bas).
S3_USB_PROPS=()
DEFAULT_SKETCH="nidmi_basic"
NVS_RESET_SKETCH="nidmi_clear_nvs"
CLEAR_NVS=false
PORT_OVERRIDE=""

# Port série (optionnel, peut être forcé via --port)
SERIAL_PORT=""

# Langue par défaut (français)
LANG_CODE="fr"

# Options d'optimisation JSON
LIGHT_MODE=false
# Pagination par défaut : NiDMI nécessite la pagination pour éviter la troncature du JSON des définitions
PAGINATION_MODE=true

# Partition C3 sans SPIFFS (app ~4 Mo). Par défaut activée pour C3 uniquement (évite 97% flash).
LARGE_APP=false
NO_LARGE_APP=false
# Partition split-fs (2x LittleFS dédiés: seqfs + mapfs)
SPLIT_FS=false
# Partition OTA (S3) : 2 slots app (app0/app1) + seqfs + mapfs -> permet la MAJ firmware par Wi-Fi
OTA_MODE=false
# Variante de firmware (off|on) : force le mode USB-MIDI au build sans éditer le header.
#   off -> USB-MIDI désactivé, S3 en usb_mode=1 (HW CDC/JTAG, série stable)
#   on  -> USB-MIDI activé, S3 en usb_mode=0 (OTG/TinyUSB, requis pour le MIDI)
# Vide = utilise la valeur par défaut du header UsbMidiManager.h.
VARIANT=""
USB_MIDI_DEFINE=()

# Parser les arguments pour --lang, --board, --light, --pagination, --no-pagination, --large-app, --no-large-app, --split-fs, --port
ARGS=()
while [[ $# -gt 0 ]]; do
    case $1 in
        --lang)
            LANG_CODE="$2"
            shift 2
            ;;
        --board)
            BOARD_TYPE="$2"
            shift 2
            ;;
        --port)
            PORT_OVERRIDE="$2"
            SERIAL_PORT="$2"
            shift 2
            ;;
        --clear-nvs)
            CLEAR_NVS=true
            shift
            ;;
        --light)
            LIGHT_MODE=true
            shift
            ;;
        --pagination)
            PAGINATION_MODE=true
            shift
            ;;
        --no-pagination)
            PAGINATION_MODE=false
            shift
            ;;
        --large-app)
            LARGE_APP=true
            shift
            ;;
        --no-large-app)
            NO_LARGE_APP=true
            shift
            ;;
        --split-fs)
            SPLIT_FS=true
            shift
            ;;
        --ota)
            OTA_MODE=true
            shift
            ;;
        --variant)
            VARIANT="$2"
            shift 2
            ;;
        *)
            ARGS+=("$1")
            shift
            ;;
    esac
done

# Réinsérer les arguments sans --lang et --board
set -- "${ARGS[@]}"

# Définir le nom du sketch APRÈS le parsing des arguments
SKETCH_NAME="${2:-$DEFAULT_SKETCH}"  # Utiliser le 2ème argument ou le défaut
SKETCH_PATH="$ARDUINO_LIB_DIR/examples/$SKETCH_NAME/$SKETCH_NAME.ino"

# Fonction pour forcer un sketch
set_sketch() {
    SKETCH_NAME="$1"
    SKETCH_PATH="$ARDUINO_LIB_DIR/examples/$SKETCH_NAME/$SKETCH_NAME.ino"
}

# Validation de la langue
if [ "$LANG_CODE" != "fr" ] && [ "$LANG_CODE" != "en" ]; then
    echo "⚠️  Langue non supportée: $LANG_CODE, utilisation de 'fr' par défaut"
    LANG_CODE="fr"
fi

# Validation et configuration du board
case "$BOARD_TYPE" in
    c3|C3)
        BOARD="esp32:esp32:XIAO_ESP32C3"
        BOARD_TYPE="c3"
        ;;
    s3|S3)
        BOARD="esp32:esp32:XIAO_ESP32S3:PSRAM=opi"
        BOARD_TYPE="s3"
        ;;
    *)
        echo "⚠️  Board non supporté: $BOARD_TYPE, utilisation de 's3' par défaut"
        BOARD="esp32:esp32:XIAO_ESP32S3:PSRAM=opi"
        BOARD_TYPE="s3"
        ;;
esac

# C3 : activer --large-app par défaut (partition ~4 Mo) sauf si --no-large-app
if [[ "$BOARD" == *"XIAO_ESP32C3"* ]] && [ "$NO_LARGE_APP" != true ]; then
    LARGE_APP=true
fi

# Mode USB-MIDI et build properties USB S3 (calculés après parsing ; --variant prioritaire)
_usb_midi_flag=$(grep -E '^\s*#define\s+NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME\s+' "$REPO_DIR/src/network/UsbMidiManager.h" 2>/dev/null | tail -1 | awk '{print $3}')
if [ -n "$VARIANT" ]; then
    case "$VARIANT" in
        off|OFF) _usb_midi_flag=0 ;;
        on|ON)   _usb_midi_flag=1 ;;
        *) echo "❌ --variant inconnu: '$VARIANT' (attendu: off | on)"; exit 1 ;;
    esac
    # Le header utilise #ifndef -> ce -D l'emporte sur sa valeur par défaut, sans éditer le fichier.
    USB_MIDI_DEFINE=("-DNIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME=$_usb_midi_flag")
fi
if [ "$_usb_midi_flag" = "1" ]; then
    # USB-MIDI activé : USB-OTG (TinyUSB), le firmware contrôle la pile USB (MIDI USB).
    S3_USB_PROPS=( --build-property "build.usb_mode=0" --build-property "build.cdc_on_boot=0" )
else
    # USB-MIDI désactivé : usb_mode=1 (Hardware CDC and JTAG) = port USB-Serial-JTAG matériel,
    # stable, toujours présent, survit aux crashs (détection fiable). cdc_on_boot=1 -> Serial sur JTAG.
    S3_USB_PROPS=( --build-property "build.usb_mode=1" --build-property "build.cdc_on_boot=1" )
fi

# Export pour build_html_simple.sh
export LANG_CODE

# Fonction d'aide
show_help() {
    echo "🚀 NiDMI - Script unifié"
    echo "=============================="
    echo ""
    echo "Usage: ./scripts/nidmi.sh [OPTION] [SKETCH] [--lang LANG] [--board BOARD] [--port DEVICE]"
    echo ""
    echo "Options:"
    echo "  sync     - Synchroniser les fichiers seulement"
    echo "  compile  - Synchroniser + compiler"
    echo "  build    - Synchroniser + compiler + stocker le binaire"
    echo "  build-all- [S3] Builder toutes les variantes (usbmidi off + on) pour une release"
    echo "  upload   - Synchroniser + compiler + uploader"
    echo "  flash    - Uploader sans recompiler (binaire déjà compilé pour ce sketch + board)"
    echo "  monitor  - Ouvrir le moniteur série"
    echo "  erase-nvs - Effacer la région NVS via esptool (0x9000, 20 Ko ; voir doc si partition Arduino différente)"
    echo "  all      - Tout faire (sync + compile + upload + test)"
    echo "  clean    - Nettoyer le cache seulement"
    echo "  help     - Afficher cette aide"
    echo ""
    echo "Options:"
    echo "  --lang LANG     - Langue de l'interface web (fr|en, défaut: fr)"
    echo "                    Nécessite jq installé (brew install jq sur macOS)"
    echo "  --large-app     - [C3] Forcer la partition sans SPIFFS (app ~4 Mo)"
    echo "  --no-large-app  - [C3] Désactiver la partition agrandie (défaut: activée pour C3)"
    echo "  --split-fs      - [C3/S3] 2 partitions LittleFS dédiées (seqfs + mapfs)"
    echo "  --ota           - [S3] Partition OTA (app0/app1 + seqfs + mapfs) : MAJ firmware par Wi-Fi"
    echo "  --variant V     - [S3] Force la variante USB-MIDI au build (off|on) sans éditer le header :"
    echo "                    off = USB-MIDI désactivé, série stable (HW CDC/JTAG) ; on = MIDI USB (OTG)"
    echo "  --board BOARD - Type de carte ESP32 (c3|s3, défaut: s3)"
    echo "                  c3 = XIAO ESP32-C3"
    echo "                  s3 = XIAO ESP32-S3"
    echo "  --port DEVICE - Port série explicite (ex: /dev/ttyUSB0, /dev/ttyACM0, /dev/cu.usbmodem*)"
    echo "  --clear-nvs   - Utiliser le sketch de reset NVS"
    echo "  --light          - Mode LIGHT: définitions simplifiées (réduit la taille JSON)"
    echo "  --pagination     - Activer la pagination (défaut: activée)"
    echo "  --no-pagination  - Désactiver la pagination (à utiliser seulement avec --large-app sur C3)"
    echo "  --port PORT      - Forcer le port série (ex: /dev/cu.usbmodem101)"
    echo ""
    echo "Variables d'environnement (optionnel) :"
    echo "  ARDUINO_LIB_DIR   Bibliothèque NiDMI (défaut: \$HOME/Documents/Arduino/libraries/NiDMI)"
    echo "  ARDUINO_CACHE_DIR Cache sketches Arduino (défaut: macOS ~/Library/Caches/... ; Linux ~/.cache/...)"
    echo ""
    echo "  Pagination : activée par défaut (C3 et S3). Évite la troncature du JSON des définitions."
    echo "  Partition C3 : --large-app est activé par défaut pour C3 (partition ~4 Mo). Utiliser --no-large-app pour désactiver."
    echo "  Split FS : optionnel via --split-fs (remplace la partition standard par app0 + seqfs + mapfs)."
    echo ""
    echo "Sketches disponibles:"
    echo "  nidmi_basic (défaut)"
    echo "  nidmi_clear_nvs (reset NVS)"
    echo ""
    echo "Exemples:"
    echo "  ./scripts/nidmi.sh sync                    # Synchroniser (français, S3 par défaut)"
    echo "  ./scripts/nidmi.sh sync --lang en          # Synchroniser en anglais"
    echo "  ./scripts/nidmi.sh compile --board c3      # C3 : pagination + partition 4 Mo par défaut"
    echo "  ./scripts/nidmi.sh compile --board c3 --no-large-app   # C3 sans partition agrandie"
    echo "  ./scripts/nidmi.sh compile --board c3 --split-fs       # C3 avec seqfs 128KB + mapfs 128KB"
    echo "  ./scripts/nidmi.sh compile --board s3 --split-fs       # S3 avec seqfs 512KB + mapfs 1MB"
    echo "  ./scripts/nidmi.sh compile --board s3      # Compiler pour ESP32-S3"
    echo "  ./scripts/nidmi.sh upload --board s3       # Uploader sur ESP32-S3"
    echo "  ./scripts/nidmi.sh flash --board c3        # Reflash rapide (même build qu’après compile)"
    echo "  ./scripts/nidmi.sh upload --port /dev/ttyUSB0  # Uploader en forçant le port"
    echo "  ./scripts/nidmi.sh build                   # Build (S3 par défaut)"
    echo "  ./scripts/nidmi.sh upload nidmi_osc        # Upload sketch OSC"
    echo "  ./scripts/nidmi.sh upload --clear-nvs      # Upload sketch reset NVS"
    echo "  ./scripts/nidmi.sh upload --clear-nvs --split-fs   # Idem, même partition que split-fs"
    echo "  ./scripts/nidmi.sh erase-nvs --port /dev/cu.usbmodem1101   # NVS via esptool (PC)"
    echo "  ./scripts/nidmi.sh monitor                 # Ouvrir le moniteur série"
    echo "  ./scripts/nidmi.sh all                     # Tout faire + test"
    echo "  ./scripts/nidmi.sh clean                   # Nettoyer le cache"
    echo ""
}

# Fonction de synchronisation
# ── L'app embarquee est-elle EN RETARD sur le depot ? ────────────────────────
#
# L'app web voyage DANS le binaire (src/ui/app_archive.cpp, genere par
# scripts/cartes/embarquer-app.py du depot nidmi). Rien ne le rappelait : on
# pouvait modifier l'interface, construire, flasher — et la carte servait l'app
# d'avant, sans le moindre signe. Constate : archive.js en 404 sur la carte
# alors qu'il existait depuis des heures dans le depot.
#
# On ne construit donc plus sans le dire. AVERTISSEMENT, pas refus : construire
# le firmware seul, sans toucher a l'UI, est legitime.
verifier_app_embarquee() {
    local nidmi="${NIDMI_APP_DIR:-$HOME/Documents/OSC CRReaM/nidmi}"
    local arch="$REPO_DIR/src/ui/app_archive.cpp"
    [ -f "$arch" ] || return 0
    [ -d "$nidmi" ] || return 0
    local recents
    recents=$(find "$nidmi/nidmi.html" "$nidmi/js" "$nidmi/css" \
                   -type f -newer "$arch" 2>/dev/null | grep -v "/\." | head -20)
    [ -z "$recents" ] && return 0
    echo ""
    echo "⚠️  L'APP EMBARQUÉE EST EN RETARD sur le dépôt :"
    echo "$recents" | sed "s|$nidmi/|      |"
    echo "   La carte servirait cette version-là, sans rien dire."
    echo "   Régénérer :  python3 '$nidmi/scripts/cartes/embarquer-app.py'"
    echo ""
}

# Le vocabulaire que la carte PUBLIE (/api/mapping/vocabulaire) est derive de
# MappingEngine.cpp. Ajouter un verbe sans regenerer, c'est livrer une carte qui
# execute l'objet mais le declare inconnu — et l'app, qui la croit sur parole,
# le colorierait en rouge. On regenere donc AVANT chaque build, et on le DIT
# quand ca change : c'est le signe qu'un verbe est arrive depuis la derniere fois.
generer_vocabulaire() {
    local entete="$REPO_DIR/src/mapping/VocabulaireEmbarque.h"
    local avant=""
    [ -f "$entete" ] && avant=$(cat "$entete")
    python3 "$REPO_DIR/scripts/generer-vocabulaire.py" || return 0
    if [ "$avant" != "$(cat "$entete")" ]; then
        echo "   ↻ vocabulaire embarque mis a jour — a commiter avec le moteur"
    fi
}

sync_files() {
    echo "🔄 Synchronisation des fichiers..."
    echo "   📁 Source: $REPO_DIR"
    echo "   📁 Destination: $ARDUINO_LIB_DIR"
    echo "   🌐 Langue: $LANG_CODE"
    
    # Vérifier si jq est installé (nécessaire pour les traductions)
    if [ "$LANG_CODE" != "fr" ] && ! command -v jq &> /dev/null; then
        echo "   ⚠️  jq non trouvé, impossible d'utiliser la langue $LANG_CODE"
        echo "   📝 Installation: brew install jq (macOS) ou sudo apt-get install jq (Linux)"
        echo "   📝 Utilisation du français par défaut"
        export LANG_CODE="fr"
    fi
    
    # Minifier l'UI avant la synchronisation
    if [ -f "$REPO_DIR/scripts/build_html_simple.sh" ]; then
        echo "   🗜️  Minification de l'UI..."
        cd "$REPO_DIR"
        ./scripts/build_html_simple.sh
        echo "   ✅ UI minifiée"
    else
        echo "   ⚠️  Script de minification non trouvé, synchronisation sans minification"
    fi

    # Générer l'en-tête de version (gravé dans le firmware, exposé par /api/status).
    # Version = git describe (ou date si hors git) ; variante = selon le flag USB-MIDI courant.
    _fw_ver=$(cd "$REPO_DIR" && git describe --tags --always --dirty 2>/dev/null || date +%Y%m%d-%H%M%S)
    _fw_variant=$([ "${_usb_midi_flag:-0}" = "1" ] && echo "usbmidi-on" || echo "usbmidi-off")
    printf '#pragma once\n#define NIDMI_FW_VERSION "%s"\n#define NIDMI_FW_VARIANT "%s"\n' "$_fw_ver" "$_fw_variant" > "$REPO_DIR/src/nidmi_fw_version.h"
    echo "   🏷️  Version firmware: $_fw_ver ($_fw_variant)"

    generer_vocabulaire

    # Nettoyer les anciens fichiers source (pour éviter les conflits après réorganisation)
    rm -rf $ARDUINO_LIB_DIR/src/* 2>/dev/null || true
    
    # Créer le dossier src/ et tous les sous-dossiers
    # ── Copie RECURSIVE de src/ ────────────────────────────────────────────
    # Avant : une liste de dossiers ecrite a la main (mkdir enumerant 30
    # chemins, puis un « cp » par dossier). Tout NOUVEAU dossier etait donc
    # invisible au build, EN SILENCE — vecu en ajoutant src/components/audio et
    # src/managers/complex/dac : la compilation echouait sur un « No such file
    # or directory » pointant la bibliotheque Arduino, pas le depot, ce qui
    # envoie chercher au mauvais endroit.
    # Une copie recursive n'a pas de liste a tenir a jour.
    mkdir -p "$ARDUINO_LIB_DIR/src"
    ( cd "$REPO_DIR/src" && find . -type d -exec mkdir -p "$ARDUINO_LIB_DIR/src/{}" \; )
    ( cd "$REPO_DIR/src" && find . -type f \( -name '*.h' -o -name '*.cpp' -o -name '*.c' -o -name '*.hpp' \) \
        -exec cp -f {} "$ARDUINO_LIB_DIR/src/{}" \; )

    # Copier les exemples
    mkdir -p $ARDUINO_LIB_DIR/examples
    cp -rf $REPO_DIR/examples/* $ARDUINO_LIB_DIR/examples/ 2>/dev/null || true
    
    echo "   ✅ Fichiers synchronisés"
}

# Fonction de nettoyage
clean_cache() {
    echo "🧹 Nettoyage du cache Arduino..."
    
    # Nettoyer le cache des sketches
    if [ -d "$ARDUINO_CACHE_DIR" ]; then
        rm -rf $ARDUINO_CACHE_DIR/*
        echo "   ✅ Cache sketches nettoyé: $ARDUINO_CACHE_DIR"
    fi
    
    # Nettoyer les bibliothèques staging (copies temporaires Arduino)
    if [ -z "${ARDUINO_STAGING_DIR:-}" ]; then
        case "$PLATFORM" in
            mac)
                ARDUINO_STAGING_DIR="$HOME/Library/Arduino15/staging/libraries"
                ;;
            wsl|linux)
                ARDUINO_STAGING_DIR="$HOME/.arduino15/staging/libraries"
                ;;
            *)
                ARDUINO_STAGING_DIR="$HOME/.arduino15/staging/libraries"
                ;;
        esac
    fi
    if [ -d "$ARDUINO_STAGING_DIR" ]; then
        rm -rf "$ARDUINO_STAGING_DIR"/* 2>/dev/null || true
        echo "   ✅ Bibliothèques staging nettoyées: $ARDUINO_STAGING_DIR"
    fi
    
    # Nettoyer les dossiers build et bin dans la bibliothèque Arduino
    if [ -d "$ARDUINO_LIB_DIR" ]; then
        if [ -d "$ARDUINO_LIB_DIR/build" ]; then
            rm -rf "$ARDUINO_LIB_DIR/build"
            echo "   ✅ Dossier build supprimé: $ARDUINO_LIB_DIR/build"
        fi
        if [ -d "$ARDUINO_LIB_DIR/bin" ]; then
            rm -rf "$ARDUINO_LIB_DIR/bin"
            echo "   ✅ Dossier bin supprimé: $ARDUINO_LIB_DIR/bin"
        fi
    fi
    
    echo "   ✅ Cache Arduino nettoyé"
}

# Copie un CSV de partitions dans le package ESP32 Arduino (Arduino15)
install_partition_csv() {
    local SRC="$1"
    local DST_NAME="$2"
    local PKG_BASE=""
    local DST_DIR=""

    if [ -d "$HOME/Library/Arduino15/packages/esp32/hardware/esp32" ]; then
        PKG_BASE="$HOME/Library/Arduino15/packages/esp32/hardware/esp32"
    elif [ -d "$HOME/.arduino15/packages/esp32/hardware/esp32" ]; then
        PKG_BASE="$HOME/.arduino15/packages/esp32/hardware/esp32"
    fi

    if [ -n "$PKG_BASE" ]; then
        DST_DIR=$(ls -d "$PKG_BASE"/[0-9]*.[0-9]*.[0-9]*/tools/partitions 2>/dev/null | tail -1)
    fi

    if [ -z "$PKG_BASE" ] || [ -z "$DST_DIR" ]; then
        echo "   ⚠️  Package ESP32 non trouvé (Arduino15), installation partition ignorée"
        return 1
    fi

    if [ ! -f "$SRC" ]; then
        echo "   ⚠️  Fichier partition manquant: $SRC, installation ignorée"
        return 1
    fi

    cp "$SRC" "$DST_DIR/$DST_NAME"
    echo "   📦 Partition installée: $DST_DIR/$DST_NAME"
    return 0
}

# Copie la partition C3 sans SPIFFS dans le package Arduino (pour --large-app)
setup_c3_large_app_partition() {
    install_partition_csv "$REPO_DIR/tools/nidmi_c3_no_spiffs.csv" "nidmi_c3_no_spiffs.csv"
}

# Copie les partitions split-fs (2x LittleFS dédiés) pour C3/S3
setup_split_fs_partition() {
    if [[ "$BOARD" == *"XIAO_ESP32C3"* ]]; then
        install_partition_csv "$REPO_DIR/tools/nidmi_c3_dual_littlefs.csv" "nidmi_c3_dual_littlefs.csv"
    elif [[ "$BOARD" == *"XIAO_ESP32S3"* ]]; then
        install_partition_csv "$REPO_DIR/tools/nidmi_s3_dual_littlefs.csv" "nidmi_s3_dual_littlefs.csv"
    else
        echo "   ⚠️  --split-fs non supporté pour ce board: $BOARD"
        return 1
    fi
    return 0
}

# Copie la partition OTA S3 (2 slots app + seqfs + mapfs) pour la MAJ firmware par Wi-Fi
setup_ota_partition() {
    if [[ "$BOARD" == *"XIAO_ESP32S3"* ]]; then
        install_partition_csv "$REPO_DIR/tools/nidmi_s3_ota_dual_littlefs.csv" "nidmi_s3_ota_dual_littlefs.csv"
    else
        echo "   ⚠️  --ota non supporté pour ce board: $BOARD (OTA prévu pour S3 8 Mo ; le C3 n'a pas l'USB et est trop serré en 4 Mo)"
        return 1
    fi
    return 0
}

# Détecte si un port est le port JTAG hardware du S3 (nom contient la MAC, ex. usbmodem1020BA76E641)
# Retourne 0 (vrai) si c'est JTAG, 1 sinon
nidmi_port_is_jtag() {
    local p="$1"
    # Le port JTAG ESP32-S3 a toujours un suffixe hexadécimal long (≥10 chars hex, en majuscules)
    if echo "$p" | grep -qE 'usbmodem[0-9A-Fa-f]{10,}$'; then
        return 0
    fi
    return 1
}

# Sélection du port pour l'UPLOAD : préfère le port JTAG (stable même en boot loop)
nidmi_pick_upload_port() {
    if [ -n "$PORT_OVERRIDE" ]; then PORT="$PORT_OVERRIDE"; return 0; fi
    if [ -n "$NIDMI_PORT" ]; then PORT="$NIDMI_PORT"; return 0; fi
    local all_ports
    case "$PLATFORM" in
        mac)
            all_ports=$(ls /dev/cu.usbserial-* /dev/cu.usbmodem* /dev/cu.SLAB_USBtoUART* 2>/dev/null)
            ;;
        wsl|linux)
            all_ports=$(ls /dev/ttyUSB* /dev/ttyACM* /dev/ttyS* 2>/dev/null)
            ;;
    esac
    # Priorité 1 : port JTAG (stable même en boot loop)
    for p in $all_ports; do
        if nidmi_port_is_jtag "$p"; then PORT="$p"; break; fi
    done
    # Priorité 2 : premier port disponible
    if [ -z "$PORT" ]; then PORT=$(echo "$all_ports" | head -1); fi
    if [ -z "$PORT" ]; then
        echo "   ❌ Aucun port USB trouvé"
        echo "   📝 Ports (cu.*): $(ls /dev/cu.* 2>/dev/null | tr '\n' ' ')"
        echo "   📝 Utilisez --port ou NIDMI_PORT"
        return 1
    fi
    if nidmi_port_is_jtag "$PORT"; then
        echo "   ℹ️  Port JTAG hardware sélectionné (stable) : $PORT"
        echo "      (si le firmware est en boot loop, c'est normal — upload possible quand même)"
    fi
    return 0
}

# Sélection du port pour le MONITEUR : préfère le port CDC (là où Serial.println écrit)
nidmi_pick_monitor_port() {
    if [ -n "$PORT_OVERRIDE" ]; then PORT="$PORT_OVERRIDE"; return 0; fi
    if [ -n "$NIDMI_PORT" ]; then PORT="$NIDMI_PORT"; return 0; fi
    local all_ports
    case "$PLATFORM" in
        mac)
            all_ports=$(ls /dev/cu.usbserial-* /dev/cu.usbmodem* /dev/cu.SLAB_USBtoUART* 2>/dev/null)
            ;;
        wsl|linux)
            all_ports=$(ls /dev/ttyUSB* /dev/ttyACM* /dev/ttyS* 2>/dev/null)
            ;;
    esac
    # Priorité 1 : port CDC (nom court, PAS le JTAG avec MAC longue)
    for p in $all_ports; do
        if ! nidmi_port_is_jtag "$p"; then PORT="$p"; break; fi
    done
    # Priorité 2 : premier disponible (cas où seul le JTAG est présent)
    if [ -z "$PORT" ]; then
        PORT=$(echo "$all_ports" | head -1)
        if [ -n "$PORT" ]; then
            echo "   ⚠️  Seul le port JTAG est visible ($PORT)."
            echo "      Serial.println() sort sur le port CDC USB-OTG (cu.usbmodemXXX)."
            echo "      Si le firmware est en boot loop, uploadez d'abord le firmware fixé."
        fi
    fi
    if [ -z "$PORT" ]; then
        echo "   ❌ Aucun port USB trouvé"
        echo "   📝 Ports (cu.*): $(ls /dev/cu.* 2>/dev/null | tr '\n' ' ')"
        echo "   📝 Utilisez --port ou NIDMI_PORT"
        return 1
    fi
    return 0
}

# Port série générique (upload) — conservé pour erase-nvs et compatibilité
nidmi_pick_serial_port() {
    nidmi_pick_upload_port
}

# Après upload de nidmi_clear_nvs
nidmi_clear_nvs_upload_hint() {
    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "  Sketch nidmi_clear_nvs téléversé"
    echo "  • L’ESP redémarre et exécute nvs_flash_erase() (~1,5 s après boot)."
    echo "  • Si le moniteur USB reste vide : les logs peuvent être sur UART ;"
    echo "    l’effacement NVS s’exécute quand même dans setup()."
    echo "  • Ensuite : ./scripts/nidmi.sh upload  avec les mêmes options que d’habitude"
    echo "    (--board, --split-fs, --no-large-app, --port, etc.), sans --clear-nvs."
    echo "  • SSID toujours faux ?  ./scripts/nidmi.sh erase-nvs [--board …] [--port …]"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo ""
}

# Synchronise nidmi-core vers ~/Documents/Arduino/libraries/nidmi-core/
# Priorité : repo local frère → git pull du clone Arduino → avertissement.
sync_nidmi_core() {
    local REPO_SRC="$REPO_DIR/../nidmi-core"
    local ARDUINO_LIBS_BASE
    case "$PLATFORM" in
        mac)  ARDUINO_LIBS_BASE="$HOME/Documents/Arduino/libraries" ;;
        *)    ARDUINO_LIBS_BASE="$HOME/Arduino/libraries" ;;
    esac
    local DEST="$ARDUINO_LIBS_BASE/nidmi-core"

    if [ -d "$REPO_SRC/src" ]; then
        mkdir -p "$DEST/src"
        cp -r "$REPO_SRC/src/"* "$DEST/src/"
        cp "$REPO_SRC/library.properties" "$DEST/" 2>/dev/null || true
        echo "   ✅ nidmi-core synced depuis $REPO_SRC"
    elif [ -d "$DEST/.git" ]; then
        git -C "$DEST" pull --ff-only --quiet && echo "   ✅ nidmi-core updated (git pull)" || echo "   ⚠️  nidmi-core git pull échoué (merge conflict ?)"
    else
        echo "   ⚠️  nidmi-core introuvable. Lancer ./scripts/install_deps.sh" >&2
    fi
}

# Fonction de compilation
compile_sketch() {
    echo "🔨 Compilation du sketch..."
    echo "   📁 Sketch: $SKETCH_PATH"
    echo "   📋 Board: $BOARD"
    
    if [ "$LIGHT_MODE" = true ]; then
        echo "   💡 Mode LIGHT activé (définitions simplifiées)"
    fi
    
    if [ "$PAGINATION_MODE" = true ]; then
        echo "   📄 Mode PAGINATION activé (chargement par pages)"
    fi
    
    if [ "$OTA_MODE" = true ] && [[ "$BOARD" == *"XIAO_ESP32S3"* ]]; then
        echo "   🔁 Mode OTA activé (S3: app0/app1 + seqfs + mapfs ; MAJ firmware par Wi-Fi)"
        setup_ota_partition || true
    elif [ "$SPLIT_FS" = true ]; then
        echo "   💾 Mode SPLIT-FS activé (seqfs + mapfs en LittleFS dédiés)"
        setup_split_fs_partition || true
    elif [ "$LARGE_APP" = true ] && [[ "$BOARD" == *"XIAO_ESP32C3"* ]]; then
        echo "   📦 Mode LARGE-APP activé (partition C3 sans SPIFFS, app ~4 Mo)"
        setup_c3_large_app_partition || true
    fi

    sync_nidmi_core

    if command -v arduino-cli &> /dev/null; then
        echo "   Utilisation d'arduino-cli..."
        
        # Construire les flags de build
        EXTRA_FLAGS_ARRAY=()
        
        if [ "$LIGHT_MODE" = true ]; then
            EXTRA_FLAGS_ARRAY+=("-DNIDMI_COMPONENT_DEFS_LIGHT")
        fi
        
        if [ "$PAGINATION_MODE" = true ]; then
            EXTRA_FLAGS_ARRAY+=("-DNIDMI_COMPONENT_DEFS_PAGINATION")
        fi

        # --variant : forcer le flag USB-MIDI au build (sans éditer le header)
        if [ ${#USB_MIDI_DEFINE[@]} -gt 0 ]; then
            EXTRA_FLAGS_ARRAY+=("${USB_MIDI_DEFINE[@]}")
        fi

        # Audio : -O3 est OBLIGATOIRE pour le DSP (le core compile en -Os, soit
        # un facteur 1,11 mesuré sur Plaits — le 1,82 du banc de biquads
        # sur-évaluait, voir MESURES.md §18) ; -DTEST bascule stmlib sur ses
        # versions portables de Clip16/ClipU16/Sqrt, les originales étant de
        # l'assembleur ARM. Vérifié : aucune collision sur TEST dans src/.
        if true; then
            EXTRA_FLAGS_ARRAY+=("-O3" "-DTEST")
        fi

        # Echappatoire generique : le contenu de NIDMI_EXTRA_FLAGS est ajoute aux
        # flags C++. Il est donc vu AUSSI par les bibliotheques, puisque
        # compiler.cpp.extra_flags les atteint (MESURES.md §18).
        #   NIDMI_EXTRA_FLAGS="-DPLAITS_LEGER" ./scripts/nidmi.sh build --board s3 …
        if [ -n "${NIDMI_EXTRA_FLAGS:-}" ]; then
            # Decoupage voulu sur les espaces : plusieurs drapeaux possibles.
            # shellcheck disable=SC2206
            EXTRA_FLAGS_ARRAY+=($NIDMI_EXTRA_FLAGS)
        fi

        
        # Build properties (flags + partition C3 si --large-app)
        BUILD_PROPS=()
        if [[ "$BOARD" == *"XIAO_ESP32S3"* ]]; then
            BUILD_PROPS+=("${S3_USB_PROPS[@]}")
        fi
        if [ ${#EXTRA_FLAGS_ARRAY[@]} -gt 0 ]; then
            BUILD_PROPS+=(--build-property "compiler.cpp.extra_flags=${EXTRA_FLAGS_ARRAY[*]}")
        fi
        if [ "$OTA_MODE" = true ] && [[ "$BOARD" == *"XIAO_ESP32S3"* ]]; then
            BUILD_PROPS+=(--build-property "build.partitions=nidmi_s3_ota_dual_littlefs")
            BUILD_PROPS+=(--build-property "upload.maximum_size=3342336")
        elif [ "$SPLIT_FS" = true ] && [[ "$BOARD" == *"XIAO_ESP32C3"* ]]; then
            BUILD_PROPS+=(--build-property "build.partitions=nidmi_c3_dual_littlefs")
            BUILD_PROPS+=(--build-property "upload.maximum_size=3801088")
        elif [ "$SPLIT_FS" = true ] && [[ "$BOARD" == *"XIAO_ESP32S3"* ]]; then
            BUILD_PROPS+=(--build-property "build.partitions=nidmi_s3_dual_littlefs")
            BUILD_PROPS+=(--build-property "upload.maximum_size=6684672")
        elif [ "$LARGE_APP" = true ] && [[ "$BOARD" == *"XIAO_ESP32C3"* ]]; then
            BUILD_PROPS+=(--build-property "build.partitions=nidmi_c3_no_spiffs")
            BUILD_PROPS+=(--build-property "upload.maximum_size=4063232")
        fi
        
        arduino-cli compile --fqbn $BOARD "${BUILD_PROPS[@]}" --library "$REPO_DIR/../nidmi-core" $SKETCH_PATH
        echo "   ✅ Compilation réussie"
    else
        echo "   ⚠️  arduino-cli non trouvé"
        echo "   📝 Veuillez compiler manuellement dans l'IDE Arduino"
        echo "   📝 Sketch: $SKETCH_PATH"
        echo "   📝 Board: $BOARD"
    fi
}

# Fonction de build (compilation + stockage binaire)
build_binary() {
    echo "🔨 Compilation et stockage du binaire..."
    echo "   📁 Sketch: $SKETCH_PATH"
    echo "   📋 Board: $BOARD"
    echo "   📦 Dossier de sortie: $REPO_DIR/bin"
    
    if [ "$LIGHT_MODE" = true ]; then
        echo "   💡 Mode LIGHT activé (définitions simplifiées)"
    fi
    
    if [ "$PAGINATION_MODE" = true ]; then
        echo "   📄 Mode PAGINATION activé (chargement par pages)"
    fi
    
    # Créer le dossier bin s'il n'existe pas
    mkdir -p "$REPO_DIR/bin"
    
    if command -v arduino-cli &> /dev/null; then
        echo "   Utilisation d'arduino-cli..."
        
        if [ "$OTA_MODE" = true ] && [[ "$BOARD" == *"XIAO_ESP32S3"* ]]; then
            echo "   🔁 Mode OTA activé (S3: app0/app1 + seqfs + mapfs ; MAJ firmware par Wi-Fi)"
            setup_ota_partition || true
        elif [ "$SPLIT_FS" = true ]; then
            echo "   💾 Mode SPLIT-FS activé (seqfs + mapfs en LittleFS dédiés)"
            setup_split_fs_partition || true
        elif [ "$LARGE_APP" = true ] && [[ "$BOARD" == *"XIAO_ESP32C3"* ]]; then
            echo "   📦 Mode LARGE-APP activé (partition C3 sans SPIFFS)"
            setup_c3_large_app_partition || true
        fi
        
        # Construire les flags de build (même logique que compile_sketch)
        EXTRA_FLAGS_ARRAY=()
        
        if [ "$LIGHT_MODE" = true ]; then
            EXTRA_FLAGS_ARRAY+=("-DNIDMI_COMPONENT_DEFS_LIGHT")
        fi
        
        if [ "$PAGINATION_MODE" = true ]; then
            EXTRA_FLAGS_ARRAY+=("-DNIDMI_COMPONENT_DEFS_PAGINATION")
        fi

        # --variant : forcer le flag USB-MIDI au build (sans éditer le header)
        if [ ${#USB_MIDI_DEFINE[@]} -gt 0 ]; then
            EXTRA_FLAGS_ARRAY+=("${USB_MIDI_DEFINE[@]}")
        fi

        # Audio : -O3 est OBLIGATOIRE pour le DSP (le core compile en -Os, soit
        # un facteur 1,11 mesuré sur Plaits — le 1,82 du banc de biquads
        # sur-évaluait, voir MESURES.md §18) ; -DTEST bascule stmlib sur ses
        # versions portables de Clip16/ClipU16/Sqrt, les originales étant de
        # l'assembleur ARM. Vérifié : aucune collision sur TEST dans src/.
        if true; then
            EXTRA_FLAGS_ARRAY+=("-O3" "-DTEST")
        fi

        # Echappatoire generique : le contenu de NIDMI_EXTRA_FLAGS est ajoute aux
        # flags C++. Il est donc vu AUSSI par les bibliotheques, puisque
        # compiler.cpp.extra_flags les atteint (MESURES.md §18).
        #   NIDMI_EXTRA_FLAGS="-DPLAITS_LEGER" ./scripts/nidmi.sh build --board s3 …
        if [ -n "${NIDMI_EXTRA_FLAGS:-}" ]; then
            # Decoupage voulu sur les espaces : plusieurs drapeaux possibles.
            # shellcheck disable=SC2206
            EXTRA_FLAGS_ARRAY+=($NIDMI_EXTRA_FLAGS)
        fi

        BUILD_PROPS=()
        if [[ "$BOARD" == *"XIAO_ESP32S3"* ]]; then
            BUILD_PROPS+=("${S3_USB_PROPS[@]}")
        fi
        if [ ${#EXTRA_FLAGS_ARRAY[@]} -gt 0 ]; then
            BUILD_PROPS+=(--build-property "compiler.cpp.extra_flags=${EXTRA_FLAGS_ARRAY[*]}")
        fi
        if [ "$OTA_MODE" = true ] && [[ "$BOARD" == *"XIAO_ESP32S3"* ]]; then
            BUILD_PROPS+=(--build-property "build.partitions=nidmi_s3_ota_dual_littlefs")
            BUILD_PROPS+=(--build-property "upload.maximum_size=3342336")
        elif [ "$SPLIT_FS" = true ] && [[ "$BOARD" == *"XIAO_ESP32C3"* ]]; then
            BUILD_PROPS+=(--build-property "build.partitions=nidmi_c3_dual_littlefs")
            BUILD_PROPS+=(--build-property "upload.maximum_size=3801088")
        elif [ "$SPLIT_FS" = true ] && [[ "$BOARD" == *"XIAO_ESP32S3"* ]]; then
            BUILD_PROPS+=(--build-property "build.partitions=nidmi_s3_dual_littlefs")
            BUILD_PROPS+=(--build-property "upload.maximum_size=6684672")
        elif [ "$LARGE_APP" = true ] && [[ "$BOARD" == *"XIAO_ESP32C3"* ]]; then
            BUILD_PROPS+=(--build-property "build.partitions=nidmi_c3_no_spiffs")
            BUILD_PROPS+=(--build-property "upload.maximum_size=4063232")
        fi
        
        arduino-cli compile --fqbn "$BOARD" --output-dir "$REPO_DIR/bin" "${BUILD_PROPS[@]}" --library "$REPO_DIR/../nidmi-core" "$SKETCH_PATH"
        echo "   ✅ Binaire compilé et stocké dans bin/"
        # --variant : étiqueter les binaires pour distinguer off/on
        #   .bin        = image applicative (pour l'OTA, écrit dans le slot app)
        #   .merged.bin = bootloader + partitions + app (pour ESP Web Tools / flash complet)
        if [ -n "$VARIANT" ]; then
            _vlabel="nidmi-${BOARD_TYPE}-usbmidi-${VARIANT}"
            [ -f "$REPO_DIR/bin/$SKETCH_NAME.ino.bin" ] && cp -f "$REPO_DIR/bin/$SKETCH_NAME.ino.bin" "$REPO_DIR/bin/${_vlabel}.bin"
            [ -f "$REPO_DIR/bin/$SKETCH_NAME.ino.merged.bin" ] && cp -f "$REPO_DIR/bin/$SKETCH_NAME.ino.merged.bin" "$REPO_DIR/bin/${_vlabel}.merged.bin"
            echo "   🏷️  Variante '$VARIANT' -> bin/${_vlabel}.bin (OTA) + bin/${_vlabel}.merged.bin (Web Tools)"
        fi
        echo "   📁 Fichiers créés:"
        ls -la "$REPO_DIR/bin/" 2>/dev/null || echo "   📁 Aucun fichier trouvé"
    else
        echo "   ⚠️  arduino-cli non trouvé"
        echo "   📝 Veuillez compiler manuellement dans l'IDE Arduino"
        echo "   📝 Sketch: $SKETCH_PATH"
        echo "   📝 Board: $BOARD"
    fi
}

# Fonction de moniteur série
monitor_serial() {
    echo "📺 Ouverture du moniteur série..."
    echo "   ℹ️  Serial.println() sort sur le port CDC USB-OTG (cu.usbmodemXXX court)."
    echo "      Le port JTAG hardware (cu.usbmodemXXXXXXXXXX long) ne montre pas les logs."
    
    if ! nidmi_pick_monitor_port; then
        exit 1
    fi
    
    echo "   📡 Port: $PORT"
    
    # Utiliser arduino-cli monitor si disponible
    if command -v arduino-cli &> /dev/null; then
        echo "   📺 Utilisation d'arduino-cli monitor..."
        echo "   📝 Pour quitter: Ctrl+C"
        echo "   📝 Pour envoyer des commandes: tapez directement"
        echo "   📝 Appuyez sur RESET de l'ESP32 pour voir les logs de démarrage"
        echo ""
        
        # Vérifier que le port est accessible
        if [ ! -r "$PORT" ]; then
            echo "   ❌ Port $PORT non accessible en lecture"
            echo "   📝 Essayez avec sudo ou vérifiez les permissions"
            exit 1
        fi
        
        # Configuration du moniteur avec arduino-cli et options robustes
        echo "   🔧 Lancement du moniteur série..."
        arduino-cli monitor -p "$PORT" \
            -c baudrate=115200
    else
        echo "   ⚠️  arduino-cli non trouvé"
        echo "   📝 Installez arduino-cli ou utilisez l'IDE Arduino"
        echo "   📝 Moniteur série: Outils → Moniteur série"
        exit 1
    fi
}

# Fonction d'upload
upload_sketch() {
    echo "📤 Upload vers l'ESP32..."
    
    if command -v arduino-cli &> /dev/null; then
        if ! nidmi_pick_upload_port; then
            exit 1
        fi
        
        echo "   📡 Port: $PORT"
        arduino-cli upload -p $PORT --fqbn $BOARD $SKETCH_PATH
        echo "   ✅ Upload réussi"
    else
        echo "   ⚠️  arduino-cli non trouvé"
        echo "   📝 Veuillez uploader manuellement dans l'IDE Arduino"
    fi
}

# Fonction principale
main() {
    if [ "$CLEAR_NVS" = true ]; then
        set_sketch "$NVS_RESET_SKETCH"
        echo ""
        if [ "$SPLIT_FS" = true ]; then
            echo "⚠️  --split-fs actif : pour reflasher nidmi_basic, garde aussi --split-fs (même table de partitions / même zone NVS)."
        else
            echo "ℹ️  Sans --split-fs, nidmi_clear_nvs et nidmi_basic utilisent le même profil de partition (défaut Arduino sur S3 ;"
            echo "   sur C3, --large-app du script s’applique aux deux sauf si tu passes --no-large-app)."
        fi
        echo "   Si le SSID ne redevient pas « nidmi » : ./scripts/nidmi.sh erase-nvs --port … , autre port USB, ou bootloader."
        echo ""
    fi
    case "${1:-help}" in
        "sync")
            echo "🚀 NiDMI - Synchronisation"
            echo "================================"
            sync_files
            clean_cache
            echo ""
            echo "✅ Synchronisation terminée !"
            echo "📝 Maintenant compile et upload dans l'IDE Arduino"
            ;;
               "compile")
                   echo "🚀 NiDMI - Synchronisation + Compilation"
                   echo "================================================"
                   sync_files
                   clean_cache
                   compile_sketch
                   echo ""
                   echo "✅ Compilation terminée !"
                   echo "📝 Maintenant upload dans l'IDE Arduino"
                   ;;
               "build")
                   echo "🚀 NiDMI - Synchronisation + Compilation + Stockage"
                   echo "========================================================"
                   verifier_app_embarquee
                   sync_files
                   clean_cache
                   build_binary
                   echo ""
                   echo "✅ Build terminé !"
                   echo "📦 Binaire stocké dans bin/"
                   ;;
               "build-all")
                   echo "🚀 NiDMI - Build de TOUTES les variantes S3 (release)"
                   echo "======================================================"
                   bash "$0" build --board s3 --ota --variant off
                   bash "$0" build --board s3 --ota --variant on
                   # Nettoyer les transitoires (régénérés à chaque build), ne garder que les étiquetés
                   rm -f "$REPO_DIR"/bin/nidmi_basic.ino.* "$REPO_DIR"/bin/nidmi_basic.uf2 2>/dev/null
                   echo ""
                   echo "✅ Variantes prêtes (transitoires nettoyés) :"
                   ls -la "$REPO_DIR"/bin/nidmi-*-usbmidi-*.bin "$REPO_DIR"/bin/nidmi-*-usbmidi-*.merged.bin 2>/dev/null
                   ;;
               "upload")
                   echo "🚀 NiDMI - Synchronisation + Compilation + Upload"
                   echo "====================================================="
                   sync_files
                   clean_cache
                   compile_sketch
                   upload_sketch
                   if [ "$CLEAR_NVS" = true ]; then
                       nidmi_clear_nvs_upload_hint
                   fi
                   echo ""
                   echo "🎉 Processus terminé !"
                   echo ""
                   echo "📋 Prochaines étapes:"
                   echo "   1. Ouvrir http://192.168.4.1 dans le navigateur"
                   echo "   2. Ouvrir la console du navigateur (F12)"
                   echo "   3. Tester le clic sur SDA"
                   echo "   4. Vérifier les logs dans la console"
                   ;;
               "flash")
                   echo "⚡ NiDMI - Upload seul (sans sync ni compilation)"
                   echo "=================================================="
                   echo "   Utilise le dernier binaire arduino-cli pour ce sketch et ce FQBN."
                   echo "   Faire au moins un compile ou upload une fois avant, avec les mêmes options (--board, --split-fs, etc.)."
                   if [ ! -f "$SKETCH_PATH" ]; then
                       echo "   ❌ Sketch introuvable: $SKETCH_PATH"
                       echo "   📝 Lance d’abord: ./scripts/nidmi.sh sync"
                       exit 1
                   fi
                   upload_sketch
                   echo ""
                   echo "✅ Flash terminé (firmware inchangé depuis la dernière compilation)."
                   ;;
               "monitor")
                   echo "🚀 NiDMI - Moniteur série"
                   echo "============================="
                   monitor_serial
                   ;;
               "erase-nvs")
                   echo "🔥 NiDMI - Effacement NVS (esptool erase_region)"
                   echo "   Région 0x9000, taille 0x5000 (20 Ko) — OK pour les CSV NiDMI split-fs et souvent pour le défaut XIAO."
                   echo "   Si ta partition « nvs » fait 24 Ko (0x6000) dans l’IDE, allonge manuellement la commande esptool."
                   echo ""
                   if ! nidmi_pick_serial_port; then
                       exit 1
                   fi
                   CHIP="esp32s3"
                   if [[ "$BOARD_TYPE" == "c3" ]]; then
                       CHIP="esp32c3"
                   fi
                   echo "   📡 Port: $PORT  Chip: $CHIP"
                   if python3 -m esptool version >/dev/null 2>&1; then
                       python3 -m esptool --chip "$CHIP" --port "$PORT" erase_region 0x9000 0x5000
                   elif command -v esptool.py &>/dev/null; then
                       esptool.py --chip "$CHIP" --port "$PORT" erase_region 0x9000 0x5000
                   else
                       echo "   ❌ esptool introuvable : pip install esptool"
                       exit 1
                   fi
                   echo ""
                   echo "✅ Région NVS effacée. Reflashez le firmware : ./scripts/nidmi.sh upload …"
                   ;;
        "all")
            echo "🚀 NiDMI - TOUT FAIRE (Sync + Compile + Upload + Test)"
            echo "========================================================="
            sync_files
            clean_cache
            compile_sketch
            upload_sketch
            if [ "$CLEAR_NVS" = true ]; then
                nidmi_clear_nvs_upload_hint
            fi
            echo ""
            echo "🎉 Processus terminé !"
            echo ""
            echo "🧪 Test automatique:"
            echo "   Attente de 5 secondes pour le démarrage de l'ESP32..."
            sleep 5
                   echo "   🌐 Ouverture automatique du navigateur (Firefox)..."
                   open -a Firefox http://192.168.4.1 2>/dev/null || echo "   ⚠️  Ouvrez manuellement http://192.168.4.1 dans Firefox"
            echo ""
            echo "📋 Instructions de test:"
            echo "   1. Ouvrir la console du navigateur (F12 → Console)"
            echo "   2. Rafraîchir la page (F5)"
            echo "   3. Cliquer sur SDA dans l'interface"
            echo "   4. Vérifier les logs dans la console"
            echo "   5. Les pins I2C devraient se griser automatiquement"
            ;;
        "clean")
            echo "🚀 NiDMI - Nettoyage du cache"
            echo "==================================="
            clean_cache
            echo ""
            echo "✅ Cache nettoyé !"
            ;;
        "help"|*)
            show_help
            ;;
    esac
}

# Exécution
main "$@"