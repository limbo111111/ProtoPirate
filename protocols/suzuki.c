#include "suzuki.h"

#define TAG "SuzukiProtocol"

static const SubGhzBlockConst subghz_protocol_suzuki_const = {
    .te_short = 250,
    .te_long = 500,
    .te_delta = 100,
    .min_count_bit_for_found = 64,
};

#define SUZUKI_GAP_TIME 2000
#define SUZUKI_GAP_DELTA 400

typedef struct SubGhzProtocolDecoderSuzuki
{
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;

    uint32_t te_last;
    uint32_t data_low;
    uint32_t data_high;
    uint8_t data_count_bit;
    uint16_t header_count;
} SubGhzProtocolDecoderSuzuki;

#include <furi.h>

typedef struct SubGhzProtocolEncoderSuzuki
{
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;

    // Encoder state
    uint32_t serial;
    uint8_t button;
    uint16_t count;
    uint8_t crc;

    uint16_t yield_state;
    uint64_t data;
} SubGhzProtocolEncoderSuzuki;

typedef enum
{
    SuzukiDecoderStepReset = 0,
    SuzukiDecoderStepFoundStartPulse,
    SuzukiDecoderStepSaveDuration,
} SuzukiDecoderStep;

// Forward declarations for encoder
void* subghz_protocol_encoder_suzuki_alloc(SubGhzEnvironment* environment);
void subghz_protocol_encoder_suzuki_free(void* context);
SubGhzProtocolStatus subghz_protocol_encoder_suzuki_deserialize(void* context, FlipperFormat* flipper_format);
void subghz_protocol_encoder_suzuki_stop(void* context);
LevelDuration subghz_protocol_encoder_suzuki_yield(void* context);


