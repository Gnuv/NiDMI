#include "MidiRouter.h"
#include "../mapping/MappingEngine.h"
#include "../mapping/ScriptStore.h"
#include <Preferences.h>
#include "CcMap.h"
#include <Arduino.h>

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
void MidiRouter::ccEntrant(uint8_t channel, uint8_t control, uint8_t value) {
    // 1. Apprentissage. Il precede l'application pour que la cible bouge des
    //    le geste qui l'apprend — sans ca il faut toucher le potentiometre une
    //    seconde fois pour entendre quoi que ce soit, et l'apprentissage a
    //    l'air de n'avoir rien fait.
    CcMap::apprendre(channel, control);
    // 2. Table CC -> parametre (moteur audio ou parametre de script).
    CcMap::appliquer(channel, control, value);
    // 3. Composants : comportement historique, conserve tel quel.
    g_componentManager.handleMidiControlChange(channel, control, value);
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

void MidiRouter::setScriptMidi(const String& script) {
    scriptEntrant = script;
    // Un script pousse EN LIGNE n'est plus celui du fichier : laisser
    // nomScriptActif tel quel faisait annoncer « transpose.nms » alors qu'un
    // tout autre code tournait. On dit ce qui est vrai.
    nomScriptActif = script.length() ? String("(en ligne)") : String("");
    Serial.printf("[MidiRouter] script MIDI entrant : %s\n",
                  scriptEntrant.length() ? scriptEntrant.c_str() : "(aucun)");
}

// Point d'entree unique de toute note ENTRANTE. Le script est applique ICI,
// donc identiquement pour l'USB, le clavier de l'app et le reste : c'est ce qui
// garantit qu'un bloc mapping vaut pour toutes les sources, et pas seulement
// pour celles qu'on aurait pense cabler une a une.
void MidiRouter::noteEntrante(uint8_t channel, uint8_t note, uint8_t velocity, bool estNoteOff) {
    uint8_t n = note, v = velocity, c = channel;

    if (scriptEntrant.length()) {
        MappingEngine::SortieNote sortie;
        if (MappingEngine::executeMidiNote(scriptEntrant.c_str(), note, velocity, channel,
                                           estNoteOff, sortie)) {
            n = sortie.note; v = sortie.velo; c = sortie.canal;
        }
        // Script present mais muet (aucun note.out) : on NE JOUE PAS. Un script
        // qui filtre doit pouvoir bloquer une note — sinon « filtrer » serait
        // impossible a exprimer.
        else return;
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


// ── Script nomme : contenu dans LittleFS, nom en NVS ───────────────────────
namespace {
constexpr const char* NVS_ESPACE_MIDI = "nidmi-midi";
constexpr const char* NVS_CLE_SCRIPT  = "script";
}

bool MidiRouter::chargerScriptNomme(const char* nom, bool persister) {
    if (!nom || !*nom) {                       // "" = plus de script du tout
        scriptEntrant = "";
        nomScriptActif = "";
        if (persister) {
            Preferences p;
            if (p.begin(NVS_ESPACE_MIDI, false)) { p.remove(NVS_CLE_SCRIPT); p.end(); }
        }
        Serial.println("[MidiRouter] script MIDI : aucun (passage direct)");
        return true;
    }
    String contenu;
    if (!ScriptStore::lire(nom, contenu)) {
        Serial.printf("[MidiRouter] script '%s' introuvable dans mapfs\n", nom);
        return false;
    }
    scriptEntrant  = contenu;
    nomScriptActif = nom;
    if (persister) {
        Preferences p;
        if (p.begin(NVS_ESPACE_MIDI, false)) { p.putString(NVS_CLE_SCRIPT, nomScriptActif); p.end(); }
    }
    Serial.printf("[MidiRouter] script '%s' charge (%u o)%s\n",
                  nom, (unsigned)contenu.length(), persister ? " et memorise" : "");
    return true;
}

void MidiRouter::restaurerScript() {
    Preferences p;
    if (!p.begin(NVS_ESPACE_MIDI, true)) return;
    const String nom = p.getString(NVS_CLE_SCRIPT, "");
    p.end();
    if (!nom.length()) return;
    if (!chargerScriptNomme(nom.c_str(), false)) {
        // Le fichier a disparu (mapfs efface, script supprime). On ne bloque
        // rien : la carte demarre en passage direct plutot qu'a moitie
        // configuree, et le nom reste en NVS au cas ou le fichier revienne.
        Serial.printf("[MidiRouter] script memorise '%s' absent — passage direct\n", nom.c_str());
    }
}
