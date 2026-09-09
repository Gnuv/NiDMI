#include "ComponentRegistry.h"
#include "Definitions.h"  // Centralise tous les includes de définitions pour garantir leur enregistrement automatique
#include <cstring>
#include <utility>  // pour std::move

// Initialisation des membres statiques
// Utiliser un pointeur pour éviter l'initialisation statique avant setup() (crash sur ESP32-C3)
std::vector<ComponentDefinition>* ComponentRegistry::definitions_ptr = nullptr;
std::vector<ComponentDefinition>* ComponentRegistry::pending_definitions_ptr = nullptr;
bool ComponentRegistry::initialized_ = false;

namespace {
void registerBuiltinDefinitions(std::vector<ComponentDefinition>& defs) {
    auto addIfMissing = [&](ComponentDefinition&& def) {
        if (def.id == nullptr) { def.cleanup(); return; }
        for (const auto& existing : defs) {
            if (existing.id && strcmp(existing.id, def.id) == 0) {
                def.cleanup();
                return;
            }
        }
        defs.push_back(std::move(def));
    };

    // BASIC
    addIfMissing(Components::Potentiometer::createDefinition());
    addIfMissing(Components::Button::createDefinition());
    addIfMissing(Components::Led::createDefinition());
    addIfMissing(Components::Velostat::createDefinition());
    addIfMissing(Components::Touch::createDefinition());
    addIfMissing(Components::Joystick::createDefinition());
    addIfMissing(Components::Joystick3::createDefinition());

    // AUDIO
    addIfMissing(Components::DacI2s::createDefinition());
    addIfMissing(Components::Ultrasonic::createDefinition());

    // MULTIPLEXER
    addIfMissing(Components::HC4067::createDefinition());
    addIfMissing(Components::HC4051::createDefinition());

    // DISTANCE
    addIfMissing(Components::IrDistanceSharp::createDefinition());
    addIfMissing(Components::LidarTof::createDefinition());
    addIfMissing(Components::InductiveProximity::createDefinition());

    // ENVIRONMENT
    addIfMissing(Components::EnvironmentGeneric::createDefinition());
    addIfMissing(Components::LightSensorGrove::createDefinition());
    addIfMissing(Components::TempHumDht::createDefinition());
    addIfMissing(Components::TempHumI2c::createDefinition());
    addIfMissing(Components::BarometerDps310::createDefinition());
    addIfMissing(Components::SoilMoistureCapacitive::createDefinition());
    addIfMissing(Components::UvSensorGrove::createDefinition());

    // MOTION
    addIfMissing(Components::MotionGeneric::createDefinition());
    addIfMissing(Components::PirMotion::createDefinition());
    addIfMissing(Components::Imu6Axis::createDefinition());
    addIfMissing(Components::Lis3dh::createDefinition());
    addIfMissing(Components::GestureIr::createDefinition());
    addIfMissing(Components::RadarDoppler::createDefinition());

    // COLOR
    addIfMissing(Components::ColorGeneric::createDefinition());

    // INTERFACE
    addIfMissing(Components::Fsr::createDefinition());
    addIfMissing(Components::Mpr121::createDefinition());
    addIfMissing(Components::RotaryAngleGrove::createDefinition());
    addIfMissing(Components::ThumbJoystickGrove::createDefinition());

    // ACTUATOR
    addIfMissing(Components::ActuatorGeneric::createDefinition());
    addIfMissing(Components::RelayGrove::createDefinition());
    addIfMissing(Components::BuzzerGrove::createDefinition());
    addIfMissing(Components::VibrationMotorGrove::createDefinition());
    addIfMissing(Components::SolenoidGrove::createDefinition());

    // DISPLAY
    addIfMissing(Components::DisplayGeneric::createDefinition());
    addIfMissing(Components::BargraphLedGrove::createDefinition());
    addIfMissing(Components::OledI2cGrove::createDefinition());
    addIfMissing(Components::Lcd16x2I2cGrove::createDefinition());
    addIfMissing(Components::FourDigitDisplayGrove::createDefinition());
    addIfMissing(Components::LedMatrixGrove::createDefinition());
}
} // namespace

