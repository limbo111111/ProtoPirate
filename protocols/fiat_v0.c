#include "fiat_v0.h"
#include <lib/toolbox/manchester_decoder.h>

#define TAG "FiatProtocolV0"

static const SubGhzBlockConst subghz_protocol_fiat_v0_const = {
    .te_short = 200,
    .te_long = 400,
    .te_delta = 100,
    .min_count_bit_for_found = 64,
};

struct SubGhzProtocolDecoderFiatV0 {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    SubGhzBlockGeneric generic;
    ManchesterState manchester_state;
    uint8_t decoder_state;
    uint16_t preamble_count;
    uint32_t data_low;
    uint32_t data_high;
    uint8_t bit_count;
    uint32_t hop;
    uint32_t fix;
    uint8_t endbyte;
    uint8_t final_count;
    uint32_t te_last;
};

struct SubGhzProtocolEncoderFiatV0 {
    SubGhzProtocolEncoderBase base;
    SubGhzProtocolBlockEncoder encoder;
    SubGhzBlockGeneric generic;
    uint32_t hop;
    uint32_t fix;
    uint8_t endbyte;
    uint64_t data;
    uint16_t yield_state;
};

typedef enum {
    FiatV0DecoderStepReset = 0,
    FiatV0DecoderStepPreamble = 1,
    FiatV0DecoderStepData = 2,
} FiatV0DecoderStep;

const SubGhzProtocolDecoder subghz_protocol_fiat_v0_decoder = {
    .alloc = subghz_protocol_decoder_fiat_v0_alloc,
    .free = subghz_protocol_decoder_fiat_v0_free,
    .feed = subghz_protocol_decoder_fiat_v0_feed,
    .reset = subghz_protocol_decoder_fiat_v0_reset,
    .get_hash_data = subghz_protocol_decoder_fiat_v0_get_hash_data,
    .serialize = subghz_protocol_decoder_fiat_v0_serialize,
    .deserialize = subghz_protocol_decoder_fiat_v0_deserialize,
    .get_string = subghz_protocol_decoder_fiat_v0_get_string,
};

const SubGhzProtocolEncoder subghz_protocol_fiat_v0_encoder = {
    .alloc = subghz_protocol_encoder_fiat_v0_alloc,
    .free = subghz_protocol_encoder_fiat_v0_free,
    .deserialize = subghz_protocol_encoder_fiat_v0_deserialize,
    .stop = subghz_protocol_encoder_fiat_v0_stop,
    .yield = subghz_protocol_encoder_fiat_v0_yield,
};

const SubGhzProtocol fiat_protocol_v0 = {
    .name = FIAT_PROTOCOL_V0_NAME,
    .type = SubGhzProtocolTypeDynamic,
    .flag = SubGhzProtocolFlag_433 | SubGhzProtocolFlag_FM | SubGhzProtocolFlag_Decodable,
    .decoder = &subghz_protocol_fiat_v0_decoder,
    .encoder = &subghz_protocol_fiat_v0_encoder,
};

void* subghz_protocol_decoder_fiat_v0_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolDecoderFiatV0* instance = malloc(sizeof(SubGhzProtocolDecoderFiatV0));
    instance->base.protocol = &fiat_protocol_v0;
    instance->generic.protocol_name = instance->base.protocol->name;
    return instance;
}

void subghz_protocol_decoder_fiat_v0_free(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderFiatV0* instance = context;
    free(instance);
}

void subghz_protocol_decoder_fiat_v0_reset(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderFiatV0* instance = context;
    instance->decoder.parser_step = FiatV0DecoderStepReset;
    instance->decoder_state = 0;
    instance->preamble_count = 0;
    instance->data_low = 0;
    instance->data_high = 0;
    instance->bit_count = 0;
    instance->hop = 0;
    instance->fix = 0;
    instance->endbyte = 0;
    instance->final_count = 0;
    instance->te_last = 0;
    instance->manchester_state = ManchesterStateMid1;
}

