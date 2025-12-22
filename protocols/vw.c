#include "vw.h"
#include <lib/subghz/blocks/const.h>
#include <lib/subghz/blocks/decoder.h>
#include <lib/subghz/blocks/encoder.h>
#include <lib/subghz/blocks/generic.h>
#include <lib/subghz/blocks/math.h>

#define TAG "VWProtocol"

static const SubGhzBlockConst subghz_protocol_vw_const = {
    .te_short = 500,
    .te_long = 1000,
    .te_delta = 120,
    .min_count_bit_for_found = 80,
};

typedef struct SubGhzProtocolDecoderVw
{
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    ManchesterState manchester_state;
    uint64_t data_2; // Additional 16 bits (type byte + check byte)
} SubGhzProtocolDecoderVw;

typedef enum
{
    VwEncoderStepReset = 0,
    VwEncoderStepSync,
    VwEncoderStepStart,
    VwEncoderStepData,
    VwEncoderStepStop,
} VwEncoderStep;

typedef struct SubGhzProtocolEncoderVw
{
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    VwEncoderStep step;
    uint8_t sync_count;
    uint8_t data_bit_index;
    LevelDuration manchester_pulse;
    uint64_t data_2; // For Type and Check bytes
} SubGhzProtocolEncoderVw;

typedef enum
{
    VwDecoderStepReset = 0,
    VwDecoderStepFoundSync,
    VwDecoderStepFoundStart1,
    VwDecoderStepFoundStart2,
    VwDecoderStepFoundStart3,
    VwDecoderStepFoundData,
} VwDecoderStep;

const SubGhzProtocolDecoder subghz_protocol_vw_decoder = {
    .alloc = subghz_protocol_decoder_vw_alloc,
    .free = subghz_protocol_decoder_vw_free,
    .feed = subghz_protocol_decoder_vw_feed,
    .reset = subghz_protocol_decoder_vw_reset,
    .get_hash_data = subghz_protocol_decoder_vw_get_hash_data,
    .serialize = subghz_protocol_decoder_vw_serialize,
    .deserialize = subghz_protocol_decoder_vw_deserialize,
    .get_string = subghz_protocol_decoder_vw_get_string,
};

// Encoder forward declarations
void *subghz_protocol_encoder_vw_alloc(SubGhzEnvironment *environment);
void subghz_protocol_encoder_vw_free(void *context);
SubGhzProtocolStatus subghz_protocol_encoder_vw_deserialize(void *context, FlipperFormat *flipper_format);
void subghz_protocol_encoder_vw_stop(void *context);
LevelDuration subghz_protocol_encoder_vw_yield(void *context);

const SubGhzProtocolEncoder subghz_protocol_vw_encoder = {
    .alloc = subghz_protocol_encoder_vw_alloc,
    .free = subghz_protocol_encoder_vw_free,
    .deserialize = subghz_protocol_encoder_vw_deserialize,
    .stop = subghz_protocol_encoder_vw_stop,
    .yield = subghz_protocol_encoder_vw_yield,
};

const SubGhzProtocol vw_protocol = {
    .name = VW_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_vw_decoder,
    .encoder = &subghz_protocol_vw_encoder,
};

// Fixed manchester_advance for VW protocol
static bool vw_manchester_advance(
    ManchesterState state,
    ManchesterEvent event,
    ManchesterState *next_state,
    bool *data)
{

    bool result = false;
    ManchesterState new_state = ManchesterStateMid1;

    if (event == ManchesterEventReset)
    {
        new_state = ManchesterStateMid1;
    }
    else if (state == ManchesterStateMid0 || state == ManchesterStateMid1)
    {
        if (event == ManchesterEventShortHigh)
        {
            new_state = ManchesterStateStart1;
        }
        else if (event == ManchesterEventShortLow)
        {
            new_state = ManchesterStateStart0;
        }
        else
        {
            new_state = ManchesterStateMid1;
        }
    }
    else if (state == ManchesterStateStart1)
    {
        if (event == ManchesterEventShortLow)
        {
            new_state = ManchesterStateMid1;
            result = true;
            if (data)
                *data = true;
        }
        else if (event == ManchesterEventLongLow)
        {
            new_state = ManchesterStateStart0;
            result = true;
            if (data)
                *data = true;
        }
        else
        {
            new_state = ManchesterStateMid1;
        }
    }
    else if (state == ManchesterStateStart0)
    {
        if (event == ManchesterEventShortHigh)
        {
            new_state = ManchesterStateMid0;
            result = true;
            if (data)
                *data = false;
        }
        else if (event == ManchesterEventLongHigh)
        {
            new_state = ManchesterStateStart1;
            result = true;
            if (data)
                *data = false;
        }
        else
        {
            new_state = ManchesterStateMid1;
        }
    }

    *next_state = new_state;
    return result;
}

