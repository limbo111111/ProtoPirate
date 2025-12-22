#include "ford_v0.h"
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>

#define TAG "FordProtocolV0"

static const SubGhzBlockConst subghz_protocol_ford_v0_const = {
    .te_short = 250,
    .te_long = 500,
    .te_delta = 100,
    .min_count_bit_for_found = 64,
};

typedef struct SubGhzProtocolDecoderFordV0
{
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    ManchesterState manchester_state;

    uint64_t data_low;
    uint64_t data_high;
    uint8_t bit_count;

    uint16_t header_count;

    uint64_t key1;
    uint16_t key2;
    uint32_t serial;
    uint8_t button;
    uint32_t count;
} SubGhzProtocolDecoderFordV0;

typedef enum
{
    FordEncoderStepReset = 0,
    FordEncoderStepPreamble,
    FordEncoderStepGap,
    FordEncoderStepData,
    FordEncoderStepStop,
} FordEncoderStep;

typedef struct SubGhzProtocolEncoderFordV0
{
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    FordEncoderStep step;
    uint64_t payload_key1;
    uint16_t payload_key2;
    uint8_t preamble_count;
    uint8_t data_bit_index;
    LevelDuration manchester_pulse;

    // From .sub file
    uint32_t serial;
    uint8_t button;
    uint32_t count;
    uint8_t bs;  // Byte 8
    uint8_t crc; // Byte 9
} SubGhzProtocolEncoderFordV0;

typedef enum
{
    FordV0DecoderStepReset = 0,
    FordV0DecoderStepPreamble,
    FordV0DecoderStepPreambleCheck,
    FordV0DecoderStepGap,
    FordV0DecoderStepData,
} FordV0DecoderStep;

// Forward declarations
static void ford_v0_add_bit(SubGhzProtocolDecoderFordV0 *instance, bool bit);
static void decode_ford_v0(uint64_t key1, uint16_t key2, uint32_t *serial, uint8_t *button, uint32_t *count);
static bool ford_v0_process_data(SubGhzProtocolDecoderFordV0 *instance);

// Encoder forward declarations
void *subghz_protocol_encoder_ford_v0_alloc(SubGhzEnvironment *environment);
void subghz_protocol_encoder_ford_v0_free(void *context);
SubGhzProtocolStatus subghz_protocol_encoder_ford_v0_deserialize(void *context, FlipperFormat *flipper_format);
void subghz_protocol_encoder_ford_v0_stop(void *context);
LevelDuration subghz_protocol_encoder_ford_v0_yield(void *context);
static void encode_ford_v0(SubGhzProtocolEncoderFordV0 *instance);

