#include "kia_v3_v4.h"
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>

#define TAG "KiaV3V4"

static const uint64_t kia_mf_key = 0xA8F5DFFC8DAA5CDB;
static const char *kia_version_names[] = {"Kia V4", "Kia V3"};

static const SubGhzBlockConst kia_protocol_v3_v4_const = {
    .te_short = 400,
    .te_long = 800,
    .te_delta = 150,
    .min_count_bit_for_found = 64,
};

typedef struct SubGhzProtocolDecoderKiaV3V4
{
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint16_t header_count;

    uint8_t raw_bits[32];
    uint16_t raw_bit_count;
    bool is_v3_sync; // true = V3 (long LOW sync), false = V4 (long HIGH sync)

    uint32_t encrypted;
    uint32_t decrypted;
    uint8_t version; // 0 = V4, 1 = V3
} SubGhzProtocolDecoderKiaV3V4;

typedef enum
{
    KiaV3V4EncoderStepReset = 0,
    KiaV3V4EncoderStepPreamble,
    KiaV3V4EncoderStepSync,
    KiaV3V4EncoderStepData,
    KiaV3V4EncoderStepStop,
} KiaV3V4EncoderStep;

typedef struct SubGhzProtocolEncoderKiaV3V4
{
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    KiaV3V4EncoderStep step;
    uint8_t preamble_count;
    uint8_t data_bit_index;
    bool is_v3_sync; // To match decoder's sync behavior

    uint32_t encrypted;
    uint32_t decrypted;
    uint8_t version;
} SubGhzProtocolEncoderKiaV3V4;

typedef enum
{
    KiaV3V4DecoderStepReset = 0,
    KiaV3V4DecoderStepCheckPreamble,
    KiaV3V4DecoderStepCollectRawBits,
} KiaV3V4DecoderStep;

// KeeLoq decrypt
static uint32_t keeloq_common_decrypt(uint32_t data, uint64_t key)
{
    uint32_t block = data;
    uint64_t tkey = key;
    for (int i = 0; i < 528; i++)
    {
        int lutkey = ((block >> 0) & 1) | ((block >> 7) & 2) | ((block >> 17) & 4) |
                     ((block >> 22) & 8) | ((block >> 26) & 16);
        int lsb =
            ((block >> 31) ^ ((block >> 15) & 1) ^ ((0x3A5C742E >> lutkey) & 1) ^
             ((tkey >> 15) & 1));
        block = ((block & 0x7FFFFFFF) << 1) | lsb;
        tkey = ((tkey & 0x7FFFFFFFFFFFFFFFULL) << 1) | (tkey >> 63);
    }
    return block;
}

// KeeLoq encrypt
static uint32_t keeloq_common_encrypt(uint32_t data, uint64_t key)
{
    uint32_t block = data;
    uint64_t tkey = key;
    for (int i = 0; i < 528; i++)
    {
        int lutkey = ((block >> 1) & 1) | ((block >> 8) & 2) | ((block >> 18) & 4) |
                     ((block >> 23) & 8) | ((block >> 27) & 16);
        int msb =
            (((block >> 0) & 1) ^ ((block >> 16) & 1) ^ ((0x3A5C742E >> lutkey) & 1) ^
             ((tkey >> 0) & 1));
        block = (block >> 1) | (msb << 31);
        tkey = (tkey >> 1) | ((tkey & 1) << 63);
    }
    return block;
}

static uint8_t reverse8(uint8_t byte)
{
    byte = (byte & 0xF0) >> 4 | (byte & 0x0F) << 4;
    byte = (byte & 0xCC) >> 2 | (byte & 0x33) << 2;
    byte = (byte & 0xAA) >> 1 | (byte & 0x55) << 1;
    return byte;
}

static void kia_v3_v4_add_raw_bit(SubGhzProtocolDecoderKiaV3V4 *instance, bool bit)
{
    if (instance->raw_bit_count < 256)
    {
        uint16_t byte_idx = instance->raw_bit_count / 8;
        uint8_t bit_idx = 7 - (instance->raw_bit_count % 8);
        if (bit)
        {
            instance->raw_bits[byte_idx] |= (1 << bit_idx);
        }
        else
        {
            instance->raw_bits[byte_idx] &= ~(1 << bit_idx);
        }
        instance->raw_bit_count++;
    }
}

