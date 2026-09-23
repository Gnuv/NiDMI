#include "NiDMI.h"
#include <soc/rtc_cntl_reg.h>
#include "server/ServerCore.h"
#include "managers/ComponentManager.h"
#include "utils/PinMapper.h"
#include "midi/MidiRouter.h"
#include "midi/CcMap.h"
#include "mapping/MappingEngine.h"
#include "network/UsbMidiManager.h"
#include "network/UsbNetBootstrap.h"
#include <esp_heap_caps.h>
#include <esp_system.h>
#include "server/WebDebugConsole.h"
#include "Globals.h"
#include <Preferences.h>
#include <WiFi.h>
#include "audio/AudioEngine.h"
#include "mapping/CueStore.h"
#if defined(NIDMI_USB_MIDI_SUPPORTED) && NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME
#include <esp32-hal-tinyusb.h>
#endif

// Variables globales pour la gestion des composants
MidiRouter g_midiRouter;
ComponentManager g_componentManager;

// Configuration STA mise en cache pour permettre une reconnexion automatique
static String g_staSsid;
static String g_staPass;
static String g_staIpStr;
static String g_staGwStr;
static String g_staSnStr;

// Gestion de la reconnexion STA (backoff exponentiel pour éviter le spam série
// et les tentatives inutiles quand le réseau est durablement absent)
static unsigned long g_lastStaConnectAttempt = 0;
static const unsigned long STA_RECONNECT_BASE_MS = 10000;  // 1re tentative après 10 s
static const unsigned long STA_RECONNECT_MAX_MS  = 60000;  // plafond du backoff
/* ── LE CABLE OU LE WIFI ─────────────────────────────────────────────────────
 * Une premiere version coupait le WiFi SUR COMMANDE, avec un repli : lien USB
 * bas 20 s -> radio rallumee. Le repli croyait linkUp(). Or linkUp() a ete vu
 * VRAI sur un lien MORT, deux fois (§140, §143) : les deux bouts disent
 * « monte », plus aucune trame ne passe. Couper le WiFi dans cet etat rendait
 * la carte injoignable jusqu'a ce qu'on la debranche. Cette coupure est
 * RETIREE tant que le lien USB n'a pas de preuve de vie qui ne mente pas.
 *
 * Reste L'ESSAI : couper le WiFi N secondes, mesurer, le rallumer — sur son
 * SEUL minuteur, sans rien attendre du lien USB. Il marche donc sur tous les
 * builds, et le pire cas est N secondes sans reseau. Rien n'est memorise. */
static volatile bool g_wifiRallumerDemande = false;
static unsigned long g_wifiRallumerA = 0;
static volatile bool g_essaiDemande = false;
static unsigned long g_essaiDemandeA = 0;
static unsigned long g_essaiDureeDemandee = 10000;
struct EssaiWifi {
    bool enCours = false, fait = false, mesurePendant = false, mesureApres = false;
    unsigned long debut = 0, fin = 0, dureeMs = 0;
    uint32_t blocAvant = 0,   tasAvant = 0;
    uint32_t blocPendant = 0, tasPendant = 0;
    uint32_t blocApres = 0,   tasApres = 0;
};
static EssaiWifi g_essai;
static unsigned long g_staReconnectInterval = STA_RECONNECT_BASE_MS;
static bool g_staWasConnected = false;  // pour logguer les transitions STA (visibilité)

// Version du schéma de données NVS. À INCRÉMENTER quand le format stocké en NVS
// change de façon incompatible (clés/types JSON, layout des blobs mux, etc.).
// Au boot, si la version stockée diffère, la config (pins/mux/mappings) est
// réinitialisée mais le réseau (STA + mDNS) est préservé -> évite de charger
// d'anciennes données dans une nouvelle structure (crash / valeurs fausses).
static const uint32_t NIDMI_NVS_SCHEMA_VERSION = 1;

// Demande de rechargement des configs pins depuis l'API (débounce 500 ms pour grouper les sauvegardes séquentielles)
static volatile bool g_requestReloadPins = false;
static unsigned long g_reloadRequestTime = 0;
extern "C" void nidmi_requestReloadPins(){
    g_requestReloadPins = true;
    g_reloadRequestTime = millis();
}

// Redémarrage différé (depuis la loop, pas depuis le handler HTTP — évite de couper la NVS en plein écriture)
static volatile bool g_requestReboot = false;
static unsigned long g_rebootRequestTime = 0;
/* Le redemarrage etait CONFIE A LA BOUCLE. Cela marche tant que la boucle
 * tourne — mais apres un OTA, elle ne tourne plus : l'ecriture de l'image
 * laisse le coeur applicatif fige (journal : « [OTA] Image validee,
 * redemarrage... » puis plus rien, tache audio comprise), si bien que
 * ESP.restart() n'etait jamais atteint. La carte restait en vie par sa seule
 * pile reseau : elle repondait en HTTP, mais ne redemarrait pas, ne rechargeait
 * pas ses configs, et l'image fraichement ecrite n'etait jamais lancee. Il
 * fallait la debrancher.
 *
 * On confie donc le redemarrage a une TACHE DEDIEE, qui dort deux secondes —
 * le temps que la reponse HTTP parte — puis redemarre. Elle ne depend de rien
 * d'autre. La boucle garde son propre chemin pour les cas ordinaires. */
