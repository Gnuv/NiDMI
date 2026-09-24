#pragma once
// src/mapping/CompoStore.h — LA COMPOSITION, GARDEE PAR LA CARTE (MESURES §156).
//
// La carte EXECUTE cues.txt et les .nms ; ils derivent d'une composition que
// seul le navigateur detenait. Rechargez la page : l'app repartait vide, la
// carte jouait toujours l'ancienne — « je ne vois plus mon bloc » — et la
// modification suivante proposait de la remplacer par l'ecran presque vide.
//
// La carte garde donc aussi la SOURCE — le JSON que l'app serialise (.compo) —
// telle quelle : elle ne la lit pas, elle la rend. L'app la recharge en se
// connectant, et la page montre ce que la carte porte.
//
// Recue en PSRAM, rendue depuis la PSRAM, ecrite en flash AU SILENCE seulement
// (AudioEngine::silencePourLaFlash) : une ecriture en flash qui efface arrete
// les deux coeurs ~45 ms, et une composition de quelques ko en efface
// plusieurs. La plus recente est toujours celle qu'on rend, ecrite ou non.

#include <Arduino.h>
#include <memory>

namespace Compo {

constexpr size_t MAX_OCTETS = 256 * 1024;   // plafond d'une composition

void demarrer();                   // lit /compo.json de mapfs, s'il existe

// La composition la plus recente — nullptr si la carte n'en a pas. Le tampon
// partage reste valide tant qu'on le tient, meme si une autre arrive.
std::shared_ptr<char> courante(size_t& octets);

// Adopter une composition recue (tampon PSRAM, JSON). Rendue tout de suite ;
// ecrite en flash au premier silence, 3 s au moins apres la derniere.
void adopter(std::shared_ptr<char> tampon, size_t octets);

void boucle();                     // depuis nidmi_loop : l'ecriture differee

bool enAttente();                  // une composition recue n'est pas encore en flash
String etatJson();                 // octets, ecritures, pire, echecs, en attente

}  // namespace Compo
