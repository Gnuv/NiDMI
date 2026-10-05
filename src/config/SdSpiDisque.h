#pragma once
// src/config/SdSpiDisque.h — NOTRE PILOTE DE CARTE SD EN SPI, en lecture seule.
//
// Pourquoi pas la bibliotheque SD d'Arduino : elle exige que CMD8 renvoie l'echo
// 0x1AA, et abandonne sinon (`goto unknown_card`), sans dire pourquoi — la carte n'a
// pas de port serie. La carte de l'usager s'initialise parfaitement (CMD0, OCR,
// ACMD41 : trace brute au §204) mais repond a CMD8 par `01 00 00 00 00` ; un lecteur
// de bureau l'accepte, la bibliotheque non. Ici : CMD8 est un renseignement, pas une
// condition, et chaque echec dit a quelle etape il a eu lieu.
//
// LE PILOTE VA PLUS LOIN QUE LE MONTAGE : c'est lui qui lira les sons en flux.
// Lectures multi-blocs (CMD18) d'un seul tenant, CRC16 des blocs verifie (compte,
// ne rejette pas : une carte qui n'envoie pas de CRC valide se voit dans les
// compteurs), relecture d'un bloc en echec. Rien n'y ecrit : FatFs voit un support
// protege.
//
// Il se branche sur FatFs comme le fait la bibliotheque (ff_diskio_register, puis
// esp_vfs_fat_register) : les fichiers s'ouvrent par l'API `File` d'Arduino, sous
// le point de montage.
#include <Arduino.h>
#include <FS.h>
#include <SPI.h>

namespace SdSpiDisque {

/* Initialise la carte (400 kHz), la monte sous `point` (« /sd ») et passe a `hz`.
 * Faux : `raison` dit l'etape qui a echoue. A appeler depuis UNE tache a elle
 * (CarteSd) : l'initialisation attend la carte, jusqu'a 1,5 s. */
bool monter(SPIClass& spi, uint8_t cs, uint32_t hz, const char* point, String& raison);
void demonter();

bool        montee();
uint64_t    capacite();          // en octets
const char* type();              // « SD », « SDHC/SDXC »
uint32_t    echoCmd8();          // ce que CMD8 a rendu (0x1AA : une carte conforme)

File ouvrir(const char* chemin);                 // en lecture ; vide si rien n'est monte

// Compteurs, depuis le montage.
uint32_t lectures();             // blocs de 512 o lus
uint32_t erreursCrc();           // CRC16 qui ne correspond pas (donnee gardee quand meme)
uint32_t relectures();           // blocs relus apres un echec

}  // namespace SdSpiDisque