static void tacheRedemarrage(void*) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

/* ── QUI A DEMANDE LE REDEMARRAGE ─────────────────────────────────────────
 * Constate le 23/09 : la carte a redemarre seule, sans le son, et personne ne
 * savait pourquoi. reset_reason disait « logiciel » — donc on le lui avait
 * DEMANDE — mais rien ne disait qui. Trois chemins le peuvent (identifiants
 * WiFi, flash, redemarrage par l'API), et la carte n'en gardait aucune trace.
 *
 * La RTC RAM survit a un redemarrage logiciel (pas a une coupure de courant) :
 * on y depose QUI a demande, au moment de la demande, et le demarrage suivant
 * le lit puis l'efface. Meme procede que la phase des broches (§42). Si le
 * redemarrage suivant n'a ete demande par personne — panique, chien de garde —
 * il n'y aura rien a lire : c'est voulu, ce n'etait pas une demande. */
RTC_NOINIT_ATTR static char     s_redemParRtc[48];
RTC_NOINIT_ATTR static uint32_t s_redemMagie;
RTC_NOINIT_ATTR static uint32_t s_aVideMagie;
static const uint32_t REDEM_MAGIE = 0x52454445UL;   // "REDE"
static const uint32_t AVIDE_MAGIE = 0x56494445UL;   // "VIDE"
static char s_redemPar[48] = "";
static bool s_demarreAVide = false;

static void noterDemandeur(const char* par) {
    strlcpy(s_redemParRtc, (par && *par) ? par : "?", sizeof(s_redemParRtc));
    s_redemMagie = REDEM_MAGIE;
}

/* Au tout debut de nidmi_begin(), avant que quoi que ce soit ne redemarre. */
static void capturerDemandeur() {
    if (s_redemMagie == REDEM_MAGIE) {
        s_redemParRtc[sizeof(s_redemParRtc) - 1] = '\0';
        strlcpy(s_redemPar, s_redemParRtc, sizeof(s_redemPar));
    } else {
        s_redemPar[0] = '\0';
    }
    s_redemMagie = 0;   // consomme : un redemarrage NON demande ne le re-affichera pas
}

extern "C" const char* nidmi_redemarrageDemandePar() { return s_redemPar; }

/* ── DEMARRER A VIDE, UNE FOIS ──────────────────────────────────────────────
 * « Decharger le moteur et redemarrer » ecrivait « aucun moteur » en NVS : la
 * carte repartait sans le son, et RESTAIT sans le son a tous les demarrages
 * suivants, jusqu'a ce que quelqu'un le rende a la main. Une carte qui se tait
 * pour toujours apres un geste de depannage est un piege sur scene.
 *
 * Desormais le demarrage a vide vaut pour UN demarrage : un drapeau en RTC,
 * consomme par restaurerAuBoot(). La NVS n'est pas touchee — le choix du son
 * reste celui de l'utilisateur, et il revient au redemarrage suivant. */
extern "C" void nidmi_demanderDemarrageAVide() { s_aVideMagie = AVIDE_MAGIE; }
extern "C" bool nidmi_prendreDemarrageAVide() {
    const bool oui = (s_aVideMagie == AVIDE_MAGIE);
    s_aVideMagie = 0;
    if (oui) s_demarreAVide = true;
    return oui;
}
extern "C" bool nidmi_demarreAVide() { return s_demarreAVide; }

/* ── LA SANTE DE LA CARTE ────────────────────────────────────────────────────
 * « Indiquer qu'il y a un souci suffit ; on rentre dans les reglages pour les
 * details. » La CARTE decide s'il y a un souci — l'app ne fait que l'afficher.
 * Recalculer ces seuils cote navigateur, c'est deux verites qui divergent.
 *
 * Et elle l'ANNONCE quand ca change, sans se faire sonder : sonder est ce qui
 * la fait giguer (MESURES §82). Meme chemin que l'annonce des cues.
 *
 * Chaque cause, et pourquoi ce seuil :
 *   son_coupe    le garde-fou de demarrage a coupe l'audio (§138).
 *   decrochages  une sous-alimentation audio — un craquement. Tenue 60 s :
 *                un decrochage dure quelques ms, il faut avoir le temps de le voir.
 *   charge       rendu audio a 85 % du budget ou plus.
 *   memoire      le TOTAL libre est descendu sous 3 072 o depuis l'allumage —
 *                le seuil de la jauge « Creux », mesure : en dessous, des
 *                requetes se perdent. PAS le plus gros bloc : avec le son et un
 *                onglet ouvert, il est normalement a ~8 000, et un voyant qui
 *                s'allume en usage normal ne signale plus rien.
 *   reprises     une reprise refusee, file pleine. Tenue 60 s.
 *   plantage     le demarrage en cours fait suite a une panique, un chien de
 *                garde ou une chute de tension.
 * Un octet, ecrit par la boucle seule et lu par les routes : pas de chaine
 * partagee entre deux taches (§20, piege 11). */
