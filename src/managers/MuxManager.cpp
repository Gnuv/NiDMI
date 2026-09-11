#include "MuxManager.h"
#include <Preferences.h>
#include "../osc/OSCQueue.h"
#include "../midi/MidiSender.h"
#include "ComponentManager.h"
#include "../Globals.h"

MuxManager::MuxManager() : mux_count(0), muxTaskHandle(nullptr), taskStarted(false) {
     for (int i = 0; i < MAX_MUXES; i++) {
         muxes[i] = nullptr;
         mux_configs[i].enabled = false;
     }
 }
 
 MuxManager::~MuxManager() {
     stop(); // Arrêter la tâche avant destruction
     for (int i = 0; i < MAX_MUXES; i++) {
         if (muxes[i] != nullptr) {
             delete muxes[i];
             muxes[i] = nullptr;
         }
     }
 }
 
 float MuxManager::mapFilterIntensity(uint8_t intensity) {
     if (intensity < 1) intensity = 1;
     if (intensity > 10) intensity = 10;
     
     float normalized = (intensity - 1) / 9.0f;
     float alpha_min = 0.05f;
     float alpha_max = 0.5f;
     float alpha = alpha_max - (alpha_max - alpha_min) * (normalized * normalized);
     
     return alpha;
 }
 
 uint8_t MuxManager::computeCcForChannel(const MuxConfig& config, uint8_t channel) const {
     uint16_t cc = config.cc_base + channel;
     if (cc > 127) cc = 127;
     return (uint8_t)cc;
 }
 
 bool MuxManager::addMux(uint8_t mux_id, uint8_t sig, uint8_t s0, uint8_t s1, uint8_t s2, uint8_t s3,
                          uint8_t en, uint16_t analog_min, uint16_t analog_max,
                          bool hysteresis_enabled, MuxOSCFormat osc_format, uint8_t filter_intensity,
                          uint8_t cc_base, uint8_t midi_channel, const char* osc_base) {
     if (mux_id >= MAX_MUXES) return false;
     
     if (muxes[mux_id] != nullptr) {
         delete muxes[mux_id];
         muxes[mux_id] = nullptr;
     }
     
     muxes[mux_id] = new AnalogMux(sig, s0, s1, s2, s3, en);
     if (muxes[mux_id] == nullptr) {
         Serial.printf("[MuxManager] Echec allocation memoire pour Mux %d\n", mux_id);
         return false;
     }
     
     muxes[mux_id]->begin();
     
     mux_configs[mux_id].sig_pin = sig;
     mux_configs[mux_id].s0 = s0;
     mux_configs[mux_id].s1 = s1;
     mux_configs[mux_id].s2 = s2;
     mux_configs[mux_id].s3 = s3;
     mux_configs[mux_id].en_pin = en;
     mux_configs[mux_id].enabled = true;
     mux_configs[mux_id].cc_base = cc_base;
     mux_configs[mux_id].midi_channel = midi_channel;
     for (uint8_t ch = 0; ch < 16; ch++) {
         mux_configs[mux_id].analog_min[ch] = analog_min;
         mux_configs[mux_id].analog_max[ch] = analog_max;
     }
     mux_configs[mux_id].hysteresis_enabled = hysteresis_enabled;
     mux_configs[mux_id].osc_format = osc_format;
     mux_configs[mux_id].filter_intensity = filter_intensity;
     
     if (osc_base != nullptr && strlen(osc_base) > 0) {
         strncpy(mux_configs[mux_id].osc_base, osc_base, sizeof(mux_configs[mux_id].osc_base) - 1);
         mux_configs[mux_id].osc_base[sizeof(mux_configs[mux_id].osc_base) - 1] = '\0';
     } else {
         snprintf(mux_configs[mux_id].osc_base, sizeof(mux_configs[mux_id].osc_base), "/mux%d", mux_id);
     }
     
     mux_cache[mux_id] = MuxCache();
     
     mux_count = 0;
     for (int i = 0; i < MAX_MUXES; i++) {
         if (mux_configs[i].enabled) mux_count++;
     }
     
     Serial.printf("[MuxManager] Mux %d configure: SIG=%d, S0=%d, S1=%d, S2=%d, S3=%d, EN=%d\n",
                  mux_id, sig, s0, s1, s2, s3, en);
     
     return true;
 }
 
 bool MuxManager::removeMux(uint8_t mux_id) {
     if (mux_id >= MAX_MUXES) return false;
     
     if (muxes[mux_id] != nullptr) {
         delete muxes[mux_id];
         muxes[mux_id] = nullptr;
     }
     
     mux_configs[mux_id].enabled = false;
     mux_cache[mux_id] = MuxCache();
     
     mux_count = 0;
     for (int i = 0; i < MAX_MUXES; i++) {
         if (mux_configs[i].enabled) mux_count++;
     }
     
     Serial.printf("[MuxManager] Mux %d supprime\n", mux_id);
     return true;
 }
 
 const MuxConfig* MuxManager::getMuxConfig(uint8_t mux_id) const {
     if (mux_id >= MAX_MUXES) return nullptr;
     return &mux_configs[mux_id];
 }
 
 void MuxManager::updateMuxCache(uint8_t mux_id) {
     if (mux_id >= MAX_MUXES || muxes[mux_id] == nullptr) {
         return;
     }
     
     MuxCache& cache = mux_cache[mux_id];
     const MuxConfig& config = mux_configs[mux_id];
     bool was_valid = cache.valid;
     
     if (muxes[mux_id]->readAll(cache.raw_values)) {
         cache.values_changed = false;
         for (uint8_t ch = 0; ch < 16; ch++) {
             uint16_t mapped_value;
             if (config.analog_max[ch] > config.analog_min[ch]) {
                 int32_t raw = cache.raw_values[ch];
                 int32_t min_val = config.analog_min[ch];
                 int32_t max_val = config.analog_max[ch];
                 
                 if (raw < min_val) raw = min_val;
                 if (raw > max_val) raw = max_val;
                 
                 mapped_value = (uint16_t)map(raw, min_val, max_val, 0, 4095);
             } else {
                 mapped_value = 0;
             }
             
             uint8_t intensity = 5; // Défaut (MUX n'a pas encore de config spécifique)
             cache.filters[ch].setAlphaFromIntensity(intensity);
             cache.filtered_values[ch] = cache.filters[ch].process(mapped_value);
             
             uint8_t old_stable = cache.stable_values[ch];
             if (config.hysteresis_enabled) {
                 cache.hysteresis[ch].update(cache.filtered_values[ch]);
                 cache.stable_values[ch] = cache.hysteresis[ch].getValue();
             } else {
                 cache.stable_values[ch] = cache.filtered_values[ch] >> 5;
             }
             
             if (abs((int)cache.stable_values[ch] - (int)old_stable) >= 1) {
                 cache.values_changed = true;
             }
         }
         cache.last_update = millis();
         cache.valid = true;
         
         if (!was_valid) {
             cache.values_changed = true;
         }
     }
 }
 
