#pragma once

#include <Arduino.h>                 // SCK, MISO, MOSI: the variant's pinout
#include "../ComponentDefinition.h"
#include "../FormFieldHelpers.h"
#include "../MidiMessageCatalog.h"   // makeFlashDef

/**
 * @file SdSpiDef.h
 * @brief The SD card (SPI) — a peripheral you DECLARE, like the DAC.
 *
 * Same mechanism as `dac_i2s` (DacI2sDef.h): no declared card = no pin taken and no
 * SPI access; declaring it reserves its four pins (the handler answers `isGpioUsed`)
 * and the board mounts it, in its own task — neither the loop nor the web server
 * waits for it (src/config/SdCard.h).
 *
 * THE MAIN PIN IS THE CS (card select): it is the only one you choose, among those
 * nothing occupies. SCK, MISO and MOSI are the SPI bus of the card (D8, D9, D10 on
 * the XIAO S3): given as `defaultValue`, and imposed by the validation — the LIS3DH
 * in SPI mode plugs into it too, on its own CS.
 *
 * It sends neither MIDI nor OSC: no script (the app reads that in the definition).
 */

namespace Components {

struct SdSpi {
    static constexpr const char* ID           = "sd_spi";
    static constexpr const char* DISPLAY_NAME = "Carte SD (SPI)";
    static constexpr const char* FAMILY_NAME  = "Stockage";   // shown to the user: stays French

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

    /* The CS is any digital pin, except those of the bus. */
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