enum : uint8_t {
    SANTE_SON_COUPE = 1, SANTE_DECROCHAGES = 2, SANTE_CHARGE = 4,
    SANTE_MEMOIRE = 8,   SANTE_REPRISES = 16,   SANTE_PLANTAGE = 32
};
static volatile uint8_t s_sante = 0;
static unsigned long s_santeProchain = 0, s_decrochagesJusqua = 0, s_reprisesJusqua = 0;
static uint32_t s_sousAlimAvant = 0, s_refusAvant = 0;

extern "C" uint8_t nidmi_sante() { return s_sante; }
extern "C" void nidmi_santeTexte(uint8_t f, char* out, unsigned n) {
    static const struct { uint8_t bit; const char* nom; } CAUSES[] = {
        { SANTE_SON_COUPE, "son_coupe" }, { SANTE_DECROCHAGES, "decrochages" },
        { SANTE_CHARGE, "charge" },       { SANTE_MEMOIRE, "memoire" },
        { SANTE_REPRISES, "reprises" },   { SANTE_PLANTAGE, "plantage" } };
    if (!out || !n) return;
    out[0] = '\0';
    for (const auto& c : CAUSES) {
        if (!(f & c.bit)) continue;
        if (out[0]) strlcat(out, ",", n);
        strlcat(out, c.nom, n);
    }
}

static void verifierSante() {
    const unsigned long maintenant = millis();
    if ((long)(maintenant - s_santeProchain) < 0) return;
    s_santeProchain = maintenant + 1000;

    const AudioEngine::Metriques m = AudioEngine::metriques();
    uint16_t enCours = 0, maxVu = 0, capacite = 0; uint32_t refus = 0;
    MappingEngine::statsReprises(enCours, maxVu, refus, capacite);
    if (m.sousAlimentations > s_sousAlimAvant) s_decrochagesJusqua = maintenant + 60000;
    s_sousAlimAvant = m.sousAlimentations;
    if (refus > s_refusAvant) s_reprisesJusqua = maintenant + 60000;
    s_refusAvant = refus;

    const esp_reset_reason_t r = esp_reset_reason();
    uint8_t f = 0;
    if (m.bootCoupe)                                          f |= SANTE_SON_COUPE;
    if ((long)(s_decrochagesJusqua - maintenant) > 0)         f |= SANTE_DECROCHAGES;
    if (m.cyclesParEch * 100u >= 85u * 5000u)                 f |= SANTE_CHARGE;
    if (heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) < 3072) f |= SANTE_MEMOIRE;
    if ((long)(s_reprisesJusqua - maintenant) > 0)            f |= SANTE_REPRISES;
    if (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT
        || r == ESP_RST_WDT || r == ESP_RST_BROWNOUT)         f |= SANTE_PLANTAGE;

    if (f == s_sante) return;
    s_sante = f;
    if (!nidmi_ws_quelqu_un_ecoute()) return;
    char trame[96] = "NIDMI_SANTE:";
    nidmi_santeTexte(f, trame + 12, sizeof(trame) - 12);
    nidmi_ws_pousser(trame);
}

extern "C" void nidmi_requestReboot(const char* par){
    noterDemandeur(par);
    g_rebootRequestTime = millis();
    g_requestReboot = true;
    // Filet de securite : si la boucle est morte, cette tache redemarre quand meme.
    xTaskCreate(tacheRedemarrage, "reboot", 2048, nullptr, configMAX_PRIORITIES - 2, nullptr);
}

// Mode téléchargement (bootloader ROM), demandé par l'API. Même différé que le
// reboot : bloquer ici bloquerait async_tcp, donc la réponse HTTP ne partirait
// jamais — c'est ce qui s'était passé au premier essai.
static volatile bool g_requestDownload = false;
extern "C" void nidmi_requestDownloadMode(const char* par){
    noterDemandeur(par);
    g_rebootRequestTime = millis();
    g_requestDownload = true;
}

// Le cable OU le WiFi — voir « LE CABLE OU LE WIFI ».
extern "C" void nidmi_requestRallumerWifi(){
    g_wifiRallumerA = millis();
    g_wifiRallumerDemande = true;
}
extern "C" void nidmi_requestEssaiWifi(unsigned long dureeMs){
    g_essaiDureeDemandee = dureeMs;
    g_essaiDemandeA = millis();
    g_essaiDemande = true;
}
String nidmi_essaiWifiJson(){
    const EssaiWifi e = g_essai;
    String j = "{\"etat\":\"";
    j += e.enCours ? "wifi_coupe" : (e.fait ? (e.mesureApres ? "fait" : "retour") : "jamais");
    j += "\",\"duree_ms\":" + String(e.dureeMs);
    j += ",\"avant\":{\"bloc\":"      + String(e.blocAvant)   + ",\"tas\":" + String(e.tasAvant)   + "}";
    j += ",\"wifi_coupe\":{\"bloc\":" + String(e.blocPendant) + ",\"tas\":" + String(e.tasPendant) + "}";
    j += ",\"apres\":{\"bloc\":"      + String(e.blocApres)   + ",\"tas\":" + String(e.tasApres)   + "}}";
    return j;
}