bool ComponentRegistry::registerDefinition(ComponentDefinition&& def) {
    // Créer le vecteur temporaire si nécessaire
    // Note: Ce vecteur est créé au premier enregistrement, même avant init()
    if (pending_definitions_ptr == nullptr) {
        pending_definitions_ptr = new (std::nothrow) std::vector<ComponentDefinition>();
        if (pending_definitions_ptr == nullptr) {
            #ifdef ARDUINO
            Serial.printf("[ComponentRegistry] WARNING: Failed to allocate pending vector\n");
            #endif
            return false;
        }
    }
    
    // Vérifier que la définition est valide
    if (def.id == nullptr) {
        #ifdef ARDUINO
        Serial.printf("[ComponentRegistry] WARNING: Tentative d'enregistrement avec id=nullptr\n");
        #endif
        return false;
    }
    
    #ifdef ARDUINO
    Serial.printf("[ComponentRegistry] Enregistrement: %s\n", def.id);
    #endif
    
    // Déplacer la définition dans le vecteur temporaire
    pending_definitions_ptr->push_back(std::move(def));
    return true;
}

void ComponentRegistry::init() {
    if (initialized_) return;
    
    // Créer le vector seulement maintenant (heap est prêt après setup())
    // Cela évite les problèmes d'initialisation statique sur ESP32-C3
    if (definitions_ptr == nullptr) {
        definitions_ptr = new (std::nothrow) std::vector<ComponentDefinition>();
        if (definitions_ptr == nullptr) {
            // En mode production, Serial n'est peut-être pas encore initialisé
            // mais on essaie quand même pour le debug
            #ifdef ARDUINO
            Serial.printf("[ComponentRegistry] CRITICAL: Failed to allocate vector\n");
            #endif
            return;
        }
        // Réserver l'espace dans le vector pour éviter les réallocations
        // qui peuvent créer des copies temporaires
        definitions_ptr->reserve(48);  // Marge confortable pour toutes les familles
    }
    
    // Initialiser le ValidationRegistry d'abord
    ValidationRegistry::init();
    
    // === ENREGISTREMENT AUTOMATIQUE ===
    // Les définitions ont été enregistrées automatiquement via registerDefinition()
    // lors du chargement des modules. Il suffit maintenant de les déplacer
    // du vecteur temporaire vers le vecteur principal.
    
    // Déplacer toutes les définitions enregistrées automatiquement
    if (pending_definitions_ptr != nullptr && !pending_definitions_ptr->empty()) {
        // Réserver l'espace si nécessaire
        if (definitions_ptr->size() + pending_definitions_ptr->size() > definitions_ptr->capacity()) {
            definitions_ptr->reserve(definitions_ptr->size() + pending_definitions_ptr->size());
        }
        
        // Déplacer toutes les définitions enregistrées
        for (auto& def : *pending_definitions_ptr) {
            if (def.id != nullptr) {
                definitions_ptr->push_back(std::move(def));
                #ifdef ARDUINO
                Serial.printf("[ComponentRegistry] Composant enregistré: %s\n", def.id);
                #endif
            }
        }
        
        #ifdef ARDUINO
        Serial.printf("[ComponentRegistry] Total composants enregistrés: %zu\n", definitions_ptr->size());
        #endif
        
        // Nettoyer le vecteur temporaire
        pending_definitions_ptr->clear();
        delete pending_definitions_ptr;
        pending_definitions_ptr = nullptr;
    }
    
    // Garantit un registre complet même si l'éditeur de liens ignore
    // certains TU contenant les auto-register statiques.
    registerBuiltinDefinitions(*definitions_ptr);

    // === FAMILLES FUTURES ===
    // ENCODER : encodeurs rotatifs
    // DISPLAY : écrans OLED, LCD
    
    // Note: I2C et SPI ne sont PAS des composants, ce sont des bus hardware.
    // Les pins I2C/SPI sont identifiées via caps.bus dans l'API /api/pins/caps.
    // Les composants qui utilisent ces bus (écran OLED, DAC, etc.) seront ajoutés plus tard.
    
    // Note: TX et RX (UART) sont des pins digitales indépendantes.
    // Elles peuvent être utilisées séparément (ex: TX seul pour MIDI OUT).
    
    initialized_ = true;
}

const std::vector<ComponentDefinition>& ComponentRegistry::getAll() {
    // Créer un vector vide si pas encore initialisé (éviter crash)
    static std::vector<ComponentDefinition> empty_vector;
    if (definitions_ptr == nullptr) {
        return empty_vector;
    }
    return *definitions_ptr;
}

const ComponentDefinition* ComponentRegistry::findById(const char* id) {
    if (definitions_ptr == nullptr) return nullptr;
    for (const auto& def : *definitions_ptr) {
        if (strcmp(def.id, id) == 0) {
            return &def;
        }
    }
    return nullptr;
}

