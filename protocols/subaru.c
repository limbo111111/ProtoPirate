#include "subaru.h"
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>

#define TAG "SubaruProtocol"

static const SubGhzBlockConst subghz_protocol_subaru_const = {
    .te_short = 800,
    .te_long = 1600,
    .te_delta = 250,
    .min_count_bit_for_found = 64,
};

typedef struct SubGhzProtocolDecoderSubaru
{
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint16_t header_count;
    uint16_t bit_count;
    uint8_t data[8];

    uint64_t key;
    uint32_t serial;
    uint8_t button;
    uint16_t count;
} SubGhzProtocolDecoderSubaru;

typedef enum
{
    SubaruEncoderStepReset = 0,
    SubaruEncoderStepPreamble,
    SubaruEncoderStepGap,
    SubaruEncoderStepSync,
    SubaruEncoderStepData,
    SubaruEncoderStepStop,
} SubaruEncoderStep;

typedef struct SubGhzProtocolEncoderSubaru
{
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    SubaruEncoderStep step;
    uint8_t preamble_count;
    uint8_t data_bit_index;
} SubGhzProtocolEncoderSubaru;

typedef enum
{
    SubaruDecoderStepReset = 0,
    SubaruDecoderStepCheckPreamble,
    SubaruDecoderStepFoundGap,
    SubaruDecoderStepFoundSync,
    SubaruDecoderStepSaveDuration,
    SubaruDecoderStepCheckDuration,
} SubaruDecoderStep;

const SubGhzProtocolDecoder subghz_protocol_subaru_decoder = {
    .alloc = subghz_protocol_decoder_subaru_alloc,
    .free = subghz_protocol_decoder_subaru_free,
    .feed = subghz_protocol_decoder_subaru_feed,
    .reset = subghz_protocol_decoder_subaru_reset,
    .get_hash_data = subghz_protocol_decoder_subaru_get_hash_data,
    .serialize = subghz_protocol_decoder_subaru_serialize,
    .deserialize = subghz_protocol_decoder_subaru_deserialize,
    .get_string = subghz_protocol_decoder_subaru_get_string,
};

// Encoder forward declarations
void *subghz_protocol_encoder_subaru_alloc(SubGhzEnvironment *environment);
void subghz_protocol_encoder_subaru_free(void *context);
SubGhzProtocolStatus subghz_protocol_encoder_subaru_deserialize(void *context, FlipperFormat *flipper_format);
void subghz_protocol_encoder_subaru_stop(void *context);
LevelDuration subghz_protocol_encoder_subaru_yield(void *context);
static void subaru_encode_count(const uint32_t serial, const uint16_t count, uint8_t *out_bytes);

const SubGhzProtocolEncoder subghz_protocol_subaru_encoder = {
    .alloc = subghz_protocol_encoder_subaru_alloc,
    .free = subghz_protocol_encoder_subaru_free,
    .deserialize = subghz_protocol_encoder_subaru_deserialize,
    .stop = subghz_protocol_encoder_subaru_stop,
    .yield = subghz_protocol_encoder_subaru_yield,
};

const SubGhzProtocol subaru_protocol = {
    .name = SUBARU_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_subaru_decoder,
    .encoder = &subghz_protocol_subaru_encoder,
};

