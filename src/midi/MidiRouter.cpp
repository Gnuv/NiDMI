#include "MidiRouter.h"
#include "../mapping/MappingEngine.h"
#include "../mapping/ScriptStore.h"
#include <Preferences.h>
#include "CcMap.h"
#include <Arduino.h>
#include <esp_heap_caps.h>   // le plus gros bloc contigu : la vraie limite de la chaine
#include <new>               // std::nothrow : une allocation refusee se dit, elle ne jette pas

// Dépendances vers le serveur core
#include "../server/ServerCore.h"
#include "../managers/ComponentManager.h"
#include "../Globals.h"
#include "../audio/AudioEngine.h"

MidiRouter::MidiRouter()
    : rtpEnabled(true), oscEnabled(true), bluetoothEnabled(true), usbMidiEnabled(true), oscToSta(true), oscPort(8000), defaultChannel(1) {}

MidiRouter::~MidiRouter() {}

void MidiRouter::begin() {
    // Initialiser USB MIDI si supporté et activé
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().begin();
    }
}

void MidiRouter::update() {
    // Mise à jour RTP si nécessaire
    serverCore.rtpMidi().update();
}

void MidiRouter::sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    const uint8_t ch = channel ? channel : defaultChannel;
    if (rtpEnabled) {
        serverCore.rtpMidi().sendNoteOn(ch, note, velocity);
    }
    if (bluetoothEnabled) {
        serverCore.bluetooth().sendNoteOn(ch, note, velocity);
    }
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().sendNoteOn(ch, note, velocity);
    }
    // Optionnel: route OSC si disponible côté serveur
    // Activez avec -DNIDMI_ENABLE_OSC_ROUTER et implémentez les wrappers dans NiDMIServer
    // Écho local : les LEDs/bargraphs sont pilotés par note+canal via handleMidiNoteOn(), qui
    // n'était câblé qu'aux notes ENTRANTES (hooks RTP-MIDI). Sans cet écho, une note générée
    // localement par un composant du boîtier n'allume aucune LED du même boîtier.
    g_componentManager.handleMidiNoteOn(ch, note, velocity);

    // Écho AUDIO : le boîtier s'entend lui-même. Le moteur démarre à la
    // première note (init paresseuse) — rien au boot, l'OTA reste sauf.
    AudioEngine::noteOn(note, velocity);

    #ifdef NIDMI_ENABLE_OSC_ROUTER
    if (oscEnabled) {
        serverCore.sendOscNote(ch, note, velocity, oscToSta, oscPort);
    }
    #endif
}

void MidiRouter::sendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
    const uint8_t ch = channel ? channel : defaultChannel;
    if (rtpEnabled) {
        serverCore.rtpMidi().sendNoteOff(ch, note, velocity);
    }
    if (bluetoothEnabled) {
        serverCore.bluetooth().sendNoteOff(ch, note, velocity);
    }
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().sendNoteOff(ch, note, velocity);
    }
    // Écho local (cf. sendNoteOn) : éteint les LEDs appairées à cette note.
    g_componentManager.handleMidiNoteOff(ch, note, velocity);

    AudioEngine::noteOff(note);

    #ifdef NIDMI_ENABLE_OSC_ROUTER
    if (oscEnabled) {
        serverCore.sendOscNoteOff(ch, note, velocity, oscToSta, oscPort);
    }
    #endif
}

void MidiRouter::sendControlChange(uint8_t channel, uint8_t control, uint8_t value) {
    const uint8_t ch = channel ? channel : defaultChannel;
    if (rtpEnabled) {
        serverCore.rtpMidi().sendControlChange(ch, control, value);
    }
    if (bluetoothEnabled) {
        serverCore.bluetooth().sendControlChange(ch, control, value);
    }
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().sendControlChange(ch, control, value);
    }
    #ifdef NIDMI_ENABLE_OSC_ROUTER
    if (oscEnabled) {
        serverCore.sendOscCC(ch, control, value, oscToSta, oscPort);
    }
    #endif
}

