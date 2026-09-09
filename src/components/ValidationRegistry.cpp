#include "../audio/AudioEngine.h"   // broches imposees du bus I2S
#include "ValidationRegistry.h"
#include "ComponentRegistry.h"
#include "../utils/PinMapper.h"
#include "../managers/MuxValidator.h"
#include "../managers/MuxManager.h"
#include "../managers/complex/ComplexHandler.h"

// Initialisation des membres statiques
ValidatorEntry ValidationRegistry::validators_[MAX_VALIDATORS] = {};
size_t ValidationRegistry::validatorCount_ = 0;
ComplexValidatorEntry ValidationRegistry::complexValidators_[MAX_COMPLEX_VALIDATORS] = {};
size_t ValidationRegistry::complexValidatorCount_ = 0;

// Fonctions de validation statiques (au lieu de lambdas)
// Note: Les validators pour composants simples (potentiomètre, bouton, LED, velostat)
// ont été supprimés car ils sont maintenant validés dynamiquement via ComponentDefinition::pinType

static bool validateMux(uint8_t gpio, const void* config) {
    if (config == nullptr) {
        // Validation basique : juste vérifier que c'est une pin ADC
        return PinMapper::hasAdc(gpio);
    }
    // Validation complète avec la config MUX
    const MuxConfig* muxCfg = static_cast<const MuxConfig*>(config);
    auto result = MuxValidator::validatePins(
        muxCfg->sig_pin, muxCfg->s0, muxCfg->s1, muxCfg->s2, muxCfg->s3, muxCfg->en_pin
    );
    return result.valid;
}

void ValidationRegistry::registerValidator(const char* componentId, ValidatorFunc validator) {
    if (validatorCount_ >= MAX_VALIDATORS) {
        return; // Tableau plein
    }
    // Utiliser directement les pointeurs de chaînes statiques (pas de std::string)
    validators_[validatorCount_].componentId = componentId;
    validators_[validatorCount_].validator = validator;
    validatorCount_++;
}

bool ValidationRegistry::validate(const char* componentId, uint8_t gpio, const void* config) {
    // Recherche linéaire (plus simple et sans std::string temporaire)
    // D'abord chercher un validator spécifique (pour cas spéciaux comme MUX)
    for (size_t i = 0; i < validatorCount_; i++) {
        if (strcmp(validators_[i].componentId, componentId) == 0) {
            return validators_[i].validator(gpio, config);
        }
    }
    
    // Pas de validator spécifique : validation dynamique basée sur ComponentDefinition
    const ComponentDefinition* def = ComponentRegistry::findById(componentId);
    if (def) {
        // Validation basée sur le type de pin requis
        switch (def->pinType) {
            case PinType::PIN_ANALOG:
                // Composant analogique : vérifier que le GPIO a un ADC
                return PinMapper::hasAdc(gpio);
            
            case PinType::PIN_DIGITAL:
                // Composant digital : vérifier que c'est un GPIO valide
                return gpio < 48;
            
            default:
                // Type inconnu : validation basique
                return gpio < 48;
        }
    }
    
    // Pas de définition trouvée = invalide par défaut
    return false;
}

bool ValidationRegistry::hasValidator(const char* componentId) {
    // Recherche linéaire (plus simple et sans std::string temporaire)
    for (size_t i = 0; i < validatorCount_; i++) {
        if (strcmp(validators_[i].componentId, componentId) == 0) {
            return true;
        }
    }
    return false;
}

