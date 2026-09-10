#include "Joystick3Processor.h"
#include "ProcessorRegistry.h"
#include "../components/ComponentTypes.h"
#include "../components/basic/Joystick3Def.h"
#include "../midi/handlers/MidiOutputCoordinator.h"
#include "../midi/MidiMessageType.h"
#include "../utils/PinMapper.h"
#include "../utils/AxisUtils.h"
#include "../managers/complex/joystick3/Joystick3Handler.h"
#include "../managers/complex/ComplexHandlerRegistry.h"
#include "../mapping/MappingEngine.h"

static const uint8_t MAX_JOYSTICK3_FILTERS = 16;
static AnalogFilter yFilters3[MAX_JOYSTICK3_FILTERS];
static AnalogFilter zFilters3[MAX_JOYSTICK3_FILTERS];
static uint8_t filterGpios3[MAX_JOYSTICK3_FILTERS];
static uint8_t filterCount3 = 0;
static uint8_t lastXNormValues3[MAX_JOYSTICK3_FILTERS];
static uint8_t lastYNormValues3[MAX_JOYSTICK3_FILTERS];
static uint8_t lastZNormValues3[MAX_JOYSTICK3_FILTERS];
static uint8_t joy3SweepLastNote[MAX_JOYSTICK3_FILTERS][3];
static uint32_t joy3SweepNoteOnTime[MAX_JOYSTICK3_FILTERS][3];

static uint8_t axisCharToJoy3Slot(char axis) {
    if (axis == 'x') return 0u;
    if (axis == 'y') return 1u;
    return 2u;
}

static uint8_t findJoystick3IndexByXGpio(uint8_t xGpio) {
    for (uint8_t i = 0; i < filterCount3; i++) {
        if (filterGpios3[i] == xGpio) {
            return i;
        }
    }
    return 255;
}

static uint8_t getOrCreateJoystick3Index(uint8_t xGpio) {
    for (uint8_t i = 0; i < filterCount3; i++) {
        if (filterGpios3[i] == xGpio) {
            return i;
        }
    }
    if (filterCount3 < MAX_JOYSTICK3_FILTERS) {
        uint8_t idx = filterCount3++;
        filterGpios3[idx] = xGpio;
        yFilters3[idx].initialized = false;
        zFilters3[idx].initialized = false;
        lastXNormValues3[idx] = 255;
        lastYNormValues3[idx] = 255;
        lastZNormValues3[idx] = 255;
        joy3SweepLastNote[idx][0] = joy3SweepLastNote[idx][1] = joy3SweepLastNote[idx][2] = 255;
        joy3SweepNoteOnTime[idx][0] = joy3SweepNoteOnTime[idx][1] = joy3SweepNoteOnTime[idx][2] = 0;
        return idx;
    }
    return 0;
}

/** Regroupe les paramètres d'un axe (seuils + config MIDI) pour éviter la triplication X/Y/Z. */
struct Joy3AxisCfg {
    uint16_t min, zeroMin, zeroMax, max;
    bool invert;
    MidiMessageType msgType;
    uint8_t param, channel;
    uint8_t noteSweepMin, noteSweepMax;
    uint16_t autoOffDelay;
    uint8_t noteVelFix;
};