void MidiRouter::sendProgramChange(uint8_t channel, uint8_t program) {
    const uint8_t ch = channel ? channel : defaultChannel;
    if (rtpEnabled) {
        serverCore.rtpMidi().sendProgramChange(ch, program);
    }
    if (bluetoothEnabled) {
        serverCore.bluetooth().sendProgramChange(ch, program);
    }
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().sendProgramChange(ch, program);
    }
}

void MidiRouter::sendPitchBend(uint8_t channel, int bend) {
    const uint8_t ch = channel ? channel : defaultChannel;
    if (rtpEnabled) {
        serverCore.rtpMidi().sendPitchBend(ch, bend);
    }
    if (bluetoothEnabled) {
        serverCore.bluetooth().sendPitchBend(ch, bend);
    }
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().sendPitchBend(ch, bend);
    }
}

void MidiRouter::sendAftertouch(uint8_t channel, uint8_t pressure) {
    const uint8_t ch = channel ? channel : defaultChannel;
    if (rtpEnabled) {
        serverCore.rtpMidi().sendAftertouch(ch, pressure);
    }
    if (bluetoothEnabled) {
        // BluetoothManager n'a pas sendAftertouch, on peut l'ignorer ou l'implémenter plus tard
    }
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().sendAftertouch(ch, pressure);
    }
}

void MidiRouter::sendKeyPressure(uint8_t channel, uint8_t note, uint8_t pressure) {
    const uint8_t ch = channel ? channel : defaultChannel;
    if (rtpEnabled) {
        serverCore.rtpMidi().sendKeyPressure(ch, note, pressure);
    }
    if (bluetoothEnabled) {
        // BluetoothManager n'a pas sendKeyPressure, on peut l'ignorer ou l'implémenter plus tard
    }
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().sendKeyPressure(ch, note, pressure);
    }

    // Écho local (cf. sendNoteOn) : module la luminosité des LEDs en mode PWM appairées
    // à cette note, pour que l'aftertouch soit visible sur le boîtier lui-même.
    g_componentManager.handleMidiKeyPressure(ch, note, pressure);
    #ifdef NIDMI_ENABLE_OSC_ROUTER
    if (oscEnabled) {
        serverCore.sendOscKeyPressure(ch, note, pressure, oscToSta, oscPort);
    }
    #endif
}

void MidiRouter::sendClock() {
    if (rtpEnabled) {
        serverCore.rtpMidi().sendClock();
    }
    if (bluetoothEnabled) {
        // BluetoothManager n'a pas sendClock, on peut l'ignorer ou l'implémenter plus tard
    }
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().sendClock();
    }
}

void MidiRouter::sendStart() {
    if (rtpEnabled) {
        serverCore.rtpMidi().sendStart();
    }
    if (bluetoothEnabled) {
        // BluetoothManager n'a pas sendStart, on peut l'ignorer ou l'implémenter plus tard
    }
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().sendStart();
    }
}

void MidiRouter::sendStop() {
    if (rtpEnabled) {
        serverCore.rtpMidi().sendStop();
    }
    if (bluetoothEnabled) {
        // BluetoothManager n'a pas sendStop, on peut l'ignorer ou l'implémenter plus tard
    }
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().sendStop();
    }
}

void MidiRouter::sendContinue() {
    if (rtpEnabled) {
        serverCore.rtpMidi().sendContinue();
    }
    if (bluetoothEnabled) {
        // BluetoothManager n'a pas sendContinue, on peut l'ignorer ou l'implémenter plus tard
    }
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().sendContinue();
    }
}

void MidiRouter::enableRtpMidi(bool enabled) { rtpEnabled = enabled; }
void MidiRouter::enableOsc(bool enabled) { oscEnabled = enabled; }
void MidiRouter::enableBluetooth(bool enabled) { bluetoothEnabled = enabled; }
void MidiRouter::enableUsbMidi(bool enabled) { usbMidiEnabled = enabled; }
void MidiRouter::setOscTargetSta(bool sta) { oscToSta = sta; }
void MidiRouter::setOscPort(uint16_t port) { oscPort = port; }
void MidiRouter::setMidiChannel(uint8_t channel) { defaultChannel = channel; }