void MuxManager::updateAllCaches() {
    static uint32_t last_update = 0;
    const uint32_t UPDATE_INTERVAL_MS = 10; // 10ms entre chaque lecture
    
    uint32_t now = millis();
    if (now - last_update < UPDATE_INTERVAL_MS) {
        return; // Trop tôt, skip cette fois
    }
    last_update = now;
    
    // Mettre à jour un seul multiplexeur par tour (round-robin)
    static uint8_t current_mux = 0;
    
    for (uint8_t attempt = 0; attempt < MAX_MUXES; attempt++) {
        uint8_t mux_id = current_mux;
        current_mux = (current_mux + 1) % MAX_MUXES;
        
        if (muxes[mux_id] != nullptr && mux_configs[mux_id].enabled) {
            updateMuxCache(mux_id);
            break; // Un seul par appel
        }
    }
}
 
 void MuxManager::sendOscBatches(OSCQueue& osc_queue) {
     for (uint8_t mux_id = 0; mux_id < MAX_MUXES; mux_id++) {
         if (muxes[mux_id] != nullptr && mux_configs[mux_id].enabled) {
             MuxCache& cache = mux_cache[mux_id];
             
             if (cache.valid && cache.values_changed) {
                 const MuxConfig& mux_config = mux_configs[mux_id];
                 String oscBase = (mux_config.osc_base[0] != '\0') ?
                     String(mux_config.osc_base) : "/mux" + String(mux_id);
                 
                 switch (mux_config.osc_format) {
                     case MuxOSCFormat::RAW: {
                         uint16_t raw_values[16];
                         for (uint8_t ch = 0; ch < 16; ch++) {
                             raw_values[ch] = (uint16_t)cache.stable_values[ch] * 4095 / 127;
                         }
                         osc_queue.enqueueIntArray(oscBase, raw_values, 16);
                         break;
                     }
                     case MuxOSCFormat::FLOAT: {
                         float normalized_values[16];
                         for (uint8_t ch = 0; ch < 16; ch++) {
                             normalized_values[ch] = cache.stable_values[ch] / 127.0f;
                         }
                         osc_queue.enqueueFloatArray(oscBase, normalized_values, 16);
                         break;
                     }
                     case MuxOSCFormat::MIDI: {
                         osc_queue.enqueueMidiArray(oscBase, cache.stable_values, 16);
                         break;
                     }
                 }
                 cache.values_changed = false;
             }
         }
     }
 }
 
 void MuxManager::sendMidiUpdates(MidiSender* midi_sender) {
     if (!midi_sender) return;
     
     for (uint8_t mux_id = 0; mux_id < MAX_MUXES; mux_id++) {
         if (muxes[mux_id] == nullptr || !mux_configs[mux_id].enabled) continue;
         
         MuxCache& cache = mux_cache[mux_id];
         if (!cache.valid) continue;
         
         const MuxConfig& config = mux_configs[mux_id];
         uint8_t channel = config.midi_channel;
         if (channel < 1 || channel > 16) channel = 1;
         
         for (uint8_t ch = 0; ch < 16; ch++) {
             uint8_t value = cache.stable_values[ch];
             if (value == cache.last_sent_values[ch]) {
                 continue;
             }
             uint8_t cc = computeCcForChannel(config, ch);
             midi_sender->sendControlChange(channel, cc, value);
             cache.last_sent_values[ch] = value;
         }
     }
 }
 
 bool MuxManager::readMuxAllChannels(uint8_t mux_id, uint16_t* values) {
     if (mux_id >= MAX_MUXES || !values || muxes[mux_id] == nullptr) {
         return false;
     }
     
     updateMuxCache(mux_id);
     
     MuxCache& cache = mux_cache[mux_id];
     for (uint8_t ch = 0; ch < 16; ch++) {
         values[ch] = (uint16_t)cache.stable_values[ch] * 4095 / 127;
     }
     
     return true;
 }
 
 uint16_t MuxManager::readMuxChannel(uint8_t gpio) {
     if (!isMuxGpio(gpio)) return 0xFFFF;
     
     uint8_t offset = gpio - MUX_GPIO_BASE;
     uint8_t mux_id = offset / MUX_CHANNELS;
     uint8_t channel = offset % MUX_CHANNELS;
     
     if (mux_id >= MAX_MUXES || muxes[mux_id] == nullptr) {
         return 0xFFFF;
     }
     
     MuxCache& cache = mux_cache[mux_id];
     uint32_t now = millis();
     
     if (!cache.valid || (now - cache.last_update) > 10) {
         updateMuxCache(mux_id);
     }
     
     return (uint16_t)cache.stable_values[channel] * 4095 / 127;
 }
 
 bool MuxManager::calibrateMux(uint8_t mux_id, uint8_t channel, bool is_min, bool all_channels, OSCQueue& osc_queue) {
     if (mux_id >= MAX_MUXES || muxes[mux_id] == nullptr) {
         Serial.printf("[MuxManager] Mux %d non configure\n", mux_id);
         return false;
     }
     
     if (!all_channels && channel >= 16) {
         Serial.printf("[MuxManager] Canal %d invalide (max 15)\n", channel);
         return false;
     }
     
     updateMuxCache(mux_id);
     MuxCache& cache = mux_cache[mux_id];
     
     if (!cache.valid) {
         Serial.printf("[MuxManager] Cache Mux %d invalide\n", mux_id);
         return false;
     }
     
     if (all_channels) {
         for (uint8_t ch = 0; ch < 16; ch++) {
             if (is_min) {
                 mux_configs[mux_id].analog_min[ch] = cache.raw_values[ch];
                 Serial.printf("[MuxManager] Mux %d canal %d: analog_min = %d\n", mux_id, ch, cache.raw_values[ch]);
             } else {
                 mux_configs[mux_id].analog_max[ch] = cache.raw_values[ch];
                 Serial.printf("[MuxManager] Mux %d canal %d: analog_max = %d\n", mux_id, ch, cache.raw_values[ch]);
             }
         }
     } else {
         if (is_min) {
             mux_configs[mux_id].analog_min[channel] = cache.raw_values[channel];
             Serial.printf("[MuxManager] Mux %d canal %d: analog_min = %d\n", mux_id, channel, cache.raw_values[channel]);
         } else {
             mux_configs[mux_id].analog_max[channel] = cache.raw_values[channel];
             Serial.printf("[MuxManager] Mux %d canal %d: analog_max = %d\n", mux_id, channel, cache.raw_values[channel]);
         }
     }
     
     Preferences prefs;
     prefs.begin("nidmi", false);
     String key = "mux_thresh_" + String(mux_id);
     
     uint16_t first_min = mux_configs[mux_id].analog_min[0];
     uint16_t first_max = mux_configs[mux_id].analog_max[0];
     bool uniform = true;
     
     for (uint8_t ch = 1; ch < 16; ch++) {
         if (mux_configs[mux_id].analog_min[ch] != first_min ||
             mux_configs[mux_id].analog_max[ch] != first_max) {
             uniform = false;
             break;
         }
     }
     
     if (uniform) {
         uint8_t buffer[5];
         buffer[0] = 0x01;
         buffer[1] = first_min & 0xFF;
         buffer[2] = (first_min >> 8) & 0xFF;
         buffer[3] = first_max & 0xFF;
         buffer[4] = (first_max >> 8) & 0xFF;
         prefs.putBytes(key.c_str(), buffer, 5);
     } else {
         uint8_t buffer[65];
         buffer[0] = 0x00;
         for (uint8_t ch = 0; ch < 16; ch++) {
             uint16_t val = mux_configs[mux_id].analog_min[ch];
             buffer[1 + ch * 2] = val & 0xFF;
             buffer[1 + ch * 2 + 1] = (val >> 8) & 0xFF;
         }
         for (uint8_t ch = 0; ch < 16; ch++) {
             uint16_t val = mux_configs[mux_id].analog_max[ch];
             buffer[33 + ch * 2] = val & 0xFF;
             buffer[33 + ch * 2 + 1] = (val >> 8) & 0xFF;
         }
         prefs.putBytes(key.c_str(), buffer, 65);
     }
     
     prefs.end();
     
     const MuxConfig& mux_config = mux_configs[mux_id];
     String oscBase = (mux_config.osc_base[0] != '\0') ?
         String(mux_config.osc_base) : "/mux" + String(mux_id);
     
     osc_queue.enqueueIntArray(oscBase + "/cal/min", mux_configs[mux_id].analog_min, 16);
     osc_queue.enqueueIntArray(oscBase + "/cal/max", mux_configs[mux_id].analog_max, 16);
     
     return true;
 }
 
 bool MuxManager::resetMuxThresholds(uint8_t mux_id, uint8_t channel, bool all_channels, OSCQueue& osc_queue) {
     if (mux_id >= MAX_MUXES || muxes[mux_id] == nullptr) {
         Serial.printf("[MuxManager] Mux %d non configure\n", mux_id);
         return false;
     }
     
     if (!all_channels && channel >= 16) {
         Serial.printf("[MuxManager] Canal %d invalide (max 15)\n", channel);
         return false;
     }
     
     if (all_channels) {
         for (uint8_t ch = 0; ch < 16; ch++) {
             mux_configs[mux_id].analog_min[ch] = 0;
             mux_configs[mux_id].analog_max[ch] = 4095;
         }
     } else {
         mux_configs[mux_id].analog_min[channel] = 0;
         mux_configs[mux_id].analog_max[channel] = 4095;
     }
     
     Preferences prefs;
     prefs.begin("nidmi", false);
     String key = "mux_thresh_" + String(mux_id);
     
     uint16_t first_min = mux_configs[mux_id].analog_min[0];
     uint16_t first_max = mux_configs[mux_id].analog_max[0];
     bool uniform = true;
     
     for (uint8_t ch = 1; ch < 16; ch++) {
         if (mux_configs[mux_id].analog_min[ch] != first_min ||
             mux_configs[mux_id].analog_max[ch] != first_max) {
             uniform = false;
             break;
         }
     }
     
     if (uniform) {
         uint8_t buffer[5];
         buffer[0] = 0x01;
         buffer[1] = first_min & 0xFF;
         buffer[2] = (first_min >> 8) & 0xFF;
         buffer[3] = first_max & 0xFF;
         buffer[4] = (first_max >> 8) & 0xFF;
         prefs.putBytes(key.c_str(), buffer, 5);
     } else {
         uint8_t buffer[65];
         buffer[0] = 0x00;
         for (uint8_t ch = 0; ch < 16; ch++) {
             uint16_t val = mux_configs[mux_id].analog_min[ch];
             buffer[1 + ch * 2] = val & 0xFF;
             buffer[1 + ch * 2 + 1] = (val >> 8) & 0xFF;
         }
         for (uint8_t ch = 0; ch < 16; ch++) {
             uint16_t val = mux_configs[mux_id].analog_max[ch];
             buffer[33 + ch * 2] = val & 0xFF;
             buffer[33 + ch * 2 + 1] = (val >> 8) & 0xFF;
         }
         prefs.putBytes(key.c_str(), buffer, 65);
     }
     
     prefs.end();
     
     const MuxConfig& mux_config = mux_configs[mux_id];
     String oscBase = (mux_config.osc_base[0] != '\0') ?
         String(mux_config.osc_base) : "/mux" + String(mux_id);
     
     osc_queue.enqueueIntArray(oscBase + "/cal/min", mux_configs[mux_id].analog_min, 16);
     osc_queue.enqueueIntArray(oscBase + "/cal/max", mux_configs[mux_id].analog_max, 16);
     
     return true;
 }
 
 void MuxManager::loadMuxConfigFromNVS() {
     Preferences prefs;
     prefs.begin("nidmi", true);
     
     for (uint8_t i = 0; i < MAX_MUXES; i++) {
         String key = "mux_" + String(i);
         String config = prefs.getString(key.c_str(), "");
         
         if (!config.isEmpty()) {
            int vals[11] = {0, 0, 0, 0, 0, 255, 1, 1, 1, 1, 5};
             String osc_base_str = "";
             int idx = 0;
             int start = 0;
             int comma_count = 0;
             
             for (int j = 0; j < (int)config.length(); j++) {
                 if (config[j] == ',') comma_count++;
             }
             
             for (int j = 0; j <= (int)config.length() && idx < 11; j++) {
                 if (j == (int)config.length() || config[j] == ',') {
                     vals[idx++] = config.substring(start, j).toInt();
                     start = j + 1;
                 }
             }
             
            bool has_extended_format = comma_count >= 11;
            if ((has_extended_format || comma_count >= 9) && start < (int)config.length()) {
                 osc_base_str = config.substring(start);
             }
             
            if (idx >= 5) {
                uint8_t cc_base = 1;
                uint8_t midi_channel = 1;
                bool hysteresis_enabled = true;
                MuxOSCFormat osc_format = MuxOSCFormat::FLOAT;
                uint8_t filter_intensity = 5;
                
                if (has_extended_format) {
                    cc_base = (idx >= 7) ? vals[6] : 1;
                    midi_channel = (idx >= 8) ? vals[7] : 1;
                    hysteresis_enabled = (idx >= 9) ? (vals[8] != 0) : true;
                    osc_format = (idx >= 10) ? static_cast<MuxOSCFormat>(vals[9]) : MuxOSCFormat::FLOAT;
                    filter_intensity = (idx >= 11) ? vals[10] : 5;
                } else {
                    hysteresis_enabled = (idx >= 7) ? (vals[6] != 0) : true;
                    osc_format = (idx >= 8) ? static_cast<MuxOSCFormat>(vals[7]) : MuxOSCFormat::FLOAT;
                    filter_intensity = (idx >= 9) ? vals[8] : 5;
                }
                 
                 if (osc_format > MuxOSCFormat::MIDI) {
                     osc_format = MuxOSCFormat::FLOAT;
                 }
                 
                 if (filter_intensity < 1) filter_intensity = 1;
                 if (filter_intensity > 10) filter_intensity = 10;
                 
                 if (cc_base > 127) cc_base = 127;
                 if (midi_channel < 1 || midi_channel > 16) midi_channel = 1;
                 
                 String thresh_key = "mux_thresh_" + String(i);
                 uint8_t buffer[65];
                 size_t bytes_read = prefs.getBytes(thresh_key.c_str(), buffer, 65);
                 
                 uint16_t analog_min = 0;
                 uint16_t analog_max = 4095;
                 
                 if (bytes_read == 5) {
                     if (buffer[0] == 0x01) {
                         analog_min = buffer[1] | (buffer[2] << 8);
                         analog_max = buffer[3] | (buffer[4] << 8);
                     }
                 } else if (bytes_read == 65) {
                     if (buffer[0] == 0x00) {
                         analog_min = buffer[1] | (buffer[2] << 8);
                         analog_max = buffer[33] | (buffer[34] << 8);
                     }
                 }
                 
                 const char* osc_base_ptr = (osc_base_str.length() > 0) ? osc_base_str.c_str() : nullptr;
                 addMux(i, vals[0], vals[1], vals[2], vals[3], vals[4], vals[5],
                        analog_min, analog_max, hysteresis_enabled, osc_format,
                        filter_intensity, cc_base, midi_channel, osc_base_ptr);
                 
                 if (bytes_read == 65 && buffer[0] == 0x00) {
                     for (uint8_t ch = 0; ch < 16; ch++) {
                         mux_configs[i].analog_min[ch] = buffer[1 + ch * 2] | (buffer[1 + ch * 2 + 1] << 8);
                         mux_configs[i].analog_max[ch] = buffer[33 + ch * 2] | (buffer[33 + ch * 2 + 1] << 8);
                     }
                     Serial.printf("[MuxManager] Loaded mux %d from NVS (non-uniform thresholds)\n", i);
                 } else {
                     Serial.printf("[MuxManager] Loaded mux %d from NVS (min=%d, max=%d, hyst=%d, osc_fmt=%d)\n",
                                  i, analog_min, analog_max, hysteresis_enabled, (int)osc_format);
                 }
             }
         }
     }
     
     prefs.end();
}

