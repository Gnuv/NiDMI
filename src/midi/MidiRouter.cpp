#include "MidiRouter.h"
#include "../config/EcrituresDifferees.h"
#include "../mapping/MappingEngine.h"
#include "../mapping/ScriptStore.h"
#include "../mapping/Repertoire.h"
#include <LittleFS.h>
#include <Preferences.h>
#include "CcMap.h"
#include "TransportNotes.h"   // a pedal on the next cue, swallowed before the scripts (§215)
#include <Arduino.h>
#include <esp_heap_caps.h>   // le plus gros bloc contigu : la vraie limite de la chaine
#include <new>               // std::nothrow : une allocation refusee se dit, elle ne jette pas
#include <atomic>

// Dépendances vers le serveur core
#include "../server/ServerCore.h"
#include "../managers/ComponentManager.h"
#include "../Globals.h"
#include "../audio/AudioEngine.h"
#include "../audio/SfzStore.h"   // controle() : les CC des portes locc/hicc du lecteur SFZ

/* ── LE RTP-MIDI SORTANT PASSE PAR LA BOUCLE (MESURES §175) ────────────────
 * La bibliotheque AppleMIDI n'est pas faite pour deux taches : un envoi
 * EMPILE dans son tampon (outMidiBuffer), que la boucle vide en servant la
 * session (available(), dans update()) — et quand le tampon est presque
 * plein, l'envoi part d'UDP depuis la tache qui ecrit, sur le meme objet
 * WiFiUDP que la boucle. Or les scripts et les broches emettent depuis
 * MidiTask (coeur 0), la boucle lit sur le coeur 1 : deux taches dans le meme
 * tampon, des qu'une session est ouverte. On DEPOSE donc (file PSRAM de 256,
 * jamais bloquante), et la boucle emet juste avant de servir la session
 * (emettreRtp) : une seule tache touche AppleMIDI. Sans session, rien n'entre
 * — la bibliotheque n'aurait rien envoye (beginTransmission). Le RTP est de
 * l'UDP : une perte par nature ; la file pleine se compte. */
namespace {
enum : uint8_t { RtpNoteOn, RtpNoteOff, RtpCc, RtpPgm, RtpBend, RtpTouch, RtpPoly,
                 RtpHorloge, RtpStart, RtpStop, RtpContinue };
struct EnvoiRtp { uint8_t genre, canal, a, b; int16_t bend; };
constexpr UBaseType_t kEnvoisRtpMax = 256;
StaticQueue_t g_fileRtpTcb;
QueueHandle_t g_fileRtp = nullptr;
std::atomic<uint32_t> g_rtpJetes{0};

void deposerRtp(uint8_t genre, uint8_t canal = 0, uint8_t a = 0, uint8_t b = 0, int16_t bend = 0) {
    if (!g_fileRtp || !serverCore.rtpMidi().isConnected()) return;
    const EnvoiRtp e{genre, canal, a, b, bend};
    if (xQueueSend(g_fileRtp, &e, 0) != pdTRUE) g_rtpJetes.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace

/* loopTask SEULEMENT, juste avant de servir la session (ServerCore::update). */
void MidiRouter::emettreRtp() {
    if (!g_fileRtp) return;
    EnvoiRtp e;
    auto& rtp = serverCore.rtpMidi();
    for (int k = 0; k < 64 && xQueueReceive(g_fileRtp, &e, 0) == pdTRUE; k++) {
        switch (e.genre) {
            case RtpNoteOn:   rtp.sendNoteOn(e.canal, e.a, e.b);         break;
            case RtpNoteOff:  rtp.sendNoteOff(e.canal, e.a, e.b);        break;
            case RtpCc:       rtp.sendControlChange(e.canal, e.a, e.b);  break;
            case RtpPgm:      rtp.sendProgramChange(e.canal, e.a);       break;
            case RtpBend:     rtp.sendPitchBend(e.canal, e.bend);        break;
            case RtpTouch:    rtp.sendAftertouch(e.canal, e.a);          break;
            case RtpPoly:     rtp.sendKeyPressure(e.canal, e.a, e.b);    break;
            case RtpHorloge:  rtp.sendClock();                           break;
            case RtpStart:    rtp.sendStart();                           break;
            case RtpStop:     rtp.sendStop();                            break;
            case RtpContinue: rtp.sendContinue();                        break;
            default: break;
        }
    }
}

uint32_t MidiRouter::rtpJetes() { return g_rtpJetes.load(std::memory_order_relaxed); }

/* Voir « LE MIDI COMPTE AUSSI POUR LE SILENCE » (MidiRouter.h). 0 au
 * demarrage : un long silence, rien n'est passe. */
namespace {
std::atomic<uint32_t> g_dernierTraficMs{0};
}
void MidiRouter::noterTrafic() { g_dernierTraficMs.store(millis(), std::memory_order_relaxed); }
uint32_t MidiRouter::silenceMidiDepuisMs() {
    // Le dernier passage D'ABORD, l'heure ensuite : dans cet ordre la
    // difference non signee est juste (meme raisonnement que silenceDepuisMs).
    const uint32_t dernier = g_dernierTraficMs.load(std::memory_order_relaxed);
    return millis() - dernier;
}

MidiRouter::MidiRouter()
    : rtpEnabled(true), oscEnabled(true), bluetoothEnabled(true), usbMidiEnabled(true), oscToSta(true), oscPort(8000), defaultChannel(1) {}

MidiRouter::~MidiRouter() {}

void MidiRouter::begin() {
    if (!g_fileRtp) {
        /* La file du RTP sortant, en PSRAM : seules des taches y touchent. */
        uint8_t* stock = (uint8_t*)heap_caps_malloc(kEnvoisRtpMax * sizeof(EnvoiRtp),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (stock) g_fileRtp = xQueueCreateStatic(kEnvoisRtpMax, sizeof(EnvoiRtp), stock, &g_fileRtpTcb);
    }
    // Initialiser USB MIDI si supporté et activé
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().begin();
    }
}

void MidiRouter::sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
    noterTrafic();
    const uint8_t ch = channel ? channel : defaultChannel;
    if (rtpEnabled) {
        deposerRtp(RtpNoteOn, ch, note, velocity);
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
    // Vers les instruments de la cue du maillon qui l'emet (§212), et de son groupe (§221).
    AudioEngine::noteOn(ch, note, velocity, pourCourant(), groupeCourant());

    #ifdef NIDMI_ENABLE_OSC_ROUTER
    if (oscEnabled) {
        serverCore.sendOscNote(ch, note, velocity, oscToSta, oscPort);
    }
    #endif
}

void MidiRouter::sendNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) {
    noterTrafic();
    const uint8_t ch = channel ? channel : defaultChannel;
    if (rtpEnabled) {
        deposerRtp(RtpNoteOff, ch, note, velocity);
    }
    if (bluetoothEnabled) {
        serverCore.bluetooth().sendNoteOff(ch, note, velocity);
    }
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().sendNoteOff(ch, note, velocity);
    }
    // Écho local (cf. sendNoteOn) : éteint les LEDs appairées à cette note.
    g_componentManager.handleMidiNoteOff(ch, note, velocity);

