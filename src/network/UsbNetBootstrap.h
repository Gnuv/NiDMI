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

/* ── LE CABLE OU LE WIFI : ON COUPE EN MARCHE, JAMAIS AU DEMARRAGE ────────
 * Une premiere version (NIDMI_USB_SEUL, retiree) n'allumait pas la radio au
 * demarrage. La carte n'a jamais demarre : le serveur et le mDNS avaient
 * besoin d'une pile reseau que plus personne ne posait, et son repli vivait
 * dans une boucle que setup() n'atteignait jamais (MESURES §142).
 *
 * Desormais le demarrage est TOUJOURS celui qui marche, radio allumee. Le WiFi
 * se coupe ensuite, sur commande (POST /api/reseau/wifi), et :
 *   - la commande est REFUSEE si aucun lien USB ne peut prendre le relais ;
 *   - si le lien USB tombe ensuite 20 s d'affilee, la radio se rallume ;
 *   - rien n'est memorise : un redemarrage ramene TOUJOURS le WiFi.
 * On ne peut pas s'enfermer dehors : le pire cas est un redemarrage. */

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

/** Etat du lien et ses compteurs, en JSON — pour /api/reseau/liens.
 *  Le lien a deja ete vu MOURIR sous charge sans que l'hote s'en apercoive
 *  (MESURES §140) : sans ces compteurs, on ne saurait pas dire comment. */
String etatJson();

}  // namespace nidmi_usbnet
