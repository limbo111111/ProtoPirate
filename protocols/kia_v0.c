#include "kia_v0.h"

#define TAG "KiaProtocolV0"

static const SubGhzBlockConst subghz_protocol_kia_const = {
    .te_short = 250,
    .te_long = 500,
    .te_delta = 100,
    .min_count_bit_for_found = 61,
};

struct SubGhzProtocolDecoderKIA
{
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    uint16_t header_count;
};

struct SubGhzProtocolEncoderKIA
{
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    uint32_t serial;
    uint8_t button;
    uint16_t counter;
    uint16_t yield_state;
};

typedef enum
{
    KIADecoderStepReset = 0,
    KIADecoderStepCheckPreambula,
    KIADecoderStepSaveDuration,
    KIADecoderStepCheckDuration,
} KIADecoderStep;

// Forward declarations for encoder
void *subghz_protocol_encoder_kia_alloc(SubGhzEnvironment *environment);
void subghz_protocol_encoder_kia_free(void *context);
SubGhzProtocolStatus subghz_protocol_encoder_kia_deserialize(void *context, FlipperFormat *flipper_format);
void subghz_protocol_encoder_kia_stop(void *context);
LevelDuration subghz_protocol_encoder_kia_yield(void *context);

const SubGhzProtocolDecoder subghz_protocol_kia_decoder = {
    .alloc = subghz_protocol_decoder_kia_alloc,
    .free = subghz_protocol_decoder_kia_free,
    .feed = subghz_protocol_decoder_kia_feed,
    .reset = subghz_protocol_decoder_kia_reset,
    .get_hash_data = subghz_protocol_decoder_kia_get_hash_data,
    .serialize = subghz_protocol_decoder_kia_serialize,
    .deserialize = subghz_protocol_decoder_kia_deserialize,
    .get_string = subghz_protocol_decoder_kia_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_kia_encoder = {
    .alloc = subghz_protocol_encoder_kia_alloc,
    .free = subghz_protocol_encoder_kia_free,
    .deserialize = subghz_protocol_encoder_kia_deserialize,
    .stop = subghz_protocol_encoder_kia_stop,
    .yield = subghz_protocol_encoder_kia_yield,
};

const SubGhzProtocol kia_protocol_v0 = {
    .name = KIA_PROTOCOL_V0_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable |
            SubGhzProtocolFlag_Save | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_kia_decoder,
    .encoder = &subghz_protocol_kia_encoder,
};

// Encoder implementation
void* subghz_protocol_encoder_kia_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderKIA* instance = malloc(sizeof(SubGhzProtocolEncoderKIA));
    instance->base.protocol = &kia_protocol_v0;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->yield_state = 0;
    instance->serial = 0;
    instance->button = 0;
    instance->counter = 0;
    return instance;
}

void subghz_protocol_encoder_kia_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKIA* instance = context;
    free(instance);
}

// Function to update the data with new button/counter values
static void subghz_protocol_encoder_kia_update_data(SubGhzProtocolEncoderKIA* instance) {
    // Reconstruct the 61-bit data with updated button and counter
    uint64_t data = instance->generic.data;

    // Clear and update button bits (8-11)
    data &= ~(0x0FULL << 8);
    data |= ((uint64_t)(instance->button & 0x0F) << 8);

    // Clear and update counter bits (40-55)
    data &= ~(0xFFFFULL << 40);
    data |= ((uint64_t)(instance->counter & 0xFFFF) << 40);

    instance->generic.data = data;

    // Reset transmission state for new data
    instance->yield_state = 0;
}