static void subaru_decode_count(const uint8_t *KB, uint16_t *count)
{
    uint8_t lo = 0;
    if ((KB[4] & 0x40) == 0)
        lo |= 0x01;
    if ((KB[4] & 0x80) == 0)
        lo |= 0x02;
    if ((KB[5] & 0x01) == 0)
        lo |= 0x04;
    if ((KB[5] & 0x02) == 0)
        lo |= 0x08;
    if ((KB[6] & 0x01) == 0)
        lo |= 0x10;
    if ((KB[6] & 0x02) == 0)
        lo |= 0x20;
    if ((KB[5] & 0x40) == 0)
        lo |= 0x40;
    if ((KB[5] & 0x80) == 0)
        lo |= 0x80;

    uint8_t REG_SH1 = (KB[7] << 4) & 0xF0;
    if (KB[5] & 0x04)
        REG_SH1 |= 0x04;
    if (KB[5] & 0x08)
        REG_SH1 |= 0x08;
    if (KB[6] & 0x80)
        REG_SH1 |= 0x02;
    if (KB[6] & 0x40)
        REG_SH1 |= 0x01;

    uint8_t REG_SH2 = ((KB[6] << 2) & 0xF0) | ((KB[7] >> 4) & 0x0F);

    uint8_t SER0 = KB[3];
    uint8_t SER1 = KB[1];
    uint8_t SER2 = KB[2];

    uint8_t total_rot = 4 + lo;
    for (uint8_t i = 0; i < total_rot; ++i)
    {
        uint8_t t_bit = (SER0 >> 7) & 1;
        SER0 = ((SER0 << 1) & 0xFE) | ((SER1 >> 7) & 1);
        SER1 = ((SER1 << 1) & 0xFE) | ((SER2 >> 7) & 1);
        SER2 = ((SER2 << 1) & 0xFE) | t_bit;
    }

    uint8_t T1 = SER1 ^ REG_SH1;
    uint8_t T2 = SER2 ^ REG_SH2;

    uint8_t hi = 0;
    if ((T1 & 0x10) == 0)
        hi |= 0x04;
    if ((T1 & 0x20) == 0)
        hi |= 0x08;
    if ((T2 & 0x80) == 0)
        hi |= 0x02;
    if ((T2 & 0x40) == 0)
        hi |= 0x01;
    if ((T1 & 0x01) == 0)
        hi |= 0x40;
    if ((T1 & 0x02) == 0)
        hi |= 0x80;
    if ((T2 & 0x08) == 0)
        hi |= 0x20;
    if ((T2 & 0x04) == 0)
        hi |= 0x10;

    *count = ((hi << 8) | lo) & 0xFFFF;
}

static void subaru_encode_count(const uint32_t serial, const uint16_t count, uint8_t *out_bytes)
{
    uint8_t lo = count & 0xFF;
    uint8_t hi = (count >> 8) & 0xFF;

    uint8_t T1 = 0, T2 = 0;
    if ((hi & 0x04) == 0)
        T1 |= 0x10;
    if ((hi & 0x08) == 0)
        T1 |= 0x20;
    if ((hi & 0x02) == 0)
        T2 |= 0x80;
    if ((hi & 0x01) == 0)
        T2 |= 0x40;
    if ((hi & 0x40) == 0)
        T1 |= 0x01;
    if ((hi & 0x80) == 0)
        T1 |= 0x02;
    if ((hi & 0x20) == 0)
        T2 |= 0x08;
    if ((hi & 0x10) == 0)
        T2 |= 0x04;

    uint8_t SER0_enc = (serial >> 0) & 0xFF;
    uint8_t SER1_enc = (serial >> 16) & 0xFF;
    uint8_t SER2_enc = (serial >> 8) & 0xFF;

    uint8_t total_rot = 4 + lo;
    for (uint8_t i = 0; i < total_rot; ++i)
    {
        uint8_t t_bit = SER2_enc & 1;
        SER2_enc = (SER2_enc >> 1) | ((SER1_enc & 1) << 7);
        SER1_enc = (SER1_enc >> 1) | ((SER0_enc & 1) << 7);
        SER0_enc = (SER0_enc >> 1) | (t_bit << 7);
    }

    uint8_t REG_SH1_enc = T1 ^ SER1_enc;
    uint8_t REG_SH2_enc = T2 ^ SER2_enc;

    out_bytes[4] = 0;
    out_bytes[5] = 0;
    out_bytes[6] = 0;
    out_bytes[7] = 0;

    if ((lo & 0x01) == 0)
        out_bytes[4] |= 0x40;
    if ((lo & 0x02) == 0)
        out_bytes[4] |= 0x80;
    if ((lo & 0x04) == 0)
        out_bytes[5] |= 0x01;
    if ((lo & 0x08) == 0)
        out_bytes[5] |= 0x02;
    if ((lo & 0x10) == 0)
        out_bytes[6] |= 0x01;
    if ((lo & 0x20) == 0)
        out_bytes[6] |= 0x02;
    if ((lo & 0x40) == 0)
        out_bytes[5] |= 0x40;
    if ((lo & 0x80) == 0)
        out_bytes[5] |= 0x80;

    if (REG_SH1_enc & 0x04)
        out_bytes[5] |= 0x04;
    if (REG_SH1_enc & 0x08)
        out_bytes[5] |= 0x08;
    if (REG_SH1_enc & 0x02)
        out_bytes[6] |= 0x80;
    if (REG_SH1_enc & 0x01)
        out_bytes[6] |= 0x40;

    out_bytes[6] |= (REG_SH2_enc & 0xF0) >> 2;
    out_bytes[7] |= (REG_SH1_enc & 0xF0) >> 4;
    out_bytes[7] |= (REG_SH2_enc & 0x0F) << 4;
}

