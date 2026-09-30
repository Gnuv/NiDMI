#pragma once
// src/config/Instrument.h — L'INSTRUMENT : son nom, et les README au-dessus des
// compositions (CONVERGENCE §9.1, §9.7 ; MESURES §188).
//
// L'instrument, c'est l'interface et tout le repertoire des compositions : la
// carte entiere. Il a un NOM, pour s'y retrouver dans les sauvegardes — en NVS
// (« nidmi », « instrument »), HORS de l'interface : recharger une interface ne
// le touche pas. Ce n'est pas le nom reseau de la carte (mDNS, RTP-MIDI), qui
// reste un reglage de l'interface, avec ses regles techniques. Memes regles que
// les noms de composition (Repertoire::nomValide) ; vide : pas de nom.
#include <Arduino.h>

namespace Instrument {

/* Les README.txt au-dessus des compositions (§9.7) : celui de l'instrument a
 * la racine de storage, celui de l'interface dans son dossier. Celui d'une
 * composition est dans son dossier nomme (Repertoire::LISEZMOI). */
constexpr const char* LISEZMOI           = "/README.txt";
constexpr const char* LISEZMOI_INTERFACE = "/interface/README.txt";

void   demarrer();                                    // lit le nom, au demarrage
String nom();
/* Le nom vaut tout de suite, et s'ecrit au silence (Differe). "" le retire. */
bool   renommer(const String& nom, String& raison);

}  // namespace Instrument