static uint8_t vw_get_bit_index(uint8_t bit)
{
    uint8_t bit_index = 0;

    if (bit < 72 && bit >= 8)
    {
        // use generic.data (bytes 1-8)
        bit_index = bit - 8;
    }
    else
    {
        // use data_2
        if (bit >= 72)
        {
            bit_index = bit - 64; // byte 0 = type
        }
        if (bit < 8)
        {
            bit_index = bit; // byte 9 = check digit
        }
        bit_index |= 0x80; // mark for data_2
    }

    return bit_index;
}

static void vw_add_bit(SubGhzProtocolDecoderVw *instance, bool level)
{
    if (instance->generic.data_count_bit >= subghz_protocol_vw_const.min_count_bit_for_found)
    {
        return;
    }

    uint8_t bit_index_full = subghz_protocol_vw_const.min_count_bit_for_found - 1 - instance->generic.data_count_bit;
    uint8_t bit_index_masked = vw_get_bit_index(bit_index_full);
    uint8_t bit_index = bit_index_masked & 0x7F;

    if (bit_index_masked & 0x80)
    {
        // use data_2
        if (level)
        {
            instance->data_2 |= (1ULL << bit_index);
        }
        else
        {
            instance->data_2 &= ~(1ULL << bit_index);
        }
    }
    else
    {
        // use data
        if (level)
        {
            instance->generic.data |= (1ULL << bit_index);
        }
        else
        {
            instance->generic.data &= ~(1ULL << bit_index);
        }
    }

    instance->generic.data_count_bit++;

    if (instance->generic.data_count_bit >= subghz_protocol_vw_const.min_count_bit_for_found)
    {
        if (instance->base.callback)
        {
            instance->base.callback(&instance->base, instance->base.context);
        }
    }
}

