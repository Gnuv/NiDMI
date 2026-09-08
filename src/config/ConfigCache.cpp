#include "ConfigCache.h"
#include "../Globals.h"

/* Forward declarations */
String mergeConfigWithDefaults(const String& nvsConfig, const String& defaultConfig);
// Preferences utilisées localement dans chaque fonction
String getDefaultConfig(String pin);
extern "C" {
    void nidmi_requestReloadPins();
}

ConfigCache::ConfigCache() : lastSave(0), count(0) {
    for (int i = 0; i < MAX_PINS; i++) {
        dirty[i] = false;
    }
}

/* Stocker une config en cache (pas de NVS) */
void ConfigCache::setConfig(const String& pin, const String& config) {
    int index = findPinIndex(pin);
    if (index == -1 && count < MAX_PINS) {
        /* Nouvelle pin */
        index = count++;
        pinNames[index] = pin;
    }
    
    if (index != -1) {
        cache[index] = config;
        dirty[index] = true;
        debug_network( "[ConfigCache] Pin %s mise en cache\n", pin.c_str());
    }
}

/* Récupérer config depuis cache ou NVS */
String ConfigCache::getConfig(const String& pin) {
    int index = findPinIndex(pin);
    if (index != -1 && !cache[index].isEmpty()) {
        return cache[index];  /* Depuis cache */
    }
    
    /* Depuis NVS */
    Preferences preferences;
    preferences.begin("nidmi", true);
    String key = "pin_" + pin;
    String config = preferences.getString(key.c_str(), "\n");
    preferences.end();
    
    if (!config.isEmpty()) {
        /* Compléter la config NVS avec les valeurs par défaut manquantes */
        String defaultConfig = getDefaultConfig(pin);
        String mergedConfig = mergeConfigWithDefaults(config, defaultConfig);
        
        /* Mettre en cache pour prochaine fois */
        setConfigClean(pin, mergedConfig);
        return mergedConfig;
    }
    
    /* Défaut */
    return getDefaultConfig(pin);
}

/* Sauvegarder toutes les pins dirty en NVS */
void ConfigCache::saveAllDirty() {
    debug_network( "[ConfigCache] DEBUG forceSave() - count=%d\n", count);
    
    if (count == 0) {
        debug_network( "[ConfigCache] DEBUG Aucune pin en cache\n\n");
        return;
    }
    
    bool hasChanges = false;
    for (int i = 0; i < count; i++) {
        if (dirty[i]) {
            hasChanges = true;
            debug_network( "[ConfigCache] DEBUG Pin %s est dirty\n", pinNames[i].c_str());
        }
    }
    
    if (!hasChanges) {
        debug_network( "[ConfigCache] DEBUG Aucune pin dirty\n\n");
        return;
    }
    
    debug_network( "[ConfigCache] DEBUG Début sauvegarde NVS...\n\n");

    Preferences preferences;
    preferences.begin("nidmi", false);
    for (int i = 0; i < count; i++) {
        if (dirty[i]) {
            String key = "pin_" + pinNames[i];
            bool success = preferences.putString(key.c_str(), cache[i]);
            dirty[i] = false;
            debug_network( "[ConfigCache] Pin %s sauvegardée en NVS (success=%s)\n", 
                         pinNames[i].c_str(), success ? "true" : "false\n");
        }
    }
    preferences.end();
    
    lastSave = millis();
    debug_network( "[ConfigCache] Sauvegarde groupée terminée (%d pins)\n", count);
    
    /* Demander le rechargement des configs pins */
    nidmi_requestReloadPins();
}

/* Auto-save si nécessaire (30 secondes) */
void ConfigCache::autoSave() {
    if (millis() - lastSave > 30000) {  /* 30 secondes */
        saveAllDirty();
    }
}

/* Forcer la sauvegarde immédiate */
void ConfigCache::forceSave() {
    debug_network( "[ConfigCache] DEBUG forceSave() appelé\n\n");
    saveAllDirty();
}

int ConfigCache::findPinIndex(const String& pin) {
    for (int i = 0; i < count; i++) {
        if (pinNames[i] == pin) return i;
    }
    return -1;
}

