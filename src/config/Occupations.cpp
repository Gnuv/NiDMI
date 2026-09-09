#include "Occupations.h"

#include <Preferences.h>

#include "../audio/AudioEngine.h"
#include "../utils/PinMapper.h"
#include "../managers/ComponentManager.h"
#include "../managers/complex/ComplexHandlerRegistry.h"
#include "../Globals.h"

namespace {

/* Copie en RAM des déclarations, relue par rafraichir(). On ne va PAS lire la
 * NVS à chaque interrogation : qui() est appelée pour chaque broche pendant un
 * rechargement, qui tient déjà la NVS ouverte. */
bool s_audio = false;
bool s_i2c = false, s_spi = false, s_uart = false;

bool cleExiste(Preferences& p, const char* cle) { return p.isKey(cle); }

}  // namespace

namespace Occupations {

void relacher(const char* pinLabel, uint8_t gpio, const char* role) {
    if (!role || !*role) return;
    rafraichir();                       // partir de l'etat reel, pas d'un cache
    const bool audioAvant = audioDeclare();

    ComplexHandler* h = ComplexHandlerRegistry::getHandler(role);
    if (h) {
        h->removeComponent(pinLabel ? pinLabel : "", gpio);
        Serial.printf("[Occupations] %s relache sur %s : ses broches sont libres\n",
                      role, pinLabel ? pinLabel : "?");
    }
    rafraichir();

    /* Perdre le DAC, c'est perdre le son : l'I2S tourne sur des broches qui ne
     * lui appartiennent plus. On coupe, et on ne le rearme pas au demarrage —
     * sans DAC declare il echouerait de toute facon. */
    if (audioAvant && !audioDeclare() && AudioEngine::isStarted()) {
        Serial.println("[Occupations] plus de DAC declare — le moteur audio est arrete");
        AudioEngine::setEngine(-1, /*persister=*/true);
        AudioEngine::arreter();     // et FERMER l'I2S : sinon le son continue
    }
}

void rafraichir() {
    /* L'audio occupe ses broches quand le DAC est DÉCLARÉ — plus quand un
     * moteur se trouve mémorisé. C'est ce que l'utilisateur voit et manipule
     * dans l'inventaire, et c'est la même règle que pour tout le reste : pas de
     * déclaration, pas d'occupation, et les broches sont libres. */
    {
        ComplexHandler* h = ComplexHandlerRegistry::getHandler("dac_i2s");
        s_audio = h && h->getComponentCount() > 0;
    }

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

bool prisParUnComposant(uint8_t gpio) {
    /* Les composants multi-broches savent DEJA repondre : chaque ComplexHandler
     * implemente isGpioUsed(). On leur demande plutot que de tenir un second
     * inventaire qui divergerait. */
    for (uint8_t i = 0; i < ComplexHandlerRegistry::getHandlerCount(); i++) {
        ComplexHandler* h = ComplexHandlerRegistry::getHandlerByIndex(i);
        if (h && h->isGpioUsed(gpio)) return true;
    }
    return g_componentManager.findComponentByGpio(gpio) != 255;
}

bool tenuCommeSupplementaire(uint8_t gpio) {
    /* Un handler l'utilise, mais ce n'est la broche PRINCIPALE d'aucun
     * composant : c'est donc une broche supplementaire. findComponentByGpio()
     * ne connait que les principales — la difference suffit. */
    if (g_componentManager.findComponentByGpio(gpio) != 255) return false;
    for (uint8_t i = 0; i < ComplexHandlerRegistry::getHandlerCount(); i++) {
        ComplexHandler* h = ComplexHandlerRegistry::getHandlerByIndex(i);
        if (h && h->isGpioUsed(gpio)) return true;
    }
    return false;
}

bool libre(uint8_t gpio) {
    return qui(gpio).bus == nullptr && !prisParUnComposant(gpio);
}

static bool convient(uint8_t gpio, PinType type) {
    switch (type) {
        case PinType::PIN_ANALOG:            return PinMapper::hasAdc(gpio);
        case PinType::PIN_ANALOG_OR_DIGITAL: return true;
        case PinType::PIN_DIGITAL:
        case PinType::PIN_PWM:               return true;   // universelles sur ces cartes
        default:                             return true;
    }
}

uint8_t premiereLibre(PinType type, uint8_t apres) {
    PinMapper::detectMcu();
    const PinMapping* m = PinMapper::getAllMappings();
    const size_t n = PinMapper::getMappingCount();
    /* Deux passes : d'abord APRES la broche principale (l'ordre de la
     * serigraphie est celui du bornier, donc celui du cablage), puis depuis le
     * debut — mieux vaut une broche avant qu'un refus. */
    for (int passe = 0; passe < 2; passe++) {
        for (size_t i = 0; i < n; i++) {
            const uint8_t g = m[i].gpio;
            if (g == 255 || g > 48) continue;
            if (passe == 0 && g <= apres) continue;
            if (passe == 1 && g > apres) continue;
            if (!convient(g, type)) continue;
            if (!libre(g)) continue;
            return g;
        }
    }
    return 255;
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