// Le mapping GPIO est maintenant géré par PinMapper

// Charger configuration pins depuis NVS
void loadPinConfigs() {
    // Le ComponentManager gère maintenant le chargement des configurations
    g_componentManager.reloadConfigs();
}

// Traitement des composants dans la boucle
void processComponents() {
    // Le ComponentManager gère maintenant tous les composants
    g_componentManager.update();
}

// Diagnostic touch au boot : mettre à 1 pour activer, 0 pour désactiver.
// Surchargeable au build : -DTOUCH_BOOT_DIAG=0 desactive le diagnostic.
// GPIO1 est AUSSI le BCK de l'I2S (AudioEngine.h) : touchRead(1) bascule la
// broche en mode RTC, et rien ne garantit qu'un i2s.begin() ulterieur la
// reprenne. Piste testee pour le blocage de la tache audio.
// 0 PAR DEFAUT : le diagnostic sonde GPIO1 (T1) au boot, or GPIO1 est le BCK de
// l'I2S sur la maquette audio. touchRead() capture la broche au peripherique
// tactile et l'I2S ne peut plus la cadencer -> tache audio figee (MESURES.md
// §19). Le remede par touch_pad_deinit() crashait (conflit legacy/new driver) :
// on NE SONDE simplement PAS. Reactiver avec -DTOUCH_BOOT_DIAG=1 sur une carte
// sans audio sur ces broches.
#ifndef TOUCH_BOOT_DIAG
#define TOUCH_BOOT_DIAG 0
#endif

static void touchDiag(const char* label) {
#if TOUCH_BOOT_DIAG && (defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ESP32S3_DEV) || defined(ARDUINO_ESP32S3))
    Serial.printf("[TOUCH DIAG] %s: ", label);
    for (int i = 0; i < 5; i++) {
        uint32_t v = touchRead(1); // GPIO1 = T1
        Serial.printf("%lu ", (unsigned long)v);
        delay(50);
    }
    Serial.println();
#else
    (void)label;
#endif
}

