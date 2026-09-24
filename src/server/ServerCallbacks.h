#pragma once

/**
 * @file ServerCallbacks.h
 * @brief Callbacks C pour les fonctions externes
 * 
 * Centralise les déclarations de callbacks C utilisées par l'API
 * pour éviter les dépendances inutiles vers NiDMIServer.h
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Demander le rechargement des configurations de pins
 * 
 * Cette fonction est appelée depuis l'API web pour demander
 * un rechargement des configurations de pins depuis la NVS.
 */
void nidmi_requestReloadPins();

/**
 * @brief Demander le rechargement de la configuration OSC
 * 
 * Cette fonction peut être appelée depuis l'API web pour demander
 * un rechargement de la configuration OSC.
 */
void nidmi_requestReloadOsc();

/**
 * @brief Demander un redémarrage différé (2 s pour laisser la réponse HTTP partir et la NVS se fermer proprement)
 */
/* `par` : QUI demande — note en RTC, lu au demarrage suivant, publie dans
 * /api/audio/status (« redemarrage_demande_par »). Voir NiDMI.cpp. */
void nidmi_requestReboot(const char* par);
void nidmi_requestDownloadMode(const char* par);
const char* nidmi_redemarrageDemandePar(void);

/* Demarrer a vide UNE fois : le son revient au redemarrage suivant. */
void nidmi_demanderDemarrageAVide(void);
bool nidmi_prendreDemarrageAVide(void);   // consomme le drapeau (restaurerAuBoot)
bool nidmi_demarreAVide(void);

/* La sante, decidee par la carte : un octet de causes, et leur nom. */
unsigned char nidmi_sante(void);
void nidmi_santeTexte(unsigned char f, char* out, unsigned n);

/**
 * @brief L'ESSAI — voir NiDMI.cpp, « LE CABLE OU LE WIFI » : couper la radio
 * dureeMs, mesurer, la rallumer sur son seul minuteur. Differe de 300 ms. Rien
 * n'est memorise. (Rallumer la radio, c'est le forcage :
 * nidmi_demanderWifiForce.)
 */
void nidmi_requestEssaiWifi(unsigned long dureeMs);

/**
 * @brief La bascule « cable prioritaire » — voir NiDMI.cpp. Demander de
 * l'activer ou de la retirer (appliquee par nidmi_loop dans la seconde,
 * memorisee en NVS au premier silence de la sortie audio, 3 s au moins apres
 * le dernier changement — « LE WIFI : TROIS REGLES »).
 */
void nidmi_demanderCablePrioritaire(bool actif);

/**
 * @brief Le WiFi : trois regles, un seul chef — voir NiDMI.cpp (MESURES §155).
 * L'option « Instrument autonome » (memorisee), le forcage (jamais memorise :
 * un redemarrage le retire), et si c'est une REGLE qui tient la radio coupee
 * en ce moment (bascule ou autonomie).
 */
void nidmi_demanderAutonome(bool actif);
void nidmi_demanderWifiForce(bool actif);
bool nidmi_regleTientLeWifiCoupe(void);

/**
 * @brief s("sys.<nom>") dans un script .nms : demander a la carte (NiDMI.cpp,
 * « LES FONCTIONS DE LA CARTE, POUR LES SCRIPTS »). Pose une demande, rien
 * d'autre : appelable depuis une tache temps reel.
 */
void nidmi_sys_recevoir(const char* nom, float valeur);

/**
 * @brief Relancer le cable : refaire l'enumeration USB (MESURES §154). Executee
 * par nidmi_loop ; coupe aussi le MIDI USB une a deux secondes.
 */
void nidmi_demanderRelanceCable(void);

/**
 * @brief Demander un redémarrage différé "persist USB"
 * (utile quand on change la configuration/dé-énumération USB via TinyUSB)
 */
#ifdef __cplusplus
}
#endif
