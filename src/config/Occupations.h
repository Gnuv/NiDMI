#pragma once
/**
 * @file Occupations.h
 * @brief Qui occupe quelle broche — d'après ce qui est DÉCLARÉ, pas d'après le
 *        brochage théorique de la carte.
 *
 * Avant : /api/pins/caps annonçait i2c/spi/uart (puis audio) comme occupés en
 * permanence. Mesuré sur une carte réelle : DIX broches réservées, ZÉRO
 * réellement utilisée — aucun bus déclaré, aucun process audio mémorisé, donc
 * l'I2S jamais ouvert. L'app interdisait cinq broches parfaitement libres, et
 * en laissait trois ouvertes qui, elles, bloquaient la carte quand l'audio
 * tournait (MESURES.md §45).
 *
 * La règle, énoncée par l'utilisateur : « si ce n'est pas déclaré, les broches
 * sont libres ». C'est la même que partout ailleurs dans le projet — la carte
 * décrit ce qui EST, pas ce qui pourrait être.
 *
 * Une occupation vient donc d'une DÉCLARATION :
 *   - audio : un process est mémorisé (nidmi-audio/moteur) ou l'I2S tourne ;
 *   - i2c / spi / uart : une clé pin_I2C / pin_SPI / pin_TX / pin_RX existe.
 *
 * Et elle vaut dans les DEUX SENS : on refuse un composant sur une broche
 * occupée, et on refuse de démarrer un périphérique dont une broche porte un
 * composant. Sans la seconde moitié, poser un potentiomètre sur D0 puis choisir
 * un moteur audio ramène exactement le blocage du §45.
 */
#include <Arduino.h>

namespace Occupations {

struct Qui {
    const char* bus;   // "audio", "i2c", "spi", "uart" — nullptr si libre
    const char* role;  // "bck", "sda", "mosi", "tx"...  — nullptr si libre
};

/** Relit les déclarations depuis la NVS. À appeler avant tout rechargement. */
void rafraichir();

/** Qui occupe cette broche ? {nullptr, nullptr} si elle est libre. */
Qui qui(uint8_t gpio);

/** Vrai si un process audio est mémorisé (donc l'I2S s'ouvrira ou est ouvert). */
bool audioDeclare();

/** Le bloc "bus" de /api/pins/caps : UNIQUEMENT ce qui est déclaré. */
String json();

}  // namespace Occupations
