#include "SdHandler.h"

#include <cstring>

#include "../../../components/storage/SdSpiDef.h"
#include "../../../config/CarteSd.h"

bool SdHandler::addComponent(const ComplexComponentData& data) {
    cs_ = data.mainPinGpio;
    sck_  = Components::SdSpi::BUS_SCK;
    miso_ = Components::SdSpi::BUS_MISO;
    mosi_ = Components::SdSpi::BUS_MOSI;
    for (uint8_t i = 0; i < data.additionalPinCount; i++) {
        const char* id = data.additionalPins[i].id;
        if (!id) continue;
        if      (!strcmp(id, "sdSck"))  sck_  = data.additionalPins[i].gpio;
        else if (!strcmp(id, "sdMiso")) miso_ = data.additionalPins[i].gpio;
        else if (!strcmp(id, "sdMosi")) mosi_ = data.additionalPins[i].gpio;
    }
    Serial.printf("[SD] declaree : CS=%u SCK=%u MISO=%u MOSI=%u\n",
                  (unsigned)cs_, (unsigned)sck_, (unsigned)miso_, (unsigned)mosi_);
    CarteSd::declarer(cs_, sck_, miso_, mosi_);     // monte dans SA tache : on ne l'attend pas
    return true;
}

bool SdHandler::removeComponent(const char*, uint8_t) {
    Serial.println("[SD] retiree : les broches sont liberees");
    cs_ = sck_ = miso_ = mosi_ = 255;
    CarteSd::retirer();
    return true;
}

bool SdHandler::getComponentInfo(const char*, uint8_t, String& json) {
    if (cs_ == 255) return false;
    json = "\"additionalPins\":{\"sdSck\":" + String(sck_)
         + ",\"sdMiso\":" + String(miso_)
         + ",\"sdMosi\":" + String(mosi_) + "},";
    return true;
}

bool SdHandler::isGpioUsed(uint8_t gpio) const {
    if (cs_ == 255) return false;
    return gpio == cs_ || gpio == sck_ || gpio == miso_ || gpio == mosi_;
}
