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
// (Differe, src/config/EcrituresDifferees.h) : une ecriture en flash qui efface
// arrete les deux coeurs ~45 ms, et une composition de quelques ko en efface
// plusieurs. La plus recente est toujours celle qu'on rend, ecrite ou non.

#include <Arduino.h>
#include <memory>

namespace Compo {

/* OU ELLE VIT : `composition.json`, dans le dossier de la composition OUVERTE
 * du repertoire (Repertoire::chemin, MESURES §186). */

constexpr size_t MAX_OCTETS = 256 * 1024;   // plafond d'une composition

void demarrer();                   // lit la source de la composition ouverte, s'il y en a une
/* La composition ouverte a change (Repertoire::ouvrir) : sa source se relit —
 * ce qui attend le silence d'abord, la flash sinon. Aucune : plus de source. */
void recharger();

// La composition la plus recente — nullptr si la carte n'en a pas. Le tampon
// partage reste valide tant qu'on le tient, meme si une autre arrive.
std::shared_ptr<char> courante(size_t& octets);

// Adopter une composition recue (tampon PSRAM, JSON). Rendue tout de suite ;
// ecrite en flash au premier silence, 3 s au moins apres la derniere. Sur un
// repertoire vide, elle en cree la premiere composition, nommee d'apres son
// titre (« meta.name »). false : `raison` dit pourquoi — rien n'a change.
bool adopter(std::shared_ptr<char> tampon, size_t octets, String& raison);

String etatJson();                 // octets, en attente

}  // namespace Compo