SubGhzProtocolStatus subghz_protocol_encoder_kia_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderKIA* instance = context;

    SubGhzProtocolStatus result = subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_kia_const.min_count_bit_for_found);

    if (result == SubGhzProtocolStatusOk) {
        // Read or extract serial
        if (!flipper_format_read_uint32(flipper_format, "Serial", &instance->serial, 1)) {
            instance->serial = (uint32_t)((instance->generic.data >> 12) & 0x0FFFFFFF);
        }

        // Read or extract button
        uint32_t btn_temp;
        if (flipper_format_read_uint32(flipper_format, "Btn", &btn_temp, 1)) {
            instance->button = (uint8_t)btn_temp;
        } else {
            instance->button = (instance->generic.data >> 8) & 0x0F;
        }

        // Read or extract counter
        uint32_t cnt_temp;
        if (flipper_format_read_uint32(flipper_format, "Cnt", &cnt_temp, 1)) {
            instance->counter = (uint16_t)cnt_temp;
        } else {
            instance->counter = (instance->generic.data >> 40) & 0xFFFF;
        }

        // Update the key data with button and counter values
        subghz_protocol_encoder_kia_update_data(instance);
    }

    return result;
}

void subghz_protocol_encoder_kia_stop(void *context) {
    furi_assert(context);
    SubGhzProtocolEncoderKIA* instance = context;
    instance->yield_state = 0;
}

LevelDuration subghz_protocol_encoder_kia_yield(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderKIA* instance = context;

    if (instance->yield_state < 32) { // Preamble (16 pairs of short high/low)
        instance->yield_state++;
        if ((instance->yield_state - 1) % 2 == 0) {
            return level_duration_make(true, subghz_protocol_kia_const.te_short);
        } else {
            return level_duration_make(false, subghz_protocol_kia_const.te_short);
        }
    } else if (instance->yield_state < 34) { // Sync (long high, long low)
        instance->yield_state++;
        if ((instance->yield_state - 1) == 32) {
            return level_duration_make(true, subghz_protocol_kia_const.te_long);
        } else {
            return level_duration_make(false, subghz_protocol_kia_const.te_long);
        }
    } else if (instance->yield_state < 34 + (61 * 2)) { // Data (61 bits * 2 pulses/bit)
        uint8_t bit_index = (instance->yield_state - 34) / 2;
        bool pulse_is_first = ((instance->yield_state - 34) % 2 == 0);
        instance->yield_state++;

        bool bit = (instance->generic.data >> (60 - bit_index)) & 1;

        if (pulse_is_first) {
            if (bit) {
                return level_duration_make(true, subghz_protocol_kia_const.te_long);
            } else {
                return level_duration_make(true, subghz_protocol_kia_const.te_short);
            }
        } else {
            if (bit) {
                return level_duration_make(false, subghz_protocol_kia_const.te_long);
            } else {
                return level_duration_make(false, subghz_protocol_kia_const.te_short);
            }
        }
    } else { // Done
        return level_duration_reset();
    }
}

// Allow button/counter updates
void subghz_protocol_encoder_kia_set_button(void* context, uint8_t button) {
    SubGhzProtocolEncoderKIA* instance = context;
    instance->button = button & 0x0F;
    subghz_protocol_encoder_kia_update_data(instance);
}

void subghz_protocol_encoder_kia_set_counter(void* context, uint16_t counter) {
    SubGhzProtocolEncoderKIA* instance = context;
    instance->counter = counter;
    subghz_protocol_encoder_kia_update_data(instance);
}