void MuxManager::begin() {
    if (taskStarted) {
        return; // Déjà démarrée
    }
    
    // Créer la tâche FreeRTOS sur Core 0 (PRO_CPU) pour les lectures ADC
    BaseType_t result = xTaskCreatePinnedToCore(
        muxTask,           // Fonction de la tâche
        "MuxTask",         // Nom
        4096,              // Stack size (4KB)
        this,              // Paramètre (instance)
        5,                 // Priorité (haute priorité pour temps réel)
        &muxTaskHandle,    // Handle
        0                  // Core 0 (PRO_CPU - processeur principal)
    );
    
    if (result == pdPASS) {
        taskStarted = true;
        Serial.println("[MuxManager] FreeRTOS task started on Core 0");
    } else {
        Serial.println("[MuxManager] ERROR: Failed to create FreeRTOS task");
    }
}

void MuxManager::stop() {
    if (!taskStarted || muxTaskHandle == nullptr) {
        return;
    }
    
    vTaskDelete(muxTaskHandle);
    muxTaskHandle = nullptr;
    taskStarted = false;
    Serial.println("[MuxManager] FreeRTOS task stopped");
}

void MuxManager::muxTask(void* parameter) {
    MuxManager* instance = static_cast<MuxManager*>(parameter);
    instance->muxTaskLoop();
}