void MidiRouter::handleMidiNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    // Transmettre au ComponentManager pour piloter les LEDs
    g_componentManager.handleMidiNoteOn(channel, note, velocity);
}

void MidiRouter::handleMidiNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
    // Transmettre au ComponentManager pour piloter les LEDs
    g_componentManager.handleMidiNoteOff(channel, note, velocity);
}

void MidiRouter::handleMidiControlChange(uint8_t channel, uint8_t control, uint8_t value) {
    // Transmettre au ComponentManager pour piloter les LEDs
    g_componentManager.handleMidiControlChange(channel, control, value);
}

// Point d'entree unique de tout CC ENTRANT — pendant de noteEntrante. Toutes
// les sources (USB, RTP, et demain la WebSocket de l'app) passent par ici,
// sinon la table ne vaudrait que pour celles qu'on aurait pense cabler : c'est
// exactement l'erreur qui avait ete faite sur les notes.
/* LA CHAINE DES SCRIPTS MAP, pour un CC.
 *
 * Extraite de ccEntrant pour etre EPROUVABLE : sans point d'entree, la chaine
 * n'etait verifiable qu'en jouant du MIDI sur la carte, donc en pratique jamais.
 * /api/midi/chaine l'appelle telle quelle — ce que l'essai prouve est
 * exactement ce que la production execute, pas une copie qui divergera.
 *
 * Rend false si un emplacement a AVALE l'evenement (traite sans emission) :
 * c'est ce qui rend un filtre exprimable.
 */
bool MidiRouter::chaineScriptsCc(uint8_t& c, uint8_t& n, uint8_t& v) {
    for (uint8_t e = 0; e < emplacements.size(); e++) {
        Emplacement& em = *emplacements[e];
        if (!em.contenu.length()) continue;
        MappingEngine::SortieCc sortie;
        MappingEngine::executeMidiCc(em.contenu.c_str(), n, v, c, sortie,
                                     em.etats, MappingEngine::MAX_PIPELINES_SCRIPT);
        if (sortie.emise) { c = sortie.canal; n = sortie.cc; v = sortie.valeur; }
        else if (sortie.traite) return false;
        // Ni emise ni traite : cet emplacement ne parle pas de CC, on passe.
    }
    return true;
}

void MidiRouter::ccEntrant(uint8_t channel, uint8_t control, uint8_t value) {
    uint8_t c = channel, n = control, v = value;

    // 1. Le SCRIPT d'abord, comme pour les notes : ce qui suit doit voir le CC
    //    tel que le .nms l'a decide, pas tel qu'il est arrive. Un ctl.out() qui
    //    renumerote un CC doit donc renumeroter aussi ce qu'apprend CcMap.
    if (!chaineScriptsCc(c, n, v)) return;   // un emplacement a avale le CC

    // 2. Apprentissage. Il precede l'application pour que la cible bouge des
    //    le geste qui l'apprend — sans ca il faut toucher le potentiometre une
    //    seconde fois pour entendre quoi que ce soit, et l'apprentissage a
    //    l'air de n'avoir rien fait.
    CcMap::apprendre(c, n);
    // 3. Table CC -> parametre (moteur audio ou parametre de script).
    CcMap::appliquer(c, n, v);
    // 4. Composants : comportement historique, conserve tel quel.
    g_componentManager.handleMidiControlChange(c, n, v);
}




// ── Script .nms sur le MIDI entrant ────────────────────────────────────────
void MidiRouter::setParamsScript(const String& params) {
    int debut = 0;
    while (debut < (int)params.length()) {
        int fin = params.indexOf(';', debut);
        if (fin < 0) fin = params.length();
        String kv = params.substring(debut, fin);
        const int eq = kv.indexOf('=');
        if (eq > 0) {
            String cle = kv.substring(0, eq); cle.trim();
            const float v = kv.substring(eq + 1).toFloat();
            FluxRegistry::update(cle.c_str(), v);
        }
        debut = fin + 1;
    }
}

