#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/* LE SURVEILLANT DE LA FLASH (MESURES §161).
 *
 * Toute operation du pilote de la flash — une simple lecture NVS ou LittleFS
 * comprise — coupe le cache des DEUX coeurs, gare l'autre coeur (ipc0/ipc1) et
 * retient les interruptions ordinaires du sien : celle du DMA audio aussi.
 * Mesure : un bloc audio de 103 ms, pendant lequel ipc0 a tourne 84 ms — une
 * operation en flash lancee depuis le coeur de l'audio.
 *
 * Un crochet autour de chaque operation (les « os functions » du pilote) :
 * combien, combien de temps en tout, la plus longue, et quelle tache l'a
 * lancee. Le surveillant de l'audio le releve a chaque quart de seconde et le
 * dit avec chaque bloc lent. Les crochets s'executent cache coupe : IRAM, et
 * rien que de la RAM interne. */
namespace SurveillantFlash {

struct Releve {
  uint32_t operations = 0;
  uint32_t totalUs = 0;
  uint32_t pireUs = 0;
  TaskHandle_t pireTache = nullptr;
  uint32_t pireAttenteUs = 0;              // attente du coeur voisin avant l'operation
  TaskHandle_t pireAttenteTache = nullptr;
};

/** Pose les crochets. Une fois, au demarrage. */
void installer();

/** Ce qui s'est passe depuis le releve precedent, et remise a zero. */
Releve releverEtRaz();

}  // namespace SurveillantFlash