/* Gigue d'ordonnancement de la tache ADC — meme mesure que pour MidiTask, meme
 * raison (CONVERGENCE §1.5). Celle-ci est a la priorite 5, le serveur web a 10.
 * Publiee par /api/diag/gigue. */
volatile uint32_t g_gigueMuxMaxUs   = 0;
volatile uint32_t g_gigueMuxTours   = 0;
volatile uint32_t g_gigueMuxRetards = 0;
volatile uint32_t g_gigueMuxCumulUs = 0;
void nidmi_gigue_mux_reset(){
    g_gigueMuxMaxUs = g_gigueMuxTours = g_gigueMuxRetards = g_gigueMuxCumulUs = 0;
}

void MuxManager::muxTaskLoop() {
    const TickType_t xFrequency = pdMS_TO_TICKS(5); // 5ms = 200Hz
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const uint32_t PERIODE_US = 5000;
    uint32_t precedent = 0;

    for(;;) {
        if (g_componentManager.isNvsWriteInProgress()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            precedent = 0;          // une pause n'est pas de la gigue
            continue;
        }
        const uint32_t maintenantUs = micros();
        if (precedent) {
            const uint32_t d = maintenantUs - precedent;
            const uint32_t ecart = (d > PERIODE_US) ? (d - PERIODE_US) : (PERIODE_US - d);
            if (ecart > g_gigueMuxMaxUs) g_gigueMuxMaxUs = ecart;
            if (ecart > 2500) g_gigueMuxRetards++;     // la moitie de la periode
            g_gigueMuxCumulUs += ecart;
            g_gigueMuxTours++;
        }
        precedent = maintenantUs;
        // Mettre à jour un multiplexeur par tour (round-robin)
        static uint8_t current_mux = 0;
        
        for (uint8_t attempt = 0; attempt < MAX_MUXES; attempt++) {
            uint8_t mux_id = current_mux;
            current_mux = (current_mux + 1) % MAX_MUXES;
            
            if (muxes[mux_id] != nullptr && mux_configs[mux_id].enabled) {
                updateMuxCache(mux_id);
                break; // Un seul par tour
            }
        }
        
        // Attendre jusqu'à la prochaine période (5ms)
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}
