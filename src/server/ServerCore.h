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
    /* Retenus au begin() pour que la radio puisse etre allumee PLUS TARD, sans
     * que l'appelant ait a garder ces chaines vivantes. Voir demarrerRadioWifi(). */
    String apSsidRetenu, apPassRetenu;
    bool   apOnlyRetenu = false;
    bool   radioAllumee = false;
    // La coupure preparee (voir coupureSure()).
    volatile unsigned long dernierEvenementReseauA = 0;
    unsigned long coupurePrepareeA = 0, coupureDemandeeA = 0;
    void mdnsInterfacesWifi(bool actives);
    
public:
    ServerCore();
    
    // Initialisation (apOnlyMode: true si aucun STA en NVS — WIFI_AP pur évite boucles d’auth sur certains ESP32)
    void begin(const char* apSsid, const char* apPass, const char* hostname, bool apOnlyMode = false);

    /* ── LA RADIO WIFI, SEPAREE DU RESTE ───────────────────────────────────
     * begin() faisait trois choses d'un bloc : allumer la radio, publier le
     * mDNS, installer le serveur web. On la separe pour pouvoir la COUPER et la
     * RALLUMER en marche — AsyncWebServer ecoute sur INADDR_ANY, il continue
     * donc de servir sur le netif USB quand le WiFi est coupe.
     *
     * Idempotent : un second appel ne fait rien. C'est ce qui permet au repli
     * de l'appeler sans savoir si la radio est deja la. */
    void demarrerRadioWifi();
    void couperRadioWifi();
    bool radioWifiAllumee() const { return radioAllumee; }

    /* ── COUPER SANS FAIRE PLANTER LE mDNS (MESURES §161) ─────────────────
     * Le mDNS traite les evenements reseau PLUS TARD, dans sa tache : coupee
     * entre-temps, la radio detruit l'interface qu'une action « activer »
     * attend encore — _mdns_enable_pcb puis esp_netif_is_netif_up(NULL),
     * LoadProhibited, la carte redemarre (vu en rallumant/coupant toutes les
     * 10 s). Avant de couper : desactiver le mDNS sur les interfaces WiFi
     * (ces demandes passent APRES les activations en attente), puis attendre
     * 1,5 s sans nouvel evenement reseau. Vrai quand on peut couper — ou que
     * la radio l'est deja. A appeler a chaque tour tant qu'on veut couper :
     * une preparation delaissee rend le mDNS aux interfaces (update()). */
    bool coupureSure();
    /** Un evenement qui fait AGIR le mDNS (connexion, adresse, AP) vient
     *  d'arriver : la preparation d'une coupure recommence. */
    void noterEvenementReseau() { dernierEvenementReseauA = millis(); }

    /* ── LE VERROU RADIO ──────────────────────────────────────────────────
     * WiFi.mode(WIFI_OFF) detruit les interfaces AP et STA (wifiLowLevelDeinit),
     * attend esp_wifi_deinit(), et SEULEMENT ENSUITE remet a NULL les pointeurs
     * que lisent WiFi.softAPIP(), WiFi.localIP()… (WiFiGeneric.cpp, core 3.3.5).
     * Pendant cette attente, une autre tache qui lit l'etat du WiFi lit une
     * interface DETRUITE, dont la memoire a deja pu servir a autre chose : vu,
     * async_tcp dans /api/status, LoadProhibited (MESURES §149). La bascule
     * « cable prioritaire » rend ces transitions courantes.
     * Les transitions tiennent ce verrou ; toute lecture d'un etat WiFi depuis
     * une AUTRE tache que loopTask le prend aussi, le temps de ses lectures.
     * attente 0 : ne jamais attendre — ce qu'une tache temps reel doit faire. */
    class LectureRadio {
    public:
        explicit LectureRadio(TickType_t attente);
        ~LectureRadio();
        /** Verrou pris ET radio allumee : les lectures WiFi.* sont sures. */
        bool allumee() const;
        LectureRadio(const LectureRadio&) = delete;
        LectureRadio& operator=(const LectureRadio&) = delete;
    private:
        bool pris;
    };
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
 *   1. personne n'ecoute : en headless — la cible — on ne formate meme pas.
 *      C'est le §9.5 : rien ne part sans abonnement d'un client.
 *   2. un onglet ne suit pas (sa file est pleine) : on JETTE la trame, pour
 *      LUI seulement.
 *
 * Le second point est un choix de conception : ces flux sont des
 * VISUALISATIONS. Perdre des points de courbe est correct — l'oeil ne les
 * verra pas ; perdre la connexion ne l'est pas. Empiler pour ne rien perdre,
 * c'est perdre tout. */

