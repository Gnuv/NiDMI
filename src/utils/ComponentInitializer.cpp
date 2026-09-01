#include "ComponentInitializer.h"
#include "../components/ComponentTypes.h"  // Définitions communes
#include "../components/ComponentRegistry.h"  // Pour obtenir les définitions
#include "../components/ComponentDefinition.h"  // Pour FormFieldDef, FieldType, MAX_FORM_FIELDS
#include "../components/basic/ButtonDef.h"
#include "../components/basic/LedDef.h"
#include "../components/basic/PotentiometerDef.h"
#include "../components/basic/VelostatDef.h"
#include "../components/basic/JoystickDef.h"
#include "../components/basic/Joystick3Def.h"
#include "../components/motion/Lis3dhDef.h"
#include "../components/interface/Mpr121Def.h"
#include "../components/signal/NoiseSamplerDef.h"
#include "../midi/MidiMessageType.h"
#include "../utils/PinMapper.h"  // Pour PinMapper::hasTouch()

void ComponentInitializer::initializeConfig(
    ComponentConfig& config,
    uint8_t gpio,
    ComponentType type,
    uint8_t midi_param,
    uint8_t channel,
    MidiMessageType msg_type
) {
    config.gpio = gpio;
    config.type = type;
    config.midi_param = midi_param;
    config.midi_channel = channel;
    config.msg_type = msg_type;
    config.flags = 0x03; // rtp_enabled + osc_enabled par défaut
    strncpy(config.osc_address, "/ctl", sizeof(config.osc_address));
    config.osc_address[sizeof(config.osc_address)-1] = '\0';
    // Initialiser les champs pour NOTE_SWEEP
    config.rtpNoteMin = 48;  // Défaut: C3
    config.rtpNoteMax = 72;  // Défaut: C5
    config.rtpNoteVelFix = 100; // Défaut: vélocité fixe
    config.rtpNoteSweepAutoOffDelay = 1000; // Défaut: 1000 ms (0 = timer désactivé)
    
    // Initialiser la plage MIDI (défaut: 0-127 = plage complète)
    config.midiCcRangeMin = 0;   // Défaut: 0
    config.midiCcRangeMax = 127; // Défaut: 127
    
    // Initialiser les valeurs CC On/Off pour boutons (défaut: 127 ON, 0 OFF)
    config.midiCcOnOffMin = 0;   // Défaut: 0 (OFF)
    config.midiCcOnOffMax = 127; // Défaut: 127 (ON)

    // [correctif NiDMI] Vélocité Note On : le champ existait dans l'UI
    // (ButtonDef.h:92, défaut "100") mais nulle part dans le firmware.
    config.midiVelocity = 100;
    
    // Initialiser les champs génériques à zéro/vide
    config.customField1[0] = '\0';
    config.customField2[0] = '\0';
    config.customInt1 = 0;
    config.customInt2 = 0;


    // Allouer et initialiser la configuration spécifique selon le type
    const ComponentDefinition* def = ComponentRegistry::findByType(type);
    
    switch (type) {
        case ComponentType::BUTTON: {
            Components::ButtonConfig* btnConfig = new Components::ButtonConfig();
            if (def && def->formFields) {
                for (uint8_t i = 0; i < def->formFieldCount && i < MAX_FORM_FIELDS; i++) {
                    const FormFieldDef& field = def->formFields[i];
                    if (field.id && field.defaultValue) {
                        if (strcmp(field.id, "btnMode") == 0) {
                            strncpy(btnConfig->btnMode, field.defaultValue, sizeof(btnConfig->btnMode) - 1);
                            btnConfig->btnMode[sizeof(btnConfig->btnMode) - 1] = '\0';
                        } else if (strcmp(field.id, "btnPulseTiming") == 0) {
                            strncpy(btnConfig->btnPulseTiming, field.defaultValue, sizeof(btnConfig->btnPulseTiming) - 1);
                            btnConfig->btnPulseTiming[sizeof(btnConfig->btnPulseTiming) - 1] = '\0';
                        } else if (strcmp(field.id, "btnPullMode") == 0) {
                            strncpy(btnConfig->btnPullMode, field.defaultValue, sizeof(btnConfig->btnPullMode) - 1);
                            btnConfig->btnPullMode[sizeof(btnConfig->btnPullMode) - 1] = '\0';
                        }
                    }
                }
            }
            config.specificConfig.button = btnConfig;
            break;
        }
        case ComponentType::LED: {
            Components::LedConfig* ledConfig = new Components::LedConfig();
            if (def && def->formFields) {
                for (uint8_t i = 0; i < def->formFieldCount && i < MAX_FORM_FIELDS; i++) {
                    const FormFieldDef& field = def->formFields[i];
                    if (field.id && field.defaultValue && strcmp(field.id, "ledMode") == 0) {
                        strncpy(ledConfig->ledMode, field.defaultValue, sizeof(ledConfig->ledMode) - 1);
                        ledConfig->ledMode[sizeof(ledConfig->ledMode) - 1] = '\0';
                    }
                }
            }
            config.specificConfig.led = ledConfig;
            break;
        }
        case ComponentType::POTENTIOMETER: {
            Components::PotentiometerConfig* potConfig = new Components::PotentiometerConfig();
            if (def && def->formFields) {
                for (uint8_t i = 0; i < def->formFieldCount && i < MAX_FORM_FIELDS; i++) {
                    const FormFieldDef& field = def->formFields[i];
                    if (field.id && field.defaultValue) {
                        if (strcmp(field.id, "filterIntensity") == 0) {
                            potConfig->filter_intensity = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "potMin") == 0) {
                            potConfig->potMin = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "potMax") == 0) {
                            potConfig->potMax = atoi(field.defaultValue);
                        }
                    }
                }
            }
            config.specificConfig.potentiometer = potConfig;
            break;
        }
        case ComponentType::VELOSTAT: {
            Components::VelostatConfig* veloConfig = new Components::VelostatConfig();
            if (def && def->formFields) {
                for (uint8_t i = 0; i < def->formFieldCount && i < MAX_FORM_FIELDS; i++) {
                    const FormFieldDef& field = def->formFields[i];
                    if (field.id && field.defaultValue) {
                        if (strcmp(field.id, "filterIntensity") == 0) {
                            veloConfig->filter_intensity = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "velocityThreshold") == 0) {
                            veloConfig->velocityThreshold = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "aftertouchThreshold") == 0) {
                            veloConfig->aftertouchThreshold = atoi(field.defaultValue);
                        }
                    }
                }
            }
            config.specificConfig.velostat = veloConfig;
            break;
        }
        case ComponentType::JOYSTICK: {
            Components::JoystickConfig* joyConfig = new Components::JoystickConfig();
            if (def && def->formFields) {
                for (uint8_t i = 0; i < def->formFieldCount && i < MAX_FORM_FIELDS; i++) {
                    const FormFieldDef& field = def->formFields[i];
                    if (field.id && field.defaultValue) {
                        if (strcmp(field.id, "filterIntensity") == 0) {
                            joyConfig->filter_intensity = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "xMin") == 0) {
                            joyConfig->joyXMin = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "xZeroMin") == 0) {
                            joyConfig->joyXZeroMin = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "xZeroMax") == 0) {
                            joyConfig->joyXZeroMax = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "xMax") == 0) {
                            joyConfig->joyXMax = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "yMin") == 0) {
                            joyConfig->joyYMin = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "yZeroMin") == 0) {
                            joyConfig->joyYZeroMin = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "yZeroMax") == 0) {
                            joyConfig->joyYZeroMax = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "yMax") == 0) {
                            joyConfig->joyYMax = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "invertX") == 0) {
                            joyConfig->invertX = (atoi(field.defaultValue) != 0);
                        } else if (strcmp(field.id, "invertY") == 0) {
                            joyConfig->invertY = (atoi(field.defaultValue) != 0);
                        }
                    }
                }
            }
            config.specificConfig.joystick = joyConfig;
            break;
        }
        case ComponentType::JOYSTICK3: {
            Components::Joystick3Config* joyConfig = new Components::Joystick3Config();
            if (def && def->formFields) {
                for (uint8_t i = 0; i < def->formFieldCount && i < MAX_FORM_FIELDS; i++) {
                    const FormFieldDef& field = def->formFields[i];
                    if (field.id && field.defaultValue) {
                        if (strcmp(field.id, "filterIntensity") == 0) {
                            joyConfig->filter_intensity = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "xMin") == 0) {
                            joyConfig->joyXMin = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "xZeroMin") == 0) {
                            joyConfig->joyXZeroMin = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "xZeroMax") == 0) {
                            joyConfig->joyXZeroMax = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "xMax") == 0) {
                            joyConfig->joyXMax = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "yMin") == 0) {
                            joyConfig->joyYMin = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "yZeroMin") == 0) {
                            joyConfig->joyYZeroMin = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "yZeroMax") == 0) {
                            joyConfig->joyYZeroMax = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "yMax") == 0) {
                            joyConfig->joyYMax = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "zMin") == 0) {
                            joyConfig->joyZMin = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "zZeroMin") == 0) {
                            joyConfig->joyZZeroMin = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "zZeroMax") == 0) {
                            joyConfig->joyZZeroMax = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "zMax") == 0) {
                            joyConfig->joyZMax = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "invertX") == 0) {
                            joyConfig->invertX = (atoi(field.defaultValue) != 0);
                        } else if (strcmp(field.id, "invertY") == 0) {
                            joyConfig->invertY = (atoi(field.defaultValue) != 0);
                        } else if (strcmp(field.id, "invertZ") == 0) {
                            joyConfig->invertZ = (atoi(field.defaultValue) != 0);
                        }
                    }
                }
            }
            config.specificConfig.joystick3 = joyConfig;
            break;
        }
        case ComponentType::IMU: {
            Components::ImuConfig* imuConfig = new Components::ImuConfig();
            if (def && def->formFields) {
                for (uint8_t i = 0; i < def->formFieldCount && i < MAX_FORM_FIELDS; i++) {
                    const FormFieldDef& field = def->formFields[i];
                    if (field.id && field.defaultValue) {
                        if (strcmp(field.id, "filterIntensity") == 0) {
                            imuConfig->filter_intensity = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "i2cAddress") == 0) {
                            imuConfig->i2c_address = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "csGpio") == 0) {
                            imuConfig->cs_gpio = (uint8_t)atoi(field.defaultValue);
                        } else if (strcmp(field.id, "range") == 0) {
                            imuConfig->range = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "dataRate") == 0) {
                            imuConfig->data_rate = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "xMin") == 0) {
                            imuConfig->xMin = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "xZeroMin") == 0) {
                            imuConfig->xZeroMin = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "xZeroMax") == 0) {
                            imuConfig->xZeroMax = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "xMax") == 0) {
                            imuConfig->xMax = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "yMin") == 0) {
                            imuConfig->yMin = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "yZeroMin") == 0) {
                            imuConfig->yZeroMin = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "yZeroMax") == 0) {
                            imuConfig->yZeroMax = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "yMax") == 0) {
                            imuConfig->yMax = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "zMin") == 0) {
                            imuConfig->zMin = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "zZeroMin") == 0) {
                            imuConfig->zZeroMin = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "zZeroMax") == 0) {
                            imuConfig->zZeroMax = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "zMax") == 0) {
                            imuConfig->zMax = atoi(field.defaultValue);
                        } else if (strcmp(field.id, "invertX") == 0) {
                            imuConfig->invertX = (atoi(field.defaultValue) != 0);
                        } else if (strcmp(field.id, "invertY") == 0) {
                            imuConfig->invertY = (atoi(field.defaultValue) != 0);
                        } else if (strcmp(field.id, "invertZ") == 0) {
                            imuConfig->invertZ = (atoi(field.defaultValue) != 0);
                        }
                    }
                }
            }
            config.specificConfig.imu = imuConfig;
            break;
        }
        case ComponentType::MPR121: {
            Components::Mpr121Config* mpr121Config = new Components::Mpr121Config();
            if (def && def->formFields) {
                for (uint8_t i = 0; i < def->formFieldCount && i < MAX_FORM_FIELDS; i++) {
                    const FormFieldDef& field = def->formFields[i];
                    if (field.id && field.defaultValue) {
                        if (strcmp(field.id, "i2cAddress") == 0) {
                            mpr121Config->i2c_address = (uint8_t)atoi(field.defaultValue);
                        } else if (strcmp(field.id, "baseNote") == 0) {
                            mpr121Config->base_note = (uint8_t)atoi(field.defaultValue);
                        } else if (strcmp(field.id, "touchThreshold") == 0) {
                            mpr121Config->touch_threshold = (uint8_t)atoi(field.defaultValue);
                        } else if (strcmp(field.id, "releaseThreshold") == 0) {
                            mpr121Config->release_threshold = (uint8_t)atoi(field.defaultValue);
                        }
                    }
                }
            }
            mpr121Config->midi_channel = channel;
            mpr121Config->msg_type = msg_type;
            config.specificConfig.mpr121 = mpr121Config;
            break;
        }
        case ComponentType::NOISE_SAMPLER: {
            Components::NoiseSamplerConfig* nsConfig = new Components::NoiseSamplerConfig();
            if (def && def->formFields) {
                for (uint8_t i = 0; i < def->formFieldCount && i < MAX_FORM_FIELDS; i++) {
                    const FormFieldDef& field = def->formFields[i];
                    if (field.id && field.defaultValue) {
                        if (strcmp(field.id, "sampleMode") == 0) {
                            strncpy(nsConfig->sampleMode, field.defaultValue, sizeof(nsConfig->sampleMode) - 1);
                            nsConfig->sampleMode[sizeof(nsConfig->sampleMode) - 1] = '\0';
                        } else if (strcmp(field.id, "rateMs") == 0) {
                            nsConfig->rateMs = (uint16_t)atoi(field.defaultValue);
                        } else if (strcmp(field.id, "inMin") == 0) {
                            nsConfig->inMin = (uint16_t)atoi(field.defaultValue);
                        } else if (strcmp(field.id, "inMax") == 0) {
                            nsConfig->inMax = (uint16_t)atoi(field.defaultValue);
                        } else if (strcmp(field.id, "filterIntensity") == 0) {
                            nsConfig->filter_intensity = (uint8_t)atoi(field.defaultValue);
                        }
                    }
                }
            }
            config.specificConfig.noiseSampler = nsConfig;
            break;
        }
        default:
            // Pas de config spécifique pour ce type
            config.specificConfig.specific = nullptr;
            break;
    }
}

