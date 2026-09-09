#include "ConfigLoader.h"
#include "../managers/ComponentManager.h"
#include "../components/ComponentTypes.h"  // Définitions communes
#include "../components/ComponentRegistry.h"  // Pour trouver les définitions de composants
#include "../components/basic/ButtonDef.h"
#include "../components/basic/LedDef.h"
#include "../components/basic/PotentiometerDef.h"
#include "../components/basic/VelostatDef.h"
#include "../components/basic/JoystickDef.h"
#include "../components/basic/Joystick3Def.h"
#include "../components/motion/Lis3dhDef.h"
#include "../components/interface/Mpr121Def.h"
#include "../components/signal/NoiseSamplerDef.h"
#include "../utils/JSONParser.h"
#include "../utils/PinMapper.h"
#include "../utils/ComponentInitializer.h"  // Pour setupGpio
#include "../managers/complex/ComplexHandler.h"
#include "../managers/complex/ComplexHandlerRegistry.h"
#include "../managers/complex/joystick/JoystickHandler.h"
#include "../managers/complex/joystick3/Joystick3Handler.h"
#include "../midi/MidiMessageType.h"
#include <Preferences.h>

/** Balayage : UI envoie midiNoteSweepMin/Max ; par axe X_midiNoteSweepMin, etc. */
static void loadNoteSweepRangeFromJson(const String& pinConfig, const char* axisPrefix,
                                       uint8_t& outMin, uint8_t& outMax) {
    int mn = -999;
    int mx = -999;
    if (axisPrefix && axisPrefix[0]) {
        char kmin[40], kmax[40];
        snprintf(kmin, sizeof(kmin), "%s_midiNoteSweepMin", axisPrefix);
        snprintf(kmax, sizeof(kmax), "%s_midiNoteSweepMax", axisPrefix);
        mn = JSONParser::extractInt(pinConfig, kmin, -999);
        mx = JSONParser::extractInt(pinConfig, kmax, -999);
    }
    if (mn < 0 || mn > 127) {
        mn = JSONParser::extractInt(pinConfig, "midiNoteSweepMin",
            JSONParser::extractInt(pinConfig, "midiNoteMin",
            JSONParser::extractInt(pinConfig, "rtpNoteMin", 48)));
    }
    if (mx < 0 || mx > 127) {
        mx = JSONParser::extractInt(pinConfig, "midiNoteSweepMax",
            JSONParser::extractInt(pinConfig, "midiNoteMax",
            JSONParser::extractInt(pinConfig, "rtpNoteMax", 72)));
    }
    outMin = (uint8_t)constrain(mn, 0, 127);
    outMax = (uint8_t)constrain(mx, 0, 127);
    if (outMin > outMax) {
        uint8_t t = outMin;
        outMin = outMax;
        outMax = t;
    }
}

