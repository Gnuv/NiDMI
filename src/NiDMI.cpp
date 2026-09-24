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
#include "mapping/VocabulaireEmbarque.h"
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
 * « On n'a pas besoin de deux acces en simultane : soit l'un, soit l'autre. »
 * Mesure (MESURES §147), son charge, sous charge : cable seul, MIDI 0 retard et
 * 0 sous-alimentation ; WiFi allume, des retards MIDI et des decrochages — les
 * paquets de la radio passent avant tout. (La memoire, elle, ne l'impose plus
 * depuis le §152 : WiFi allume, 31 732 o d'un seul tenant.)
 *
 * Qui decide : « LE WIFI : TROIS REGLES, UN SEUL CHEF » — le forcage,
 * l'instrument autonome, puis la bascule « cable prioritaire », qui coupe la
 * radio quand le cable VIT et la rallume des qu'il se tait. La preuve de vie,
 * ce sont des TRAMES RECUES de l'hote (linkUp() a ete vu vrai sur un lien mort,
 * §140, §143) ; au repos un Mac se tait jusqu'a une minute : on le sonde
 * (§148). Le demarrage, lui, allume TOUJOURS la radio (§142) : on coupe en
 * marche.
 *
 * L'ESSAI coupe le WiFi N secondes sur son SEUL minuteur, pour mesurer. Il
 * marche sur tous les builds. Refuse pendant qu'une regle tient le WiFi coupe :
 * il n'y aurait rien a mesurer. */
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
/* Les retards audio causes par une memorisation d'option faite EN SILENCE
 * (MESURES §155) : reels — le compteur du moteur les garde —, mais rien ne
 * s'est entendu, donc ils ne rallument pas le voyant. Seule une ecriture LENTE
 * (un effacement : >= 20 ms, le seuil de retard du moteur) peut en causer ;
 * wifiBoucle() lui attribue ceux du quart de seconde qui suit, et d'ici la le
 * voyant attend. Un retard apres une ecriture rapide a une autre cause : il
 * rallume le voyant comme n'importe quel autre. */
static uint32_t s_retardsInaudibles = 0;
static bool     s_ecritureAVerifier = false;

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
    if (!s_ecritureAVerifier) {
        const uint32_t audibles = m.sousAlimentations - s_retardsInaudibles;
        if (audibles > s_sousAlimAvant) s_decrochagesJusqua = maintenant + 60000;
        s_sousAlimAvant = audibles;
    }
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
extern "C" void nidmi_requestEssaiWifi(unsigned long dureeMs){
    g_essaiDureeDemandee = dureeMs;
    g_essaiDemandeA = millis();
    g_essaiDemande = true;
}
/* ── LA BASCULE « CABLE PRIORITAIRE » ─────────────────────────────────────
 * Deux etats. VEILLE : la radio est la ; si le cable vit sans interruption
 * CABLE_CONFIRMATION_MS, avec au moins CABLE_TRAMES_MIN trames, on la coupe.
 * CABLE : la radio est coupee ; elle revient AUSSITOT si le bus USB n'est plus
 * configure ou passe en veille (hote endormi, ou cable debranche : sans
 * detection de VBUS, un debranchement se voit ainsi), si l'emission expire,
 * ou si le reglage est retire ; et au bout de CABLE_SILENCE_MS sans une trame.
 *
 * LE SILENCE SE PROVOQUE. Au repos, un Mac se tait jusqu'a UNE MINUTE sur le
 * lien (22 trames en 5 min, ecarts de 50 a 61 s, MESURES §148) : attendre qu'il
 * parle ferait croire le cable mort — ou le rendrait lent a declarer mort.
 * Apres CABLE_SONDE_APRES_MS sans trame, la carte lui envoie une requete ARP
 * toutes les CABLE_SONDE_TOUS_MS ; un hote vivant repond toujours. Le cable
 * n'est « vivant » que si l'hote a un bail : sans adresse, pas de sonde, et
 * pas de coupure.
 * Apres un retour, pas de nouvelle coupure avant CABLE_RECOUPE_MS : un cable
 * qui clignote ne fait pas clignoter le WiFi.
 * Tout se passe dans nidmi_loop() : l'API ne fait que lever un drapeau, et la
 * NVS s'ecrit ici, hors du contexte async (§13). */
