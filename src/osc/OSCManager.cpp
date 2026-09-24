#ifndef debug_network
#define debug_network(...) ((void)0)
#endif
#include "OSCManager.h"
#include <WiFi.h>
#include "../server/ServerCore.h"   // LectureRadio : lire le WiFi sans courir apres une transition

OSCManager::OSCManager() : 
    targetIP(""),
    targetPort(8000),
    localPort(4000),
    initialized(false),
    enabled(false),
    broadcastEnabled(false),
    broadcastIP(""),
    networkInterface(OSC_INTERFACE_AP),
    messageCallback() {
}

OSCManager::~OSCManager() {
    end();
}

bool OSCManager::begin(const String& target_ip, uint16_t target_port, uint16_t local_port) {
    if (initialized) {
        end();
    }

    targetIP = target_ip;
    targetPort = target_port;
    localPort = local_port;

    // Démarrer UDP pour l'envoi
    if (!udp.begin(localPort)) {
        debug_network( "[OSC] Erreur: Impossible de démarrer UDP\n");
        return false;
    }

    initialized = true;
    enabled = true;

    // Calculer l'adresse broadcast selon le réseau actuel — sous le verrou
    // radio, sans attendre (voir ServerCore.h, « LE VERROU RADIO »).
    ServerCore::LectureRadio radio(0);
    if (radio.allumee() && WiFi.status() == WL_CONNECTED) {
        // Mode STA : broadcast du réseau WiFi
        IPAddress ip = WiFi.localIP();
        IPAddress subnet = WiFi.subnetMask();
        IPAddress broadcast = IPAddress(ip[0] | (~subnet[0]), 
                                       ip[1] | (~subnet[1]), 
                                       ip[2] | (~subnet[2]), 
                                       ip[3] | (~subnet[3]));
        broadcastIP = broadcast.toString();
    } else {
        // Mode AP : broadcast du réseau AP (192.168.4.255)
        broadcastIP = "192.168.4.255";
    }

    debug_network( "[OSC] Démarré - Local:%d -> %s:%d\n", 
                  localPort, targetIP.c_str(), targetPort);
    debug_network( "[OSC] Broadcast disponible: %s:%d\n", 
                  broadcastIP.c_str(), targetPort);
    debug_network( "[OSC] Interface réseau: %d (0=AP, 1=STA, 2=BOTH)\n", networkInterface);
    debug_network( "[OSC] Broadcast activé: %s\n", broadcastEnabled ? "OUI" : "NON\n");
    
    return true;
}

void OSCManager::end() {
    if (initialized) {
        udp.stop();
        initialized = false;
        enabled = false;
        debug_network( "[OSC] Arrêté\n");
    }
}

void OSCManager::setEnabled(bool enable) {
    enabled = enable && initialized;
    debug_network( "[OSC] %s\n", enabled ? "Activé" : "Désactivé\n");
}

bool OSCManager::isEnabled() const {
    return enabled && initialized;
}

bool OSCManager::sendFloat(const String& address, float value) {
    OSCMessage msg(address.c_str());
    msg.add(value);
    
    #ifdef NIDMI_debug_osc
    debug_network( "[OSC] Préparation %s %.3f\n", address.c_str(), value);
    #endif
    
    return sendOSCMessage(msg);
}

bool OSCManager::sendInt(const String& address, int value) {
    if (!isEnabled()) {
        return false;
    }

    OSCMessage msg(address.c_str());
    msg.add((intOSC_t)value);

    return sendOSCMessage(msg);
}

bool OSCManager::sendNote(const String& address, uint8_t note, uint8_t velocity) {
    if (!isEnabled()) {
        return false;
    }

    OSCMessage msg(address.c_str());
    msg.add((intOSC_t)note);
    msg.add((intOSC_t)velocity);

    return sendOSCMessage(msg);
}

bool OSCManager::sendMidiMessage(const String& address, uint8_t data1, uint8_t data2, uint8_t channel) {
    if (!isEnabled()) {
        return false;
    }

    OSCMessage msg(address.c_str());
    msg.add((intOSC_t)data1);    // Note/Control
    msg.add((intOSC_t)data2);    // Velocity/Value  
    msg.add((intOSC_t)channel);  // Canal MIDI

    return sendOSCMessage(msg);
}

bool OSCManager::sendMultiFloat(const String& address, float* values, int count) {
    if (!isEnabled() || count <= 0) {
        return false;
    }

    OSCMessage msg(address.c_str());
    for (int i = 0; i < count; i++) {
        msg.add(values[i]);
    }

    return sendOSCMessage(msg);
}

void OSCManager::setTarget(const String& target_ip, uint16_t target_port) {
    targetIP = target_ip;
    targetPort = target_port;
    
    debug_network( "[OSC] Nouvelle destination: %s:%d\n", 
                  targetIP.c_str(), targetPort);
}

String OSCManager::getTargetIP() const {
    return targetIP;
}