    AudioEngine::noteOff(ch, note, pourCourant(), groupeCourant());

    #ifdef NIDMI_ENABLE_OSC_ROUTER
    if (oscEnabled) {
        serverCore.sendOscNoteOff(ch, note, velocity, oscToSta, oscPort);
    }
    #endif
}

void MidiRouter::sendControlChange(uint8_t channel, uint8_t control, uint8_t value) {
    noterTrafic();
    const uint8_t ch = channel ? channel : defaultChannel;
    if (rtpEnabled) {
        deposerRtp(RtpCc, ch, control, value);
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
    noterTrafic();
    const uint8_t ch = channel ? channel : defaultChannel;
    if (rtpEnabled) {
        deposerRtp(RtpPgm, ch, program);
    }
    if (bluetoothEnabled) {
        serverCore.bluetooth().sendProgramChange(ch, program);
    }
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().sendProgramChange(ch, program);
    }
}

void MidiRouter::sendPitchBend(uint8_t channel, int bend) {
    noterTrafic();
    const uint8_t ch = channel ? channel : defaultChannel;
    if (rtpEnabled) {
        deposerRtp(RtpBend, ch, 0, 0, (int16_t)bend);
    }
    if (bluetoothEnabled) {
        serverCore.bluetooth().sendPitchBend(ch, bend);
    }
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().sendPitchBend(ch, bend);
    }
}

void MidiRouter::sendAftertouch(uint8_t channel, uint8_t pressure) {
    noterTrafic();
    const uint8_t ch = channel ? channel : defaultChannel;
    if (rtpEnabled) {
        deposerRtp(RtpTouch, ch, pressure);
    }
    if (bluetoothEnabled) {
        // BluetoothManager n'a pas sendAftertouch, on peut l'ignorer ou l'implémenter plus tard
    }
    if (usbMidiEnabled && serverCore.usbMidi().isSupported()) {
        serverCore.usbMidi().sendAftertouch(ch, pressure);
    }
}

