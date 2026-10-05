/*
 * SampleStore — les échantillons du boîtier : stockage flash, lecture PSRAM.
 *
 * Deux mémoires, deux rôles, et c'est le point du fichier :
 *
 *  - LittleFS sur la partition de fichiers, `storage` (1,56 Mo ; « mapfs »,
 *    1 Mo, jusqu'au §185 des mesures). C'est la persistance :
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
 * THE SD CARD (SdCard, component `sd_spi`) is a third place: its sounds
 * `/samples/*.wav` are read into PSRAM like those of storage, in the same store
 * and under their base name — but by the card's task, not ours (MBs over SPI,
 * in chunks). A name that storage already holds wins. Sounds too long for PSRAM
 * are STREAMED (SdStream.h) and keep only their header here.
 *
 * Coût en RAM interne : quelques centaines d'octets. À comparer aux 26 632 o de
 * Plaits — c'est ce qui permet au lecteur d'échantillons de cohabiter avec le
 * service de l'interface sans la dégrader.
 */
#pragma once
#include <Arduino.h>
#include <FS.h>

namespace SampleStore {

// Le dossier des sons dans storage — declare ici, lu par l'explorateur (FichiersAPI).
constexpr const char* DOSSIER = "/samples";

// Montage paresseux de storage (formatage si vierge). Sûr à appeler souvent.
bool monter();
bool estMonte();

size_t espaceTotal();
size_t espaceUtilise();

// Liste JSON des échantillons présents : [{"name":…,"bytes":…}, …]
String listerJson();

// Écriture par fragments (le corps d'un POST arrive en morceaux). UN
// téléversement à la fois, tenu par `qui` (la requête qui l'a ouvert) : les
// autres fonctions n'agissent que pour lui. ecrireFin() referme, puis refuse
// — et RETIRE — un fichier que le lecteur ne saurait pas jouer (écriture
// incomplète, pas un WAV PCM 16 bits) : il était accepté, listé, marqué ●, et
// ne sonnait jamais (MESURES §177). ecrireAbandon() : la requête est partie
// avant la fin, le fichier à moitié écrit s'en va avec elle.
constexpr size_t NOM_MAX = 48;          // nul final compris
/* UN NOM QU'UNE CUE PEUT PORTER (§179). La ligne de cue sépare ses champs par
 * `|`, ses réglages par `;` et `=`, ses sons par `,` : un son nommé « a,b.wav »
 * se téléversait, et aucune cue n'aurait jamais pu le jouer. Refusés aussi : ce
 * qui casserait un chemin ou du JSON (`/`, `\`, `"`), les caractères de
 * contrôle, le vide, et au-delà de NOM_MAX - 1. */
bool nomValide(const char* nom, String& raison);
bool ecrireDebut(const void* qui, const char* nom);
bool ecrireMorceau(const void* qui, const uint8_t* donnees, size_t taille);
bool ecrireFin(const void* qui, String& raison);
void ecrireAbandon(const void* qui);

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
//     storage plafonne à 1 638 400 o — c'est TOUT ce que la carte peut stocker
//     PSRAM libre                     8 249 372 o
//     donc le pire cas absolu tient dans 19,9 % de la PSRAM
//
// Décharger ne rend donc rien qui manque, et recharger coûte 32 à 72 ms par
// échantillon (mesuré) — une latence à chaque changement de cue, pour rien.
// On charge tout UNE fois ; ensuite, un téléversement ou une suppression ne
// touche qu'à SON échantillon (§177) — il ne se passait rien : un son
// téléversé après ce chargement restait « absent » jusqu'au redémarrage, un
// son remplacé gardait l'ancien.
#ifndef SAMPLES_MAX
#define SAMPLES_MAX 24        // storage n'en tiendra jamais beaucoup plus
#endif

// Charge tout ce que storage contient, la première fois ; les appels suivants
// ne relisent rien. Retourne le nombre d'échantillons prêts.
uint8_t chargerTout();
// The names of the SD CARD sounds (SdCard) that the store does not have yet, so that
// the card's task installs them one by one (installer(..., true)).
uint8_t sdSoundNames(char noms[][NOM_MAX], uint8_t max);
/* The PRELOAD ceiling from the SD: one sound, and all of them together. Beyond that, a
 * sound is streamed (SdStream.h) instead of being kept in PSRAM. */
constexpr size_t PRELOAD_SOUND_MAX = 1536 * 1024;
constexpr size_t PRELOAD_SD_MAX    = 3 * 1024 * 1024;
// A WAV header: `f` is left at the start of the data. False (and `raison`) if it is not 16-bit PCM.
bool wavHeader(File& f, uint16_t& canaux, uint32_t& freq, uint32_t& octetsData, String& raison);
bool    charge();                      // chargerTout() est passé : lisible() dit vrai

// ── Changer le magasin pendant que la tâche audio le lit ───────────────────
//
// Un emplacement n'est jamais réécrit sous une voix. Le son NOUVEAU va dans un
// emplacement libre ; il devient lisible (par son nom) AVANT que l'ancien du
// même nom cesse de l'être — aucun déclenchement ne tombe entre les deux.
// L'ancien est RETIRÉ : plus trouvé par son nom, plus lu (donnees() rend nul,
// ses voix se taisent au bloc suivant). Sa PSRAM ne se rend qu'avec liberer(),
// que l'appelant ne fait qu'une fois la tâche audio sortie du bloc qui a pu la
// lire (AudioEngine::echantillonArrive / echantillonParti).
//
// installer() : lit `nom` (30 à 70 ms de flash, hors verrou — `fromSdCard`: on the
// SD card, in chunks, from ITS task only) et le publie ;
// `retire` = l'emplacement de l'ancien, -1 s'il n'y en avait pas. Magasin pas
// encore chargé : rien à faire, chargerTout() le lira avec les autres.
bool installer(const char* nom, String& raison, int& retire, bool fromSdCard = false);
int  retirer(const char* nom);                 // l'emplacement retiré, -1 si absent
void liberer(int i);

uint8_t         nombreCharges();               // les emplacements lisibles
int             indexDe(const char* nom);      // -1 si absent
bool            lisible(uint8_t i);            // faux : retiré, ou vide
// An SD sound read AS A STREAM (SdStream): readable, with frames and a frequency — but no
// data in PSRAM (`donnees` returns null). `dataOffset`: the byte where the data begins.
bool            isStreamed(uint8_t i);
uint32_t        dataOffset(uint8_t i);
const int16_t*  donnees(uint8_t i);            // en PSRAM ; nul si pas lisible
size_t          trames(uint8_t i);
bool            stereo(uint8_t i);
uint32_t        frequence(uint8_t i);          // celle du fichier, pas celle de l'I2S
size_t          octetsPsram();                 // total, tous échantillons

}  // namespace SampleStore
