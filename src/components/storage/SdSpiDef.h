#pragma once

#include <Arduino.h>                 // SCK, MISO, MOSI : le brochage de la variante
#include "../ComponentDefinition.h"
#include "../FormFieldHelpers.h"
#include "../MidiMessageCatalog.h"   // makeFlashDef

/**
 * @file SdSpiDef.h
 * @brief La carte SD (SPI) — un périphérique qu'on DÉCLARE, comme le DAC.
 *
 * Même mécanisme que `dac_i2s` (DacI2sDef.h) : pas de carte déclarée = aucune
 * broche prise et aucun accès SPI ; la déclarer réserve ses quatre broches (le
 * handler répond `isGpioUsed`) et la carte la monte, dans sa propre tâche — ni
 * la boucle ni le serveur web n'attendent après elle (src/config/CarteSd.h).
 *
 * LA BROCHE PRINCIPALE EST LE CS (sélection de la carte) : c'est la seule qu'on
 * choisit, parmi celles que rien n'occupe. SCK, MISO et MOSI sont celles du bus
 * SPI de la carte (D8, D9, D10 sur la XIAO S3) : données en `defaultValue`, et
 * imposées par la validation — le LIS3DH en SPI s'y branche aussi, sur son propre CS.
 *
 * Elle n'émet ni MIDI ni OSC : pas de script (l'app le lit dans la définition).
 */

namespace Components {

struct SdSpi {
    static constexpr const char* ID           = "sd_spi";
    static constexpr const char* DISPLAY_NAME = "Carte SD (SPI)";
    static constexpr const char* FAMILY_NAME  = "Stockage";

    static constexpr ComponentFamily FAMILY = ComponentFamily::BASIC;
    static constexpr ComponentType   TYPE   = ComponentType::SD_SPI;
    static constexpr PinType     PIN_TYPE   = PinType::PIN_DIGITAL;
    static constexpr bool IMPLEMENTED    = true;
    static constexpr bool SUPPORTS_MIDI  = false;
    static constexpr bool SUPPORTS_OSC   = false;

    /* Le bus SPI de la carte, tel que le brochage de la variante le donne
     * (pins_arduino.h : SCK, MISO, MOSI) — jamais recopié ici en nombres. */
    static constexpr uint8_t BUS_SCK  = (uint8_t)SCK;
    static constexpr uint8_t BUS_MISO = (uint8_t)MISO;
    static constexpr uint8_t BUS_MOSI = (uint8_t)MOSI;

    /* Le CS est n'importe quelle broche numérique, sauf celles du bus. */
    static bool validate(uint8_t gpio) {
        return gpio < 48 && gpio != BUS_SCK && gpio != BUS_MISO && gpio != BUS_MOSI;
    }

    static ComponentDefinition createDefinition() {
        static constexpr FormFieldDef FF[] = {
            makeInfoField("sdInfo",
                "Carte SD sur le bus SPI. Ce composant se pose sur la broche CS "
                "(sélection de la carte) ; SCK, MISO et MOSI sont celles du bus SPI, "
                "réservées d'office. Les sons du dossier /samples de la carte sont lus "
                "au démarrage, comme ceux de la mémoire interne — la carte se remplit "
                "depuis un ordinateur. La brancher hors tension."),
        };
        static constexpr AdditionalPinDef AP[] = {
            {"sdSck",  "SCK (horloge)",  PinType::PIN_DIGITAL, false, BUS_SCK},
            {"sdMiso", "MISO (sortie carte)", PinType::PIN_DIGITAL, false, BUS_MISO},
            {"sdMosi", "MOSI (entrée carte)", PinType::PIN_DIGITAL, false, BUS_MOSI},
        };
        return makeFlashDef(ID, DISPLAY_NAME, "cardSd", FAMILY, FAMILY_NAME, TYPE, PIN_TYPE,
                            SUPPORTS_MIDI, SUPPORTS_OSC, IMPLEMENTED, FF, 1, nullptr, 0,
                            nullptr, AP, 3);
    }
};

}  // namespace Components