void ComponentInitializer::initializeState(ComponentState& state) {
    state.last_value = 0;
    state.last_time = 0;
    state.debounce_state = 0;
    state.last_note = 255; // Aucune note jouée initialement
    state.note_on_time = 0; // Pas de note jouée initialement
    state.hysteresis.reset(0); // Hystérésis initialisée à 0
    state.toggle_state = false; // État toggle initialisé à false (note off)
    state.prev_stable_state = false; // État stable précédent (released par défaut)
    state.pulse_pending = false; // Pas de pulse en attente

    // Télémétrie monitoring SVG
    state.last_raw_value_u32 = 0;
    state.last_midi_value_u8 = 0;
    state.last_telemetry_ts = 0;

    state.aux_gpio = 255;
    state.last_raw_value_aux_u32 = 0;
    state.last_midi_value_aux_u8 = 0;
    state.last_telemetry_ts_aux = 0;

    state.aux_gpio2 = 255;
    state.last_raw_value_aux2_u32 = 0;
    state.last_midi_value_aux2_u8 = 0;
    state.last_telemetry_ts_aux2 = 0;

    // Initialiser les champs de debouncing simple
    state.last_button_state = false;
    state.last_change_time = 0;
    
    // Pour Velostat : initialiser last_aftertouch
    state.last_aftertouch = 0;
}

