// CueStore / CueEngine — le systeme de cues SUR LA CARTE.
//
// Condition du headless : une carte deployee n'a pas de navigateur pour lui
// dire quelle cue jouer. Elle doit tenir sa propre liste, la parcourir, et
// appliquer a chaque cue ce qui la definit : un script .nms, et — si la carte
// a de l'audio — un moteur et ses reglages.
//
// FORMAT (une cue par ligne, mapfs:/cues.txt) :
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
// POURQUOI PAS DE LISTE EN RAM : 64 cues avec noms et params tiendraient
// plusieurs kilo-octets — sur ce budget, c'est refuse. Seule la cue ACTIVE vit
// en RAM ; les autres restent dans le fichier et sont relues au changement.
// Un changement de cue est un evenement rare, pas un chemin temps reel.
#pragma once
#include <Arduino.h>

namespace Cues {

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
bool   lire(int index, Cue& sortie);   // relit la cue N depuis mapfs
bool   ecrireTout(const String& contenu);
String contenu();                      // le fichier brut, pour l'app

// ── Transport ─────────────────────────────────────────────────────────────
void  demarrer();          // PLAY : active la cue courante et lance le decompte
void  arreter();           // STOP : silence, decompte a plat
/* PAUSE : gele le decompte la ou il en est ; demarrer() reprend sans recharger
 * la cue. L'app avait sa propre pause, locale — elle affichait « arrete »
 * pendant que la carte continuait de jouer. Un sequenceur, donc une pause. */
void  pauser();
void  suivant();           // GO   : cue suivante
bool  aller(int index);    // saut direct
void  boucle();            // appelee par nidmi_loop : avance les cues minutees

bool  enLecture();
int   indexCourant();
float restantSec();        // temps restant sur la cue courante (0 si infinie)

}  // namespace Cues
