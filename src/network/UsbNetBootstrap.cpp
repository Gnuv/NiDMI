#include "UsbNetBootstrap.h"

#if NIDMI_USB_NET

#include <nidmi_core.h>
#include "tusb.h"   // tud_suspended

namespace {

// Portee globale, imperativement : le constructeur enregistre le descripteur
// NCM aupres de TinyUSB, et TinyUSB assemble sa configuration une seule fois,
// a USB.begin(). NiDMI construit son USBMIDI puis appelle USB.begin() a
// l'execution (UsbMidiManager::begin) ; une instance globale est enregistree
// bien avant, pendant l'initialisation statique.
nidmi_core::UsbNetService g_usbNet;

bool g_started = false;

constexpr const char* IP_DU_LIEN     = "192.168.7.1";
constexpr const char* MASQUE_DU_LIEN = "255.255.255.0";

}  // namespace

namespace nidmi_usbnet {

bool enabled() {
  return true;
}

bool begin() {
  if (g_started) {
    return true;
  }
  if (!nidmi_core::UsbNetService::available()) {
    Serial.println("[UsbNet] cible sans USB-OTG : variant sans effet");
    return false;
  }

  nidmi_core::UsbNetConfig cfg;
  cfg.interfaceName = "NiDMI USB Network";
  // Sous-reseau distinct de l'AP WiFi (192.168.4.x) pour que les deux liens
  // puissent etre actifs en meme temps.
  cfg.ip = IP_DU_LIEN;
  cfg.netmask = MASQUE_DU_LIEN;
  cfg.dhcpServer = true;
  // Ni routeur ni DNS dans le bail : brancher l'instrument ne doit jamais
  // detourner la route par defaut de la machine hote.
  cfg.advertiseRouter = false;
  // usbnet_rx (12) sur le coeur 0, sous le MIDI (19) et les capteurs (20) :
  // hors du coeur de l'audio, a une place previsible (MESURES §149).
  cfg.rxCore = 0;

  g_started = g_usbNet.begin(cfg);
  if (!g_started) {
    Serial.printf("[UsbNet] begin() a echoue, step=%d\n", (int)g_usbNet.lastStep());
  }
  return g_started;
}

void update() {
  g_usbNet.update();
}

bool linkUp() {
  return g_usbNet.isLinkUp();
}

void compteurs(uint32_t& rx, uint32_t& txExpirees) {
  const nidmi_core::UsbNetStats s = g_usbNet.stats();
  rx = s.rxFrames;
  txExpirees = s.txTimeouts;
}

bool suspendu() {
  return tud_suspended();
}

bool hoteConnu() {
  return g_usbNet.hoteConnu();
}

bool sonder() {
  return g_usbNet.sonderHote();
}

bool relancer() {
  return g_usbNet.relancerEnumeration();
}

bool reseauActif() {
  return g_usbNet.reseauActif();
}

String ip() {
  return g_usbNet.localIp().toString();
}

bool parLeCable(const IPAddress& locale, const IPAddress& distante) {
  if (!g_started) return false;
  const uint32_t moi = g_usbNet.localIp();
  IPAddress masque;
  if (!moi || !masque.fromString(MASQUE_DU_LIEN)) return false;
  const uint32_t m = masque;
  return (uint32_t)locale == moi && (uint32_t)distante != moi &&
         ((uint32_t)distante & m) == (moi & m);
}

String broadcastAddress() {
  return g_usbNet.broadcastAddress();
}

String statusLine() {
  const nidmi_core::UsbNetStats s = g_usbNet.stats();
  String out = "lien ";
  out += g_usbNet.isLinkUp() ? "monte" : "bas";
  out += ", ip " + g_usbNet.localIp().toString();
  out += ", rx " + String(s.rxFrames) + " (drop " + String(s.rxDropped) + ")";
  out += ", tx " + String(s.txFrames) + " (timeout " + String(s.txTimeouts) + ")";
  return out;
}

String etatJson() {
  const nidmi_core::UsbNetStats s = g_usbNet.stats();
  String j = "{\"compile\":true,\"demarre\":";
  j += g_started ? "true" : "false";
  j += ",\"lien\":";
  j += g_usbNet.isLinkUp() ? "true" : "false";
  j += ",\"ip\":\"" + g_usbNet.localIp().toString() + "\"";
  j += ",\"etape\":" + String((int)g_usbNet.lastStep());
  j += ",\"rx\":" + String(s.rxFrames);
  j += ",\"rx_rejetees\":" + String(s.rxDropped);
  j += ",\"tx\":" + String(s.txFrames);
  j += ",\"tx_expirees\":" + String(s.txTimeouts);
  j += ",\"relance_en_cours\":";
  j += g_usbNet.relanceEnCours() ? "true" : "false";
  j += ",\"relances\":" + String(g_usbNet.relances());
  // L'hote utilise-t-il le reseau du cable ? Bus monte mais faux : il l'a
  // laisse desactive — ce que « Relancer le cable » resout (MESURES §154).
  j += ",\"reseau_actif\":";
  j += g_usbNet.reseauActif() ? "true" : "false";
  j += ",\"diag\":" + g_usbNet.diagJson() + "}";
  return j;
}

}  // namespace nidmi_usbnet

#else  // variant desactive

namespace nidmi_usbnet {

bool enabled() {
  return false;
}
bool begin() {
  return false;
}
void update() {}
bool linkUp() {
  return false;
}
void compteurs(uint32_t& rx, uint32_t& txExpirees) {
  rx = 0;
  txExpirees = 0;
}
bool suspendu() {
  return false;
}
bool hoteConnu() {
  return false;
}
bool sonder() {
  return false;
}
bool relancer() {
  return false;
}
bool reseauActif() {
  return false;
}
String ip() {
  return String("0.0.0.0");
}
bool parLeCable(const IPAddress&, const IPAddress&) {
  return false;
}
String broadcastAddress() {
  return String();
}
String statusLine() {
  return String("variant USB net non compile");
}
String etatJson() {
  return String("{\"compile\":false,\"lien\":false}");
}

}  // namespace nidmi_usbnet

#endif
