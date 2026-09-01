// GÉNÉRÉ par scripts/cartes/embarquer-app.py (dépôt nidmi) — NE PAS ÉDITER.
// L'application web NiDMI embarquée : chaque entrée est servie telle quelle
// (gzip → Content-Encoding: gzip). Régénérer depuis le dépôt nidmi.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <pgmspace.h>

struct AppFile {
    const char*    chemin;   // chemin URL ("/css/theme.css")
    const uint8_t* donnees;
    size_t         taille;
    const char*    type;     // Content-Type
    bool           gz;
};

extern const AppFile APP_FILES[];
extern const size_t  APP_FILES_COUNT;
