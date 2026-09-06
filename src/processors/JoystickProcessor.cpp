#include "JoystickProcessor.h"
#include "ProcessorRegistry.h"
#include "../components/ComponentTypes.h"
#include "../components/basic/JoystickDef.h"
#include "../midi/handlers/MidiOutputCoordinator.h"
#include "../midi/MidiMessageType.h"
#include "../utils/PinMapper.h"
#include "../utils/AxisUtils.h"
#include "../managers/complex/joystick/JoystickHandler.h"
#include "../managers/complex/ComplexHandlerRegistry.h"
#include "../mapping/MappingEngine.h"

static const uint8_t MAX_JOYSTICK_FILTERS = 16;
static AnalogFilter yFilters[MAX_JOYSTICK_FILTERS];
static uint8_t yFilterGpios[MAX_JOYSTICK_FILTERS];
static uint8_t yFilterCount = 0;
static uint8_t lastXNormValues[MAX_JOYSTICK_FILTERS];
static uint8_t lastYNormValues[MAX_JOYSTICK_FILTERS];
static uint8_t joySweepLastNote[MAX_JOYSTICK_FILTERS][2];
static uint32_t joySweepNoteOnTime[MAX_JOYSTICK_FILTERS][2];

static uint8_t axisCharToJoySlot(char axis) {
    return (axis == 'x') ? 0u : 1u;
}

static uint8_t findJoystickIndexByXGpio(uint8_t xGpio) {
    for (uint8_t i = 0; i < yFilterCount; i++) {
        if (yFilterGpios[i] == xGpio) {
            return i;
        }
    }
    return 255;
}

static uint8_t getOrCreateJoystickIndex(uint8_t xGpio) {
    for (uint8_t i = 0; i < yFilterCount; i++) {
        if (yFilterGpios[i] == xGpio) {
            return i;
        }
    }
    if (yFilterCount < MAX_JOYSTICK_FILTERS) {
        uint8_t idx = yFilterCount++;
        yFilterGpios[idx] = xGpio;
        yFilters[idx].initialized = false;
        lastXNormValues[idx] = 255;
        lastYNormValues[idx] = 255;
        joySweepLastNote[idx][0] = joySweepLastNote[idx][1] = 255;
        joySweepNoteOnTime[idx][0] = joySweepNoteOnTime[idx][1] = 0;
        return idx;
    }
    return 0;
}

static void processNoteSweepJoystickAxis(
    uint8_t joyIdx,
    char axis,
    const ComponentConfig& config,
    Components::JoystickConfig* jc,
    MidiSender* midi_sender,
    int32_t rawAxisValue
) {
    if (!midi_sender) {
        return;
    }
    MidiMessageType msgType = config.msg_type;
    uint8_t channel = config.midi_channel;
    uint8_t nmin = config.rtpNoteMin;
    uint8_t nmax = config.rtpNoteMax;
    int32_t axisMin = 0;
    int32_t axisMax = 4095;
    bool inv = false;
    uint16_t axisAutoOffMs = config.rtpNoteSweepAutoOffDelay;
    uint8_t axisVelocity = config.rtpNoteVelFix;
    if (jc) {
        if (axis == 'x') {
            msgType = jc->xMsgType;
            channel = jc->xMidiChannel;
            nmin = jc->xNoteSweepMin;
            nmax = jc->xNoteSweepMax;
            axisMin = static_cast<int32_t>(jc->joyXMin);
            axisMax = static_cast<int32_t>(jc->joyXMax);
            inv = jc->invertX;
            axisAutoOffMs = jc->xAutoOffDelay;
            axisVelocity = jc->xNoteVelFix;
        } else {
            msgType = jc->yMsgType;
            channel = jc->yMidiChannel;
            nmin = jc->yNoteSweepMin;
            nmax = jc->yNoteSweepMax;
            axisMin = static_cast<int32_t>(jc->joyYMin);
            axisMax = static_cast<int32_t>(jc->joyYMax);
            inv = jc->invertY;
            axisAutoOffMs = jc->yAutoOffDelay;
            axisVelocity = jc->yNoteVelFix;
        }
    }
    if (msgType != MidiMessageType::NOTE_SWEEP) {
        return;
    }
    const uint16_t sweepOffMs = effectiveNoteSweepAutoOffMs(axisAutoOffMs);
    uint8_t ax = axisCharToJoySlot(axis);
    // Auto-off : coupe la note si le délai est écoulé, mais conserve joySweepLastNote
    // pour éviter un retrigger si la position correspond toujours à la même note.
    if (sweepOffMs > 0 &&
        joySweepLastNote[joyIdx][ax] != 255 &&
        joySweepNoteOnTime[joyIdx][ax] > 0) {
        uint32_t elapsed = millis() - joySweepNoteOnTime[joyIdx][ax];
        if (elapsed >= sweepOffMs) {
            midi_sender->sendNoteOff(channel, joySweepLastNote[joyIdx][ax], 0);
            joySweepNoteOnTime[joyIdx][ax] = 0;
        }
    }
    uint8_t newNote = mapNoteSweepFromFullAxisTravel(rawAxisValue, axisMin, axisMax, nmin, nmax, inv);
    if (newNote == joySweepLastNote[joyIdx][ax]) {
        return;
    }
    // Note différente : couper l'ancienne uniquement si encore active.
    if (joySweepLastNote[joyIdx][ax] != 255 && joySweepNoteOnTime[joyIdx][ax] > 0) {
        midi_sender->sendNoteOff(channel, joySweepLastNote[joyIdx][ax], 0);
    }
    midi_sender->sendNoteOn(channel, newNote, axisVelocity);
    joySweepLastNote[joyIdx][ax] = newNote;
    joySweepNoteOnTime[joyIdx][ax] = millis();
}

