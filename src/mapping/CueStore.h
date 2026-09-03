// CueStore / CueEngine — le systeme de cues SUR LA CARTE.
//
// Condition du headless : une carte deployee n'a pas de navigateur pour lui
// dire quelle cue jouer. Elle doit tenir sa propre liste, la parcourir, et
// appliquer a chaque cue ce qui la definit : un script .nms, et — si la carte
// a de l'audio — un moteur et ses reglages.
//
// FORMAT (une cue par ligne, mapfs:/cues.txt) :
//     nom | duree_s | script.nms | engine | params
//   duree 0        = attendre un GO (cue infinie)
//   script vide    = passage direct du MIDI
//   engine -1      = pas d'audio  (une carte sans audio n'ecrit que ca)
//   params         = "harmonics=0.2;timbre=0.5", facultatif
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
  String  script;            // nom de fichier .nms ("" = aucun)
  int     engine  = -1;      // -1 = pas d'audio
  String  params;            // "cle=valeur;cle=valeur"
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
void  suivant();           // GO   : cue suivante
bool  aller(int index);    // saut direct
void  boucle();            // appelee par nidmi_loop : avance les cues minutees

bool  enLecture();
int   indexCourant();
float restantSec();        // temps restant sur la cue courante (0 si infinie)

}  // namespace Cues
