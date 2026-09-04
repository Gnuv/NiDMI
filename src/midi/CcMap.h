// src/midi/CcMap.h — table CC → parametre, DANS la carte.
//
// Jusqu'ici le CC learn vivait entierement dans le navigateur (b.ccMappings,
// startCCLearn) : apprendre un CC sur « harmonics » marchait tant que l'app
// etait la, et cessait des qu'on la debranchait. C'est exactement ce que la
// regle du headless interdit — la carte doit continuer sans personne.
//
// Cote carte, un CC entrant n'allait qu'au ComponentManager (LEDs appairees).
// Rien ne le reliait a un parametre. Cette table est ce lien.
//
// DEUX FAMILLES DE CIBLES, une seule table :
//   - parametres du MOTEUR AUDIO : engine, harmonics, timbre, morph, decay,
//     lpg_colour — appliques par AudioEngine::setParams / setEngine ;
//   - parametres de SCRIPT : n'importe quel nom lu par r("param","nom",…) dans
//     un .nms — ecrits dans le FluxRegistry, exactement comme le fait
//     /api/midi/script?params=. Un bloc transpose se pilote donc au CC.
// La cible decide seule de sa famille (voir estParamAudio) : l'app n'a pas a
// dire laquelle, et un futur parametre de script ne demande aucun code ici.
//
// PERSISTANCE : la table vit en NVS, pas en LittleFS. C'est le meme partage que
// le script — le CONTENU d'un .nms est gros et va dans mapfs, la CONFIGURATION
// est minuscule et va en NVS, pour que la carte se retrouve entiere au
// demarrage sans monter de systeme de fichiers. Douze affectations tiennent
// dans quelques centaines d'octets.
#pragma once

#include <Arduino.h>

namespace CcMap {

// Douze : de quoi couvrir un controleur a huit potentiometres et quelques
// boutons sans reserver de RAM pour rien. Chaque entree pese ~28 o.
constexpr int MAX = 12;

struct Entree {
    uint8_t canal;      // 1..16, ou 0 = tous canaux
    uint8_t cc;         // 0..127
    char    cible[16];  // nom du parametre vise
    float   min, max;   // bornes dans lesquelles on etale la valeur 0..127
};

// Restaure la table memorisee. A appeler une fois au demarrage.
void monter();

// Un CC vient d'arriver. Applique toutes les affectations qui le visent
// (plusieurs cibles peuvent partager un CC) et renvoie leur nombre.
int appliquer(uint8_t canal, uint8_t cc, uint8_t valeur);

// ── Apprentissage ────────────────────────────────────────────────────────────
// L'app arme une cible, puis l'utilisateur tourne un potentiometre : c'est la
// CARTE qui capture le CC et cree l'affectation. Le navigateur n'a servi qu'a
// dire quoi apprendre — debranche-le, l'affectation reste.
void armer(const char* cible, float min, float max);
void desarmer();
bool arme();
const char* cibleArmee();
// Consomme l'armement si un CC arrive. Renvoie true si une affectation est nee.
bool apprendre(uint8_t canal, uint8_t cc);

// ── Serialisation ────────────────────────────────────────────────────────────
// Format texte, une affectation par ';' et cinq champs par ':' :
//     canal:cc:cible:min:max;canal:cc:cible:min:max
// Pas de JSON : aucune bibliotheque a bord, et le budget de tas se compte en
// milliers d'octets (meme raison que CueStore).
String texte();
bool   setTexte(const String& t, bool persister);
void   vider(bool persister);

// Vrai si le nom designe un parametre du moteur audio (les autres sont des
// parametres de script, ecrits dans le FluxRegistry).
bool estParamAudio(const char* nom);

}  // namespace CcMap
