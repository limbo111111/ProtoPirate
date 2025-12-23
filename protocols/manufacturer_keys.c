#include "manufacturer_keys.h"

const ManufacturerKey manufacturer_keys[] = {
    // Found in protocols/kia_v3_v4.c
    {
        .name = "Kia V3/V4",
        .key = MANUFACTURER_KEY_KIA_V3_V4,
    },
    // Standard Microchip KeeLoq "Simple Learning" Key
    // Common default key for HCS200/300 encoders
    {
        .name = "Simple Learning",
        .key = MANUFACTURER_KEY_SIMPLE_LEARNING,
    },
    // Microchip KeeLoq "Normal Learning" Key (Standard Test Key)
    // Common default key for development kits
    {
        .name = "Normal Learning",
        .key = MANUFACTURER_KEY_NORMAL_LEARNING,
    },
    // Note: Extensive research was conducted for VAG, Ford, Hyundai, and other
    // proprietary manufacturer keys. No verifiable public sources for these
    // 64-bit master keys were found. To maintain data integrity, unverified
    // placeholders have been omitted.
};

const size_t manufacturer_keys_count = sizeof(manufacturer_keys) / sizeof(ManufacturerKey);