// Decoder implementation
void *subghz_protocol_decoder_kia_alloc(SubGhzEnvironment *environment)
{
    UNUSED(environment);
    SubGhzProtocolDecoderKIA *instance = malloc(sizeof(SubGhzProtocolDecoderKIA));
    instance->base.protocol = &kia_protocol_v0;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_kia_free(void *context)
{
    furi_assert(context);
    SubGhzProtocolDecoderKIA *instance = context;
    free(instance);
}

void subghz_protocol_decoder_kia_reset(void *context)
{
    furi_assert(context);
    SubGhzProtocolDecoderKIA *instance = context;
    instance->decoder.parser_step = KIADecoderStepReset;
}

void subghz_protocol_decoder_kia_feed(void *context, bool level, uint32_t duration)
{
    furi_assert(context);
    SubGhzProtocolDecoderKIA *instance = context;

    switch (instance->decoder.parser_step)
    {
    case KIADecoderStepReset:
        if ((level) && (DURATION_DIFF(duration, subghz_protocol_kia_const.te_short) < subghz_protocol_kia_const.te_delta))
        {
            instance->decoder.parser_step = KIADecoderStepCheckPreambula;
            instance->decoder.te_last = duration;
            instance->header_count = 0;
        }
        break;
    case KIADecoderStepCheckPreambula:
        if (level)
        {
            if ((DURATION_DIFF(duration, subghz_protocol_kia_const.te_short) < subghz_protocol_kia_const.te_delta) ||
                (DURATION_DIFF(duration, subghz_protocol_kia_const.te_long) < subghz_protocol_kia_const.te_delta))
            {
                instance->decoder.te_last = duration;
            }
            else
            {
                instance->decoder.parser_step = KIADecoderStepReset;
            }
        }
        else if (
            (DURATION_DIFF(duration, subghz_protocol_kia_const.te_short) < subghz_protocol_kia_const.te_delta) &&
            (DURATION_DIFF(instance->decoder.te_last, subghz_protocol_kia_const.te_short) < subghz_protocol_kia_const.te_delta))
        {
            instance->header_count++;
            break;
        }
        else if (
            (DURATION_DIFF(duration, subghz_protocol_kia_const.te_long) < subghz_protocol_kia_const.te_delta) &&
            (DURATION_DIFF(instance->decoder.te_last, subghz_protocol_kia_const.te_long) < subghz_protocol_kia_const.te_delta))
        {
            if (instance->header_count > 15)
            {
                instance->decoder.parser_step = KIADecoderStepSaveDuration;
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 1;
                subghz_protocol_blocks_add_bit(&instance->decoder, 1);
                FURI_LOG_I(TAG, "Starting data decode after %u header pulses", instance->header_count);
            }
            else
            {
                instance->decoder.parser_step = KIADecoderStepReset;
            }
        }
        else
        {
            instance->decoder.parser_step = KIADecoderStepReset;
        }
        break;
    case KIADecoderStepSaveDuration:
        if (level)
        {
            if (duration >=
                (subghz_protocol_kia_const.te_long + subghz_protocol_kia_const.te_delta * 2UL))
            {
                // Signal ended too early!
                FURI_LOG_W(TAG, "Signal ended at %u bits (expected 61). Duration: %lu", 
                          instance->decoder.decode_count_bit, duration);
                
                instance->decoder.parser_step = KIADecoderStepReset;
                if (instance->decoder.decode_count_bit ==
                    subghz_protocol_kia_const.min_count_bit_for_found)
                {
                    instance->generic.data = instance->decoder.decode_data;
                    instance->generic.data_count_bit = instance->decoder.decode_count_bit;
                    if (instance->base.callback)
                        instance->base.callback(&instance->base, instance->base.context);
                }
                else
                {
                    FURI_LOG_E(TAG, "Incomplete signal: only %u bits", instance->decoder.decode_count_bit);
                }
                instance->decoder.decode_data = 0;
                instance->decoder.decode_count_bit = 0;
                break;
            }
            else
            {
                instance->decoder.te_last = duration;
                instance->decoder.parser_step = KIADecoderStepCheckDuration;
            }
        }
        else
        {
            instance->decoder.parser_step = KIADecoderStepReset;
        }
        break;
    case KIADecoderStepCheckDuration:
        if (!level)
        {
            if ((DURATION_DIFF(instance->decoder.te_last, subghz_protocol_kia_const.te_short) < subghz_protocol_kia_const.te_delta) &&
                (DURATION_DIFF(duration, subghz_protocol_kia_const.te_short) < subghz_protocol_kia_const.te_delta))
            {
                subghz_protocol_blocks_add_bit(&instance->decoder, 0);
                if (instance->decoder.decode_count_bit % 10 == 0) {
                    FURI_LOG_D(TAG, "Decoded %u bits so far", instance->decoder.decode_count_bit);
                }
                instance->decoder.parser_step = KIADecoderStepSaveDuration;
            }
            else if (
                (DURATION_DIFF(instance->decoder.te_last, subghz_protocol_kia_const.te_long) < subghz_protocol_kia_const.te_delta) &&
                (DURATION_DIFF(duration, subghz_protocol_kia_const.te_long) < subghz_protocol_kia_const.te_delta))
            {
                subghz_protocol_blocks_add_bit(&instance->decoder, 1);
                if (instance->decoder.decode_count_bit % 10 == 0) {
                    FURI_LOG_D(TAG, "Decoded %u bits so far", instance->decoder.decode_count_bit);
                }
                instance->decoder.parser_step = KIADecoderStepSaveDuration;
            }
            else
            {
                FURI_LOG_W(TAG, "Timing mismatch at bit %u. Last: %lu, Current: %lu", 
                          instance->decoder.decode_count_bit, instance->decoder.te_last, duration);
                instance->decoder.parser_step = KIADecoderStepReset;
            }
        }
        else
        {
            instance->decoder.parser_step = KIADecoderStepReset;
        }
        break;
    }
}

static void subghz_protocol_kia_check_remote_controller(SubGhzBlockGeneric *instance)
{
    instance->serial = (uint32_t)((instance->data >> 12) & 0x0FFFFFFF);
    instance->btn = (instance->data >> 8) & 0x0F;
    instance->cnt = (instance->data >> 40) & 0xFFFF;
}

uint8_t subghz_protocol_decoder_kia_get_hash_data(void *context)
{
    furi_assert(context);
    SubGhzProtocolDecoderKIA *instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_kia_serialize(
    void *context,
    FlipperFormat *flipper_format,
    SubGhzRadioPreset *preset)
{
    furi_assert(context);
    SubGhzProtocolDecoderKIA *instance = context;

    // Ensure fields are extracted
    subghz_protocol_kia_check_remote_controller(&instance->generic);
    
    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;
    
    // Write the header and standard fields manually to ensure correct format
    do {
        // Frequency
        if(!flipper_format_write_uint32(flipper_format, "Frequency", &preset->frequency, 1)) break;
        
        // Preset
        if(!flipper_format_write_string_cstr(flipper_format, "Preset", 
            furi_string_get_cstr(preset->name))) break;
        
        // Protocol
        if(!flipper_format_write_string_cstr(flipper_format, "Protocol", 
            instance->generic.protocol_name)) break;
        
        // Bit - FORCE 61 bits
        uint32_t bits = 61;
        if(!flipper_format_write_uint32(flipper_format, "Bit", &bits, 1)) break;
        
        // Key - write as continuous hex string without spaces
        char key_str[20];
        snprintf(key_str, sizeof(key_str), "%016llX", instance->generic.data);
        if(!flipper_format_write_string_cstr(flipper_format, "Key", key_str)) break;
        
        // Additional fields
        if(!flipper_format_write_uint32(flipper_format, "Serial", &instance->generic.serial, 1)) break;
        
        uint32_t temp = instance->generic.btn;
        if(!flipper_format_write_uint32(flipper_format, "Btn", &temp, 1)) break;
        
        if(!flipper_format_write_uint32(flipper_format, "Cnt", &instance->generic.cnt, 1)) break;
        
        ret = SubGhzProtocolStatusOk;
    } while(false);

    return ret;
}

SubGhzProtocolStatus
subghz_protocol_decoder_kia_deserialize(void *context, FlipperFormat *flipper_format)
{
    furi_assert(context);
    SubGhzProtocolDecoderKIA *instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_kia_const.min_count_bit_for_found);
}

void subghz_protocol_decoder_kia_get_string(void *context, FuriString *output)
{
    furi_assert(context);
    SubGhzProtocolDecoderKIA *instance = context;

    subghz_protocol_kia_check_remote_controller(&instance->generic);
    uint32_t code_found_hi = instance->generic.data >> 32;
    uint32_t code_found_lo = instance->generic.data & 0x00000000ffffffff;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%08lX%08lX\r\n"
        "Sn:%07lX Btn:%X Cnt:%04lX\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        code_found_hi,
        code_found_lo,
        instance->generic.serial,
        instance->generic.btn,
        instance->generic.cnt);
}