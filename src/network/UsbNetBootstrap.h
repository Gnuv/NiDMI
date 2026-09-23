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

/* ── USB SEUL : le cable OU le WiFi, pas les deux ──────────────────────────
 * Mesure (MESURES.md §140) : faire tourner les DEUX coute 8 192 o de bloc
 * contigu et double la gigue MIDI. Or on n'a pas besoin des deux acces en
 * meme temps — c'est l'un ou l'autre. Ce drapeau n'allume pas la radio WiFi
 * au demarrage ; le serveur web, lui, sert sur le netif USB (AsyncWebServer
 * ecoute sur INADDR_ANY).
 *
 * IL PORTE SON REPLI, et ce n'est pas optionnel : une carte qui demarre radio
 * eteinte sur un lien USB qui ne monte pas n'est joignable QUE par le bouton
 * BOOT. Si le lien n'est pas monte au bout de NIDMI_USB_SEUL_REPLI_MS, la
 * radio s'allume. Cas reels que le repli couvre : cable sur un simple chargeur,
 * hote qui n'active jamais l'interface de donnees, descripteur refuse.
 *
 * « Une fonction qui peut enfermer doit porter sa sortie » — regle 7. */
#ifndef NIDMI_USB_SEUL
#define NIDMI_USB_SEUL 0
#endif
#ifndef NIDMI_USB_SEUL_REPLI_MS
#define NIDMI_USB_SEUL_REPLI_MS 20000
#endif

#if NIDMI_USB_SEUL && !NIDMI_USB_NET
#error "NIDMI_USB_SEUL sans NIDMI_USB_NET : la carte n'aurait AUCUN acces reseau."
#endif

/* ── NIDMI_USB_SEUL EST CONDAMNE TANT QU'IL N'EST PAS CORRIGE ─────────────
 * Flashe le 23/09 : la carte n'a JAMAIS demarre. Ni sur le bus USB, ni en
 * WiFi — recuperee au bouton BOOT, et la recuperation a efface la flash
 * entiere (MESURES §142).
 *
 * Cause tres probable : MDNS.begin() s'execute dans serverCore.begin(), bien
 * AVANT nidmi_usbnet::begin() — celui qui pose esp_netif et la boucle
 * d'evenements. Avec le WiFi, c'est son init qui les posait avant. Sans lui,
 * personne : mdns_init() part sur du vide, au demarrage. La doc du spike le
 * disait (« mDNS APRES usbNet.begin() ») ; notre ordre d'appels est l'inverse.
 *
 * Et le repli ne pouvait rien : il vit dans nidmi_loop(), que setup() n'atteint
 * jamais. UNE SORTIE POSEE APRES LA PORTE QUI SE FERME N'EST PAS UNE SORTIE.
 *
 * Avant de lever cette garde : (1) remettre esp_netif avant le mDNS dans ce
 * mode, (2) poser le repli au DEMARRAGE — compteur en NVS comme le garde-fou
 * audio, pas dans la boucle — (3) le voir tourner sur une carte dont la flash
 * a d'abord ete SAUVEGARDEE (esptool read-flash marche en mode download). */
#if NIDMI_USB_SEUL
#error "NIDMI_USB_SEUL met la carte hors service au demarrage (MESURES §142). Ne pas lever sans lire ce commentaire."
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
