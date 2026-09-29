#pragma once
// src/mapping/Repertoire.h — LE REPERTOIRE DES COMPOSITIONS
// (CONVERGENCE_NIDMI.md §9.2, §9.7 ; MESURES §186).
//
// La carte garde plusieurs compositions, chacune dans DEUX dossiers :
//     compositions/<nn>/<nom>/
// le dossier d'INDEX (deux chiffres) est sa PLACE — l'adresse d'un bouton,
// s("sys.compo") = n ; le dossier NOMME est la composition elle-meme, le meme
// que le .composition decompresse sur l'ordinateur. La place n'en fait pas
// partie : la meme composition peut etre la n°3 ici et la n°7 ailleurs.
//
// Dedans, des noms generiques — le dossier dit deja de qui il s'agit :
//     composition.json   la source, ce que l'app ouvre
//     cues.txt           ce que la carte execute
//     chain.txt          les maillons permanents de la chaine (MidiRouter)
//     options.txt        « la liste boucle »
//     README.txt         les instructions, ecrites a la main
//     *.nms              les scripts de ses cues et de sa chaine, A PLAT
// A plat : sur cette flash, un dossier coute deux blocs de 4 Ko (LittleFS range
// chaque dossier dans une paire de metadonnees), et un niveau de plus
// allongerait les chemins.
//
// PAS DE FICHIER D'INDEX : la liste, ce sont les dossiers — une liste recopiee
// finit par mentir (CONVERGENCE §9.6). Le numero de la composition OUVERTE vit
// en NVS (« nidmi », « compo ») : la carte redemarre dessus.
#include <Arduino.h>

namespace Repertoire {

constexpr const char* DOSSIER    = "/compositions";
constexpr uint8_t     NUMERO_MAX = 99;
constexpr size_t      NOM_MAX    = 48;      // octets UTF-8 : un accent en vaut deux
/* Le nom d'une composition qui n'en a pas encore : elle prend le titre de la
 * premiere source qu'elle recoit (Compo::adopter). */
constexpr const char* SANS_TITRE = "Sans titre";

// Les fichiers d'une composition : des NOMS, pas des chemins.
constexpr const char* SOURCE   = "composition.json";
constexpr const char* CUES     = "cues.txt";
constexpr const char* CHAINE   = "chain.txt";
constexpr const char* OPTIONS  = "options.txt";
constexpr const char* LISEZMOI = "README.txt";

/* Au demarrage, AVANT les magasins (composition, cues, chaine) : retrouve la
 * composition ouverte — celle de la NVS, sinon la premiere du repertoire,
 * sinon aucune. */
void demarrer();

uint8_t numeroOuvert();                 // 0 : aucune
String  nomOuvert();                    // "" : aucune
String  dossierOuvert();                // "/compositions/01/Concert d'ete" ; "" : aucune
String  chemin(const char* fichier);    // dossierOuvert() + "/" + fichier ; "" : aucune

/* La composition ouverte, creee s'il n'y en a aucune : au premier numero libre,
 * sous `nomPropose` s'il est valide, « Sans titre » sinon. Un geste de
 * PREPARATION (le premier envoi de l'app sur un repertoire vide) : ses dossiers
 * s'ecrivent tout de suite, annonces au moteur comme un televersement (§177). */
bool assurerOuverte(const String& nomPropose, String& raison);

/* Un nom de composition. Espaces et accents permis ; interdits / \ : * ? " < > |
 * et les caracteres de controle — la SD (FAT) et Windows les refusent, et une
 * composition doit garder son nom en y allant. Ni espace au debut ou a la fin,
 * ni point final. */
bool nomValide(const String& nom, String& raison);

/* Un chemin est-il dans une composition ? Rend son numero, son nom et le
 * fichier (ce qui suit le dossier nomme). */
bool decouper(const String& chemin, uint8_t& numero, String& nom, String& fichier);

// {"ouverte":1,"compositions":[{"numero":1,"nom":"Concert d'ete"}, …]}
String listerJson();

/* ── LES GESTES DU REPERTOIRE (CONVERGENCE §9.7) ───────────────────────────
 * Un a la fois. Tous rendent false avec la `raison`, dite a l'usager.
 *
 * Ce sont des gestes de PREPARATION : dossiers et copies s'ecrivent tout de
 * suite, annonces au moteur (§177). Ils ne se font pas pendant un spectacle —
 * sauf OUVRIR, qui n'ecrit rien que le numero ouvert (NVS, au silence). */

/* Les magasins qui tiennent la composition ouverte en memoire la relisent a
 * chaque ouverture ; qui veut savoir que le repertoire a change l'apprend.
 * Poses par NiDMI.cpp : le repertoire ne connait pas les magasins. */
void surOuverture(void (*recharger)());
void surChangement(void (*prevenir)());

/* Ouvrir, c'est comme REDEMARRER dessus : la composition se relit, la tete va
 * sur la cue 1, la lecture part si « lecture au demarrage » est coche. */
bool ouvrir(uint8_t numero, String& raison);
/* Une composition vide, au numero voulu (0 : le premier libre) ; ouverte. */
bool nouvelle(const String& nom, uint8_t numero, String& raison, uint8_t& obtenu);
/* ENREGISTRER SOUS : la composition ouverte copiee sous un autre nom et un
 * autre numero, et l'on CONTINUE SUR LA COPIE — l'original reste tel qu'il
 * etait. Rien ne se relit : ce qui est en memoire est la copie. */
bool enregistrerSous(const String& nom, uint8_t numero, String& raison, uint8_t& obtenu);
bool renommer(uint8_t numero, const String& nom, String& raison);
bool changerNumero(uint8_t de, uint8_t vers, String& raison);
/* Jamais la composition ouverte. Les sons sont communs : ils restent. */
bool supprimer(uint8_t numero, String& raison);

/* La suivante (+1) ou la precedente (-1) dans le repertoire, trous sautes, et
 * apres la derniere la premiere. 0 : aucune autre. */
uint8_t voisine(int sens);

}  // namespace Repertoire