void MidiRouter::battreHorloge(uint32_t maintenant) {
    for (uint8_t e = 0; e < emplacements.size(); e++) {
        Emplacement& em = *emplacements[e];
        if (!em.contenu.length()) continue;
        /* Init d'abord : loadbang() doit partir avant le premier metro(). On le
         * differe jusqu'ici plutot que de l'emettre depuis le gestionnaire HTTP —
         * emettre du MIDI depuis async_tcp, c'est le genre de raccourci qui finit
         * en tache bloquee. */
        /* QUI execute : « map:0 »..« map:3 ». Sans cela un print() d'emplacement
         * arrivait a l'app sans origine, indistinguable de celui d'une broche. */
        char org[12]; snprintf(org, sizeof org, "map:%u", (unsigned)e);
        if (em.initEnAttente) {
            em.initEnAttente = false;
            MappingEngine::battre(em.contenu.c_str(), MappingEngine::Evenement::Init,
                                  maintenant, this, em.etats,
                                  MappingEngine::MAX_PIPELINES_SCRIPT, org);
        }
        MappingEngine::battre(em.contenu.c_str(), MappingEngine::Evenement::Tick,
                              maintenant, this, em.etats,
                              MappingEngine::MAX_PIPELINES_SCRIPT, org);
    }
}

void MidiRouter::recevoirOsc(const char* adresse, float valeur) {
    for (uint8_t e = 0; e < emplacements.size(); e++) {
        Emplacement& em = *emplacements[e];
        if (!em.contenu.length()) continue;
        char org[12]; snprintf(org, sizeof org, "map:%u", (unsigned)e);
        MappingEngine::battreOsc(em.contenu.c_str(), adresse, valeur, this,
                                 em.etats, MappingEngine::MAX_PIPELINES_SCRIPT, org);
    }
}

// ── Script nomme : contenu dans LittleFS, nom en NVS ───────────────────────
namespace {
constexpr const char* NVS_ESPACE_MIDI = "nidmi-midi";
constexpr const char* NVS_CLE_SCRIPT  = "script";
}

static String cleNvsEmplacement(uint8_t e) {
    return e == 0 ? String(NVS_CLE_SCRIPT) : (String(NVS_CLE_SCRIPT) + String((int)e));
}

/* Le nom du fichier ou la carte range un script pousse EN LIGNE.
 * Le tiret bas dit qu'il appartient a la carte, pas a l'utilisateur : il ne se
 * confond pas avec transpose.nms ou grave5.nms. */
static String fichierEmplacement(uint8_t e) {
    return String("_emplacement") + String((int)e) + ".nms";
}