static bool kia_v3_v4_process_buffer(SubGhzProtocolDecoderKiaV3V4 *instance)
{
    if (instance->raw_bit_count < 64)
    {
        return false;
    }

    uint8_t *b = instance->raw_bits;

    // For V3-style (long LOW sync), data is inverted
    if (instance->is_v3_sync)
    {
        uint16_t num_bytes = (instance->raw_bit_count + 7) / 8;
        for (uint16_t i = 0; i < num_bytes; i++)
        {
            b[i] = ~b[i];
        }
    }

    // Extract fields
    uint32_t encrypted = ((uint32_t)reverse8(b[3]) << 24) | ((uint32_t)reverse8(b[2]) << 16) |
                         ((uint32_t)reverse8(b[1]) << 8) | (uint32_t)reverse8(b[0]);

    uint32_t serial = ((uint32_t)reverse8(b[7] & 0xF0) << 24) | ((uint32_t)reverse8(b[6]) << 16) |
                      ((uint32_t)reverse8(b[5]) << 8) | (uint32_t)reverse8(b[4]);

    uint8_t btn = (reverse8(b[7]) & 0xF0) >> 4;
    uint8_t our_serial_lsb = serial & 0xFF;

    // Decrypt
    uint32_t decrypted = keeloq_common_decrypt(encrypted, kia_mf_key);
    uint8_t dec_btn = (decrypted >> 28) & 0x0F;
    uint8_t dec_serial_lsb = (decrypted >> 16) & 0xFF;

    // Validate
    if (dec_btn != btn || dec_serial_lsb != our_serial_lsb)
    {
        return false;
    }

    // Valid decode - version determined by sync type
    instance->encrypted = encrypted;
    instance->decrypted = decrypted;
    instance->generic.serial = serial;
    instance->generic.btn = btn;
    instance->generic.cnt = decrypted & 0xFFFF;
    instance->version = instance->is_v3_sync ? 1 : 0;

    uint64_t key_data = ((uint64_t)b[0] << 56) | ((uint64_t)b[1] << 48) | ((uint64_t)b[2] << 40) |
                        ((uint64_t)b[3] << 32) | ((uint64_t)b[4] << 24) | ((uint64_t)b[5] << 16) |
                        ((uint64_t)b[6] << 8) | (uint64_t)b[7];
    instance->generic.data = key_data;
    instance->generic.data_count_bit = 64;

    return true;
}

const SubGhzProtocolDecoder kia_protocol_v3_v4_decoder = {
    .alloc = kia_protocol_decoder_v3_v4_alloc,
    .free = kia_protocol_decoder_v3_v4_free,
    .feed = kia_protocol_decoder_v3_v4_feed,
    .reset = kia_protocol_decoder_v3_v4_reset,
    .get_hash_data = kia_protocol_decoder_v3_v4_get_hash_data,
    .serialize = kia_protocol_decoder_v3_v4_serialize,
    .deserialize = kia_protocol_decoder_v3_v4_deserialize,
    .get_string = kia_protocol_decoder_v3_v4_get_string,
};

// Encoder forward declarations
void *kia_protocol_encoder_v3_v4_alloc(SubGhzEnvironment *environment);
void kia_protocol_encoder_v3_v4_free(void *context);
SubGhzProtocolStatus kia_protocol_encoder_v3_v4_deserialize(void *context, FlipperFormat *flipper_format);
void kia_protocol_encoder_v3_v4_stop(void *context);
LevelDuration kia_protocol_encoder_v3_v4_yield(void *context);
static void subghz_protocol_kia_v3_v4_encrypt_and_assemble(SubGhzProtocolEncoderKiaV3V4 *instance);

const SubGhzProtocolEncoder kia_protocol_v3_v4_encoder = {
    .alloc = kia_protocol_encoder_v3_v4_alloc,
    .free = kia_protocol_encoder_v3_v4_free,
    .deserialize = kia_protocol_encoder_v3_v4_deserialize,
    .stop = kia_protocol_encoder_v3_v4_stop,
    .yield = kia_protocol_encoder_v3_v4_yield,
};

const SubGhzProtocol kia_protocol_v3_v4 = {
    .name = KIA_PROTOCOL_V3_V4_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Send,
    .decoder = &kia_protocol_v3_v4_decoder,
    .encoder = &kia_protocol_v3_v4_encoder,
};

