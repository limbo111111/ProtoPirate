#include "kia_v1.h"
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>

#define TAG "KiaV1"

// OOK PCM 800µs timing
static const SubGhzBlockConst kia_protocol_v1_const = {
    .te_short = 800,
    .te_long = 1600,
    .te_delta = 200,
    .min_count_bit_for_found = 56,
};

struct SubGhzProtocolDecoderKiaV1
{
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint16_t header_count;

    uint8_t raw_bits[24];
    uint16_t raw_bit_count;
};

typedef enum
{
    KiaV1EncoderStepReset = 0,
    KiaV1EncoderStepPreamble,
    KiaV1EncoderStepSync,
    KiaV1EncoderStepData,
    KiaV1EncoderStepStop,
} KiaV1EncoderStep;

struct SubGhzProtocolEncoderKiaV1
{
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    KiaV1EncoderStep step;
    uint8_t preamble_bit_index;
    uint8_t data_bit_index;
    LevelDuration manchester_pulse;
};

typedef enum
{
    KiaV1DecoderStepReset = 0,
    KiaV1DecoderStepCheckPreamble,
    KiaV1DecoderStepFoundShortLow,
    KiaV1DecoderStepCollectRawBits,
} KiaV1DecoderStep;

const SubGhzProtocolDecoder kia_protocol_v1_decoder = {
    .alloc = kia_protocol_decoder_v1_alloc,
    .free = kia_protocol_decoder_v1_free,
    .feed = kia_protocol_decoder_v1_feed,
    .reset = kia_protocol_decoder_v1_reset,
    .get_hash_data = kia_protocol_decoder_v1_get_hash_data,
    .serialize = kia_protocol_decoder_v1_serialize,
    .deserialize = kia_protocol_decoder_v1_deserialize,
    .get_string = kia_protocol_decoder_v1_get_string,
};

// Encoder forward declarations
void *kia_protocol_encoder_v1_alloc(SubGhzEnvironment *environment);
void kia_protocol_encoder_v1_free(void *context);
SubGhzProtocolStatus kia_protocol_encoder_v1_deserialize(void *context, FlipperFormat *flipper_format);
void kia_protocol_encoder_v1_stop(void *context);
LevelDuration kia_protocol_encoder_v1_yield(void *context);

const SubGhzProtocolEncoder kia_protocol_v1_encoder = {
    .alloc = kia_protocol_encoder_v1_alloc,
    .free = kia_protocol_encoder_v1_free,
    .deserialize = kia_protocol_encoder_v1_deserialize,
    .stop = kia_protocol_encoder_v1_stop,
    .yield = kia_protocol_encoder_v1_yield,
};

const SubGhzProtocol kia_protocol_v1 = {
    .name = KIA_PROTOCOL_V1_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM |
            SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Send,
    .decoder = &kia_protocol_v1_decoder,
    .encoder = &kia_protocol_v1_encoder,
};

