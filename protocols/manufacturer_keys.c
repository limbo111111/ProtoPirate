#include "manufacturer_keys.h"

const ManufacturerKey manufacturer_keys[] = {
    // Found in protocols/kia_v3_v4.c
    {
        .name = "Kia V3/V4",
        .key = 0xA8F5DFFC8DAA5CDB,
    },
    // Standard Microchip KeeLoq "Simple Learning" Key
    {
        .name = "Simple Learning",
        .key = 0x123456789ABCDEF0,
    },
    // Microchip KeeLoq "Normal Learning" Key (Standard Test Key)
    {
        .name = "Normal Learning",
        .key = 0x0123456789ABCDEF,
    },
    // Placeholders from reference/cars.txt (Unverified)
    {
        .name = "VW Golf 8 (Unverified)",
        .key = 0x1A2B3C4D5E6F7A8B,
    },
    {
        .name = "BMW iX (Unverified)",
        .key = 0x4D5E6F7A8B9C0D1E,
    },
    {
        .name = "Mercedes S-Class (Unverified)",
        .key = 0x9C0D1E2F3A4B5C6D,
    },
    {
        .name = "Ford Mustang (Unverified)",
        .key = 0x1E2F3A4B5C6D7E8F,
    },
    {
        .name = "Toyota RAV4 (Unverified)",
        .key = 0x3A4B5C6D7E8F9A0B,
    },
    {
        .name = "Honda CR-V (Unverified)",
        .key = 0x5C6D7E8F9A0B1C2D,
    },
    {
        .name = "Nissan Leaf (Unverified)",
        .key = 0x7E8F9A0B1C2D3E4F,
    },
    {
        .name = "Audi Q5 (Unverified)",
        .key = 0x9A0B1C2D3E4F5A6B,
    },
    {
        .name = "Seat Ateca (Unverified)",
        .key = 0xB1C2D3E4F5A6B7C8ULL,
    },
    {
        .name = "Skoda Kodiaq (Unverified)",
        .key = 0xC2D3E4F5A6B7C8D9ULL,
    },
    {
        .name = "Volvo XC90 (Unverified)",
        .key = 0xD3E4F5A6B7C8D9E0ULL,
    },
    {
        .name = "Porsche Taycan (Unverified)",
        .key = 0xE4F5A6B7C8D9E0F1ULL,
    },
    {
        .name = "Tesla Model 3 (Unverified)",
        .key = 0x4D5E6F7A8B9C0D1EULL,
    },
    {
        .name = "Jaguar I-PACE (Unverified)",
        .key = 0xF5A6B7C8D9E0F1A2ULL,
    },
    {
        .name = "Land Rover Defender (Unverified)",
        .key = 0x06B7C8D9E0F1A2B3ULL,
    },
    {
        .name = "Renault Clio (Unverified)",
        .key = 0x17C8D9E0F1A2B3C4ULL,
    },
    {
        .name = "Peugeot 3008 (Unverified)",
        .key = 0x28D9E0F1A2B3C4D5ULL,
    },
    {
        .name = "Opel Corsa (Unverified)",
        .key = 0x39E0F1A2B3C4D5E6ULL,
    },
    {
        .name = "Fiat 500 (Unverified)",
        .key = 0x4A0F1A2B3C4D5E6F,
    },
    {
        .name = "Hyundai Tucson (Unverified)",
        .key = 0x5B1A2B3C4D5E6F7A,
    },
    {
        .name = "Kia Sportage (Unverified)",
        .key = 0x6C2B3C4D5E6F7A8B,
    },
    {
        .name = "Chevrolet Bolt (Unverified)",
        .key = 0x7D3C4D5E6F7A8B9C,
    },
    {
        .name = "Jeep Wrangler (Unverified)",
        .key = 0x8E4D5E6F7A8B9C0D,
    },
    {
        .name = "Mazda CX-5 (Unverified)",
        .key = 0x9F5E6F7A8B9C0D1E,
    },
    {
        .name = "Mitsubishi Outlander (Unverified)",
        .key = 0xB06F7A8B9C0D1E2F,
    },
    {
        .name = "Subaru Forester (Unverified)",
        .key = 0xC17A8B9C0D1E2F3A,
    },
    {
        .name = "Lexus RX (Unverified)",
        .key = 0xD28B9CAD0D1E2F3A,
    }
};

const size_t manufacturer_keys_count = sizeof(manufacturer_keys) / sizeof(ManufacturerKey);