void ConfigLoader::loadFromNVS(ComponentManager& manager) {
    Preferences preferences;
    preferences.begin("nidmi", true);
    const bool oscOutAll = preferences.getBool("osc_out_all", true);
    
    // Serial.println("[ConfigLoader] Loading configs from NVS...");
    
    // Charger les configurations depuis NVS
    // Les clés sont sauvegardées comme "pin_A0", "pin_D2", etc.
    // Générer les labels dynamiquement pour éviter le dépassement de pile
    // au lieu d'un tableau String[200] sur la pile (~6400 bytes)
    
    for (int i = 0; i < 200; i++) { // Max 200 pins possibles (A0-A99 + D0-D99)
        // Générer le label dynamiquement : A0-A99, puis D0-D99
        // Réduire la portée des String pour éviter le dépassement de pile
        const char* pinLabelCStr;
        char pinLabelBuf[8]; // Suffisant pour "A99" ou "D99"
        if (i < 100) {
            snprintf(pinLabelBuf, sizeof(pinLabelBuf), "A%d", i);
        } else {
            snprintf(pinLabelBuf, sizeof(pinLabelBuf), "D%d", i - 100);
        }
        pinLabelCStr = pinLabelBuf;
        
        // Construire la clé sans String intermédiaire
        char keyBuf[16]; // "pin_A99" ou "pin_D99"
        snprintf(keyBuf, sizeof(keyBuf), "pin_%s", pinLabelCStr);
        
        manager.marquer("nvs-scan", i);
        if (!preferences.isKey(keyBuf)) {
            continue; // Passer au suivant
        }
        manager.marquer("nvs-pin", i);
        
        manager.marquer("nvs-getString", i);
        // Lire la config - limiter strictement la portée de la String
        String pinConfig = preferences.getString(keyBuf, "");
        if (pinConfig.length() == 0) {
            continue;
        }
        
        manager.marquer("nvs-role", i);
        // Extraire role - garder en vie jusqu'à l'utilisation
        String role = JSONParser::extractStr(pinConfig, "role", "\n");
        if (role.length() == 0) continue;
        
        // Trouver la définition du composant via ComponentRegistry
        const ComponentDefinition* def = ComponentRegistry::findById(role.c_str());
        if (!def) {
            Serial.printf("[ConfigLoader] WARNING: Component '%s' not found in registry for pin %s\n", 
                          role.c_str(), pinLabelCStr);
            continue;
        }
        
        // Utiliser PinMapper pour obtenir le GPIO (accepte const char*)
        uint8_t gpio = PinMapper::labelToGpio(pinLabelCStr);
        if (gpio == 255) {
            Serial.printf("[ConfigLoader] Invalid pin label: %s (GPIO=255)\n", pinLabelCStr);
            continue;
        }
        
        Serial.printf("[ConfigLoader] NVS -> pin=%s GPIO%d role=%s type=%d\n",
                      pinLabelCStr, gpio, role.c_str(), (int)def->type);
        
        // Vérifier que la pin a les capacités requises selon pinType
        if (def->pinType == PinType::PIN_ANALOG) {
            if (!PinMapper::hasAdc(gpio)) {
                Serial.printf("[ConfigLoader] WARNING: Pin %s (GPIO%d) n'a pas d'ADC, ignorée\n", 
                              pinLabelCStr, gpio);
                continue;
            }
        }
        
        manager.marquer("nvs-midi", i);
        // Extraire paramètres MIDI
        uint8_t midi_param = 7; // défaut CC
        uint8_t channel = 1;    // défaut canal 1
        MidiMessageType msg_type = MidiMessageType::NOTE; // défaut
        
        // Lire midiMessageType (ou rtpType pour compatibilité) - garder en vie jusqu'à l'utilisation
        String rtpTypeStr = JSONParser::extractStr(pinConfig, "midiMessageType", "");
        if (rtpTypeStr.length() == 0) {
            rtpTypeStr = JSONParser::extractStr(pinConfig, "rtpType", ""); // Compatibilité ancien format
        }
        
        // Chercher le MidiMessageDef correspondant
        const MidiMessageDef* msgDef = nullptr;
        if (rtpTypeStr.length() > 0) {
            // Chercher par displayName d'abord, puis par id
            for (uint8_t i = 0; i < def->midiMessageCount && i < MAX_MIDI_MESSAGES; i++) {
                if (def->midiMessages[i].displayName && strcmp(def->midiMessages[i].displayName, rtpTypeStr.c_str()) == 0) {
                    msgDef = &def->midiMessages[i];
                    break;
                }
            }
            // Si pas trouvé par displayName, chercher par id
            if (!msgDef) {
                for (uint8_t i = 0; i < def->midiMessageCount && i < MAX_MIDI_MESSAGES; i++) {
                    if (def->midiMessages[i].id && strcmp(def->midiMessages[i].id, rtpTypeStr.c_str()) == 0) {
                        msgDef = &def->midiMessages[i];
                        break;
                    }
                }
            }
            // Convertir en MidiMessageType pour msg_type
            if (msgDef && msgDef->displayName) {
                msg_type = stringToMidiMessageType(msgDef->displayName);
            } else {
                msg_type = stringToMidiMessageType(rtpTypeStr);
            }
        } else {
            // Défaut : utiliser le premier message MIDI du composant s'il existe
            if (def->midiMessageCount > 0) {
                msgDef = &def->midiMessages[0];
                if (msgDef->id) {
                    msg_type = stringToMidiMessageType(msgDef->id);
                }
            }
        }
        
        // Extraire les paramètres MIDI selon les paramètres définis dans MidiMessageDef
        if (msgDef) {
            // Lire midiChannel (ou rtpChan pour compatibilité)
            channel = JSONParser::extractInt(pinConfig, "midiChannel", 1);
            if (channel == 1) {
                channel = JSONParser::extractInt(pinConfig, "rtpChan", 1); // Compatibilité ancien format
            }
            
            // Parcourir les paramètres du message pour extraire la valeur appropriée
            for (uint8_t i = 0; i < msgDef->paramCount && i < MAX_MIDI_PARAMS; i++) {
                const MidiParamDef& param = msgDef->params[i];
                if (param.id) {
                    // Extraire selon le type de paramètre (nouveau format puis ancien pour compatibilité)
                    if (strcmp(param.id, "midiNote") == 0) {
                        midi_param = JSONParser::extractInt(pinConfig, "midiNote", param.defaultValue ? atoi(param.defaultValue) : 60);
                        if (midi_param == (param.defaultValue ? atoi(param.defaultValue) : 60)) {
                            midi_param = JSONParser::extractInt(pinConfig, "rtpNote", midi_param); // Compatibilité
                        }
                    } else if (strcmp(param.id, "midiCc") == 0) {
                        midi_param = JSONParser::extractInt(pinConfig, "midiCc", param.defaultValue ? atoi(param.defaultValue) : 7);
                        if (midi_param == (param.defaultValue ? atoi(param.defaultValue) : 7)) {
                            midi_param = JSONParser::extractInt(pinConfig, "rtpCc", midi_param); // Compatibilité
                        }
                    } else if (strcmp(param.id, "midiPc") == 0) {
                        // Lire midiPc avec défaut 1 (1-based, comme le formulaire)
                        midi_param = JSONParser::extractInt(pinConfig, "midiPc", 1);
                        if (midi_param == 1) {
                            // Compatibilité : essayer ancien format (0-based)
                            int old_value = JSONParser::extractInt(pinConfig, "rtpPc", -1);
                            if (old_value >= 0 && old_value <= 127) {
                                // Ancienne valeur 0-based trouvée : convertir en 1-based pour cohérence
                                midi_param = old_value + 1;
                            }
                        } else if (midi_param == 0) {
                            // Valeur 0 trouvée : probablement ancienne valeur 0-based, convertir en 1-based
                            // Chercher rtpPc pour confirmation
                            int old_value = JSONParser::extractInt(pinConfig, "rtpPc", -1);
                            if (old_value >= 0 && old_value <= 127) {
                                midi_param = old_value + 1; // Convertir 0-based → 1-based
                            } else {
                                midi_param = 1; // Par défaut : programme 1
                            }
                        }
                        // S'assurer que le programme est dans la plage valide (1-128)
                        if (midi_param < 1) midi_param = 1;
                        if (midi_param > 128) midi_param = 128;
                    } else if (strcmp(param.id, "midiCcRange") == 0 && param.type == FieldType::RANGE) {
                        // Plage MIDI : lire midiCcRangeMin et midiCcRangeMin
                        // Les valeurs seront stockées après l'ajout du composant
                    }
                    // Les autres paramètres (midiChannel, midiVelocity, etc.) sont lus ailleurs
                }
            }
        } else {
            // Fallback si pas de définition de message
            channel = JSONParser::extractInt(pinConfig, "midiChannel", 1);
            if (channel == 1) {
                channel = JSONParser::extractInt(pinConfig, "rtpChan", 1); // Compatibilité
            }
            if (msg_type == MidiMessageType::CONTROL_CHANGE) {
                midi_param = JSONParser::extractInt(pinConfig, "midiCc", 7);
                if (midi_param == 7) {
                    midi_param = JSONParser::extractInt(pinConfig, "rtpCc", 7); // Compatibilité
                }
            } else if (msg_type == MidiMessageType::PROGRAM_CHANGE) {
                // Lire midiPc avec défaut 1 (1-based, comme le formulaire)
                midi_param = JSONParser::extractInt(pinConfig, "midiPc", 1);
                if (midi_param == 1) {
                    // Compatibilité : essayer ancien format (0-based)
                    int old_value = JSONParser::extractInt(pinConfig, "rtpPc", -1);
                    if (old_value >= 0 && old_value <= 127) {
                        // Ancienne valeur 0-based trouvée : convertir en 1-based pour cohérence
                        midi_param = old_value + 1;
                    }
                }
                // S'assurer que le programme est dans la plage valide (1-128)
                if (midi_param < 1) midi_param = 1;
                if (midi_param > 128) midi_param = 128;
            } else if (msg_type == MidiMessageType::NOTE || msg_type == MidiMessageType::NOTE_VELOCITY || msg_type == MidiMessageType::NOTE_SWEEP) {
                midi_param = JSONParser::extractInt(pinConfig, "midiNote", 60);
                if (midi_param == 60) {
                    midi_param = JSONParser::extractInt(pinConfig, "rtpNote", 60); // Compatibilité
                }
            }
        }
        
        // Ajouter le composant en utilisant le type depuis la définition
        ComponentType type = def->type;
        
        // DEBUG: Afficher les valeurs extraites
        Serial.printf("[ConfigLoader] Pin %s: type=%d, midi_param=%d, channel=%d, msg_type=%d\n", 
                     pinLabelCStr, (int)type, midi_param, channel, (int)msg_type);
        
        manager.marquer("nvs-addComponent", i);
        bool success = manager.addComponent(gpio, type, midi_param, channel, msg_type);
        
        if (!success) {
            // Échec silencieux pour éviter le spam (les erreurs sont déjà loggées dans addComponent)
            continue;
        }
        
        /* COMPOSANTS A BROCHES SUPPLEMENTAIRES : restauration GENERIQUE.
         *
         * Ce passage etait ecrit A LA MAIN, type par type — un bloc pour le
         * joystick, un pour le joystick 3 axes. Tout nouveau composant a
         * broches multiples n'etait donc PAS restaure au demarrage : constate
         * avec le DAC, dont la declaration disparaissait au redemarrage (la
         * memoire le contenait, /api/pins/caps ne le voyait plus, et le son
         * restait coupe). Un cas particulier de plus par composant, c'est
         * exactement ce qu'il fallait cesser.
         *
         * On reconstruit donc les donnees depuis la configuration rangee et on
         * les rend au handler du role, quel qu'il soit. Les handlers sont
         * idempotents : reappeler addComponent met a jour l'existant. */
        if (def->additionalPinCount > 0 && def->additionalPins) {
            ComplexHandler* handler = ComplexHandlerRegistry::getHandler(role.c_str());
            if (handler) {
                ComplexComponentData data;
                data.def = def;
                data.pinLabel = pinLabelCStr;
                data.mainPinGpio = gpio;
                data.additionalPinCount = def->additionalPinCount;
                data.additionalPins =
                    new ComplexComponentData::AdditionalPinValue[data.additionalPinCount];
                for (uint8_t i = 0; i < def->additionalPinCount && i < def->additionalPinsCapacity; i++) {
                    const AdditionalPinDef& ap = def->additionalPins[i];
                    data.additionalPins[i].id = ap.id;
                    data.additionalPins[i].gpio =
                        (uint8_t)JSONParser::extractInt(pinConfig, ap.id, ap.defaultValue);
                }
                if (!handler->addComponent(data))
                    Serial.printf("[ConfigLoader] %s sur %s : le handler a refuse la restauration\n",
                                  role.c_str(), pinLabelCStr);
                delete[] data.additionalPins;
            }
        }

        // Configurer les flags OSC si le composant a été ajouté avec succès
        if (success) {
            // Trouver l'index du composant ajouté
            uint8_t index = manager.findComponentByGpio(gpio);
            if (index != 255) {
                ComponentConfig* config = manager.getConfigMutable(index);
                if (config) {
                    // Lire oscEnabled, oscFormat et oscAddress depuis la config (× master NVS osc_out_all)
                    bool oscEnabled = oscOutAll && JSONParser::extractBool(pinConfig, "oscEnabled", false);
                    String oscFormat = JSONParser::extractStr(pinConfig, "oscFormat", "float");
                    String oscAddress = JSONParser::extractStr(pinConfig, "oscAddress", "");
                    
                    // Configurer les flags (bit 0x02 pour OSC, bit 0x04 pour format MIDI, bit 0x08 pour format RAW)
                    if (oscEnabled) {
                        config->flags |= 0x02; // Activer OSC
                        if (oscFormat.length() > 0 && oscFormat.equalsIgnoreCase("midi")) {
                            config->flags |= 0x04; // Format MIDI
                            config->flags &= ~0x08; // Pas RAW
                        } else if (oscFormat.length() > 0 && oscFormat.equalsIgnoreCase("raw")) {
                            config->flags |= 0x08; // Format RAW
                            config->flags &= ~0x04; // Pas MIDI
                        } else {
                            config->flags &= ~0x04; // Format float
                            config->flags &= ~0x08; // Pas RAW
                        }
                    } else {
                        config->flags &= ~0x02; // Désactiver OSC
                        config->flags &= ~0x04; // Pas MIDI
                        config->flags &= ~0x08; // Pas RAW
                    }
                    
                    // Configurer l'adresse OSC (utiliser valeur par défaut si vide)
                    if (oscAddress.length() > 0) {
                        strncpy(config->osc_address, oscAddress.c_str(), sizeof(config->osc_address) - 1);
                        config->osc_address[sizeof(config->osc_address) - 1] = '\0';
                        Serial.printf("[ConfigLoader] OSC address from config: '%s' for %s\n", 
                                      oscAddress.c_str(), pinLabelCStr);
                    } else {
                        Serial.printf("[ConfigLoader] OSC address empty for %s, using default: '%s'\n", 
                                      pinLabelCStr, config->osc_address);
                    }

                    // Mapping script: stocke la chaîne user-defined dans le ComponentConfig
                    bool hasMappingScript = false;
                    {
                        String script = JSONParser::extractStr(pinConfig, "mappingScript", "");
                        if (script.length() > 0) {
                            strncpy(config->mappingScript, script.c_str(), sizeof(config->mappingScript) - 1);
                            config->mappingScript[sizeof(config->mappingScript) - 1] = '\0';
                            hasMappingScript = true;
                        } else {
                            config->mappingScript[0] = '\0';
                        }
                    }

                    // Nom personnalisé du composant
                    {
                        String componentName = JSONParser::extractStr(pinConfig, "name", "");
                        if (componentName.length() > 0) {
                            strncpy(config->name, componentName.c_str(), sizeof(config->name) - 1);
                            config->name[sizeof(config->name) - 1] = '\0';
                        } else {
                            config->name[0] = '\0';
                        }
                    }

                    // MIDI mode: RTP vs Mapping
                    // Compat legacy: si midiMode absent mais mappingScript présent, activer SCRIPT.
                    {
                        String midiModeStr = JSONParser::extractStr(pinConfig, "midiMode", "");
                        if (midiModeStr == "script") {
                            config->midiMode = MidiMode::SCRIPT;
                        } else if (midiModeStr == "rtp") {
                            config->midiMode = MidiMode::RTP;
                        } else {
                            config->midiMode = hasMappingScript ? MidiMode::SCRIPT : MidiMode::RTP;
                        }

                        Serial.printf("[ConfigLoader] pin=%s midiModeRaw='%s' midiMode=%s mappingScript='%s'\n",
                                      pinLabelCStr,
                                      midiModeStr.c_str(),
                                      (config->midiMode == MidiMode::SCRIPT) ? "SCRIPT" : "RTP",
                                      config->mappingScript);
                    }
                    
                    // Charger les configurations spécifiques selon le type de composant
                    // La config spécifique doit déjà être allouée par ComponentInitializer
                    switch (config->type) {
                        case ComponentType::BUTTON: {
                            if (config->specificConfig.button) {
                                Components::ButtonConfig* btnConfig = config->specificConfig.button;
                                if (def && def->formFields) {
                                    for (uint8_t i = 0; i < def->formFieldCount && i < MAX_FORM_FIELDS; i++) {
                                        const FormFieldDef& field = def->formFields[i];
                                        if (field.id) {
                                            if (strcmp(field.id, "btnMode") == 0) {
                                                String fieldValue = JSONParser::extractStr(pinConfig, field.id, field.defaultValue ? field.defaultValue : "");
                                                if (fieldValue.length() > 0) {
                                                    strncpy(btnConfig->btnMode, fieldValue.c_str(), sizeof(btnConfig->btnMode) - 1);
                                                    btnConfig->btnMode[sizeof(btnConfig->btnMode) - 1] = '\0';
                                                }
                                            } else if (strcmp(field.id, "btnPulseTiming") == 0) {
                                                String fieldValue = JSONParser::extractStr(pinConfig, field.id, field.defaultValue ? field.defaultValue : "");
                                                if (fieldValue.length() > 0) {
                                                    strncpy(btnConfig->btnPulseTiming, fieldValue.c_str(), sizeof(btnConfig->btnPulseTiming) - 1);
                                                    btnConfig->btnPulseTiming[sizeof(btnConfig->btnPulseTiming) - 1] = '\0';
                                                }
                                            } else if (strcmp(field.id, "btnPullMode") == 0) {
                                                String fieldValue = JSONParser::extractStr(pinConfig, field.id, field.defaultValue ? field.defaultValue : "");
                                                if (fieldValue.length() > 0) {
                                                    strncpy(btnConfig->btnPullMode, fieldValue.c_str(), sizeof(btnConfig->btnPullMode) - 1);
                                                    btnConfig->btnPullMode[sizeof(btnConfig->btnPullMode) - 1] = '\0';
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            break;
                        }
                        case ComponentType::LED: {
                            if (config->specificConfig.led) {
                                Components::LedConfig* ledConfig = config->specificConfig.led;
                                if (def && def->formFields) {
                                    for (uint8_t i = 0; i < def->formFieldCount && i < MAX_FORM_FIELDS; i++) {
                                        const FormFieldDef& field = def->formFields[i];
                                        if (field.id && strcmp(field.id, "ledMode") == 0) {
                                            String fieldValue = JSONParser::extractStr(pinConfig, field.id, field.defaultValue ? field.defaultValue : "");
                                            if (fieldValue.length() > 0) {
                                                strncpy(ledConfig->ledMode, fieldValue.c_str(), sizeof(ledConfig->ledMode) - 1);
                                                ledConfig->ledMode[sizeof(ledConfig->ledMode) - 1] = '\0';
                                            }
                                        } else if (field.id && strcmp(field.id, "ledPolarity") == 0) {
                                            String pol = JSONParser::extractStr(pinConfig, field.id, field.defaultValue ? field.defaultValue : "high");
                                            ledConfig->activeLow = (pol == "low");
                                        }
                                    }
                                }
                            }
                            break;
                        }
                        case ComponentType::POTENTIOMETER: {
                            if (config->specificConfig.potentiometer) {
                                Components::PotentiometerConfig* potConfig = config->specificConfig.potentiometer;
                                int potMin = JSONParser::extractInt(pinConfig, "potMin", 0);
                                int potMax = JSONParser::extractInt(pinConfig, "potMax", 4095);
                                potConfig->potMin = (potMin >= 0 && potMin <= 4095) ? potMin : 0;
                                potConfig->potMax = (potMax >= 0 && potMax <= 4095) ? potMax : 4095;
                                if (potConfig->potMin >= potConfig->potMax) {
                                    uint16_t temp = potConfig->potMin;
                                    potConfig->potMin = potConfig->potMax;
                                    potConfig->potMax = temp;
                                }
                                if (def && def->formFields) {
                                    for (uint8_t i = 0; i < def->formFieldCount && i < MAX_FORM_FIELDS; i++) {
                                        const FormFieldDef& field = def->formFields[i];
                                        if (field.id && strcmp(field.id, "filterIntensity") == 0) {
                                            uint8_t filter_intensity = JSONParser::extractInt(pinConfig, "filterIntensity", field.defaultValue ? atoi(field.defaultValue) : 5);
                                            if (filter_intensity < 1) filter_intensity = 1;
                                            if (filter_intensity > 10) filter_intensity = 10;
                                            potConfig->filter_intensity = filter_intensity;
                                        }
                                    }
                                }
                            }
                            break;
                        }
                        case ComponentType::NOISE_SAMPLER: {
                            if (config->specificConfig.noiseSampler) {
                                Components::NoiseSamplerConfig* nsConfig = config->specificConfig.noiseSampler;
                                String mode = JSONParser::extractStr(pinConfig, "sampleMode", "sandh");
                                if (mode.length() > 0) {
                                    strncpy(nsConfig->sampleMode, mode.c_str(), sizeof(nsConfig->sampleMode) - 1);
                                    nsConfig->sampleMode[sizeof(nsConfig->sampleMode) - 1] = '\0';
                                }
                                int rateMs = JSONParser::extractInt(pinConfig, "rateMs", 250);
                                if (rateMs < 1) rateMs = 1;
                                if (rateMs > 65535) rateMs = 65535;
                                nsConfig->rateMs = (uint16_t)rateMs;
                                int inMin = JSONParser::extractInt(pinConfig, "inMin", 0);
                                int inMax = JSONParser::extractInt(pinConfig, "inMax", 4095);
                                nsConfig->inMin = (inMin >= 0 && inMin <= 4095) ? inMin : 0;
                                nsConfig->inMax = (inMax >= 0 && inMax <= 4095) ? inMax : 4095;
                                if (nsConfig->inMin >= nsConfig->inMax) {
                                    uint16_t t = nsConfig->inMin;
                                    nsConfig->inMin = nsConfig->inMax;
                                    nsConfig->inMax = t;
                                }
                                int fi = JSONParser::extractInt(pinConfig, "filterIntensity", 3);
                                if (fi < 1) fi = 1;
                                if (fi > 10) fi = 10;
                                nsConfig->filter_intensity = (uint8_t)fi;
                            }
                            break;
                        }
                        case ComponentType::VELOSTAT: {
                            if (config->specificConfig.velostat) {
                                Components::VelostatConfig* veloConfig = config->specificConfig.velostat;
                                if (def && def->formFields) {
                                    for (uint8_t i = 0; i < def->formFieldCount && i < MAX_FORM_FIELDS; i++) {
                                        const FormFieldDef& field = def->formFields[i];
                                        if (field.id) {
                                            if (strcmp(field.id, "filterIntensity") == 0) {
                                                uint8_t filter_intensity = JSONParser::extractInt(pinConfig, "filterIntensity", field.defaultValue ? atoi(field.defaultValue) : 5);
                                                if (filter_intensity < 1) filter_intensity = 1;
                                                if (filter_intensity > 10) filter_intensity = 10;
                                                veloConfig->filter_intensity = filter_intensity;
                                            } else if (strcmp(field.id, "velocityThreshold") == 0) {
                                                veloConfig->velocityThreshold = JSONParser::extractInt(pinConfig, "velocityThreshold", field.defaultValue ? atoi(field.defaultValue) : 50);
                                            } else if (strcmp(field.id, "aftertouchThreshold") == 0) {
                                                veloConfig->aftertouchThreshold = JSONParser::extractInt(pinConfig, "aftertouchThreshold", field.defaultValue ? atoi(field.defaultValue) : 4);
                                            } else if (strcmp(field.id, "velocityMax") == 0) {
                                                int vmax = JSONParser::extractInt(pinConfig, "velocityMax", field.defaultValue ? atoi(field.defaultValue) : 4095);
                                                if (vmax < 1) vmax = 1;
                                                if (vmax > 4095) vmax = 4095;
                                                veloConfig->velocityMax = (uint16_t)vmax;
                                            } else if (strcmp(field.id, "scanTimeMs") == 0) {
                                                int scan = JSONParser::extractInt(pinConfig, "scanTimeMs", field.defaultValue ? atoi(field.defaultValue) : 5);
                                                if (scan < 0) scan = 0;
                                                if (scan > 20) scan = 20;
                                                veloConfig->scanTimeMs = (uint8_t)scan;
                                            } else if (strcmp(field.id, "maskTimeMs") == 0) {
                                                int mask = JSONParser::extractInt(pinConfig, "maskTimeMs", field.defaultValue ? atoi(field.defaultValue) : 30);
                                                if (mask < 0) mask = 0;
                                                if (mask > 200) mask = 200;
                                                veloConfig->maskTimeMs = (uint8_t)mask;
                                            }
                                        }
                                    }
                                }
                            }
                            break;
                        }
                        case ComponentType::JOYSTICK: {
                            if (config->specificConfig.joystick) {
                                Components::JoystickConfig* joyConfig = config->specificConfig.joystick;
                                joyConfig->joyXMin = JSONParser::extractInt(pinConfig, "xMin", 200);
                                joyConfig->joyXZeroMin = JSONParser::extractInt(pinConfig, "xZeroMin", 1900);
                                joyConfig->joyXZeroMax = JSONParser::extractInt(pinConfig, "xZeroMax", 2100);
                                joyConfig->joyXMax = JSONParser::extractInt(pinConfig, "xMax", 4000);
                                joyConfig->joyYMin = JSONParser::extractInt(pinConfig, "yMin", 200);
                                joyConfig->joyYZeroMin = JSONParser::extractInt(pinConfig, "yZeroMin", 1900);
                                joyConfig->joyYZeroMax = JSONParser::extractInt(pinConfig, "yZeroMax", 2100);
                                joyConfig->joyYMax = JSONParser::extractInt(pinConfig, "yMax", 4000);
                                // Valider les valeurs (0-4095)
                                if (joyConfig->joyXMin > 4095) joyConfig->joyXMin = 200;
                                if (joyConfig->joyXZeroMin > 4095) joyConfig->joyXZeroMin = 1900;
                                if (joyConfig->joyXZeroMax > 4095) joyConfig->joyXZeroMax = 2100;
                                if (joyConfig->joyXMax > 4095) joyConfig->joyXMax = 4000;
                                if (joyConfig->joyYMin > 4095) joyConfig->joyYMin = 200;
                                if (joyConfig->joyYZeroMin > 4095) joyConfig->joyYZeroMin = 1900;
                                if (joyConfig->joyYZeroMax > 4095) joyConfig->joyYZeroMax = 2100;
                                if (joyConfig->joyYMax > 4095) joyConfig->joyYMax = 4000;
                                if (def && def->formFields) {
                                    for (uint8_t i = 0; i < def->formFieldCount && i < MAX_FORM_FIELDS; i++) {
                                        const FormFieldDef& field = def->formFields[i];
                                        if (field.id && strcmp(field.id, "filterIntensity") == 0) {
                                            uint8_t filter_intensity = JSONParser::extractInt(pinConfig, "filterIntensity", field.defaultValue ? atoi(field.defaultValue) : 5);
                                            if (filter_intensity < 1) filter_intensity = 1;
                                            if (filter_intensity > 10) filter_intensity = 10;
                                            joyConfig->filter_intensity = filter_intensity;
                                        }
                                    }
                                }
                                
                                // Inversion d'axes (checkbox → "0"/"1" dans le JSON)
                                joyConfig->invertX = JSONParser::extractBool(pinConfig, "invertX", false);
                                joyConfig->invertY = JSONParser::extractBool(pinConfig, "invertY", false);
                                
                                // Charger les types MIDI par axe
                                String xMsgTypeStr = JSONParser::extractStr(pinConfig, "midiMessageTypeX", "");
                                String yMsgTypeStr = JSONParser::extractStr(pinConfig, "midiMessageTypeY", "");
                                if (xMsgTypeStr.length() > 0) {
                                    joyConfig->xMsgType = stringToMidiMessageType(xMsgTypeStr);
                                } else {
                                    // Fallback: utiliser le msg_type global (déjà parsé)
                                    joyConfig->xMsgType = msg_type;
                                }
                                if (yMsgTypeStr.length() > 0) {
                                    joyConfig->yMsgType = stringToMidiMessageType(yMsgTypeStr);
                                } else {
                                    joyConfig->yMsgType = joyConfig->xMsgType; // Même type que X par défaut
                                }
                                
                                // Charger les paramètres MIDI par axe (préfixés X_/Y_)
                                // Le param dépend du type de message sélectionné
                                // CC -> midiCc, Note/Sweep -> midiNote, PitchBend/Aftertouch -> pas de param
                                auto loadAxisParam = [&](const char* axisPrefix, MidiMessageType type, uint8_t fallback) -> uint8_t {
                                    char key[20];
                                    if (type == MidiMessageType::CONTROL_CHANGE) {
                                        snprintf(key, sizeof(key), "%s_midiCc", axisPrefix);
                                        return JSONParser::extractInt(pinConfig, key, 
                                            JSONParser::extractInt(pinConfig, "midiCc", fallback));
                                    } else if (type == MidiMessageType::NOTE_SWEEP || type == MidiMessageType::NOTE) {
                                        snprintf(key, sizeof(key), "%s_midiNote", axisPrefix);
                                        return JSONParser::extractInt(pinConfig, key, 
                                            JSONParser::extractInt(pinConfig, "midiNote", fallback));
                                    }
                                    return fallback;
                                };
                                joyConfig->xMidiParam = loadAxisParam("X", joyConfig->xMsgType, midi_param);
                                joyConfig->yMidiParam = loadAxisParam("Y", joyConfig->yMsgType, midi_param);
                                joyConfig->xMidiChannel = JSONParser::extractInt(pinConfig, "X_midiChannel",
                                    JSONParser::extractInt(pinConfig, "midiChannel", channel));
                                joyConfig->yMidiChannel = JSONParser::extractInt(pinConfig, "Y_midiChannel",
                                    JSONParser::extractInt(pinConfig, "midiChannel", channel));

                                loadNoteSweepRangeFromJson(pinConfig, "X", joyConfig->xNoteSweepMin, joyConfig->xNoteSweepMax);
                                loadNoteSweepRangeFromJson(pinConfig, "Y", joyConfig->yNoteSweepMin, joyConfig->yNoteSweepMax);

                                // Auto-off NOTE_SWEEP et vélocité fixe par axe (formulaire préfixe X_/Y_)
                                auto loadAxisAutoOff = [&](const char* axisPrefix) -> uint16_t {
                                    char key[40];
                                    snprintf(key, sizeof(key), "%s_midiNoteSweepAutoOffDelay", axisPrefix);
                                    int off = JSONParser::extractInt(pinConfig, key, -1);
                                    if (off < 0) off = JSONParser::extractInt(pinConfig, "midiNoteSweepAutoOffDelay", -1);
                                    if (off < 0) off = JSONParser::extractInt(pinConfig, "rtpNoteSweepAutoOffDelay", 1000);
                                    if (off < 0) off = 0;
                                    if (off > 65535) off = 65535;
                                    return (uint16_t)off;
                                };
                                auto loadAxisVel = [&](const char* axisPrefix) -> uint8_t {
                                    char key[40];
                                    snprintf(key, sizeof(key), "%s_midiNoteVelocityFix", axisPrefix);
                                    int vel = JSONParser::extractInt(pinConfig, key, -1);
                                    if (vel < 0) vel = JSONParser::extractInt(pinConfig, "midiNoteVelocityFix", -1);
                                    if (vel < 0) vel = JSONParser::extractInt(pinConfig, "rtpNoteVelFix", 100);
                                    if (vel < 1) vel = 1;
                                    if (vel > 127) vel = 127;
                                    return (uint8_t)vel;
                                };
                                joyConfig->xAutoOffDelay = loadAxisAutoOff("X");
                                joyConfig->yAutoOffDelay = loadAxisAutoOff("Y");
                                joyConfig->xNoteVelFix = loadAxisVel("X");
                                joyConfig->yNoteVelFix = loadAxisVel("Y");

                                // Aussi mettre à jour le msg_type principal pour le X
                                config->msg_type = joyConfig->xMsgType;
                                
                                Serial.printf("[ConfigLoader] Joystick MIDI: X=%d(ch%d,p%d) Y=%d(ch%d,p%d)\n",
                                    (int)joyConfig->xMsgType, joyConfig->xMidiChannel, joyConfig->xMidiParam,
                                    (int)joyConfig->yMsgType, joyConfig->yMidiChannel, joyConfig->yMidiParam);
                            }
                            break;
                        }
                        case ComponentType::JOYSTICK3: {
                            if (config->specificConfig.joystick3) {
                                Components::Joystick3Config* joyConfig = config->specificConfig.joystick3;
                                joyConfig->joyXMin = JSONParser::extractInt(pinConfig, "xMin", 200);
                                joyConfig->joyXZeroMin = JSONParser::extractInt(pinConfig, "xZeroMin", 1900);
                                joyConfig->joyXZeroMax = JSONParser::extractInt(pinConfig, "xZeroMax", 2100);
                                joyConfig->joyXMax = JSONParser::extractInt(pinConfig, "xMax", 4000);
                                joyConfig->joyYMin = JSONParser::extractInt(pinConfig, "yMin", 200);
                                joyConfig->joyYZeroMin = JSONParser::extractInt(pinConfig, "yZeroMin", 1900);
                                joyConfig->joyYZeroMax = JSONParser::extractInt(pinConfig, "yZeroMax", 2100);
                                joyConfig->joyYMax = JSONParser::extractInt(pinConfig, "yMax", 4000);
                                joyConfig->joyZMin = JSONParser::extractInt(pinConfig, "zMin", 200);
                                joyConfig->joyZZeroMin = JSONParser::extractInt(pinConfig, "zZeroMin", 1900);
                                joyConfig->joyZZeroMax = JSONParser::extractInt(pinConfig, "zZeroMax", 2100);
                                joyConfig->joyZMax = JSONParser::extractInt(pinConfig, "zMax", 4000);
                                // Valider les valeurs (0-4095)
                                if (joyConfig->joyXMin > 4095) joyConfig->joyXMin = 200;
                                if (joyConfig->joyXZeroMin > 4095) joyConfig->joyXZeroMin = 1900;
                                if (joyConfig->joyXZeroMax > 4095) joyConfig->joyXZeroMax = 2100;
                                if (joyConfig->joyXMax > 4095) joyConfig->joyXMax = 4000;
                                if (joyConfig->joyYMin > 4095) joyConfig->joyYMin = 200;
                                if (joyConfig->joyYZeroMin > 4095) joyConfig->joyYZeroMin = 1900;
                                if (joyConfig->joyYZeroMax > 4095) joyConfig->joyYZeroMax = 2100;
                                if (joyConfig->joyYMax > 4095) joyConfig->joyYMax = 4000;
                                if (joyConfig->joyZMin > 4095) joyConfig->joyZMin = 200;
                                if (joyConfig->joyZZeroMin > 4095) joyConfig->joyZZeroMin = 1900;
                                if (joyConfig->joyZZeroMax > 4095) joyConfig->joyZZeroMax = 2100;
                                if (joyConfig->joyZMax > 4095) joyConfig->joyZMax = 4000;
                                if (def && def->formFields) {
                                    for (uint8_t i = 0; i < def->formFieldCount && i < MAX_FORM_FIELDS; i++) {
                                        const FormFieldDef& field = def->formFields[i];
                                        if (field.id && strcmp(field.id, "filterIntensity") == 0) {
                                            uint8_t filter_intensity = JSONParser::extractInt(pinConfig, "filterIntensity", field.defaultValue ? atoi(field.defaultValue) : 5);
                                            if (filter_intensity < 1) filter_intensity = 1;
                                            if (filter_intensity > 10) filter_intensity = 10;
                                            joyConfig->filter_intensity = filter_intensity;
                                        }
                                    }
                                }

                                // Inversion d'axes (checkbox → "0"/"1" dans le JSON)
                                joyConfig->invertX = JSONParser::extractBool(pinConfig, "invertX", false);
                                joyConfig->invertY = JSONParser::extractBool(pinConfig, "invertY", false);
                                joyConfig->invertZ = JSONParser::extractBool(pinConfig, "invertZ", false);

                                // Charger les types MIDI par axe
                                String xMsgTypeStr = JSONParser::extractStr(pinConfig, "midiMessageTypeX", "");
                                String yMsgTypeStr = JSONParser::extractStr(pinConfig, "midiMessageTypeY", "");
                                String zMsgTypeStr = JSONParser::extractStr(pinConfig, "midiMessageTypeZ", "");
                                if (xMsgTypeStr.length() > 0) {
                                    joyConfig->xMsgType = stringToMidiMessageType(xMsgTypeStr);
                                } else {
                                    // Fallback: utiliser le msg_type global (déjà parsé)
                                    joyConfig->xMsgType = msg_type;
                                }
                                if (yMsgTypeStr.length() > 0) {
                                    joyConfig->yMsgType = stringToMidiMessageType(yMsgTypeStr);
                                } else {
                                    joyConfig->yMsgType = joyConfig->xMsgType; // Même type que X par défaut
                                }
                                if (zMsgTypeStr.length() > 0) {
                                    joyConfig->zMsgType = stringToMidiMessageType(zMsgTypeStr);
                                } else {
                                    joyConfig->zMsgType = joyConfig->xMsgType; // Même type que X par défaut
                                }

                                // Charger les paramètres MIDI par axe (préfixés X_/Y_/Z_)
                                // Le param dépend du type de message sélectionné
                                // CC -> midiCc, Note/Sweep -> midiNote, PitchBend/Aftertouch -> pas de param
                                auto loadAxisParam = [&](const char* axisPrefix, MidiMessageType type, uint8_t fallback) -> uint8_t {
                                    char key[20];
                                    if (type == MidiMessageType::CONTROL_CHANGE) {
                                        snprintf(key, sizeof(key), "%s_midiCc", axisPrefix);
                                        return JSONParser::extractInt(pinConfig, key,
                                            JSONParser::extractInt(pinConfig, "midiCc", fallback));
                                    } else if (type == MidiMessageType::NOTE_SWEEP || type == MidiMessageType::NOTE) {
                                        snprintf(key, sizeof(key), "%s_midiNote", axisPrefix);
                                        return JSONParser::extractInt(pinConfig, key,
                                            JSONParser::extractInt(pinConfig, "midiNote", fallback));
                                    }
                                    return fallback;
                                };
                                joyConfig->xMidiParam = loadAxisParam("X", joyConfig->xMsgType, midi_param);
                                joyConfig->yMidiParam = loadAxisParam("Y", joyConfig->yMsgType, midi_param);
                                joyConfig->zMidiParam = loadAxisParam("Z", joyConfig->zMsgType, midi_param);
                                joyConfig->xMidiChannel = JSONParser::extractInt(pinConfig, "X_midiChannel",
                                    JSONParser::extractInt(pinConfig, "midiChannel", channel));
                                joyConfig->yMidiChannel = JSONParser::extractInt(pinConfig, "Y_midiChannel",
                                    JSONParser::extractInt(pinConfig, "midiChannel", channel));
                                joyConfig->zMidiChannel = JSONParser::extractInt(pinConfig, "Z_midiChannel",
                                    JSONParser::extractInt(pinConfig, "midiChannel", channel));

                                loadNoteSweepRangeFromJson(pinConfig, "X", joyConfig->xNoteSweepMin, joyConfig->xNoteSweepMax);
                                loadNoteSweepRangeFromJson(pinConfig, "Y", joyConfig->yNoteSweepMin, joyConfig->yNoteSweepMax);
                                loadNoteSweepRangeFromJson(pinConfig, "Z", joyConfig->zNoteSweepMin, joyConfig->zNoteSweepMax);

                                // Auto-off NOTE_SWEEP et vélocité fixe par axe (formulaire préfixe X_/Y_/Z_)
                                auto loadAxisAutoOff = [&](const char* axisPrefix) -> uint16_t {
                                    char key[40];
                                    snprintf(key, sizeof(key), "%s_midiNoteSweepAutoOffDelay", axisPrefix);
                                    int off = JSONParser::extractInt(pinConfig, key, -1);
                                    if (off < 0) off = JSONParser::extractInt(pinConfig, "midiNoteSweepAutoOffDelay", -1);
                                    if (off < 0) off = JSONParser::extractInt(pinConfig, "rtpNoteSweepAutoOffDelay", 1000);
                                    if (off < 0) off = 0;
                                    if (off > 65535) off = 65535;
                                    return (uint16_t)off;
                                };
                                auto loadAxisVel = [&](const char* axisPrefix) -> uint8_t {
                                    char key[40];
                                    snprintf(key, sizeof(key), "%s_midiNoteVelocityFix", axisPrefix);
                                    int vel = JSONParser::extractInt(pinConfig, key, -1);
                                    if (vel < 0) vel = JSONParser::extractInt(pinConfig, "midiNoteVelocityFix", -1);
                                    if (vel < 0) vel = JSONParser::extractInt(pinConfig, "rtpNoteVelFix", 100);
                                    if (vel < 1) vel = 1;
                                    if (vel > 127) vel = 127;
                                    return (uint8_t)vel;
                                };
                                joyConfig->xAutoOffDelay = loadAxisAutoOff("X");
                                joyConfig->yAutoOffDelay = loadAxisAutoOff("Y");
                                joyConfig->zAutoOffDelay = loadAxisAutoOff("Z");
                                joyConfig->xNoteVelFix = loadAxisVel("X");
                                joyConfig->yNoteVelFix = loadAxisVel("Y");
                                joyConfig->zNoteVelFix = loadAxisVel("Z");

                                // Aussi mettre à jour le msg_type principal pour le X
                                config->msg_type = joyConfig->xMsgType;

                                Serial.printf("[ConfigLoader] Joystick3 MIDI: X=%d(ch%d,p%d) Y=%d(ch%d,p%d) Z=%d(ch%d,p%d)\n",
                                    (int)joyConfig->xMsgType, joyConfig->xMidiChannel, joyConfig->xMidiParam,
                                    (int)joyConfig->yMsgType, joyConfig->yMidiChannel, joyConfig->yMidiParam,
                                    (int)joyConfig->zMsgType, joyConfig->zMidiChannel, joyConfig->zMidiParam);
                            }
                            break;
                        }
                        case ComponentType::TOUCH: {
                            // customField2 = "aftertouch,onRaw,offRaw" (ex. "20000,2000,500") pour limiter la taille JSON
                            // filterIntensity -> customField1
                            long aftRange = JSONParser::extractInt(pinConfig, "aftertouchRange", 20000);
                            if (aftRange < 0) aftRange = 0;
                            if (aftRange > 500000) aftRange = 500000;
                            String sVal = JSONParser::extractStr(pinConfig, "s", "0,0");
                            snprintf(config->customField2, sizeof(config->customField2), "%ld,%s", aftRange, sVal.c_str());
                            {
                                int filt = JSONParser::extractInt(pinConfig, "filterIntensity", 5);
                                if (filt < 1) filt = 1;
                                if (filt > 10) filt = 10;
                                snprintf(config->customField1, sizeof(config->customField1), "%d", filt);
                            }
                            break;
                        }
                        case ComponentType::IMU: {
                            if (config->specificConfig.imu) {
                                Components::ImuConfig* imuConfig = config->specificConfig.imu;
                                
                                // busInterface est déduit du label de la pin (pas de champ formulaire)
                                imuConfig->bus_interface = (uint8_t)JSONParser::extractInt(pinConfig, "busInterface", 0);
                                
                                // Charger les paramètres I2C et filtrage
                                if (def && def->formFields) {
                                    for (uint8_t i = 0; i < def->formFieldCount && i < MAX_FORM_FIELDS; i++) {
                                        const FormFieldDef& field = def->formFields[i];
                                        if (field.id) {
                                            if (strcmp(field.id, "filterIntensity") == 0) {
                                                uint8_t filter_intensity = JSONParser::extractInt(pinConfig, "filterIntensity", field.defaultValue ? atoi(field.defaultValue) : 5);
                                                if (filter_intensity < 1) filter_intensity = 1;
                                                if (filter_intensity > 10) filter_intensity = 10;
                                                imuConfig->filter_intensity = filter_intensity;
                                            } else if (strcmp(field.id, "i2cAddress") == 0) {
                                                imuConfig->i2c_address = JSONParser::extractInt(pinConfig, "i2cAddress", field.defaultValue ? atoi(field.defaultValue) : 24);
                                            } else if (strcmp(field.id, "csGpio") == 0) {
                                                imuConfig->cs_gpio = (uint8_t)JSONParser::extractInt(pinConfig, "csGpio", field.defaultValue ? atoi(field.defaultValue) : 2);
                                            } else if (strcmp(field.id, "range") == 0) {
                                                imuConfig->range = JSONParser::extractInt(pinConfig, "range", field.defaultValue ? atoi(field.defaultValue) : 0);
                                            } else if (strcmp(field.id, "dataRate") == 0) {
                                                imuConfig->data_rate = JSONParser::extractInt(pinConfig, "dataRate", field.defaultValue ? atoi(field.defaultValue) : 4);
                                            } else if (strcmp(field.id, "xMin") == 0) {
                                                imuConfig->xMin = JSONParser::extractInt(pinConfig, "xMin", field.defaultValue ? atoi(field.defaultValue) : -2000);
                                            } else if (strcmp(field.id, "xZeroMin") == 0) {
                                                imuConfig->xZeroMin = JSONParser::extractInt(pinConfig, "xZeroMin", field.defaultValue ? atoi(field.defaultValue) : -100);
                                            } else if (strcmp(field.id, "xZeroMax") == 0) {
                                                imuConfig->xZeroMax = JSONParser::extractInt(pinConfig, "xZeroMax", field.defaultValue ? atoi(field.defaultValue) : 100);
                                            } else if (strcmp(field.id, "xMax") == 0) {
                                                imuConfig->xMax = JSONParser::extractInt(pinConfig, "xMax", field.defaultValue ? atoi(field.defaultValue) : 2000);
                                            } else if (strcmp(field.id, "yMin") == 0) {
                                                imuConfig->yMin = JSONParser::extractInt(pinConfig, "yMin", field.defaultValue ? atoi(field.defaultValue) : -2000);
                                            } else if (strcmp(field.id, "yZeroMin") == 0) {
                                                imuConfig->yZeroMin = JSONParser::extractInt(pinConfig, "yZeroMin", field.defaultValue ? atoi(field.defaultValue) : -100);
                                            } else if (strcmp(field.id, "yZeroMax") == 0) {
                                                imuConfig->yZeroMax = JSONParser::extractInt(pinConfig, "yZeroMax", field.defaultValue ? atoi(field.defaultValue) : 100);
                                            } else if (strcmp(field.id, "yMax") == 0) {
                                                imuConfig->yMax = JSONParser::extractInt(pinConfig, "yMax", field.defaultValue ? atoi(field.defaultValue) : 2000);
                                            } else if (strcmp(field.id, "zMin") == 0) {
                                                imuConfig->zMin = JSONParser::extractInt(pinConfig, "zMin", field.defaultValue ? atoi(field.defaultValue) : -2000);
                                            } else if (strcmp(field.id, "zZeroMin") == 0) {
                                                imuConfig->zZeroMin = JSONParser::extractInt(pinConfig, "zZeroMin", field.defaultValue ? atoi(field.defaultValue) : -100);
                                            } else if (strcmp(field.id, "zZeroMax") == 0) {
                                                imuConfig->zZeroMax = JSONParser::extractInt(pinConfig, "zZeroMax", field.defaultValue ? atoi(field.defaultValue) : 100);
                                            } else if (strcmp(field.id, "zMax") == 0) {
                                                imuConfig->zMax = JSONParser::extractInt(pinConfig, "zMax", field.defaultValue ? atoi(field.defaultValue) : 2000);
                                            } else if (strcmp(field.id, "invertX") == 0) {
                                                imuConfig->invertX = JSONParser::extractBool(pinConfig, "invertX", false);
                                            } else if (strcmp(field.id, "invertY") == 0) {
                                                imuConfig->invertY = JSONParser::extractBool(pinConfig, "invertY", false);
                                            } else if (strcmp(field.id, "invertZ") == 0) {
                                                imuConfig->invertZ = JSONParser::extractBool(pinConfig, "invertZ", false);
                                            }
                                        }
                                    }
                                }
                                
                                // Charger les types MIDI par axe
                                String xMsgTypeStr = JSONParser::extractStr(pinConfig, "midiMessageTypeX", "");
                                String yMsgTypeStr = JSONParser::extractStr(pinConfig, "midiMessageTypeY", "");
                                String zMsgTypeStr = JSONParser::extractStr(pinConfig, "midiMessageTypeZ", "");
                                if (xMsgTypeStr.length() > 0) {
                                    imuConfig->xMsgType = stringToMidiMessageType(xMsgTypeStr);
                                } else {
                                    imuConfig->xMsgType = msg_type;
                                }
                                if (yMsgTypeStr.length() > 0) {
                                    imuConfig->yMsgType = stringToMidiMessageType(yMsgTypeStr);
                                } else {
                                    imuConfig->yMsgType = imuConfig->xMsgType;
                                }
                                if (zMsgTypeStr.length() > 0) {
                                    imuConfig->zMsgType = stringToMidiMessageType(zMsgTypeStr);
                                } else {
                                    imuConfig->zMsgType = imuConfig->xMsgType;
                                }
                                
                                // Charger les paramètres MIDI par axe (préfixés X_/Y_/Z_)
                                auto loadAxisParam = [&](const char* axisPrefix, MidiMessageType type, uint8_t fallback) -> uint8_t {
                                    char key[20];
                                    if (type == MidiMessageType::CONTROL_CHANGE) {
                                        snprintf(key, sizeof(key), "%s_midiCc", axisPrefix);
                                        return JSONParser::extractInt(pinConfig, key, 
                                            JSONParser::extractInt(pinConfig, "midiCc", fallback));
                                    } else if (type == MidiMessageType::NOTE_SWEEP || type == MidiMessageType::NOTE) {
                                        snprintf(key, sizeof(key), "%s_midiNote", axisPrefix);
                                        return JSONParser::extractInt(pinConfig, key, 
                                            JSONParser::extractInt(pinConfig, "midiNote", fallback));
                                    }
                                    return fallback;
                                };
                                imuConfig->xMidiParam = loadAxisParam("X", imuConfig->xMsgType, midi_param);
                                imuConfig->yMidiParam = loadAxisParam("Y", imuConfig->yMsgType, midi_param);
                                imuConfig->zMidiParam = loadAxisParam("Z", imuConfig->zMsgType, midi_param);
                                imuConfig->xMidiChannel = JSONParser::extractInt(pinConfig, "X_midiChannel",
                                    JSONParser::extractInt(pinConfig, "midiChannel", channel));
                                imuConfig->yMidiChannel = JSONParser::extractInt(pinConfig, "Y_midiChannel",
                                    JSONParser::extractInt(pinConfig, "midiChannel", channel));
                                imuConfig->zMidiChannel = JSONParser::extractInt(pinConfig, "Z_midiChannel",
                                    JSONParser::extractInt(pinConfig, "midiChannel", channel));

                                loadNoteSweepRangeFromJson(pinConfig, "X", imuConfig->xNoteSweepMin, imuConfig->xNoteSweepMax);
                                loadNoteSweepRangeFromJson(pinConfig, "Y", imuConfig->yNoteSweepMin, imuConfig->yNoteSweepMax);
                                loadNoteSweepRangeFromJson(pinConfig, "Z", imuConfig->zNoteSweepMin, imuConfig->zNoteSweepMax);
                                    
                                // Mettre à jour le msg_type principal pour le X
                                config->msg_type = imuConfig->xMsgType;
                                
                                Serial.printf("[ConfigLoader] IMU MIDI: X=%d(ch%d,p%d) Y=%d(ch%d,p%d) Z=%d(ch%d,p%d)\n",
                                    (int)imuConfig->xMsgType, imuConfig->xMidiChannel, imuConfig->xMidiParam,
                                    (int)imuConfig->yMsgType, imuConfig->yMidiChannel, imuConfig->yMidiParam,
                                    (int)imuConfig->zMsgType, imuConfig->zMidiChannel, imuConfig->zMidiParam);
                            }
                            break;
                        }
                        case ComponentType::MPR121: {
                            if (config->specificConfig.mpr121) {
                                Components::Mpr121Config* mpr121Config = config->specificConfig.mpr121;
                                if (def && def->formFields) {
                                    for (uint8_t i = 0; i < def->formFieldCount && i < MAX_FORM_FIELDS; i++) {
                                        const FormFieldDef& field = def->formFields[i];
                                        if (field.id) {
                                            if (strcmp(field.id, "i2cAddress") == 0) {
                                                mpr121Config->i2c_address = (uint8_t)JSONParser::extractInt(pinConfig, "i2cAddress", field.defaultValue ? atoi(field.defaultValue) : 90);
                                            } else if (strcmp(field.id, "baseNote") == 0) {
                                                mpr121Config->base_note = (uint8_t)JSONParser::extractInt(pinConfig, "baseNote", field.defaultValue ? atoi(field.defaultValue) : 60);
                                            } else if (strcmp(field.id, "touchThreshold") == 0) {
                                                mpr121Config->touch_threshold = (uint8_t)JSONParser::extractInt(pinConfig, "touchThreshold", field.defaultValue ? atoi(field.defaultValue) : 6);
                                            } else if (strcmp(field.id, "releaseThreshold") == 0) {
                                                mpr121Config->release_threshold = (uint8_t)JSONParser::extractInt(pinConfig, "releaseThreshold", field.defaultValue ? atoi(field.defaultValue) : 3);
                                            }
                                        }
                                    }
                                }
                                mpr121Config->midi_channel = JSONParser::extractInt(pinConfig, "midiChannel", channel);
                                String msgTypeStr = JSONParser::extractStr(pinConfig, "midiMessageType", "");
                                if (msgTypeStr.length() > 0) {
                                    mpr121Config->msg_type = stringToMidiMessageType(msgTypeStr);
                                } else {
                                    mpr121Config->msg_type = msg_type;
                                }
                                config->msg_type = mpr121Config->msg_type;
                            }
                            break;
                        }
                        default:
                            // Pas de config spécifique pour ce type
                            break;
                    }
                    
                    // Lire midiCcRangeMin et midiCcRangeMax (plage MIDI)
                    int midiCcRangeMin = JSONParser::extractInt(pinConfig, "midiCcRangeMin", 0);
                    int midiCcRangeMax = JSONParser::extractInt(pinConfig, "midiCcRangeMax", 127);
                    // Valider et stocker
                    config->midiCcRangeMin = (midiCcRangeMin >= 0 && midiCcRangeMin <= 127) ? midiCcRangeMin : 0;
                    config->midiCcRangeMax = (midiCcRangeMax >= 0 && midiCcRangeMax <= 127) ? midiCcRangeMax : 127;
                    // S'assurer que min < max
                    if (config->midiCcRangeMin >= config->midiCcRangeMax) {
                        uint8_t temp = config->midiCcRangeMin;
                        config->midiCcRangeMin = config->midiCcRangeMax;
                        config->midiCcRangeMax = temp;
                    }
                    
                    // Lire midiCcOnOffMin et midiCcOnOffMax pour boutons en CC
                    int midiCcOnOffMin = JSONParser::extractInt(pinConfig, "midiCcOnOffMin", 0);
                    int midiCcOnOffMax = JSONParser::extractInt(pinConfig, "midiCcOnOffMax", 127);
                    // Valider et stocker
                    config->midiCcOnOffMin = (midiCcOnOffMin >= 0 && midiCcOnOffMin <= 127) ? midiCcOnOffMin : 0;
                    config->midiCcOnOffMax = (midiCcOnOffMax >= 0 && midiCcOnOffMax <= 127) ? midiCcOnOffMax : 127;

                    // [correctif NiDMI] Lire midiVelocity (Note On des boutons).
                    // Le champ etait declare dans l'UI et jamais lu ici.
                    int midiVelocity = JSONParser::extractInt(pinConfig, "midiVelocity", 100);
                    config->midiVelocity = (midiVelocity >= 1 && midiVelocity <= 127) ? midiVelocity : 100;
                    
                    // Balayage NOTE_SWEEP : clés midiNoteSweepMin/Max (formulaire), pas seulement midiNoteMin
                    // Pour les composants multi-axes (joystick), le formulaire envoie des clés préfixées
                    // X_/Y_ ; on les essaie en fallback si la clé globale n'existe pas.
                    {
                        int vel = JSONParser::extractInt(pinConfig, "midiNoteVelocityFix", -1);
                        if (vel < 0) vel = JSONParser::extractInt(pinConfig, "X_midiNoteVelocityFix", -1);
                        if (vel < 0) vel = JSONParser::extractInt(pinConfig, "Y_midiNoteVelocityFix", -1);
                        if (vel < 0) vel = JSONParser::extractInt(pinConfig, "rtpNoteVelFix", 100);
                        if (vel < 1) vel = 1;
                        if (vel > 127) vel = 127;
                        config->rtpNoteVelFix = (uint8_t)vel;
                        int off = JSONParser::extractInt(pinConfig, "midiNoteSweepAutoOffDelay", -1);
                        if (off < 0) off = JSONParser::extractInt(pinConfig, "X_midiNoteSweepAutoOffDelay", -1);
                        if (off < 0) off = JSONParser::extractInt(pinConfig, "Y_midiNoteSweepAutoOffDelay", -1);
                        if (off < 0) off = JSONParser::extractInt(pinConfig, "rtpNoteSweepAutoOffDelay", 1000);
                        if (off < 0) off = 0;
                        if (off > 65535) off = 65535;
                        config->rtpNoteSweepAutoOffDelay = (uint16_t)off;
                    }
                    loadNoteSweepRangeFromJson(pinConfig, nullptr, config->rtpNoteMin, config->rtpNoteMax);
                    
                    // Note: filter_intensity est maintenant chargé dans les configs spécifiques ci-dessus
                    
                    // Reconfigurer le GPIO après avoir chargé tous les champs (notamment btnPullMode pour les boutons)
                    ComponentInitializer::setupGpio(gpio, type, config);
                    
                    Serial.printf("[ConfigLoader] Final OSC config: %s addr:%s for GPIO%d\n", 
                                 oscEnabled ? "enabled" : "disabled", config->osc_address, gpio);
                }
            }
        }
        // Serial.printf("[ConfigLoader] Added component: %s on GPIO%d -> %s\n", 
        //              pinLabel.c_str(), gpio, success ? "OK" : "FAILED");
    }
    
    // Charger aussi les configurations de bus (I2C, SPI, UART)
    const char* busLabels[] = {"I2C", "SPI", "TX", "RX"};
    for (int i = 0; i < 4; i++) {
        const char* busLabel = busLabels[i];
        char keyBuf[16];
        snprintf(keyBuf, sizeof(keyBuf), "pin_%s", busLabel);
        
        if (!preferences.isKey(keyBuf)) {
            continue;
        }
        
        String pinConfig = preferences.getString(keyBuf, "");
        if (pinConfig.length() == 0) {
            continue;
        }
        
        String role = JSONParser::extractStr(pinConfig, "role", "\n");
        if (role.length() == 0) continue;
        
        const ComponentDefinition* def = ComponentRegistry::findById(role.c_str());
        if (!def) {
            Serial.printf("[ConfigLoader] WARNING: Component '%s' not found in registry for bus %s\n", 
                          role.c_str(), busLabel);
            continue;
        }
        
        // Pour les bus, utiliser le GPIO du signal principal (SDA pour I2C, MOSI pour SPI, TX pour UART)
        uint8_t gpio = 255;
        if (strcmp(busLabel, "I2C") == 0) {
            gpio = PinMapper::labelToGpio("SDA");
        } else if (strcmp(busLabel, "SPI") == 0) {
            gpio = PinMapper::labelToGpio("MOSI");
        } else if (strcmp(busLabel, "TX") == 0 || strcmp(busLabel, "RX") == 0) {
            gpio = PinMapper::labelToGpio(busLabel);
        }
        
        if (gpio == 255) {
            Serial.printf("[ConfigLoader] Invalid bus label: %s (GPIO=255)\n", busLabel);
            continue;
        }
        
        // Continuer avec le chargement normal de la configuration
        // (réutiliser le code existant mais avec busLabel au lieu de pinLabelCStr)
        // ... (le reste du code de chargement est identique)
        
        // Pour simplifier, on va juste charger la config avec le GPIO du bus
        // Le code suivant est une copie adaptée de la logique existante
        // (on pourrait factoriser, mais pour l'instant on duplique pour éviter les erreurs)
        
        // Extraire paramètres MIDI (même logique que pour les pins normales)
        uint8_t midi_param = 7;
        uint8_t channel = 1;
        MidiMessageType msg_type = MidiMessageType::NOTE;
        
        String rtpTypeStr = JSONParser::extractStr(pinConfig, "midiMessageType", "");
        if (rtpTypeStr.length() == 0) {
            rtpTypeStr = JSONParser::extractStr(pinConfig, "rtpType", "");
        }
        
        const MidiMessageDef* msgDef = nullptr;
        if (rtpTypeStr.length() > 0) {
            for (uint8_t j = 0; j < def->midiMessageCount && j < MAX_MIDI_MESSAGES; j++) {
                if (def->midiMessages[j].displayName && strcmp(def->midiMessages[j].displayName, rtpTypeStr.c_str()) == 0) {
                    msgDef = &def->midiMessages[j];
                    break;
                }
            }
            if (!msgDef) {
                for (uint8_t j = 0; j < def->midiMessageCount && j < MAX_MIDI_MESSAGES; j++) {
                    if (def->midiMessages[j].id && strcmp(def->midiMessages[j].id, rtpTypeStr.c_str()) == 0) {
                        msgDef = &def->midiMessages[j];
                        break;
                    }
                }
            }
            if (msgDef && msgDef->displayName) {
                msg_type = stringToMidiMessageType(msgDef->displayName);
            } else {
                msg_type = stringToMidiMessageType(rtpTypeStr);
            }
        } else {
            if (def->midiMessageCount > 0) {
                msgDef = &def->midiMessages[0];
                if (msgDef->id) {
                    msg_type = stringToMidiMessageType(msgDef->id);
                }
            }
        }
        
        // Extraire les paramètres MIDI (simplifié pour l'instant)
        if (msgDef) {
            channel = JSONParser::extractInt(pinConfig, "midiChannel", 1);
            if (channel == 1) {
                channel = JSONParser::extractInt(pinConfig, "rtpChan", 1);
            }
            
            for (uint8_t j = 0; j < msgDef->paramCount && j < MAX_MIDI_PARAMS; j++) {
                const MidiParamDef& param = msgDef->params[j];
                if (param.id) {
                    if (strcmp(param.id, "midiCc") == 0) {
                        midi_param = JSONParser::extractInt(pinConfig, "midiCc", param.defaultValue ? atoi(param.defaultValue) : 7);
                        if (midi_param == (param.defaultValue ? atoi(param.defaultValue) : 7)) {
                            midi_param = JSONParser::extractInt(pinConfig, "rtpCc", midi_param);
                        }
                        break;
                    } else if (strcmp(param.id, "midiNote") == 0) {
                        midi_param = JSONParser::extractInt(pinConfig, "midiNote", param.defaultValue ? atoi(param.defaultValue) : 60);
                        if (midi_param == (param.defaultValue ? atoi(param.defaultValue) : 60)) {
                            midi_param = JSONParser::extractInt(pinConfig, "rtpNote", midi_param);
                        }
                        break;
                    }
                }
            }
        }
        
        // Extraire flags OSC et Debug (OSC effectif = master NVS osc_out_all × pin)
        bool oscEnabled = oscOutAll && JSONParser::extractBool(pinConfig, "oscEnabled", false);
        bool dbgEnabled = JSONParser::extractBool(pinConfig, "dbgEnabled", false);
        String oscAddress = JSONParser::extractStr(pinConfig, "oscAddress", "");
        String oscFormat = JSONParser::extractStr(pinConfig, "oscFormat", "");

        // Ajouter le composant au manager (comme pour A/D), puis récupérer sa config mutables
        ComponentType type = def->type;
        bool success = manager.addComponent(gpio, type, midi_param, channel, msg_type);
        if (!success) {
            Serial.printf("[ConfigLoader] Failed to add bus component %s on GPIO%d\n", busLabel, gpio);
            continue;
        }

        uint8_t index = manager.findComponentByGpio(gpio);
        if (index == 255) {
            Serial.printf("[ConfigLoader] WARNING: Added bus component %s on GPIO%d but not found by GPIO\n", busLabel, gpio);
            continue;
        }

        ComponentConfig* configPtr = manager.getConfigMutable(index);
        if (!configPtr) {
            Serial.printf("[ConfigLoader] WARNING: getConfigMutable returned NULL for bus %s on GPIO%d\n", busLabel, gpio);
            continue;
        }

        // Appliquer les flags OSC et Debug
        if (oscEnabled) configPtr->flags |= 0x02;
        if (dbgEnabled) configPtr->flags |= 0x01;
        if (oscAddress.length() > 0) {
            strncpy(configPtr->osc_address, oscAddress.c_str(), sizeof(configPtr->osc_address) - 1);
            configPtr->osc_address[sizeof(configPtr->osc_address) - 1] = '\0';
        }
        if (oscFormat == "raw") configPtr->flags |= 0x08;
        else if (oscFormat == "midi") configPtr->flags |= 0x04;

        // Nom personnalisé du composant
        {
            String componentName = JSONParser::extractStr(pinConfig, "name", "");
            if (componentName.length() > 0) {
                strncpy(configPtr->name, componentName.c_str(), sizeof(configPtr->name) - 1);
                configPtr->name[sizeof(configPtr->name) - 1] = '\0';
            } else {
                configPtr->name[0] = '\0';
            }
        }

        // Charger la configuration spécifique selon le type de composant
        if (configPtr->type == ComponentType::IMU && configPtr->specificConfig.imu) {
            Components::ImuConfig* imuConfig = configPtr->specificConfig.imu;

            // Auto-déterminer bus_interface depuis le label du bus (pas de champ formulaire)
            if (strcmp(busLabel, "SPI") == 0) {
                imuConfig->bus_interface = 1;
            } else {
                imuConfig->bus_interface = 0;
            }

            // Charger les paramètres I2C et filtrage depuis formFields
            if (def && def->formFields) {
                for (uint8_t j = 0; j < def->formFieldCount && j < MAX_FORM_FIELDS; j++) {
                    const FormFieldDef& field = def->formFields[j];
                    if (field.id) {
                        if (strcmp(field.id, "filterIntensity") == 0) {
                            imuConfig->filter_intensity = JSONParser::extractInt(pinConfig, "filterIntensity", field.defaultValue ? atoi(field.defaultValue) : 5);
                        } else if (strcmp(field.id, "i2cAddress") == 0) {
                            imuConfig->i2c_address = JSONParser::extractInt(pinConfig, "i2cAddress", field.defaultValue ? atoi(field.defaultValue) : 24);
                        } else if (strcmp(field.id, "csGpio") == 0) {
                            imuConfig->cs_gpio = (uint8_t)JSONParser::extractInt(pinConfig, "csGpio", field.defaultValue ? atoi(field.defaultValue) : 2);
                        } else if (strcmp(field.id, "range") == 0) {
                            imuConfig->range = JSONParser::extractInt(pinConfig, "range", field.defaultValue ? atoi(field.defaultValue) : 0);
                        } else if (strcmp(field.id, "dataRate") == 0) {
                            imuConfig->data_rate = JSONParser::extractInt(pinConfig, "dataRate", field.defaultValue ? atoi(field.defaultValue) : 4);
                        }
                        // Charger les seuils d'axes (xMin, xZeroMin, xZeroMax, xMax, etc.)
                        else if (strcmp(field.id, "xMin") == 0) imuConfig->xMin = JSONParser::extractInt(pinConfig, "xMin", field.defaultValue ? atoi(field.defaultValue) : -16000);
                        else if (strcmp(field.id, "xZeroMin") == 0) imuConfig->xZeroMin = JSONParser::extractInt(pinConfig, "xZeroMin", field.defaultValue ? atoi(field.defaultValue) : -100);
                        else if (strcmp(field.id, "xZeroMax") == 0) imuConfig->xZeroMax = JSONParser::extractInt(pinConfig, "xZeroMax", field.defaultValue ? atoi(field.defaultValue) : 100);
                        else if (strcmp(field.id, "xMax") == 0) imuConfig->xMax = JSONParser::extractInt(pinConfig, "xMax", field.defaultValue ? atoi(field.defaultValue) : 16000);
                        else if (strcmp(field.id, "yMin") == 0) imuConfig->yMin = JSONParser::extractInt(pinConfig, "yMin", field.defaultValue ? atoi(field.defaultValue) : -16000);
                        else if (strcmp(field.id, "yZeroMin") == 0) imuConfig->yZeroMin = JSONParser::extractInt(pinConfig, "yZeroMin", field.defaultValue ? atoi(field.defaultValue) : -100);
                        else if (strcmp(field.id, "yZeroMax") == 0) imuConfig->yZeroMax = JSONParser::extractInt(pinConfig, "yZeroMax", field.defaultValue ? atoi(field.defaultValue) : 100);
                        else if (strcmp(field.id, "yMax") == 0) imuConfig->yMax = JSONParser::extractInt(pinConfig, "yMax", field.defaultValue ? atoi(field.defaultValue) : 16000);
                        else if (strcmp(field.id, "zMin") == 0) imuConfig->zMin = JSONParser::extractInt(pinConfig, "zMin", field.defaultValue ? atoi(field.defaultValue) : -16000);
                        else if (strcmp(field.id, "zZeroMin") == 0) imuConfig->zZeroMin = JSONParser::extractInt(pinConfig, "zZeroMin", field.defaultValue ? atoi(field.defaultValue) : -100);
                        else if (strcmp(field.id, "zZeroMax") == 0) imuConfig->zZeroMax = JSONParser::extractInt(pinConfig, "zZeroMax", field.defaultValue ? atoi(field.defaultValue) : 100);
                        else if (strcmp(field.id, "zMax") == 0) imuConfig->zMax = JSONParser::extractInt(pinConfig, "zMax", field.defaultValue ? atoi(field.defaultValue) : 16000);
                    }
                }
            }

            // Charger les types MIDI par axe
            String xMsgTypeStr = JSONParser::extractStr(pinConfig, "midiMessageTypeX", "");
            String yMsgTypeStr = JSONParser::extractStr(pinConfig, "midiMessageTypeY", "");
            String zMsgTypeStr = JSONParser::extractStr(pinConfig, "midiMessageTypeZ", "");
            if (xMsgTypeStr.length() > 0) { imuConfig->xMsgType = stringToMidiMessageType(xMsgTypeStr); } else { imuConfig->xMsgType = msg_type; }
            if (yMsgTypeStr.length() > 0) { imuConfig->yMsgType = stringToMidiMessageType(yMsgTypeStr); } else { imuConfig->yMsgType = imuConfig->xMsgType; }
            if (zMsgTypeStr.length() > 0) { imuConfig->zMsgType = stringToMidiMessageType(zMsgTypeStr); } else { imuConfig->zMsgType = imuConfig->xMsgType; }

            // Charger les paramètres MIDI par axe (préfixés X_/Y_/Z_)
            auto loadAxisParam = [&](const char* axisPrefix, MidiMessageType typeAxis, uint8_t fallback) -> uint8_t {
                char key[32];
                snprintf(key, sizeof(key), "%s_midiCc", axisPrefix);
                uint8_t val = JSONParser::extractInt(pinConfig, key, 0);
                if (val == 0) {
                    snprintf(key, sizeof(key), "%s_midiNote", axisPrefix);
                    val = JSONParser::extractInt(pinConfig, key, 0);
                }
                if (val == 0) val = fallback;
                return val;
            };
            imuConfig->xMidiParam = loadAxisParam("X", imuConfig->xMsgType, midi_param);
            imuConfig->yMidiParam = loadAxisParam("Y", imuConfig->yMsgType, midi_param);
            imuConfig->zMidiParam = loadAxisParam("Z", imuConfig->zMsgType, midi_param);
            imuConfig->xMidiChannel = JSONParser::extractInt(pinConfig, "X_midiChannel", JSONParser::extractInt(pinConfig, "midiChannel", channel));
            imuConfig->yMidiChannel = JSONParser::extractInt(pinConfig, "Y_midiChannel", JSONParser::extractInt(pinConfig, "midiChannel", channel));
            imuConfig->zMidiChannel = JSONParser::extractInt(pinConfig, "Z_midiChannel", JSONParser::extractInt(pinConfig, "midiChannel", channel));

            loadNoteSweepRangeFromJson(pinConfig, "X", imuConfig->xNoteSweepMin, imuConfig->xNoteSweepMax);
            loadNoteSweepRangeFromJson(pinConfig, "Y", imuConfig->yNoteSweepMin, imuConfig->yNoteSweepMax);
            loadNoteSweepRangeFromJson(pinConfig, "Z", imuConfig->zNoteSweepMin, imuConfig->zNoteSweepMax);
            {
                // Auto-off et vélocité fixe NOTE_SWEEP par axe (formulaire préfixe X_/Y_/Z_).
                // Legacy global (midi.../rtp...) conservé en fallback pour les configs existantes.
                auto loadAxisAutoOff = [&](const char* axisPrefix) -> uint16_t {
                    char key[40];
                    snprintf(key, sizeof(key), "%s_midiNoteSweepAutoOffDelay", axisPrefix);
                    int off = JSONParser::extractInt(pinConfig, key, -1);
                    if (off < 0) off = JSONParser::extractInt(pinConfig, "midiNoteSweepAutoOffDelay", -1);
                    if (off < 0) off = JSONParser::extractInt(pinConfig, "rtpNoteSweepAutoOffDelay", 1000);
                    if (off < 0) off = 0;
                    if (off > 65535) off = 65535;
                    return (uint16_t)off;
                };
                auto loadAxisVel = [&](const char* axisPrefix) -> uint8_t {
                    char key[40];
                    snprintf(key, sizeof(key), "%s_midiNoteVelocityFix", axisPrefix);
                    int vel = JSONParser::extractInt(pinConfig, key, -1);
                    if (vel < 0) vel = JSONParser::extractInt(pinConfig, "midiNoteVelocityFix", -1);
                    if (vel < 0) vel = JSONParser::extractInt(pinConfig, "rtpNoteVelFix", 100);
                    if (vel < 1) vel = 1;
                    if (vel > 127) vel = 127;
                    return (uint8_t)vel;
                };
                imuConfig->xAutoOffDelay = loadAxisAutoOff("X");
                imuConfig->yAutoOffDelay = loadAxisAutoOff("Y");
                imuConfig->zAutoOffDelay = loadAxisAutoOff("Z");
                imuConfig->xNoteVelFix = loadAxisVel("X");
                imuConfig->yNoteVelFix = loadAxisVel("Y");
                imuConfig->zNoteVelFix = loadAxisVel("Z");
                // Champs globaux conservés = X (rétrocompat pour les parties non multi-axes du processeur).
                configPtr->rtpNoteVelFix = imuConfig->xNoteVelFix;
                configPtr->rtpNoteSweepAutoOffDelay = imuConfig->xAutoOffDelay;
            }
            loadNoteSweepRangeFromJson(pinConfig, nullptr, configPtr->rtpNoteMin, configPtr->rtpNoteMax);

            configPtr->msg_type = imuConfig->xMsgType;
            Serial.printf("[ConfigLoader] IMU MIDI (bus): X=%d(ch%d,p%d) Y=%d(ch%d,p%d) Z=%d(ch%d,p%d)\n",
                (int)imuConfig->xMsgType, imuConfig->xMidiChannel, imuConfig->xMidiParam,
                (int)imuConfig->yMsgType, imuConfig->yMidiChannel, imuConfig->yMidiParam,
                (int)imuConfig->zMsgType, imuConfig->zMidiChannel, imuConfig->zMidiParam);
            Serial.printf("[ConfigLoader] IMU config (bus): bus=%d cs=%d range=%d dataRate=%d filter=%d seuils X[%d,%d,%d,%d]\n",
                imuConfig->bus_interface, imuConfig->cs_gpio, imuConfig->range, imuConfig->data_rate,
                imuConfig->filter_intensity, imuConfig->xMin, imuConfig->xZeroMin, imuConfig->xZeroMax, imuConfig->xMax);
        } else if (configPtr->type == ComponentType::MPR121 && configPtr->specificConfig.mpr121) {
            Components::Mpr121Config* mpr121Config = configPtr->specificConfig.mpr121;
            if (def && def->formFields) {
                for (uint8_t j = 0; j < def->formFieldCount && j < MAX_FORM_FIELDS; j++) {
                    const FormFieldDef& field = def->formFields[j];
                    if (field.id) {
                        if (strcmp(field.id, "i2cAddress") == 0) {
                            mpr121Config->i2c_address = (uint8_t)JSONParser::extractInt(pinConfig, "i2cAddress", field.defaultValue ? atoi(field.defaultValue) : 90);
                        } else if (strcmp(field.id, "baseNote") == 0) {
                            mpr121Config->base_note = (uint8_t)JSONParser::extractInt(pinConfig, "baseNote", field.defaultValue ? atoi(field.defaultValue) : 60);
                        } else if (strcmp(field.id, "touchThreshold") == 0) {
                            mpr121Config->touch_threshold = (uint8_t)JSONParser::extractInt(pinConfig, "touchThreshold", field.defaultValue ? atoi(field.defaultValue) : 6);
                        } else if (strcmp(field.id, "releaseThreshold") == 0) {
                            mpr121Config->release_threshold = (uint8_t)JSONParser::extractInt(pinConfig, "releaseThreshold", field.defaultValue ? atoi(field.defaultValue) : 3);
                        }
                    }
                }
            }
            mpr121Config->midi_channel = JSONParser::extractInt(pinConfig, "midiChannel", channel);
            String msgTypeStr = JSONParser::extractStr(pinConfig, "midiMessageType", "");
            if (msgTypeStr.length() > 0) {
                mpr121Config->msg_type = stringToMidiMessageType(msgTypeStr);
            } else {
                mpr121Config->msg_type = msg_type;
            }
            configPtr->msg_type = mpr121Config->msg_type;
        }

        Serial.printf("[ConfigLoader] Loaded bus component %s (%s) on GPIO%d\n", busLabel, role.c_str(), gpio);
    }
    
    preferences.end();
    // Serial.printf("[ConfigLoader] Loaded %d components from NVS\n", manager.getComponentCount());
}
