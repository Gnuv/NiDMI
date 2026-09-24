/* UsbClassesAbsentes.c — les classes USB que la carte n'offre pas ne prennent
 * plus de place dans sa RAM.
 *
 * La bibliotheque TinyUSB du core Arduino est PRECOMPILEE avec toutes ses
 * classes (son sdkconfig : CDC, MSC, HID, MIDI, VIDEO, DFU, DFU runtime,
 * VENDOR, NCM, et la pile HOTE). Sa table de pilotes (usbd.c) les cite toutes :
 * l'editeur de liens embarquait donc chaque classe — et ses tampons STATIQUES —
 * sans qu'aucune interface ne l'annonce. Releve dans le .map (MESURES §150) :
 *
 *   NCM 6 472 o (hors firmware cable) · MSC 4 160 · DFU 4 106 · pile hote
 *   3 443 (usbh, hub, cdc/hid/msc hote, hcd_dwc2) · CDC 777 · video 153
 *
 * soit ~12,6 ko de RAM interne sur un firmware cable, ~19,1 ko sans : retires
 * au tas avant meme le demarrage. Or le plus gros bloc contigu est LE chiffre
 * qui decide (RESSOURCES_CARTE.md).
 *
 * Le remede : des pilotes VIDES, definis ici en symboles FAIBLES. La table de
 * usbd.c se resout sur eux, et l'editeur de liens n'extrait plus les vrais
 * pilotes de l'archive — il n'extrait un membre que pour un symbole encore NON
 * defini. Un pilote vide repond « pas mon interface » (open -> 0) : usbd passe
 * au suivant, comme le vrai pilote devant l'interface d'une autre classe.
 *
 * FAIBLES, et c'est ce qui rend le procede sur : le jour ou le code se sert
 * vraiment d'une de ces classes (USBMSC, un port serie USB, le cable reseau
 * NCM...), il appelle une fonction publique du vrai pilote (tud_msc_*,
 * tud_cdc_*, tud_network_*) que rien d'autre ne definit ; l'editeur de liens
 * extrait alors le vrai pilote, dont les definitions FORTES remplacent les
 * notres. Le firmware cable le montre : UsbNetService appelle tud_network_xmit,
 * le vrai pilote NCM est lie, nos netd_* s'effacent d'eux-memes.
 *
 * Restent liees : MIDI (la classe qu'on offre), HID et VENDOR — la
 * bibliotheque USB d'Arduino est liee en entier (--whole-archive) et ses objets
 * USBHID / USBVendor appellent leurs pilotes (584 o a eux deux ; les ecarter
 * demanderait de simuler leur API publique, ce qu'on ne fait pas).
 *
 * Verification, a chaque montee du core Arduino : le .map ne doit plus citer
 * msc_device, dfu_device, dfu_rt_device, cdc_device, video_device, usbh, hub,
 * cdc_host, hid_host, msc_host, hcd_dwc2 — ni ncm_device hors firmware cable.
 * Les prototypes viennent des en-tetes de TinyUSB : une signature qui change
 * casse la compilation ici, pas le fonctionnement sur la carte. */

#include "sdkconfig.h"

#if CONFIG_TINYUSB_ENABLED

#include "tusb.h"
#include "device/usbd_pvt.h"
#include "host/usbh_pvt.h"
#include "host/hcd.h"

#define VIDE __attribute__((weak))

/* Ce que usbd.c appelle sur chaque pilote de sa table. */
#define PILOTE_VIDE(p)                                                                            \
  VIDE void p##_init(void) {}                                                                     \
  VIDE bool p##_deinit(void) { return true; }                                                     \
  VIDE void p##_reset(uint8_t rhport) { (void)rhport; }                                           \
  VIDE uint16_t p##_open(uint8_t rhport, tusb_desc_interface_t const* itf, uint16_t max_len) {    \
    (void)rhport; (void)itf; (void)max_len;                                                       \
    return 0;                                                                                     \
  }                                                                                               \
  VIDE bool p##_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const* r) { \
    (void)rhport; (void)stage; (void)r;                                                           \
    return false;                                                                                 \
  }

/* ... et, pour ceux qui ont des points d'acces de donnees. */
#define TRANSFERT_VIDE(p)                                                                         \
  VIDE bool p##_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t res, uint32_t n) {         \
    (void)rhport; (void)ep_addr; (void)res; (void)n;                                              \
    return false;                                                                                 \
  }

#if CFG_TUD_CDC
PILOTE_VIDE(cdcd)
TRANSFERT_VIDE(cdcd)
#endif

#if CFG_TUD_MSC
PILOTE_VIDE(mscd)
TRANSFERT_VIDE(mscd)
#endif

#if CFG_TUD_VIDEO
PILOTE_VIDE(videod)
TRANSFERT_VIDE(videod)
#endif

#if CFG_TUD_DFU_RUNTIME
PILOTE_VIDE(dfu_rtd)
#endif

#if CFG_TUD_DFU
PILOTE_VIDE(dfu_moded)
#endif

#if CFG_TUD_NCM
PILOTE_VIDE(netd)
TRANSFERT_VIDE(netd)
#endif

/* La pile HOTE : tusb.c la cite (initialisation selon le role du port) et le
 * gestionnaire d'interruption du controleur aussi (dwc2_int_handler_wrap, qui
 * choisit selon le role fixe a l'allocation de l'interruption). Le port de la
 * carte est PERIPHERIQUE : aucune de ces fonctions n'est appelee. */
#if CFG_TUH_ENABLED
VIDE bool tuh_inited(void) { return false; }
VIDE bool tuh_rhport_init(uint8_t rhport, const tusb_rhport_init_t* rh_init) {
  (void)rhport; (void)rh_init;
  return false;
}
VIDE bool tuh_deinit(uint8_t rhport) {
  (void)rhport;
  return true;
}
VIDE bool usbh_edpt_claim(uint8_t dev_addr, uint8_t ep_addr) {
  (void)dev_addr; (void)ep_addr;
  return false;
}
VIDE bool usbh_edpt_release(uint8_t dev_addr, uint8_t ep_addr) {
  (void)dev_addr; (void)ep_addr;
  return false;
}
VIDE bool usbh_edpt_xfer_with_callback(uint8_t dev_addr, uint8_t ep_addr, uint8_t* buffer, uint16_t total_bytes,
                                       tuh_xfer_cb_t complete_cb, uintptr_t user_data) {
  (void)dev_addr; (void)ep_addr; (void)buffer; (void)total_bytes; (void)complete_cb; (void)user_data;
  return false;
}
VIDE void hcd_int_handler(uint8_t rhport, bool in_isr) {
  (void)rhport; (void)in_isr;
}
#endif

#endif  // CONFIG_TINYUSB_ENABLED