/* Supprimer une pin du cache et de la NVS */
void ConfigCache::removeConfig(const String& pin) {
    int index = findPinIndex(pin);
    String key = "pin_" + pin;
    
    Preferences preferences;
    preferences.begin("nidmi", false);
    
    bool key_exists = preferences.isKey(key.c_str());
    
    if (key_exists) {
        bool removed = preferences.remove(key.c_str());
        preferences.end();
        
        preferences.begin("nidmi", true);
        bool still_exists = preferences.isKey(key.c_str());
        preferences.end();
        
        if (still_exists) {
            Serial.printf("[ConfigCache] remove() n'a pas fonctionne pour '%s', tentative forcee\n", key.c_str());
            preferences.begin("nidmi", false);
            preferences.putString(key.c_str(), "");
            delay(5);
            preferences.remove(key.c_str());
            delay(5);
            preferences.end();
            
            preferences.begin("nidmi", true);
            bool final_check = preferences.isKey(key.c_str());
            preferences.end();
            
            if (final_check) {
                Serial.printf("[ConfigCache] La cle '%s' existe toujours apres toutes les tentatives!\n", key.c_str());
            } else {
                Serial.printf("[ConfigCache] Cle '%s' supprimee avec succes (methode forcee)\n", key.c_str());
            }
        } else {
            Serial.printf("[ConfigCache] ✓ Clé '%s' supprimée avec succès\n", key.c_str());
        }
    } else {
        preferences.end();
        Serial.printf("[ConfigCache] Clé '%s' n'existait pas dans NVS\n", key.c_str());
    }
    
    /* Retirer du cache si présent */
    if (index != -1 && index < count && count > 0) {
        for (int i = index; i < count - 1; i++) {
            pinNames[i] = pinNames[i + 1];
            cache[i] = cache[i + 1];
            dirty[i] = dirty[i + 1];
        }
        count--;
        
        if (count >= 0 && count < MAX_PINS) {
            pinNames[count].clear();
            cache[count].clear();
            dirty[count] = false;
        }
        Serial.printf("[ConfigCache] Pin '%s' retirée du cache (count=%d)\n", pin.c_str(), count);
    }

    /* RECHARGER, comme toute autre ecriture.
     *
     * Supprimer une broche nettoyait la NVS mais ne demandait rien : la carte
     * continuait d'EXECUTER le composant supprime, indefiniment. Mesure :
     * /api/pins/list ne montrait plus D0, /api/pins/actif le montrait encore
     * six secondes plus tard, et pour toujours. Un composant qui emet du MIDI
     * sans plus etre ni visible ni modifiable — la negation exacte de la regle
     * du projet (« la page montre l'etat exact de ce qui tourne »).
     *
     * saveAll() et le vidage complet le faisaient deja ; la suppression d'UNE
     * broche avait ete oubliee. On s'appuie sur la meme regle qu'ailleurs : la
     * NVS fait foi, la carte se reconstruit dessus — ce qui rend inoffensif le
     * fait qu'aucun ComplexHandler ne corresponde a un composant simple. */
    nidmi_requestReloadPins();
}

/* Clear complet de la NVS - supprime tout le namespace "nidmi" */
void ConfigCache::clearAllNVS() {
    Serial.println("[ConfigCache] ⚠️ CLEAR COMPLET DE LA NVS - suppression de toutes les configs!");
    
    Preferences preferences;
    preferences.begin("nidmi", false);  // false = mode écriture
    
    /* Clear complet du namespace */
    bool cleared = preferences.clear();
    preferences.end();
    
    /* Clear aussi le cache en mémoire */
    count = 0;
    for (int i = 0; i < MAX_PINS; i++) {
        pinNames[i].clear();
        cache[i].clear();
        dirty[i] = false;
    }
    
    if (cleared) {
        Serial.println("[ConfigCache] ✓ NVS complètement vidée");
    } else {
        Serial.println("[ConfigCache] ⚠️ Échec du clear NVS");
    }
    
    /* Demander le rechargement des composants (qui sera vide maintenant) */
    nidmi_requestReloadPins();
}

/* Mettre en cache sans marquer dirty (pour lecture NVS) */
void ConfigCache::setConfigClean(const String& pin, const String& config) {
    int index = findPinIndex(pin);
    if (index == -1 && count < MAX_PINS) {
        index = count++;
        pinNames[index] = pin;
        dirty[index] = false;
    }
    
    if (index != -1) {
        cache[index] = config;
    }
}

void ConfigCache::setConfigClean(const String& pin, const char* config, size_t configLen) {
    if (!config) return;
    int index = findPinIndex(pin);
    if (index == -1 && count < MAX_PINS) {
        index = count++;
        pinNames[index] = pin;
        dirty[index] = false;
    }
    if (index != -1) {
        cache[index] = String(config, configLen);
    }
}

/* Fusionner la config NVS avec les valeurs par défaut manquantes */
String mergeConfigWithDefaults(const String& nvsConfig, const String& defaultConfig) {
    // Vérifier si oscFormat est présent dans la config NVS
    if (nvsConfig.indexOf("\"oscFormat\"") != -1) {
        // La config NVS contient déjà oscFormat, la retourner telle quelle
        return nvsConfig;
    }
    
    // La config NVS ne contient pas oscFormat, utiliser la config par défaut
    // qui contient tous les champs nécessaires
    return defaultConfig;
}

/* Instance globale */
ConfigCache g_configCache;