void MidiRouter::setScriptMidi(const String& script, uint8_t emplacement) {
    Emplacement* pem = _assurerEmplacement(emplacement);
    if (!pem) return;                 // plafond ou memoire : deja dit sur le port serie
    Emplacement& em = *pem;
    const bool memeCode = (em.contenu == script);

    /* ⚠ LES REPRISES DE L'ANCIEN TEXTE, D'ABORD.
     * Une reprise en attente — del(), makenote(), lag(), ramp() — retient un
     * `const char*` sur le texte du script. Remplacer `em.contenu` libere ce
     * tampon (ou, pire, le REUTILISE pour le nouveau texte) : la file rejouait
     * alors sur de la memoire rendue, ou sur le segment d'un autre script.
     * `viderDifferes` existait pour ca et n'etait appelee QUE par le chemin des
     * broches (ComponentManager) — les emplacements de scripts map ne l'ont
     * jamais appelee. Trouve par le compteur de /api/diag/reservoirs : apres
     * avoir vide l'emplacement 0, la file annoncait encore 16 places tenues. */
    MappingEngine::viderDifferes(em.contenu.c_str());
    em.contenu = script;
    em.initEnAttente = true;          // nouveau script : son loadbang() est du
    /* L'etat par pipeline de CET emplacement n'a plus de sens : un counter
     * reprendrait la ou en etait le script precedent — decalage silencieux, et
     * d'autant plus deroutant qu'il ne se voit qu'a la deuxieme note. On ne
     * touche PAS aux autres emplacements, qui n'ont pas change. */
    for (int i = 0; i < MappingEngine::MAX_PIPELINES_SCRIPT; i++) em.etats[i].reinitialiser();

    /* UN SCRIPT POUSSE EN LIGNE EST PERSISTE, comme les autres.
     *
     * Il ne l'etait pas : le contenu vivait en RAM et le nom valait « (en
     * ligne) ». Au redemarrage il disparaissait — et la carte revenait au
     * dernier script NOMME, donc a un AUTRE comportement, sans rien dire.
     * Mesure : « note.in() : +(7) » pousse, puis redemarrage, puis
     * transpose.nms de retour. C'est la negation de la regle du projet — ce qui
     * tourne en headless doit etre ce qu'on a configure.
     *
     * On applique donc la regle du §9.3 ICI plutot que de demander a chaque
     * client de le faire : le contenu part au fichier, le nom en NVS. Tout
     * client en beneficie, pas seulement notre app.
     *
     * L'ecriture est evitee quand le code n'a pas change : l'app republie a
     * chaque enregistrement, et la flash n'a pas a payer une republication
     * identique. */
    const String cle = cleNvsEmplacement(emplacement);
    if (!script.length()) {
        em.nom = "";
        Preferences p;
        if (p.begin(NVS_ESPACE_MIDI, false)) { p.remove(cle.c_str()); p.end(); }
        Serial.printf("[MidiRouter] emplacement %u : vide\n", (unsigned)emplacement);
        return;
    }

    const String fichier = fichierEmplacement(emplacement);
    if (!memeCode || em.nom != fichier) {
        if (ScriptStore::ecrire(fichier.c_str(), script)) {
            em.nom = fichier;
            Preferences p;
            if (p.begin(NVS_ESPACE_MIDI, false)) { p.putString(cle.c_str(), em.nom); p.end(); }
        } else {
            /* mapfs pleine ou absente : le script TOURNE quand meme, mais il ne
             * survivra pas au redemarrage. On le DIT — un comportement qui
             * s'evapore sans message est ce qu'on passe la session a supprimer. */
            em.nom = "(en ligne, NON persiste)";
            Serial.printf("[MidiRouter] emplacement %u : ecriture de '%s' impossible — "
                          "le script tourne mais ne survivra pas au redemarrage\n",
                          (unsigned)emplacement, fichier.c_str());
        }
    }
    Serial.printf("[MidiRouter] emplacement %u : %u o (%s)\n",
                  (unsigned)emplacement, (unsigned)script.length(), em.nom.c_str());
}

// Point d'entree unique de toute note ENTRANTE. Le script est applique ICI,
// donc identiquement pour l'USB, le clavier de l'app et le reste : c'est ce qui
// garantit qu'un bloc mapping vaut pour toutes les sources, et pas seulement
// pour celles qu'on aurait pense cabler une a une.
void MidiRouter::noteEntrante(uint8_t channel, uint8_t note, uint8_t velocity, bool estNoteOff) {
    uint8_t n = note, v = velocity, c = channel;

    for (uint8_t e = 0; e < emplacements.size(); e++) {
        Emplacement& em = *emplacements[e];
        if (!em.contenu.length()) continue;
        MappingEngine::SortieNote sortie;
        if (MappingEngine::executeMidiNote(em.contenu.c_str(), n, v, c,
                                           estNoteOff, sortie,
                                           em.etats, MappingEngine::MAX_PIPELINES_SCRIPT)) {
            n = sortie.note; v = sortie.velo; c = sortie.canal;
        }
        // Script qui PARLE de notes mais n'a rien emis pour celle-ci : on NE
        // JOUE PAS. Un script qui filtre doit pouvoir bloquer une note — sinon
        // « filtrer » serait impossible a exprimer.
        // Mais un script qui ne parle QUE de CC ne doit rien bloquer : sans
        // cette nuance, charger un mapping de controleurs rendait le clavier
        // muet. Symetrique de ce que fait ccEntrant pour les CC.
        else if (sortie.traite) return;
    }

    // Les LEDs appairees suivent la note REELLEMENT jouee, pas la note brute.
    if (estNoteOff) {
        g_componentManager.handleMidiNoteOff(c, n, v);
        AudioEngine::noteOff(n);
    } else {
        g_componentManager.handleMidiNoteOn(c, n, v);
        AudioEngine::noteOn(n, v);
    }
}



