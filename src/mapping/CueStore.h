// CueStore / CueEngine — le systeme de cues SUR LA CARTE.
//
// Condition du headless : une carte deployee n'a pas de navigateur pour lui
// dire quelle cue jouer. Elle doit tenir sa propre liste, la parcourir, et
// appliquer a chaque cue ce qui la definit : un script .nms, et — si la carte
// a de l'audio — un moteur et ses reglages.
//
// FORMAT (une cue par ligne, storage:/cues.txt) :
//     nom | duree_s | scripts | engine | params_audio | params_script | env
//   duree 0        = attendre un GO (cue infinie)
//   scripts        = les .nms de la CHAINE, separes par des virgules : la
//                    position dit le maillon (« a.nms,,b.nms » met a.nms au
//                    premier, libere le deuxieme, met b.nms au troisieme).
//                    Vide = passage direct du MIDI, chaine entierement liberee.
//   engine -1      = pas d'audio  (une carte sans audio n'ecrit que ca)
//   params         = "harmonics=0.2;timbre=0.5", facultatif
//   env            = "harmonics:0.1,0.2,...;volume:1.0,0.9,...", facultatif
//
// LES ENVELOPPES SONT REECHANTILLONNEES PAR L'APP, pas evaluees ici.
// Une enveloppe y est une liste de points PLUS des courbes de Bezier par
// segment, etendue sur plusieurs cues avec des zones ponderees par la largeur
// et la duree de chacune. Reecrire cela en C++ serait une seconde
// implementation a faire diverger — et la carte n'a rien a y gagner.
// L'app sait evaluer la courbe : elle echantillonne la portion qui concerne
// CETTE cue et n'envoie que des nombres. On interpole lineairement entre eux
// sur la duree de la cue. La carte ne connait donc ni Bezier ni disposition
// multi-cues, et l'app reste seule maitresse de la forme.
// Les lignes vides et celles commencant par '#' sont ignorees.
//
// POURQUOI PAS DU JSON : le firmware n'a pas de bibliotheque JSON (il parse a
// la main partout ailleurs), et surtout le budget de tas interne est d'environ
// 11 ko (MESURES.md §15). Un format ligne se lit en STREAMING, sans document
// intermediaire.
//
// LA LISTE VIT EN PSRAM (MESURES §157), pas dans le tas interne — dont le
// budget interdisait de la garder, et qui la faisait relire de storage a chaque
// changement de cue. Le texte entier y est tenu, lu de la memoire a chaque GO ;
// ecrireTout() le rend aussitot et ne l'ecrit en flash qu'au premier silence
// (src/config/EcrituresDifferees.h) : l'ecriture qui efface arrete l'audio.
// Seule la cue ACTIVE est decodee (struct Cue) ; les autres restent du texte.
#pragma once
#include <Arduino.h>

namespace Cues {

// La liste de cues dans storage — declaree ici, lue par l'explorateur (FichiersAPI).
constexpr const char* FICHIER = "/cues.txt";

struct Cue {
  String  nom;
  float   duree   = 0.0f;    // secondes ; 0 = infinie (attend un GO)
  /* Les noms de fichiers .nms de la chaine, separes par des VIRGULES — la
   * position dit le maillon, une position vide le libere. "" = aucun script.
   * Une seule valeur reste donc valide : elle occupe le premier maillon. */
  String  script;
  int     engine  = -1;      // -1 = pas d'audio
  String  params;            // "cle=valeur;cle=valeur"
  String  paramsScript;      // reglages du .nms, meme format ("semitones=12")
  String  env;               // enveloppes : "param:v0,v1,...;param2:..." (cf. plus bas)
};

// ── Magasin ───────────────────────────────────────────────────────────────
bool   monter();
int    nombre();                       // compte les lignes utiles
bool   lire(int index, Cue& sortie);   // decode la cue N (texte en PSRAM)
bool   ecrireTout(const String& contenu);
String contenu();                      // le fichier brut, pour l'app

// ── Transport ─────────────────────────────────────────────────────────────
void  demarrer();          // PLAY : active la cue courante et lance le decompte
void  arreter();           // STOP : ferme la porte de silence, decompte a plat
/* PAUSE : gele le TEMPS, pas le SON. Le decompte, l'automation d'enveloppe,
 * l'enchainement et l'horloge des scripts s'arretent ; la porte de silence,
 * elle, ne bouge pas — une note tenue reste tenue, un echantillon continue.
 * demarrer() reprend la ou l'on en etait, sans recharger la cue.
 *
 * Deux dettes reglees d'un coup. L'app avait sa propre pause, locale : elle
 * affichait « arrete » pendant que la carte continuait de jouer. Puis la carte
 * a eu la sienne, mais elle coupait le son — « pause » n'etait qu'un arret qui
 * se souvenait de l'heure. Un sequenceur, une pause, et c'en est une. */
void  pauser();
void  suivant();           // GO   : cue suivante — la premiere apres la derniere si la liste boucle, rien sinon
void  precedent();         // cue precedente (la derniere avant la premiere si la liste boucle)
bool  aller(int index);    // saut direct
void  boucle();            // appelee par nidmi_loop : avance les cues minutees

// ── Options du transport (MESURES §181) ──────────────────────────────────
/* LA LISTE BOUCLE : apres la derniere cue, la suivante est la premiere — pour
 * GO (l'app, un bouton : s("sys.nextcue")) comme pour l'enchainement minute.
 * Sans elle, la fin de liste ARRETE (comportement d'origine). */
void  fixerBoucle(bool oui);
bool  boucleActive();
/* LECTURE AU DEMARRAGE : la carte lance la cue 1 seule, a chaque allumage —
 * l'instrument headless qu'on branche et qui joue. Sautee apres des plantages
 * consecutifs (le garde-fou coupe la restauration : la lecture pourrait etre
 * la cause) et sur un demarrage a vide. */
void  fixerLectureAuDemarrage(bool oui);
bool  lectureAuDemarrage();
/* Les deux vivent en NVS (« nidmi-cues ») ; elles valent tout de suite et se
 * memorisent au silence (Differe, §157). Lues une fois au demarrage. */
void  restaurerOptions();

/* L'ETAT DU TRANSPORT, pour r("sys.…") : le rappel est appele a CHAQUE
 * changement — lecture, pause, cue, liste, options —, pas par sondage : un
 * bouton qui fait GO voit r("sys.cue") changer au tour suivant. `suivante` :
 * l'index de la cue que GO jouerait, -1 s'il n'y en a pas (fin de liste). */
struct Etat {
  bool lecture, pause, boucle, auDemarrage;
  int  index, suivante, nombre;
};
void  surChangement(void (*rappel)(const Etat&));
void  annoncerEtat();      // republie l'etat (au demarrage, apres une liste)

bool  enLecture();
bool  enPause();           // gelee — DIFFERENT d'arretee, et le son y coule encore
int   indexCourant();
float restantSec();        // temps restant sur la cue courante (0 si infinie)

}  // namespace Cues