void *subghz_protocol_decoder_vw_alloc(SubGhzEnvironment *environment)
{
    UNUSED(environment);
    SubGhzProtocolDecoderVw *instance = malloc(sizeof(SubGhzProtocolDecoderVw));
    instance->base.protocol = &vw_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_vw_free(void *context)
{
    furi_assert(context);
    SubGhzProtocolDecoderVw *instance = context;
    free(instance);
}

void subghz_protocol_decoder_vw_reset(void *context)
{
    furi_assert(context);
    SubGhzProtocolDecoderVw *instance = context;
    instance->decoder.parser_step = VwDecoderStepReset;
    instance->generic.data_count_bit = 0;
    instance->generic.data = 0;
    instance->data_2 = 0;
    instance->manchester_state = ManchesterStateMid1;
}

void subghz_protocol_decoder_vw_feed(void *context, bool level, uint32_t duration)
{
    furi_assert(context);
    SubGhzProtocolDecoderVw *instance = context;

    uint32_t te_short = subghz_protocol_vw_const.te_short;
    uint32_t te_long = subghz_protocol_vw_const.te_long;
    uint32_t te_delta = subghz_protocol_vw_const.te_delta;
    uint32_t te_med = (te_long + te_short) / 2;
    uint32_t te_end = te_long * 5;

    ManchesterEvent event = ManchesterEventReset;

    switch (instance->decoder.parser_step)
    {
    case VwDecoderStepReset:
        if (DURATION_DIFF(duration, te_short) < te_delta)
        {
            instance->decoder.parser_step = VwDecoderStepFoundSync;
        }
        break;

    case VwDecoderStepFoundSync:
        if (DURATION_DIFF(duration, te_short) < te_delta)
        {
            // Stay - sync pattern repeats ~43 times
            break;
        }

        if (level && DURATION_DIFF(duration, te_long) < te_delta)
        {
            instance->decoder.parser_step = VwDecoderStepFoundStart1;
            break;
        }

        instance->decoder.parser_step = VwDecoderStepReset;
        break;

    case VwDecoderStepFoundStart1:
        if (!level && DURATION_DIFF(duration, te_short) < te_delta)
        {
            instance->decoder.parser_step = VwDecoderStepFoundStart2;
            break;
        }

        instance->decoder.parser_step = VwDecoderStepReset;
        break;

    case VwDecoderStepFoundStart2:
        if (level && DURATION_DIFF(duration, te_med) < te_delta)
        {
            instance->decoder.parser_step = VwDecoderStepFoundStart3;
            break;
        }

        instance->decoder.parser_step = VwDecoderStepReset;
        break;

    case VwDecoderStepFoundStart3:
        if (DURATION_DIFF(duration, te_med) < te_delta)
        {
            // Stay - med pattern repeats
            break;
        }

        if (level && DURATION_DIFF(duration, te_short) < te_delta)
        {
            // Start data collection
            vw_manchester_advance(
                instance->manchester_state,
                ManchesterEventReset,
                &instance->manchester_state,
                NULL);
            vw_manchester_advance(
                instance->manchester_state,
                ManchesterEventShortHigh,
                &instance->manchester_state,
                NULL);
            instance->generic.data_count_bit = 0;
            instance->generic.data = 0;
            instance->data_2 = 0;
            instance->decoder.parser_step = VwDecoderStepFoundData;
            break;
        }

        instance->decoder.parser_step = VwDecoderStepReset;
        break;

    case VwDecoderStepFoundData:
        if (DURATION_DIFF(duration, te_short) < te_delta)
        {
            event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
        }

        if (DURATION_DIFF(duration, te_long) < te_delta)
        {
            event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
        }

        // Last bit can be arbitrarily long
        if (instance->generic.data_count_bit == subghz_protocol_vw_const.min_count_bit_for_found - 1 &&
            !level && duration > te_end)
        {
            event = ManchesterEventShortLow;
        }

        if (event == ManchesterEventReset)
        {
            subghz_protocol_decoder_vw_reset(instance);
        }
        else
        {
            bool new_level;
            if (vw_manchester_advance(
                    instance->manchester_state,
                    event,
                    &instance->manchester_state,
                    &new_level))
            {
                vw_add_bit(instance, new_level);
            }
        }
        break;
    }
}

uint8_t subghz_protocol_decoder_vw_get_hash_data(void *context)
{
    furi_assert(context);
    SubGhzProtocolDecoderVw *instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_vw_serialize(
    void *context,
    FlipperFormat *flipper_format,
    SubGhzRadioPreset *preset)
{
    furi_assert(context);
    SubGhzProtocolDecoderVw *instance = context;

    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    ret = subghz_block_generic_serialize(&instance->generic, flipper_format, preset);

    if (ret == SubGhzProtocolStatusOk)
    {
        // Add VW-specific data
        uint32_t type = (instance->data_2 >> 8) & 0xFF;
        uint32_t check = instance->data_2 & 0xFF;
        uint32_t btn = (check >> 4) & 0xF;

        flipper_format_write_uint32(flipper_format, "Type", &type, 1);
        flipper_format_write_uint32(flipper_format, "Check", &check, 1);
        flipper_format_write_uint32(flipper_format, "Btn", &btn, 1);
    }

    return ret;
}

SubGhzProtocolStatus subghz_protocol_decoder_vw_deserialize(void *context, FlipperFormat *flipper_format)
{
    furi_assert(context);
    SubGhzProtocolDecoderVw *instance = context;
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_vw_const.min_count_bit_for_found);

    if (ret == SubGhzProtocolStatusOk)
    {
        uint32_t temp_val;
        if (flipper_format_read_uint32(flipper_format, "Type", &temp_val, 1))
        {
            instance->data_2 |= (temp_val << 8);
        }
        if (flipper_format_read_uint32(flipper_format, "Check", &temp_val, 1))
        {
            instance->data_2 |= temp_val;
        }
    }
    return ret;
}

static const char *vw_get_button_name(uint8_t btn)
{
    switch (btn)
    {
    case 0x1:
        return "UNLOCK";
    case 0x2:
        return "LOCK";
    case 0x3:
        return "Un+Lk";
    case 0x4:
        return "TRUNK";
    case 0x5:
        return "Un+Tr";
    case 0x6:
        return "Lk+Tr";
    case 0x7:
        return "Un+Lk+Tr";
    case 0x8:
        return "PANIC";
    default:
        return "Unknown";
    }
}

void subghz_protocol_decoder_vw_get_string(void *context, FuriString *output)
{
    furi_assert(context);
    SubGhzProtocolDecoderVw *instance = context;

    uint8_t type = (instance->data_2 >> 8) & 0xFF;
    uint8_t check = instance->data_2 & 0xFF;
    uint8_t btn = (check >> 4) & 0xF;

    uint32_t key_high = (instance->generic.data >> 32) & 0xFFFFFFFF;
    uint32_t key_low = instance->generic.data & 0xFFFFFFFF;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%02X%08lX%08lX%02X\r\n"
        "Type:%02X Btn:%X %s\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        type,
        key_high,
        key_low,
        check,
        type,
        btn,
        vw_get_button_name(btn));
}