static void kia_v1_add_raw_bit(SubGhzProtocolDecoderKiaV1 *instance, bool bit)
{
    if (instance->raw_bit_count < 192)
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

static inline bool kia_v1_get_raw_bit(SubGhzProtocolDecoderKiaV1 *instance, uint16_t idx)
{
    uint16_t byte_idx = idx / 8;
    uint8_t bit_idx = 7 - (idx % 8);
    return (instance->raw_bits[byte_idx] >> bit_idx) & 1;
}

static bool kia_v1_manchester_decode(SubGhzProtocolDecoderKiaV1 *instance)
{
    if (instance->raw_bit_count < 113)
    {
        FURI_LOG_D(TAG, "Not enough raw bits: %u", instance->raw_bit_count);
        return false;
    }

    FURI_LOG_D(
        TAG,
        "Raw: %02X %02X %02X %02X %02X %02X",
        instance->raw_bits[0],
        instance->raw_bits[1],
        instance->raw_bits[2],
        instance->raw_bits[3],
        instance->raw_bits[4],
        instance->raw_bits[5]);

    // Try different offsets to find best alignment (RTL-433 uses -1 bit offset)
    uint16_t best_bits = 0;
    uint64_t best_data = 0;
    uint16_t best_offset = 0;

    for (uint16_t offset = 0; offset < 8; offset++)
    {
        uint64_t data = 0;
        uint16_t decoded_bits = 0;

        for (uint16_t i = offset; i + 1 < instance->raw_bit_count && decoded_bits < 56; i += 2)
        {
            bool bit1 = kia_v1_get_raw_bit(instance, i);
            bool bit2 = kia_v1_get_raw_bit(instance, i + 1);

            uint8_t two_bits = (bit1 << 1) | bit2;

            // V1 uses: 10=1, 01=0
            if (two_bits == 0x02)
            { // 10 = decoded 1
                data = (data << 1) | 1;
                decoded_bits++;
            }
            else if (two_bits == 0x01)
            { // 01 = decoded 0
                data = (data << 1);
                decoded_bits++;
            }
            else
            {
                break;
            }
        }

        if (decoded_bits > best_bits)
        {
            best_bits = decoded_bits;
            best_data = data;
            best_offset = offset;
        }
    }

    FURI_LOG_I(TAG, "Best: offset=%u bits=%u data=%014llX", best_offset, best_bits, best_data);

    instance->decoder.decode_data = best_data;
    instance->decoder.decode_count_bit = best_bits;

    return best_bits >= kia_protocol_v1_const.min_count_bit_for_found;
}

void *kia_protocol_decoder_v1_alloc(SubGhzEnvironment *environment)
{
    UNUSED(environment);
    SubGhzProtocolDecoderKiaV1 *instance = malloc(sizeof(SubGhzProtocolDecoderKiaV1));
    instance->base.protocol = &kia_protocol_v1;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void kia_protocol_decoder_v1_free(void *context)
{
    furi_assert(context);
    SubGhzProtocolDecoderKiaV1 *instance = context;
    free(instance);
}

void kia_protocol_decoder_v1_reset(void *context)
{
    furi_assert(context);
    SubGhzProtocolDecoderKiaV1 *instance = context;
    instance->decoder.parser_step = KiaV1DecoderStepReset;
    instance->header_count = 0;
    instance->raw_bit_count = 0;
    memset(instance->raw_bits, 0, sizeof(instance->raw_bits));
}

void kia_protocol_decoder_v1_feed(void *context, bool level, uint32_t duration)
{
    furi_assert(context);
    SubGhzProtocolDecoderKiaV1 *instance = context;

    switch (instance->decoder.parser_step)
    {
    case KiaV1DecoderStepReset:
        // Preamble 0xCCCCCCCD produces alternating LONG pulses
        if ((level) && (DURATION_DIFF(duration, kia_protocol_v1_const.te_long) <
                        kia_protocol_v1_const.te_delta))
        {
            instance->decoder.parser_step = KiaV1DecoderStepCheckPreamble;
            instance->decoder.te_last = duration;
            instance->header_count = 1;
        }
        break;

    case KiaV1DecoderStepCheckPreamble:
        if (level)
        {
            if (DURATION_DIFF(duration, kia_protocol_v1_const.te_long) <
                kia_protocol_v1_const.te_delta)
            {
                instance->decoder.te_last = duration;
                instance->header_count++;
            }
            else if (
                DURATION_DIFF(duration, kia_protocol_v1_const.te_short) <
                kia_protocol_v1_const.te_delta)
            {
                instance->decoder.te_last = duration;
            }
            else
            {
                instance->decoder.parser_step = KiaV1DecoderStepReset;
            }
        }
        else
        {
            // LOW pulse
            if (DURATION_DIFF(duration, kia_protocol_v1_const.te_long) <
                kia_protocol_v1_const.te_delta)
            {
                instance->header_count++;
            }
            else if (
                DURATION_DIFF(duration, kia_protocol_v1_const.te_short) <
                kia_protocol_v1_const.te_delta)
            {
                // Short LOW - this is the start of sync (0xCD ends: ...long H, short L, short H)
                if (instance->header_count > 12)
                {
                    instance->decoder.parser_step = KiaV1DecoderStepFoundShortLow;
                }
            }
            else
            {
                instance->decoder.parser_step = KiaV1DecoderStepReset;
            }
        }
        break;

    case KiaV1DecoderStepFoundShortLow:
        // Expecting SHORT HIGH to complete sync
        if (level && (DURATION_DIFF(duration, kia_protocol_v1_const.te_short) <
                      kia_protocol_v1_const.te_delta))
        {
            FURI_LOG_I(TAG, "Sync! hdr=%u", instance->header_count);
            instance->decoder.parser_step = KiaV1DecoderStepCollectRawBits;
            instance->raw_bit_count = 0;
            memset(instance->raw_bits, 0, sizeof(instance->raw_bits));
            // Add the sync short HIGH as first raw bit
            kia_v1_add_raw_bit(instance, true);
        }
        else
        {
            instance->decoder.parser_step = KiaV1DecoderStepReset;
        }
        break;

    case KiaV1DecoderStepCollectRawBits:
        if (duration > 2400)
        {
            FURI_LOG_I(TAG, "End! raw_bits=%u", instance->raw_bit_count);

            if (kia_v1_manchester_decode(instance))
            {
                instance->generic.data = instance->decoder.decode_data;
                instance->generic.data_count_bit = instance->decoder.decode_count_bit;

                // Extract fields from 56-bit data per RTL-433:
                // Serial: bits 55-24 (32 bits)
                // Btn: bits 23-16 (8 bits)
                // Count: bits 15-8 (8 bits)
                // CRC: bits 7-0 (8 bits)
                instance->generic.serial = (uint32_t)((instance->generic.data >> 24) & 0xFFFFFFFF);
                instance->generic.btn = (uint8_t)((instance->generic.data >> 16) & 0xFF);
                instance->generic.cnt = (uint8_t)((instance->generic.data >> 8) & 0xFF);

                FURI_LOG_I(
                    TAG,
                    "DECODE! Key=%014llX Sn=%08lX Btn=%02X Cnt=%02X",
                    instance->generic.data,
                    instance->generic.serial,
                    instance->generic.btn,
                    (uint8_t)instance->generic.cnt);

                if (instance->base.callback)
                    instance->base.callback(&instance->base, instance->base.context);
            }

            instance->decoder.parser_step = KiaV1DecoderStepReset;
            break;
        }

        int num_bits = 0;
        if (DURATION_DIFF(duration, kia_protocol_v1_const.te_short) <
            kia_protocol_v1_const.te_delta)
        {
            num_bits = 1;
        }
        else if (
            DURATION_DIFF(duration, kia_protocol_v1_const.te_long) <
            kia_protocol_v1_const.te_delta)
        {
            num_bits = 2;
        }
        else
        {
            FURI_LOG_D(
                TAG,
                "Invalid pulse: %s %lu, raw_bits=%u",
                level ? "H" : "L",
                duration,
                instance->raw_bit_count);
            instance->decoder.parser_step = KiaV1DecoderStepReset;
            break;
        }

        for (int i = 0; i < num_bits; i++)
        {
            kia_v1_add_raw_bit(instance, level);
        }

        break;
    }
}

uint8_t kia_protocol_decoder_v1_get_hash_data(void *context)
{
    furi_assert(context);
    SubGhzProtocolDecoderKiaV1 *instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus kia_protocol_decoder_v1_serialize(
    void *context,
    FlipperFormat *flipper_format,
    SubGhzRadioPreset *preset)
{
    furi_assert(context);
    SubGhzProtocolDecoderKiaV1 *instance = context;

    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    ret = subghz_block_generic_serialize(&instance->generic, flipper_format, preset);

    if (ret == SubGhzProtocolStatusOk)
    {
        // Save CRC (last byte)
        uint32_t crc = instance->generic.data & 0xFF;
        flipper_format_write_uint32(flipper_format, "CRC", &crc, 1);

        // Save decoded fields
        flipper_format_write_uint32(flipper_format, "Serial", &instance->generic.serial, 1);

        uint32_t temp = instance->generic.btn;
        flipper_format_write_uint32(flipper_format, "Btn", &temp, 1);

        temp = instance->generic.cnt;
        flipper_format_write_uint32(flipper_format, "Cnt", &temp, 1);
    }

    return ret;
}

SubGhzProtocolStatus
kia_protocol_decoder_v1_deserialize(void *context, FlipperFormat *flipper_format)
{
    furi_assert(context);
    SubGhzProtocolDecoderKiaV1 *instance = context;
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, kia_protocol_v1_const.min_count_bit_for_found);

    if (ret == SubGhzProtocolStatusOk)
    {
        // Kia V1 decoder doesn't fully deserialize, but we can try for completeness
        uint32_t temp_val;
        flipper_format_read_uint32(flipper_format, "Serial", &instance->generic.serial, 1);
        if (flipper_format_read_uint32(flipper_format, "Btn", &temp_val, 1))
        {
            instance->generic.btn = temp_val;
        }
        if (flipper_format_read_uint32(flipper_format, "Cnt", &temp_val, 1))
        {
            instance->generic.cnt = temp_val;
        }
    }
    return ret;
}

void kia_protocol_decoder_v1_get_string(void *context, FuriString *output)
{
    furi_assert(context);
    SubGhzProtocolDecoderKiaV1 *instance = context;

    uint8_t crc = instance->generic.data & 0xFF;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%014llX\r\n"
        "Sn:%08lX Btn:%02X\r\n"
        "Cnt:%02X CRC:%02X\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        instance->generic.data,
        instance->generic.serial,
        instance->generic.btn,
        (uint8_t)instance->generic.cnt,
        crc);
}

// Encoder implementation
void *kia_protocol_encoder_v1_alloc(SubGhzEnvironment *environment)
{
    UNUSED(environment);
    SubGhzProtocolEncoderKiaV1 *instance = malloc(sizeof(SubGhzProtocolEncoderKiaV1));
    instance->base.protocol = &kia_protocol_v1;
    instance->step = KiaV1EncoderStepReset;
    return instance;
}

void kia_protocol_encoder_v1_free(void *context)
{
    furi_assert(context);
    SubGhzProtocolEncoderKiaV1 *instance = context;
    free(instance);
}

SubGhzProtocolStatus kia_protocol_encoder_v1_deserialize(void *context, FlipperFormat *flipper_format)
{
    furi_assert(context);
    SubGhzProtocolEncoderKiaV1 *instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    if (subghz_block_generic_deserialize_check_count_bit(
            &instance->generic, flipper_format, kia_protocol_v1_const.min_count_bit_for_found))
    {
        // No need to read other fields as they are part of the main 56-bit key
        ret = SubGhzProtocolStatusOk;
    }

    if (ret == SubGhzProtocolStatusOk)
    {
        instance->step = KiaV1EncoderStepPreamble;
    }
    return ret;
}

void kia_protocol_encoder_v1_stop(void *context)
{
    furi_assert(context);
    SubGhzProtocolEncoderKiaV1 *instance = context;
    instance->step = KiaV1EncoderStepStop;
}

LevelDuration kia_protocol_encoder_v1_yield(void *context)
{
    furi_assert(context);
    SubGhzProtocolEncoderKiaV1 *instance = context;
    const uint32_t preamble = 0xCCCCCCCD; // 32 bits
    uint32_t te_short = kia_protocol_v1_const.te_short;

    switch (instance->step)
    {
    case KiaV1EncoderStepReset:
        instance->preamble_bit_index = 0;
        instance->data_bit_index = 0;
        instance->manchester_pulse.duration = 0;
        instance->step = KiaV1EncoderStepPreamble;
        // fallthrough

    case KiaV1EncoderStepPreamble:
        if (instance->manchester_pulse.duration > 0)
        {
            LevelDuration pulse = instance->manchester_pulse;
            instance->manchester_pulse.duration = 0;
            return pulse;
        }

        if (instance->preamble_bit_index < 32)
        {
            bool bit = (preamble >> (31 - instance->preamble_bit_index)) & 1;
            instance->preamble_bit_index++;

            if (bit)
            { // 1 -> 10
                instance->manchester_pulse = level_duration_make(false, te_short);
                return level_duration_make(true, te_short);
            }
            else
            { // 0 -> 01
                instance->manchester_pulse = level_duration_make(true, te_short);
                return level_duration_make(false, te_short);
            }
        }
        else
        {
            instance->step = KiaV1EncoderStepData;
        }
        // fallthrough

    case KiaV1EncoderStepData:
        if (instance->manchester_pulse.duration > 0)
        {
            LevelDuration pulse = instance->manchester_pulse;
            instance->manchester_pulse.duration = 0;
            return pulse;
        }
        if (instance->data_bit_index < 56)
        {
            bool bit = (instance->generic.data >> (55 - instance->data_bit_index)) & 1;
            instance->data_bit_index++;
            if (bit)
            {
                instance->manchester_pulse = level_duration_make(false, te_short);
                return level_duration_make(true, te_short);
            }
            else
            {
                instance->manchester_pulse = level_duration_make(true, te_short);
                return level_duration_make(false, te_short);
            }
        }
        else
        {
            instance->step = KiaV1EncoderStepStop;
        }
        // fallthrough
    case KiaV1EncoderStepStop:
        return level_duration_reset();
    case KiaV1EncoderStepSync:
        // Not used in this encoder's state machine, but needs to be handled
        break;
    }
    return level_duration_reset();
}
