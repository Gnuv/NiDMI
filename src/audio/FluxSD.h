#pragma once
// src/audio/FluxSD.h — LIRE UN SON LONG DEPUIS LA CARTE SD, SANS LE CHARGER.
//
// PSRAM : 8 Mo pour tous les sons. Un son de la SD de plusieurs minutes n'y tient
// pas, et l'interet d'une SD est justement la. Il se joue donc EN FLUX : un lecteur
// (une tache a lui) remplit un tampon circulaire en PSRAM un peu en avance, et la
// tache audio ne lit QUE ce tampon. C'est le principe d'un lecteur de disque
// d'echantillonneur ; la difference avec un ordinateur est que personne ne fait la
// lecture anticipee a notre place, et que l'audio ne doit JAMAIS attendre la SD :
//
//   - une lecture peut prendre de 1 a 100 ms et plus, ou un saut en 68 ms (mesure,
//     MESURES §204), quand le DMA de l'audio n'a que ~30 ms d'avance ;
//   - un tampon vide rend du SILENCE, pas un bloc en retard : on perd du son, jamais
//     du temps. Le temps du clip continue (sa position avance) ; le lecteur, s'il est
//     en retard, saute en avant et rattrape.
//
// DEUX PIECES.
//
//  LA TETE. Un clip qui demarre au milieu d'un long fichier ne peut pas attendre
//  68 ms + 24 ms de lecture. Le DEBUT de chaque clip arme (TETE_TRAMES, 0,37 s a
//  44,1 kHz) est donc lu d'avance, une fois, par la tache de la carte, et gardee en
//  PSRAM : le clip part de la tete, instantanement, pendant que le lecteur se place
//  derriere et remplit le tampon a partir de la fin de la tete. Une tete se
//  reconnait par (son, trame de debut) : deux clips qui commencent au meme endroit
//  la partagent. Elle est ELAGUEE quand plus rien ne la nomme (AudioEngine).
//
//  LE FLUX. Une voix de play list, un tampon (ANNEAU_TRAMES), un fichier ouvert. La
//  voix joue la tete, puis le tampon. Au rebouclage ou a l'enchainement, la voix
//  repart de la tete d'un clip et le flux se replace (`demarrer` / `redemarrer`).
//
// QUI ECRIT QUOI (un seul ecrivain par champ, pas de verrou dans la tache audio) :
//   l'audio  : seq, depart, fin, son, offset, canaux, cons
//   le lecteur : lo, hi, seqVue, et ses fichiers
// `seqVue == seq` : le lecteur s'est place pour la derniere demande ; avant, le
// tampon n'est pas a lire (la tete couvre l'attente).
#include <Arduino.h>

