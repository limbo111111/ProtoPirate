// helpers/protopirate_settings.h
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define PROTOPIRATE_SETTINGS_FILE EXT_PATH("apps_data/protopirate/settings.txt")
#define PROTOPIRATE_SETTINGS_DIR EXT_PATH("apps_data/protopirate")

typedef struct {
    uint32_t frequency;
    uint8_t preset_index;
    bool auto_save;
    bool hopping_enabled;
} ProtoPirateSettings;

void protopirate_settings_load(ProtoPirateSettings* settings);
void protopirate_settings_save(ProtoPirateSettings* settings);
void protopirate_settings_set_defaults(ProtoPirateSettings* settings);