void *kia_protocol_decoder_v3_v4_alloc(SubGhzEnvironment *environment)
{
    UNUSED(environment);
    SubGhzProtocolDecoderKiaV3V4 *instance = malloc(sizeof(SubGhzProtocolDecoderKiaV3V4));
    instance->base.protocol = &kia_protocol_v3_v4;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void kia_protocol_decoder_v3_v4_free(void *context)
{
    furi_assert(context);
    SubGhzProtocolDecoderKiaV3V4 *instance = context;
    free(instance);
}

void kia_protocol_decoder_v3_v4_reset(void *context)
{
    furi_assert(context);
    SubGhzProtocolDecoderKiaV3V4 *instance = context;
    instance->decoder.parser_step = KiaV3V4DecoderStepReset;
    instance->header_count = 0;
    instance->raw_bit_count = 0;
    memset(instance->raw_bits, 0, sizeof(instance->raw_bits));
}

void kia_protocol_decoder_v3_v4_feed(void *context, bool level, uint32_t duration)
{
    furi_assert(context);
    SubGhzProtocolDecoderKiaV3V4 *instance = context;

    switch (instance->decoder.parser_step)
    {
    case KiaV3V4DecoderStepReset:
        if (level && DURATION_DIFF(duration, kia_protocol_v3_v4_const.te_short) <
                         kia_protocol_v3_v4_const.te_delta)
        {
            instance->decoder.parser_step = KiaV3V4DecoderStepCheckPreamble;
            instance->decoder.te_last = duration;
            instance->header_count = 1;
        }
        break;

    case KiaV3V4DecoderStepCheckPreamble:
        if (level)
        {
            if (DURATION_DIFF(duration, kia_protocol_v3_v4_const.te_short) <
                kia_protocol_v3_v4_const.te_delta)
            {
                instance->decoder.te_last = duration;
            }
            else if (duration > 1000 && duration < 1500)
            {
                // V4 style: Sync is LONG HIGH
                if (instance->header_count >= 8)
                {
                    instance->decoder.parser_step = KiaV3V4DecoderStepCollectRawBits;
                    instance->raw_bit_count = 0;
                    instance->is_v3_sync = false;
                    memset(instance->raw_bits, 0, sizeof(instance->raw_bits));
                }
                else
                {
                    instance->decoder.parser_step = KiaV3V4DecoderStepReset;
                }
            }
            else
            {
                instance->decoder.parser_step = KiaV3V4DecoderStepReset;
            }
        }
        else
        {
            if (duration > 1000 && duration < 1500)
            {
                // V3 style: Sync is LONG LOW
                if (instance->header_count >= 8)
                {
                    instance->decoder.parser_step = KiaV3V4DecoderStepCollectRawBits;
                    instance->raw_bit_count = 0;
                    instance->is_v3_sync = true;
                    memset(instance->raw_bits, 0, sizeof(instance->raw_bits));
                }
                else
                {
                    instance->decoder.parser_step = KiaV3V4DecoderStepReset;
                }
            }
            else if (
                DURATION_DIFF(duration, kia_protocol_v3_v4_const.te_short) <
                    kia_protocol_v3_v4_const.te_delta &&
                DURATION_DIFF(instance->decoder.te_last, kia_protocol_v3_v4_const.te_short) <
                    kia_protocol_v3_v4_const.te_delta)
            {
                instance->header_count++;
            }
            else if (duration > 1500)
            {
                instance->decoder.parser_step = KiaV3V4DecoderStepReset;
            }
        }
        break;

    case KiaV3V4DecoderStepCollectRawBits:
        if (level)
        {
            if (duration > 1000 && duration < 1500)
            {
                // Next sync pulse (V4 style) - end this packet
                if (kia_v3_v4_process_buffer(instance))
                {
                    if (instance->base.callback)
                        instance->base.callback(&instance->base, instance->base.context);
                }
                instance->decoder.parser_step = KiaV3V4DecoderStepReset;
            }
            else if (
                DURATION_DIFF(duration, kia_protocol_v3_v4_const.te_short) <
                kia_protocol_v3_v4_const.te_delta)
            {
                kia_v3_v4_add_raw_bit(instance, false);
            }
            else if (
                DURATION_DIFF(duration, kia_protocol_v3_v4_const.te_long) <
                kia_protocol_v3_v4_const.te_delta)
            {
                kia_v3_v4_add_raw_bit(instance, true);
            }
            else
            {
                instance->decoder.parser_step = KiaV3V4DecoderStepReset;
            }
        }
        else
        {
            if (duration > 1000 && duration < 1500)
            {
                // Next sync pulse (V3 style) - end this packet
                if (kia_v3_v4_process_buffer(instance))
                {
                    if (instance->base.callback)
                        instance->base.callback(&instance->base, instance->base.context);
                }
                instance->decoder.parser_step = KiaV3V4DecoderStepReset;
            }
            else if (duration > 1500)
            {
                // Long gap - end of transmission
                if (kia_v3_v4_process_buffer(instance))
                {
                    if (instance->base.callback)
                        instance->base.callback(&instance->base, instance->base.context);
                }
                instance->decoder.parser_step = KiaV3V4DecoderStepReset;
            }
        }
        break;
    }
}

uint8_t kia_protocol_decoder_v3_v4_get_hash_data(void *context)
{
    furi_assert(context);
    SubGhzProtocolDecoderKiaV3V4 *instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus kia_protocol_decoder_v3_v4_serialize(
    void *context,
    FlipperFormat *flipper_format,
    SubGhzRadioPreset *preset)
{
    furi_assert(context);
    SubGhzProtocolDecoderKiaV3V4 *instance = context;

    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    ret = subghz_block_generic_serialize(&instance->generic, flipper_format, preset);

    if (ret == SubGhzProtocolStatusOk)
    {
        flipper_format_write_uint32(flipper_format, "Encrypted", &instance->encrypted, 1);
        flipper_format_write_uint32(flipper_format, "Decrypted", &instance->decrypted, 1);

        uint32_t temp = instance->version;
        flipper_format_write_uint32(flipper_format, "Version", &temp, 1);
    }

    return ret;
}

SubGhzProtocolStatus
kia_protocol_decoder_v3_v4_deserialize(void *context, FlipperFormat *flipper_format)
{
    furi_assert(context);
    SubGhzProtocolDecoderKiaV3V4 *instance = context;
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, kia_protocol_v3_v4_const.min_count_bit_for_found);

    if (ret == SubGhzProtocolStatusOk)
    {
        // Read extra fields for display and serialization
        flipper_format_read_uint32(flipper_format, "Encrypted", &instance->encrypted, 1);
        flipper_format_read_uint32(flipper_format, "Decrypted", &instance->decrypted, 1);
        uint32_t temp_version;
        if (flipper_format_read_uint32(flipper_format, "Version", &temp_version, 1))
        {
            instance->version = temp_version;
        }
    }
    return ret;
}

void kia_protocol_decoder_v3_v4_get_string(void *context, FuriString *output)
{
    furi_assert(context);
    SubGhzProtocolDecoderKiaV3V4 *instance = context;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%016llX\r\n"
        "Sn:%07lX Btn:%X Cnt:%04lX\r\n"
        "Enc:%08lX Dec:%08lX\r\n",
        kia_version_names[instance->version],
        instance->generic.data_count_bit,
        instance->generic.data,
        instance->generic.serial,
        instance->generic.btn,
        instance->generic.cnt,
        instance->encrypted,
        instance->decrypted);
}