static const unsigned long CABLE_CONFIRMATION_MS = 10000;
static const uint32_t      CABLE_TRAMES_MIN      = 3;
static const unsigned long CABLE_SILENCE_MS      = 30000;   // 4 sondes sans reponse (10, 15, 20, 25 s)
static const unsigned long CABLE_SONDE_APRES_MS  = 10000;
static const unsigned long CABLE_SONDE_TOUS_MS   = 5000;
static const unsigned long CABLE_RECOUPE_MS      = 30000;
static const unsigned long CABLE_RADIO_REESSAI_MS = 5000;
struct BasculeCable {
    bool lu = false;                  // reglage lu en NVS
    bool prioritaire = true;          // NVS "cable_prio" ; absent = oui
    bool tientLeWifi = false;         // etat CABLE : c'est nous qui avons coupe la radio
    unsigned long depuis = 0;         // entree dans l'etat courant
    uint32_t rx = 0, txExp = 0;       // derniers compteurs vus
    unsigned long dernierRx = 0, derniereExpiree = 0;
    unsigned long vivantDepuis = 0;   // fenetre de confirmation ; 0 = pas vivant
    uint32_t tramesFenetre = 0;
    unsigned long retourA = 0;        // dernier retour du WiFi (retours > 0)
    bool radioEnAttente = false;      // la radio n'a pas pu revenir : on reessaie
    unsigned long radioEchecA = 0;
    unsigned long derniereSonde = 0;
    uint32_t sondes = 0;
    uint32_t coupures = 0, retours = 0;
    const char* derniereCause = "";
    unsigned long dernierTic = 0;
};
static BasculeCable g_bascule;
static volatile int8_t g_basculeDemande = -1;   // -1 rien ; 0 retirer ; 1 activer

// La politique du WiFi (forcage, autonomie) — voir « LE WIFI : TROIS REGLES ».
struct PolitiqueWifi {
    bool lu = false;
    bool autonome = false;                 // NVS "standalone"
    bool force = false;                    // jamais memorise
    bool autonomeTient = false;            // c'est l'autonomie qui a coupe la radio
    bool autonomeAEcrire = false, prioAEcrire = false;
    unsigned long autonomeChangeA = 0, prioChangeA = 0;
    unsigned long derniereRelanceScript = 0;
    bool sysInconnuDit = false;
    // Ce que coute la memorisation d'une option (MESURES §155) : une ecriture
    // NVS qui efface une page arrete l'autre coeur le temps de l'effacement.
    uint32_t ecrituresNvs = 0, ecrituresLentes = 0, ecritureNvsPireUs = 0, ecritureNvsDerniereUs = 0;
    uint32_t retardsDesEcritures = 0, sousAlimAvant = 0;
};
static PolitiqueWifi g_politique;
static volatile int8_t g_demandeAutonome = -1;   // -1 rien ; 0 retirer ; 1 activer
static volatile int8_t g_demandeForce = -1;
static volatile bool g_relanceDemandeScript = false;
static volatile bool g_sysInconnu = false;
static char g_sysInconnuNom[16];                 // le premier, pour le dire
static const unsigned long OPTION_MEMORISEE_APRES_MS = 3000;
static const uint32_t SILENCE_AVANT_MEMORISATION_MS = 500;
static const uint32_t ECRITURE_LENTE_US = 20000;      // le seuil de retard du moteur audio
static const unsigned long RELANCE_SCRIPT_TOUS_MS = 10000;

extern "C" void nidmi_demanderCablePrioritaire(bool actif){
    g_basculeDemande = actif ? 1 : 0;
}

/* Relancer le cable (MESURES §154) : quand macOS releve lui-meme l'interface
 * reseau du cable, il repasse en alt 0 une milliseconde apres l'avoir activee
 * et ne revient plus (§153) ; seule une nouvelle enumeration rend le lien. Le
 * gestionnaire HTTP leve le drapeau, la boucle execute : jamais le pilote USB
 * depuis async_tcp. La bascule voit ensuite le cable repartir puis revivre,
 * comme a un branchement. */