uint16_t OSCManager::getTargetPort() const {
    return targetPort;
}

bool OSCManager::isInitialized() const {
    return initialized;
}

void OSCManager::setBroadcast(bool enable) {
    broadcastEnabled = enable;
    debug_network( "[OSC] Broadcast %s\n", enable ? "activé" : "désactivé\n");
    if (enable && !broadcastIP.isEmpty()) {
        debug_network( "[OSC] Broadcast vers: %s:%d\n", broadcastIP.c_str(), targetPort);
    }
}

void OSCManager::setInterface(uint8_t interface) {
    networkInterface = interface;
    const char* interfaceNames[] = {"AP", "STA", "BOTH"};
    debug_network( "[OSC] Interface réseau configurée: %s\n", 
                 interface < 3 ? interfaceNames[interface] : "INVALID\n");
}

uint8_t OSCManager::getInterface() const {
    return networkInterface;
}

bool OSCManager::isBroadcastEnabled() const {
    return broadcastEnabled;
}

bool OSCManager::sendOSCMessage(OSCMessage& msg) {
    if (!isEnabled()) {
        debug_network( "[OSC] OSC désactivé\n");
        return false;
    }

    bool success = false;
    int retryCount = 0;
    const int maxRetries = 2;
    
    // Mode broadcast OU IP spécifique selon l'interface configurée
    if (broadcastEnabled) {
        // Mode broadcast selon l'interface réseau
        if (networkInterface == OSC_INTERFACE_AP || networkInterface == OSC_INTERFACE_BOTH) {
            // Broadcast sur AP (192.168.4.255)
            while (retryCount <= maxRetries && !success) {
                if (udp.beginPacket("192.168.4.255", targetPort)) {
                    msg.send(udp);
                    if (udp.endPacket()) {
                        success = true;
                        debug_network( "[OSC] Broadcast AP réussi (tentative %d)\n", retryCount + 1);
                    } else {
                        debug_network( "[OSC] Échec endPacket AP (tentative %d)\n", retryCount + 1);
                    }
                } else {
                    debug_network( "[OSC] Échec beginPacket AP (tentative %d)\n", retryCount + 1);
                }
                retryCount++;
            }
        }
        
        // Adresse de diffusion STA lue sous le verrou radio, SANS attendre : une
        // tache temps reel saute la diffusion plutot que d'attendre une
        // transition du WiFi (voir ServerCore.h, « LE VERROU RADIO »).
        bool staPret = false;
        IPAddress broadcast;
        if (networkInterface == OSC_INTERFACE_STA || networkInterface == OSC_INTERFACE_BOTH) {
            ServerCore::LectureRadio radio(0);
            if (radio.allumee() && WiFi.status() == WL_CONNECTED) {
                IPAddress ip = WiFi.localIP();
                IPAddress subnet = WiFi.subnetMask();
                broadcast = IPAddress(ip[0] | (~subnet[0]), ip[1] | (~subnet[1]),
                                      ip[2] | (~subnet[2]), ip[3] | (~subnet[3]));
                staPret = true;
            }
        }
        if (staPret) {
            // Broadcast sur STA (calculé selon le réseau)
            
            retryCount = 0;
            while (retryCount <= maxRetries && !success) {
                if (udp.beginPacket(broadcast, targetPort)) {
                    msg.send(udp);
                    if (udp.endPacket()) {
                        success = true;
                        debug_network( "[OSC] Broadcast STA réussi (tentative %d)\n", retryCount + 1);
                    } else {
                        debug_network( "[OSC] Échec endPacket STA (tentative %d)\n", retryCount + 1);
                    }
                } else {
                    debug_network( "[OSC] Échec beginPacket STA (tentative %d)\n", retryCount + 1);
                }
                retryCount++;
            }
        }
    } else {
        // Mode IP spécifique
        if (!targetIP.isEmpty()) {
            while (retryCount <= maxRetries && !success) {
                if (udp.beginPacket(targetIP.c_str(), targetPort)) {
                    msg.send(udp);
                    if (udp.endPacket()) {
                        success = true;
                        debug_network( "[OSC] Unicast réussi (tentative %d)\n", retryCount + 1);
                    } else {
                        debug_network( "[OSC] Échec endPacket unicast (tentative %d)\n", retryCount + 1);
                    }
                } else {
                    debug_network( "[OSC] Échec beginPacket unicast (tentative %d)\n", retryCount + 1);
                }
                retryCount++;
            }
        }
    }
    
    if (!success) {
        debug_network( "[OSC] Échec définitif après %d tentatives\n", maxRetries + 1);
    }
    
    return success;
}

