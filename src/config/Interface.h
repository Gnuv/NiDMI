#pragma once
// src/config/Interface.h — L'INTERFACE : tous les reglages de la carte, en un
// document (CONVERGENCE §9.1, §9.7 ; MESURES §190).
//
// L'interface, c'est ce qui est boulonne au materiel : broches, capteurs et
// leurs scripts, reseau, transports MIDI, OSC, tactile, lecture au demarrage.
// Tout vit en NVS, dans les espaces « nidmi » et « nidmi-cues ». Plutot qu'une
// liste de reglages ecrite a la main, qui deriverait (§9.6), la carte PARCOURT
// ces espaces : un reglage ajoute demain voyage sans rien changer ici.
//
// N'en sont PAS — une autre sauvegarde les porte, ou personne : « compo » (la
// composition ouverte : le repertoire), « instrument » (son nom, hors de
// l'interface), « interface » (son propre nom : le fichier le porte),
// « nvs_schema » (la version de la NVS, pour la carte seule). Le mot de passe
// WiFi (« sta_pass ») n'en sort que par le cable, et sur demande (§159) ; une
// interface rechargee sans lui garde celui de la carte.
#include <Arduino.h>

namespace Interface {

/* Les interfaces enregistrees SUR la carte : des .interface — les ZIP que
 * l'app construit —, gardes tels quels (le dialogue, Interface × Sur la carte). */
constexpr const char* DOSSIER_ENREGISTREES = "/interface/saved";

/* Son nom, pour s'y retrouver dans les sauvegardes ; "" : sans nom. Memes
 * regles que les noms de composition. Il vaut tout de suite et s'ecrit au
 * silence (Differe). */
String nom();
bool   renommer(const String& nom, String& raison);

/* Chaque reglage de l'interface, dans l'ordre de la NVS. `type` : u8, i8,
 * u16, i16, u32, i32, u64, i64, str, blob ; `valeur` en texte — un nombre en
 * decimal, un texte tel quel, des octets en hexadecimal. `secrets` : avec le
 * mot de passe WiFi. Rend false si la NVS ne se lit pas. `tus` : les cles
 * retenues (le mot de passe, sans secrets). */
typedef void (*Visiteur)(const char* espace, const char* cle, const char* type,
                         const char* valeur, void* ctx);
bool visiter(bool secrets, Visiteur voir, void* ctx, String& tus);

/* REMPLACER l'interface de la carte par ce document : des lignes
 * « espace|cle|type|valeur » — la carte n'a pas de lecteur JSON, l'app les tire
 * de son interface.json ; un texte ou des octets y sont %-encodes (%, |, les
 * caracteres de controle). Tout est verifie AVANT d'ecrire : une ligne fausse,
 * et rien ne change. Puis chaque reglage est pose, et ceux que le document ne
 * porte pas sont RETIRES — sauf ce qui n'en est pas, et le mot de passe WiFi.
 * `nom` devient son nom. La carte doit redemarrer pour les prendre (l'appelant
 * le demande). */
bool appliquer(const char* texte, size_t n, const String& nom, String& raison,
               int& poses, int& retires);

}  // namespace Interface