// Encoder implementation
static void subghz_protocol_kia_v3_v4_encrypt_and_assemble(SubGhzProtocolEncoderKiaV3V4 *instance)
{
    uint32_t decrypted =
        (instance->generic.btn << 28) |
        ((instance->generic.serial & 0xFF) << 16) |
        instance->generic.cnt;

    uint32_t encrypted = keeloq_common_encrypt(decrypted, kia_mf_key);

    uint8_t b[8] = {0};
    b[0] = reverse8((uint8_t)(encrypted & 0xFF));
    b[1] = reverse8((uint8_t)((encrypted >> 8) & 0xFF));
    b[2] = reverse8((uint8_t)((encrypted >> 16) & 0xFF));
    b[3] = reverse8((uint8_t)((encrypted >> 24) & 0xFF));
    b[4] = reverse8((uint8_t)(instance->generic.serial & 0xFF));
    b[5] = reverse8((uint8_t)((instance->generic.serial >> 8) & 0xFF));
    b[6] = reverse8((uint8_t)((instance->generic.serial >> 16) & 0xFF));
    b[7] = reverse8(
        ((uint8_t)((instance->generic.serial >> 24) & 0xFF) & 0x0F) |
        (instance->generic.btn << 4));

    instance->generic.data = 0;
    for (int i = 0; i < 8; i++)
    {
        instance->generic.data |= ((uint64_t)b[i] << (i * 8));
    }

    if (instance->is_v3_sync)
    {
        instance->generic.data = ~instance->generic.data;
    }

    instance->encrypted = encrypted;
    instance->decrypted = decrypted;
}

void *kia_protocol_encoder_v3_v4_alloc(SubGhzEnvironment *environment)
{
    UNUSED(environment);
    SubGhzProtocolEncoderKiaV3V4 *instance = malloc(sizeof(SubGhzProtocolEncoderKiaV3V4));
    instance->base.protocol = &kia_protocol_v3_v4;
    instance->step = KiaV3V4EncoderStepReset;
    return instance;
}

