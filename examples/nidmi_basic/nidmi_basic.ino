/*
 * NiDMI Basic - ESP32-C3/S3 (XIAO)
 * 
 * Sketch universel pour ESP32-C3 et ESP32-S3 (XIAO)
 * Le MCU est détecté automatiquement
 * 
 * Ce sketch utilise l'architecture optimisée avec ComponentManager.
 * 
 * Fonctionnalités :
 * - Détection automatique du MCU (ESP32-C3 ou ESP32-S3)
 * - Interface web pour configuration des pins
 * - RTP-MIDI automatique
 * - Gestion optimisée des composants
 * - OSC (Open Sound Control)
 * - Touch pins (ESP32-S3 uniquement)
 * 
 * Pins disponibles :
 * - ESP32-C3 : A0-A2, D0-D10 (A3 n'existe pas)
 * - ESP32-S3 : A0-A4, D0-D9 (toutes les touch pins sont analogiques)
 * 
 * Usage :
 * 1. Sélectionner le board : XIAO_ESP32C3 ou XIAO_ESP32S3 dans Arduino IDE
 * 2. Uploader ce sketch
 * 3. Se connecter au WiFi "nidmi" (mot de passe: "nidmipass")
 * 4. Ouvrir http://192.168.4.1 dans Firefox (recommandé)
 * 5. Configurer les pins via l'interface web
 * 6. Les composants envoient automatiquement du MIDI
 * 
 * 🌐 Navigateur recommandé : Firefox
 * - Firefox fonctionne immédiatement sans configuration
 * - Interface web compatible avec tous les navigateurs
 * - Brave/Chrome fonctionnent aussi pour la configuration
 * 
 */

#include <NiDMI.h>

void setup() {
    /* UN TAMPON D'EMISSION (MESURES §172) : sans lui, toute trace de plus de
     * 128 octets (la FIFO de l'UART) bloquait la tache qui ecrivait, a
     * 115 200 bauds — ~4 ms par changement de cue, dans la boucle. 1 Ko :
     * l'ecriture rend la main, l'UART vide en tache de fond. Avant begin(). */
    Serial.setTxBufferSize(1024);
    Serial.begin(115200);
    delay(100);
    
    // Initialisation automatique (détection MCU automatique)
    nidmi.begin();
    
    // Le système est maintenant prêt :
    // - WiFi AP "nidmi" actif (mot de passe: "nidmipass")
    // - Interface web sur http://192.168.4.1
    // - RTP-MIDI initialisé automatiquement
    // - ComponentManager prêt à recevoir des configurations
}

void loop() {
    // Traitement automatique
    nidmi.loop();
    
    // Le ComponentManager gère :
    // - Lecture des potentiomètres
    // - Anti-rebond des boutons
    // - Envoi MIDI automatique
    // - Rechargement des configurations
}