static void subaru_add_bit(SubGhzProtocolDecoderSubaru *instance, bool bit)
{
    if (instance->bit_count < 64)
    {
        uint8_t byte_idx = instance->bit_count / 8;
        uint8_t bit_idx = 7 - (instance->bit_count % 8);
        if (bit)
        {
            instance->data[byte_idx] |= (1 << bit_idx);
        }
        else
        {
            instance->data[byte_idx] &= ~(1 << bit_idx);
        }
        instance->bit_count++;
    }
}

static bool subaru_process_data(SubGhzProtocolDecoderSubaru *instance)
{
    if (instance->bit_count < 64)
    {
        return false;
    }

    uint8_t *b = instance->data;

    instance->key = ((uint64_t)b[0] << 56) | ((uint64_t)b[1] << 48) |
                    ((uint64_t)b[2] << 40) | ((uint64_t)b[3] << 32) |
                    ((uint64_t)b[4] << 24) | ((uint64_t)b[5] << 16) |
                    ((uint64_t)b[6] << 8) | ((uint64_t)b[7]);

    instance->serial = ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    instance->button = b[0] & 0x0F;
    subaru_decode_count(b, &instance->count);

    return true;
}

void *subghz_protocol_decoder_subaru_alloc(SubGhzEnvironment *environment)
{
    UNUSED(environment);
    SubGhzProtocolDecoderSubaru *instance = malloc(sizeof(SubGhzProtocolDecoderSubaru));
    instance->base.protocol = &subaru_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_subaru_free(void *context)
{
    furi_assert(context);
    SubGhzProtocolDecoderSubaru *instance = context;
    free(instance);
}

void subghz_protocol_decoder_subaru_reset(void *context)
{
    furi_assert(context);
    SubGhzProtocolDecoderSubaru *instance = context;
    instance->decoder.parser_step = SubaruDecoderStepReset;
    instance->decoder.te_last = 0;
    instance->header_count = 0;
    instance->bit_count = 0;
    memset(instance->data, 0, sizeof(instance->data));
}

void subghz_protocol_decoder_subaru_feed(void *context, bool level, uint32_t duration)
{
    furi_assert(context);
    SubGhzProtocolDecoderSubaru *instance = context;

    switch (instance->decoder.parser_step)
    {
    case SubaruDecoderStepReset:
        if (level && DURATION_DIFF(duration, subghz_protocol_subaru_const.te_long) < subghz_protocol_subaru_const.te_delta)
        {
            instance->decoder.parser_step = SubaruDecoderStepCheckPreamble;
            instance->decoder.te_last = duration;
            instance->header_count = 1;
        }
        break;

    case SubaruDecoderStepCheckPreamble:
        if (!level)
        {
            if (DURATION_DIFF(duration, subghz_protocol_subaru_const.te_long) < subghz_protocol_subaru_const.te_delta)
            {
                instance->header_count++;
            }
            else if (duration > 2000 && duration < 3500)
            {
                if (instance->header_count > 20)
                {
                    instance->decoder.parser_step = SubaruDecoderStepFoundGap;
                }
                else
                {
                    instance->decoder.parser_step = SubaruDecoderStepReset;
                }
            }
            else
            {
                instance->decoder.parser_step = SubaruDecoderStepReset;
            }
        }
        else
        {
            if (DURATION_DIFF(duration, subghz_protocol_subaru_const.te_long) < subghz_protocol_subaru_const.te_delta)
            {
                instance->decoder.te_last = duration;
                instance->header_count++;
            }
            else
            {
                instance->decoder.parser_step = SubaruDecoderStepReset;
            }
        }
        break;

    case SubaruDecoderStepFoundGap:
        if (level && duration > 2000 && duration < 3500)
        {
            instance->decoder.parser_step = SubaruDecoderStepFoundSync;
        }
        else
        {
            instance->decoder.parser_step = SubaruDecoderStepReset;
        }
        break;

    case SubaruDecoderStepFoundSync:
        if (!level && DURATION_DIFF(duration, subghz_protocol_subaru_const.te_long) < subghz_protocol_subaru_const.te_delta)
        {
            instance->decoder.parser_step = SubaruDecoderStepSaveDuration;
            instance->bit_count = 0;
            memset(instance->data, 0, sizeof(instance->data));
        }
        else
        {
            instance->decoder.parser_step = SubaruDecoderStepReset;
        }
        break;

    case SubaruDecoderStepSaveDuration:
        if (level)
        {
            // HIGH pulse duration encodes the bit:
            // Short HIGH (~800µs) = 1
            // Long HIGH (~1600µs) = 0
            if (DURATION_DIFF(duration, subghz_protocol_subaru_const.te_short) < subghz_protocol_subaru_const.te_delta)
            {
                // Short HIGH = bit 1
                subaru_add_bit(instance, true);
                instance->decoder.te_last = duration;
                instance->decoder.parser_step = SubaruDecoderStepCheckDuration;
            }
            else if (DURATION_DIFF(duration, subghz_protocol_subaru_const.te_long) < subghz_protocol_subaru_const.te_delta)
            {
                // Long HIGH = bit 0
                subaru_add_bit(instance, false);
                instance->decoder.te_last = duration;
                instance->decoder.parser_step = SubaruDecoderStepCheckDuration;
            }
            else if (duration > 3000)
            {
                // End of transmission
                if (instance->bit_count >= 64)
                {
                    if (subaru_process_data(instance))
                    {
                        instance->generic.data = instance->key;
                        instance->generic.data_count_bit = 64;
                        instance->generic.serial = instance->serial;
                        instance->generic.btn = instance->button;
                        instance->generic.cnt = instance->count;

                        if (instance->base.callback)
                        {
                            instance->base.callback(&instance->base, instance->base.context);
                        }
                    }
                }
                instance->decoder.parser_step = SubaruDecoderStepReset;
            }
            else
            {
                instance->decoder.parser_step = SubaruDecoderStepReset;
            }
        }
        else
        {
            instance->decoder.parser_step = SubaruDecoderStepReset;
        }
        break;

    case SubaruDecoderStepCheckDuration:
        if (!level)
        {
            // LOW pulse - just validates timing, doesn't encode bit
            if (DURATION_DIFF(duration, subghz_protocol_subaru_const.te_short) < subghz_protocol_subaru_const.te_delta ||
                DURATION_DIFF(duration, subghz_protocol_subaru_const.te_long) < subghz_protocol_subaru_const.te_delta)
            {
                instance->decoder.parser_step = SubaruDecoderStepSaveDuration;
            }
            else if (duration > 3000)
            {
                // Gap - end of packet
                if (instance->bit_count >= 64)
                {
                    if (subaru_process_data(instance))
                    {
                        instance->generic.data = instance->key;
                        instance->generic.data_count_bit = 64;
                        instance->generic.serial = instance->serial;
                        instance->generic.btn = instance->button;
                        instance->generic.cnt = instance->count;

                        if (instance->base.callback)
                        {
                            instance->base.callback(&instance->base, instance->base.context);
                        }
                    }
                }
                instance->decoder.parser_step = SubaruDecoderStepReset;
            }
            else
            {
                instance->decoder.parser_step = SubaruDecoderStepReset;
            }
        }
        else
        {
            instance->decoder.parser_step = SubaruDecoderStepReset;
        }
        break;
    }
}

uint8_t subghz_protocol_decoder_subaru_get_hash_data(void *context)
{
    furi_assert(context);
    SubGhzProtocolDecoderSubaru *instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_subaru_serialize(
    void *context,
    FlipperFormat *flipper_format,
    SubGhzRadioPreset *preset)
{
    furi_assert(context);
    SubGhzProtocolDecoderSubaru *instance = context;

    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    ret = subghz_block_generic_serialize(&instance->generic, flipper_format, preset);

    if (ret == SubGhzProtocolStatusOk)
    {
        // Subaru specific data - the counter uses special decoding
        flipper_format_write_uint32(flipper_format, "Serial", &instance->serial, 1);

        uint32_t temp = instance->button;
        flipper_format_write_uint32(flipper_format, "Btn", &temp, 1);

        temp = instance->count;
        flipper_format_write_uint32(flipper_format, "Cnt", &temp, 1);

        // Save raw data for exact reproduction
        uint32_t raw_high = (uint32_t)(instance->key >> 32);
        uint32_t raw_low = (uint32_t)(instance->key & 0xFFFFFFFF);
        flipper_format_write_uint32(flipper_format, "DataHi", &raw_high, 1);
        flipper_format_write_uint32(flipper_format, "DataLo", &raw_low, 1);
    }

    return ret;
}

SubGhzProtocolStatus subghz_protocol_decoder_subaru_deserialize(void *context, FlipperFormat *flipper_format)
{
    furi_assert(context);
    SubGhzProtocolDecoderSubaru *instance = context;
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_subaru_const.min_count_bit_for_found);

    if (ret == SubGhzProtocolStatusOk)
    {
        uint32_t temp_val;
        if (flipper_format_read_uint32(flipper_format, "DataHi", &temp_val, 1))
        {
            instance->key = ((uint64_t)temp_val << 32);
        }
        if (flipper_format_read_uint32(flipper_format, "DataLo", &temp_val, 1))
        {
            instance->key |= temp_val;
            instance->generic.data = instance->key; // Keep data in sync
        }
    }
    return ret;
}

void subghz_protocol_decoder_subaru_get_string(void *context, FuriString *output)
{
    furi_assert(context);
    SubGhzProtocolDecoderSubaru *instance = context;

    uint32_t key_hi = (uint32_t)(instance->key >> 32);
    uint32_t key_lo = (uint32_t)(instance->key & 0xFFFFFFFF);

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%08lX%08lX\r\n"
        "Sn:%06lX Btn:%X Cnt:%04X\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        key_hi,
        key_lo,
        instance->serial,
        instance->button,
        instance->count);
}

