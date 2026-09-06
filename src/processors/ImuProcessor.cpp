#include "ImuProcessor.h"
#include "ProcessorRegistry.h"
#include "../components/ComponentTypes.h"
#include "../components/motion/Lis3dhDef.h"
#include "../hardware/Lis3dhDriver.h"
#include "../utils/PinMapper.h"
#include "../midi/MidiMessageType.h"
#include "../midi/handlers/MidiOutputCoordinator.h"
#include "../utils/AxisUtils.h"
#include "../mapping/MappingEngine.h"

// Instance statique du driver LIS3DH (singleton par GPIO)
static Lis3dhDriver* lis3dh_driver = nullptr;
static uint8_t lis3dh_gpio = 255;
static uint8_t lis3dh_cs = 255;
static uint8_t lis3dh_bus = 255;
static uint8_t lis3dh_range = 255;
static uint8_t lis3dh_data_rate = 255;
static bool lis3dh_initialized = false;
/** Après échec d'init, ne pas retenter avant cette date (évite spam SPI / Serial à chaque frame). */
static unsigned long lis3dh_init_cooldown_until = 0;
static constexpr unsigned long kLis3dhInitRetryMs = 8000;

static const uint8_t MAX_IMU_FILTERS = 8;
static AnalogFilter yFilters[MAX_IMU_FILTERS];
static AnalogFilter zFilters[MAX_IMU_FILTERS];
static uint8_t imuFilterGpios[MAX_IMU_FILTERS];
static uint8_t imuFilterCount = 0;
static uint8_t lastXNormValues[MAX_IMU_FILTERS];
static uint8_t lastYNormValues[MAX_IMU_FILTERS];
static uint8_t lastZNormValues[MAX_IMU_FILTERS];
static uint8_t imuSweepLastNote[MAX_IMU_FILTERS][3];
static uint32_t imuSweepNoteOnTime[MAX_IMU_FILTERS][3];

static uint8_t axisCharToSlot(char axis) {
    if (axis == 'x') return 0;
    if (axis == 'y') return 1;
    return 2;
}

static uint8_t findImuIndexByGpio(uint8_t gpio) {
    for (uint8_t i = 0; i < imuFilterCount; i++) {
        if (imuFilterGpios[i] == gpio) {
            return i;
        }
    }
    return 255;
}

static uint8_t getOrCreateImuIndex(uint8_t gpio) {
    for (uint8_t i = 0; i < imuFilterCount; i++) {
        if (imuFilterGpios[i] == gpio) {
            return i;
        }
    }
    if (imuFilterCount < MAX_IMU_FILTERS) {
        uint8_t idx = imuFilterCount++;
        imuFilterGpios[idx] = gpio;
        yFilters[idx].initialized = false;
        zFilters[idx].initialized = false;
        lastXNormValues[idx] = 255;
        lastYNormValues[idx] = 255;
        lastZNormValues[idx] = 255;
        for (uint8_t a = 0; a < 3; a++) {
            imuSweepLastNote[idx][a] = 255;
            imuSweepNoteOnTime[idx][a] = 0;
        }
        return idx;
    }
    return 0;
}

