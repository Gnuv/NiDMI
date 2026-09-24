#pragma once
// src/config/EcrituresDifferees.h — CE QUI PEUT ATTENDRE ATTEND LE SILENCE
// (MESURES §155, §156, §157).
//
// Une ecriture en flash qui efface arrete les DEUX coeurs le temps de
// l'effacement : 43 a 53 ms mesures, plus que les 30 ms d'avance du DMA audio.
// Mesure en jeu : cinq reecritures de cues.txt ont coute un bloc de 26 ms.
// Or presque tout ce que la carte ecrit en flash peut attendre : la valeur
// vaut tout de suite en memoire, seule sa memorisation est differee.
//
// Ici, un seul mecanisme pour tous :
//   - des FICHIERS de mapfs : le contenu le plus recent, en PSRAM, est rendu
//     aux lecteurs (qui demandent d'abord ce qui attend), ecrit a cote puis
//     renomme ;
//   - des valeurs NVS : la derniere par (espace, cle) gagne.
// Tout part au premier silence de la sortie audio (AudioEngine::
// silencePourLaFlash), 3 s au moins apres la derniere pose, et chaque
// ecriture est ANNONCEE au moteur (ses blocs en retard sont comptes a part,
// hors du voyant). Avant un redemarrage voulu, tout part tout de suite : le son
// s'arrete de toute facon.
//
// Une coupure de courant avant le silence perd ce qui attendait : la carte
// redemarre sur les valeurs d'avant. C'est le prix, et il est dit.

#include <Arduino.h>
#include <memory>

namespace Differe {

// ── Fichiers de mapfs ────────────────────────────────────────────────────
// Adopter un contenu (tampon PSRAM partage, sans copie) pour `chemin`.
// false : plus de place dans la file d'attente — l'appelant ecrit lui-meme.
bool poserFichier(const char* chemin, std::shared_ptr<char> tampon, size_t n);
// Copie en PSRAM, puis poserFichier. Pour qui n'a qu'une String.
bool poserFichierCopie(const char* chemin, const char* data, size_t n);
bool supprimerFichier(const char* chemin);

// Ce qui attend pour ce chemin. true : quelque chose attend — soit un contenu
// (`tampon`, `n`), soit une suppression (`supprime`). false : rien, lire la flash.
bool attente(const char* chemin, std::shared_ptr<char>& tampon, size_t& n, bool& supprime);

// Visiter ce qui attend sous un dossier (pour lister ce que la flash n'a pas
// encore) : `visiter(chemin, n, supprime, ctx)`.
void visiterAttente(const char* prefixe,
                    void (*visiter)(const char* chemin, size_t n, bool supprime, void* ctx),
                    void* ctx);

// ── NVS ──────────────────────────────────────────────────────────────────
void nvsChaine(const char* espace, const char* cle, const String& valeur);
void nvsOctet(const char* espace, const char* cle, uint8_t valeur);
void nvsRetirer(const char* espace, const char* cle);

// ── La boucle ────────────────────────────────────────────────────────────
void boucle();                 // nidmi_loop : ecrit ce qui attend, au silence
void toutEcrireMaintenant();   // avant un redemarrage voulu
bool enAttente();
String etatJson();

}  // namespace Differe