static Joy3AxisCfg getAxisCfg(Components::Joystick3Config* jc, char axis, const ComponentConfig& config) {
    Joy3AxisCfg c;
    if (jc) {
        switch (axis) {
            case 'x':
                c.min = jc->joyXMin; c.zeroMin = jc->joyXZeroMin; c.zeroMax = jc->joyXZeroMax; c.max = jc->joyXMax;
                c.invert = jc->invertX;
                c.msgType = jc->xMsgType; c.param = jc->xMidiParam; c.channel = jc->xMidiChannel;
                c.noteSweepMin = jc->xNoteSweepMin; c.noteSweepMax = jc->xNoteSweepMax;
                c.autoOffDelay = jc->xAutoOffDelay; c.noteVelFix = jc->xNoteVelFix;
                break;
            case 'y':
                c.min = jc->joyYMin; c.zeroMin = jc->joyYZeroMin; c.zeroMax = jc->joyYZeroMax; c.max = jc->joyYMax;
                c.invert = jc->invertY;
                c.msgType = jc->yMsgType; c.param = jc->yMidiParam; c.channel = jc->yMidiChannel;
                c.noteSweepMin = jc->yNoteSweepMin; c.noteSweepMax = jc->yNoteSweepMax;
                c.autoOffDelay = jc->yAutoOffDelay; c.noteVelFix = jc->yNoteVelFix;
                break;
            default:
                c.min = jc->joyZMin; c.zeroMin = jc->joyZZeroMin; c.zeroMax = jc->joyZZeroMax; c.max = jc->joyZMax;
                c.invert = jc->invertZ;
                c.msgType = jc->zMsgType; c.param = jc->zMidiParam; c.channel = jc->zMidiChannel;
                c.noteSweepMin = jc->zNoteSweepMin; c.noteSweepMax = jc->zNoteSweepMax;
                c.autoOffDelay = jc->zAutoOffDelay; c.noteVelFix = jc->zNoteVelFix;
                break;
        }
    } else {
        // Config spécifique absente (ne devrait pas arriver, allouée par ComponentInitializer) :
        // plage ADC complète, pas de dead-zone, valeurs génériques du composant.
        c.min = 0; c.zeroMin = 2047; c.zeroMax = 2048; c.max = 4095;
        c.invert = false;
        c.msgType = config.msg_type; c.param = config.midi_param; c.channel = config.midi_channel;
        c.noteSweepMin = config.rtpNoteMin; c.noteSweepMax = config.rtpNoteMax;
        c.autoOffDelay = config.rtpNoteSweepAutoOffDelay; c.noteVelFix = config.rtpNoteVelFix;
    }
    return c;
}

static void processNoteSweepJoystick3Axis(
    uint8_t joyIdx,
    char axis,
    const ComponentConfig& config,
    Components::Joystick3Config* jc,
    MidiSender* midi_sender,
    int32_t rawAxisValue
) {
    if (!midi_sender) {
        return;
    }
    Joy3AxisCfg ac = getAxisCfg(jc, axis, config);
    if (ac.msgType != MidiMessageType::NOTE_SWEEP) {
        return;
    }
    const uint16_t sweepOffMs = effectiveNoteSweepAutoOffMs(ac.autoOffDelay);
    uint8_t ax = axisCharToJoy3Slot(axis);
    // Auto-off : coupe la note si le délai est écoulé, mais conserve joy3SweepLastNote
    // pour éviter un retrigger si la position correspond toujours à la même note.
    if (sweepOffMs > 0 &&
        joy3SweepLastNote[joyIdx][ax] != 255 &&
        joy3SweepNoteOnTime[joyIdx][ax] > 0) {
        uint32_t elapsed = millis() - joy3SweepNoteOnTime[joyIdx][ax];
        if (elapsed >= sweepOffMs) {
            midi_sender->sendNoteOff(ac.channel, joy3SweepLastNote[joyIdx][ax], 0);
            joy3SweepNoteOnTime[joyIdx][ax] = 0;
        }
    }
    uint8_t newNote = mapNoteSweepFromFullAxisTravel(rawAxisValue, (int32_t)ac.min, (int32_t)ac.max, ac.noteSweepMin, ac.noteSweepMax, ac.invert);
    if (newNote == joy3SweepLastNote[joyIdx][ax]) {
        return;
    }
    // Note différente : couper l'ancienne uniquement si encore active.
    if (joy3SweepLastNote[joyIdx][ax] != 255 && joy3SweepNoteOnTime[joyIdx][ax] > 0) {
        midi_sender->sendNoteOff(ac.channel, joy3SweepLastNote[joyIdx][ax], 0);
    }
    midi_sender->sendNoteOn(ac.channel, newNote, ac.noteVelFix);
    joy3SweepLastNote[joyIdx][ax] = newNote;
    joy3SweepNoteOnTime[joyIdx][ax] = millis();
}