static void processNoteSweepImuAxis(
    uint8_t imuIdx,
    char axis,
    const ComponentConfig& config,
    Components::ImuConfig* ic,
    MidiSender* midi_sender,
    int32_t rawFiltered
) {
    if (!midi_sender || !ic) {
        return;
    }
    MidiMessageType msgType;
    uint8_t channel;
    uint8_t nmin;
    uint8_t nmax;
    int32_t axisMin;
    int32_t axisMax;
    bool inv;
    uint16_t axisAutoOffMs;
    uint8_t axisVelocity;
    if (axis == 'x') {
        msgType = ic->xMsgType;
        channel = ic->xMidiChannel;
        nmin = ic->xNoteSweepMin;
        nmax = ic->xNoteSweepMax;
        axisMin = ic->xMin;
        axisMax = ic->xMax;
        inv = ic->invertX;
        axisAutoOffMs = ic->xAutoOffDelay;
        axisVelocity = ic->xNoteVelFix;
    } else if (axis == 'y') {
        msgType = ic->yMsgType;
        channel = ic->yMidiChannel;
        nmin = ic->yNoteSweepMin;
        nmax = ic->yNoteSweepMax;
        axisMin = ic->yMin;
        axisMax = ic->yMax;
        inv = ic->invertY;
        axisAutoOffMs = ic->yAutoOffDelay;
        axisVelocity = ic->yNoteVelFix;
    } else {
        msgType = ic->zMsgType;
        channel = ic->zMidiChannel;
        nmin = ic->zNoteSweepMin;
        nmax = ic->zNoteSweepMax;
        axisMin = ic->zMin;
        axisMax = ic->zMax;
        inv = ic->invertZ;
        axisAutoOffMs = ic->zAutoOffDelay;
        axisVelocity = ic->zNoteVelFix;
    }
    if (msgType != MidiMessageType::NOTE_SWEEP) {
        return;
    }
    const uint16_t sweepOffMs = effectiveNoteSweepAutoOffMs(axisAutoOffMs);
    uint8_t ax = axisCharToSlot(axis);
    // Auto-off : coupe la note si le délai est écoulé, mais conserve imuSweepLastNote
    // pour éviter un retrigger si la position correspond toujours à la même note.
    if (sweepOffMs > 0 &&
        imuSweepLastNote[imuIdx][ax] != 255 &&
        imuSweepNoteOnTime[imuIdx][ax] > 0) {
        uint32_t elapsed = millis() - imuSweepNoteOnTime[imuIdx][ax];
        if (elapsed >= sweepOffMs) {
            midi_sender->sendNoteOff(channel, imuSweepLastNote[imuIdx][ax], 0);
            imuSweepNoteOnTime[imuIdx][ax] = 0;
        }
    }
    uint8_t newNote = mapNoteSweepFromFullAxisTravel(rawFiltered, axisMin, axisMax, nmin, nmax, inv);
    if (newNote == imuSweepLastNote[imuIdx][ax]) {
        return;
    }
    // Note différente : couper l'ancienne uniquement si encore active.
    if (imuSweepLastNote[imuIdx][ax] != 255 && imuSweepNoteOnTime[imuIdx][ax] > 0) {
        midi_sender->sendNoteOff(channel, imuSweepLastNote[imuIdx][ax], 0);
    }
    midi_sender->sendNoteOn(channel, newNote, axisVelocity);
    imuSweepLastNote[imuIdx][ax] = newNote;
    imuSweepNoteOnTime[imuIdx][ax] = millis();
}

