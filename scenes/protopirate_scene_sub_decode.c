// scenes/protopirate_scene_sub_decode.c
#include "../protopirate_app_i.h"
#include "../protocols/protocol_items.h"
#include <dialogs/dialogs.h>
#include <ctype.h>
#include <math.h>

#define TAG "ProtoPirateSubDecode"

#define SUBGHZ_APP_FOLDER EXT_PATH("subghz")
#define SAMPLES_PER_TICK 256
#define MAX_RAW_SAMPLES 8192
#define SUCCESS_DISPLAY_TICKS 18
#define FAILURE_DISPLAY_TICKS 18

// Decode state machine
typedef enum {
    DecodeStateIdle,
    DecodeStateOpenFile,
    DecodeStateReadHeader,
    DecodeStateLoadRawSamples,
    DecodeStateDecodingRaw,
    DecodeStateDecodingProtocol,
    DecodeStateShowSuccess,
    DecodeStateShowFailure,
    DecodeStateDone,
} DecodeState;

// Context for the whole decode operation
typedef struct {
    DecodeState state;
    uint16_t animation_frame;
    uint8_t result_display_counter;
    
    // File info
    FuriString* file_path;
    FuriString* protocol_name;
    FuriString* result;
    FuriString* error_info;
    uint32_t frequency;
    
    // File handle
    Storage* storage;
    FlipperFormat* ff;
    
    // RAW decode state
    int32_t* raw_samples;
    size_t total_samples;
    size_t current_sample;
    size_t current_protocol_idx;
    void* current_decoder;
    const SubGhzProtocol* current_protocol;
    bool decode_success;
    
    // Callback context
    bool callback_fired;
    FuriString* decoded_string;
} SubDecodeContext;

static SubDecodeContext* g_decode_ctx = NULL;

// Callback when decoder successfully decodes
static void protopirate_decode_callback(SubGhzProtocolDecoderBase* decoder_base, void* context) {
    SubDecodeContext* ctx = context;
    ctx->callback_fired = true;
    
    if(ctx->current_protocol && ctx->current_protocol->decoder && ctx->current_protocol->decoder->get_string) {
        ctx->current_protocol->decoder->get_string(decoder_base, ctx->decoded_string);
    }
    
    FURI_LOG_I(TAG, "Decode callback fired for %s!", ctx->current_protocol->name);
}

// Case-insensitive string search
static bool str_contains_ci(const char* haystack, const char* needle) {
    if(!haystack || !needle) return false;
    
    size_t haystack_len = strlen(haystack);
    size_t needle_len = strlen(needle);
    
    if(needle_len > haystack_len) return false;
    
    for(size_t i = 0; i <= haystack_len - needle_len; i++) {
        bool match = true;
        for(size_t j = 0; j < needle_len; j++) {
            if(tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j])) {
                match = false;
                break;
            }
        }
        if(match) return true;
    }
    return false;
}

// Check if protocol names match
static bool protocol_names_match(const char* file_proto, const char* registry_proto) {
    if(!file_proto || !registry_proto) return false;
    
    if(strcasecmp(file_proto, registry_proto) == 0) return true;
    if(str_contains_ci(file_proto, registry_proto)) return true;
    
    const char* file_version = NULL;
    const char* reg_version = NULL;
    
    for(const char* p = file_proto; *p; p++) {
        if((*p == 'V' || *p == 'v') && isdigit((unsigned char)*(p + 1))) {
            file_version = p;
            break;
        }
    }
    
    for(const char* p = registry_proto; *p; p++) {
        if((*p == 'V' || *p == 'v') && isdigit((unsigned char)*(p + 1))) {
            reg_version = p;
            break;
        }
    }
    
    if(file_version && reg_version) {
        size_t file_ver_len = 0;
        size_t reg_ver_len = 0;
        
        for(const char* p = file_version; *p && (isalnum((unsigned char)*p) || *p == '/'); p++) {
            file_ver_len++;
        }
        for(const char* p = reg_version; *p && (isalnum((unsigned char)*p) || *p == '/'); p++) {
            reg_ver_len++;
        }
        
        if(file_ver_len == reg_ver_len && 
           strncasecmp(file_version, reg_version, file_ver_len) == 0) {
            if((str_contains_ci(file_proto, "KIA") || str_contains_ci(file_proto, "HYU")) && 
               str_contains_ci(registry_proto, "Kia")) {
                return true;
            }
            if(str_contains_ci(file_proto, "Ford") && str_contains_ci(registry_proto, "Ford")) {
                return true;
            }
            if(str_contains_ci(file_proto, "Subaru") && str_contains_ci(registry_proto, "Subaru")) {
                return true;
            }
            if(str_contains_ci(file_proto, "Suzuki") && str_contains_ci(registry_proto, "Suzuki")) {
                return true;
            }
            if(str_contains_ci(file_proto, "VW") && str_contains_ci(registry_proto, "VW")) {
                return true;
            }
        }
    }
    
    return false;
}