void nidmi_begin() {
    /* AVANT tout chargement : lire la phase laissee par la vie precedente,
     * sinon le premier marquer() de ce demarrage l'ecraserait. */
    ComponentManager::capturerPhasePrecedente();
    capturerDemandeur();
    Serial.begin(115200);
    delay(50);

    touchDiag("AVANT tout (juste apres Serial)");

    // Ne pas laisser la pile WiFi relire/écrire une config STA/AP dans la NVS système
    // (notre SSID AP et STA viennent du namespace Preferences "nidmi").
    WiFi.persistent(false);

    // Détecter et afficher le MCU
    PinMapper::detectMcu();
    PinMapper::printMappings();

    // Nettoyer les anciens réglages NVS si nécessaire
    // (décommentez la ligne suivante pour forcer le reset)
    // Preferences::clear("nidmi\n\n");

    // Lire nom serveur + STA depuis NVS (ne pas appeler get* si begin a échoué)
    Preferences preferences;
    String serverName = "nidmi";
    g_staSsid = "";
    g_staPass = "";
    g_staIpStr = "";
    g_staGwStr = "";
    g_staSnStr = "";
    bool touchEnabled = false;
    uint32_t storedSchema = 0;
    const bool usbMidiEnabled = nidmi_usb_midi_enabled_at_compile_time();

    if (preferences.begin("nidmi", true)) {
        serverName = preferences.getString("mdns_name", "nidmi");
        g_staSsid = preferences.getString("sta_ssid", "");
        g_staPass = preferences.getString("sta_pass", "");
        g_staIpStr = preferences.getString("sta_ip", "");
        g_staGwStr = preferences.getString("sta_gw", "");
        g_staSnStr = preferences.getString("sta_sn", "");
        touchEnabled = preferences.getBool("touch_enabled", false);
        storedSchema = preferences.getUInt("nvs_schema", 0);
        preferences.end();
    } else {
        Serial.println("[NiDMI] ERREUR: ouverture NVS en lecture échouée - NVS peut être corrompue");
        Serial.println("[NiDMI] Utilisation des valeurs par défaut. Flashez nidmi_clear_nvs pour réinitialiser.");
    }
    
    // Nettoyer le nom serveur (supprimer caractères invalides pour SSID WiFi)
    serverName.trim();  // Supprimer espaces en début/fin
    // Supprimer les caractères de contrôle et caractères invalides
    serverName.replace("\n", "");
    serverName.replace("\r", "");
    serverName.replace("\t", "");
    if (serverName.length() == 0) serverName = "nidmi";
    
    // Sauvegarder le nom mDNS dans NVS pour RTP-MIDI (seulement si NVS ouvre en écriture)
    if (preferences.begin("nidmi", false)) {
        // Garde de schéma NVS : si le format stocké diffère du firmware (mise à jour
        // incompatible), on réinitialise la config (pins/mux/mappings/osc) mais on
        // PRÉSERVE le réseau (STA + mDNS) -> la carte reste joignable, et on ne charge
        // jamais d'anciennes données dans une nouvelle structure.
        if (storedSchema == 0) {
            // Aucune version stockée : appareil existant déjà au format courant (ou NVS vierge).
            // On tamponne la version SANS rien effacer -> les réglages existants sont conservés.
            // (À la 1re introduction de la garde, le format courant EST le schéma v1.)
            preferences.putUInt("nvs_schema", NIDMI_NVS_SCHEMA_VERSION);
        } else if (storedSchema != NIDMI_NVS_SCHEMA_VERSION) {
            // Version connue mais différente -> format NVS incompatible (vrai changement, ex. v1->v2) :
            // on réinitialise la config (pins/mux/mappings/osc) mais on PRÉSERVE le réseau.
            Serial.printf("[NiDMI] Schéma NVS stocké=%u attendu=%u -> reset config (réseau préservé)\n",
                          (unsigned)storedSchema, (unsigned)NIDMI_NVS_SCHEMA_VERSION);
            preferences.clear();
            if (g_staSsid.length() > 0)  preferences.putString("sta_ssid", g_staSsid);
            if (g_staPass.length() > 0)  preferences.putString("sta_pass", g_staPass);
            if (g_staIpStr.length() > 0) preferences.putString("sta_ip", g_staIpStr);
            if (g_staGwStr.length() > 0) preferences.putString("sta_gw", g_staGwStr);
            if (g_staSnStr.length() > 0) preferences.putString("sta_sn", g_staSnStr);
            preferences.putUInt("nvs_schema", NIDMI_NVS_SCHEMA_VERSION);
        }
        // storedSchema == NIDMI_NVS_SCHEMA_VERSION -> rien à faire (déjà à jour)
        preferences.putString("mdns_name", serverName);
        preferences.putString("rtp_name", serverName);
        preferences.end();
    }
    
    Serial.println("[NiDMI] Names synchronized:");
    Serial.printf("  SSID: %s\n", serverName.c_str());
    Serial.printf("  mDNS: %s.local\n", serverName.c_str());

    const char* apSsid = serverName.c_str();
    const char* apPass = "nidmipass";
    const char* host   = serverName.c_str();

    touchDiag("AVANT WiFi/serveur");

    // Démarre l’AP : AP seul si aucun STA en NVS (évite soucis d’association client en APSTA « vide »)
    NIDMI_WEB_LOG("[MEM] avant WiFi: %d\n", (int)ESP.getFreeHeap());
    serverCore.begin(apSsid, apPass, host, g_staSsid.length() == 0);
    NIDMI_WEB_LOG("[MEM] apres WiFi+serveur: %d\n", (int)ESP.getFreeHeap());

    touchDiag("APRES WiFi/serveur");

    // Tente STA après que le mode APSTA soit configuré
    if (g_staSsid.length() > 0) {
        if (g_staIpStr.length() > 0 && g_staGwStr.length() > 0 && g_staSnStr.length() > 0) {
            IPAddress ip, gw, sn;
            if (ip.fromString(g_staIpStr) && gw.fromString(g_staGwStr) && sn.fromString(g_staSnStr)) {
                serverCore.setStaticStaIp(ip, gw, sn);
                Serial.printf("[NiDMI] STA static IP: %s GW: %s SN: %s\n", g_staIpStr.c_str(), g_staGwStr.c_str(), g_staSnStr.c_str());
            }
        }
        serverCore.connectSta(g_staSsid.c_str(), g_staPass.length() > 0 ? g_staPass.c_str() : nullptr);
    } else {
        Serial.println("[NiDMI] No STA configuration found");
    }
    
    // USB-MIDI : NIDMI_USB_MIDI_ENABLED_AT_COMPILE_TIME dans UsbMidiManager.h (pas NVS)
    g_midiRouter.enableUsbMidi(usbMidiEnabled);

    // Initialiser MidiRouter (qui initialisera USB MIDI si activé et supporté)
    g_midiRouter.begin();

    /* Script .nms memorise : la carte se reconfigure SEULE au demarrage. C'est
       la condition du headless — une carte deployee n'a pas de navigateur pour
       lui redire quoi faire. Le NOM vient de la NVS, le CONTENU de mapfs. */
    g_midiRouter.restaurerScript();
    // La table CC -> parametre revient elle aussi de la NVS : sans elle, un
    // redemarrage rendait muets tous les potentiometres appris.
    CcMap::monter();

    /* print() d'un .nms : au journal ET vers l'app.
     * En headless, le moteur du navigateur n'execute plus rien — son
     * _msePrintLog n'est donc jamais appele, et la console de la zone I/O
     * restait vide alors que le script tournait tres bien. La carte pousse
     * maintenant une trame « NMS_PRINT:<etiquette>\x1f<valeur> » que l'app
     * affiche dans la console du bloc concerne. */
    MappingEngine::surImpression([](const char* origine, const char* etiquette,
                                   float valeur, uint8_t montre,
                                   uint8_t pipe, uint8_t seg) {
        /* L'ORIGINE D'ABORD. La trame ne portait que l'etiquette et la valeur :
         * l'app ne pouvait donc pas savoir QUI avait imprime, et attribuait
         * tout au bloc map dont le script tourne sur la carte — les print() des
         * broches compris. Trois champs desormais : « pin:5 », « map:2 » ou
         * « essai », puis l'etiquette, puis la valeur. */
        const char* org = (origine && origine[0]) ? origine : "?";
        char trame[112];
        /* ⚠️ CE RAPPEL S'EXECUTE DANS MidiTask, SUR LE COEUR 0. Il ne doit donc
         * PAS toucher a la WebSocket : cleanupClients() efface la liste de
         * clients depuis loopTask, et l'iterer d'ici est un acces a de la
         * memoire rendue (ServerCore.h). On POUSSE dans une file, loopTask
         * draine. Le compteur, lui, se lit sans risque de partout. */
        /* NI COURBE NI NOMBRE N'ONT D'HISTORIQUE : s'ils ne partent pas, ils
         * n'existent pas. On sort donc AVANT de formater — en headless ce
         * chemin ne coute rien du tout. Seul print() garde une trace. */
        const bool vivant = (montre != MappingEngine::MontreTexte);
        if (vivant && !nidmi_ws_quelqu_un_ecoute()) return;
        if (montre == MappingEngine::MontreNombre) {
            /* La POSITION voyage : « pipeline:segment ». C'est elle qui permet
             * a l'editeur d'annoter la bonne boite, exactement comme le
             * `${pi}:${si}` du moteur de reference. */
            snprintf(trame, sizeof(trame), "NMS_NUM:%s\x1f%s\x1f%.4f\x1f%u:%u",
                     org, etiquette, valeur, (unsigned)pipe, (unsigned)seg);
        } else if (montre == MappingEngine::MontreCourbe) {
            /* PAS dans le journal texte : c'est tout l'objet de graph(). Une
             * valeur continue qui defile en chiffres noie le journal — vingt
             * lignes par seconde pour un potentiometre qui tremble. */
            snprintf(trame, sizeof(trame), "NMS_GRAPH:%s\x1f%s\x1f%.4f",
                     org, etiquette, valeur);
        } else {
            /* Le journal de bord AVANT la garde, et toujours : c'est
             * l'historique qu'un client rejouera en s'abonnant plus tard. Une
             * trace qu'on n'emet pas n'est pas une trace qu'on efface. */
            NIDMI_WEB_LOG("[%s] %s : %.4f", org, etiquette, valeur);
            snprintf(trame, sizeof(trame), "NMS_PRINT:%s\x1f%s\x1f%.4f",
                     org, etiquette, valeur);
        }
        /* Personne n'ecoute : on s'arrete la. Sinon on POUSSE — file pleine =
         * le client ne suit pas, la trame est jetee sans bloquer cette tache.
         * Empiler pour ne rien perdre, c'est perdre tout. */
        if (!nidmi_ws_quelqu_un_ecoute()) return;
        nidmi_ws_pousser(trame);
    });

    /* MIDI USB ENTRANT -> moteur audio.
       Le port USB de la carte etait uniquement SORTANT : un clavier ou un DAW
       branche dessus n'avait aucun effet (UsbMidiManager::update() ne lisait
       rien). On branche donc la reception sur les memes deux destinations que
       le MIDI RTP : les composants (LEDs appairees) ET le moteur audio, pour
       que la carte SONNE quand on la joue de l'exterieur.
       Note : la porte de silence reste maitresse — sans PLAY, ces notes
       n'atteignent pas le DAC (MESURES.md §19). */
    serverCore.usbMidi().setMidiInputHooks(
        [](uint8_t ch, uint8_t note, uint8_t vel) { g_midiRouter.noteEntrante(ch, note, vel, false); },
        [](uint8_t ch, uint8_t note, uint8_t vel) { g_midiRouter.noteEntrante(ch, note, vel, true); },
        [](uint8_t ch, uint8_t cc, uint8_t val) { g_midiRouter.ccEntrant(ch, cc, val); }
    );
    NIDMI_WEB_LOG("[MEM] apres MidiRouter: %d\n", (int)ESP.getFreeHeap());

    // Interface réseau sur le câble USB. Impérativement APRÈS MidiRouter :
    // c'est lui qui appelle USB.begin(), et TinyUSB n'assemble sa
    // configuration qu'une fois. Le descripteur NCM, lui, a été enregistré
    // pendant l'initialisation statique (instance globale dans
    // UsbNetBootstrap.cpp), donc bien avant.
    if (nidmi_usbnet::enabled()) {
        if (nidmi_usbnet::begin()) {
            NIDMI_WEB_LOG("[UsbNet] %s", nidmi_usbnet::statusLine().c_str());
        }
        NIDMI_WEB_LOG("[MEM] apres UsbNet: %d\n", (int)ESP.getFreeHeap());
    }
    
    // Initialiser RTP-MIDI
    serverCore.rtpMidi().begin(serverName.c_str());
    serverCore.rtpMidi().setMidiInputHooks(
        [](uint8_t ch, uint8_t note, uint8_t vel) { g_componentManager.handleMidiNoteOn(ch, note, vel); },
        [](uint8_t ch, uint8_t note, uint8_t vel) { g_componentManager.handleMidiNoteOff(ch, note, vel); },
        [](uint8_t ch, uint8_t cc,   uint8_t val) { g_midiRouter.ccEntrant(ch, cc, val); }
    );
    NIDMI_WEB_LOG("[MEM] apres RTP-MIDI: %d\n", (int)ESP.getFreeHeap());
    
    // Initialiser Bluetooth MIDI
    serverCore.bluetooth().begin(serverName.c_str());
    NIDMI_WEB_LOG("[MEM] apres Bluetooth: %d\n", (int)ESP.getFreeHeap());
    
    touchDiag("AVANT ComponentManager.begin");

    // Initialiser ComponentManager
    g_componentManager.begin(&g_midiRouter);
    NIDMI_WEB_LOG("[MEM] apres ComponentManager: %d\n", (int)ESP.getFreeHeap());

    touchDiag("APRES ComponentManager.begin (MuxTask+MidiTask demarres)");
    
    Serial.println("[NiDMI] Ready");
    NIDMI_WEB_LOG("[NiDMI] Ready (console web dispo sur S3 si activée)");
    Serial.print("  AP SSID: "); Serial.println(apSsid);
    Serial.print("  AP PASS: "); Serial.println(apPass);
    Serial.print("  AP IP: "); Serial.println(WiFi.softAPIP());
    Serial.print("  mDNS: http://"); Serial.print(host); Serial.println(".local/");
    if (nidmi_usbnet::enabled()) {
        Serial.print("  USB net: http://"); Serial.print(nidmi_usbnet::ip());
        Serial.print("/  (aussi http://"); Serial.print(host); Serial.println(".local/)");
    }
    Serial.print("  RTP-MIDI: "); Serial.println(serverCore.rtpMidi().isReady() ? "Initialized" : "Failed");
    Serial.print("  Bluetooth: "); Serial.println(serverCore.bluetooth().isInitialized() ? "Initialized" : "Failed");
    Serial.printf("Touch Enabled: %s\n", touchEnabled ? "true" : "false");
    Serial.println();
}