void OSCManager::update() {
    if (!isEnabled() || !initialized) {
        return;
    }

    // Vérifier s'il y a des paquets entrants
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
        Serial.printf("[OSC] Paquet reçu: %d bytes\n", packetSize);
        
        // Lire le paquet entrant
        uint8_t buffer[256];
        int len = udp.read(buffer, sizeof(buffer) - 1);
        if (len > 0) {
            buffer[len] = 0; // Null terminate
            
            // Debug: afficher les premiers bytes du buffer
            Serial.printf("[OSC] Buffer brut (premiers 32 bytes): ");
            for (int i = 0; i < len && i < 32; i++) {
                Serial.printf("%02X ", buffer[i]);
            }
            Serial.println();
            
            // Debug: essayer de lire comme string (pour voir l'adresse)
            Serial.printf("[OSC] Buffer comme string: '%s'\n", (char*)buffer);
            
            // Parser le message OSC
            OSCMessage msg;
            msg.fill(buffer, len);
            
            if (!msg.hasError()) {
                // Essayer différentes méthodes pour obtenir l'adresse
                const char* addr_ptr = msg.getAddress();
                Serial.printf("[OSC] getAddress() retourne: '%s'\n", addr_ptr ? addr_ptr : "(null)");
                
                // Extraire l'adresse dans un buffer
                char addressBuffer[64];
                addressBuffer[0] = '\0';
                msg.getAddress(addressBuffer, sizeof(addressBuffer));
                Serial.printf("[OSC] getAddress(buffer) remplit: '%s'\n", addressBuffer);
                
                String address = String(addr_ptr ? addr_ptr : addressBuffer);
                Serial.printf("[OSC] Message reçu: '%s'\n", address.c_str());
                
                // Appeler le callback si défini (avec ou sans valeur)
                if (messageCallback) {
                    float value = -1.0f; // -1 = sentinelle pour "pas de valeur" (tous les canaux)
                    String arg_string = "";
                    
                    if (msg.size() > 0) {
                        if (msg.isFloat(0)) {
                            // Message avec valeur float
                            value = msg.getFloat(0);
                            Serial.printf("[OSC] Appel callback avec valeur float: %f\n", value);
                        } else if (msg.isString(0)) {
                            // Message avec argument string
                            char strBuffer[64];
                            msg.getString(0, strBuffer, sizeof(strBuffer));
                            arg_string = String(strBuffer);
                            Serial.printf("[OSC] Appel callback avec argument string: '%s'\n", arg_string.c_str());
                        } else if (msg.isInt(0)) {
                            // Message avec valeur(s) int
                            // Format MIDI (3 int) : data1, data2, channel
                            // Format simple (1 int) : value
                            if (msg.size() >= 3 && msg.isInt(0) && msg.isInt(1) && msg.isInt(2)) {
                                // Format MIDI : construire "param,channel" depuis data1 et channel
                                int32_t data1 = msg.getInt(0);  // note/CC
                                int32_t data2 = msg.getInt(1);  // velocity/value
                                int32_t channel = msg.getInt(2); // canal MIDI
                                value = (float)data2; // Utiliser data2 comme valeur principale
                                arg_string = String((int)data1) + "," + String((int)channel);
                                Serial.printf("[OSC] Format MIDI: data1=%d, data2=%d, channel=%d\n", 
                                            (int)data1, (int)data2, (int)channel);
                            } else {
                                // Format simple (1 int)
                                value = (float)msg.getInt(0);
                                Serial.printf("[OSC] Appel callback avec valeur int: %d\n", (int)value);
                            }
                        } else {
                            Serial.printf("[OSC] Appel callback avec argument de type inconnu, size=%d\n", msg.size());
                        }
                    } else {
                        Serial.printf("[OSC] Appel callback sans argument (commande), size=%d\n", msg.size());
                    }
                    
                    messageCallback(address, value, arg_string);
                } else {
                    Serial.printf("[OSC] ERREUR: callback non défini!\n");
                }
            } else {
                Serial.printf("[OSC] ERREUR parsing message OSC: code=%d\n", msg.getError());
            }
        } else {
            Serial.printf("[OSC] ERREUR: impossible de lire le paquet\n");
        }
    }
}

void OSCManager::setMessageCallback(OSCMessageCallback callback) {
    messageCallback = callback;
    #ifdef NIDMI_debug_osc
    debug_network( "[OSC] Callback %s\n", callback ? "défini" : "supprimé\n");
    #endif
}

void OSCManager::printStatus() const {
    debug_network( "=== OSC Manager Status ===\n");
    debug_network( "Initialisé: %s\n", initialized ? "Oui" : "Non\n");
    debug_network( "Activé: %s\n", enabled ? "Oui" : "Non\n");
    debug_network( "Destination: %s:%d\n", targetIP.c_str(), targetPort);
    debug_network( "Broadcast: %s\n", broadcastEnabled ? "Oui" : "Non\n");
    debug_network( "Callback: %s\n", messageCallback ? "Défini" : "Non défini\n");
    debug_network( "=========================\n");
}

void OSCManager::disconnect() {
    // UDP n'a pas besoin de déconnexion explicite
    debug_network("[OSC] UDP arrêté\n");
}
