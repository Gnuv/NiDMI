#include "UsbNetBootstrap.h"

#if NIDMI_USB_NET

#include <nidmi_core.h>

namespace {

// Portee globale, imperativement : le constructeur enregistre le descripteur
// NCM aupres de TinyUSB, et TinyUSB assemble sa configuration une seule fois,
// a USB.begin(). NiDMI construit son USBMIDI puis appelle USB.begin() a
// l'execution (UsbMidiManager::begin) ; une instance globale est enregistree
// bien avant, pendant l'initialisation statique.
nidmi_core::UsbNetService g_usbNet;

bool g_started = false;

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
  cfg.ip = "192.168.7.1";
  cfg.netmask = "255.255.255.0";
  cfg.dhcpServer = true;
  // Ni routeur ni DNS dans le bail : brancher l'instrument ne doit jamais
  // detourner la route par defaut de la machine hote.
  cfg.advertiseRouter = false;

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

String ip() {
  return g_usbNet.localIp().toString();
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
String ip() {
  return String("0.0.0.0");
}
String broadcastAddress() {
  return String();
}
String statusLine() {
  return String("variant USB net non compile");
}

}  // namespace nidmi_usbnet

#endif
