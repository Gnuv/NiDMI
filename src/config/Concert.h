#pragma once
// src/config/Concert.h — LE VERROU DE CONCERT (MESURES §197).
//
// Un concert n'est pas un atelier. Pendant le spectacle, rien de ce qui DEFINIT
// ce que la carte joue ne doit bouger — ni les cues, ni les scripts, ni les
// CC appris, ni le moteur, ni les broches — quel que soit le client qui le
// demande : un onglet oublie depuis la veille, une tablette de scene rechargee
// (elle repart en EDIT), un geste de travers, un outil. Les modes EDIT / REGIE /
// SCENE de l'app etaient propres a chaque onglet, en memoire de ce navigateur :
// ils ne protegeaient de rien contre un AUTRE client.
//
// La regle du projet vaut ici — « tout s'execute sur la carte ; le navigateur ne
// fait que PREPARER » — : c'est donc la CARTE qui refuse. Un seul octet d'etat,
// memorise au silence, annonce a tous les onglets (NIDMI_VERROU:0|1).
//
// CE QUI PASSE, verrouille : les GESTES DE JEU — le transport (play, pause,
// stop, go, goto, prev), les notes et les CC (WebSocket), les paramètres et les
// gains (/api/audio/params sans `engine`, /api/audio/sampler/gain), les clips,
// les reglages d'un script (sans son code), l'arret du son, le changement de
// piece (/api/compositions/ouvrir), et le verrou lui-meme. Une regie, une scene,
// plusieurs clients : tous jouent.
//
// CE QUI EST REFUSE (423), et c'est le DEFAUT : toute requete qui ecrit
// (POST, PUT, DELETE, PATCH) et n'est pas dans la liste ci-dessus. Une route
// ajoutee demain est donc refusee tant qu'on n'a pas dit qu'elle est un geste de
// jeu — une garde par liste noire oublie la route qu'on n'a pas pensee. Elle est
// posee AVANT les autres routes (AsyncWebHandler, premier qui reconnait la
// requete) : un televersement refuse n'ecrit pas un octet en flash.
//
// Debrancher l'app ne change rien : le verrou est a la carte, il survit a un
// redemarrage (une carte qui replante en plein concert revient verrouillee).
// Il n'est pas une securite — personne n'est authentifie — mais une
// INTERDICTION DE L'ACCIDENT : le lever est un geste, ecrit, qui dit qui.

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

namespace Concert {

/* Relit l'etat memorise (NVS « nidmi-etat », hors de l'interface : ce n'est pas
 * un reglage, c'est une situation — un .interface sauvegarde un jour de concert
 * ne doit pas reverrouiller une carte). Au demarrage, avant le serveur. */
void demarrer();

/* Un octet : lisible de partout, sans verrou. */
bool verrouille();

/* Pose ou leve le verrou. Vaut tout de suite ; se memorise au premier silence
 * (Differe) ; s'annonce a tous les onglets. `par` : qui le demande, pour le dire
 * (« app · 192.168.7.2 »). */
void fixer(bool oui, const String& par);

/* Ce que la carte sait du verrou, en JSON : verrouillee, par, depuis_s. */
String etatJson();

/* La garde HTTP. A poser AVANT toute autre route : le premier handler qui
 * reconnait la requete la prend, et le reste n'en voit jamais ni le corps ni le
 * televersement. */
void installerGarde(AsyncWebServer& server);

/* Pour une route MIXTE — un geste de jeu qui porte aussi, parfois, de la
 * structure (/api/audio/params avec `engine`, /api/midi/script avec `script`) :
 * verrouille, elle repond 423 et rend true — l'appelant sort sans rien faire. */
bool refuse(AsyncWebServerRequest* requete, const char* pourquoi);

}  // namespace Concert