void Joystick3Processor::process(
    const ComponentConfig& config,
    ComponentState& state,
    AnalogFilter& xFilter,
    AnalogFilter& yFilter,
    AnalogFilter& zFilter,
    uint8_t yGpio,
    uint8_t zGpio,
    MidiSender* midi_sender,
    OSCQueue& osc_queue,
    uint8_t* lastXNormPtr,
    uint8_t* lastYNormPtr,
    uint8_t* lastZNormPtr
) {
    if (config.gpio >= 255 || config.gpio > 48 || yGpio >= 255 || yGpio > 48 || zGpio >= 255 || zGpio > 48) {
        return;
    }

    if (!PinMapper::hasAdc(config.gpio) || !PinMapper::hasAdc(yGpio) || !PinMapper::hasAdc(zGpio)) {
        return;
    }

    uint16_t xRaw = analogRead(config.gpio);
    uint16_t yRaw = analogRead(yGpio);
    uint16_t zRaw = analogRead(zGpio);

    Components::Joystick3Config* joyCfg = config.specificConfig.joystick3;
    uint8_t intensity = joyCfg ? joyCfg->filter_intensity : 5;
    if (intensity == 0) intensity = 5;

    xFilter.setAlphaFromIntensity(intensity);
    yFilter.setAlphaFromIntensity(intensity);
    zFilter.setAlphaFromIntensity(intensity);

    uint16_t xFiltered = xFilter.process(xRaw);
    uint16_t yFiltered = yFilter.process(yRaw);
    uint16_t zFiltered = zFilter.process(zRaw);

    Joy3AxisCfg xc = getAxisCfg(joyCfg, 'x', config);
    Joy3AxisCfg yc = getAxisCfg(joyCfg, 'y', config);
    Joy3AxisCfg zc = getAxisCfg(joyCfg, 'z', config);

    int8_t xNorm = mapAxisValue(xFiltered, xc.min, xc.zeroMin, xc.zeroMax, xc.max, xc.invert);
    int8_t yNorm = mapAxisValue(yFiltered, yc.min, yc.zeroMin, yc.zeroMax, yc.max, yc.invert);
    int8_t zNorm = mapAxisValue(zFiltered, zc.min, zc.zeroMin, zc.zeroMax, zc.max, zc.invert);

    // Déclencher le monitoring SVG pour les axes Y (2e pin) et Z (3e pin)
    state.aux_gpio = yGpio;
    state.aux_gpio2 = zGpio;

    auto normToMidiValue = [&](int8_t normalizedValue) -> uint8_t {
        // Même mapping que sendOscForAxis() (format MIDI OSC: data1)
        if (normalizedValue <= 0) {
            return (uint8_t)map((long)normalizedValue, -127, 0, 0, 64);
        }
        return (uint8_t)map((long)normalizedValue, 0, 127, 64, 127);
    };

    int8_t lastXNorm = (lastXNormPtr && *lastXNormPtr != 255) ? (int8_t)(*lastXNormPtr - 127) : 128;
    int8_t lastYNorm = (lastYNormPtr && *lastYNormPtr != 255) ? (int8_t)(*lastYNormPtr - 127) : 128;
    int8_t lastZNorm = (lastZNormPtr && *lastZNormPtr != 255) ? (int8_t)(*lastZNormPtr - 127) : 128;

    uint8_t joyIdx = getOrCreateJoystick3Index(config.gpio);
    bool xCh = (xNorm != lastXNorm);
    bool yCh = (yNorm != lastYNorm);
    bool zCh = (zNorm != lastZNorm);

    auto handleAxis = [&](char axis, const Joy3AxisCfg& ac, int8_t norm, uint16_t filtered, bool changed, uint8_t* lastNormPtr) {
        if (ac.msgType == MidiMessageType::NOTE_SWEEP) {
            processNoteSweepJoystick3Axis(joyIdx, axis, config, joyCfg, midi_sender, static_cast<int32_t>(filtered));
        } else if (changed && lastNormPtr) {
            sendMidiForAxis(midi_sender, config, axis, norm, static_cast<int32_t>(filtered));
        }
    };

    handleAxis('x', xc, xNorm, xFiltered, xCh, lastXNormPtr);
    if (xCh && lastXNormPtr) {
        sendOscForAxis(osc_queue, config, 'x', xNorm, xFiltered);
        *lastXNormPtr = (uint8_t)(xNorm + 127);
        state.last_raw_value_u32 = xFiltered;
        state.last_midi_value_u8 = normToMidiValue(xNorm);
        state.last_telemetry_ts = millis();
    }

    handleAxis('y', yc, yNorm, yFiltered, yCh, lastYNormPtr);
    if (yCh && lastYNormPtr) {
        sendOscForAxis(osc_queue, config, 'y', yNorm, yFiltered);
        *lastYNormPtr = (uint8_t)(yNorm + 127);
        state.last_raw_value_aux_u32 = yFiltered;
        state.last_midi_value_aux_u8 = normToMidiValue(yNorm);
        state.last_telemetry_ts_aux = millis();
    }

    handleAxis('z', zc, zNorm, zFiltered, zCh, lastZNormPtr);
    if (zCh && lastZNormPtr) {
        sendOscForAxis(osc_queue, config, 'z', zNorm, zFiltered);
        *lastZNormPtr = (uint8_t)(zNorm + 127);
        state.last_raw_value_aux2_u32 = zFiltered;
        state.last_midi_value_aux2_u8 = normToMidiValue(zNorm);
        state.last_telemetry_ts_aux2 = millis();
    }

    state.last_value = xFiltered;
    state.last_time = millis();

    /* PLUS DE PUBLICATION D'OFFICE AU BUS. Le registre est un bus PARTAGE :
     * il ne doit contenir que ce qu'on y a mis expressement — « in() : s("x") ».
     * Y publier l'entree de chaque composant remplissait un espace commun de
     * valeurs que personne n'avait demande a partager. Le script lit desormais
     * ses entrees par in(n) / raw.in(n), qui ne passent pas par le registre. */
    // Script mode must run even without a component name.
    if (config.midiMode == MidiMode::SCRIPT && config.mappingScript[0] != '\0') {
        /* LES TROIS AXES, dans l'ordre des inlets : in(0)=X, in(1)=Y, in(2)=Z.
         * Le script ne voyait que X — Y et Z existaient, partaient en MIDI et en
         * OSC par ce processeur, et restaient invisibles au script. Et ce qui
         * lui parvenait etait `last_value`, c'est-a-dire la valeur FILTREE
         * (0..4095), la ou in() promet une echelle MIDI : la lecture native va
         * desormais dans raw.in(n), a sa place. */
        /* NORMALISES 0..1 : xNorm est centre, de -127 a +127. Aucune echelle
         * de protocole avant le script — c'est lui qui mettra a l'echelle. */
        const float valeurs[3] = { ((float)xNorm + 127.0f) / 254.0f,
                                   ((float)yNorm + 127.0f) / 254.0f,
                                   ((float)zNorm + 127.0f) / 254.0f };
        const float bruts[3]   = { (float)xFiltered, (float)yFiltered, (float)zFiltered };
        MappingEngine::executerCapteur(config.mappingScript, valeurs, 3,
                                      midi_sender, state.scriptEtats,
                                      ComponentState::MAX_PIPELINES_BROCHE, bruts);
    }
}