void kia_protocol_encoder_v3_v4_free(void *context)
{
    furi_assert(context);
    SubGhzProtocolEncoderKiaV3V4 *instance = context;
    free(instance);
}

SubGhzProtocolStatus kia_protocol_encoder_v3_v4_deserialize(void *context, FlipperFormat *flipper_format)
{
    furi_assert(context);
    SubGhzProtocolEncoderKiaV3V4 *instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    if (subghz_block_generic_deserialize_check_count_bit(
            &instance->generic, flipper_format, kia_protocol_v3_v4_const.min_count_bit_for_found))
    {
        uint32_t temp_val;
        if (!flipper_format_read_uint32(flipper_format, "Version", &temp_val, 1))
            return SubGhzProtocolStatusError;
        instance->version = temp_val;

        if (!flipper_format_read_uint32(flipper_format, "Encrypted", &instance->encrypted, 1))
            return SubGhzProtocolStatusError;
        if (!flipper_format_read_uint32(flipper_format, "Decrypted", &instance->decrypted, 1))
            return SubGhzProtocolStatusError;

        // Reconstruct fields from decrypted
        instance->generic.cnt = instance->decrypted & 0xFFFF;
        instance->generic.btn = (instance->decrypted >> 28) & 0x0F;
        // Serial is needed in full from generic.data
        uint64_t d = instance->generic.data;
        instance->generic.serial = ((uint32_t)reverse8((d >> 60) & 0x0F) << 24) |
                                   ((uint32_t)reverse8((d >> 48) & 0xFF) << 16) |
                                   ((uint32_t)reverse8((d >> 40) & 0xFF) << 8) |
                                   (uint32_t)reverse8((d >> 32) & 0xFF);

        instance->is_v3_sync = (instance->version == 1);
        subghz_protocol_kia_v3_v4_encrypt_and_assemble(instance);
        ret = SubGhzProtocolStatusOk;
    }
    return ret;
}

void kia_protocol_encoder_v3_v4_stop(void *context)
{
    furi_assert(context);
    SubGhzProtocolEncoderKiaV3V4 *instance = context;
    instance->step = KiaV3V4EncoderStepStop;
}

LevelDuration kia_protocol_encoder_v3_v4_yield(void *context)
{
    furi_assert(context);
    SubGhzProtocolEncoderKiaV3V4 *instance = context;

    uint32_t te_short = kia_protocol_v3_v4_const.te_short;
    uint32_t te_long = kia_protocol_v3_v4_const.te_long;
    uint32_t sync_duration = 1200;

    switch (instance->step)
    {
    case KiaV3V4EncoderStepReset:
        instance->preamble_count = 0;
        instance->data_bit_index = 0;
        instance->step = KiaV3V4EncoderStepPreamble;
        // fallthrough
    case KiaV3V4EncoderStepPreamble:
        if (instance->preamble_count < 16)
        { // 8 pairs of short H/L
            if (instance->preamble_count % 2 == 0)
            {
                instance->preamble_count++;
                return level_duration_make(true, te_short);
            }
            else
            {
                instance->preamble_count++;
                return level_duration_make(false, te_short);
            }
        }
        else
        {
            instance->step = KiaV3V4EncoderStepSync;
        }
        // fallthrough
    case KiaV3V4EncoderStepSync:
        instance->step = KiaV3V4EncoderStepData;
        if (instance->is_v3_sync)
        {
            return level_duration_make(false, sync_duration); // V3: long LOW
        }
        else
        {
            return level_duration_make(true, sync_duration); // V4: long HIGH
        }

    case KiaV3V4EncoderStepData:
        if (instance->data_bit_index < 128)
        {
            if (instance->data_bit_index % 2 == 0) {
                bool bit = (instance->generic.data >> (instance->data_bit_index / 2)) & 1;
                instance->data_bit_index++;
                if (bit)
                { // bit 1 is long H
                    return level_duration_make(true, te_long);
                }
                else
                { // bit 0 is short H
                    return level_duration_make(true, te_short);
                }
            } else {
                instance->data_bit_index++;
                // Return short L pulse
                return level_duration_make(false, te_short);
            }
        }
        else
        {
            instance->step = KiaV3V4EncoderStepStop;
        }
        // fallthrough
    case KiaV3V4EncoderStepStop:
        return level_duration_reset();
    }
    return level_duration_reset();
}