void subghz_protocol_decoder_fiat_v0_feed(void* context, bool level, uint32_t duration) {
    furi_assert(context);
    SubGhzProtocolDecoderFiatV0* instance = context;
    uint32_t te_short = (uint32_t)subghz_protocol_fiat_v0_const.te_short;
    uint32_t te_long = (uint32_t)subghz_protocol_fiat_v0_const.te_long;
    uint32_t te_delta = (uint32_t)subghz_protocol_fiat_v0_const.te_delta;
    uint32_t gap_threshold = 800;
    uint32_t diff;
    switch(instance->decoder_state) {
    case FiatV0DecoderStepReset:
        if(!level) {
            return;
        }
        if(duration < te_short) {
            diff = te_short - duration;
        } else {
            diff = duration - te_short;
        }
        if(diff < te_delta) {
            instance->data_low = 0;
            instance->data_high = 0;
            instance->decoder_state = FiatV0DecoderStepPreamble;
            instance->te_last = duration;
            instance->preamble_count = 0;
            instance->bit_count = 0;
            manchester_advance(
                instance->manchester_state,
                ManchesterEventReset,
                &instance->manchester_state,
                NULL);
        }
        break;
    case FiatV0DecoderStepPreamble:
        if(level) {
            return;
        }
        if(duration < te_short) {
            diff = te_short - duration;
            if(diff < te_delta) {
                instance->preamble_count++;
                instance->te_last = duration;
                if(instance->preamble_count >= 0x96) {
                    if(duration < gap_threshold) {
                        diff = gap_threshold - duration;
                    } else {
                        diff = duration - gap_threshold;
                    }
                    if(diff < te_delta) {
                        instance->decoder_state = FiatV0DecoderStepData;
                        instance->preamble_count = 0;
                        instance->data_low = 0;
                        instance->data_high = 0;
                        instance->bit_count = 0;
                        instance->te_last = duration;
                        return;
                    }
                }
            } else {
                instance->decoder_state = FiatV0DecoderStepReset;
                if(instance->preamble_count >= 0x96) {
                    if(duration < gap_threshold) {
                        diff = gap_threshold - duration;
                    } else {
                        diff = duration - gap_threshold;
                    }
                    if(diff < te_delta) {
                        instance->decoder_state = FiatV0DecoderStepData;
                        instance->preamble_count = 0;
                        instance->data_low = 0;
                        instance->data_high = 0;
                        instance->bit_count = 0;
                        instance->te_last = duration;
                        return;
                    }
                }
            }
        } else {
            diff = duration - te_short;
            if(diff < te_delta) {
                instance->preamble_count++;
                instance->te_last = duration;
            } else {
                instance->decoder_state = FiatV0DecoderStepReset;
            }
            if(instance->preamble_count >= 0x96) {
                if(duration >= 799) {
                    diff = duration - gap_threshold;
                } else {
                    diff = gap_threshold - duration;
                }
                if(diff < te_delta) {
                    instance->decoder_state = FiatV0DecoderStepData;
                    instance->preamble_count = 0;
                    instance->data_low = 0;
                    instance->data_high = 0;
                    instance->bit_count = 0;
                    instance->te_last = duration;
                    return;
                }
            }
        }
        break;
    case FiatV0DecoderStepData:
        ManchesterEvent event = ManchesterEventReset;
        if(duration < te_short) {
            diff = te_short - duration;
            if(diff < te_delta) {
                event = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
            }
        } else {
            diff = duration - te_short;
            if(diff < te_delta) {
                event = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
            } else {
                if(duration < te_long) {
                    diff = te_long - duration;
                } else {
                    diff = duration - te_long;
                }
                if(diff < te_delta) {
                    event = level ? ManchesterEventLongLow : ManchesterEventLongHigh;
                }
            }
        }

        if(event != ManchesterEventReset) {
            bool data_bit_bool;
            if(manchester_advance(
                   instance->manchester_state,
                   event,
                   &instance->manchester_state,
                   &data_bit_bool)) {
                uint32_t new_bit = data_bit_bool ? 1 : 0;

                uint32_t carry = (instance->data_low >> 31) & 1;
                instance->data_low = (instance->data_low << 1) | new_bit;
                instance->data_high = (instance->data_high << 1) | carry;

                instance->bit_count++;

                if(instance->bit_count == 0x40) {
                    instance->fix = instance->data_low;
                    instance->hop = instance->data_high;
                    instance->data_low = 0;
                    instance->data_high = 0;
                }

                if(instance->bit_count > 0x46) {
                    instance->final_count = instance->bit_count;

                    instance->endbyte = (uint8_t)instance->data_low;

                    instance->generic.data = ((uint64_t)instance->hop << 32) | instance->fix;
                    instance->generic.data_count_bit = 64;
                    instance->generic.serial = instance->fix;
                    instance->generic.btn =
                        instance->endbyte; // still exported as btn for UI compatibility
                    instance->generic.cnt = instance->hop;

                    if(instance->base.callback) {
                        instance->base.callback(&instance->base, instance->base.context);
                    }

                    instance->data_low = 0;
                    instance->data_high = 0;
                    instance->bit_count = 0;
                    instance->decoder_state = FiatV0DecoderStepReset;
                }
            }
        }
        instance->te_last = duration;
        break;
    }
}

