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
void nidmi_requestReboot();
void nidmi_requestDownloadMode();

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