void JoystickProcessor::process(
    const ComponentConfig& config,
    ComponentState& state,
    AnalogFilter& xFilter,
    AnalogFilter& yFilter,
    uint8_t yGpio,
    MidiSender* midi_sender,
    OSCQueue& osc_queue,
    uint8_t* lastXNormPtr,
    uint8_t* lastYNormPtr
) {
    if (config.gpio >= 255 || config.gpio > 48 || yGpio >= 255 || yGpio > 48) {
        return;
    }
    
    if (!PinMapper::hasAdc(config.gpio) || !PinMapper::hasAdc(yGpio)) {
        return;
    }
    
    uint16_t xRaw = analogRead(config.gpio);
    uint16_t yRaw = analogRead(yGpio);
    
    uint8_t intensity = 5;
    uint16_t xMin = 200, xZeroMin = 1900, xZeroMax = 2100, xMax = 4000;
    uint16_t yMin = 200, yZeroMin = 1900, yZeroMax = 2100, yMax = 4000;
    bool invertX = false;
    bool invertY = false;
    
    if (config.specificConfig.joystick) {
        Components::JoystickConfig* joyConfig = config.specificConfig.joystick;
        intensity = joyConfig->filter_intensity;
        if (intensity == 0) intensity = 5;
        xMin = joyConfig->joyXMin;
        xZeroMin = joyConfig->joyXZeroMin;
        xZeroMax = joyConfig->joyXZeroMax;
        xMax = joyConfig->joyXMax;
        yMin = joyConfig->joyYMin;
        yZeroMin = joyConfig->joyYZeroMin;
        yZeroMax = joyConfig->joyYZeroMax;
        yMax = joyConfig->joyYMax;
        invertX = joyConfig->invertX;
        invertY = joyConfig->invertY;
    }
    
    xFilter.setAlphaFromIntensity(intensity);
    yFilter.setAlphaFromIntensity(intensity);
    
    uint16_t xFiltered = xFilter.process(xRaw);
    uint16_t yFiltered = yFilter.process(yRaw);
    
    int8_t xNorm = mapAxisValue(xFiltered, xMin, xZeroMin, xZeroMax, xMax, invertX);
    int8_t yNorm = mapAxisValue(yFiltered, yMin, yZeroMin, yZeroMax, yMax, invertY);

    // Déclencher le monitoring SVG pour l’axe Y (2e pin analogique)
    state.aux_gpio = yGpio;

    auto normToMidiValue = [&](int8_t normalizedValue) -> uint8_t {
        // Même mapping que sendOscForAxis() (format MIDI OSC: data1)
        if (normalizedValue <= 0) {
            return (uint8_t)map((long)normalizedValue, -127, 0, 0, 64);
        }
        return (uint8_t)map((long)normalizedValue, 0, 127, 64, 127);
    };
    
    int8_t lastXNorm = (lastXNormPtr && *lastXNormPtr != 255) ? (int8_t)(*lastXNormPtr - 127) : 128;
    int8_t lastYNorm = (lastYNormPtr && *lastYNormPtr != 255) ? (int8_t)(*lastYNormPtr - 127) : 128;

    uint8_t joyIdx = getOrCreateJoystickIndex(config.gpio);
    bool xCh = (xNorm != lastXNorm);
    bool yCh = (yNorm != lastYNorm);

    Components::JoystickConfig* joyCfg = config.specificConfig.joystick;

    if (joyCfg) {
        if (joyCfg->xMsgType == MidiMessageType::NOTE_SWEEP) {
            processNoteSweepJoystickAxis(joyIdx, 'x', config, joyCfg, midi_sender, static_cast<int32_t>(xFiltered));
        } else if (xCh && lastXNormPtr) {
            sendMidiForAxis(midi_sender, config, 'x', xNorm, static_cast<int32_t>(xFiltered));
        }
    } else if (config.msg_type == MidiMessageType::NOTE_SWEEP) {
        processNoteSweepJoystickAxis(joyIdx, 'x', config, nullptr, midi_sender, static_cast<int32_t>(xFiltered));
    } else if (xCh && lastXNormPtr) {
        sendMidiForAxis(midi_sender, config, 'x', xNorm, static_cast<int32_t>(xFiltered));
    }

    if (xCh && lastXNormPtr) {
        sendOscForAxis(osc_queue, config, 'x', xNorm, xFiltered);
        *lastXNormPtr = (uint8_t)(xNorm + 127);
        state.last_raw_value_u32 = xFiltered;
        state.last_midi_value_u8 = normToMidiValue(xNorm);
        state.last_telemetry_ts = millis();
    }

    if (joyCfg) {
        if (joyCfg->yMsgType == MidiMessageType::NOTE_SWEEP) {
            processNoteSweepJoystickAxis(joyIdx, 'y', config, joyCfg, midi_sender, static_cast<int32_t>(yFiltered));
        } else if (yCh && lastYNormPtr) {
            sendMidiForAxis(midi_sender, config, 'y', yNorm, static_cast<int32_t>(yFiltered));
        }
    } else if (config.msg_type == MidiMessageType::NOTE_SWEEP) {
        processNoteSweepJoystickAxis(joyIdx, 'y', config, nullptr, midi_sender, static_cast<int32_t>(yFiltered));
    } else if (yCh && lastYNormPtr) {
        sendMidiForAxis(midi_sender, config, 'y', yNorm, static_cast<int32_t>(yFiltered));
    }

    if (yCh && lastYNormPtr) {
        sendOscForAxis(osc_queue, config, 'y', yNorm, yFiltered);
        *lastYNormPtr = (uint8_t)(yNorm + 127);
        state.last_raw_value_aux_u32 = yFiltered;
        state.last_midi_value_aux_u8 = normToMidiValue(yNorm);
        state.last_telemetry_ts_aux = millis();
    }
    
    state.last_value = xFiltered;
    state.last_time = millis();
    
    // Update FluxRegistry only when the component has a declared name.
    if (config.name && config.name[0] != '\0') {
        FluxRegistry::update(config.name, (float)state.last_value);
    }
    // Script mode must run even without a component name.
    if (config.midiMode == MidiMode::SCRIPT && config.mappingScript[0] != '\0') {
        MappingEngine::executerCapteur(config.mappingScript, (float)state.last_value,
                                      midi_sender, &state.scriptEtat);
    }
}

