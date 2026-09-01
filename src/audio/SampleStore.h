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

// ── Chargement en PSRAM ────────────────────────────────────────────────────
// Analyse l'en-tête WAV, refuse ce qui n'est pas du PCM 16 bits, et copie les
// données dans la PSRAM. Retourne false avec une raison lisible sinon.
bool charger(const char* nom, String& raison);
void decharger();

bool            estCharge();
const int16_t*  donnees();     // en PSRAM
size_t          trames();      // nombre de trames (pas d'octets)
bool            stereo();
uint32_t        frequence();   // celle du fichier, pas celle de l'I2S
const char*     nomCharge();
size_t          octetsPsram();

}  // namespace SampleStore
