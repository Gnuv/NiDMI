#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <IPAddress.h>
#include <nidmi_core.h>
#include "../network/BluetoothManager.h"
#include "../network/UsbMidiManager.h"

/**
 * @brief Infrastructure serveur (WiFi, mDNS, RTP-MIDI, Web)
 * 
 * Cette classe gère l'infrastructure de base :
 * - Access Point Wi-Fi
 * - Station Wi-Fi (optionnelle)
 * - mDNS
 * - Serveur HTTP asynchrone
 * - WebSocket
 * - RTP-MIDI
 * - Bluetooth MIDI
 * - USB MIDI (ESP32-S3 uniquement)
 */
class ServerCore {
private:
    AsyncWebServer server;
    AsyncWebSocket ws;
    nidmi_core::RtpMidiService rtpMidiInstance;
    BluetoothManager bluetoothInstance;
    UsbMidiManager usbMidiInstance;
    bool useStaticSta = false;
    IPAddress staIp, staGw, staSn;
    
public:
    ServerCore();
    
    // Initialisation (apOnlyMode: true si aucun STA en NVS — WIFI_AP pur évite boucles d’auth sur certains ESP32)
    void begin(const char* apSsid, const char* apPass, const char* hostname, bool apOnlyMode = false);
    void connectSta(const char* staSsid, const char* staPass);
    void setStaticStaIp(IPAddress ip, IPAddress gateway, IPAddress subnet);
    void reconfigureMdns(const char* hostname);
    
    // Accès aux services
    AsyncWebServer& web();
    AsyncWebSocket& websocket();
    nidmi_core::RtpMidiService& rtpMidi();
    BluetoothManager& bluetooth();
    UsbMidiManager& usbMidi();
    
    // Mise à jour périodique
    void update();
};

/* ── ÉMETTRE, OU JETER — jamais empiler ────────────────────────────────────
 *
 * Trois chemins envoyaient sur la WebSocket sans jamais demander si quelqu'un
 * ecoutait ni si ce quelqu'un suivait : print()/graph(), la console de
 * debogage, la telemetrie de broche. Mesure (MESURES §84) : un script emettant
 * cent trames par seconde FERME la socket du client a 26 ms, code 1006, des
 * qu'il s'abonne a la console — le rattrapage d'historique et le flot de
 * textAll remplissent la meme file, et AsyncWebSocket ferme ce qui deborde.
 * L'app ne se reconnectait pas : le moniteur restait muet sans rien dire.
 *
 * Deux questions, dans cet ordre :
 *   1. count() == 0      personne n'ecoute. En headless — la cible — on ne
 *                        formate meme pas. C'est le §9.5 : rien ne part sans
 *                        abonnement d'un client.
 *   2. !availableForWriteAll()   quelqu'un ne suit pas. On JETTE la trame.
 *
 * Le second point est le coeur du correctif, et c'est un choix de conception :
 * ces flux sont des VISUALISATIONS. Perdre des points de courbe est correct —
 * l'oeil ne les verra pas ; perdre la connexion ne l'est pas. Empiler pour ne
 * rien perdre, c'est perdre tout.
 *
 * L'en-tete de la bibliotheque recommande exactement ces appels avant
 * d'envoyer (AsyncWebSocket.h, commentaire ligne 280). */
bool nidmi_ws_peut_emettre(AsyncWebSocket& ws);

// Note: L'instance globale serverCore est déclarée dans Globals.h
