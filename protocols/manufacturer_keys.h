#pragma once

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Structure to hold manufacturer key information.
 */
typedef struct {
    const char* name;       /**< Human-readable name of the manufacturer/protocol */
    uint64_t key;           /**< The 64-bit manufacturer key */
} ManufacturerKey;

// Known Keys Constants
#define MANUFACTURER_KEY_KIA_V3_V4          0xA8F5DFFC8DAA5CDBULL
#define MANUFACTURER_KEY_SIMPLE_LEARNING    0x123456789ABCDEF0ULL
#define MANUFACTURER_KEY_NORMAL_LEARNING    0x0123456789ABCDEFULL

/**
 * @brief List of known manufacturer keys (Blacklist).
 *
 * These keys are used for decrypting rolling code protocols like KeeLoq.
 * Note: Possession or use of these keys may be legally restricted in some jurisdictions.
 * This list is for educational and research purposes only.
 */
extern const ManufacturerKey manufacturer_keys[];

/**
 * @brief Number of keys in the manufacturer_keys array.
 */
extern const size_t manufacturer_keys_count;