const SubGhzProtocolDecoder subghz_protocol_ford_v0_decoder = {
    .alloc = subghz_protocol_decoder_ford_v0_alloc,
    .free = subghz_protocol_decoder_ford_v0_free,
    .feed = subghz_protocol_decoder_ford_v0_feed,
    .reset = subghz_protocol_decoder_ford_v0_reset,
    .get_hash_data = subghz_protocol_decoder_ford_v0_get_hash_data,
    .serialize = subghz_protocol_decoder_ford_v0_serialize,
    .deserialize = subghz_protocol_decoder_ford_v0_deserialize,
    .get_string = subghz_protocol_decoder_ford_v0_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_ford_v0_encoder = {
    .alloc = subghz_protocol_encoder_ford_v0_alloc,
    .free = subghz_protocol_encoder_ford_v0_free,
    .deserialize = subghz_protocol_encoder_ford_v0_deserialize,
    .stop = subghz_protocol_encoder_ford_v0_stop,
    .yield = subghz_protocol_encoder_ford_v0_yield,
};

const SubGhzProtocol ford_protocol_v0 = {
    .name = FORD_PROTOCOL_V0_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_ford_v0_decoder,
    .encoder = &subghz_protocol_ford_v0_encoder,
};

// Reverse of decode_ford_v0
static void encode_ford_v0(SubGhzProtocolEncoderFordV0 *instance)
{
    uint8_t buf[13] = {0};

    // 1. Pack serial, button, count into buffer structure
    buf[5] = (instance->button << 4) | ((instance->count >> 16) & 0x0F);
    buf[6] = (instance->count >> 8) & 0xFF;
    buf[7] = instance->count & 0xFF;

    uint32_t serial_be = instance->serial;
    uint32_t serial_le = ((serial_be & 0xFF) << 24) |
                         (((serial_be >> 8) & 0xFF) << 16) |
                         (((serial_be >> 16) & 0xFF) << 8) |
                         ((serial_be >> 24) & 0xFF);

    buf[1] = serial_le & 0xFF;
    buf[2] = (serial_le >> 8) & 0xFF;
    buf[3] = (serial_le >> 16) & 0xFF;
    buf[4] = (serial_le >> 24) & 0xFF;

    // 2. Reverse the nibble/bit swapping between buf[6] and buf[7]
    uint8_t b6_from_count = buf[6];
    uint8_t b7_from_count = buf[7];
    buf[6] = (b6_from_count & 0xAA) | (b7_from_count & 0x55);
    buf[7] = (b7_from_count & 0xAA) | (b6_from_count & 0x55);

    // 3. Load BS and CRC from instance, calculate parity for XOR step
    buf[8] = instance->bs;
    buf[9] = instance->crc;

    uint8_t tmp = buf[8];
    uint8_t parity = 0;
    uint8_t parity_any = (tmp != 0);
    while (tmp)
    {
        parity ^= (tmp & 1);
        tmp >>= 1;
    }
    buf[11] = parity_any ? parity : 0;

    // 4. Reverse the XOR operations (apply them again)
    uint8_t xor_byte;
    uint8_t limit;
    if (buf[11])
    {
        xor_byte = buf[7];
        limit = 7;
    }
    else
    {
        xor_byte = buf[6];
        limit = 6;
    }

    if (buf[11] == 0)
    {
        buf[7] ^= xor_byte;
    }

    for (int idx = 1; idx < limit; ++idx)
    {
        buf[idx] ^= xor_byte;
    }

    // Use first byte of original key as buf[0]
    buf[0] = (uint8_t)(instance->generic.data >> 56);

    // 5. Pack buffer back into key1 and key2
    uint64_t key1 = 0;
    for (int i = 0; i < 8; ++i)
    {
        key1 = (key1 << 8) | buf[i];
    }

    uint16_t key2 = (buf[8] << 8) | buf[9];

    // 6. Bitwise NOT the final keys
    instance->payload_key1 = ~key1;
    instance->payload_key2 = ~key2;
}

static void ford_v0_add_bit(SubGhzProtocolDecoderFordV0 *instance, bool bit)
{
    uint32_t low = (uint32_t)instance->data_low;
    instance->data_low = (instance->data_low << 1) | (bit ? 1 : 0);
    instance->data_high = (instance->data_high << 1) | ((low >> 31) & 1);
    instance->bit_count++;
}

static void decode_ford_v0(uint64_t key1, uint16_t key2, uint32_t *serial, uint8_t *button, uint32_t *count)
{
    uint8_t buf[13] = {0};

    for (int i = 0; i < 8; ++i)
    {
        buf[i] = (uint8_t)(key1 >> (56 - i * 8));
    }

    buf[8] = (uint8_t)(key2 >> 8);
    buf[9] = (uint8_t)(key2 & 0xFF);

    uint8_t tmp = buf[8];
    uint8_t parity = 0;
    uint8_t parity_any = (tmp != 0);
    while (tmp)
    {
        parity ^= (tmp & 1);
        tmp >>= 1;
    }
    buf[11] = parity_any ? parity : 0;

    uint8_t xor_byte;
    uint8_t limit;
    if (buf[11])
    {
        xor_byte = buf[7];
        limit = 7;
    }
    else
    {
        xor_byte = buf[6];
        limit = 6;
    }

    for (int idx = 1; idx < limit; ++idx)
    {
        buf[idx] ^= xor_byte;
    }

    if (buf[11] == 0)
    {
        buf[7] ^= xor_byte;
    }

    uint8_t orig_b7 = buf[7];

    buf[7] = (orig_b7 & 0xAA) | (buf[6] & 0x55);
    uint8_t mixed = (buf[6] & 0xAA) | (orig_b7 & 0x55);
    buf[12] = mixed;
    buf[6] = mixed;

    uint32_t serial_le = ((uint32_t)buf[1]) |
                         ((uint32_t)buf[2] << 8) |
                         ((uint32_t)buf[3] << 16) |
                         ((uint32_t)buf[4] << 24);

    *serial = ((serial_le & 0xFF) << 24) |
              (((serial_le >> 8) & 0xFF) << 16) |
              (((serial_le >> 16) & 0xFF) << 8) |
              ((serial_le >> 24) & 0xFF);

    *button = (buf[5] >> 4) & 0x0F;

    *count = ((buf[5] & 0x0F) << 16) |
             (buf[6] << 8) |
             buf[7];
}

static bool ford_v0_process_data(SubGhzProtocolDecoderFordV0 *instance)
{
    if (instance->bit_count == 64)
    {
        uint64_t combined = ((uint64_t)instance->data_high << 32) | instance->data_low;
        instance->key1 = ~combined;
        instance->data_low = 0;
        instance->data_high = 0;
        return false;
    }

    if (instance->bit_count == 80)
    {
        uint16_t key2_raw = (uint16_t)(instance->data_low & 0xFFFF);
        uint16_t key2 = ~key2_raw;

        decode_ford_v0(instance->key1, key2, &instance->serial, &instance->button, &instance->count);
        instance->key2 = key2;
        return true;
    }

    return false;
}

void *subghz_protocol_decoder_ford_v0_alloc(SubGhzEnvironment *environment)
{
    UNUSED(environment);
    SubGhzProtocolDecoderFordV0 *instance = malloc(sizeof(SubGhzProtocolDecoderFordV0));
    instance->base.protocol = &ford_protocol_v0;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_ford_v0_free(void *context)
{
    furi_assert(context);
    SubGhzProtocolDecoderFordV0 *instance = context;
    free(instance);
}

void subghz_protocol_decoder_ford_v0_reset(void *context)
{
    furi_assert(context);
    SubGhzProtocolDecoderFordV0 *instance = context;
    instance->decoder.parser_step = FordV0DecoderStepReset;
    instance->decoder.te_last = 0;
    instance->manchester_state = ManchesterStateMid1;
    instance->data_low = 0;
    instance->data_high = 0;
    instance->bit_count = 0;
    instance->header_count = 0;
    instance->key1 = 0;
    instance->key2 = 0;
    instance->serial = 0;
    instance->button = 0;
    instance->count = 0;
}

void subghz_protocol_decoder_ford_v0_feed(void *context, bool level, uint32_t duration)
{
    furi_assert(context);
    SubGhzProtocolDecoderFordV0 *instance = context;

    uint32_t te_short = subghz_protocol_ford_v0_const.te_short;
    uint32_t te_long = subghz_protocol_ford_v0_const.te_long;
    uint32_t te_delta = subghz_protocol_ford_v0_const.te_delta;
    uint32_t gap_threshold = 3500;

    switch (instance->decoder.parser_step)
    {
    case FordV0DecoderStepReset:
        if (level && (DURATION_DIFF(duration, te_short) < te_delta))
        {
            instance->data_low = 0;
            instance->data_high = 0;
            instance->decoder.parser_step = FordV0DecoderStepPreamble;
            instance->decoder.te_last = duration;
            instance->header_count = 0;
            instance->bit_count = 0;
            manchester_advance(instance->manchester_state, ManchesterEventReset, &instance->manchester_state, NULL);
        }
        break;

    case FordV0DecoderStepPreamble:
        if (!level)
        {
            if (DURATION_DIFF(duration, te_long) < te_delta)
            {
                instance->decoder.te_last = duration;
                instance->decoder.parser_step = FordV0DecoderStepPreambleCheck;
            }
            else
            {
                instance->decoder.parser_step = FordV0DecoderStepReset;
            }
        }
        break;

    case FordV0DecoderStepPreambleCheck:
        if (level)
        {
            if (DURATION_DIFF(duration, te_long) < te_delta)
            {
                instance->header_count++;
                instance->decoder.te_last = duration;
                instance->decoder.parser_step = FordV0DecoderStepPreamble;
            }
            else if (DURATION_DIFF(duration, te_short) < te_delta)
            {
                instance->decoder.parser_step = FordV0DecoderStepGap;
            }
            else
            {
                instance->decoder.parser_step = FordV0DecoderStepReset;
            }
        }
        break;

    case FordV0DecoderStepGap:
        if (!level && (DURATION_DIFF(duration, gap_threshold) < 250))
        {
            instance->data_low = 1;
            instance->data_high = 0;
            instance->bit_count = 1;
            instance->decoder.parser_step = FordV0DecoderStepData;
        }
        else if (!level && duration > gap_threshold + 250)
        {
            instance->decoder.parser_step = FordV0DecoderStepReset;
        }
        break;

    case FordV0DecoderStepData:
    {
        ManchesterEvent event;

        if (DURATION_DIFF(duration, te_short) < te_delta)
        {
            event = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
        }
        else if (DURATION_DIFF(duration, te_long) < te_delta)
        {
            event = level ? ManchesterEventLongLow : ManchesterEventLongHigh;
        }
        else
        {
            instance->decoder.parser_step = FordV0DecoderStepReset;
            break;
        }

        bool data_bit;
        if (manchester_advance(instance->manchester_state, event, &instance->manchester_state, &data_bit))
        {
            ford_v0_add_bit(instance, data_bit);

            if (ford_v0_process_data(instance))
            {
                instance->generic.data = instance->key1;
                instance->generic.data_count_bit = 64;
                instance->generic.serial = instance->serial;
                instance->generic.btn = instance->button;
                instance->generic.cnt = instance->count;

                if (instance->base.callback)
                {
                    instance->base.callback(&instance->base, instance->base.context);
                }

                instance->data_low = 0;
                instance->data_high = 0;
                instance->bit_count = 0;
                instance->decoder.parser_step = FordV0DecoderStepReset;
            }
        }

        instance->decoder.te_last = duration;
        break;
    }
    }
}

uint8_t subghz_protocol_decoder_ford_v0_get_hash_data(void *context)
{
    furi_assert(context);
    SubGhzProtocolDecoderFordV0 *instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_ford_v0_serialize(
    void *context,
    FlipperFormat *flipper_format,
    SubGhzRadioPreset *preset)
{
    furi_assert(context);
    SubGhzProtocolDecoderFordV0 *instance = context;

    SubGhzProtocolStatus ret = SubGhzProtocolStatusOk;

    if (!subghz_block_generic_serialize(&instance->generic, flipper_format, preset))
    {
        ret = SubGhzProtocolStatusError;
    }

    if (ret == SubGhzProtocolStatusOk)
    {
        // Add Ford-specific data
        uint32_t temp = (instance->key2 >> 8) & 0xFF; // BS byte
        if (!flipper_format_write_uint32(flipper_format, "BS", &temp, 1))
        {
            ret = SubGhzProtocolStatusError;
        }

        temp = instance->key2 & 0xFF; // CRC byte
        if (!flipper_format_write_uint32(flipper_format, "CRC", &temp, 1))
        {
            ret = SubGhzProtocolStatusError;
        }

        // Ensure serial, button, count are saved
        if (!flipper_format_write_uint32(flipper_format, "Serial", &instance->serial, 1))
        {
            ret = SubGhzProtocolStatusError;
        }

        temp = instance->button;
        if (!flipper_format_write_uint32(flipper_format, "Btn", &temp, 1))
        {
            ret = SubGhzProtocolStatusError;
        }

        if (!flipper_format_write_uint32(flipper_format, "Cnt", &instance->count, 1))
        {
            ret = SubGhzProtocolStatusError;
        }
    }

    return ret;
}

SubGhzProtocolStatus subghz_protocol_decoder_ford_v0_deserialize(void *context, FlipperFormat *flipper_format)
{
    furi_assert(context);
    SubGhzProtocolDecoderFordV0 *instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    if (subghz_block_generic_deserialize_check_count_bit(
            &instance->generic, flipper_format, subghz_protocol_ford_v0_const.min_count_bit_for_found))
    {
        // The decoder doesn't strictly need these to re-decode, but it does to re-serialize or display correctly.
        uint32_t temp_bs = 0, temp_crc = 0;
        flipper_format_read_uint32(flipper_format, "BS", &temp_bs, 1);
        flipper_format_read_uint32(flipper_format, "CRC", &temp_crc, 1);
        instance->key2 = (temp_bs << 8) | temp_crc;

        flipper_format_read_uint32(flipper_format, "Serial", &instance->serial, 1);
        uint32_t temp_btn = 0;
        flipper_format_read_uint32(flipper_format, "Btn", &temp_btn, 1);
        instance->button = temp_btn;
        flipper_format_read_uint32(flipper_format, "Cnt", &instance->count, 1);

        ret = SubGhzProtocolStatusOk;
    }

    return ret;
}

void subghz_protocol_decoder_ford_v0_get_string(void *context, FuriString *output)
{
    furi_assert(context);
    SubGhzProtocolDecoderFordV0 *instance = context;

    uint32_t code_found_hi = (uint32_t)(instance->key1 >> 32);
    uint32_t code_found_lo = (uint32_t)(instance->key1 & 0xFFFFFFFF);

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%08lX%08lX\r\n"
        "Sn:%08lX Btn:%02X Cnt:%06lX\r\n"
        "BS:%02X CRC:%02X\r\n",
        instance->generic.protocol_name,
        80, // Corrected bit count for display
        code_found_hi,
        code_found_lo,
        instance->serial,
        instance->button,
        instance->count,
        (instance->key2 >> 8) & 0xFF,
        instance->key2 & 0xFF);
}

// Encoder implementation
void *subghz_protocol_encoder_ford_v0_alloc(SubGhzEnvironment *environment)
{
    UNUSED(environment);
    SubGhzProtocolEncoderFordV0 *instance = malloc(sizeof(SubGhzProtocolEncoderFordV0));
    instance->base.protocol = &ford_protocol_v0;
    instance->step = FordEncoderStepReset;
    return instance;
}

void subghz_protocol_encoder_ford_v0_free(void *context)
{
    furi_assert(context);
    SubGhzProtocolEncoderFordV0 *instance = context;
    free(instance);
}

SubGhzProtocolStatus subghz_protocol_encoder_ford_v0_deserialize(void *context, FlipperFormat *flipper_format)
{
    furi_assert(context);
    SubGhzProtocolEncoderFordV0 *instance = context;
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    if (subghz_block_generic_deserialize_check_count_bit(&instance->generic, flipper_format, 64))
    {
        uint32_t temp_val;
        if (!flipper_format_read_uint32(flipper_format, "Serial", &instance->serial, 1))
        {
            FURI_LOG_E(TAG, "Missing Serial");
            return SubGhzProtocolStatusError;
        }
        if (!flipper_format_read_uint32(flipper_format, "Btn", &temp_val, 1))
        {
            FURI_LOG_E(TAG, "Missing Btn");
            return SubGhzProtocolStatusError;
        }
        instance->button = temp_val;
        if (!flipper_format_read_uint32(flipper_format, "Cnt", &instance->count, 1))
        {
            FURI_LOG_E(TAG, "Missing Cnt");
            return SubGhzProtocolStatusError;
        }
        if (!flipper_format_read_uint32(flipper_format, "BS", &temp_val, 1))
        {
            FURI_LOG_E(TAG, "Missing BS");
            return SubGhzProtocolStatusError;
        }
        instance->bs = temp_val;
        if (!flipper_format_read_uint32(flipper_format, "CRC", &temp_val, 1))
        {
            FURI_LOG_E(TAG, "Missing CRC");
            return SubGhzProtocolStatusError;
        }
        instance->crc = temp_val;
        ret = SubGhzProtocolStatusOk;
    }

    if (ret == SubGhzProtocolStatusOk)
    {
        encode_ford_v0(instance);
        instance->step = FordEncoderStepPreamble;
    }

    return ret;
}

void subghz_protocol_encoder_ford_v0_stop(void *context)
{
    furi_assert(context);
    SubGhzProtocolEncoderFordV0 *instance = context;
    instance->step = FordEncoderStepStop;
}

LevelDuration subghz_protocol_encoder_ford_v0_yield(void *context)
{
    furi_assert(context);
    SubGhzProtocolEncoderFordV0 *instance = context;

    uint32_t te_short = subghz_protocol_ford_v0_const.te_short;
    uint32_t te_long = subghz_protocol_ford_v0_const.te_long;
    uint32_t gap_threshold = 3500;

    switch (instance->step)
    {
    case FordEncoderStepReset:
        instance->preamble_count = 0;
        instance->data_bit_index = 0;
        instance->manchester_pulse.duration = 0;
        instance->step = FordEncoderStepPreamble;
        // fallthrough
    case FordEncoderStepPreamble:
        if (instance->preamble_count < 20)
        {
            if (instance->preamble_count % 2 == 0)
            {
                return level_duration_make(true, te_long);
            }
            else
            {
                instance->preamble_count++;
                return level_duration_make(false, te_long);
            }
        }
        else if (instance->preamble_count == 20)
        {
            instance->preamble_count++;
            return level_duration_make(true, te_short);
        }
        else
        {
            instance->step = FordEncoderStepGap;
            return level_duration_make(false, gap_threshold);
        }

    case FordEncoderStepGap:
        instance->step = FordEncoderStepData;
        instance->data_bit_index = 1;
        // The first data bit is always 1, send its first half
        instance->manchester_pulse = level_duration_make(false, te_short); // 1 is encoded as 10
        return level_duration_make(true, te_short);

    case FordEncoderStepData:
        // Send the second half of the previous bit if it exists
        if (instance->manchester_pulse.duration > 0)
        {
            LevelDuration pulse = instance->manchester_pulse;
            instance->manchester_pulse.duration = 0;
            return pulse;
        }

        if (instance->data_bit_index < 80)
        {
            bool bit;
            if (instance->data_bit_index < 64)
            {
                bit = (instance->payload_key1 >> (63 - instance->data_bit_index)) & 1;
            }
            else
            {
                bit = (instance->payload_key2 >> (15 - (instance->data_bit_index - 64))) & 1;
            }
            instance->data_bit_index++;

            // Manchester: 1 -> 10, 0 -> 01
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
            instance->step = FordEncoderStepStop;
        }
        // fallthrough
    case FordEncoderStepStop:
        return level_duration_reset();
    }
    return level_duration_reset();
}