int8_t Joystick3Processor::mapAxisValue(
    uint16_t value,
    uint16_t min,
    uint16_t zeroMin,
    uint16_t zeroMax,
    uint16_t max,
    bool invert
) {
    AxisRangeConfig cfg;
    cfg.min     = static_cast<int32_t>(min);
    cfg.zeroMin = static_cast<int32_t>(zeroMin);
    cfg.zeroMax = static_cast<int32_t>(zeroMax);
    cfg.max     = static_cast<int32_t>(max);
    cfg.invert  = invert;
    return mapAxisValueGeneric(static_cast<int32_t>(value), cfg);
}

void Joystick3Processor::sendMidiForAxis(
    MidiSender* midi_sender,
    const ComponentConfig& config,
    char axis,
    int8_t normalizedValue,
    int32_t rawAxisValue
) {
    if (!midi_sender) return;

    Joy3AxisCfg ac = getAxisCfg(config.specificConfig.joystick3, axis, config);

    // CC : centre joystick (0) → valeur MIDI 64 (pas 63)
    uint8_t midiValue;
    if (normalizedValue <= 0) {
        midiValue = (uint8_t)map(normalizedValue, -127, 0, 0, 64);
    } else {
        midiValue = (uint8_t)map(normalizedValue, 0, 127, 64, 127);
    }

    switch (ac.msgType) {
        case MidiMessageType::CONTROL_CHANGE:
            midi_sender->sendControlChange(ac.channel, ac.param, midiValue);
            break;
        case MidiMessageType::PITCH_BEND: {
            // Pitch Bend 14-bit : centre = 8192 (0 en signé)
            int pbValue;
            if (normalizedValue <= 0) {
                pbValue = (int)((long)normalizedValue * 8192 / 127);
            } else {
                pbValue = (int)((long)normalizedValue * 8191 / 127);
            }
            midi_sender->sendPitchBend(ac.channel, pbValue);
            break;
        }
        case MidiMessageType::AFTERTOUCH:
            midi_sender->sendAftertouch(ac.channel, midiValue);
            break;
        case MidiMessageType::NOTE_SWEEP:
            // Balayage géré dans process() via processNoteSweepJoystick3Axis (chaque échantillon).
            break;
        default:
            midi_sender->sendControlChange(ac.channel, ac.param, midiValue);
            break;
    }
}