// Fonction de validation pour composants complexes MUX
static ValidationResult validateMuxComplex(const ComplexComponentData& data) {
    ValidationResult result;
    result.valid = true;
    
    // Vérifier que la définition est présente
    if (!data.def) {
        result.valid = false;
        result.error_message = "Définition du composant manquante";
        return result;
    }
    
    // Extraire les pins additionnelles
    uint8_t s0 = 255, s1 = 255, s2 = 255, s3 = 255, en = 255;
    for (uint8_t i = 0; i < data.additionalPinCount; i++) {
        const char* pinId = data.additionalPins[i].id;
        uint8_t gpio = data.additionalPins[i].gpio;
        
        if (strcmp(pinId, "s0") == 0) s0 = gpio;
        else if (strcmp(pinId, "s1") == 0) s1 = gpio;
        else if (strcmp(pinId, "s2") == 0) s2 = gpio;
        else if (strcmp(pinId, "s3") == 0) s3 = gpio;
        else if (strcmp(pinId, "en") == 0) en = gpio;
    }
    
    // Valider les pins avec MuxValidator
    auto pinResult = MuxValidator::validatePins(
        data.mainPinGpio, s0, s1, s2, s3, en
    );
    if (!pinResult.valid) {
        // Convertir MuxValidator::ValidationResult vers ValidationResult
        result.valid = false;
        result.error_message = pinResult.error_message;
        return result;
    }
    
    // Vérifier que les pins requises sont présentes
    const char* componentId = data.def->id;
    bool isHc4051 = (componentId && strcmp(componentId, "hc4051") == 0);
    
    if (s0 == 255 || s1 == 255 || s2 == 255) {
        result.valid = false;
        result.error_message = "Pins S0, S1 et S2 sont requises";
        return result;
    }
    
    if (!isHc4051 && s3 == 255) {
        result.valid = false;
        result.error_message = "Pin S3 est requise pour HC4067";
        return result;
    }
    
    // Extraire et valider les seuils analogiques
    uint16_t analog_min = 0, analog_max = 4095;
    for (uint8_t i = 0; i < data.formFieldCount; i++) {
        const char* fieldId = data.formFields[i].id;
        String value = data.formFields[i].value;
        
        if (strcmp(fieldId, "muxMin") == 0 || strcmp(fieldId, "min") == 0) {
            analog_min = value.toInt();
        } else if (strcmp(fieldId, "muxMax") == 0 || strcmp(fieldId, "max") == 0) {
            analog_max = value.toInt();
        }
    }
    
    // Valider les seuils avec MuxValidator
    auto threshResult = MuxValidator::validateThresholds(analog_min, analog_max);
    if (!threshResult.valid) {
        // Convertir MuxValidator::ValidationResult vers ValidationResult
        result.valid = false;
        result.error_message = threshResult.error_message;
        return result;
    }
    
    return result; // Tout est valide
}

// Fonction de validation pour composant complexe Joystick
static ValidationResult validateJoystickComplex(const ComplexComponentData& data) {
    ValidationResult result;
    result.valid = true;
    
    // Vérifier que la définition est présente
    if (!data.def) {
        result.valid = false;
        result.error_message = "Définition du composant manquante";
        return result;
    }
    
    // Vérifier que le GPIO principal (X) est valide
    if (data.mainPinGpio >= 255) {
        result.valid = false;
        result.error_message = "GPIO principal (axe X) invalide";
        return result;
    }
    
    // Vérifier que le GPIO Y est présent et valide
    uint8_t yGpio = 255;
    for (uint8_t i = 0; i < data.additionalPinCount; i++) {
        const char* pinId = data.additionalPins[i].id;
        if (pinId && strcmp(pinId, "joyYPin") == 0) {
            yGpio = data.additionalPins[i].gpio;
            break;
        }
    }
    
    if (yGpio >= 255) {
        result.valid = false;
        result.error_message = "Pin axe Y (joyYPin) requise";
        return result;
    }
    
    // Vérifier que les GPIOs ont un ADC (analogique)
    if (!PinMapper::hasAdc(data.mainPinGpio)) {
        result.valid = false;
        result.error_message = "GPIO principal (axe X) doit être analogique";
        return result;
    }
    
    if (!PinMapper::hasAdc(yGpio)) {
        result.valid = false;
        result.error_message = "GPIO axe Y doit être analogique";
        return result;
    }
    
    return result; // Tout est valide
}

// Fonction de validation pour composant complexe Joystick 3 axes
static ValidationResult validateJoystick3Complex(const ComplexComponentData& data) {
    ValidationResult result;
    result.valid = true;

    // Vérifier que la définition est présente
    if (!data.def) {
        result.valid = false;
        result.error_message = "Définition du composant manquante";
        return result;
    }

    // Vérifier que le GPIO principal (X) est valide
    if (data.mainPinGpio >= 255) {
        result.valid = false;
        result.error_message = "GPIO principal (axe X) invalide";
        return result;
    }

    // Vérifier que les GPIO Y et Z sont présents et valides
    uint8_t yGpio = 255;
    uint8_t zGpio = 255;
    for (uint8_t i = 0; i < data.additionalPinCount; i++) {
        const char* pinId = data.additionalPins[i].id;
        if (pinId && strcmp(pinId, "joyYPin") == 0) {
            yGpio = data.additionalPins[i].gpio;
        } else if (pinId && strcmp(pinId, "joyZPin") == 0) {
            zGpio = data.additionalPins[i].gpio;
        }
    }

    if (yGpio >= 255) {
        result.valid = false;
        result.error_message = "Pin axe Y (joyYPin) requise";
        return result;
    }

    if (zGpio >= 255) {
        result.valid = false;
        result.error_message = "Pin axe Z (joyZPin) requise";
        return result;
    }

    // Vérifier que les GPIOs ont un ADC (analogique)
    if (!PinMapper::hasAdc(data.mainPinGpio)) {
        result.valid = false;
        result.error_message = "GPIO principal (axe X) doit être analogique";
        return result;
    }

    if (!PinMapper::hasAdc(yGpio)) {
        result.valid = false;
        result.error_message = "GPIO axe Y doit être analogique";
        return result;
    }

    if (!PinMapper::hasAdc(zGpio)) {
        result.valid = false;
        result.error_message = "GPIO axe Z doit être analogique";
        return result;
    }

    return result; // Tout est valide
}

