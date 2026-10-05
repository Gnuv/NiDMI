#pragma once

#include "../ComplexHandler.h"

/**
 * @file SdHandler.h
 * @brief Handler of the SD card (SPI) — like the DAC, it DECLARES.
 *
 * `isGpioUsed()` answers for its four pins (CS, SCK, MISO, MOSI): the inventory shows
 * them taken and `/api/pins/set` refuses them to another component. Nothing else by
 * hand: mounting the card is `SdCard`'s business (its own task, never the declaring
 * request's nor the loop's).
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