static volatile bool g_relanceDemande = false;
extern "C" void nidmi_demanderRelanceCable(){
    g_relanceDemande = true;
}
String nidmi_cablePrioritaireJson(){
    const BasculeCable b = g_bascule;
    const PolitiqueWifi pol = g_politique;
    const unsigned long now = millis();
    const uint32_t muette = AudioEngine::silenceDepuisMs();   // UINT32_MAX : pas de son
    const char* etat = pol.force         ? "force"
                     : pol.autonomeTient ? "autonome"
                     : b.tientLeWifi     ? "cable"
                     : !b.prioritaire    ? "desactive"
                     : b.vivantDepuis    ? "confirmation" : "veille";
    String j = "{\"prioritaire\":";
    j += b.prioritaire ? "true" : "false";
    j += ",\"autonome\":";
    j += pol.autonome ? "true" : "false";
    j += ",\"wifi_force\":";
    j += pol.force ? "true" : "false";
    j += ",\"etat\":\"" + String(etat) + "\"";
    j += ",\"depuis_ms\":" + String(now - b.depuis);
    j += ",\"dernier_rx_ms\":" + (b.dernierRx ? String(now - b.dernierRx) : String("null"));
    j += ",\"coupures\":" + String(b.coupures) + ",\"retours\":" + String(b.retours);
    j += ",\"sondes\":" + String(b.sondes);
    j += ",\"derniere_cause\":\"" + String(b.derniereCause) + "\"";
    j += ",\"radio_en_attente\":";
    j += b.radioEnAttente ? "true" : "false";
    j += ",\"nvs\":{\"ecritures\":" + String(pol.ecrituresNvs)
       + ",\"pire_us\":" + String(pol.ecritureNvsPireUs)
       + ",\"derniere_us\":" + String(pol.ecritureNvsDerniereUs)
       + ",\"lentes\":" + String(pol.ecrituresLentes)
       + ",\"retards_audio_causes\":" + String(pol.retardsDesEcritures)
       + ",\"en_attente\":" + ((pol.prioAEcrire || pol.autonomeAEcrire) ? "true" : "false")
       + ",\"sortie_muette_ms\":" + (muette == UINT32_MAX ? String("null") : String(muette)) + "}";
    j += ",\"confirmation_ms\":" + String(CABLE_CONFIRMATION_MS);
    j += ",\"silence_ms\":" + String(CABLE_SILENCE_MS) + "}";
    return j;
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

/* Rallumer la radio et relancer la STA tout de suite. Si le pilote n'a pas
 * pu demarrer (memoire), on reessaie : la carte n'a PAS le droit de rester
 * sans WiFi parce qu'un cable est mort. */
/* DUREES ECOULEES, jamais des dates futures : millis() repasse par zero au
 * bout de 49,7 jours, et une installation tourne aussi longtemps. */
static void basculeRallumerRadio(unsigned long now){
    serverCore.demarrerRadioWifi();
    if (serverCore.radioWifiAllumee()) {
        g_bascule.radioEnAttente = false;
        g_lastStaConnectAttempt = 0;
        g_staReconnectInterval = STA_RECONNECT_BASE_MS;
    } else {
        g_bascule.radioEnAttente = true;
        g_bascule.radioEchecA = now;
    }
}

// La regle « cable prioritaire » elle-meme — appelee par wifiBoucle() quand ni
// le forcage ni l'autonomie ne decident (voir « LE WIFI : TROIS REGLES »).
static void basculeCable(unsigned long now){
    BasculeCable& b = g_bascule;

    // La preuve de vie : des trames recues, des emissions qui n'expirent pas.
    uint32_t rx = 0, txExp = 0;
    nidmi_usbnet::compteurs(rx, txExp);
    if (rx != b.rx)       { b.tramesFenetre += rx - b.rx; b.rx = rx; b.dernierRx = now; }
    if (txExp != b.txExp) { b.txExp = txExp; b.derniereExpiree = now; }
    const bool enVeille   = nidmi_usbnet::suspendu();
    const bool branche    = nidmi_usbnet::linkUp() && !enVeille;
    const bool silencieux = b.dernierRx == 0 || now - b.dernierRx >= CABLE_SILENCE_MS;
    const bool enEchec    = b.derniereExpiree != 0 && now - b.derniereExpiree < 5000;

    // Sonder l'hote qui se tait — seulement si la bascule a quelque chose a
    // decider : reglage actif, ou radio tenue coupee.
    if (branche && (b.prioritaire || b.tientLeWifi) && b.dernierRx != 0 &&
        now - b.dernierRx >= CABLE_SONDE_APRES_MS && now - b.derniereSonde >= CABLE_SONDE_TOUS_MS) {
        b.derniereSonde = now;
        if (nidmi_usbnet::sonder()) b.sondes++;
    }

    if (b.tientLeWifi) {
        const char* cause = !b.prioritaire ? "reglage"
                          : enVeille       ? "veille ou debranche"
                          : !branche       ? "demonte"
                          : enEchec        ? "emission"
                          : silencieux     ? "silence" : nullptr;
        if (!cause) return;
        b.tientLeWifi = false;
        b.depuis = now;
        b.retours++;
        b.derniereCause = cause;
        b.retourA = now;
        b.vivantDepuis = 0;
        basculeRallumerRadio(now);
        NIDMI_WEB_LOG("[NiDMI] cable prioritaire : WiFi rallume (%s)", cause);
        return;
    }

    const bool vivant = branche && !silencieux && !enEchec && nidmi_usbnet::hoteConnu();
    const bool essai  = g_essai.enCours || g_essaiDemande;
    const bool auRepos = b.retours > 0 && now - b.retourA < CABLE_RECOUPE_MS;
    if (!b.prioritaire || !vivant || essai || !serverCore.radioWifiAllumee() || auRepos) {
        b.vivantDepuis = 0;
        return;
    }
    if (b.vivantDepuis == 0) {
        b.vivantDepuis = now;
        b.tramesFenetre = 0;
        return;
    }
    if (now - b.vivantDepuis < CABLE_CONFIRMATION_MS || b.tramesFenetre < CABLE_TRAMES_MIN) return;
    serverCore.couperRadioWifi();
    b.tientLeWifi = true;
    b.depuis = now;
    b.coupures++;
    b.vivantDepuis = 0;
}

/* ── LE WIFI : TROIS REGLES, UN SEUL CHEF ────────────────────────────────
 * Qui decide si la radio est allumee, dans cet ordre (MESURES §155) :
 *   1. FORCE — un bouton (s("sys.wifi") recoit une valeur > 0) ou l'app
 *      (POST /api/reseau/wifi etat=on) : la radio est allumee, quelle que soit
 *      la regle, jusqu'a une valeur 0 (etat=off) ou au redemarrage. C'est la
 *      porte de secours d'un instrument autonome. Pas memorise, a dessein.
 *   2. AUTONOME — l'option « Instrument autonome » (NVS "standalone") : la
 *      radio est coupee, cable branche ou non. On ne branche l'instrument a
 *      l'ordinateur que pour le configurer, par le cable (192.168.7.1). Refusee
 *      sur un firmware sans lien reseau USB : on s'enfermerait dehors.
 *   3. CABLE PRIORITAIRE — la bascule ci-dessus.
 * Le demarrage allume TOUJOURS la radio (§142) : la regle coupe en marche.
 * Les deux options valent tout de suite. Elles se MEMORISENT 3 s apres leur
 * dernier changement — un bouton qui bascule vite n'use pas la flash — et
 * seulement quand la sortie audio est muette depuis 0,5 s : l'ecriture qui
 * efface une page NVS (une sur ~120) arrete les deux coeurs 43 a 45 ms, plus
 * que la marge du DMA (30 ms), et chacune a coute un bloc audio en retard
 * (MESURES §155). Un son qui ne s'arrete jamais retarde donc la memorisation,
 * jamais l'option ; coupee avant, la carte redemarre sur l'ancienne valeur.
 * Un tour toutes les 250 ms. */

extern "C" void nidmi_demanderAutonome(bool actif){
    g_demandeAutonome = actif ? 1 : 0;
}
extern "C" void nidmi_demanderWifiForce(bool actif){
    g_demandeForce = actif ? 1 : 0;
}
extern "C" bool nidmi_regleTientLeWifiCoupe(){
    return g_bascule.tientLeWifi || g_politique.autonomeTient;
}

/* ── LES FONCTIONS DE LA CARTE, POUR LES SCRIPTS ─────────────────────────
 * Un s("sys.<nom>") dans un script .nms — bouton, capteur, bloc map — n'ecrit
 * pas le bus : il demande a la carte (MappingEngine.cpp). Appele depuis la
 * tache qui execute le script, MIDI ou capteurs : on ne fait que POSER la
 * demande, et nidmi_loop l'execute — jamais la radio, l'USB ou la NVS depuis
 * une tache temps reel. La valeur est celle qui entre dans le s() : > 0 = oui,
 * 0 = non.
 *   sys.wifi        force la radio allumee ; 0 rend la main a la regle
 *   sys.standalone  l'option « Instrument autonome »
 *   sys.cablefirst  l'option « Cable prioritaire »
 *   sys.reconnect   front montant : relancer le cable (au plus toutes les 10 s)
 * Et r() relit l'etat : ces noms-la, plus sys.cable (1 si l'ordinateur utilise
 * le reseau du cable) — publies dans le bus par nidmi_loop. */
extern "C" void nidmi_sys_recevoir(const char* nom, float valeur){
    const bool oui = valeur > 0.0f;
    if (!strcmp(nom, "sys.wifi"))              g_demandeForce = oui ? 1 : 0;
    else if (!strcmp(nom, "sys.standalone"))   g_demandeAutonome = oui ? 1 : 0;
    else if (!strcmp(nom, "sys.cablefirst"))   g_basculeDemande = oui ? 1 : 0;
    else if (!strcmp(nom, "sys.reconnect")) {
        static bool avant = false;             // front montant : une relance par appui
        if (oui && !avant) g_relanceDemandeScript = true;
        avant = oui;
    } else if (!g_sysInconnu) {
        strlcpy(g_sysInconnuNom, nom, sizeof g_sysInconnuNom);
        __sync_synchronize();                  // le nom avant le drapeau
        g_sysInconnu = true;
    }
}

// Les etats, lus par r("sys.<nom>"). Publies a chaque changement seulement.
static void publierEtatsSys(){
    static int8_t wifi = -1, cable = -1, autonome = -1, prio = -1;
    const int8_t w = serverCore.radioWifiAllumee() ? 1 : 0;
    const int8_t c = nidmi_usbnet::reseauActif() ? 1 : 0;
    const int8_t a = g_politique.autonome ? 1 : 0;
    const int8_t p = g_bascule.prioritaire ? 1 : 0;
    if (w != wifi)     { wifi = w;     FluxRegistry::update("sys.wifi", w); }
    if (c != cable)    { cable = c;    FluxRegistry::update("sys.cable", c); }
    if (a != autonome) { autonome = a; FluxRegistry::update("sys.standalone", a); }
    if (p != prio)     { prio = p;     FluxRegistry::update("sys.cablefirst", p); }
}

static void wifiBoucle(){
    PolitiqueWifi& p = g_politique;
    BasculeCable& b = g_bascule;
    const unsigned long now = millis();
    if (now - b.dernierTic < 250) return;
    b.dernierTic = now;

    if (!p.lu) {
        Preferences prefs;
        prefs.begin("nidmi", true);
        b.prioritaire = prefs.getBool("cable_prio", true);
        p.autonome = prefs.getBool("standalone", false) && nidmi_usbnet::enabled();
        prefs.end();
        p.lu = true;
        b.lu = true;
        b.depuis = now;
    }

    // Les demandes — de l'app ou d'un script. L'etat change tout de suite ; la
    // memorisation attend que l'option se soit posee.
    const int8_t dPrio = g_basculeDemande;
    if (dPrio >= 0) {
        g_basculeDemande = -1;
        if ((dPrio == 1) != b.prioritaire) {
            b.prioritaire = (dPrio == 1);
            p.prioAEcrire = true;
            p.prioChangeA = now;
        }
    }
    const int8_t dAuto = g_demandeAutonome;
    if (dAuto >= 0) {
        g_demandeAutonome = -1;
        const bool voulu = (dAuto == 1) && nidmi_usbnet::enabled();
        if (voulu != p.autonome) {
            p.autonome = voulu;
            p.autonomeAEcrire = true;
            p.autonomeChangeA = now;
        }
    }
    const int8_t dForce = g_demandeForce;
    if (dForce >= 0) {
        g_demandeForce = -1;
        p.force = (dForce == 1);
    }
    if (g_relanceDemandeScript) {
        g_relanceDemandeScript = false;
        if (p.derniereRelanceScript == 0 || now - p.derniereRelanceScript >= RELANCE_SCRIPT_TOUS_MS) {
            p.derniereRelanceScript = now;
            nidmi_usbnet::relancer();
        }
    }
    if (g_sysInconnu && !p.sysInconnuDit) {
        p.sysInconnuDit = true;
        NIDMI_WEB_LOG("[NiDMI] s(\"%s\") : la carte n'a pas cette fonction — elle connait "
                      VOCABULAIRE_SYS_COMMANDES, g_sysInconnuNom);
    }
    // Un bloc audio en retard dans le quart de seconde qui suit une ecriture
    // LENTE ? Elle l'a cause, et elle a eu lieu en silence : rien ne s'est entendu.
    if (s_ecritureAVerifier) {
        s_ecritureAVerifier = false;
        const uint32_t sa = AudioEngine::metriques().sousAlimentations;
        if (sa > p.sousAlimAvant) {
            p.retardsDesEcritures += sa - p.sousAlimAvant;
            s_retardsInaudibles += sa - p.sousAlimAvant;
        }
    }
    const bool prioMure = p.prioAEcrire && now - p.prioChangeA >= OPTION_MEMORISEE_APRES_MS;
    const bool autoMure = p.autonomeAEcrire && now - p.autonomeChangeA >= OPTION_MEMORISEE_APRES_MS;
    if ((prioMure || autoMure) && AudioEngine::silenceDepuisMs() >= SILENCE_AVANT_MEMORISATION_MS) {
        Preferences prefs;
        prefs.begin("nidmi", false);
        p.sousAlimAvant = AudioEngine::metriques().sousAlimentations;
        const uint32_t t0 = micros();
        bool ecrit = false;
        if (prioMure) {
            p.prioAEcrire = false;
            if (prefs.getBool("cable_prio", true) != b.prioritaire) {
                prefs.putBool("cable_prio", b.prioritaire);
                ecrit = true;
            }
        }
        if (autoMure) {
            p.autonomeAEcrire = false;
            if (prefs.getBool("standalone", false) != p.autonome) {
                prefs.putBool("standalone", p.autonome);
                ecrit = true;
            }
        }
        const uint32_t dt = micros() - t0;
        prefs.end();
        if (ecrit) {
            p.ecrituresNvs++;
            p.ecritureNvsDerniereUs = dt;
            if (dt > p.ecritureNvsPireUs) p.ecritureNvsPireUs = dt;
            if (dt >= ECRITURE_LENTE_US) {
                p.ecrituresLentes++;
                s_ecritureAVerifier = true;
            }
        }
    }

    const bool essai = g_essai.enCours || g_essaiDemande;
    // Une remise en marche qui a manque de memoire se retente — si la radio
    // est encore voulue : l'instrument autonome l'annule.
    const bool reessai = b.radioEnAttente && now - b.radioEchecA >= CABLE_RADIO_REESSAI_MS;

    if (p.force) {
        // La radio doit etre la : on la reprend a qui la tient.
        if (b.tientLeWifi || p.autonomeTient) {
            b.tientLeWifi = false;
            p.autonomeTient = false;
            b.depuis = now;
            b.retours++;
            b.derniereCause = "force";
            b.retourA = now;
            b.vivantDepuis = 0;
        }
        if (reessai || (!serverCore.radioWifiAllumee() && !b.radioEnAttente && !essai))
            basculeRallumerRadio(now);
    } else if (p.autonome) {
        // La radio doit etre coupee. Deja coupee par la bascule, ou en attente
        // d'etre rallumee : elle change de main, sans rallumage inutile.
        if (b.tientLeWifi || b.radioEnAttente) {
            b.tientLeWifi = false;
            b.radioEnAttente = false;
            b.depuis = now;
            b.vivantDepuis = 0;
            p.autonomeTient = true;
        }
        if (!p.autonomeTient && serverCore.radioWifiAllumee() && !essai) {
            serverCore.couperRadioWifi();
            p.autonomeTient = true;
            NIDMI_WEB_LOG("[NiDMI] instrument autonome : WiFi coupe");
        }
    } else {
        if (reessai) basculeRallumerRadio(now);
        if (p.autonomeTient) {                     // l'autonomie retiree : la radio revient
            p.autonomeTient = false;
            b.depuis = now;
            b.retourA = now;
            b.retours++;
            b.derniereCause = "autonome";
            basculeRallumerRadio(now);
            NIDMI_WEB_LOG("[NiDMI] instrument autonome retire : WiFi rallume");
        }
        basculeCable(now);
    }
    publierEtatsSys();
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

    // Les etats de la carte, lisibles par r("sys.<nom>") : poses AVANT que les
    // taches des scripts ne demarrent, pour que le bus n'ait plus qu'a en
    // changer les valeurs (NiDMI.cpp, « LES FONCTIONS DE LA CARTE »).
    publierEtatsSys();

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
    wifiBoucle();
    if (g_relanceDemande) {
        g_relanceDemande = false;
        nidmi_usbnet::relancer();
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

    // Lien USB : épingle la tâche usbd au cœur de l'interruption (sans quoi
    // l'émission se fige, MESURES §147), suit le montage pour le netif, et
    // porte l'activation mDNS. N'annonce PAS l'état du lien : il appartient
    // au pilote NCM.
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