namespace FluxSD {

constexpr uint8_t  FLUX_MAX       = 3;        // flux simultanes (un par voix de play list qui joue)
constexpr uint8_t  TETES_MAX      = 64;       // entrees de la table (quelques dizaines d'octets chacune)
constexpr uint32_t TETE_TRAMES    = 16384;    // la tete d'un clip : 0,37 s a 44,1 kHz
/* LE BUDGET DES TETES, EN OCTETS, pas en nombre : une tete stereo prend 64 Ko, une mono 32 Ko.
 * 2 Mo = ~32 clips stereo (~64 mono) a tete — le quart de la PSRAM libre (8,2 Mo au demarrage).
 * Somme des plafonds de la PSRAM : sons de storage 1,56 Mo + sons de SD precharges 3 Mo + tetes
 * 2 Mo + tampons ~0,5 Mo ≈ 7 Mo — d'ou la reserve ci-dessous : AUCUNE tete ne se prend si la PSRAM
 * libre tombe sous 1 Mo (les tampons de reponse du serveur web, les banques, la console y vivent). */
constexpr uint32_t TETES_BUDGET_OCTETS    = 2u * 1024u * 1024u;
/* Les tetes ANTICIPEES (la cue suivante, option « Precharger » d'un bloc) n'ont que la moitie : la
 * cue qui joue passe toujours avant, et les evince au besoin. */
constexpr uint32_t ANTICIPE_BUDGET_OCTETS = 1u * 1024u * 1024u;
constexpr uint32_t PSRAM_RESERVE_OCTETS   = 1u * 1024u * 1024u;
constexpr uint32_t ANNEAU_TRAMES  = 32768;    // un tampon : 0,74 s a 44,1 kHz (puissance de 2)
constexpr uint32_t MORCEAU_TRAMES = 4096;     // ce que le lecteur lit d'un coup
constexpr uint32_t GARDE_TRAMES   = 1024;     // jamais ecrire a moins de cela du lecteur audio
constexpr uint32_t SAUT_TRAMES    = 2048;     // un lecteur depasse se replace a cette avance

// ── Les tetes ────────────────────────────────────────────────────────────────
/* L'index de la tete de (son, debut) — la demande, si elle n'existe pas. Se charge dans la tache de
 * la carte SD ; `tetePrete` dit quand. `anticipee` : pour la cue SUIVANTE (sous son demi-budget ; une
 * demande courante la promeut, et peut l'evincer tant qu'elle ne l'a pas fait).
 *   >= 0 : l'index ;  -1 : pas une tete possible (son absent, pas lu en flux, debut au-dela de la fin) ;
 *   -2 : le BUDGET est plein (ou la PSRAM sous sa reserve, ou la table) — l'appelant eleve ce qu'il peut,
 *        redemande, puis compte un refus (`noterRefus`) ;
 *   -3 : une demande ANTICIPEE, et le demi-budget des anticipees est plein — seule une anticipee perimee
 *        peut lui faire de la place (evincer des tetes courantes n'y changerait rien). */
constexpr int8_t TETE_INCONNUE = -1, TETE_BUDGET_PLEIN = -2, TETE_ANTICIPE_PLEIN = -3;
int8_t   demanderTete(const char* son, uint32_t debut, bool anticipee = false);
bool     teteAnticipee(int8_t i);                       // demandee pour la cue suivante, pas encore promue
bool     teteEvincable(int8_t i);                       // prete ou en echec — jamais une tete en cours de lecture
void     noterRefus();                                  // un clip n'a pas eu de tete : budget plein
void     noterRefusAnticipe();                          // une tete anticipee n'a pas trouve de place (sans gravite)
bool     tetePrete(int8_t i);
struct   TeteVue { const int16_t* pcm; uint32_t debut; uint32_t trames; };
bool     teteVue(int8_t i, TeteVue& v);                 // faux si elle n'est pas prete
uint32_t teteAgeMs(int8_t i);                           // depuis la derniere demande ; 0 si libre
bool     teteUtilisee(int8_t i);                        // la table l'a (prete, demandee ou en echec)
void     libererTete(int8_t i);                         // l'appelant a verifie que rien ne la nomme
void     chargerTetes();                                // par la tache de la carte SD
bool     teteEnCours(int8_t i);                         // demandee, pas encore lue (ni en echec)
void     prioriser(int8_t i);                           // une note l'attend : a lire avant les autres
void     noterAttente(uint32_t ms);                     // une note a attendu sa tete (compteurs de diagnostic)
void     noterAbandon();                                // ... ou on y a renonce

// ── Les flux ─────────────────────────────────────────────────────────────────
// Appeles par la TACHE AUDIO, et elle seule (sauf diagnostic).
int8_t   acquerir(uint8_t voix);                        // -1 : plus de flux, ou plus de PSRAM
bool     possede(int8_t f, uint8_t voix);
bool     actif(int8_t f);
uint8_t  voixDe(int8_t f);
void     liberer(int8_t f);
/* (Re)place le flux : remplir [depart, fin) de `son`. `offsetOctets` : ou commencent les
 * donnees dans le fichier. */
void     demarrer(int8_t f, const char* son, uint32_t depart, uint32_t fin,
                  uint8_t canaux, uint32_t offsetOctets);
void     redemarrer(int8_t f);                          // memes parametres : la voix a rebouclé
void     position(int8_t f, uint32_t trame);            // ou en est la voix
/* Une trame du tampon. Faux : pas encore (ou plus) dans le tampon — silence. */
bool     lire(int8_t f, uint32_t trame, int16_t& g, int16_t& d);
void     manque(int8_t f);                              // un bloc qui a manque de donnees
void     arreterTout();                                 // la carte se demonte : rendre les flux et fermer les fichiers

String   diagnostic();

}  // namespace FluxSD
