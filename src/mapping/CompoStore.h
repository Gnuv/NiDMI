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
// partage reste valide tant qu'on le tient, meme si une autre arrive. `revision`
// (facultative) rend celle de CE tampon, prise sous le meme verrou : un client
// qui lit la source sait de quelle version elle est (MESURES §198).
std::shared_ptr<char> courante(size_t& octets, uint32_t* revision = nullptr);

/* LA REVISION (MESURES §198) : un CRC32 de la source, calcule par la carte — a
 * la reception (au fil du corps) et a la relecture —, jamais un compteur : il ne
 * depend que du CONTENU, donc il survit a un redemarrage, deux onglets qui ont
 * le meme texte ont la meme revision, et rien n'est a memoriser en flash. 0 : la
 * composition n'a pas de source. */
uint32_t revision();

// Adopter une composition recue (tampon PSRAM, JSON). Rendue tout de suite ;
// ecrite en flash au premier silence, 3 s au moins apres la derniere. Sur un
// repertoire vide, elle en cree la premiere composition, nommee d'apres son
// titre (« meta.name »). false : `raison` dit pourquoi — rien n'a change.
//
// `aBase` : l'ecriture DIT de quelle revision elle derive (`base`). Si ce n'est
// plus celle de la carte — un autre onglet a ecrit entre-temps —, elle est
// refusee (`obsolete`, et `revision` rend celle de la carte) : un onglet reste
// en retard ne remplace plus ce qu'un autre vient de poser. Le compare et la
// pose sont indivisibles. Sans `aBase` : les outils, les bancs — comme sans
// `compo=N`. Une composition sans source (revision 0) accepte tout.
bool adopter(std::shared_ptr<char> tampon, size_t octets, String& raison,
             bool aBase = false, uint32_t base = 0,
             bool* obsolete = nullptr, uint32_t* revision = nullptr);

String etatJson();                 // octets, en attente

}  // namespace Compo
