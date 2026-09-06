#include "ButtonProcessor.h"
#include "ProcessorRegistry.h"
#include "../components/ComponentTypes.h"  // Définitions communes
#include "../components/basic/ButtonDef.h"
#include "../midi/handlers/MidiOutputCoordinator.h"
#include "../mapping/MappingEngine.h"

void ButtonProcessor::process(
    const ComponentConfig& config,
    ComponentState& state,
    MidiSender* midi_sender,
    OSCQueue& osc_queue
) {
    // Lecture digitale avec anti-rebond
    // Déterminer le mode pull configuré
    String pullMode = "pullup"; // Défaut
    if (config.specificConfig.button) {
        pullMode = String(config.specificConfig.button->btnPullMode);
        if (pullMode.length() == 0) {
            pullMode = "pullup"; // Défaut
        }
    }
    
    bool raw_state = digitalRead(config.gpio);
    bool pressed;
    
    if (pullMode == "pullup") {
        pressed = !raw_state; // LOW = pressed avec pullup
    } else if (pullMode == "pulldown") {
        pressed = raw_state;  // HIGH = pressed avec pulldown
    } else {
        // "none" ou autre : suppose pull externe vers VCC (comportement pullup)
        pressed = !raw_state;
    }
    
    uint32_t now = millis();

    // Prime debounce state on first run to avoid a synthetic edge at boot.
    if (state.last_time == 0) {
        state.last_button_state = pressed;
        state.prev_stable_state = pressed;
        state.last_change_time = now;
        state.last_value = pressed ? 127 : 0;
        // SCRIPT arming state (reuse note_on_time):
        // 0 = not armed yet, >0 = armed timestamp.
        state.note_on_time = 0;
        state.last_time = now;
        if (config.name && config.name[0] != '\0') {
            FluxRegistry::update(config.name, pressed ? 1.0f : 0.0f);
        }
        return;
    }
    
    // Debouncing simple et fiable
    static const unsigned long DEBOUNCE_TIME = 50; // 50ms
    
    // Détecter changement d'état
    if (pressed != state.last_button_state) {
        state.last_change_time = now;
        state.last_button_state = pressed;
    }
    
    // Attendre la fin du rebond
    if ((now - state.last_change_time) < DEBOUNCE_TIME) {
        return; // Pas encore stable
    }
    
    // État stable actuel (après debounce)
    // Avec INPUT_PULLUP : pressed = true quand bouton pressé (LOW), false quand relâché (HIGH)
    bool currentStableState = pressed;
    bool prevStableState = state.prev_stable_state;
    
    // Détecter Falling (HIGH → LOW, press) et Rising (LOW → HIGH, release)
    // Falling = transition de released (false) à pressed (true)
    // Rising = transition de pressed (true) à released (false)
    bool falling = currentStableState && !prevStableState;  // false → true = press
    bool rising = !currentStableState && prevStableState;   // true → false = release
    
    // Mettre à jour l'état stable précédent pour la prochaine itération
    state.prev_stable_state = currentStableState;
    
    // In script mode, arm after a short stabilization window regardless of idle polarity.
    // This suppresses startup/floating transients while still working with pullup or pulldown wiring.
    if (config.midiMode == MidiMode::SCRIPT && state.note_on_time == 0) {
        if ((now - state.last_time) > 300) {
            state.note_on_time = now;
            state.last_button_state = pressed;
            state.prev_stable_state = currentStableState;
            state.last_change_time = now;
            if (config.name && config.name[0] != '\0') {
                FluxRegistry::update(config.name, currentStableState ? 1.0f : 0.0f);
            }
            Serial.printf("[ButtonProcessor] GPIO%d script armed (stable=%d)\n", config.gpio, currentStableState ? 1 : 0);
        }
    }

    // Si pas de transition, on s'arrête là
    if (!falling && !rising) {
        return;
    }

    Serial.printf("[ButtonProcessor] GPIO%d edge=%s midiMode=%s script=%s\n",
                 config.gpio,
                 falling ? "press" : "release",
                 (config.midiMode == MidiMode::SCRIPT) ? "SCRIPT" : "RTP",
                 (config.mappingScript[0] != '\0') ? config.mappingScript : "<empty>");
    
    // Fonction helper pour envoyer Note On (utilise le coordinateur si mode RTP)
    uint32_t raw_for_event = falling ? 1 : 0; // RAW digital monitoring : 1=press, 0=release
    auto sendNoteOn = [&]() {
        // [correctif NiDMI] etait code en dur a 127 : le champ velocite de
        // l'interface etait affiche, saisi, serialise... et ignore.
        uint8_t value = config.midiVelocity ? config.midiVelocity : 127;
        if (config.msg_type == MidiMessageType::CONTROL_CHANGE) {
            value = config.midiCcOnOffMin;
        }
        if (config.midiMode != MidiMode::SCRIPT) {
            MidiOutputCoordinator::sendMidiAndOsc(midi_sender, osc_queue, config, value);
        }
        state.last_raw_value_u32 = raw_for_event;
        state.last_midi_value_u8 = value;
        state.last_telemetry_ts = millis();
    };
    
    // Fonction helper pour envoyer Note Off (utilise le coordinateur si mode RTP)
    auto sendNoteOff = [&]() {
        if (config.msg_type == MidiMessageType::PROGRAM_CHANGE ||
            config.msg_type == MidiMessageType::CLOCK ||
            config.msg_type == MidiMessageType::TAP_TEMPO) {
            return;
        }
        uint8_t value = 0;
        if (config.msg_type == MidiMessageType::CONTROL_CHANGE) {
            value = config.midiCcOnOffMax;
        }
        if (config.midiMode != MidiMode::SCRIPT) {
            MidiOutputCoordinator::sendMidiAndOsc(midi_sender, osc_queue, config, value);
        }
        state.last_raw_value_u32 = raw_for_event;
        state.last_midi_value_u8 = value;
        state.last_telemetry_ts = millis();
    };
    
    // In script mode, button behavior must be deterministic and edge-driven,
    // independently of btnMode (pulse/toggle/press_release).
    if (config.midiMode == MidiMode::SCRIPT) {
        // If not armed yet, do not emit MIDI on this edge.
        if (state.note_on_time == 0) {
            state.last_time = now;
            return;
        }

        state.last_value = falling ? 127 : 0;
        state.last_raw_value_u32 = raw_for_event;
        state.last_midi_value_u8 = (uint8_t)state.last_value;
        state.last_telemetry_ts = now;

        if (config.name && config.name[0] != '\0') {
            // Keep script source logical (0/1), not MIDI-scaled (0/127),
            // so arithmetic like *(100) produces expected velocities.
            FluxRegistry::update(config.name, currentStableState ? 1.0f : 0.0f);
        }

        /* Le script decide, pas nous.
         *
         * Ici vivait un echafaudage du moteur d'origine : on cherchait
         * « note.on( », « note.off( », « note.out( », « seq.out( », « ctl.out( »
         * dans le texte du script pour decider s'il fallait l'executer, et on
         * le DECOUPAIT en deux versions (appui / relachement) en retirant des
         * segments. Deux defauts, du meme genre que ceux du §22 :
         *   - la garde ne connaissait que cinq verbes : un script en
         *     « bend.out » ou « noteoff.out » ne s'executait tout simplement
         *     pas, sans rien signaler ;
         *   - le decoupeur recollait les segments avec « : », ce qui detruisait
         *     un script a PLUSIEURS pipelines separes par « ; ».
         * Le moteur web n'a pas besoin de ca : la valeur du bouton est dans le
         * registre, et le script choisit lui-meme quoi faire de chaque front —
         *     r("bouton") : sel(1) : map(60) : note.out(1) ;
         *     r("bouton") : sel(0) : map(60) : noteoff.out(1) ;
         * On execute donc a chaque changement d'etat stable, sans rien lire du
         * texte. */
        if (config.mappingScript[0] != '\0') {
            MappingEngine::executerCapteur(config.mappingScript,
                                           currentStableState ? 1.0f : 0.0f,
                                           midi_sender, &state.scriptEtat);
        }

        state.last_time = now;
        return;
    }

    // Déterminer le mode (défaut: press_release)
    String btnMode = "press_release"; // Défaut
    if (config.specificConfig.button) {
        btnMode = String(config.specificConfig.button->btnMode);
        if (btnMode.length() == 0) {
            btnMode = "press_release";
        }
    }
    
    // Déterminer le timing pour mode pulse (défaut: release)
    String btnPulseTiming = "release"; // Défaut
    if (config.specificConfig.button) {
        btnPulseTiming = String(config.specificConfig.button->btnPulseTiming);
        if (btnPulseTiming.length() == 0) {
            btnPulseTiming = "release";
        }
    }
    
    // Implémenter les 3 modes
    if (falling) {
        // Falling edge (press détecté)
        if (btnMode == "pulse") {
            // Mode pulse: selon le timing configuré
            if (btnPulseTiming == "press") {
                // Au press: envoyer Note On + Note Off immédiatement
                sendNoteOn();
                sendNoteOff();
            } else {
                // Au release (défaut): mémoriser qu'on a été pressé, on enverra au Rising
                state.pulse_pending = true;
            }
        } else if (btnMode == "toggle") {
            // Mode toggle: basculer l'état à chaque Falling edge
            if (!state.toggle_state) {
                // État OFF → ON
                sendNoteOn();
                state.toggle_state = true;
                state.last_value = 127;
            } else {
                // État ON → OFF
                sendNoteOff();
                state.toggle_state = false;
                state.last_value = 0;
            }
        } else {
            // Mode press_release (défaut): Note On au Falling
            sendNoteOn();
            state.last_value = 127;
        }
    } else if (rising) {
        // Rising edge (release détecté)
        if (btnMode == "pulse") {
            // Mode pulse: envoyer Note On + Note Off seulement si on avait été pressé
            if (state.pulse_pending) {
                sendNoteOn();
                sendNoteOff();
                state.pulse_pending = false;
            }
        } else if (btnMode == "press_release") {
            // Mode press_release: Note Off au Rising
            sendNoteOff();
            state.last_value = 0;
        }
        // Pour toggle, on ne fait rien au Rising
    }
    
    // Update FluxRegistry only when the component has a declared name.
    if (config.name && config.name[0] != '\0') {
        FluxRegistry::update(config.name, currentStableState ? 1.0f : 0.0f);
    }
    state.last_time = now;
}
static void processWrapper(
    const ComponentConfig& config,
    ComponentState& state,
    AnalogFilter* filter,  // Non utilisé pour les boutons
    MidiSender* midi_sender,
    OSCQueue& osc_queue
) {
    (void)filter; // Ignorer le paramètre non utilisé
    ButtonProcessor::process(config, state, midi_sender, osc_queue);
}

// Enregistrement automatique au chargement du module
static bool registered = ProcessorRegistry::registerProcessor(
    ComponentType::BUTTON,
    processWrapper
);
