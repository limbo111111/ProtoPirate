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
