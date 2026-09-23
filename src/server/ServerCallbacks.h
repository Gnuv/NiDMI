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
 * @brief Le cable OU le WiFi — voir NiDMI.cpp, « LE CABLE OU LE WIFI ».
 * Rallumer la radio, ou l'ESSAI : la couper dureeMs, mesurer, la rallumer sur
 * son seul minuteur. Differes de 300 ms. Rien n'est memorise.
 */
void nidmi_requestRallumerWifi(void);
void nidmi_requestEssaiWifi(unsigned long dureeMs);

/**
 * @brief Demander un redémarrage différé "persist USB"
 * (utile quand on change la configuration/dé-énumération USB via TinyUSB)
 */
#ifdef __cplusplus
}
#endif