const ComponentDefinition* ComponentRegistry::findByType(ComponentType type) {
    if (definitions_ptr == nullptr) return nullptr;
    for (const auto& def : *definitions_ptr) {
        if (def.type == type) {
            return &def;
        }
    }
    return nullptr;
}

int ComponentRegistry::toJsonArray(char* buffer, size_t bufferSize) {
    if (bufferSize < 3 || definitions_ptr == nullptr) return 0;
    
    int written = 0;
    buffer[written++] = '[';
    
    bool first = true;
    for (const auto& def : *definitions_ptr) {
        // Calculer l'espace restant AVANT d'écrire la virgule
        size_t remaining = bufferSize - written - 1; // -1 pour le ']' final
        
        // Vérifier si on a assez de place pour au moins un objet minimal
        if (remaining < 50) break; // Pas assez de place même pour un objet minimal
        
        // Mémoriser si on va écrire une virgule (pour pouvoir l'annuler si besoin)
        bool wroteComma = false;
        if (!first) {
            buffer[written++] = ',';
            remaining--; // La virgule prend 1 caractère
            wroteComma = true;
        }
        first = false;
        
        // Écrire la définition en JSON
        int defLen = def.toJson(buffer + written, remaining);
        if (defLen <= 0 || defLen >= (int)remaining) {
            // Pas assez de place pour cet objet → annuler la virgule si on l'a écrite
            if (wroteComma && written > 1) {
                written--; // Retirer la virgule orpheline
            }
            break; // Arrêter proprement, JSON reste valide
        }
        
        written += defLen;
    }
    
    buffer[written++] = ']';
    buffer[written] = '\0';
    
    return written;
}

#ifdef NIDMI_COMPONENT_DEFS_PAGINATION
int ComponentRegistry::toJsonArrayPage(char* buffer, size_t bufferSize, int page, int limit) {
    if (bufferSize < 3 || definitions_ptr == nullptr) return 0;
    
    int written = 0;
    buffer[written++] = '[';
    
    // Calculer l'index de début et de fin
    int startIdx = page * limit;
    int endIdx = startIdx + limit;
    int totalCount = static_cast<int>(definitions_ptr->size());
    
    if (startIdx >= totalCount) {
        // Page vide
        buffer[written++] = ']';
        buffer[written] = '\0';
        return written;
    }
    
    if (endIdx > totalCount) {
        endIdx = totalCount;
    }
    
    bool first = true;
    for (int i = startIdx; i < endIdx; i++) {
        const auto& def = (*definitions_ptr)[i];
        
        // Calculer l'espace restant AVANT d'écrire la virgule
        size_t remaining = bufferSize - written - 1; // -1 pour le ']' final
        
        // Vérifier si on a assez de place pour au moins un objet minimal
        if (remaining < 50) {
            break; // Pas assez de place même pour un objet minimal
        }
        
        // Mémoriser si on va écrire une virgule (pour pouvoir l'annuler si besoin)
        bool wroteComma = false;
        if (!first) {
            buffer[written++] = ',';
            remaining--; // La virgule prend 1 caractère
            wroteComma = true;
        }
        first = false;
        
        // Écrire la définition en JSON
        int defLen = def.toJson(buffer + written, remaining);
        
        if (defLen <= 0 || defLen >= (int)remaining) {
            // Pas assez de place pour cet objet → annuler la virgule si on l'a écrite
            if (wroteComma && written > 1) {
                written--; // Retirer la virgule orpheline
            }
            break; // Arrêter proprement, JSON reste valide
        }
        
        written += defLen;
    }
    
    buffer[written++] = ']';
    buffer[written] = '\0';
    
    return written;
}
#endif

size_t ComponentRegistry::count() {
    if (definitions_ptr == nullptr) return 0;
    return definitions_ptr->size();
}

void ComponentRegistry::cleanup() {
    if (definitions_ptr != nullptr) {
        for (auto& def : *definitions_ptr) {
            def.cleanup();
        }
        definitions_ptr->clear();
        delete definitions_ptr;
        definitions_ptr = nullptr;
    }
    if (pending_definitions_ptr != nullptr) {
        for (auto& def : *pending_definitions_ptr) {
            def.cleanup();
        }
        pending_definitions_ptr->clear();
        delete pending_definitions_ptr;
        pending_definitions_ptr = nullptr;
    }
    initialized_ = false;
}
