/**
 * Variant "USB net" : sert l'interface web de NiDMI par le cable USB (CDC-NCM),
 * en parallele de l'USB-MIDI, sur le meme connecteur.
 *
 * Compile uniquement quand NIDMI_USB_NET vaut 1 (voir ./scripts/nidmi.sh
 * --usb-net). Le firmware par defaut n'est pas modifie.
 *
 * L'UI elle-meme n'a rien de special a faire : AsyncWebServer ecoute sur
 * INADDR_ANY et web/js/websocket.js construit son URL depuis window.location.
 *
 * Implementation dans nidmi-core : nidmi_core::UsbNetService, docs/USB_NET.md.
 */
#pragma once

#include <Arduino.h>

#ifndef NIDMI_USB_NET
#define NIDMI_USB_NET 0
#endif

namespace nidmi_usbnet {

/** Vrai si le firmware a ete compile avec le variant USB net. */
bool enabled();

/**
 * A appeler APRES USB.begin() (donc apres MidiRouter::begin() qui initialise
 * l'USB-MIDI) et AVANT toute initialisation mDNS : le service met en place
 * esp_netif et la boucle d'evenements dont mdns_init() a besoin.
 *
 * Le descripteur NCM, lui, est enregistre bien plus tot : l'instance de
 * UsbNetService est globale et son constructeur s'en charge.
 */
bool begin();

/** A appeler dans nidmi_loop(). Non bloquant. */
void update();

/** Lien USB monte cote hote (interface de donnees activee). */
bool linkUp();

/** Adresse de l'ESP32 sur le lien, "0.0.0.0" si indisponible. */
String ip();

/** Adresse de diffusion du lien, pour l'OSC. Vide si indisponible. */
String broadcastAddress();

/** Resume d'etat, pour les logs de demarrage. */
String statusLine();

}  // namespace nidmi_usbnet