void nidmi_loop() {
    /* Chargement du process audio mémorisé, sur un tas encore vierge : c'est
       l'ordre d'allocation qui décide (MESURES.md §15), et trois secondes après
       le boot on est très loin devant l'ouverture d'un navigateur.

       Note honnête : ce déplacement de setup() vers loop() avait été fait en
       poursuivant un blocage de la tâche audio, sur l'hypothèse que le contexte
       d'appel comptait. C'ÉTAIT FAUX — la cause était le diagnostic tactile qui
       laissait GPIO1 (le BCK) au périphérique de touch (voir AudioEngine.cpp).
       L'appel est resté ici parce qu'il y est correct et qu'il laisse setup()
       se terminer, pas parce que setup() poserait un problème. */
    static bool audioRestaure = false;
    if (!audioRestaure && millis() > 3000) {
        audioRestaure = true;
        AudioEngine::restaurerAuBoot();
    }

    AudioEngine::entretienBoot();   // écrit la NVS hors du contexte async
    Cues::boucle();                 // avance les cues minutées — la carte tient son propre temps

    // Redémarrage différé (laisse le temps à la réponse HTTP et à la NVS de se fermer proprement)
    if (g_requestDownload && (millis() - g_rebootRequestTime >= 2000)) {
        REG_WRITE(RTC_CNTL_OPTION1_REG, 0x1);   // force_download_boot
        ESP.restart();
    }
    if (g_requestReboot && (millis() - g_rebootRequestTime >= 2000)) {
        ESP.restart();
    }

    /* ── LE CABLE OU LE WIFI ────────────────────────────────────────────── */
    if (g_wifiRallumerDemande && millis() - g_wifiRallumerA >= 300) {
        g_wifiRallumerDemande = false;
        serverCore.demarrerRadioWifi();
        g_lastStaConnectAttempt = 0;             // reconnexion STA sans attendre
        g_staReconnectInterval = STA_RECONNECT_BASE_MS;
    }
    if (g_essaiDemande && !g_essai.enCours && millis() - g_essaiDemandeA >= 300) {
        g_essaiDemande = false;
        g_essai = EssaiWifi();
        g_essai.dureeMs   = g_essaiDureeDemandee;
        g_essai.blocAvant = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        g_essai.tasAvant  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        serverCore.couperRadioWifi();
        g_essai.debut   = millis();
        g_essai.enCours = true;
    }
    if (g_essai.enCours) {
        const unsigned long ecoule = millis() - g_essai.debut;
        // 3 s : le temps que esp_wifi_deinit() ait tout rendu.
        if (!g_essai.mesurePendant && ecoule >= 3000) {
            g_essai.blocPendant = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
            g_essai.tasPendant  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            g_essai.mesurePendant = true;
        }
        // LE MINUTEUR, ET RIEN D'AUTRE : la radio revient quoi qu'il arrive.
        if (ecoule >= g_essai.dureeMs) {
            serverCore.demarrerRadioWifi();
            g_lastStaConnectAttempt = 0;
            g_staReconnectInterval = STA_RECONNECT_BASE_MS;
            g_essai.enCours = false;
            g_essai.fait = true;
            g_essai.fin = millis();
        }
    }
    // 5 s apres le retour : la radio a repris ce qu'elle prend. Que reste-t-il ?
    if (g_essai.fait && !g_essai.mesureApres && millis() - g_essai.fin >= 5000) {
        g_essai.blocApres = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        g_essai.tasApres  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        g_essai.mesureApres = true;
    }

    // Tentative de reconnexion STA automatique si des identifiants sont connus.
    // connectSta() est non bloquant : on se contente de relancer WiFi.begin() et
    // d'espacer les tentatives via un backoff (10 s -> 60 s) remis à zéro une fois connecté.
    // GARDEE PAR LA RADIO : connectSta() appelle WiFi.begin(), qui RALLUMERAIT un
    // WiFi coupe sur commande — la coupure serait un mensonge, et la mesure fausse.
    if (g_staSsid.length() > 0 && serverCore.radioWifiAllumee()) {
        wl_status_t staStatus = WiFi.status();
        unsigned long now = millis();
        if (staStatus == WL_CONNECTED) {
            if (!g_staWasConnected) {
                g_staWasConnected = true;
                NIDMI_WEB_LOG("[NiDMI] STA connectée, IP: %s", WiFi.localIP().toString().c_str());
            }
            g_staReconnectInterval = STA_RECONNECT_BASE_MS;
        } else {
            if (g_staWasConnected) {
                g_staWasConnected = false;
                NIDMI_WEB_LOG("[NiDMI] STA déconnectée");
            }
            if (now - g_lastStaConnectAttempt >= g_staReconnectInterval) {
                NIDMI_WEB_LOG("[NiDMI] STA non connecté, reconnexion auto (backoff %lus)...", g_staReconnectInterval / 1000);
                // Reconfigurer éventuellement l'IP statique
                if (g_staIpStr.length() > 0 && g_staGwStr.length() > 0 && g_staSnStr.length() > 0) {
                    IPAddress ip, gw, sn;
                    if (ip.fromString(g_staIpStr) && gw.fromString(g_staGwStr) && sn.fromString(g_staSnStr)) {
                        serverCore.setStaticStaIp(ip, gw, sn);
                    }
                }
                serverCore.connectSta(g_staSsid.c_str(), g_staPass.length() > 0 ? g_staPass.c_str() : nullptr);
                g_lastStaConnectAttempt = now;
                g_staReconnectInterval *= 2;
                if (g_staReconnectInterval > STA_RECONNECT_MAX_MS) g_staReconnectInterval = STA_RECONNECT_MAX_MS;
            }
        }
    }

    serverCore.update();

    // Porte l'annonce de lien vers l'hôte et l'activation mDNS sur le lien USB.
    // Ces deux annonces doivent être répétées : émises une seule fois elles se
    // perdent si l'hôte n'a pas fini de se configurer, et rien ne le signale.
    nidmi_usbnet::update();

    // La sante de la carte : calculee une fois par seconde, annoncee si elle change.
    verifierSante();

    // Recharger pins si demandé (débounce 500 ms pour grouper les sauvegardes séquentielles)
    if (g_requestReloadPins && (millis() - g_reloadRequestTime >= 500)) {
        g_requestReloadPins = false;
        g_componentManager.reloadConfigs();
    }
    
    processComponents();
    /* Le rattrapage de la console web : une ligne par tour, hors du rappel
     * WebSocket (voir WebDebugConsole.cpp). */
    /* LA SEULE FENETRE ou l'on ecrit sur la WebSocket : loopTask, la meme tache
     * que ws.cleanupClients() de serverCore.update(). Tout le reste du firmware
     * POUSSE dans la file. Voir ServerCore.h. */
    nidmi_ws_drainer();
    nidmi_web_debug_pump();
}

// Instance globale
NiDMIServer nidmi;

// Implémentation de l'interface publique
void NiDMIServer::begin() {
    nidmi_begin();
}

void NiDMIServer::loop() {
    nidmi_loop();
}