// Get human readable error string from status
static const char* get_protocol_status_string(SubGhzProtocolStatus status) {
    switch(status) {
        case SubGhzProtocolStatusOk: return "OK";
        case SubGhzProtocolStatusErrorParserHeader: return "Header parse error";
        case SubGhzProtocolStatusErrorParserFrequency: return "Frequency error";
        case SubGhzProtocolStatusErrorParserPreset: return "Preset error";
        case SubGhzProtocolStatusErrorParserCustomPreset: return "Custom preset error";
        case SubGhzProtocolStatusErrorParserProtocolName: return "Protocol name error";
        case SubGhzProtocolStatusErrorParserOthers: return "Parse error";
        case SubGhzProtocolStatusErrorValueBitCount: return "Bit count mismatch";
        case SubGhzProtocolStatusErrorParserKey: return "Key parse error";
        case SubGhzProtocolStatusErrorParserTe: return "TE parse error";
        default: return "Unknown error";
    }
}

// Draw the decoding animation
static void protopirate_decode_draw_callback(Canvas* canvas, void* context) {
    UNUSED(context);
    SubDecodeContext* ctx = g_decode_ctx;
    if(!ctx) return;
    
    canvas_clear(canvas);
    
    if(ctx->state == DecodeStateIdle || ctx->state == DecodeStateDone) {
        return;
    }
    
    canvas_set_color(canvas, ColorBlack);
    uint16_t frame = ctx->animation_frame;
    
    // Check for success/failure display states
    if(ctx->state == DecodeStateShowSuccess) {
        // Success screen
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 6, AlignCenter, AlignTop, "DECODED!");
        
        // Checkmark animation
        int check_progress = ctx->result_display_counter * 3;
        int cx = 64, cy = 32;
        int size = 12;
        
        // First stroke of check (going down-right from left)
        int stroke1_max = size;
        int stroke1_len = (check_progress > stroke1_max) ? stroke1_max : check_progress;
        for(int i = 0; i <= stroke1_len; i++) {
            canvas_draw_dot(canvas, cx - size + i, cy - size/2 + i);
            canvas_draw_dot(canvas, cx - size + i, cy - size/2 + i + 1);
            canvas_draw_dot(canvas, cx - size + i + 1, cy - size/2 + i);
        }
        
        // Second stroke of check (going up-right)
        if(check_progress > stroke1_max) {
            int stroke2_max = size * 2;
            int stroke2_len = check_progress - stroke1_max;
            if(stroke2_len > stroke2_max) stroke2_len = stroke2_max;
            for(int i = 0; i <= stroke2_len; i++) {
                canvas_draw_dot(canvas, cx + i, cy + size/2 - i);
                canvas_draw_dot(canvas, cx + i, cy + size/2 - i - 1);
                canvas_draw_dot(canvas, cx + i + 1, cy + size/2 - i);
            }
        }
        
        // Radiating dots
        for(int r = 0; r < 3; r++) {
            int radius = ((frame * 2 + r * 12) % 35) + 8;
            if(radius < 30) {
                for(int angle = 0; angle < 12; angle++) {
                    float a = (float)angle * 3.14159f * 2.0f / 12.0f;
                    int x = cx + (int)(radius * cosf(a));
                    int y = cy + (int)(radius * sinf(a));
                    if(x >= 0 && x < 128 && y >= 0 && y < 64) {
                        canvas_draw_dot(canvas, x, y);
                    }
                }
            }
        }
        
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 54, AlignCenter, AlignTop, "Signal matched!");
        return;
    }
    
    if(ctx->state == DecodeStateShowFailure) {
        // Failure screen
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 6, AlignCenter, AlignTop, "NO MATCH");
        
        // X animation
        int x_progress = ctx->result_display_counter * 3;
        int cx = 64, cy = 32;
        int size = 10;
        int stroke_len = size * 2 + 1;  // Full diagonal length
        
        // First stroke: top-left to bottom-right
        int stroke1_len = (x_progress > stroke_len) ? stroke_len : x_progress;
        for(int i = 0; i < stroke1_len; i++) {
            int x = cx - size + i;
            int y = cy - size + i;
            canvas_draw_dot(canvas, x, y);
            canvas_draw_dot(canvas, x + 1, y);
            canvas_draw_dot(canvas, x, y + 1);
        }
        
        // Second stroke: top-right to bottom-left  
        if(x_progress > stroke_len) {
            int stroke2_progress = x_progress - stroke_len;
            int stroke2_len = (stroke2_progress > stroke_len) ? stroke_len : stroke2_progress;
            for(int i = 0; i < stroke2_len; i++) {
                int x = cx + size - i;
                int y = cy - size + i;
                canvas_draw_dot(canvas, x, y);
                canvas_draw_dot(canvas, x - 1, y);
                canvas_draw_dot(canvas, x, y + 1);
            }
        }
        
        // Static noise effect around the edges
        for(int i = 0; i < 30; i++) {
            int x = ((frame * 7 + i * 17) * 31) % 128;
            int y = ((frame * 13 + i * 23) * 17) % 64;
            canvas_draw_dot(canvas, x, y);
        }
        
        canvas_set_font(canvas, FontSecondary);
        // Show error info if we have it
        if(furi_string_size(ctx->error_info) > 0) {
            canvas_draw_str_aligned(canvas, 64, 54, AlignCenter, AlignTop, 
                furi_string_get_cstr(ctx->error_info));
        } else {
            canvas_draw_str_aligned(canvas, 64, 54, AlignCenter, AlignTop, "Unknown protocol");
        }
        return;
    }
    
    // Normal decoding animation
    
    // Title with occasional glitch
    canvas_set_font(canvas, FontPrimary);
    int glitch = (frame % 47 == 0) ? 1 : 0;
    canvas_draw_str_aligned(canvas, 64 + glitch, 0, AlignCenter, AlignTop, "DECODING");
    
    // Waveform visualization - original style with sinf
    int wave_y = 22;
    int wave_height = 14;
    
    for(int x = 0; x < 128; x++) {
        float phase = (float)(x + frame * 4) * 0.12f;
        float phase2 = (float)(x - frame * 2) * 0.08f;
        int y_offset = (int)(sinf(phase) * wave_height / 2 + sinf(phase2) * wave_height / 4);
        
        // Add some noise variation
        if((x * 7 + frame) % 13 == 0) {
            y_offset += ((frame * x) % 5) - 2;
        }
        
        canvas_draw_dot(canvas, x, wave_y + y_offset);
        
        // Thicker line
        if((x + frame) % 3 != 0) {
            canvas_draw_dot(canvas, x, wave_y + y_offset + 1);
        }
    }
    
    // Scanning beam effect
    int scan_x = (frame * 5) % 148 - 10;
    for(int dx = 0; dx < 8; dx++) {
        int sx = scan_x + dx;
        if(sx >= 0 && sx < 128) {
            int intensity = 8 - dx;
            for(int y = wave_y - wave_height/2 - 1; y <= wave_y + wave_height/2 + 1; y++) {
                if(dx < intensity / 2) {
                    canvas_draw_dot(canvas, sx, y);
                }
            }
        }
    }
    
    // Progress bar frame
    int progress_y = 38;
    canvas_draw_rframe(canvas, 8, progress_y, 112, 10, 2);
    
    // Calculate progress
    int progress = 0;
    if(ctx->state == DecodeStateLoadRawSamples && ctx->total_samples > 0) {
        progress = 10 + (ctx->total_samples * 20) / MAX_RAW_SAMPLES;
    } else if(ctx->state == DecodeStateDecodingRaw && ctx->total_samples > 0) {
        int sample_pct = (ctx->current_sample * 100) / ctx->total_samples;
        int proto_pct = (ctx->current_protocol_idx * 100) / protopirate_protocol_registry.size;
        progress = 30 + (sample_pct * 35 + proto_pct * 35) / 100;
    } else if(ctx->state == DecodeStateOpenFile || ctx->state == DecodeStateReadHeader) {
        progress = 5 + (frame % 10);
    } else if(ctx->state == DecodeStateDecodingProtocol) {
        progress = 50 + (frame % 30);
    }
    if(progress > 100) progress = 100;
    
    // Animated progress fill with diagonal stripes
    int fill_width = (progress * 108) / 100;
    for(int x = 0; x < fill_width; x++) {
        for(int y = 0; y < 6; y++) {
            if(((x - (int)frame + y) & 3) < 2) {
                canvas_draw_dot(canvas, 10 + x, progress_y + 2 + y);
            }
        }
    }
    
    // Status text
    canvas_set_font(canvas, FontSecondary);
    const char* status_text = "Starting...";
    
    switch(ctx->state) {
        case DecodeStateOpenFile:
            status_text = "Opening file...";
            break;
        case DecodeStateReadHeader:
            status_text = "Reading header...";
            break;
        case DecodeStateLoadRawSamples:
            status_text = "Loading samples...";
            break;
        case DecodeStateDecodingRaw:
            status_text = ctx->current_protocol ? ctx->current_protocol->name : "Analyzing...";
            break;
        case DecodeStateDecodingProtocol:
            status_text = "Parsing protocol...";
            break;
        default:
            break;
    }
    canvas_draw_str_aligned(canvas, 64, 52, AlignCenter, AlignTop, status_text);
    
    // Binary rain effect on sides
    canvas_set_font(canvas, FontKeyboard);
    for(int i = 0; i < 5; i++) {
        int y_left = ((frame * 2 + i * 13) % 70) - 5;
        int y_right = ((frame * 2 + i * 17 + 35) % 70) - 5;
        char bit_l = '0' + ((frame + i) & 1);
        char bit_r = '0' + ((frame + i + 1) & 1);
        char str_l[2] = {bit_l, 0};
        char str_r[2] = {bit_r, 0};
        
        if(y_left >= 0 && y_left < 64) canvas_draw_str(canvas, 1, y_left, str_l);
        if(y_right >= 0 && y_right < 64) canvas_draw_str(canvas, 123, y_right, str_r);
    }
    
    // Corner spinners (slower)
    const char* spin = "|/-\\";
    char spinner[2] = {spin[(frame / 4) & 3], 0};
    canvas_draw_str(canvas, 1, 62, spinner);
    canvas_draw_str(canvas, 123, 62, spinner);
}