void MidiRouter::sendKeyPressure(uint8_t channel, uint8_t note, uint8_t pressure) {
    noterTrafic();
    const uint8_t ch = channel ? channel : defaultChannel;
    if (rtpEnabled) {
        deposerRtp(RtpPoly, ch, note, pressure);
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
        deposerRtp(RtpHorloge);
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
        deposerRtp(RtpStart);
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
        deposerRtp(RtpStop);
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
        deposerRtp(RtpContinue);
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
/* WHOSE SCRIPTS RUN (§212) — see MidiRouter.h — and of which group (§221): what the run sets
 * aside (a del()) keeps both in its tag. Nested contexts restore the outer one. */
struct MidiRouter::Contexte {
    MidiRouter& r; TaskHandle_t tache; AudioEngine::Pour pour; uint8_t groupe, tag;
    Contexte(MidiRouter& r_, AudioEngine::Pour p, uint8_t g = AudioEngine::GROUP_ALL)
        : r(r_), tache(r_._tacheEmission), pour(r_._pourEmission), groupe(r_._groupeEmission),
          tag(MappingEngine::tag()) {
        r._tacheEmission = xTaskGetCurrentTaskHandle(); r._pourEmission = p; r._groupeEmission = g;
        MappingEngine::setTag(tagDe(p, g));
    }
    ~Contexte() {
        r._tacheEmission = tache; r._pourEmission = pour; r._groupeEmission = groupe;
        MappingEngine::setTag(tag);
    }
};

AudioEngine::Pour MidiRouter::pourCourant() const {
    return (_tacheEmission && xTaskGetCurrentTaskHandle() == _tacheEmission) ? _pourEmission
                                                                            : AudioEngine::Pour::TOUS;
}

uint8_t MidiRouter::groupeCourant() const {
    return (_tacheEmission && xTaskGetCurrentTaskHandle() == _tacheEmission) ? _groupeEmission
                                                                            : AudioEngine::GROUP_ALL;
}

/* A tag: the branch in bits 5-6, the group in bits 0-4 (31 = every group); TAG_NONE (bit 7) when
 * nobody said. */
uint8_t MidiRouter::tagDe(AudioEngine::Pour pour, uint8_t groupe) {
    return (uint8_t)((((uint8_t)pour & 3u) << 5) | (groupe > AudioEngine::GROUP_MAX ? 31u : groupe));
}

void MidiRouter::emissionDifferee(uint8_t tag) {
    if (tag == MappingEngine::TAG_NONE) {
        if (!_differeEnCours) return;
        _tacheEmission = _tacheAvantDiffere; _pourEmission = _pourAvantDiffere; _groupeEmission = _groupeAvantDiffere;
        _differeEnCours = false;
        return;
    }
    if (!_differeEnCours) {
        _tacheAvantDiffere = _tacheEmission; _pourAvantDiffere = _pourEmission; _groupeAvantDiffere = _groupeEmission;
        _differeEnCours = true;
    }
    const uint8_t g = tag & 31u;
    _tacheEmission  = xTaskGetCurrentTaskHandle();
    _pourEmission   = (AudioEngine::Pour)((tag >> 5) & 3u);
    _groupeEmission = (g == 31u) ? AudioEngine::GROUP_ALL : g;
}

void MidiRouter::setLinkGroups(const uint8_t* groups, uint8_t n) {
    MappingEngine::Verrou verrou;   // a link's group does not change under a note (§170)
    for (size_t e = 0; e < emplacements.size(); e++) {
        const uint8_t g = (e < n) ? groups[e] : 0;
        emplacements[e]->groupe = (g > AudioEngine::GROUP_MAX) ? AudioEngine::GROUP_MAX : g;
    }
}

String MidiRouter::linkGroupsJson() {
    MappingEngine::Verrou verrou;
    String j = "[";
    for (size_t e = 0; e < emplacements.size(); e++) j += String(e ? "," : "") + String((unsigned)emplacements[e]->groupe);
    return j + "]";
}

/* The kind of output that carries a message on (§222). */
static MappingEngine::Sortie::Type sortieDe(MappingEngine::Evenement::Type t) {
    switch (t) {
        case MappingEngine::Evenement::Bend:      return MappingEngine::Sortie::Bend;
        case MappingEngine::Evenement::Touch:     return MappingEngine::Sortie::Touch;
        case MappingEngine::Evenement::PolyTouch: return MappingEngine::Sortie::PolyTouch;
        case MappingEngine::Evenement::Pgm:       return MappingEngine::Sortie::Pgm;
        default:                                  return MappingEngine::Sortie::Cc;
    }
}
static uint8_t canalDe(uint8_t c) { return (c >= 1 && c <= 16) ? c : 1; }

bool MidiRouter::_passerMessage(std::vector<Emplacement*>& liste, size_t de, size_t a, Message& m, CcFait& cc,
                                const char* branche, AudioEngine::Pour pour, uint8_t groupe) {
    const uint16_t genre = MappingEngine::masqueDe(m.type);
    const MappingEngine::Sortie::Type meme = sortieDe(m.type);
    for (size_t e = de; e < a && e < liste.size(); e++) {
        Emplacement& em = *liste[e];
        if (em.groupe != groupe) continue;    // another group's link: its own walk (§221)
        if (!(em.repond & genre)) continue;   // nothing in this link answers this kind (§170)
        Contexte ctx(*this, pour, groupe);    // what it emits goes to its cue and its group
        MappingEngine::Evenement evt;
        evt.type = m.type; evt.canal = m.c; evt.a = m.a; evt.b = m.b; evt.valeur14 = m.v14;
        /* WHO runs — « map:0 » (« out:1 »: the cue that leaves): a print() or a graph() would
         * reach the app under « ? ». */
        snprintf(evt.origine, sizeof evt.origine, "%s:%u", branche, (unsigned)e);
        MappingEngine::Sortie sorties[MappingEngine::MAX_SORTIES];
        bool traite = false;
        const int n = MappingEngine::executer(em.contenu.c_str(), evt, sorties, MappingEngine::MAX_SORTIES,
                                              traite, em.etats, MappingEngine::MAX_PIPELINES_SCRIPT);
        const MappingEngine::Sortie* suite = nullptr;   // the first of its kind carries it on
        const MappingEngine::Sortie* ctl = nullptr;     // the first CC made of it
        for (int i = 0; i < n; i++) {
            if (!suite && sorties[i].type == meme) suite = &sorties[i];
            if (!ctl && sorties[i].type == MappingEngine::Sortie::Cc) ctl = &sorties[i];
        }
        if (suite) { m.c = canalDe(suite->canal); m.a = suite->a; m.b = suite->b; m.v14 = suite->valeur14; continue; }
        if (ctl) { cc.fait = true; cc.c = canalDe(ctl->canal); cc.n = ctl->a; cc.v = ctl->b; return false; }
        if (traite) return false;   // it answers, and gave nothing of it: it swallows it
        // Neither: this link does not speak of this message, on to the next one.
    }
    return true;
}

bool MidiRouter::chaineScriptsCc(uint8_t groupe, uint8_t& c, uint8_t& n, uint8_t& v) {
    MappingEngine::Verrou verrou;   // un maillon ne se remplace pas sous un CC (§170)
    Message m; m.type = MappingEngine::Evenement::Cc; m.c = c; m.a = n; m.b = v;
    CcFait cc;
    const bool passe = _passerMessage(emplacements, 0, _nPermanents, m, cc, "map", AudioEngine::Pour::TOUS, groupe)
                    && _passerMessage(emplacements, _nPermanents, emplacements.size(), m, cc, "map",
                                      AudioEngine::Pour::TOUS, groupe);
    c = m.c; n = m.a; v = m.b;
    return passe;
}

uint32_t MidiRouter::groupesDesMessages() {
    MappingEngine::Verrou verrou;
    uint32_t groupes = AudioEngine::instrumentGroups() | CcMap::groupes();
    for (Emplacement* em : emplacements) if (em->repond) groupes |= 1u << em->groupe;
    for (Emplacement* em : sortants)     if (em->repond) groupes |= 1u << em->groupe;
    return groupes ? groupes : 1u;   // nothing anywhere: one walk all the same (the components)
}

void MidiRouter::_passerOsc(std::vector<Emplacement*>& liste, size_t de, size_t a, const char* adresse,
                            float valeur, const char* branche, AudioEngine::Pour pour) {
    const uint16_t osc = MappingEngine::masqueDe(MappingEngine::Evenement::Osc);
    for (size_t e = de; e < a && e < liste.size(); e++) {
        Emplacement& em = *liste[e];
        if (!(em.repond & osc)) continue;   // rien ici n'ecoute l'OSC (§170)
        Contexte ctx(*this, pour, em.groupe);   // what it emits goes to its group (§221)
        char org[12]; snprintf(org, sizeof org, "%s:%u", branche, (unsigned)e);
        MappingEngine::battreOsc(em.contenu.c_str(), adresse, valeur, this,
                                 em.etats, MappingEngine::MAX_PIPELINES_SCRIPT, org);
    }
}

void MidiRouter::_messageEntrant(const Message& m) {
    noterTrafic();   // le moment d'ecrire en flash attend aussi le MIDI (§170)
    /* EVERY GROUP APART (§222), as a note (§221): MAIN, then a branch per cue — the one that
     * plays and, during its Release, the one that leaves (§212) —, each from the message as it
     * came, through that group's links only. A CC MAIN makes of it, or swallows, ends that
     * group's walk, as before. What comes out is applied after the lock. */
    struct Issue { Message m; CcFait cc; bool passe; uint8_t groupe; AudioEngine::Pour pour; };
    Issue issues[2 * (AudioEngine::GROUP_MAX + 1)];
    uint8_t nIssues = 0;
    {
        MappingEngine::Verrou verrou;   // un maillon ne se remplace pas sous un message (§170)
        const uint32_t groupes = groupesDesMessages();
        const bool deux = !sortants.empty();
        const AudioEngine::Pour ici = deux ? AudioEngine::Pour::COURANTE : AudioEngine::Pour::TOUS;
        for (uint8_t g = 0; g <= AudioEngine::GROUP_MAX; g++) {
            if (!(groupes & (1u << g))) continue;
            Message mg = m; CcFait cc;
            if (!_passerMessage(emplacements, 0, _nPermanents, mg, cc, "map", AudioEngine::Pour::TOUS, g)) {
                if (cc.fait) issues[nIssues++] = Issue{mg, cc, false, g, AudioEngine::Pour::TOUS};
                continue;
            }
            Message m2 = mg; CcFait cc2;
            const bool passe = _passerMessage(emplacements, _nPermanents, emplacements.size(), mg, cc, "map", ici, g);
            if (passe || cc.fait) issues[nIssues++] = Issue{mg, cc, passe, g, ici};
            if (deux) {
                const bool passe2 = _passerMessage(sortants, 0, sortants.size(), m2, cc2, "out",
                                                   AudioEngine::Pour::SORTANTE, g);
                if (passe2 || cc2.fait) issues[nIssues++] = Issue{m2, cc2, passe2, g, AudioEngine::Pour::SORTANTE};
            }
        }
    }
    /* A CC — out of the chain, or made of the message — goes to its group (learnt CCs, kits);
     * the components see it once. A pitch bend, a pressure, a program that came out plays
     * nothing: no instrument of the card reads them (as in the browser). */
    bool composants = true;
    for (uint8_t i = 0; i < nIssues; i++) {
        const Issue& x = issues[i];
        if (x.cc.fait) { appliquerCc(x.cc.c, x.cc.n, x.cc.v, x.pour, x.groupe, composants); composants = false; }
        else if (x.passe && x.m.type == MappingEngine::Evenement::Cc) {
            appliquerCc(x.m.c, x.m.a, x.m.b, x.pour, x.groupe, composants); composants = false;
        }
    }
}

void MidiRouter::ccEntrant(uint8_t channel, uint8_t control, uint8_t value) {
    /* Le SCRIPT d'abord, comme pour les notes : ce qui suit doit voir le CC tel que le .nms
     * l'a decide, pas tel qu'il est arrive. Un ctl.out() qui renumerote un CC renumerote
     * donc aussi ce qu'apprend CcMap — dans SON groupe (§222). */
    Message m; m.type = MappingEngine::Evenement::Cc; m.c = channel; m.a = control; m.b = value;
    _messageEntrant(m);
}

void MidiRouter::polytouchEntrant(uint8_t channel, uint8_t note, uint8_t pressure) {
    /* A pad's pressure (§210): what a script makes of it — s("sys.lp_cutoff") smoothed by a
     * lag, a CC — is its group's. */
    Message m; m.type = MappingEngine::Evenement::PolyTouch; m.c = channel; m.a = note; m.b = pressure;
    _messageEntrant(m);
}

void MidiRouter::bendEntrant(uint8_t channel, int16_t bend) {
    Message m; m.type = MappingEngine::Evenement::Bend; m.c = channel; m.v14 = bend;
    _messageEntrant(m);
}

void MidiRouter::touchEntrant(uint8_t channel, uint8_t pressure) {
    Message m; m.type = MappingEngine::Evenement::Touch; m.c = channel; m.a = pressure;
    _messageEntrant(m);
}

void MidiRouter::pgmEntrant(uint8_t channel, uint8_t program) {
    Message m; m.type = MappingEngine::Evenement::Pgm; m.c = channel; m.a = program;
    _messageEntrant(m);
}

/* What an incoming CC does once group `groupe`'s scripts have said what it becomes: learnt,
 * applied, read by the SFZ kits, shown by the components. */
void MidiRouter::appliquerCc(uint8_t c, uint8_t n, uint8_t v, AudioEngine::Pour pour, uint8_t groupe,
                             bool composants) {
    const bool dela = (pour != AudioEngine::Pour::SORTANTE);   // the cue that plays (or both)
    // 2. Apprentissage. Il precede l'application pour que la cible bouge des
    //    le geste qui l'apprend — sans ca il faut toucher le potentiometre une
    //    seconde fois pour entendre quoi que ce soit, et l'apprentissage a
    //    l'air de n'avoir rien fait. La cue qui joue seulement (§212) ; le groupe
    //    de la cible armee (§222).
    if (dela) CcMap::apprendre(c, n, groupe);
    // 3. Table CC -> parametre (moteur audio ou parametre de script) : la voix Plaits
    //    de la cue `pour` (§212), les affectations du groupe (§222).
    CcMap::appliquer(c, n, v, pour, groupe);
    // 3b. Le lecteur SFZ : les portes locc/hicc de ses regions lisent ce CC (une
    //     pedale de charley au CC4 choisit le son ouvert ou ferme). Un octet
    //     range, rien n'attend, rien ne se perd. Les kits de la cue `pour`, du groupe.
    AudioEngine::controleKits(n, v, pour, groupe);
    // 4. Composants : comportement historique, conserve tel quel — une fois.
    if (dela && composants) g_componentManager.handleMidiControlChange(c, n, v);
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
    /* ARRET = L'HORLOGE S'ARRETE. Sans cette garde, un metro() continuait de
     * battre apres un STOP et le script de la cue emettait tout seul. Le MIDI
     * entrant, lui, reste traite plus bas : c'est du jeu live. */
    if (!_enLecture) return;
    MappingEngine::Verrou verrou;   // voir MappingEngine.h (§170)
    const uint16_t init = MappingEngine::masqueDe(MappingEngine::Evenement::Init);
    const uint16_t tick = MappingEngine::masqueDe(MappingEngine::Evenement::Tick);
    /* Les maillons de la cue qui joue, puis la branche de la cue qui part (§212) : ce
     * qu'un maillon emet va aux instruments de SA cue ; MAIN, a toutes. */
    for (uint8_t branche = 0; branche < 2; branche++) {
        std::vector<Emplacement*>& v = branche ? sortants : emplacements;
        for (uint8_t e = 0; e < v.size(); e++) {
            Emplacement& em = *v[e];
            if (!(em.repond & (init | tick))) continue;   // ni loadbang() ni metro() (§170)
            Contexte ctx(*this, branche ? AudioEngine::Pour::SORTANTE
                               : (e < _nPermanents ? AudioEngine::Pour::TOUS : AudioEngine::Pour::COURANTE),
                         em.groupe);   // and to its group's instruments (§221)
            /* Init d'abord : loadbang() doit partir avant le premier metro(). On le
             * differe jusqu'ici plutot que de l'emettre depuis le gestionnaire HTTP —
             * emettre du MIDI depuis async_tcp, c'est le genre de raccourci qui finit
             * en tache bloquee. */
            /* QUI execute : « map:0 »..« map:3 » (« out:1 » : la cue qui part). Sans
             * cela un print() d'emplacement arrivait a l'app sans origine. */
            char org[12];
            snprintf(org, sizeof org, branche ? "out:%u" : "map:%u", (unsigned)(branche ? _nPermanents + e : e));
            if (em.initEnAttente) {
                em.initEnAttente = false;
                if (em.repond & init)
                    MappingEngine::battre(em.contenu.c_str(), MappingEngine::Evenement::Init,
                                          maintenant, this, em.etats,
                                          MappingEngine::MAX_PIPELINES_SCRIPT, org);
            }
            /* Le battement seulement quand un metro() est du : avant, il ne ferait
             * rien — l'interpreteur le dit lui-meme (prochainUtile, §170). */
            if (!(em.repond & tick)) continue;
            if (em.tickConnu && (int32_t)(maintenant - em.prochainTick) < 0) continue;
            MappingEngine::battre(em.contenu.c_str(), MappingEngine::Evenement::Tick,
                                  maintenant, this, em.etats,
                                  MappingEngine::MAX_PIPELINES_SCRIPT, org, &em.prochainTick);
            em.tickConnu = true;
        }
    }
}

void MidiRouter::recevoirOsc(const char* adresse, float valeur) {
    /* MAIN, the cue that plays, the one in its Release (§212): what a link emits from it goes to
     * its own cue. */
    MappingEngine::Verrou verrou;
    const bool deux = !sortants.empty();
    _passerOsc(emplacements, 0, _nPermanents, adresse, valeur, "map", AudioEngine::Pour::TOUS);
    _passerOsc(emplacements, _nPermanents, emplacements.size(), adresse, valeur, "map",
               deux ? AudioEngine::Pour::COURANTE : AudioEngine::Pour::TOUS);
    if (deux) _passerOsc(sortants, 0, sortants.size(), adresse, valeur, "out", AudioEngine::Pour::SORTANTE);
}

// ── Script nomme : contenu et nom dans la composition (chain.txt, §186) ─────
/* Les anciennes cles NVS de la chaine — « nidmi-midi » : nmap, nperm, script,
 * script1… — ne servent plus qu'a etre effacees, une fois, au demarrage. */
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
    const String fichier = fichierEmplacement(emplacement);
    String copie = script;            // l'allocation AVANT le verrou : il ne tient qu'un echange
    const uint16_t repond = MappingEngine::evenementsDuScript(copie.c_str());   // idem (§170)
    bool memeCode = false, dejaNomme = false;
    {
        /* PERSONNE N'EXECUTE CE MAILLON PENDANT QU'ON LE REMPLACE (§170). */
        MappingEngine::Verrou verrou;
        Emplacement* pem = _assurerEmplacement(emplacement);
        if (!pem) return;             // plafond ou memoire : deja dit sur le port serie
        Emplacement& em = *pem;
        memeCode  = (em.contenu == copie);
        dejaNomme = (em.nom == fichier);

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
        em.contenu = std::move(copie);
        em.repond = repond;
        em.tickConnu = false;             // ses metro() repartent de zero
        em.initEnAttente = true;          // nouveau script : son loadbang() est du
        /* L'etat par pipeline de CET emplacement n'a plus de sens : un counter
         * reprendrait la ou en etait le script precedent — decalage silencieux, et
         * d'autant plus deroutant qu'il ne se voit qu'a la deuxieme note. On ne
         * touche PAS aux autres emplacements, qui n'ont pas change. */
        for (int i = 0; i < MappingEngine::MAX_PIPELINES_SCRIPT; i++) em.etats[i].reinitialiser();
        if (!script.length()) em.nom = "";
    }

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
     * identique. Hors du verrou : le script tourne deja. */
    if (!script.length()) {
        _memoriser(emplacement, String());
        Serial.printf("[MidiRouter] emplacement %u : vide\n", (unsigned)emplacement);
        return;
    }

    if (!memeCode || !dejaNomme) {
        const bool ecrit = ScriptStore::ecrire(ScriptStore::Lieu::Composition, fichier.c_str(), script);
        _nommer(emplacement, ecrit ? fichier : String("(en ligne, NON persiste)"));
        if (ecrit) {
            _memoriser(emplacement, fichier);
        } else {
            /* storage pleine ou absente : le script TOURNE quand meme, mais il ne
             * survivra pas au redemarrage. On le DIT — un comportement qui
             * s'evapore sans message est ce qu'on passe la session a supprimer. */
            Serial.printf("[MidiRouter] emplacement %u : ecriture de '%s' impossible — "
                          "le script tourne mais ne survivra pas au redemarrage\n",
                          (unsigned)emplacement, fichier.c_str());
        }
    }
    Serial.printf("[MidiRouter] emplacement %u : %u o (%s)\n",
                  (unsigned)emplacement, (unsigned)script.length(), nomEmplacement(emplacement).c_str());
}

/* Le nom d'un maillon, pose apres coup (l'ecriture du fichier se fait hors du
 * verrou) : le maillon peut avoir disparu entre-temps, raccourci par la
 * composition — on ne nomme alors rien. */
void MidiRouter::_nommer(uint8_t e, const String& nom) {
    MappingEngine::Verrou verrou;
    if (e < emplacements.size()) emplacements[e]->nom = nom;
}

void MidiRouter::_memoriser(uint8_t e, const String& nom) {
    {
        MappingEngine::Verrou verrou;
        if (e >= emplacements.size() || emplacements[e]->nomMemorise == nom) return;
        emplacements[e]->nomMemorise = nom;
    }
    _ecrireChaine();
}

void MidiRouter::_ecrireChaine() {
    String t = "# la chaine de scripts de cette composition — ecrite par la carte (MESURES §186)\n";
    {
        MappingEngine::Verrou verrou;
        t += "taille=" + String((unsigned)emplacements.size()) + "\n";
        t += "permanents=" + String((unsigned)_nPermanents) + "\n";
        for (uint8_t e = 0; e < emplacements.size(); e++)
            if (emplacements[e]->nomMemorise.length())
                t += String((unsigned)e) + "=" + emplacements[e]->nomMemorise + "\n";
    }
    /* Une chaine appartient a une composition : sur un repertoire vide, la
     * premiere se cree (l'app dimensionne la chaine avant d'envoyer ses cues). */
    String raison;
    if (!Repertoire::assurerOuverte("", raison)) {
        Serial.printf("[MidiRouter] chaine non memorisee : %s\n", raison.c_str());
        return;
    }
    Differe::poserFichierCopie(Repertoire::chemin(Repertoire::CHAINE).c_str(), t.c_str(), t.length());
}

// Point d'entree unique de toute note ENTRANTE. Le script est applique ICI,
// donc identiquement pour l'USB, le clavier de l'app et le reste : c'est ce qui
// garantit qu'un bloc mapping vaut pour toutes les sources, et pas seulement
// pour celles qu'on aurait pense cabler une a une.
bool MidiRouter::_passerNote(std::vector<Emplacement*>& liste, size_t de, size_t a,
                             uint8_t& c, uint8_t& n, uint8_t& v, bool estNoteOff, const char* branche,
                             AudioEngine::Pour pour, uint8_t groupe) {
    Contexte ctx(*this, pour, groupe);
    const uint16_t genre = MappingEngine::masqueDe(estNoteOff ? MappingEngine::Evenement::NoteOff
                                                              : MappingEngine::Evenement::NoteOn);
    for (size_t e = de; e < a && e < liste.size(); e++) {
        Emplacement& em = *liste[e];
        if (em.groupe != groupe) continue;    // another group's link: its own walk (§221)
        if (!(em.repond & genre)) continue;   // rien dans ce maillon ne repond a cette note (§170)
        MappingEngine::SortieNote sortie;
        char org[12]; snprintf(org, sizeof org, "%s:%u", branche, (unsigned)e);
        if (MappingEngine::executeMidiNote(em.contenu.c_str(), n, v, c, estNoteOff, sortie,
                                           em.etats, MappingEngine::MAX_PIPELINES_SCRIPT, org)) {
            n = sortie.note; v = sortie.velo; c = sortie.canal;
        }
        else if (sortie.traite) return false;   // il en parle, et n'a rien emis : il l'avale
    }
    return true;
}

void MidiRouter::_jouerNote(uint8_t c, uint8_t n, uint8_t v, bool estNoteOff, AudioEngine::Pour pour,
                            uint8_t groupe, bool leds) {
    // Les LEDs appairees suivent la note REELLEMENT jouee, pas la note brute.
    if (estNoteOff) {
        if (leds) g_componentManager.handleMidiNoteOff(c, n, v);
        AudioEngine::noteOff(c, n, pour, groupe);
    } else {
        if (leds) g_componentManager.handleMidiNoteOn(c, n, v);
        AudioEngine::noteOn(c, n, v, pour, groupe);
    }
}

void MidiRouter::noteEntrante(uint8_t channel, uint8_t note, uint8_t velocity, bool estNoteOff) {
    uint8_t n = note, v = velocity, c = channel;
    noterTrafic();   // le moment d'ecrire en flash attend aussi le MIDI (§170)
    /* A TRANSPORT NOTE (§215) — a pedal on the next cue — does its gesture and plays nothing:
     * before the scripts, on every input. */
    if (TransportNotes::take(channel, note, velocity, estNoteOff)) return;

    /* LES DEUX CUES (§212) : MAIN d'abord, puis une branche par cue — celle qui joue et,
     * pendant son Release, celle qui part — chacune vers SES instruments. Une seule cue :
     * comme avant, la chaine entiere, et la note a tous.
     * THE GROUPS (§221): that walk once per group — the groups of the links that answer a note
     * and of the instruments that sound —, each from the note as it came, through its own links
     * only, to its own instruments. A group without a link gets the note as it came. */
    struct Coup { uint8_t c, n, v, groupe; AudioEngine::Pour pour; };
    Coup coups[2 * (AudioEngine::GROUP_MAX + 1)];
    uint8_t nCoups = 0;
    {
        MappingEngine::Verrou verrou;   // un maillon ne se remplace pas sous une note (§170)
        const uint16_t genre = MappingEngine::masqueDe(estNoteOff ? MappingEngine::Evenement::NoteOff
                                                                  : MappingEngine::Evenement::NoteOn);
        uint32_t groupes = AudioEngine::instrumentGroups();
        for (Emplacement* em : emplacements) if (em->repond & genre) groupes |= 1u << em->groupe;
        for (Emplacement* em : sortants)     if (em->repond & genre) groupes |= 1u << em->groupe;
        if (!groupes) groupes = 1u;   // nothing anywhere: one walk all the same (the LEDs, the test tone)
        const bool deux = !sortants.empty();
        for (uint8_t g = 0; g <= AudioEngine::GROUP_MAX; g++) {
            if (!(groupes & (1u << g))) continue;
            uint8_t cg = c, ng = n, vg = v;
            if (!_passerNote(emplacements, 0, _nPermanents, cg, ng, vg, estNoteOff, "map",
                             AudioEngine::Pour::TOUS, g)) continue;   // MAIN l'a avalee, pour ce groupe
            uint8_t c2 = cg, n2 = ng, v2 = vg;
            if (_passerNote(emplacements, _nPermanents, emplacements.size(), cg, ng, vg, estNoteOff, "map",
                            deux ? AudioEngine::Pour::COURANTE : AudioEngine::Pour::TOUS, g))
                coups[nCoups++] = Coup{cg, ng, vg, g, deux ? AudioEngine::Pour::COURANTE : AudioEngine::Pour::TOUS};
            if (deux && _passerNote(sortants, 0, sortants.size(), c2, n2, v2, estNoteOff, "out",
                                    AudioEngine::Pour::SORTANTE, g))
                coups[nCoups++] = Coup{c2, n2, v2, g, AudioEngine::Pour::SORTANTE};
        }
    }
    for (uint8_t i = 0; i < nCoups; i++) {
        /* The paired LEDs once per note played, however many groups play it. */
        bool vue = false;
        for (uint8_t k = 0; k < i && !vue; k++) vue = coups[k].c == coups[i].c && coups[k].n == coups[i].n;
        _jouerNote(coups[i].c, coups[i].n, coups[i].v, estNoteOff, coups[i].pour, coups[i].groupe, !vue);
    }
}

void MidiRouter::ouvrirSortie() {
    /* Les maillons vides qui prendront la place de ceux de la cue qui part, ALLOUES AVANT le
     * verrou et l'echange : faute de memoire, pas de branche — la cue qui part se tait par ses
     * scripts, ses instruments jouent encore ce que MAIN leur donne. */
    std::vector<Emplacement*> neufs;
    for (size_t e = _nPermanents; e < emplacements.size(); e++) {
        const size_t bloc = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        Emplacement* em = (bloc >= PLANCHER_BLOC_CONTIGU + sizeof(Emplacement)) ? new (std::nothrow) Emplacement() : nullptr;
        if (!em) {
            for (Emplacement* x : neufs) delete x;
            Serial.println("[MidiRouter] branche de la cue qui part : memoire insuffisante, ses scripts se taisent");
            fermerSortie();
            return;
        }
        neufs.push_back(em);
    }
    std::vector<Emplacement*> anciens;
    {
        MappingEngine::Verrou verrou;
        anciens.swap(sortants);
        for (size_t i = 0; i < neufs.size(); i++) {
            const size_t e = _nPermanents + i;
            neufs[i]->nomMemorise = emplacements[e]->nomMemorise;   // la memoire du RANG, pas de la cue
            sortants.push_back(emplacements[e]);
            emplacements[e] = neufs[i];
        }
        for (Emplacement* x : anciens) MappingEngine::viderDifferes(x->contenu.c_str());   // §101
    }
    for (Emplacement* x : anciens) delete x;
}

String MidiRouter::nomsSortantsJson() {
    MappingEngine::Verrou verrou;
    String j = "[";
    for (size_t i = 0; i < sortants.size(); i++) j += String(i ? "," : "") + "\"" + sortants[i]->nom + "\"";
    return j + "]";
}

void MidiRouter::fermerSortie() {
    std::vector<Emplacement*> anciens;
    {
        MappingEngine::Verrou verrou;
        anciens.swap(sortants);
        for (Emplacement* x : anciens) MappingEngine::viderDifferes(x->contenu.c_str());   // §101
    }
    for (Emplacement* x : anciens) delete x;
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
    /* Leurs noms memorises partent avec eux : chain.txt est reecrit par
     * dimensionnerChaine, hors du verrou. */
    while (emplacements.size() > n) {
        Emplacement* em = emplacements.back();
        /* Les reprises retiennent un pointeur sur le texte : les purger AVANT
         * de liberer, sinon la file rejouerait sur de la memoire rendue (§101). */
        MappingEngine::viderDifferes(em->contenu.c_str());
        delete em;
        emplacements.pop_back();
    }
}

/* Combien de maillons appartiennent a la zone MAIN. Persiste, comme la
 * longueur : sans cela une carte redemarree rechargerait bien ses scripts MAIN
 * mais laisserait la premiere cue les ecraser. */
void MidiRouter::fixerMaillonsPermanents(uint8_t n) {
    if (n > emplacements.size()) n = (uint8_t)emplacements.size();
    if (n == _nPermanents) return;
    _nPermanents = n;
    _ecrireChaine();                                   // au silence (§157)
    Serial.printf("[MidiRouter] %u maillon(s) permanent(s) — les cues n'y touchent pas\n",
                  (unsigned)n);
}

/* La longueur voulue par la COMPOSITION. Rend ce qu'on a vraiment obtenu : si
 * la memoire a manque, c'est la carte qui le dit, pas l'app qui le suppose. */
uint8_t MidiRouter::dimensionnerChaine(uint8_t n) {
    if (n > PLAFOND_SCRIPTS_MAP) n = PLAFOND_SCRIPTS_MAP;
    uint8_t obtenu;
    {
        /* Le vecteur change : personne ne le parcourt pendant ce temps (§170). */
        MappingEngine::Verrou verrou;
        if (n < emplacements.size()) _reduireChaine(n);
        else if (n > 0)              _assurerEmplacement((uint8_t)(n - 1));
        obtenu = (uint8_t)emplacements.size();
    }
    /* Une chaine raccourcie sous la zone MAIN ne peut plus en tenir autant :
       la frontiere suit, sinon elle designerait des maillons disparus. */
    if (_nPermanents > obtenu) fixerMaillonsPermanents(obtenu);
    /* La longueur va dans chain.txt avec les noms : sans elle, une carte
     * redemarree retrouverait ses scripts mais pas sa chaine, et les
     * emplacements au-dela du premier seraient muets sans rien dire. */
    _ecrireChaine();                                    // au silence (§157)
    Serial.printf("[MidiRouter] chaine dimensionnee a %u emplacement(s)%s\n",
                  (unsigned)obtenu, (obtenu < n) ? " — memoire insuffisante" : "");
    return obtenu;
}

/* La cle NVS d'un emplacement : « script » pour le premier — le nom historique,
 * qu'on garde pour ne pas perdre la configuration des cartes existantes — puis
 * « script1 », « script2 »... */

bool MidiRouter::chargerScriptNomme(const char* nom, bool persister, uint8_t emplacement) {
    /* LE FICHIER SE LIT AVANT LE VERROU (§170) : le verrou ne tient qu'un
     * echange de textes, jamais une lecture de la flash. */
    String contenu;
    const bool aucun = (!nom || !*nom);        // "" = plus de script du tout
    if (!aucun && !ScriptStore::lire(ScriptStore::Lieu::Composition, nom, contenu)) {
        Serial.printf("[MidiRouter] script '%s' introuvable dans storage\n", nom);
        return false;
    }
    const unsigned taille = contenu.length();
    const uint16_t repond = MappingEngine::evenementsDuScript(contenu.c_str());   // hors verrou (§170)
    {
        MappingEngine::Verrou verrou;
        Emplacement* pem = _assurerEmplacement(emplacement);
        if (!pem) return false;
        Emplacement& em = *pem;
        MappingEngine::viderDifferes(em.contenu.c_str());   // voir setScriptMidi
        em.contenu = std::move(contenu);
        em.nom     = aucun ? "" : nom;
        em.repond  = repond;
        em.tickConnu = false;
        em.initEnAttente = !aucun;
        for (int i = 0; i < MappingEngine::MAX_PIPELINES_SCRIPT; i++) em.etats[i].reinitialiser();
    }
    if (aucun) {
        if (persister) _memoriser(emplacement, String());      // au silence (§157)
        Serial.printf("[MidiRouter] emplacement %u : aucun script\n", (unsigned)emplacement);
        return true;
    }
    if (persister) _memoriser(emplacement, String(nom));       // au silence (§157)
    Serial.printf("[MidiRouter] emplacement %u : '%s' charge (%u o)%s\n",
                  (unsigned)emplacement, nom, taille, persister ? " et memorise" : "");
    return true;
}

void MidiRouter::rechargerChaine() {
    fermerSortie();                       // la branche de la cue qui part n'a plus de sens (§212)
    {
        MappingEngine::Verrou verrou;
        _reduireChaine(0);                // chaque maillon purge ses reprises (§101)
        _nPermanents = 0;
    }
    restaurerScript();
}

void MidiRouter::restaurerScript() {
    /* LES ANCIENNES CLES DE LA CHAINE, EFFACEES (§186) : elle vivait en NVS
     * jusqu'ici. Une fois, au silence — rien ne les relit. */
    {
        Preferences p;
        if (p.begin(NVS_ESPACE_MIDI, true)) {
            const bool anciennes = p.isKey("nmap");
            const uint8_t nmap = p.getUChar("nmap", 0);
            p.end();
            if (anciennes) {
                for (uint8_t e = 0; e < nmap; e++) Differe::nvsRetirer(NVS_ESPACE_MIDI, cleNvsEmplacement(e).c_str());
                Differe::nvsRetirer(NVS_ESPACE_MIDI, "nperm");
                Differe::nvsRetirer(NVS_ESPACE_MIDI, "nmap");
            }
        }
    }
    const String fichier = Repertoire::chemin(Repertoire::CHAINE);
    std::shared_ptr<char> brut; size_t nb = 0;
    if (!fichier.length() || !Differe::lire(fichier.c_str(), brut, nb)) {
        Serial.println("[MidiRouter] aucune chaine a restaurer");
        return;
    }
    String texte;
    texte.concat(brut.get(), (unsigned)nb);
    /* « taille=N », « permanents=P », puis « e=nom.nms » par maillon memorise.
     * La LONGUEUR d'abord : c'est elle qui dit combien de maillons allouer. */
    uint8_t taille = 0, permanents = 0;
    std::vector<String> noms;
    int d = 0;
    while (d < (int)texte.length()) {
        int e = texte.indexOf('\n', d); if (e < 0) e = texte.length();
        String l = texte.substring(d, e); l.trim();
        d = e + 1;
        const int eq = l.indexOf('=');
        if (!l.length() || l[0] == '#' || eq <= 0) continue;
        const String cle = l.substring(0, eq), val = l.substring(eq + 1);
        if (cle == "taille")          taille = (uint8_t)val.toInt();
        else if (cle == "permanents") permanents = (uint8_t)val.toInt();
        else if (isDigit(cle[0])) {
            const int k = cle.toInt();
            if (k >= 0 && k < PLAFOND_SCRIPTS_MAP) {
                if ((int)noms.size() <= k) noms.resize(k + 1);
                noms[k] = val;
            }
        }
    }
    if (taille > PLAFOND_SCRIPTS_MAP) taille = PLAFOND_SCRIPTS_MAP;
    {
        MappingEngine::Verrou verrou;
        if (taille) _assurerEmplacement((uint8_t)(taille - 1));
        _nPermanents = (permanents <= emplacements.size()) ? permanents : (uint8_t)emplacements.size();
        for (uint8_t e = 0; e < emplacements.size() && e < noms.size(); e++)
            emplacements[e]->nomMemorise = noms[e];
    }
    Serial.printf("[MidiRouter] chaine restauree : %u emplacement(s), dont %u permanent(s)\n",
                  (unsigned)emplacements.size(), (unsigned)_nPermanents);
    for (uint8_t e = 0; e < noms.size() && e < emplacements.size(); e++) {
        if (!noms[e].length()) continue;
        if (!chargerScriptNomme(noms[e].c_str(), false, e)) {
            // Le fichier a disparu. On ne bloque rien : le maillon reste vide
            // plutot qu'a moitie configure, et son nom reste memorise.
            Serial.printf("[MidiRouter] emplacement %u : '%s' absent — laisse vide\n",
                          (unsigned)e, noms[e].c_str());
        }
    }
}
