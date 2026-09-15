/*
 * SampleStore — les échantillons du boîtier : stockage flash, lecture PSRAM.
 *
 * Deux mémoires, deux rôles, et c'est le point du fichier :
 *
 *  - LittleFS sur la partition « mapfs » (1 Mo, déjà partitionnée mais JAMAIS
 *    MONTÉE jusqu'ici — §12.5 de CONVERGENCE_NIDMI.md). C'est la persistance :
 *    ce qu'on téléverse survit au redémarrage et à l'OTA.
 *
 *  - PSRAM pour la lecture. La carte en a 8,37 Mo entièrement inutilisés,
 *    pendant que le tas interne se bat pour 13 ko. La PSRAM est trop lente pour
 *    du DSP au coup par coup (MESURES.md §4), mais parfaitement adaptée à un
 *    échantillon lu séquentiellement : la tâche audio y puise bloc par bloc.
 *
 * On ne lit JAMAIS la flash depuis la tâche audio : un accès LittleFS peut
 * bloquer le temps qu'un cache s'aligne, et ça s'entendrait. Le fichier est
 * copié en PSRAM au chargement, une fois pour toutes.
 *
 * Coût en RAM interne : quelques centaines d'octets. À comparer aux 26 632 o de
 * Plaits — c'est ce qui permet au lecteur d'échantillons de cohabiter avec le
 * service de l'interface sans la dégrader.
 */
#pragma once
#include <Arduino.h>

namespace SampleStore {

// Montage paresseux de mapfs (formatage si vierge). Sûr à appeler souvent.
bool monter();
bool estMonte();

size_t espaceTotal();
size_t espaceUtilise();

// Liste JSON des échantillons présents : [{"name":…,"bytes":…}, …]
String listerJson();

// Écriture par fragments (le corps d'un POST arrive en morceaux).
bool ecrireDebut(const char* nom);
bool ecrireMorceau(const uint8_t* donnees, size_t taille);
bool ecrireFin();
void ecrireAbandon();

bool supprimer(const char* nom);

// ── Chargement en PSRAM : TOUS, UNE FOIS ───────────────────────────────────
//
// Le magasin ne tenait qu'UN échantillon : deux pistes avec deux sons étaient
// donc structurellement impossibles, le second chargement écrasant le premier.
//
// ET ON NE DÉCHARGE PLUS. La règle « un process absent de la cue est déchargé
// après le release » existe pour libérer une ressource RARE. Ici elle n'a rien
// à libérer, et la mesure le dit sans appel :
//
//     mapfs plafonne à 1 048 576 o — c'est TOUT ce que la carte peut stocker
//     PSRAM libre                   8 249 372 o
//     donc le pire cas absolu tient dans 12,7 % de la PSRAM
//
// Décharger ne rend donc rien qui manque, et recharger coûte 32 à 72 ms par
// échantillon (mesuré) — une latence à chaque changement de cue, pour rien.
// On charge tout au démarrage et on n'y revient plus. Ce n'est pas une
// exception qui complique : c'est un mécanisme en moins.
#ifndef SAMPLES_MAX
#define SAMPLES_MAX 24        // mapfs n'en tiendra jamais beaucoup plus
#endif

// Charge tout ce que mapfs contient. Retourne le nombre d'échantillons prêts.
uint8_t chargerTout();
// Recharge après un téléversement ou une suppression.
void    oublierTout();

uint8_t         nombreCharges();
int             indexDe(const char* nom);      // -1 si absent
const int16_t*  donnees(uint8_t i);            // en PSRAM
size_t          trames(uint8_t i);
bool            stereo(uint8_t i);
uint32_t        frequence(uint8_t i);          // celle du fichier, pas celle de l'I2S
const char*     nom(uint8_t i);
size_t          octetsPsram();                 // total, tous échantillons

}  // namespace SampleStore