static bool protopirate_decode_input_callback(InputEvent* event, void* context) {
    UNUSED(context);
    
    if(event->type == InputTypeShort && event->key == InputKeyBack) {
        if(g_decode_ctx && g_decode_ctx->state != DecodeStateIdle && 
           g_decode_ctx->state != DecodeStateDone) {
            furi_string_set(g_decode_ctx->error_info, "Cancelled");
            g_decode_ctx->state = DecodeStateShowFailure;
            g_decode_ctx->result_display_counter = 0;
            furi_string_set(g_decode_ctx->result, "Cancelled by user");
        }
        return true;
    }
    
    return false;
}

// Process one chunk of RAW samples
static bool protopirate_process_raw_chunk(ProtoPirateApp* app, SubDecodeContext* ctx) {
    if(!ctx->current_decoder) {
        while(ctx->current_protocol_idx < protopirate_protocol_registry.size) {
            const SubGhzProtocol* protocol = protopirate_protocol_registry.items[ctx->current_protocol_idx];
            
            if(protocol->decoder && protocol->decoder->alloc) {
                ctx->current_decoder = protocol->decoder->alloc(app->txrx->environment);
                ctx->current_protocol = protocol;
                ctx->current_sample = 0;
                ctx->callback_fired = false;
                furi_string_reset(ctx->decoded_string);
                
                if(ctx->current_decoder) {
                    SubGhzProtocolDecoderBase* decoder_base = ctx->current_decoder;
                    decoder_base->callback = protopirate_decode_callback;
                    decoder_base->context = ctx;
                    
                    if(protocol->decoder->reset) {
                        protocol->decoder->reset(ctx->current_decoder);
                    }
                    
                    FURI_LOG_D(TAG, "Trying protocol: %s", protocol->name);
                    break;
                }
            }
            ctx->current_protocol_idx++;
        }
        
        if(!ctx->current_decoder) {
            return true;
        }
    }
    
    size_t end_sample = ctx->current_sample + SAMPLES_PER_TICK;
    if(end_sample > ctx->total_samples) {
        end_sample = ctx->total_samples;
    }
    
    for(size_t i = ctx->current_sample; i < end_sample && !ctx->callback_fired; i++) {
        int32_t duration = ctx->raw_samples[i];
        bool level = (duration >= 0);
        if(duration < 0) duration = -duration;
        
        ctx->current_protocol->decoder->feed(ctx->current_decoder, level, (uint32_t)duration);
    }
    
    ctx->current_sample = end_sample;
    
    if(ctx->callback_fired && furi_string_size(ctx->decoded_string) > 0) {
        furi_string_printf(ctx->result, "RAW Decoded!\nFreq: %lu.%02lu MHz\n\n%s",
            ctx->frequency / 1000000,
            (ctx->frequency % 1000000) / 10000,
            furi_string_get_cstr(ctx->decoded_string));
        ctx->decode_success = true;
        
        ctx->current_protocol->decoder->free(ctx->current_decoder);
        ctx->current_decoder = NULL;
        return true;
    }
    
    if(ctx->current_sample >= ctx->total_samples) {
        if(ctx->current_decoder) {
            ctx->current_protocol->decoder->free(ctx->current_decoder);
            ctx->current_decoder = NULL;
        }
        ctx->current_protocol_idx++;
        ctx->current_sample = 0;
        
        if(ctx->current_protocol_idx >= protopirate_protocol_registry.size) {
            return true;
        }
    }
    
    return false;
}