/* ── LES ONGLETS : UN REGISTRE A NOUS, SOUS VERROU (MESURES §171) ──────────
 *
 * AsyncWebSocket garde ses clients dans un std::list sans verrou (verifie
 * ligne a ligne, 3.9.4), et elle le MODIFIE dans sa tache, async_tcp : un
 * client arrive (emplace_back), un client part (erase — onglet ferme, page
 * rechargee, delai depasse). La regle posee ici disait « on draine depuis
 * loopTask, la tache qui appelle cleanupClients() » : elle ecartait la course
 * avec MidiTask, pas celle-la. loopTask (1) et async_tcp (10) partagent le
 * coeur 1, et async_tcp la preempte a n'importe quelle instruction — au milieu
 * d'un parcours de la liste. Et la bibliotheque avait sa propre course : un
 * client dont la file debordait etait FERME par textAll(), donc retire de la
 * liste que textAll() parcourait.
 * Constate le 28/09 : panique sur le coeur 1, loopTask dans
 * availableForWriteAll(), le verrou d'un client deja rendu (MESURES §171).
 *
 * On ne touche plus a la liste de la bibliotheque, d'aucune tache. Les
 * onglets sont dans NOTRE registre : inscrits a WS_EVT_CONNECT, rayes a
 * WS_EVT_DISCONNECT — deux evenements qu'async_tcp emet lui-meme, le second
 * depuis le destructeur du client, AVANT que sa memoire ne soit rendue. Un
 * verrou (mutex, heritage de priorite) couvre le registre ET chaque envoi : un
 * client en cours de destruction attend, dans son destructeur, que l'envoi en
 * cours finisse ; il est deja deconnecte, text() rend faux sans rien toucher.
 * Aucun client n'est plus ferme sur debordement (setCloseClientOnQueueFull) :
 * sa trame est jetee, la connexion vit. Et cleanupClients() n'est plus
 * appele : en 3.9.4 la bibliotheque retire elle-meme un client parti, et la
 * borne du nombre d'onglets se pose a l'arrivee, dans sa tache.
 *
 * Regle, toujours : ON POUSSE depuis n'importe quelle tache, ON DRAINE depuis
 * loopTask — la file garde le reseau hors des taches temps reel. Elle est en
 * PSRAM : le plus gros bloc contigu est la ressource rare de cette carte. */
bool nidmi_ws_quelqu_un_ecoute();          // un octet : lisible de partout, sans verrou
bool nidmi_ws_pousser(const char* trame);  // depuis N'IMPORTE QUELLE tache
void nidmi_ws_drainer();                   // loopTask UNIQUEMENT
/* L'envoi lui-meme, sous le verrou du registre — pour les drains de loopTask.
 * A tous : chaque onglet qui suit la recoit, les autres non. */
void nidmi_ws_envoyer_a_tous(const char* texte);
/* A un seul, par son IDENTIFIANT (jamais un pointeur gardé : il pend des que
 * l'onglet part). 1 = envoye, 0 = sa file est pleine (repasser), -1 = parti. */
int  nidmi_ws_envoyer_a(uint32_t id, const char* texte);
uint32_t nidmi_ws_trames_jetees();         // ce qu'on a perdu, pour le dire
void nidmi_ws_file_init();                 // appele par ServerCore::begin()
void nidmi_ws_client_arrive(AsyncWebSocketClient* client);   // WS_EVT_CONNECT (async_tcp)
void nidmi_ws_client_parti(AsyncWebSocketClient* client);    // WS_EVT_DISCONNECT (async_tcp)

/* ── QUI EST CONNECTE (MESURES §199) ────────────────────────────────────────
 *
 * Le registre savait combien d'onglets il y avait, pas lesquels. Chaque onglet
 * DIT maintenant ce qu'il est — « CLIENT:<jeton>|<nom>|<mode>|<visible>|<rev> »,
 * une trame a l'ouverture de sa socket puis une par changement (il passe en
 * arriere-plan, il change de mode, il reprend une composition) : jamais un
 * sondage. La carte y ajoute ce qu'elle sait seule — son adresse, si c'est le
 * cable ou le WiFi, depuis quand — et le rend a la demande (GET /api/clients).
 * Elle n'annonce que « la liste a change » (NIDMI_CLIENTS:<n>:<generation>) : la
 * trame est minuscule, et l'onglet qui n'affiche pas la liste n'a rien a lire.
 *
 * Les informations sont en PSRAM (une quinzaine d'octets par onglet n'ont pas a
 * peser sur le plus gros bloc contigu), sous le verrou du registre. */
void nidmi_ws_client_etat(uint32_t id, const char* texte);     // async_tcp : ce que l'onglet dit de lui
void nidmi_ws_clients_ecrire(String& j);                       // « "n":…,"generation":…,"clients":[…] », sans accolades

// Note: L'instance globale serverCore est déclarée dans Globals.h
