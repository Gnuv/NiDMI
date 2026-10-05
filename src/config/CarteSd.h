#pragma once
// src/config/CarteSd.h — LA CARTE SD, declaree comme un composant (`sd_spi`).
//
// Un SUPPORT de fichiers a cote de `storage` (Stockage.h) : la meme forme
// (`/samples` pour les sons), un autre volume — `sd` — que /api/fichiers declare
// avec son etat. Ce que le firmware en fait, pour l'instant :
//
//   - il la MONTE, quand le composant est declare (au demarrage, ou des qu'on
//     le declare) ;
//   - il lit ses sons — `/samples/*.wav`, PCM 16 bits — en PSRAM, comme ceux de
//     storage : meme magasin (SampleStore), memes noms dans les cues. Un nom
//     deja pris par storage l'emporte.
//   - c'est TOUT : lecture seule. Pas de televersement, pas de suppression, pas
//     de composition ni de .instrument sur la carte (CONVERGENCE §9.7, plus tard) ;
//     la carte se remplit depuis un ordinateur.
//
// UNE TACHE A ELLE, jamais la boucle ni le serveur web. Monter une carte, c'est
// des echanges SPI qui peuvent durer (une carte absente se laisse attendre) ; lire
// un son de plusieurs Mo, des secondes. Ni la boucle (cues, RTP-MIDI, OSC), ni
// async_tcp (son chien de garde a 5 s), ni l'audio ne doivent attendre apres
// elle : la tache est de priorite 1, sur le coeur 0, naissante et mourante (pas
// de pile gardee en RAM interne), et cede le processeur apres chaque morceau lu.
//
// Retirer le composant demonte la carte, mais ne decharge PAS les sons deja lus :
// ils restent jouables jusqu'au prochain demarrage (SampleStore ne rend la PSRAM
// que son par son, a la suppression d'un fichier de storage — il n'a pas de
// « decharger un volume »).
#include <Arduino.h>
#include <FS.h>

namespace CarteSd {

constexpr const char* VOLUME  = "sd";           // l'id que /api/fichiers lui donne
constexpr const char* DOSSIER = "/samples";     // la meme forme que storage
/* 10 MHz : une breadboard et des fils volants ne tiennent pas 20 MHz. Un son de
 * 1 Mo se lit en ~1 s ; il se lit une fois, au chargement. */
constexpr uint32_t    FREQUENCE_HZ = 10000000;

// Le composant est declare : ses broches sont connues, la carte est a monter.
// Idempotent (la restauration au demarrage, puis une redeclaration identique).
void declarer(uint8_t cs, uint8_t sck, uint8_t miso, uint8_t mosi);
// Le composant est retire : la carte se demonte.
void retirer();

bool     declaree();
bool     monte();          // la carte a repondu et son systeme de fichiers est monte
uint64_t total();          // sa capacite en octets, 0 si elle n'est pas montee

/* UN GESTE de l'usager a pu changer l'etat (il vient de brancher la carte) :
 * un nouvel essai, au plus un toutes les 5 s, dans la tache. Appelee par la liste
 * des fichiers quand le composant est declare mais la carte absente. */
void reessayer();

/* SampleStore a fini de lire storage : les sons de la carte peuvent suivre. */
void chargerSons();

// Ouvre un fichier de la carte en lecture ; un File vide si elle n'est pas montee.
File ouvrir(const char* chemin);

}  // namespace CarteSd
