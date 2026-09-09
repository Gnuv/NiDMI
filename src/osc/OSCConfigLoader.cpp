#include "OSCConfigLoader.h"
#include "OSCManager.h"
#include "OSCQueue.h"
#include <Preferences.h>

OSCConfigLoader::OSCConfig OSCConfigLoader::loadFromNVS() {
    Preferences prefs;
    prefs.begin("nidmi", true);
    
    OSCConfig config;
    /* LES MEMES CLES QUE LA ROUTE /api/osc, ET LES MEMES DEFAUTS.
     *
     * Elles divergeaient. La route ecrit l'ADRESSE dans « osc_target » ; le
     * chargeur y lisait un MODE et cherchait l'adresse dans « osc_ip », qui
     * n'est jamais ecrite. La cible OSC reglee dans l'app n'a donc JAMAIS servi
     * au demarrage : la carte repartait toujours en diffusion generale
     * (255.255.255.255), sur le port 8001 la ou la route en montrait 8000, et
     * l'interface enregistree etait ignoree au profit d'un STA en dur.
     *
     * Deux vocabulaires de configuration s'etaient succede — (target=mode,
     * ip=adresse) puis (target=adresse, interface=mode) — et seul le second est
     * ecrit. C'est donc celui-la qu'on lit. */
    config.ip        = prefs.getString("osc_target", "192.168.4.100");
    config.port      = prefs.getInt("osc_port", 8000);
    config.broadcast = prefs.getBool("osc_broadcast", false);
    config.target    = prefs.getString("osc_interface", "ap");
    
    prefs.end();
    
    return config;
}

void OSCConfigLoader::initialize(
    const OSCConfig& config,
    OSCManager& osc_manager,
    OSCQueue& osc_queue,
    std::function<void(const String&, float, const String&)> messageCallback
) {
    // Initialiser osc_manager avec la config NVS
    const uint8_t iface = (config.target == "sta")  ? OSC_INTERFACE_STA
                        : (config.target == "both") ? OSC_INTERFACE_BOTH
                                                    : OSC_INTERFACE_AP;
    osc_manager.begin(config.ip, config.port, 8001);
    osc_manager.setBroadcast(config.broadcast);
    osc_manager.setInterface(iface);      // etait fige a 1, quoi qu'on eut regle
    osc_manager.setEnabled(true);
    
    // Initialiser osc_queue avec la même config
    osc_queue.begin();
    osc_queue.setTarget(config.ip, config.port);
    osc_queue.setBroadcast(config.broadcast);
    osc_queue.setInterface(iface);
    
    Serial.printf("[OSCConfigLoader] OSC Config: %s:%d (broadcast=%d)\n", 
                 config.ip.c_str(), config.port, config.broadcast);
    
    // Configurer le callback OSC pour les commandes de calibrage
    osc_manager.setMessageCallback(messageCallback);
}