/* ── LA CHAINE SE DIMENSIONNE ──────────────────────────────────────────────
 *
 * Un emplacement coute son objet (deux String et 12 etats de pipeline, soit
 * ~540 o) plus le texte de son script. Rien de tout cela n'est pris tant qu'il
 * n'existe pas : une composition sans piste map ne paie aucun emplacement, la
 * ou les quatre tableaux fixes prenaient 2,1 ko de .bss en permanence.
 *
 * LA VRAIE LIMITE EST LA MEMOIRE, pas un nombre. On refuse d'allouer si le plus
 * gros bloc contigu interne tomberait sous un plancher — 12 ko, seuil sous
 * lequel la pile reseau cesse de servir (MESURES, pieges de mesure). Refuser a
 * la CONFIGURATION est un message ; manquer de memoire pendant une performance
 * est une panne. */
namespace { constexpr size_t PLANCHER_BLOC_CONTIGU = 12288; }

MidiRouter::Emplacement* MidiRouter::_assurerEmplacement(uint8_t e) {
    if (e >= PLAFOND_SCRIPTS_MAP) {
        Serial.printf("[MidiRouter] emplacement %u refuse : le plafond d'index est %u\n",
                      (unsigned)e, (unsigned)PLAFOND_SCRIPTS_MAP);
        return nullptr;
    }
    while (emplacements.size() <= e) {
        const size_t bloc = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        if (bloc < PLANCHER_BLOC_CONTIGU + sizeof(Emplacement)) {
            Serial.printf("[MidiRouter] emplacement %u refuse : plus gros bloc %u o, "
                          "plancher %u\n", (unsigned)emplacements.size(),
                          (unsigned)bloc, (unsigned)PLANCHER_BLOC_CONTIGU);
            return nullptr;
        }
        Emplacement* em = new (std::nothrow) Emplacement();
        if (!em) {
            Serial.printf("[MidiRouter] emplacement %u refuse : allocation impossible\n",
                          (unsigned)emplacements.size());
            return nullptr;
        }
        emplacements.push_back(em);
    }
    return emplacements[e];
}

void MidiRouter::_reduireChaine(uint8_t n) {
    if (emplacements.size() <= n) return;
    /* ET LEURS NOMS EN NVS. Sans ca, raccourcir la chaine laissait « script1 »,
     * « script2 »... derriere elle : des cles orphelines dans le reservoir le
     * plus tendu de la carte (630 entrees pour TOUT, §101). Vu au compteur —
     * 114 entrees avant l'essai, 129 apres, et elles ne redescendaient pas.
     * Une seule ouverture de NVS pour toute la reduction. */
    Preferences p;
    const bool nvs = p.begin(NVS_ESPACE_MIDI, false);
    while (emplacements.size() > n) {
        Emplacement* em = emplacements.back();
        /* Les reprises retiennent un pointeur sur le texte : les purger AVANT
         * de liberer, sinon la file rejouerait sur de la memoire rendue (§101). */
        MappingEngine::viderDifferes(em->contenu.c_str());
        delete em;
        emplacements.pop_back();
        if (nvs) p.remove(cleNvsEmplacement((uint8_t)emplacements.size()).c_str());
    }
    if (nvs) p.end();
}

/* La longueur voulue par la COMPOSITION. Rend ce qu'on a vraiment obtenu : si
 * la memoire a manque, c'est la carte qui le dit, pas l'app qui le suppose. */