uint8_t subghz_protocol_decoder_fiat_v0_get_hash_data(void* context) {
    furi_assert(context);
    SubGhzProtocolDecoderFiatV0* instance = context;
    SubGhzBlockDecoder decoder = {
        .decode_data = instance->generic.data,
        .decode_count_bit = instance->generic.data_count_bit};
    return subghz_protocol_blocks_get_hash_data(&decoder, (decoder.decode_count_bit / 8) + 1);
}

SubGhzProtocolStatus subghz_protocol_decoder_fiat_v0_serialize(
    void* context,
    FlipperFormat* flipper_format,
    SubGhzRadioPreset* preset) {
    furi_assert(context);
    SubGhzProtocolDecoderFiatV0* instance = context;
    return subghz_block_generic_serialize(&instance->generic, flipper_format, preset);
}

SubGhzProtocolStatus
    subghz_protocol_decoder_fiat_v0_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolDecoderFiatV0* instance = context;
    return subghz_block_generic_deserialize_check_count_bit(
        &instance->generic, flipper_format, subghz_protocol_fiat_v0_const.min_count_bit_for_found);
}

void subghz_protocol_decoder_fiat_v0_get_string(void* context, FuriString* output) {
    furi_assert(context);
    SubGhzProtocolDecoderFiatV0* instance = context;

    uint32_t code_found_hi = instance->hop;
    uint32_t code_found_lo = instance->fix;

    furi_string_cat_printf(
        output,
        "%s %dbit\r\n"
        "Key:%08lX%08lX\r\n"
        "Hop:%08lX Fix:%08lX\r\n"
        "EndByte:%02X\r\n",
        instance->generic.protocol_name,
        instance->generic.data_count_bit,
        code_found_hi,
        code_found_lo,
        instance->hop,
        instance->fix,
        instance->endbyte);
}

static void subghz_protocol_encoder_fiat_v0_update_data(SubGhzProtocolEncoderFiatV0* instance) {
    instance->data = ((uint64_t)instance->hop << 32) | instance->fix;
    instance->yield_state = 0;
}

void* subghz_protocol_encoder_fiat_v0_alloc(SubGhzEnvironment* environment) {
    UNUSED(environment);
    SubGhzProtocolEncoderFiatV0* instance = malloc(sizeof(SubGhzProtocolEncoderFiatV0));
    instance->base.protocol = &fiat_protocol_v0;
    instance->generic.protocol_name = instance->base.protocol->name;
    instance->yield_state = 0;
    return instance;
}

void subghz_protocol_encoder_fiat_v0_free(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderFiatV0* instance = context;
    free(instance);
}

SubGhzProtocolStatus
    subghz_protocol_encoder_fiat_v0_deserialize(void* context, FlipperFormat* flipper_format) {
    furi_assert(context);
    SubGhzProtocolEncoderFiatV0* instance = context;

    if(subghz_block_generic_deserialize(&instance->generic, flipper_format) !=
       SubGhzProtocolStatusOk) {
        return SubGhzProtocolStatusError;
    }

    instance->hop = instance->generic.cnt;
    instance->fix = instance->generic.serial;
    instance->endbyte = instance->generic.btn;

    subghz_protocol_encoder_fiat_v0_update_data(instance);

    return SubGhzProtocolStatusOk;
}

void subghz_protocol_encoder_fiat_v0_stop(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderFiatV0* instance = context;
    instance->yield_state = 0;
}

LevelDuration subghz_protocol_encoder_fiat_v0_yield(void* context) {
    furi_assert(context);
    SubGhzProtocolEncoderFiatV0* instance = context;

    if(instance->yield_state < 300) {
        instance->yield_state++;
        return level_duration_make(
            instance->yield_state % 2 != 0, subghz_protocol_fiat_v0_const.te_short);
    } else if(instance->yield_state == 300) {
        instance->yield_state++;
        return level_duration_make(false, 800);
    } else if(instance->yield_state < 301 + (71 * 2)) {
        uint8_t bit_index = (instance->yield_state - 301) / 2;
        bool pulse_is_first = ((instance->yield_state - 301) % 2 == 0);
        instance->yield_state++;

        bool bit;
        if(bit_index < 64) {
            bit = (instance->data >> (63 - bit_index)) & 1;
        } else {
            bit = (instance->endbyte >> (70 - bit_index)) & 1;
        }

        if(pulse_is_first) {
            return level_duration_make(!bit, subghz_protocol_fiat_v0_const.te_short);
        } else {
            return level_duration_make(bit, subghz_protocol_fiat_v0_const.te_short);
        }
    } else {
        return level_duration_reset();
    }
}