// Encoder implementation
void *subghz_protocol_encoder_vw_alloc(SubGhzEnvironment *environment)
{
    UNUSED(environment);
    SubGhzProtocolEncoderVw *instance = malloc(sizeof(SubGhzProtocolEncoderVw));
    instance->base.protocol = &vw_protocol;
    instance->step = VwEncoderStepReset;
    return instance;
}

void subghz_protocol_encoder_vw_free(void *context)
{
    furi_assert(context);
    SubGhzProtocolEncoderVw *instance = context;
    free(instance);
}

SubGhzProtocolStatus subghz_protocol_encoder_vw_deserialize(void *context, FlipperFormat *flipper_format)
{
    furi_assert(context);
    SubGhzProtocolEncoderVw *instance = context;
    SubGhzProtocolStatus ret = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_vw_const.min_count_bit_for_found);

    if (ret == SubGhzProtocolStatusOk)
    {
        uint32_t temp_val;
        if (!flipper_format_read_uint32(flipper_format, "Type", &temp_val, 1))
            return SubGhzProtocolStatusError;
        instance->data_2 |= (temp_val << 8);
        if (!flipper_format_read_uint32(flipper_format, "Check", &temp_val, 1))
            return SubGhzProtocolStatusError;
        instance->data_2 |= temp_val;
    }
    return ret;
}

void subghz_protocol_encoder_vw_stop(void *context)
{
    furi_assert(context);
    SubGhzProtocolEncoderVw *instance = context;
    instance->step = VwEncoderStepStop;
}

LevelDuration subghz_protocol_encoder_vw_yield(void *context)
{
    furi_assert(context);
    SubGhzProtocolEncoderVw *instance = context;

    uint32_t te_short = subghz_protocol_vw_const.te_short;
    uint32_t te_long = subghz_protocol_vw_const.te_long;
    uint32_t te_med = (te_long + te_short) / 2;

    switch (instance->step)
    {
    case VwEncoderStepReset:
        instance->sync_count = 0;
        instance->data_bit_index = 0;
        instance->manchester_pulse.duration = 0;
        instance->step = VwEncoderStepSync;
        // fallthrough
    case VwEncoderStepSync:
        if (instance->sync_count < 86)
        { // 43 pairs
            if (instance->sync_count % 2 == 0)
            {
                instance->sync_count++;
                return level_duration_make(true, te_short);
            }
            else
            {
                instance->sync_count++;
                return level_duration_make(false, te_short);
            }
        }
        else
        {
            instance->step = VwEncoderStepStart;
        }
        // fallthrough
    case VwEncoderStepStart:
        if (instance->sync_count == 86)
        {
            instance->sync_count++;
            return level_duration_make(true, te_long);
        }
        else if (instance->sync_count == 87)
        {
            instance->sync_count++;
            return level_duration_make(false, te_short);
        }
        else if (instance->sync_count < 92)
        { // 2 pairs of med H/L
            if (instance->sync_count % 2 == 0)
            {
                instance->sync_count++;
                return level_duration_make(true, te_med);
            }
            else
            {
                instance->sync_count++;
                return level_duration_make(false, te_med);
            }
        }
        else
        {
            instance->step = VwEncoderStepData;
            return level_duration_make(true, te_short); // Start of data
        }

    case VwEncoderStepData:
        if (instance->manchester_pulse.duration > 0)
        {
            LevelDuration pulse = instance->manchester_pulse;
            instance->manchester_pulse.duration = 0;
            return pulse;
        }
        if (instance->data_bit_index < 80)
        {
            uint64_t combined_data = instance->generic.data;
            if (instance->data_bit_index >= 64) {
                combined_data = instance->data_2;
            }
            bool bit = (combined_data >> (79 - instance->data_bit_index)) & 1;
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
            instance->step = VwEncoderStepStop;
        }
        // fallthrough
    case VwEncoderStepStop:
        return level_duration_reset();
    }
    return level_duration_reset();
}