// Encoder implementation
void *subghz_protocol_encoder_subaru_alloc(SubGhzEnvironment *environment)
{
    UNUSED(environment);
    SubGhzProtocolEncoderSubaru *instance = malloc(sizeof(SubGhzProtocolEncoderSubaru));
    instance->base.protocol = &subaru_protocol;
    instance->step = SubaruEncoderStepReset;
    return instance;
}

void subghz_protocol_encoder_subaru_free(void *context)
{
    furi_assert(context);
    SubGhzProtocolEncoderSubaru *instance = context;
    free(instance);
}

SubGhzProtocolStatus subghz_protocol_encoder_subaru_deserialize(void *context, FlipperFormat *flipper_format)
{
    furi_assert(context);
    SubGhzProtocolEncoderSubaru *instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    if (subghz_block_generic_deserialize_check_count_bit(
            &instance->generic, flipper_format, subghz_protocol_subaru_const.min_count_bit_for_found))
    {
        uint32_t temp_val;
        if (!flipper_format_read_uint32(flipper_format, "Serial", &instance->generic.serial, 1))
            return SubGhzProtocolStatusError;
        if (!flipper_format_read_uint32(flipper_format, "Btn", &temp_val, 1))
            return SubGhzProtocolStatusError;
        instance->generic.btn = temp_val;
        if (!flipper_format_read_uint32(flipper_format, "Cnt", &temp_val, 1))
            return SubGhzProtocolStatusError;
        instance->generic.cnt = temp_val;

        uint8_t b[8] = {0};
        b[0] = instance->generic.btn;
        b[1] = (instance->generic.serial >> 16) & 0xFF;
        b[2] = (instance->generic.serial >> 8) & 0xFF;
        b[3] = instance->generic.serial & 0xFF;

        uint8_t count_bytes[4] = {0};
        subaru_encode_count(instance->generic.serial, instance->generic.cnt, count_bytes);
        b[4] = count_bytes[0];
        b[5] = count_bytes[1];
        b[6] = count_bytes[2];
        b[7] = count_bytes[3];

        instance->generic.data = 0;
        for (int i = 0; i < 8; i++)
        {
            instance->generic.data |= ((uint64_t)b[i] << (56 - (i * 8)));
        }
        ret = SubGhzProtocolStatusOk;
    }
    return ret;
}