void Joystick3Processor::sendOscForAxis(
    OSCQueue& osc_queue,
    const ComponentConfig& config,
    char axis,
    int8_t normalizedValue,
    uint16_t rawValue
) {
    if (!(config.flags & 0x02)) return;

    // Même centre que MIDI : 0 → 64
    uint8_t midiValue;
    if (normalizedValue <= 0) {
        midiValue = (uint8_t)map(normalizedValue, -127, 0, 0, 64);
    } else {
        midiValue = (uint8_t)map(normalizedValue, 0, 127, 64, 127);
    }

    char address[40];
    if (config.osc_address[0] != '\0') {
        snprintf(address, sizeof(address), "%s/%c", config.osc_address, axis);
    } else {
        snprintf(address, sizeof(address), "/joy3/%c", axis);
    }

    if (config.flags & 0x08) {
        osc_queue.enqueueFloat(String(address), (float)rawValue);
    } else if (config.flags & 0x04) {
        osc_queue.enqueueMidi(String(address), midiValue, 0, config.midi_channel);
    } else {
        float fVal = normalizedValue / 127.0f;
        osc_queue.enqueueFloat(String(address), fVal);
    }
}

void Joystick3Processor::silenceNoteSweepForGpio(uint8_t xGpio, const ComponentConfig& config, MidiSender* midi_sender) {
    if (!midi_sender) {
        return;
    }
    uint8_t idx = findJoystick3IndexByXGpio(xGpio);
    if (idx == 255) {
        return;
    }
    Components::Joystick3Config* jc = config.specificConfig.joystick3;
    static const char axisChars[3] = {'x', 'y', 'z'};
    for (uint8_t ax = 0; ax < 3; ax++) {
        if (joy3SweepLastNote[idx][ax] == 255) {
            continue;
        }
        Joy3AxisCfg ac = getAxisCfg(jc, axisChars[ax], config);
        if (ac.msgType != MidiMessageType::NOTE_SWEEP) {
            continue;
        }
        midi_sender->sendNoteOff(ac.channel, joy3SweepLastNote[idx][ax], 0);
        joy3SweepLastNote[idx][ax] = 255;
        joy3SweepNoteOnTime[idx][ax] = 0;
    }
}

// Wrapper pour normaliser la signature avec ProcessorRegistry
static void processWrapper(
    const ComponentConfig& config,
    ComponentState& state,
    AnalogFilter* xFilter,
    MidiSender* midi_sender,
    OSCQueue& osc_queue
) {
    if (xFilter == nullptr) return;

    ComplexHandler* handler = ComplexHandlerRegistry::getHandler("joystick3");
    if (!handler) return;

    Joystick3Handler* joystickHandler = static_cast<Joystick3Handler*>(handler);
    uint8_t yGpio = joystickHandler->getYAxisGpio(config.gpio);
    uint8_t zGpio = joystickHandler->getZAxisGpio(config.gpio);

    if (yGpio == 255 || zGpio == 255) return;

    // Obtenir ou créer un index pour ce joystick
    uint8_t idx = getOrCreateJoystick3Index(config.gpio);
    AnalogFilter* yFilter = &yFilters3[idx];
    AnalogFilter* zFilter = &zFilters3[idx];

    // Traiter le joystick avec les pointeurs vers les dernières valeurs
    Joystick3Processor::process(config, state, *xFilter, *yFilter, *zFilter, yGpio, zGpio, midi_sender, osc_queue,
                               &lastXNormValues3[idx], &lastYNormValues3[idx], &lastZNormValues3[idx]);
}

// Enregistrement automatique au chargement du module
static bool registered = ProcessorRegistry::registerProcessor(
    ComponentType::JOYSTICK3,
    processWrapper
);
