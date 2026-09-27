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
 * les dernieres lignes des surveillants — l'audio (« [audio] », bloc lent),
 * le lien du cable (« [usbnet] », lien mort) —, les dernieres lignes tout
 * court, et les compteurs du son. Le demarrage suivant les met a
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
/** Dont les lignes des surveillants : le rapport d'un bloc lent, d'un lien
 *  mort. */
uint8_t nbLignesRapport();

/** La panique qui a mis fin a la vie precedente, si c'en etait une (§163) :
 *  le coeur fautif, la raison, les registres utiles, et la pile d'appels des
 *  deux coeurs (PC a passer a addr2line sur l'ELF de l'image). */
struct PaniqueAvant {
  int coeur = -1;
  const char* raison = "";       // vide si sa chaine n'etait pas en RAM : voir raisonAdr
  uint32_t raisonAdr = 0, adresse = 0, pc = 0, exccause = 0, excvaddr = 0;
  const uint32_t* pile[2] = {nullptr, nullptr};
  uint8_t profondeur[2] = {0, 0};
  // Les paniques survenues PENDANT son traitement (le 26/09 : le chien de
  // garde pendant l'ecriture du core dump) : combien, et la derniere en bref.
  uint32_t imbriquees = 0;
  int imbriqueeCoeur = -1;
  uint32_t imbriqueeRaisonAdr = 0, imbriqueePc = 0;
  const uint32_t* imbriqueePile = nullptr;
  uint8_t imbriqueeProfondeur = 0;
};
bool panique(PaniqueAvant& p);

/** Le rejeu pour la console : un resume, la panique s'il y en a eu une, puis
 *  les lignes datees. */
uint8_t nbLignesRejeu();
const char* ligneRejeu(uint8_t i);

}  // namespace JournalAvant
