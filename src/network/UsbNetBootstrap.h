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
 * se coupe ensuite, en marche, par la bascule « cable prioritaire » de
 * nidmi_loop() (NiDMI.cpp) — et seulement sur une PREUVE DE VIE du cable :
 * des trames recues de l'hote, pas linkUp(). Un redemarrage ramene TOUJOURS
 * le WiFi. On ne peut pas s'enfermer dehors. */

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

/** USB configure par l'hote (tud_mounted). PAS une preuve de vie : vu vrai
 *  sur un lien mort (§140, §143). La preuve de vie, ce sont les compteurs. */
bool linkUp();

/** Les compteurs de la preuve de vie : trames recues de l'hote, emissions
 *  expirees. Zeros sans le variant USB net. */
void compteurs(uint32_t& rx, uint32_t& txExpirees);

/** Bus USB en veille : hote endormi — ou cable debranche, qui se voit ainsi
 *  faute de detection de VBUS sur la XIAO. Faux sans le variant. */
bool suspendu();

/** L'hote a un bail de notre serveur DHCP : on sait qui sonder. */
bool hoteConnu();

/** Une requete ARP a l'hote : sa reponse fait bouger `rx`. Au repos, un Mac
 *  se tait jusqu'a une minute (MESURES §148) ; la preuve de vie se PROVOQUE.
 *  Faux si rien n'est parti. */
bool sonder();

/** Refait l'enumeration USB (deconnexion puis reconnexion 500 ms plus tard) :
 *  quand l'hote laisse le cable « inactive » sans jamais le reactiver
 *  (MESURES §153). Coupe AUSSI le MIDI USB une a deux secondes — un geste
 *  manuel (Reglages → Carte → Reseau), jamais automatique. A appeler depuis
 *  nidmi_loop(). Faux sans le variant, ou si une relance est en cours. */
bool relancer();

/** Adresse de l'ESP32 sur le lien, "0.0.0.0" si indisponible. */
String ip();

/** Adresse de diffusion du lien, pour l'OSC. Vide si indisponible. */
String broadcastAddress();

/** Resume d'etat, pour les logs de demarrage. */
String statusLine();

/** Etat du lien et ses compteurs, en JSON — pour /api/reseau/liens.
 *  Le lien a deja ete vu MOURIR sous charge sans que l'hote s'en apercoive
 *  (MESURES §140) : sans ces compteurs, on ne saurait pas dire comment.
 *  Porte aussi, sous "diag", le releve du controleur USB qui a trouve
 *  pourquoi (MESURES §147) — lu par lire-diag-usb.py. */
String etatJson();

}  // namespace nidmi_usbnet