const SubGhzProtocolDecoder subghz_protocol_suzuki_decoder = {
    .alloc = subghz_protocol_decoder_suzuki_alloc,
    .free = subghz_protocol_decoder_suzuki_free,
    .feed = subghz_protocol_decoder_suzuki_feed,
    .reset = subghz_protocol_decoder_suzuki_reset,
    .get_hash_data = subghz_protocol_decoder_suzuki_get_hash_data,
    .serialize = subghz_protocol_decoder_suzuki_serialize,
    .deserialize = subghz_protocol_decoder_suzuki_deserialize,
    .get_string = subghz_protocol_decoder_suzuki_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_suzuki_encoder = {
    .alloc = subghz_protocol_encoder_suzuki_alloc,
    .free = subghz_protocol_encoder_suzuki_free,
    .deserialize = subghz_protocol_encoder_suzuki_deserialize,
    .stop = subghz_protocol_encoder_suzuki_stop,
    .yield = subghz_protocol_encoder_suzuki_yield,
};

const SubGhzProtocol suzuki_protocol = {
    .name = SUZUKI_PROTOCOL_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_AM | SubGhzProtocolFlag_Decodable | SubGhzProtocolFlag_Send,
    .decoder = &subghz_protocol_suzuki_decoder,
    .encoder = &subghz_protocol_suzuki_encoder,
};

static void suzuki_add_bit(SubGhzProtocolDecoderSuzuki *instance, uint32_t bit)
{
    uint32_t carry = instance->data_low >> 31;
    instance->data_low = (instance->data_low << 1) | bit;
    instance->data_high = (instance->data_high << 1) | carry;
    instance->data_count_bit++;
}

void *subghz_protocol_decoder_suzuki_alloc(SubGhzEnvironment *environment)
{
    UNUSED(environment);
    SubGhzProtocolDecoderSuzuki *instance = malloc(sizeof(SubGhzProtocolDecoderSuzuki));
    instance->base.protocol = &suzuki_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_suzuki_free(void *context)
{
    furi_assert(context);
    SubGhzProtocolDecoderSuzuki *instance = context;
    free(instance);
}

void subghz_protocol_decoder_suzuki_reset(void *context)
{
    furi_assert(context);
    SubGhzProtocolDecoderSuzuki *instance = context;
    instance->decoder.parser_step = SuzukiDecoderStepReset;
    instance->header_count = 0;
    instance->data_count_bit = 0;
    instance->data_low = 0;
    instance->data_high = 0;
}

void subghz_protocol_decoder_suzuki_feed(void *context, bool level, uint32_t duration)
{
    furi_assert(context);
    SubGhzProtocolDecoderSuzuki *instance = context;

    switch (instance->decoder.parser_step)
    {
    case SuzukiDecoderStepReset:
        // Wait for short HIGH pulse (~250µs) to start preamble
        if (!level)
            return;

        if (DURATION_DIFF(duration, subghz_protocol_suzuki_const.te_short) > subghz_protocol_suzuki_const.te_delta)
        {
            return;
        }

        instance->data_low = 0;
        instance->data_high = 0;
        instance->decoder.parser_step = SuzukiDecoderStepFoundStartPulse;
        instance->header_count = 0;
        instance->data_count_bit = 0;
        break;

    case SuzukiDecoderStepFoundStartPulse:
        if (level)
        {
            // HIGH pulse
            if (instance->header_count < 257)
            {
                // Still in preamble - just count
                return;
            }

            // After preamble, look for long HIGH to start data
            if (DURATION_DIFF(duration, subghz_protocol_suzuki_const.te_long) < subghz_protocol_suzuki_const.te_delta)
            {
                instance->decoder.parser_step = SuzukiDecoderStepSaveDuration;
                suzuki_add_bit(instance, 1);
            }
            // Ignore short HIGHs after preamble until we see a long one
        }
        else
        {
            // LOW pulse - count as header if short
            if (DURATION_DIFF(duration, subghz_protocol_suzuki_const.te_short) < subghz_protocol_suzuki_const.te_delta)
            {
                instance->te_last = duration;
                instance->header_count++;
            }
            else
            {
                instance->decoder.parser_step = SuzukiDecoderStepReset;
            }
        }
        break;

    case SuzukiDecoderStepSaveDuration:
        if (level)
        {
            // HIGH pulse - determines bit value
            // Long HIGH (~500µs) = 1, Short HIGH (~250µs) = 0
            if (DURATION_DIFF(duration, subghz_protocol_suzuki_const.te_long) < subghz_protocol_suzuki_const.te_delta)
            {
                suzuki_add_bit(instance, 1);
            }
            else if (DURATION_DIFF(duration, subghz_protocol_suzuki_const.te_short) < subghz_protocol_suzuki_const.te_delta)
            {
                suzuki_add_bit(instance, 0);
            }
            else
            {
                instance->decoder.parser_step = SuzukiDecoderStepReset;
            }
            // Stay in this state for next bit
        }
        else
        {
            // LOW pulse - check for gap (end of transmission)
            if (DURATION_DIFF(duration, SUZUKI_GAP_TIME) < SUZUKI_GAP_DELTA)
            {
                // Gap found - end of transmission
                if (instance->data_count_bit == 64)
                {
                    instance->generic.data_count_bit = 64;
                    instance->generic.data = ((uint64_t)instance->data_high << 32) | (uint64_t)instance->data_low;

                    // Check manufacturer nibble (should be 0xF)
                    uint8_t manufacturer = (instance->data_high >> 28) & 0xF;
                    if (manufacturer == 0xF)
                    {
                        // Extract fields
                        uint64_t data = instance->generic.data;
                        uint32_t serial_button = ((instance->data_high & 0xFFF) << 20) | (instance->data_low >> 12);
                        instance->generic.serial = serial_button >> 4;
                        instance->generic.btn = serial_button & 0xF;
                        instance->generic.cnt = (data >> 44) & 0xFFFF;

                        if (instance->base.callback)
                        {
                            instance->base.callback(&instance->base, instance->base.context);
                        }
                    }
                }
                instance->decoder.parser_step = SuzukiDecoderStepReset;
            }
            // Short LOW pulses are ignored - stay in this state
        }
        break;
    }
}

uint8_t subghz_protocol_decoder_suzuki_get_hash_data(void *context)
{
    furi_assert(context);
    SubGhzProtocolDecoderSuzuki *instance = context;
    return subghz_protocol_blocks_get_hash_data(
        &instance->decoder, (instance->decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_suzuki_serialize(
    void *context,
    FlipperFormat *flipper_format,
    SubGhzRadioPreset *preset)
{
    furi_assert(context);
    SubGhzProtocolDecoderSuzuki *instance = context;

    SubGhzProtocolStatus ret = SubGhzProtocolStatusError;

    ret = subghz_block_generic_serialize(&instance->generic, flipper_format, preset);

    if (ret == SubGhzProtocolStatusOk)
    {
        // Extract and save CRC
        uint32_t crc = (instance->generic.data >> 4) & 0xFF;
        flipper_format_write_uint32(flipper_format, "CRC", &crc, 1);

        // Save decoded fields
        flipper_format_write_uint32(flipper_format, "Serial", &instance->generic.serial, 1);

        uint32_t temp = instance->generic.btn;
        flipper_format_write_uint32(flipper_format, "Btn", &temp, 1);

        flipper_format_write_uint32(flipper_format, "Cnt", &instance->generic.cnt, 1);
    }

    return ret;
}

SubGhzProtocolStatus subghz_protocol_decoder_suzuki_deserialize(void *context, FlipperFormat *flipper_format)
{
    furi_assert(context);
    SubGhzProtocolDecoderSuzuki *instance = context;
    return subghz_block_generic_deserialize(&instance->generic, flipper_format);
}

void subghz_protocol_decoder_suzuki_get_string(void *context, FuriString *output)
{
    furi_assert(context);
    SubGhzProtocolDecoderSuzuki *instance = context;

    uint64_t data = instance->generic.data;
    uint32_t key_high = (data >> 32) & 0xFFFFFFFF;
    uint32_t key_low = data & 0xFFFFFFFF;
    uint8_t crc = (data >> 4) & 0xFF;

    const char* btn_name;
    switch (instance->generic.btn)
    {
    case 1:
        btn_name = "PANIC";
        break;
    case 2:
        btn_name = "TRUNK";
        break;
    case 3:
        btn_name = "LOCK";
        break;
    case 4:
        btn_name = "UNLOCK";
        break;
    default:
        btn_name = "Unknown";
        break;
    }

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%08lX%08lX\r\n"
        "Sn:%07lX Btn:%X %s\r\n"
        "Cnt:%04lX CRC:%02X\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        key_high,
        key_low,
        instance->generic.serial,
        instance->generic.btn,
        btn_name,
        instance->generic.cnt,
        crc);
}

// Encoder implementation
void* subghz_protocol_encoder_suzuki_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderSuzuki* instance = malloc(sizeof(SubGhzProtocolEncoderSuzuki));
    instance->base.protocol = &suzuki_protocol;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->yield_state = 0;
    instance->serial = 0;
    instance->button = 0;
    instance->count = 0;
    instance->crc = 0;
    return instance;
}

void subghz_protocol_encoder_suzuki_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderSuzuki* instance = context;
    free(instance);
}

static void subghz_protocol_encoder_suzuki_update_data(SubGhzProtocolEncoderSuzuki* instance) {
    uint64_t data = 0;
    uint32_t serial_button = (instance->serial << 4) | (instance->button & 0xF);

    data |= (uint64_t)0xF << 60; // Manufacturer nibble
    data |= (uint64_t)instance->count << 44;
    data |= (uint64_t)serial_button << 12;
    data |= (uint64_t)instance->crc << 4;
    // Low 4 bits are 0

    instance->data = data;
    instance->yield_state = 0;
}

SubGhzProtocolStatus subghz_protocol_encoder_suzuki_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderSuzuki* instance = context;

    if (subghz_block_generic_deserialize(&instance->generic, flipper_format) != SubGhzProtocolStatusOk) {
        return SubGhzProtocolStatusError;
    }

    if (!flipper_format_read_uint32(flipper_format, "Serial", &instance->serial, 1) ||
        !flipper_format_read_uint32(flipper_format, "Btn", (uint32_t*)&instance->button, 1) ||
        !flipper_format_read_uint32(flipper_format, "Cnt", (uint32_t*)&instance->count, 1) ||
        !flipper_format_read_uint32(flipper_format, "CRC", (uint32_t*)&instance->crc, 1)) {

        // Fallback for older captures
        uint64_t data = instance->generic.data;
        uint32_t serial_button = ((instance->generic.data >> 12) & 0xFFFFFFF);
        instance->serial = serial_button >> 4;
        instance->button = serial_button & 0xF;
        instance->count = (data >> 44) & 0xFFFF;
        instance->crc = (data >> 4) & 0xFF;
    }

    subghz_protocol_encoder_suzuki_update_data(instance);

    return SubGhzProtocolStatusOk;
}

void subghz_protocol_encoder_suzuki_stop(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderSuzuki* instance = context;
    instance->yield_state = 0;
}

LevelDuration subghz_protocol_encoder_suzuki_yield(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderSuzuki* instance = context;

    // Preamble: ~257 pairs of (short high, short low)
    if (instance->yield_state < 514) {
        instance->yield_state++;
        if ((instance->yield_state - 1) % 2 == 0) {
             return level_duration_make(true, subghz_protocol_suzuki_const.te_short);
        } else {
             return level_duration_make(false, subghz_protocol_suzuki_const.te_short);
        }
    }
    // Data: 64 bits
    // First bit is always 1, sent as a long high
    else if (instance->yield_state == 514) {
        instance->yield_state++;
        return level_duration_make(true, subghz_protocol_suzuki_const.te_long);
    }
    // 0 = short high, 1 = long high. All followed by short low
    else if (instance->yield_state < 515 + (63 * 2)) {
         uint8_t bit_index = (instance->yield_state - 515) / 2;
         bool pulse_is_first = ((instance->yield_state - 515) % 2 == 0);
         instance->yield_state++;

         // Skip first bit, it was sent as sync
         bool bit = (instance->data >> (62 - bit_index)) & 1;

         if(pulse_is_first) {
             if(bit) {
                 return level_duration_make(true, subghz_protocol_suzuki_const.te_long);
             } else {
                 return level_duration_make(true, subghz_protocol_suzuki_const.te_short);
             }
         } else {
             return level_duration_make(false, subghz_protocol_suzuki_const.te_short);
         }
    }
     // Gap
    else if (instance->yield_state == 515 + (63 * 2)) {
        instance->yield_state++;
        return level_duration_make(false, SUZUKI_GAP_TIME);
    }
    else { // Done
        return level_duration_reset();
    }
}