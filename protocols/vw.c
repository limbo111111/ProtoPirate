#include "vw.h"

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

#include <furi.h>

typedef struct SubGhzProtocolEncoderVw
{
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    // Encoder state
    uint8_t type;
    uint8_t check;
    uint8_t btn;

    uint16_t yield_state;
    uint64_t data; // 64 bits of main data
    uint16_t data_2; // 16 bits of extra data
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

// Forward declarations for encoder
void* subghz_protocol_encoder_vw_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_vw_free(void* context);
SubGhzProtocolStatus subghz_protocol_encoder_vw_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_vw_stop(void* context);
LevelDuration subghz_protocol_encoder_vw_yield(void* context);


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
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_vw_const.min_count_bit_for_found);
}

void subghz_protocol_decoder_vw_get_string(void *context, FuriString *output)
{
    furi_assert(context);
    SubGhzProtocolDecoderVw *instance = context;

    uint8_t type = (instance->data_2 >> 8) & 0xFF;
    uint8_t check = instance->data_2 & 0xFF;
    uint8_t btn = (check >> 4) & 0xF;

    const char* btn_name;
    switch (btn)
    {
    case 0x1:
        btn_name = "UNLOCK";
        break;
    case 0x2:
        btn_name = "LOCK";
        break;
    case 0x3:
        btn_name = "Un+Lk";
        break;
    case 0x4:
        btn_name = "TRUNK";
        break;
    case 0x5:
        btn_name = "Un+Tr";
        break;
    case 0x6:
        btn_name = "Lk+Tr";
        break;
    case 0x7:
        btn_name = "Un+Lk+Tr";
        break;
    case 0x8:
        btn_name = "PANIC";
        break;
    default:
        btn_name = "Unknown";
        break;
    }

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
        btn_name);
}

// Encoder implementation
void* subghz_protocol_encoder_vw_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderVw* instance = malloc(sizeof(SubGhzProtocolEncoderVw));
    instance->base.protocol = &vw_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->yield_state = 0;
    instance->type = 0;
    instance->check = 0;
    instance->btn = 0;
    instance->data = 0;
    instance->data_2 = 0;
    return instance;
}

void subghz_protocol_encoder_vw_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderVw* instance = context;
    free(instance);
}

static void subghz_protocol_encoder_vw_update_data(SubGhzProtocolEncoderVw* instance) {
    instance->data = instance->generic.data;
    instance->data_2 = ((uint16_t)instance->type << 8) | instance->check;
    instance->yield_state = 0;
}

SubGhzProtocolStatus subghz_protocol_encoder_vw_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderVw* instance = context;

    if (subghz_block_generic_deserialize(&instance->generic, flipper_format) != SubGhzProtocolStatusOk) {
        return SubGhzProtocolStatusError;
    }

    uint32_t type_temp, check_temp, btn_temp;
    if (!flipper_format_read_uint32(flipper_format, "Type", &type_temp, 1) ||
        !flipper_format_read_uint32(flipper_format, "Check", &check_temp, 1) ||
        !flipper_format_read_uint32(flipper_format, "Btn", &btn_temp, 1)) {

        // Fallback for older captures
        // This is imperfect as data_2 is not in generic.data
        instance->type = 0;
        instance->check = 0;
        instance->btn = 0;
    } else {
        instance->type = type_temp;
        instance->check = check_temp;
        instance->btn = btn_temp;
    }

    subghz_protocol_encoder_vw_update_data(instance);

    return SubGhzProtocolStatusOk;
}

void subghz_protocol_encoder_vw_stop(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderVw* instance = context;
    instance->yield_state = 0;
}

LevelDuration subghz_protocol_encoder_vw_yield(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderVw* instance = context;

    uint32_t te_short = subghz_protocol_vw_const.te_short;
    uint32_t te_long = subghz_protocol_vw_const.te_long;
    uint32_t te_med = (te_long + te_short) / 2;

    // Sync pattern: ~43 pairs of (short high, short low)
    if (instance->yield_state < 86) {
        instance->yield_state++;
        if ((instance->yield_state - 1) % 2 == 0) {
            return level_duration_make(true, te_short);
        } else {
            return level_duration_make(false, te_short);
        }
    }
    // Start pattern
    else if (instance->yield_state == 86) {
        instance->yield_state++;
        return level_duration_make(true, te_long);
    }
    else if (instance->yield_state == 87) {
        instance->yield_state++;
        return level_duration_make(false, te_short);
    }
    else if (instance->yield_state < 92) { // 2 pairs of (med high, med low)
        instance->yield_state++;
         if ((instance->yield_state - 1) % 2 == 0) {
            return level_duration_make(true, te_med);
        } else {
            return level_duration_make(false, te_med);
        }
    }
    // Data: 80 bits, custom manchester
    else if (instance->yield_state < 92 + (80 * 2)) {
        uint8_t bit_index_full = (instance->yield_state - 92) / 2;
        bool pulse_is_first = ((instance->yield_state - 92) % 2 == 0);
        instance->yield_state++;

        uint8_t bit_index_masked = vw_get_bit_index(subghz_protocol_vw_const.min_count_bit_for_found - 1 - bit_index_full);
        uint8_t bit_index = bit_index_masked & 0x7F;
        bool use_data_2 = bit_index_masked & 0x80;

        bool bit;
        if (use_data_2) {
            bit = (instance->data_2 >> bit_index) & 1;
        } else {
            bit = (instance->data >> bit_index) & 1;
        }

        // Custom Manchester
        // 1 -> short high, short low
        // 0 -> short low, short high
        if(pulse_is_first) {
            return level_duration_make(bit, te_short);
        } else {
            return level_duration_make(!bit, te_short);
        }
    }
    else { // Done
        return level_duration_reset();
    }
}