int8_t JoystickProcessor::mapAxisValue(
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

void JoystickProcessor::sendMidiForAxis(
    MidiSender* midi_sender,
    const ComponentConfig& config,
    char axis,
    int8_t normalizedValue,
    int32_t rawAxisValue
) {
    if (!midi_sender) return;
    
    MidiMessageType msgType = config.msg_type;
    uint8_t channel = config.midi_channel;
    uint8_t param = config.midi_param;
    
    if (config.specificConfig.joystick) {
        Components::JoystickConfig* joyConfig = config.specificConfig.joystick;
        if (axis == 'x') {
            msgType = joyConfig->xMsgType;
            channel = joyConfig->xMidiChannel;
            param = joyConfig->xMidiParam;
        } else {
            msgType = joyConfig->yMsgType;
            channel = joyConfig->yMidiChannel;
            param = joyConfig->yMidiParam;
        }
    }
    
    // CC : centre joystick (0) → valeur MIDI 64 (pas 63)
    uint8_t midiValue;
    if (normalizedValue <= 0) {
        midiValue = (uint8_t)map(normalizedValue, -127, 0, 0, 64);
    } else {
        midiValue = (uint8_t)map(normalizedValue, 0, 127, 64, 127);
    }
    
    switch (msgType) {
        case MidiMessageType::CONTROL_CHANGE:
            midi_sender->sendControlChange(channel, param, midiValue);
            break;
        case MidiMessageType::PITCH_BEND: {
            // Pitch Bend 14-bit : centre = 8192 (0 en signé)
            int pbValue;
            if (normalizedValue <= 0) {
                pbValue = (int)((long)normalizedValue * 8192 / 127);
            } else {
                pbValue = (int)((long)normalizedValue * 8191 / 127);
            }
            midi_sender->sendPitchBend(channel, pbValue);
            break;
        }
        case MidiMessageType::AFTERTOUCH:
            midi_sender->sendAftertouch(channel, midiValue);
            break;
        case MidiMessageType::NOTE_SWEEP:
            // Balayage géré dans process() via processNoteSweepJoystickAxis (chaque échantillon).
            break;
        default:
            midi_sender->sendControlChange(channel, param, midiValue);
            break;
    }
}

void JoystickProcessor::sendOscForAxis(
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
        snprintf(address, sizeof(address), "/joy/%c", axis);
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

void JoystickProcessor::silenceNoteSweepForGpio(uint8_t xGpio, const ComponentConfig& config, MidiSender* midi_sender) {
    if (!midi_sender) {
        return;
    }
    uint8_t idx = findJoystickIndexByXGpio(xGpio);
    if (idx == 255) {
        return;
    }
    Components::JoystickConfig* jc = config.specificConfig.joystick;
    for (uint8_t ax = 0; ax < 2; ax++) {
        if (joySweepLastNote[idx][ax] == 255) {
            continue;
        }
        MidiMessageType msgType;
        uint8_t channel;
        if (jc) {
            if (ax == 0) {
                msgType = jc->xMsgType;
                channel = jc->xMidiChannel;
            } else {
                msgType = jc->yMsgType;
                channel = jc->yMidiChannel;
            }
        } else {
            msgType = config.msg_type;
            channel = config.midi_channel;
        }
        if (msgType != MidiMessageType::NOTE_SWEEP) {
            continue;
        }
        midi_sender->sendNoteOff(channel, joySweepLastNote[idx][ax], 0);
        joySweepLastNote[idx][ax] = 255;
        joySweepNoteOnTime[idx][ax] = 0;
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
    
    ComplexHandler* handler = ComplexHandlerRegistry::getHandler("joystick");
    if (!handler) return;
    
    JoystickHandler* joystickHandler = static_cast<JoystickHandler*>(handler);
    uint8_t yGpio = joystickHandler->getYAxisGpio(config.gpio);
    
    if (yGpio == 255) return;
    
    // Obtenir ou créer un index pour ce joystick
    uint8_t idx = getOrCreateJoystickIndex(config.gpio);
    AnalogFilter* yFilter = &yFilters[idx];
    
    // Traiter le joystick avec les pointeurs vers les dernières valeurs
    JoystickProcessor::process(config, state, *xFilter, *yFilter, yGpio, midi_sender, osc_queue,
                               &lastXNormValues[idx], &lastYNormValues[idx]);
}

// Enregistrement automatique au chargement du module
static bool registered = ProcessorRegistry::registerProcessor(
    ComponentType::JOYSTICK,
    processWrapper
);
