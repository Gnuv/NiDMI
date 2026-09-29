# Outils NiDMI

## Partition ESP32-C3 sans SPIFFS (`nidmi_c3_no_spiffs.csv`)

Table de partitions pour **ESP32-C3 (4 Mo flash)** sans partition SPIFFS, avec une partition application agrandie (~4 Mo au lieu de ~1,25 Mo).

**Utilité :** évite d’atteindre 97 % du flash (firmware trop serré). Le projet n’utilise pas SPIFFS (HTML/JS en PROGMEM), donc supprimer SPIFFS est sans impact fonctionnel.

**Comportement par défaut :** pour le C3, le script active cette partition automatiquement. Pour désactiver : `--no-large-app`.

```bash
./scripts/nidmi.sh compile --board c3    # partition 4 Mo activée par défaut
./scripts/nidmi.sh compile --board c3 --no-large-app   # revenir à la partition standard
```

Le script copie ce fichier dans le package Arduino ESP32 (`tools/partitions/`) puis compile avec `build.partitions=nidmi_c3_no_spiffs` et `upload.maximum_size=4063232`.

## Partitions à fichiers dédiés : `storage` (MESURES §185)

Une partition LittleFS, `storage`, porte tous les fichiers de la carte :
`compositions/`, `interface/`, `samples/` (CONVERGENCE_NIDMI.md §9.2). Elle
remplace `seqfs` (jamais lue), `mapfs` et `coredump` (plus écrite depuis le
§163) ; le firmware la nomme une seule fois, dans `src/config/Stockage.h`.
`nvs`, `otadata`, `app0` et `app1` gardent les noms qu'ESP-IDF et Arduino
attendent.

- `nidmi_s3_ota.csv` (ESP32-S3 8MB, mises à jour par le réseau — la table de la carte)
  - `app0`, `app1` = 0x330000 chacun (3 342 336 bytes)
  - `storage` = 0x190000 (1,56 Mo)
- `nidmi_s3.csv` (ESP32-S3 8MB, un seul emplacement)
  - `app0` = 0x660000 (6 684 672 bytes)
  - `storage` = 0x190000 (1,56 Mo)
- `nidmi_c3.csv` (ESP32-C3 4MB)
  - `app0` = 0x3A0000 (3 801 088 bytes)
  - `storage` = 0x50000 (320 Ko)

Changer de table demande un flash par USB : elle vit à 0x8000, hors des
emplacements qu'une mise à jour par le réseau réécrit.

Activation via script:

```bash
./scripts/nidmi.sh compile --board c3 --split-fs
./scripts/nidmi.sh compile --board s3 --split-fs
```