void subghz_protocol_encoder_subaru_stop(void *context)
{
    furi_assert(context);
    SubGhzProtocolEncoderSubaru *instance = context;
    instance->step = SubaruEncoderStepStop;
}

LevelDuration subghz_protocol_encoder_subaru_yield(void *context)
{
    furi_assert(context);
    SubGhzProtocolEncoderSubaru *instance = context;

    uint32_t te_short = subghz_protocol_subaru_const.te_short;
    uint32_t te_long = subghz_protocol_subaru_const.te_long;
    uint32_t gap_val = 2750;

    switch (instance->step)
    {
    case SubaruEncoderStepReset:
        instance->preamble_count = 0;
        instance->data_bit_index = 0;
        instance->step = SubaruEncoderStepPreamble;
        // fallthrough
    case SubaruEncoderStepPreamble:
        if (instance->preamble_count < 50)
        { // 25 pairs
            if (instance->preamble_count % 2 == 0)
            {
                instance->preamble_count++;
                return level_duration_make(true, te_long);
            }
            else
            {
                instance->preamble_count++;
                return level_duration_make(false, te_long);
            }
        }
        else
        {
            instance->step = SubaruEncoderStepGap;
        }
        // fallthrough
    case SubaruEncoderStepGap:
        instance->step = SubaruEncoderStepSync;
        return level_duration_make(false, gap_val);

    case SubaruEncoderStepSync:
        instance->step = SubaruEncoderStepData;
        return level_duration_make(true, gap_val);

    case SubaruEncoderStepData:
        if (instance->data_bit_index < 128)
        {
            if (instance->data_bit_index % 2 == 0) {
                bool bit = (instance->generic.data >> (63 - (instance->data_bit_index / 2))) & 1;
                instance->data_bit_index++;
                if (bit)
                { // 1 = short H
                    return level_duration_make(true, te_short);
                }
                else
                { // 0 = long H
                    return level_duration_make(true, te_long);
                }
            } else {
                instance->data_bit_index++;
                return level_duration_make(false, te_short);
            }
        }
        else
        {
            instance->step = SubaruEncoderStepStop;
        }
        // fallthrough
    case SubaruEncoderStepStop:
        return level_duration_reset();
    }
    return level_duration_reset();
}