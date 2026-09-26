#pragma once
#include <Arduino.h>

/* LE JOURNAL QUI SURVIT AU REDEMARRAGE (MESURES §162).
 *
 * Le 26/09 : le son a decroche, on a redemarre la carte depuis Reglages — le
 * geste naturel — et le rapport du surveillant, qui disait pourquoi, est parti
 * avec la memoire vive : la console garde ses lignes en PSRAM, et le
 * demarrage suivant repart d'une page blanche.
 *
 * La memoire RTC survit a un redemarrage logiciel, a une panique, a un chien
 * de garde — pas a une coupure de courant. On y garde donc, au fil de l'eau,
 * les dernieres lignes du surveillant de l'audio (« [audio] »), les dernieres
 * lignes tout court, et les compteurs du son. Le demarrage suivant les met a
 * l'abri : /api/diag/avant, Reglages → Etat de la carte, et la console, qui
 * les rejoue en tete de son historique. */
namespace JournalAvant {

/** Au tout debut de nidmi_begin(), APRES capturerDemandeur() (le resume dit
 *  qui a demande le redemarrage) : met a l'abri ce que la vie precedente a
 *  laisse en memoire RTC, puis repart d'un journal vide. */
void capturer();

/** Chaque ligne de la console. N'importe quelle tache, jamais une interruption. */
void noter(const char* ligne);

/** Les compteurs du son, repris par le surveillant a chaque quart de seconde :
 *  apres un plantage, on les connait a 250 ms pres. */
void compteurs(uint32_t blocs, uint32_t retards, uint32_t retardsEcritures);

struct Resume {
  uint32_t dureeMs = 0;            // sa duree, au dernier releve des compteurs
  uint32_t blocs = 0;
  uint32_t retards = 0;            // blocs rendus en retard
  uint32_t retardsEcritures = 0;   // dont : pendant une ecriture en flash, en silence (§157)
  /** Les decrochages ENTENDUS : les retards, moins ceux d'une ecriture faite en silence. */
  uint32_t entendus() const { return retards - (retardsEcritures <= retards ? retardsEcritures : retards); }
};

/** La vie precedente a-t-elle laisse un journal lisible ? Non apres une mise
 *  sous tension, ni apres une image dont le journal n'a pas la meme forme. */
bool disponible();
Resume resume();

/** Ses lignes gardees, dans l'ordre ou elles sont arrivees ; `t` en ms depuis
 *  son demarrage. */
uint8_t nbLignes();
bool ligne(uint8_t i, uint32_t& t, const char*& texte);
/** Dont les lignes du surveillant de l'audio : le rapport d'un bloc lent. */
uint8_t nbLignesAudio();

/** Le rejeu pour la console : un resume, puis les lignes datees. */
uint8_t nbLignesRejeu();
const char* ligneRejeu(uint8_t i);

}  // namespace JournalAvant
