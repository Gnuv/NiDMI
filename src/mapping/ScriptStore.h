// ScriptStore — les scripts .nms sur la partition de fichiers (`storage`).
//
// La partition s'appelait « mapfs », « scripts de mapping (1MB) » dans la
// table : c'etait sa raison d'etre. Elle n'avait d'abord servi qu'aux
// echantillons audio, ce qui inversait la hierarchie du systeme. Elle porte
// tous les fichiers de la carte, et s'appelle `storage` (config/Stockage.h).
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

/* DEUX LIEUX (CONVERGENCE §9.7, MESURES §186). Les scripts des cues et de la
 * chaine appartiennent a la COMPOSITION : ils vivent a plat dans son dossier
 * nomme, et changent avec elle. Ceux des broches appartiennent a l'INTERFACE :
 * interface/scripts/, quelle que soit la composition ouverte. Deux
 * compositions portant chacune leur test.nms ne s'ecrasent plus. */
enum class Lieu : uint8_t { Composition, Interface };
constexpr const char* DOSSIER_INTERFACE = "/interface/scripts";

bool   monter();
bool   estMonte();

// Le chemin d'un script ; "" si c'est la composition et qu'aucune n'est ouverte.
String chemin(Lieu lieu, const char* nom);

// Liste JSON : [{"name":"transpose.nms","bytes":123}, ...]
String listerJson(Lieu lieu);

/* Ecriture d'un script (remplace s'il existe). Dans la composition : sur un
 * repertoire vide, la premiere se cree. */
bool ecrire(Lieu lieu, const char* nom, const String& contenu);
bool supprimer(Lieu lieu, const char* nom);
/* Supprimer par le chemin (l'explorateur de fichiers) : le magasin oublie ce
 * qu'il gardait en PSRAM. false si le chemin n'est pas un script. */
bool supprimerChemin(const String& chemin);
/* Un dossier renomme, deplace ou supprime (Repertoire) : ce que le magasin
 * gardait en PSRAM sous ses chemins s'oublie. */
void oublierSous(const char* prefixe);

// Lecture. Retourne false si absent ; `contenu` est vide dans ce cas.
bool lire(Lieu lieu, const char* nom, String& contenu);

bool existe(Lieu lieu, const char* nom);

/** Reads the scripts of the open composition and of the interface into the PSRAM cache —
 *  from core 0, before the loop asks for them (§242). The other compositions' scripts leave
 *  the cache first. Any task but the audio's core: it is a burst of flash reads. */
void preload();

/** Bench (§240): the measure, step by step — the block count first if `withInfo`,
 *  then the walk, at most `maxSteps` entries. Each step is said on the console
 *  BEFORE it is taken, so the journal that survives a restart names the step that
 *  hung. Returns the steps with their time in µs, as JSON. What storage holds and
 *  occupies is StorageIndex's (§241). */
String walkTrace(int maxSteps, bool withInfo);

}  // namespace ScriptStore