uint8_t MidiRouter::dimensionnerChaine(uint8_t n) {
    if (n > PLAFOND_SCRIPTS_MAP) n = PLAFOND_SCRIPTS_MAP;
    if (n < emplacements.size()) _reduireChaine(n);
    else if (n > 0)              _assurerEmplacement((uint8_t)(n - 1));
    const uint8_t obtenu = (uint8_t)emplacements.size();
    /* La longueur va en NVS avec les noms : sans elle, une carte redemarree
     * retrouverait ses scripts mais pas sa chaine, et les emplacements au-dela
     * du premier seraient muets sans rien dire. */
    Preferences p;
    if (p.begin(NVS_ESPACE_MIDI, false)) { p.putUChar("nmap", obtenu); p.end(); }
    Serial.printf("[MidiRouter] chaine dimensionnee a %u emplacement(s)%s\n",
                  (unsigned)obtenu, (obtenu < n) ? " — memoire insuffisante" : "");
    return obtenu;
}

/* La cle NVS d'un emplacement : « script » pour le premier — le nom historique,
 * qu'on garde pour ne pas perdre la configuration des cartes existantes — puis
 * « script1 », « script2 »... */

bool MidiRouter::chargerScriptNomme(const char* nom, bool persister, uint8_t emplacement) {
    Emplacement* pem = _assurerEmplacement(emplacement);
    if (!pem) return false;
    Emplacement& em = *pem;
    const String cle = cleNvsEmplacement(emplacement);

    if (!nom || !*nom) {                       // "" = plus de script du tout
        MappingEngine::viderDifferes(em.contenu.c_str());   // voir setScriptMidi
        em.contenu = "";
        em.nom = "";
        for (int i = 0; i < MappingEngine::MAX_PIPELINES_SCRIPT; i++) em.etats[i].reinitialiser();
        if (persister) {
            Preferences p;
            if (p.begin(NVS_ESPACE_MIDI, false)) { p.remove(cle.c_str()); p.end(); }
        }
        Serial.printf("[MidiRouter] emplacement %u : aucun script\n", (unsigned)emplacement);
        return true;
    }
    String contenu;
    if (!ScriptStore::lire(nom, contenu)) {
        Serial.printf("[MidiRouter] script '%s' introuvable dans mapfs\n", nom);
        return false;
    }
    MappingEngine::viderDifferes(em.contenu.c_str());       // voir setScriptMidi
    em.contenu = contenu;
    em.nom     = nom;
    em.initEnAttente = true;
    for (int i = 0; i < MappingEngine::MAX_PIPELINES_SCRIPT; i++) em.etats[i].reinitialiser();
    if (persister) {
        Preferences p;
        if (p.begin(NVS_ESPACE_MIDI, false)) { p.putString(cle.c_str(), em.nom); p.end(); }
    }
    Serial.printf("[MidiRouter] emplacement %u : '%s' charge (%u o)%s\n",
                  (unsigned)emplacement, nom, (unsigned)contenu.length(),
                  persister ? " et memorise" : "");
    return true;
}

void MidiRouter::restaurerScript() {
    Preferences p;
    if (!p.begin(NVS_ESPACE_MIDI, true)) return;
    /* LA LONGUEUR DE LA CHAINE d'abord : c'est elle qui dit combien de noms
     * lire. Sans elle on scannerait le plafond entier — 64 lectures NVS a
     * chaque demarrage pour retrouver, le plus souvent, zero script. */
    const uint8_t nmap = p.getUChar("nmap", 0);
    std::vector<String> noms(nmap);
    for (uint8_t e = 0; e < nmap; e++)
        noms[e] = p.getString(cleNvsEmplacement(e).c_str(), "");
    p.end();
    if (nmap) _assurerEmplacement((uint8_t)(nmap - 1));
    Serial.printf("[MidiRouter] chaine restauree : %u emplacement(s)\n",
                  (unsigned)emplacements.size());
    for (uint8_t e = 0; e < nmap; e++) {
        if (!noms[e].length()) continue;
        if (!chargerScriptNomme(noms[e].c_str(), false, e)) {
            // Le fichier a disparu (mapfs efface, script supprime). On ne bloque
            // rien : l'emplacement reste vide plutot qu'a moitie configure, et le
            // nom reste en NVS au cas ou le fichier revienne.
            Serial.printf("[MidiRouter] emplacement %u : '%s' absent — laisse vide\n",
                          (unsigned)e, noms[e].c_str());
        }
    }
}
