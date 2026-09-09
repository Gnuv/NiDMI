#pragma once

#include "../ComplexHandler.h"

/**
 * @file DacHandler.h
 * @brief Handler du DAC I2S — il ne « pilote » rien, il DÉCLARE.
 *
 * Son seul rôle est de répondre `isGpioUsed()` pour ses trois broches, ce qui
 * suffit à tout le reste : l'inventaire les montre prises, `/api/pins/set` les
 * refuse à un autre composant, et `AudioEngine::ensureStarted()` sait qu'un DAC
 * est déclaré. Aucun cas particulier n'est nécessaire ailleurs — c'est le même
 * mécanisme que le joystick ou le multiplexeur (MESURES.md §47).
 */
class DacHandler : public ComplexHandler {
public:
    DacHandler() : bck_(255), lrck_(255), din_(255) {}
    virtual ~DacHandler() = default;

    const char* getComponentId() const override { return "dac_i2s"; }

    bool addComponent(const ComplexComponentData& data) override;
    bool removeComponent(const char* pinLabel, uint8_t mainPinGpio) override;
    bool getComponentInfo(const char* pinLabel, uint8_t mainPinGpio, String& json) override;
    bool isGpioUsed(uint8_t gpio) const override;
    uint8_t getComponentCount() const override { return bck_ == 255 ? 0 : 1; }

    /** Vrai si un DAC est déclaré — donc si le moteur audio a le droit de démarrer. */
    bool declare() const { return bck_ != 255; }

private:
    uint8_t bck_, lrck_, din_;
};
