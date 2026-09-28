#pragma once

#include <Arduino.h>
#include <WiFiUdp.h>
#include <OSCMessage.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Structure pour les messages OSC en queue.
/* A PLAT (MESURES §176) : une file FreeRTOS copie ses elements OCTET PAR OCTET.
 * L'adresse etait une String : au-dela de 14 caractères elle pointe le tas, la
 * copie en file gardait ce pointeur, l'original le rendait aussitot — et la
 * lecture le rendait une seconde fois. Les adresses par defaut (« /ctl »)
 * tenaient dans la String elle-meme : le defaut ne s'etait pas vu. */
struct OSCMessageItem {
    char address[48];
    float value;
    float value2; // Deuxième valeur float (pour canal MUX)
    uint8_t data1;
    uint8_t data2;
    uint8_t channel;
    uint8_t messageType; // 0=float, 1=MIDI, 2=float2
    uint32_t timestamp;
};

class OSCQueue {
public:
    OSCQueue();
    ~OSCQueue();

    bool begin();
    void end();
    
    // Ajouter des messages à la queue (non-bloquant)
    bool enqueueFloat(const String& address, float value);
    bool enqueueFloat2(const String& address, float value1, float value2);
    bool enqueueMidi(const String& address, uint8_t data1, uint8_t data2, uint8_t channel);
    
    // Envoyer un tableau de valeurs (pour MUX batch, envoi direct sans queue)
    bool enqueueFloatArray(const String& address, const float* values, int count);
    bool enqueueIntArray(const String& address, const uint16_t* values, int count);  // Raw (0-4095)
    bool enqueueMidiArray(const String& address, const uint8_t* values, int count); // MIDI (0-127)
    
    // Traiter la queue (à appeler dans loop())
    void update();
    
    // Configuration
    void setTarget(const String& target_ip, uint16_t target_port);
    void setBroadcast(bool enable);
    void setInterface(uint8_t interface);
    
    // Statistiques
    uint32_t getQueueSize() const;
    uint32_t getSentCount() const;
    uint32_t getFailedCount() const;
    void resetStats();
    
    // Diagnostic réseau
    void printNetworkStatus() const;
    void printDetailedStats() const;

private:
    void processQueue();
    bool sendOSCMessage(OSCMessage& msg);
    
private:
    QueueHandle_t messageQueue;
    StaticQueue_t messageQueueTcb;   // la structure en RAM interne, le stockage en PSRAM
    uint8_t* stockage = nullptr;     // pris une fois, garde d'un begin() a l'autre
    WiFiUDP udp;
    String targetIP;
    uint16_t targetPort;
    bool initialized;
    bool broadcastEnabled;
    uint8_t networkInterface;
    
    // Statistiques
    uint32_t sentCount;
    uint32_t failedCount;
    
    static const int QUEUE_SIZE = 32;
    static const int MAX_RETRIES = 2;
};
