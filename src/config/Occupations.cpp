#include "Occupations.h"

#include <Preferences.h>

#include "../audio/AudioEngine.h"
#include "../utils/PinMapper.h"

namespace {

/* Copie en RAM des déclarations, relue par rafraichir(). On ne va PAS lire la
 * NVS à chaque interrogation : qui() est appelée pour chaque broche pendant un
 * rechargement, qui tient déjà la NVS ouverte. */
bool s_audio = false;
bool s_i2c = false, s_spi = false, s_uart = false;

bool cleExiste(Preferences& p, const char* cle) { return p.isKey(cle); }

}  // namespace

namespace Occupations {

void rafraichir() {
    {
        Preferences p;
        if (p.begin("nidmi-audio", true)) {
            const String choix = p.getString("moteur", "");
            p.end();
            s_audio = choix.length() && choix != "-1";
        }
    }
    /* L'I2S peut aussi tourner sans rien de mémorisé (choix fait à chaud). */
    if (AudioEngine::isStarted()) s_audio = true;

    Preferences p;
    if (p.begin("nidmi", true)) {
        s_i2c  = cleExiste(p, "pin_I2C");
        s_spi  = cleExiste(p, "pin_SPI");
        s_uart = cleExiste(p, "pin_TX") || cleExiste(p, "pin_RX");
        p.end();
    }
}

bool audioDeclare() { return s_audio; }

Qui qui(uint8_t gpio) {
    if (s_audio) {
        if (gpio == AudioEngine::PIN_BCLK) return { "audio", "bck"  };
        if (gpio == AudioEngine::PIN_LRCK) return { "audio", "lrck" };
        if (gpio == AudioEngine::PIN_DIN)  return { "audio", "din"  };
    }
    if (s_i2c) {
        if (gpio == PinMapper::labelToGpio("SDA")) return { "i2c", "sda" };
        if (gpio == PinMapper::labelToGpio("SCL")) return { "i2c", "scl" };
    }
    if (s_spi) {
        if (gpio == PinMapper::labelToGpio("MOSI")) return { "spi", "mosi" };
        if (gpio == PinMapper::labelToGpio("MISO")) return { "spi", "miso" };
        if (gpio == PinMapper::labelToGpio("SCK"))  return { "spi", "sck"  };
    }
    if (s_uart) {
        if (gpio == PinMapper::labelToGpio("TX")) return { "uart", "tx" };
        if (gpio == PinMapper::labelToGpio("RX")) return { "uart", "rx" };
    }
    return { nullptr, nullptr };
}

String json() {
    String j = "{";
    bool premier = true;
    auto ajouter = [&](const char* nom, const char* corps) {
        if (!premier) j += ",";
        j += "\""; j += nom; j += "\":"; j += corps;
        premier = false;
    };
    if (s_audio) {
        ajouter("audio", (String("{\"bck\":")  + String(AudioEngine::PIN_BCLK)
                        + ",\"lrck\":" + String(AudioEngine::PIN_LRCK)
                        + ",\"din\":"  + String(AudioEngine::PIN_DIN) + "}").c_str());
    }
    if (s_i2c) {
        ajouter("i2c", (String("{\"sda\":") + String(PinMapper::labelToGpio("SDA"))
                      + ",\"scl\":" + String(PinMapper::labelToGpio("SCL")) + "}").c_str());
    }
    if (s_spi) {
        ajouter("spi", (String("{\"mosi\":") + String(PinMapper::labelToGpio("MOSI"))
                      + ",\"miso\":" + String(PinMapper::labelToGpio("MISO"))
                      + ",\"sck\":"  + String(PinMapper::labelToGpio("SCK")) + "}").c_str());
    }
    if (s_uart) {
        ajouter("uart", (String("{\"tx\":") + String(PinMapper::labelToGpio("TX"))
                       + ",\"rx\":" + String(PinMapper::labelToGpio("RX")) + "}").c_str());
    }
    j += "}";
    return j;
}

}  // namespace Occupations
