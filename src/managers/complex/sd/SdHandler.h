#pragma once

#include "../ComplexHandler.h"

/**
 * @file SdHandler.h
 * @brief Handler de la carte SD (SPI) — comme le DAC, il DÉCLARE.
 *
 * `isGpioUsed()` répond pour ses quatre broches (CS, SCK, MISO, MOSI) : l'inventaire
 * les montre prises et `/api/pins/set` les refuse à un autre composant. Rien d'autre
 * à la main : monter la carte est l'affaire de `CarteSd` (sa tâche, jamais celle de
 * la requête qui déclare ni la boucle).
 */
class SdHandler : public ComplexHandler {
public:
    SdHandler() : cs_(255), sck_(255), miso_(255), mosi_(255) {}
    virtual ~SdHandler() = default;

    const char* getComponentId() const override { return "sd_spi"; }

    bool addComponent(const ComplexComponentData& data) override;
    bool removeComponent(const char* pinLabel, uint8_t mainPinGpio) override;
    bool getComponentInfo(const char* pinLabel, uint8_t mainPinGpio, String& json) override;
    bool isGpioUsed(uint8_t gpio) const override;
    uint8_t getComponentCount() const override { return cs_ == 255 ? 0 : 1; }

private:
    uint8_t cs_, sck_, miso_, mosi_;
};
