// ScriptStore — les scripts .nms sur la partition mapfs.
//
// La partition s'appelle « mapfs » et la table la decrit comme « scripts de
// mapping (1MB) » : c'est sa raison d'etre. Elle n'avait servi jusqu'ici qu'aux
// echantillons audio, ce qui inversait la hierarchie du systeme.
//
// Le moteur de script est le COEUR du boitier, pas un accessoire de l'audio :
// une carte peut etre configuree uniquement en .nms + cues, pour piloter des
// capteurs et des actionneurs, sans le moindre moteur audio. Ce magasin
// appartient donc a l'image de base, au meme titre que MappingEngine.
//
// Repartition assumee :
//   CONTENU des scripts  -> LittleFS (ce sont des fichiers, ils peuvent grossir)
//   NOM du script actif  -> NVS (une chaine courte, comme le choix de moteur)
// Ecrire le contenu en NVS aurait fait payer une ecriture FLASH a chaque cue,
// donc un craquement audio (MESURES.md §13).
#pragma once
#include <Arduino.h>

namespace ScriptStore {

bool   monter();
bool   estMonte();

// Liste JSON : [{"name":"transpose.nms","bytes":123}, ...]
String listerJson();

// Ecriture d'un script (remplace s'il existe).
bool ecrire(const char* nom, const String& contenu);
bool supprimer(const char* nom);

// Lecture. Retourne false si absent ; `contenu` est vide dans ce cas.
bool lire(const char* nom, String& contenu);

bool existe(const char* nom);

}  // namespace ScriptStore