void ComponentInitializer::setupGpio(uint8_t gpio, ComponentType type, ComponentConfig* config) {
    // IMPORTANT: Ne PAS appeler pinMode() sur une pin réellement utilisée en TOUCH,
    // car cela désactiverait le touch sensing (touchRead() ne fonctionnerait plus).
    // Ne PAS se baser sur la simple capacité physique (hasTouch()) : une pin
    // touch-CAPABLE (ex. D2/GPIO3 sur S3) peut très bien être configurée comme un
    // BOUTON classique — dans ce cas pinMode(INPUT_PULLUP/PULLDOWN) doit s'appliquer
    // normalement, sinon aucun pull n'est jamais activé et la pin reste flottante.
    bool is_touch_type = (type == ComponentType::TOUCH);

    if (is_touch_type) {
        // Pour les pins touch, ne pas appeler pinMode()
        // touchRead() configure automatiquement la pin pour le touch sensing
        Serial.printf("[ComponentInitializer] GPIO%d: composant TOUCH, pas de pinMode() (touchRead() configure automatiquement)\n", gpio);
        return;
    }

    // Les types ci-dessous (BUTTON/LED puis les capteurs analogiques) sont traités
    // AVANT de consulter ComponentRegistry::findByType(type) : cette fonction fait un
    // scan linéaire et renvoie la PREMIÈRE définition dont .type == type. Or plusieurs
    // définitions non-implémentées (placeholders, ex. RadarDopplerDef, MotionGenericDef)
    // réutilisent délibérément ComponentType::BUTTON en attendant leur propre type — et
    // ComponentDefinition::ComponentDefinition() donne PAR DÉFAUT type=POTENTIOMETER à
    // toute définition qui ne fixe pas explicitement .type, donc N'IMPORTE QUEL
    // placeholder qui l'oublie atterrit aussi sur POTENTIOMETER. Ces placeholders sont
    // enregistrés avant les vrais composants dans ComponentRegistry, donc
    // findByType(BUTTON) / findByType(POTENTIOMETER) renvoyaient LEUR définition
    // (souvent pinType != celui attendu) au lieu de la bonne : le switch plus bas
    // tombait alors dans la mauvaise branche (aucun pull-up/pulldown pour un bouton,
    // aucun test de pin flottante pour un potentiomètre). On connaît le comportement
    // de ces types sans ambiguïté : pas besoin de dépendre de cette recherche pour eux.
    if (type == ComponentType::BUTTON) {
        // Configurer le mode pull selon btnPullMode
        String pullMode = "pullup"; // Défaut
        if (config && config->specificConfig.button && strlen(config->specificConfig.button->btnPullMode) > 0) {
            pullMode = String(config->specificConfig.button->btnPullMode);
        }

        if (pullMode == "pullup") {
            pinMode(gpio, INPUT_PULLUP);
        } else if (pullMode == "pulldown") {
            pinMode(gpio, INPUT_PULLDOWN);
        } else {
            // "none" ou autre : pas de pull interne
            pinMode(gpio, INPUT);
        }
        Serial.printf("[ComponentInitializer] GPIO%d: BUTTON pinMode appliqué, btnPullMode='%s', hasTouch=%s\n",
                     gpio, pullMode.c_str(), PinMapper::hasTouch(gpio) ? "true" : "false");
        return;
    }
    if (type == ComponentType::LED) {
        pinMode(gpio, OUTPUT);
        // État éteint au démarrage. En anode commune (LED entre 3V3 et la pin), c'est
        // l'état HAUT qui éteint : forcer LOW y allumerait toutes les LEDs au boot.
        const bool activeLow = config && config->specificConfig.led &&
                               config->specificConfig.led->activeLow;
        digitalWrite(gpio, activeLow ? HIGH : LOW);
        return;
    }

    // Joysticks (multi-pins) : rien à configurer, l'ADC est attaché à la lecture.
    // Volontairement EXCLUS de la détection "pin dans le vide" ci-dessous : le test
    // pull-up/pull-down y a produit des faux positifs qui muselaient des axes pourtant
    // câblés (les 3 axes du joystick 3 axes devenaient muets). Ne les y réintégrer
    // qu'avec une méthode validée sur la cible.
    if (type == ComponentType::JOYSTICK || type == ComponentType::JOYSTICK3) {
        return;
    }

    // Capteurs analogiques MONO-PIN connus (toujours PIN_ANALOG, cf. leurs *Def.h) :
    // une seule pin, donc pin flottante = composant entier inutilisable. Traités ici
    // plutôt que via findByType() pour la même raison d'ambiguïté que BUTTON/LED.
    bool is_single_pin_analog_sensor =
        type == ComponentType::POTENTIOMETER ||
        type == ComponentType::VELOSTAT ||
        type == ComponentType::NOISE_SAMPLER;
    if (is_single_pin_analog_sensor) {
        bool floating = PinMapper::isPinFloating(gpio);
        if (config) {
            config->pin_disconnected = floating;
        }
        if (floating) {
            Serial.printf("[ComponentInitializer] GPIO%d: pin flottante détectée — envoi MIDI/OSC désactivé\n", gpio);
        }
        return;
    }

    const ComponentDefinition* def = ComponentRegistry::findByType(type);
    if (!def) {
        // Fallback : comportement par défaut si définition non disponible
        pinMode(gpio, INPUT);
        return;
    }

    // Pour les types restants, l'ambiguïté potentielle de findByType() ne casse rien
    // ici : toutes les branches ci-dessous se résument à un simple INPUT.
    PinType pinType = static_cast<PinType>(def->pinType);
    switch (pinType) {
        case PinType::PIN_ANALOG:
            // Ne devrait plus arriver ici (types analogiques connus traités plus haut) ;
            // gardé par sécurité pour un futur type analogique pas encore listé ci-dessus.
            break;
        case PinType::PIN_DIGITAL:
        case PinType::PIN_ANALOG_OR_DIGITAL:
        case PinType::PIN_PWM:
            pinMode(gpio, INPUT);
            break;
    }
}