static void close_file_handles(SubDecodeContext* ctx) {
    if(ctx->ff) {
        flipper_format_free(ctx->ff);
        ctx->ff = NULL;
    }
    if(ctx->storage) {
        furi_record_close(RECORD_STORAGE);
        ctx->storage = NULL;
    }
}

void protopirate_scene_sub_decode_on_enter(void* context) {
    ProtoPirateApp* app = context;
    
    g_decode_ctx = malloc(sizeof(SubDecodeContext));
    memset(g_decode_ctx, 0, sizeof(SubDecodeContext));
    g_decode_ctx->file_path = furi_string_alloc();
    g_decode_ctx->protocol_name = furi_string_alloc();
    g_decode_ctx->result = furi_string_alloc();
    g_decode_ctx->error_info = furi_string_alloc();
    g_decode_ctx->decoded_string = furi_string_alloc();
    g_decode_ctx->state = DecodeStateIdle;
    
    DialogsFileBrowserOptions browser_options;
    dialog_file_browser_set_basic_options(&browser_options, ".sub", NULL);
    browser_options.base_path = SUBGHZ_APP_FOLDER;
    browser_options.hide_ext = false;
    
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    furi_string_set(g_decode_ctx->file_path, SUBGHZ_APP_FOLDER);
    
    if(dialog_file_browser_show(dialogs, g_decode_ctx->file_path, g_decode_ctx->file_path, &browser_options)) {
        FURI_LOG_I(TAG, "Selected file: %s", furi_string_get_cstr(g_decode_ctx->file_path));
        g_decode_ctx->state = DecodeStateOpenFile;
        
        view_set_draw_callback(app->view_about, protopirate_decode_draw_callback);
        view_set_input_callback(app->view_about, protopirate_decode_input_callback);
        view_set_context(app->view_about, app);
        
        view_dispatcher_switch_to_view(app->view_dispatcher, ProtoPirateViewAbout);
    } else {
        scene_manager_previous_scene(app->scene_manager);
    }
    
    furi_record_close(RECORD_DIALOGS);
}

