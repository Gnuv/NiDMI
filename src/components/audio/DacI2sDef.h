#pragma once

#include "../ComponentDefinition.h"
#include "../FormFieldHelpers.h"
#include "../../audio/AudioEngine.h"

/**
 * @file DacI2sDef.h
 * @brief Le DAC audio (I2S) — un périphérique qu'on DÉCLARE, comme les autres.
 *
 * Pourquoi il est ici et pas codé en dur ailleurs : tant que le bus audio était
 * une occupation implicite, l'app croyait D0/D1/D2 libres, on y posait un
 * capteur, et sonder le BCK bloquait la carte définitivement (MESURES.md §45).
 * Le remède n'est pas un cas particulier — c'est que le DAC se déclare comme un
 * composant, avec ses broches supplémentaires, dans le même mécanisme que le
 * joystick ou le multiplexeur (§47).
 *
 * Conséquence, voulue : pas de DAC déclaré = pas d'audio, et les trois broches
 * sont libres pour un bouton ou un potentiomètre.
 *
 * Les broches ne sont pas négociables — elles sont fixées par le périphérique
 * I2S du S3 : BCK, LRCK et DIN. Elles sont donc données en `defaultValue`, et
 * l'attribution automatique les respecte quand elles sont libres.
 */

namespace Components {

struct DacI2s {
    static constexpr const char* ID           = "dac_i2s";
    static constexpr const char* DISPLAY_NAME = "DAC audio (I2S)";
    static constexpr const char* FAMILY_NAME  = "Audio";

    static constexpr ComponentFamily FAMILY = ComponentFamily::BASIC;
    static constexpr ComponentType   TYPE   = ComponentType::DAC_I2S;
    static constexpr PinType     PIN_TYPE   = PinType::PIN_DIGITAL;
    static constexpr bool IMPLEMENTED    = true;
    static constexpr bool SUPPORTS_MIDI  = false;
    static constexpr bool SUPPORTS_OSC   = false;

    /* Le BCK est impose par le peripherique : le DAC ne se declare que la. */
    static bool validate(uint8_t gpio) {
        return gpio == (uint8_t)AudioEngine::PIN_BCLK;
    }

    static ComponentDefinition createDefinition() {
        static constexpr FormFieldDef FF[] = {
            makeInfoField("dacInfo",
                "Sortie audio I2S. Declarer ce peripherique reserve ses trois "
                "broches et autorise le moteur audio ; le retirer libere les "
                "broches et coupe le son."),
        };
        static constexpr AdditionalPinDef AP[] = {
            {"dacLrck", "LRCK (horloge de mot)", PinType::PIN_DIGITAL, false, (uint8_t)AudioEngine::PIN_LRCK},
            {"dacDin",  "DIN (donnees)",         PinType::PIN_DIGITAL, false, (uint8_t)AudioEngine::PIN_DIN},
        };
        return makeFlashDef(ID, DISPLAY_NAME, "cardDac", FAMILY, FAMILY_NAME, TYPE, PIN_TYPE,
                            SUPPORTS_MIDI, SUPPORTS_OSC, IMPLEMENTED, FF, 1, nullptr, 0,
                            nullptr, AP, 2);
    }
};

}  // namespace Components