bool ValidationRegistry::registerComplexValidator(const char* componentId, ComplexValidatorFunc validator) {
    if (complexValidatorCount_ >= MAX_COMPLEX_VALIDATORS) {
        return false; // Tableau plein
    }
    // Utiliser directement les pointeurs de chaînes statiques (pas de std::string)
    complexValidators_[complexValidatorCount_].componentId = componentId;
    complexValidators_[complexValidatorCount_].validator = validator;
    complexValidatorCount_++;
    return true;
}

ValidationResult ValidationRegistry::validateComplex(const char* componentId, const ComplexComponentData& data) {
    // Recherche linéaire
    for (size_t i = 0; i < complexValidatorCount_; i++) {
        if (strcmp(complexValidators_[i].componentId, componentId) == 0) {
            return complexValidators_[i].validator(data);
        }
    }
    // Pas de validator enregistré = invalide par défaut
    ValidationResult result;
    result.valid = false;
    result.error_message = "Aucun validator enregistré pour " + String(componentId);
    return result;
}

bool ValidationRegistry::hasComplexValidator(const char* componentId) {
    // Recherche linéaire
    for (size_t i = 0; i < complexValidatorCount_; i++) {
        if (strcmp(complexValidators_[i].componentId, componentId) == 0) {
            return true;
        }
    }
    return false;
}

/* Le DAC I2S : ses trois broches sont imposees par le peripherique, donc la
 * validation se resume a verifier qu'on lui a bien donne celles-la. Rien de
 * particulier au-dela : c'est un composant comme un autre, avec des broches
 * supplementaires (MESURES.md §47). */
static ValidationResult validateDacComplex(const ComplexComponentData& data) {
    ValidationResult result;
    if (!data.def) return ValidationResult(false, "Définition du composant manquante");
    if (data.mainPinGpio != (uint8_t)AudioEngine::PIN_BCLK)
        return ValidationResult(false, "Le DAC se déclare sur le BCK (GPIO "
                                       + String(AudioEngine::PIN_BCLK) + ")");
    uint8_t lrck = 255, din = 255;
    for (uint8_t i = 0; i < data.additionalPinCount; i++) {
        const char* id = data.additionalPins[i].id;
        if (!id) continue;
        if      (!strcmp(id, "dacLrck")) lrck = data.additionalPins[i].gpio;
        else if (!strcmp(id, "dacDin"))  din  = data.additionalPins[i].gpio;
    }
    if (lrck != (uint8_t)AudioEngine::PIN_LRCK)
        return ValidationResult(false, "LRCK doit être le GPIO " + String(AudioEngine::PIN_LRCK));
    if (din != (uint8_t)AudioEngine::PIN_DIN)
        return ValidationResult(false, "DIN doit être le GPIO " + String(AudioEngine::PIN_DIN));
    return result;
}

void ValidationRegistry::init() {
    // Note: Les composants simples (potentiomètre, bouton, LED, velostat) sont maintenant
    // validés dynamiquement via ComponentDefinition::pinType dans validate().
    // On n'enregistre plus de validators spécifiques pour eux.
    
    // MUX : validation complexe via MuxValidator (cas spécial avec config)
    // Gardé car il nécessite une validation spéciale avec la config MUX
    registerValidator("mux", validateMux);
    
    // Composants complexes : validation complète avec ComplexComponentData
    registerComplexValidator("hc4067", validateMuxComplex);
    registerComplexValidator("hc4051", validateMuxComplex);
    registerComplexValidator("joystick", validateJoystickComplex);
    registerComplexValidator("joystick3", validateJoystick3Complex);
    registerComplexValidator("dac_i2s", validateDacComplex);
}
