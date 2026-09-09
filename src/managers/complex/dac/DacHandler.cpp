#include "DacHandler.h"

#include <cstring>

bool DacHandler::addComponent(const ComplexComponentData& data) {
    bck_ = data.mainPinGpio;
    lrck_ = din_ = 255;
    for (uint8_t i = 0; i < data.additionalPinCount; i++) {
        const char* id = data.additionalPins[i].id;
        if (!id) continue;
        if      (!strcmp(id, "dacLrck")) lrck_ = data.additionalPins[i].gpio;
        else if (!strcmp(id, "dacDin"))  din_  = data.additionalPins[i].gpio;
    }
    Serial.printf("[DAC] declare : BCK=%u LRCK=%u DIN=%u\n",
                  (unsigned)bck_, (unsigned)lrck_, (unsigned)din_);
    return true;
}

bool DacHandler::removeComponent(const char*, uint8_t) {
    Serial.println("[DAC] retire : les broches sont liberees, le son est coupe");
    bck_ = lrck_ = din_ = 255;
    return true;
}

bool DacHandler::getComponentInfo(const char*, uint8_t, String& json) {
    if (bck_ == 255) return false;
    json = "\"additionalPins\":{\"dacLrck\":" + String(lrck_)
         + ",\"dacDin\":" + String(din_) + "},";
    return true;
}

bool DacHandler::isGpioUsed(uint8_t gpio) const {
    if (bck_ == 255) return false;
    return gpio == bck_ || gpio == lrck_ || gpio == din_;
}