void ImuProcessor::process(
    const ComponentConfig& config,
    ComponentState& state,
    AnalogFilter& xFilter,
    AnalogFilter& yFilter,
    AnalogFilter& zFilter,
    MidiSender* midi_sender,
    OSCQueue& osc_queue,
    uint8_t* lastXNormPtr,
    uint8_t* lastYNormPtr,
    uint8_t* lastZNormPtr
) {
    // Initialiser le driver si nécessaire (ou réinitialiser si config a changé)
    uint8_t cur_cs = config.specificConfig.imu ? config.specificConfig.imu->cs_gpio : 255;
    uint8_t cur_bus = config.specificConfig.imu ? config.specificConfig.imu->bus_interface : 255;
    uint8_t cur_range = config.specificConfig.imu ? config.specificConfig.imu->range : 255;
    uint8_t cur_data_rate = config.specificConfig.imu ? config.specificConfig.imu->data_rate : 255;
    bool config_changed = (lis3dh_gpio != config.gpio) || (lis3dh_cs != cur_cs) || (lis3dh_bus != cur_bus)
                       || (lis3dh_range != cur_range) || (lis3dh_data_rate != cur_data_rate);
    if (config_changed) {
        lis3dh_init_cooldown_until = 0;
    }
    // Échec précédent : driver NULL mais config déjà mémorisée → attendre le cooldown avant de retenter
    if (lis3dh_initialized && !lis3dh_driver && !config_changed) {
        if (millis() < lis3dh_init_cooldown_until) {
            return;
        }
        lis3dh_initialized = false;
    }

    if (!lis3dh_initialized || config_changed) {
        if (!config.specificConfig.imu) {
            static unsigned long lastLog = 0;
            if (millis() - lastLog > 5000) {
                Serial.printf("[ImuProcessor] ERREUR: specificConfig.imu est NULL pour GPIO %d\n", config.gpio);
                lastLog = millis();
            }
            return;
        }
        
        Components::ImuConfig* imuConfig = config.specificConfig.imu;
        bool use_spi = (imuConfig->bus_interface == 1);

        if (lis3dh_driver) {
            delete lis3dh_driver;
        }

        if (use_spi) {
            uint8_t sck  = PinMapper::labelToGpio("SCK");
            uint8_t miso = PinMapper::labelToGpio("MISO");
            uint8_t mosi = PinMapper::labelToGpio("MOSI");
            // NVS = GPIO « XIAO C3 » pour le pad Dx ; sur S3 le même pad a un autre numéro (ex. D7 : 20 → 44)
            uint8_t cs   = PinMapper::resolveImuCsGpioFromNvs(imuConfig->cs_gpio);
            lis3dh_driver = new Lis3dhDriver();
            if (!lis3dh_driver->beginSPI(sck, miso, mosi, cs)) {
                delete lis3dh_driver;
                lis3dh_driver = nullptr;
                lis3dh_gpio = config.gpio;
                lis3dh_cs = imuConfig->cs_gpio;
                lis3dh_bus = imuConfig->bus_interface;
                lis3dh_range = imuConfig->range;
                lis3dh_data_rate = imuConfig->data_rate;
                lis3dh_initialized = true;
                lis3dh_init_cooldown_until = millis() + kLis3dhInitRetryMs;
                return;
            }
        } else {
            uint8_t i2c_address = imuConfig->i2c_address == 24 ? Lis3dhDriver::ADDRESS_LOW : Lis3dhDriver::ADDRESS_HIGH;
            lis3dh_driver = new Lis3dhDriver(i2c_address);
            if (!lis3dh_driver->begin()) {
                delete lis3dh_driver;
                lis3dh_driver = nullptr;
                lis3dh_gpio = config.gpio;
                lis3dh_cs = imuConfig->cs_gpio;
                lis3dh_bus = imuConfig->bus_interface;
                lis3dh_range = imuConfig->range;
                lis3dh_data_rate = imuConfig->data_rate;
                lis3dh_initialized = true;
                lis3dh_init_cooldown_until = millis() + kLis3dhInitRetryMs;
                return;
            }
        }

        Lis3dhDriver::Range range = static_cast<Lis3dhDriver::Range>(imuConfig->range);
        lis3dh_driver->setRange(range);

        Lis3dhDriver::DataRate dataRate = static_cast<Lis3dhDriver::DataRate>(
            static_cast<uint8_t>(Lis3dhDriver::DataRate::RATE_1HZ) + (imuConfig->data_rate * 0x10)
        );
        if (imuConfig->data_rate > 6) {
            dataRate = imuConfig->data_rate == 7 ? Lis3dhDriver::DataRate::RATE_1_6KHZ : Lis3dhDriver::DataRate::RATE_5KHZ;
        }
        lis3dh_driver->setDataRate(dataRate);

        lis3dh_gpio = config.gpio;
        lis3dh_cs = imuConfig->cs_gpio;
        lis3dh_bus = imuConfig->bus_interface;
        lis3dh_range = imuConfig->range;
        lis3dh_data_rate = imuConfig->data_rate;
        lis3dh_initialized = true;
        lis3dh_init_cooldown_until = 0;
        Serial.printf("[ImuProcessor] LIS3DH %s initialisé (GPIO %d, CS=%d, range=%d, dataRate=%d, filter=%d)\n",
            use_spi ? "SPI" : "I2C", config.gpio,
            use_spi ? (int)PinMapper::resolveImuCsGpioFromNvs(imuConfig->cs_gpio) : (int)imuConfig->cs_gpio,
            imuConfig->range, imuConfig->data_rate, imuConfig->filter_intensity);
    }
    
    if (!lis3dh_driver) {
        static unsigned long lastLog = 0;
        if (millis() - lastLog > 5000) {
            Serial.printf("[ImuProcessor] ERREUR: lis3dh_driver est NULL pour GPIO %d\n", config.gpio);
            lastLog = millis();
        }
        return;
    }
    
    // Lire les données d'accélération
    Lis3dhDriver::AccelerationData accel;
    if (!lis3dh_driver->readAcceleration(accel)) {
        static unsigned long lastLog = 0;
        if (millis() - lastLog > 5000) {
            Serial.printf("[ImuProcessor] ERREUR: Échec lecture accélération pour GPIO %d\n", config.gpio);
            lastLog = millis();
        }
        return;
    }
    
    // Récupérer la configuration
    if (!config.specificConfig.imu) {
        return;
    }
    
    Components::ImuConfig* imuConfig = config.specificConfig.imu;
    uint8_t intensity = imuConfig->filter_intensity;
    if (intensity == 0) intensity = 5;
    
    // Configurer les filtres
    xFilter.setAlphaFromIntensity(intensity);
    yFilter.setAlphaFromIntensity(intensity);
    zFilter.setAlphaFromIntensity(intensity);
    
    // Filtrer les valeurs (convertir int16_t signé en uint16_t pour le filtre)
    int16_t xFiltered = filterSignedValue(xFilter, accel.x);
    int16_t yFiltered = filterSignedValue(yFilter, accel.y);
    int16_t zFiltered = filterSignedValue(zFilter, accel.z);
    
    // Normaliser les valeurs (avec inversion éventuelle)
    int8_t xNorm = mapAxisValue(xFiltered, imuConfig->xMin, imuConfig->xZeroMin, imuConfig->xZeroMax, imuConfig->xMax, imuConfig->invertX);
    int8_t yNorm = mapAxisValue(yFiltered, imuConfig->yMin, imuConfig->yZeroMin, imuConfig->yZeroMax, imuConfig->yMax, imuConfig->invertY);
    int8_t zNorm = mapAxisValue(zFiltered, imuConfig->zMin, imuConfig->zZeroMin, imuConfig->zZeroMax, imuConfig->zMax, imuConfig->invertZ);
    
    // Récupérer les dernières valeurs normalisées
    int8_t lastXNorm = (lastXNormPtr && *lastXNormPtr != 255) ? (int8_t)(*lastXNormPtr - 127) : 128;
    int8_t lastYNorm = (lastYNormPtr && *lastYNormPtr != 255) ? (int8_t)(*lastYNormPtr - 127) : 128;
    int8_t lastZNorm = (lastZNormPtr && *lastZNormPtr != 255) ? (int8_t)(*lastZNormPtr - 127) : 128;

    auto normToMidiValue = [&](int8_t normalizedValue) -> uint8_t {
        // Même mapping que sendOscForAxis() (OSC MIDI: data1)
        if (normalizedValue <= 0) {
            return (uint8_t)map((long)normalizedValue, -127, 0, 0, 64);
        }
        return (uint8_t)map((long)normalizedValue, 0, 127, 64, 127);
    };
    
    // Log périodique pour debug
    static unsigned long lastDebugLog = 0;
    if (millis() - lastDebugLog > 1000) {
        Serial.printf("[ImuProcessor] GPIO %d: raw X=%d Y=%d Z=%d | filt X=%d Y=%d Z=%d | norm X=%d Y=%d Z=%d | seuils X[%d,%d,%d,%d]\n", 
                     config.gpio, accel.x, accel.y, accel.z, xFiltered, yFiltered, zFiltered, 
                     xNorm, yNorm, zNorm, imuConfig->xMin, imuConfig->xZeroMin, imuConfig->xZeroMax, imuConfig->xMax);
        lastDebugLog = millis();
    }

    uint8_t imuIdx = getOrCreateImuIndex(config.gpio);
    bool xCh = (xNorm != lastXNorm);
    bool yCh = (yNorm != lastYNorm);
    bool zCh = (zNorm != lastZNorm);

    // NOTE_SWEEP : à chaque lecture (auto-off après rtpNoteSweepAutoOffDelay ms, pas seulement au changement de norme).
    if (imuConfig->xMsgType == MidiMessageType::NOTE_SWEEP) {
        processNoteSweepImuAxis(imuIdx, 'x', config, imuConfig, midi_sender, static_cast<int32_t>(xFiltered));
    } else if (xCh && lastXNormPtr) {
        sendMidiForAxis(midi_sender, config, 'x', xNorm, static_cast<int32_t>(xFiltered));
    }
    if (xCh && lastXNormPtr) {
        sendOscForAxis(osc_queue, config, 'x', xNorm, accel.x);
        *lastXNormPtr = (uint8_t)(xNorm + 127);
        state.last_raw_value_u32 = (uint16_t)(xFiltered + 32768);
        state.last_midi_value_u8 = normToMidiValue(xNorm);
        state.last_telemetry_ts = millis();
    }

    if (imuConfig->yMsgType == MidiMessageType::NOTE_SWEEP) {
        processNoteSweepImuAxis(imuIdx, 'y', config, imuConfig, midi_sender, static_cast<int32_t>(yFiltered));
    } else if (yCh && lastYNormPtr) {
        sendMidiForAxis(midi_sender, config, 'y', yNorm, static_cast<int32_t>(yFiltered));
    }
    if (yCh && lastYNormPtr) {
        sendOscForAxis(osc_queue, config, 'y', yNorm, accel.y);
        *lastYNormPtr = (uint8_t)(yNorm + 127);
        state.last_raw_value_u32 = (uint16_t)(yFiltered + 32768);
        state.last_midi_value_u8 = normToMidiValue(yNorm);
        state.last_telemetry_ts = millis();
    }

    if (imuConfig->zMsgType == MidiMessageType::NOTE_SWEEP) {
        processNoteSweepImuAxis(imuIdx, 'z', config, imuConfig, midi_sender, static_cast<int32_t>(zFiltered));
    } else if (zCh && lastZNormPtr) {
        sendMidiForAxis(midi_sender, config, 'z', zNorm, static_cast<int32_t>(zFiltered));
    }
    if (zCh && lastZNormPtr) {
        sendOscForAxis(osc_queue, config, 'z', zNorm, accel.z);
        *lastZNormPtr = (uint8_t)(zNorm + 127);
        state.last_raw_value_u32 = (uint16_t)(zFiltered + 32768);
        state.last_midi_value_u8 = normToMidiValue(zNorm);
        state.last_telemetry_ts = millis();
    }
    
    state.last_value = (uint16_t)(xFiltered + 32768); // Stocker X pour compatibilité
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

int8_t ImuProcessor::mapAxisValue(
    int16_t value,
    int16_t min,
    int16_t zeroMin,
    int16_t zeroMax,
    int16_t max,
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

void ImuProcessor::sendMidiForAxis(
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
    
    if (config.specificConfig.imu) {
        Components::ImuConfig* imuConfig = config.specificConfig.imu;
        if (axis == 'x') {
            msgType = imuConfig->xMsgType;
            channel = imuConfig->xMidiChannel;
            param = imuConfig->xMidiParam;
        } else if (axis == 'y') {
            msgType = imuConfig->yMsgType;
            channel = imuConfig->yMidiChannel;
            param = imuConfig->yMidiParam;
        } else if (axis == 'z') {
            msgType = imuConfig->zMsgType;
            channel = imuConfig->zMidiChannel;
            param = imuConfig->zMidiParam;
        }
    }
    
    // CC : centre (0) → valeur MIDI 64 (pas 63)
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
            // Balayage géré dans process() via processNoteSweepImuAxis (chaque échantillon).
            break;
        default:
            midi_sender->sendControlChange(channel, param, midiValue);
            break;
    }
}

void ImuProcessor::sendOscForAxis(
    OSCQueue& osc_queue,
    const ComponentConfig& config,
    char axis,
    int8_t normalizedValue,
    int16_t rawValue
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
        snprintf(address, sizeof(address), "/imu/%c", axis);
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

int16_t ImuProcessor::filterSignedValue(AnalogFilter& filter, int16_t value) {
    // Convertir int16_t (-32768..32767) en uint16_t (0..65535) avec offset
    uint16_t unsignedValue = (uint16_t)(value + 32768);
    
    // Filtrer
    uint16_t filteredUnsigned = filter.process(unsignedValue);
    
    // Reconvertir en int16_t
    return (int16_t)(filteredUnsigned - 32768);
}

void ImuProcessor::silenceNoteSweepForGpio(uint8_t gpio, const ComponentConfig& config, MidiSender* midi_sender) {
    if (!midi_sender || !config.specificConfig.imu) {
        return;
    }
    uint8_t idx = findImuIndexByGpio(gpio);
    if (idx == 255) {
        return;
    }
    Components::ImuConfig* ic = config.specificConfig.imu;
    for (uint8_t ax = 0; ax < 3; ax++) {
        if (imuSweepLastNote[idx][ax] == 255) {
            continue;
        }
        MidiMessageType msgType;
        uint8_t channel;
        if (ax == 0) {
            msgType = ic->xMsgType;
            channel = ic->xMidiChannel;
        } else if (ax == 1) {
            msgType = ic->yMsgType;
            channel = ic->yMidiChannel;
        } else {
            msgType = ic->zMsgType;
            channel = ic->zMidiChannel;
        }
        if (msgType != MidiMessageType::NOTE_SWEEP) {
            continue;
        }
        midi_sender->sendNoteOff(channel, imuSweepLastNote[idx][ax], 0);
        imuSweepLastNote[idx][ax] = 255;
        imuSweepNoteOnTime[idx][ax] = 0;
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
    if (xFilter == nullptr) {
        static unsigned long lastLog = 0;
        if (millis() - lastLog > 5000) {
            Serial.printf("[ImuProcessor] ERREUR: xFilter est NULL pour GPIO %d\n", config.gpio);
            lastLog = millis();
        }
        return;
    }
    
    // Obtenir ou créer un index pour ce IMU
    uint8_t idx = getOrCreateImuIndex(config.gpio);
    AnalogFilter* yFilter = &yFilters[idx];
    AnalogFilter* zFilter = &zFilters[idx];
    
    // Traiter l'IMU avec les pointeurs vers les dernières valeurs
    ImuProcessor::process(config, state, *xFilter, *yFilter, *zFilter, midi_sender, osc_queue,
                         &lastXNormValues[idx], &lastYNormValues[idx], &lastZNormValues[idx]);
}

// Enregistrement automatique au chargement du module
static bool registered = ProcessorRegistry::registerProcessor(
    ComponentType::IMU,
    processWrapper
);