bool protopirate_scene_sub_decode_on_event(void* context, SceneManagerEvent event) {
    ProtoPirateApp* app = context;
    bool consumed = false;
    SubDecodeContext* ctx = g_decode_ctx;
    
    if(!ctx) return false;
    
    if(event.type == SceneManagerEventTypeTick) {
        consumed = true;
        ctx->animation_frame++;
        
        switch(ctx->state) {
        case DecodeStateOpenFile: {
            ctx->storage = furi_record_open(RECORD_STORAGE);
            ctx->ff = flipper_format_file_alloc(ctx->storage);
            
            if(!flipper_format_file_open_existing(ctx->ff, furi_string_get_cstr(ctx->file_path))) {
                furi_string_set(ctx->result, "Failed to open file");
                furi_string_set(ctx->error_info, "File open failed");
                close_file_handles(ctx);
                ctx->state = DecodeStateShowFailure;
                ctx->result_display_counter = 0;
                notification_message(app->notifications, &sequence_error);
            } else {
                ctx->state = DecodeStateReadHeader;
            }
            break;
        }
        
        case DecodeStateReadHeader: {
            FuriString* temp_str = furi_string_alloc();
            uint32_t version = 0;
            bool success = false;
            
            do {
                if(!flipper_format_read_header(ctx->ff, temp_str, &version)) {
                    furi_string_set(ctx->result, "Invalid file format");
                    furi_string_set(ctx->error_info, "Invalid header");
                    break;
                }
                
                if(furi_string_cmp_str(temp_str, "Flipper SubGhz Key File") != 0 &&
                   furi_string_cmp_str(temp_str, "Flipper SubGhz RAW File") != 0 &&
                   furi_string_cmp_str(temp_str, "Flipper SubGhz") != 0) {
                    furi_string_set(ctx->result, "Not a SubGhz file");
                    furi_string_set(ctx->error_info, "Not SubGhz file");
                    break;
                }
                
                if(!flipper_format_read_string(ctx->ff, "Protocol", ctx->protocol_name)) {
                    furi_string_set(ctx->result, "Missing Protocol");
                    furi_string_set(ctx->error_info, "No protocol field");
                    break;
                }
                
                flipper_format_rewind(ctx->ff);
                flipper_format_read_header(ctx->ff, temp_str, &version);
                ctx->frequency = 433920000;
                flipper_format_read_uint32(ctx->ff, "Frequency", &ctx->frequency, 1);
                
                FURI_LOG_I(TAG, "Protocol: %s, Freq: %lu", 
                    furi_string_get_cstr(ctx->protocol_name), ctx->frequency);
                
                success = true;
            } while(false);
            
            furi_string_free(temp_str);
            
            if(!success) {
                close_file_handles(ctx);
                ctx->state = DecodeStateShowFailure;
                ctx->result_display_counter = 0;
                notification_message(app->notifications, &sequence_error);
            } else if(furi_string_cmp_str(ctx->protocol_name, "RAW") == 0) {
                ctx->raw_samples = malloc(sizeof(int32_t) * MAX_RAW_SAMPLES);
                if(!ctx->raw_samples) {
                    furi_string_set(ctx->result, "Memory error");
                    furi_string_set(ctx->error_info, "Out of memory");
                    close_file_handles(ctx);
                    ctx->state = DecodeStateShowFailure;
                    ctx->result_display_counter = 0;
                    notification_message(app->notifications, &sequence_error);
                } else {
                    ctx->total_samples = 0;
                    flipper_format_rewind(ctx->ff);
                    ctx->state = DecodeStateLoadRawSamples;
                }
            } else {
                ctx->state = DecodeStateDecodingProtocol;
            }
            break;
        }
        
        case DecodeStateLoadRawSamples: {
            size_t samples_this_tick = 0;
            
            while(ctx->total_samples < MAX_RAW_SAMPLES && samples_this_tick < 512) {
                uint32_t count = 0;
                if(!flipper_format_get_value_count(ctx->ff, "RAW_Data", &count) || count == 0) {
                    break;
                }
                
                size_t to_read = count;
                if(ctx->total_samples + to_read > MAX_RAW_SAMPLES) {
                    to_read = MAX_RAW_SAMPLES - ctx->total_samples;
                }
                
                if(!flipper_format_read_int32(ctx->ff, "RAW_Data", &ctx->raw_samples[ctx->total_samples], to_read)) {
                    break;
                }
                
                ctx->total_samples += to_read;
                samples_this_tick += to_read;
            }
            
            uint32_t count = 0;
            bool more_data = flipper_format_get_value_count(ctx->ff, "RAW_Data", &count) && count > 0;
            
            if(!more_data || ctx->total_samples >= MAX_RAW_SAMPLES) {
                close_file_handles(ctx);
                
                FURI_LOG_I(TAG, "Loaded %zu RAW samples", ctx->total_samples);
                
                if(ctx->total_samples < 10) {
                    furi_string_set(ctx->result, "Not enough samples");
                    furi_string_set(ctx->error_info, "Too few samples");
                    ctx->state = DecodeStateShowFailure;
                    ctx->result_display_counter = 0;
                    notification_message(app->notifications, &sequence_error);
                } else {
                    ctx->current_protocol_idx = 0;
                    ctx->current_sample = 0;
                    ctx->state = DecodeStateDecodingRaw;
                }
            }
            break;
        }
        
        case DecodeStateDecodingRaw: {
            bool done = protopirate_process_raw_chunk(app, ctx);
            
            if(done) {
                if(ctx->decode_success) {
                    ctx->state = DecodeStateShowSuccess;
                    ctx->result_display_counter = 0;
                    notification_message(app->notifications, &sequence_success);
                } else {
                    furi_string_printf(ctx->result,
                        "RAW Signal\n\n"
                        "Freq: %lu.%02lu MHz\n"
                        "Samples: %zu\n\n"
                        "No ProtoPirate protocol\n"
                        "detected in signal.",
                        ctx->frequency / 1000000,
                        (ctx->frequency % 1000000) / 10000,
                        ctx->total_samples);
                    furi_string_set(ctx->error_info, "No protocol match");
                    ctx->state = DecodeStateShowFailure;
                    ctx->result_display_counter = 0;
                    notification_message(app->notifications, &sequence_error);
                }
            }
            break;
        }
        
        case DecodeStateDecodingProtocol: {
            const char* proto_name = furi_string_get_cstr(ctx->protocol_name);
            bool decoded = false;
            SubGhzProtocolStatus last_status = SubGhzProtocolStatusOk;
            
            // Find matching protocol
            const SubGhzProtocol* custom_protocol = NULL;
            for(size_t i = 0; i < protopirate_protocol_registry.size; i++) {
                if(protocol_names_match(proto_name, protopirate_protocol_registry.items[i]->name)) {
                    custom_protocol = protopirate_protocol_registry.items[i];
                    FURI_LOG_I(TAG, "Matched to: %s", custom_protocol->name);
                    break;
                }
            }
            
            if(custom_protocol && custom_protocol->decoder && custom_protocol->decoder->alloc) {
                void* decoder = custom_protocol->decoder->alloc(app->txrx->environment);
                if(decoder) {
                    flipper_format_rewind(ctx->ff);
                    last_status = custom_protocol->decoder->deserialize(decoder, ctx->ff);
                    
                    if(last_status == SubGhzProtocolStatusOk) {
                        FuriString* dec_str = furi_string_alloc();
                        custom_protocol->decoder->get_string(decoder, dec_str);
                        
                        const char* fname = furi_string_get_cstr(ctx->file_path);
                        const char* short_name = strrchr(fname, '/');
                        if(short_name) short_name++; else short_name = fname;
                        
                        furi_string_printf(ctx->result, "File: %s\n\n%s", 
                            short_name, furi_string_get_cstr(dec_str));
                        furi_string_free(dec_str);
                        decoded = true;
                        ctx->decode_success = true;
                    } else {
                        FURI_LOG_W(TAG, "Custom decoder failed: %d", last_status);
                    }
                    custom_protocol->decoder->free(decoder);
                }
            }
            
            if(!decoded) {
                SubGhzProtocolDecoderBase* decoder = subghz_receiver_search_decoder_base_by_name(
                    app->txrx->receiver, proto_name);
                
                if(decoder) {
                    flipper_format_rewind(ctx->ff);
                    last_status = subghz_protocol_decoder_base_deserialize(decoder, ctx->ff);
                    
                    if(last_status == SubGhzProtocolStatusOk) {
                        FuriString* dec_str = furi_string_alloc();
                        subghz_protocol_decoder_base_get_string(decoder, dec_str);
                        
                        const char* fname = furi_string_get_cstr(ctx->file_path);
                        const char* short_name = strrchr(fname, '/');
                        if(short_name) short_name++; else short_name = fname;
                        
                        furi_string_printf(ctx->result, "File: %s\n\n%s", 
                            short_name, furi_string_get_cstr(dec_str));
                        furi_string_free(dec_str);
                        decoded = true;
                        ctx->decode_success = true;
                    } else {
                        FURI_LOG_W(TAG, "App receiver failed: %d", last_status);
                    }
                }
            }
            
            if(!decoded) {
                const char* fname = furi_string_get_cstr(ctx->file_path);
                const char* short_name = strrchr(fname, '/');
                if(short_name) short_name++; else short_name = fname;
                
                // Set error info based on status
                furi_string_set(ctx->error_info, get_protocol_status_string(last_status));
                
                furi_string_printf(ctx->result, "File: %s\nProtocol: %s\n\nError: %s\n\n", 
                    short_name, proto_name, get_protocol_status_string(last_status));
                
                // Read available fields to show what we can
                FuriString* temp = furi_string_alloc();
                uint32_t version, val;
                
                flipper_format_rewind(ctx->ff);
                flipper_format_read_header(ctx->ff, temp, &version);
                if(flipper_format_read_uint32(ctx->ff, "Bit", &val, 1)) {
                    furi_string_cat_printf(ctx->result, "Bits: %lu\n", val);
                }
                
                flipper_format_rewind(ctx->ff);
                flipper_format_read_header(ctx->ff, temp, &version);
                if(flipper_format_read_string(ctx->ff, "Key", temp)) {
                    furi_string_cat_printf(ctx->result, "Key: %s\n", furi_string_get_cstr(temp));
                }
                
                furi_string_cat_printf(ctx->result, "Freq: %lu.%02lu MHz\n",
                    ctx->frequency / 1000000, (ctx->frequency % 1000000) / 10000);
                
                furi_string_free(temp);
            }
            
            close_file_handles(ctx);
            
            if(ctx->decode_success) {
                ctx->state = DecodeStateShowSuccess;
                ctx->result_display_counter = 0;
                notification_message(app->notifications, &sequence_success);
            } else {
                ctx->state = DecodeStateShowFailure;
                ctx->result_display_counter = 0;
                notification_message(app->notifications, &sequence_error);
            }
            break;
        }
        
        case DecodeStateShowSuccess: {
            ctx->result_display_counter++;
            if(ctx->result_display_counter >= SUCCESS_DISPLAY_TICKS) {
                widget_reset(app->widget);
                widget_add_text_scroll_element(app->widget, 0, 0, 128, 64, furi_string_get_cstr(ctx->result));
                view_dispatcher_switch_to_view(app->view_dispatcher, ProtoPirateViewWidget);
                ctx->state = DecodeStateDone;
            }
            break;
        }
        
        case DecodeStateShowFailure: {
            ctx->result_display_counter++;
            if(ctx->result_display_counter >= FAILURE_DISPLAY_TICKS) {
                widget_reset(app->widget);
                widget_add_text_scroll_element(app->widget, 0, 0, 128, 64, furi_string_get_cstr(ctx->result));
                view_dispatcher_switch_to_view(app->view_dispatcher, ProtoPirateViewWidget);
                ctx->state = DecodeStateDone;
            }
            break;
        }
        
        default:
            break;
        }
        
        view_commit_model(app->view_about, false);
    }
    
    return consumed;
}

void protopirate_scene_sub_decode_on_exit(void* context) {
    ProtoPirateApp* app = context;
    
    if(g_decode_ctx) {
        close_file_handles(g_decode_ctx);
        
        if(g_decode_ctx->current_decoder && g_decode_ctx->current_protocol) {
            g_decode_ctx->current_protocol->decoder->free(g_decode_ctx->current_decoder);
        }
        if(g_decode_ctx->raw_samples) {
            free(g_decode_ctx->raw_samples);
        }
        furi_string_free(g_decode_ctx->file_path);
        furi_string_free(g_decode_ctx->protocol_name);
        furi_string_free(g_decode_ctx->result);
        furi_string_free(g_decode_ctx->error_info);
        furi_string_free(g_decode_ctx->decoded_string);
        free(g_decode_ctx);
        g_decode_ctx = NULL;
    }
    
    view_set_draw_callback(app->view_about, NULL);
    view_set_input_callback(app->view_about, NULL);
    widget_reset(app->widget);
}