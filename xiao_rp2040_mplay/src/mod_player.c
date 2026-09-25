// MOD player core - parsing, ProTracker effects, and mixing.
//
// Ported from ~/devel/mplay/mod_player.c (same author, MIT-licensed) with
// two changes from the original desktop version:
//
//  1. No file I/O / SDL: mod_player_init() takes a pointer to a MOD file
//     already in memory (in this project's case, flash-resident, embedded
//     via mod_data.h) instead of opening a file, and there is no audio
//     backend here at all - mod_player_produce() just fills a caller-owned
//     buffer with samples; the caller (pico_dma.c) is responsible for
//     getting them to the DAC.
//
//  2. The original mutated ModSample's big-endian fields (length_words,
//     repeat_offset, repeat_length) and sign-extended fine_tune IN PLACE
//     over the loaded file buffer, since that buffer was malloc'd and
//     therefore writable. Here the MOD data may be `const` flash (XIP,
//     not writable), so those corrected values are precomputed once into
//     parallel arrays in PlayerState instead - see mod_player_init().
//     Every other data structure, the whole effects engine (process_row/
//     process_tick), and the mixer (render_tick) are otherwise unchanged.
//
//  3. The desktop version's main() drove playback with a fixed structure:
//     for each row, compute this row's tick count and samples-per-tick,
//     then render exactly one tick's worth of samples per iteration and
//     push it to SDL. Embedded playback instead needs to fill fixed-size
//     DMA buffers that don't line up with tick boundaries. render_tick()
//     itself needed no change for this (it already just renders N samples
//     from whatever the current channel state is) - only the row/tick
//     bookkeeping around it was turned into a resumable state machine
//     (advance_tick() et al.) driven by mod_player_produce() pulling
//     however many samples it needs, spanning as many tick boundaries as
//     that requires.

#include "mod_player.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(struct ModHeader) == 1084, "ModHeader must be 1084 bytes");

#define OUTPUT_RATE      48000   // must match pico_mplay.c's SAMPLE_RATE_HZ
#define AMIGA_PAL_CLOCK  7093789.2f
#define NUM_CHANNELS     MOD_NUM_CHANNELS
#define ROWS_PER_PATTERN MOD_ROWS_PER_PATTERN
#define DEFAULT_SPEED    6
#define DEFAULT_TEMPO    125
#define MIN_PERIOD       56
#define MAX_PERIOD       1712
#define MIX_SCALE        64    // 8->16 bit output scale; lower = quieter overall (was 128)
#define FILTER_CUTOFF_HZ 4500.0f   // Amiga-style low-pass cutoff, matches mplay-rs's DEFAULT_FILTER_HZ

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Fixed-point formats used in the per-sample render path (see render_tick()).
// The RP2040 (Cortex-M0+) has no hardware FPU, so float math there is
// software-emulated and far too slow for 4-channel/48kHz real-time mixing -
// confirmed empirically (underruns climbed hard with a float version of this
// same interpolation+filter code). Everything below is integer/shift-based
// instead; only per-tick/per-note control-rate math (calc_increment(),
// filter_alpha_fp(), vibrato, portamento, ...) stays float, since that runs
// at most once per tick, not once per sample.
#define POS_FRAC_BITS    15                    // channel position/increment: Q17.15
#define POS_SCALE        (1u << POS_FRAC_BITS) // 17 integer bits covers the largest possible
                                                // sample length (65535 length_words * 2 = 131070)
#define FILTER_FRAC_BITS 8                     // filter state/input: Q(24).8
#define FILTER_SCALE     (1 << FILTER_FRAC_BITS)
#define ALPHA_FRAC_BITS  14                     // filter coefficient: Q0.14
#define ALPHA_SCALE      (1 << ALPHA_FRAC_BITS)

// Period table: 5 octaves (0-4), covering extended range used by many MODs
static const uint16_t table_periods[5 * 12] = {
    1712,1616,1524,1440,1356,1280,1208,1140,1076,1016, 960, 906, // oct 0
     856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453, // oct 1
     428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226, // oct 2
     214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113, // oct 3
     107, 101,  95,  90,  85,  80,  75,  71,  67,  63,  60,  56  // oct 4
};

// Arpeggio semitone ratios: period * ratio[n] = period shifted up by n semitones
// ratio[n] = pow(2, -n/12.0), precomputed to avoid per-tick floating point
static const float arpeggio_ratio[16] = {
    1.000000f, 0.943874f, 0.890899f, 0.840896f,  //  0- 3
    0.793701f, 0.749154f, 0.707107f, 0.667420f,  //  4- 7
    0.629961f, 0.594604f, 0.561231f, 0.529732f,  //  8-11
    0.500000f, 0.471937f, 0.445449f, 0.420448f   // 12-15
};

// ProTracker vibrato sine table (half period, 32 entries, 0-255)
static const uint8_t sine_table[32] = {
      0,  24,  49,  74,  97, 120, 141, 161,
    180, 197, 212, 224, 235, 244, 250, 253,
    255, 253, 250, 244, 235, 224, 212, 197,
    180, 161, 141, 120,  97,  74,  49,  24
};

// Waveform lookup for vibrato/tremolo: returns 0-255 amplitude
static int waveform_value(uint8_t waveform, uint8_t pos) {
    switch (waveform & 3) {
    case 0: return sine_table[pos & 31];               // sine
    case 1: {                                           // ramp down (sawtooth)
        int v = (int)(pos & 31) * 8;
        return (pos & 32) ? 255 - v : v;
    }
    case 2: return 255;                                 // square
    default: return sine_table[pos & 31];               // random -> fallback to sine
    }
}

// Called at most once per tick (vibrato/portamento/arpeggio) or once per
// note trigger, never per sample, so computing the ratio in float and only
// converting the final result to fixed point is fine performance-wise.
static uint32_t calc_increment(uint16_t period) {
    if (period == 0) return 0;
    float inc = AMIGA_PAL_CLOCK / ((float)period * 2.0f * OUTPUT_RATE);
    return (uint32_t)(inc * (float)POS_SCALE + 0.5f);
}

// One-pole IIR coefficient: alpha = 2*pi*fc / (2*pi*fc + fs) - same formula
// as mplay-rs's player.rs, so both ports emulate the Amiga's RC/LED output
// filter identically. Called once at mod_player_init(), so float here is fine.
static int32_t filter_alpha_fp(float cutoff_hz) {
    float w = 2.0f * (float)M_PI * cutoff_hz;
    float alpha = w / (w + (float)OUTPUT_RATE);
    return (int32_t)(alpha * (float)ALPHA_SCALE + 0.5f);
}

static uint16_t period_add_semitones(uint16_t period, int semitones) {
    if (semitones <= 0 || semitones > 15) return period;
    return (uint16_t)(period * arpeggio_ratio[semitones] + 0.5f);
}

static uint16_t apply_finetune(uint16_t period, int8_t finetune) {
    if (finetune == 0 || period == 0) return period;
    // finetune is -8..+7, each step is 1/8 semitone = 1/96 octave
    return (uint16_t)(period * powf(2.0f, -(float)finetune / 96.0f) + 0.5f);
}

static uint16_t swap_uint16(uint16_t value) {
    return ((value & 0xFF00) >> 8) | ((value & 0x00FF) << 8);
}

static struct ModNote mod_note_decode(const uint8_t *ptr) {
    struct ModNote note = {
        .instrument = (ptr[0] & 0xf0) | ((ptr[2] & 0xf0) >> 4),
        .period = (((ptr[0] & 0x0f) << 8) | ptr[1]),
        .effect = ptr[2] & 0x0f,
        .effect_arg = ptr[3]
    };
    return note;
}

static int header_get_max_pattern(const struct ModHeader *hdr) {
    // Scan all 128 entries, not just pattern_count - some MODs store
    // pattern references beyond song length
    int result = -1;
    for (int i = 0; i < 128; ++i) {
        if (hdr->patterns[i] > result)
            result = hdr->patterns[i];
    }
    return result;
}

// Detect 15-instrument Soundtracker format (no magic at offset 1080)
static int detect_15instrument(const uint8_t *data, size_t size) {
    if (size < 600) return 0;
    uint8_t song_len = data[470];
    if (song_len == 0 || song_len > 128) return 0;

    int max_pat = 0;
    for (int i = 0; i < song_len; ++i)
        if (data[472 + i] > max_pat) max_pat = data[472 + i];
    if (max_pat >= 64) return 0;

    uint32_t total_sample = 0;
    for (int i = 0; i < 15; ++i) {
        int off = 20 + i * 30;
        total_sample += ((data[off + 22] << 8) | data[off + 23]) * 2;
        if (data[off + 25] > 64) return 0;  /* volume must be <= 64 */
    }

    size_t sample_base = 600 + (max_pat + 1) * 1024;
    return sample_base + total_sample <= size;
}

// Convert 15-instrument file to 31-instrument layout in freshly allocated
// memory (only reads `src`, so it's fine even when src is flash/const).
static void *convert_15to31(const uint8_t *src, size_t src_size, size_t *out_size) {
    size_t dst_size = src_size + 484; // 1084 - 600 = 484 extra header bytes
    uint8_t *dst = calloc(1, dst_size);
    if (!dst) return NULL;

    memcpy(dst, src, 20);                      // title
    memcpy(dst + 20, src + 20, 15 * 30);       // 15 samples -> slots 1-15
    // slots 16-31 are zeroed by calloc
    dst[950] = src[470];                        // song length
    dst[951] = src[471];                        // tempo/restart byte
    memcpy(dst + 952, src + 472, 128);          // pattern table
    memcpy(dst + 1080, "M.K.", 4);              // synthetic magic
    memcpy(dst + 1084, src + 600, src_size - 600); // patterns + samples

    *out_size = dst_size;
    return dst;
}

// Validates format and returns a pointer to 31-instrument-layout MOD data
// (either `raw` itself, or a freshly converted buffer for ST15 files).
// No file I/O - `raw` must already be the whole file in memory.
static const uint8_t *mod_prepare(const uint8_t *raw, size_t size, size_t *out_size) {
    if (size < 600) return NULL;

    if (size >= sizeof(struct ModHeader) &&
        (memcmp(raw + 1080, "M.K.", 4) == 0 || memcmp(raw + 1080, "M!K!", 4) == 0)) {
        *out_size = size;
        return raw;
    }
    if (detect_15instrument(raw, size)) {
        return convert_15to31(raw, size, out_size);
    }
    return NULL;
}

int mod_player_init(struct PlayerState *ps, const uint8_t *mod_data, size_t mod_size) {
    memset(ps, 0, sizeof(*ps));

    size_t size;
    const uint8_t *data = mod_prepare(mod_data, mod_size, &size);
    if (!data) return -1;

    const struct ModHeader *hdr = (const struct ModHeader *)data;
    if (memcmp(hdr->magic, "M.K.", 4) != 0 && memcmp(hdr->magic, "M!K!", 4) != 0)
        return -1;
    if (hdr->pattern_count == 0)
        return -1;   // enter_row()/restart_song() would recurse forever otherwise

    ps->mod_data = data;
    ps->mod_size = size;
    ps->header = hdr;
    ps->filter_alpha = filter_alpha_fp(FILTER_CUTOFF_HZ);
    ps->speed = DEFAULT_SPEED;
    ps->tempo = DEFAULT_TEMPO;
    ps->break_row = -1;
    ps->break_position = -1;

    int num_patterns = header_get_max_pattern(hdr) + 1;
    size_t patterns_size = (size_t)num_patterns * ROWS_PER_PATTERN * NUM_CHANNELS * 4;
    if (sizeof(struct ModHeader) + patterns_size > size)
        return -1;
    ps->pattern_data = (const uint8_t *)data + sizeof(struct ModHeader);
    ps->sample_base = (const int8_t *)data + sizeof(struct ModHeader) + num_patterns * ROWS_PER_PATTERN * NUM_CHANNELS * 4;

    // Precompute corrected (byte-swapped/sign-extended) per-sample metadata
    // once here, since the source struct fields can't be mutated in place
    // when `data` is flash-resident - see the file header comment.
    const int8_t *offset = ps->sample_base;
    const int8_t *file_end = (const int8_t *)data + size;
    for (int i = 0; i < 31; ++i) {
        const struct ModSample *s = &hdr->samples[i];
        uint32_t len = swap_uint16(s->length_words) * 2u;
        uint32_t loop_start = swap_uint16(s->repeat_offset) * 2u;
        uint32_t loop_length = swap_uint16(s->repeat_length) * 2u;

        ps->sample_offsets[i + 1] = offset;
        uint32_t avail = (offset < file_end) ? (uint32_t)(file_end - offset) : 0;
        uint32_t clamped_len = (len < avail) ? len : avail;
        ps->sample_lengths[i + 1] = clamped_len;

        if (loop_start + loop_length > clamped_len)
            loop_length = (clamped_len > loop_start) ? clamped_len - loop_start : 0;
        ps->sample_loop_start[i + 1] = loop_start;
        ps->sample_loop_length[i + 1] = loop_length;
        ps->sample_finetune[i + 1] = ((s->fine_tune & 0x0F) ^ 8) - 8; // sign-extend 4-bit two's complement

        offset += len;
    }
    return 0;
}

// Shared effect implementations (each used by several effect commands)

// Axy and the slide half of 5xy/6xy: x slides up, else y slides down
static void volume_slide(struct Channel *c, uint8_t arg) {
    int v = c->volume;
    if (arg >> 4)
        v += arg >> 4;
    else
        v -= arg & 0x0f;
    if (v < 0) v = 0;
    if (v > 64) v = 64;
    c->volume = (uint8_t)v;
}

// 1xx/2xx and E1y/E2y: slide period by delta, clamped to Amiga limits
static void period_slide(struct Channel *c, int delta) {
    int p = (int)c->period + delta;
    if (p < MIN_PERIOD) p = MIN_PERIOD;
    if (p > MAX_PERIOD) p = MAX_PERIOD;
    c->period = (uint16_t)p;
    c->increment = calc_increment(c->period);
}

// 3xx and the portamento half of 5xy
static void tone_portamento(struct Channel *c) {
    if (c->target_period == 0) return;
    if (c->period < c->target_period) {
        int p = (int)c->period + c->portamento_speed;
        c->period = (p > c->target_period) ? c->target_period : (uint16_t)p;
    } else if (c->period > c->target_period) {
        int p = (int)c->period - c->portamento_speed;
        c->period = (p < c->target_period) ? c->target_period : (uint16_t)p;
    }
    c->increment = calc_increment(c->period);
}

// 4xy and the vibrato half of 6xy: modulates increment only, period unchanged
static void vibrato(struct Channel *c) {
    int delta = waveform_value(c->vibrato_waveform, c->vibrato_pos) * c->vibrato_depth / 128;
    if (c->vibrato_pos & 32) delta = -delta;
    c->increment = calc_increment((uint16_t)((int)c->period + delta));
    c->vibrato_pos += c->vibrato_speed;
}

static void trigger_note(struct PlayerState *ps, int ch, const struct ModNote *n) {
    struct Channel *c = &ps->channels[ch];
    uint8_t fx = n->effect;

    // Instrument set: load sample parameters
    if (n->instrument > 0 && n->instrument <= 31) {
        const struct ModSample *s = &ps->header->samples[n->instrument - 1];
        c->instrument = n->instrument;
        c->sample_data = ps->sample_offsets[n->instrument];
        c->sample_length = ps->sample_lengths[n->instrument];
        c->loop_start = ps->sample_loop_start[n->instrument];
        c->loop_length = ps->sample_loop_length[n->instrument];
        c->volume = s->volume;   // single byte, no byte-order issue
        if (c->volume > 64) c->volume = 64;
        c->finetune = ps->sample_finetune[n->instrument];
    }

    // Period set: handle note triggering
    if (n->period != 0) {
        if (fx == 0x03 || fx == 0x05) {
            c->target_period = apply_finetune(n->period, c->finetune);
        } else {
            c->period = apply_finetune(n->period, c->finetune);
            c->position = 0;
            c->increment = calc_increment(c->period);
            if (!(c->vibrato_waveform & 4)) c->vibrato_pos = 0;
            if (!(c->tremolo_waveform & 4)) c->tremolo_pos = 0;
        }
    }
}

static void process_row(struct PlayerState *ps, const struct ModNote notes[MOD_NUM_CHANNELS]) {
    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        struct Channel *c = &ps->channels[ch];
        const struct ModNote *n = &notes[ch];
        uint8_t fx = n->effect;
        uint8_t arg = n->effect_arg;
        uint8_t ext_cmd = (arg >> 4) & 0x0f;
        uint8_t ext_arg = arg & 0x0f;

        c->tremolo_delta = 0; // tick 0 restores base volume (as PT does)

        // EDy (y > 0): Note delay - defer triggering to tick y. ED0 triggers normally.
        if (fx == 0x0E && ext_cmd == 0x0D && ext_arg > 0) {
            // Don't trigger now; process_tick handles it
        } else {
            trigger_note(ps, ch, n);
        }

        // Tick-0 effects
        switch (fx) {
        case 0x03: // 3xx: Tone portamento (memory)
            if (arg > 0) c->portamento_speed = arg;
            break;
        case 0x04: // 4xy: Vibrato (memory)
            if ((arg >> 4) > 0) c->vibrato_speed = arg >> 4;
            if ((arg & 0x0f) > 0) c->vibrato_depth = arg & 0x0f;
            break;
        case 0x07: // 7xy: Tremolo (memory)
            if ((arg >> 4) > 0) c->tremolo_speed = arg >> 4;
            if ((arg & 0x0f) > 0) c->tremolo_depth = arg & 0x0f;
            break;
        case 0x08: // 8xx: Set panning (mono output - no-op)
            break;
        case 0x09: // 9xx: Sample offset (900 reuses last nonzero argument)
            if (arg > 0)
                c->offset_memory = arg;
            if (c->offset_memory > 0 && c->sample_data) {
                uint32_t off = (uint32_t)c->offset_memory * 256;
                if (off < c->sample_length)
                    c->position = off << POS_FRAC_BITS;
            }
            break;
        case 0x0C: // Cxx: Set volume
            c->volume = (arg > 64) ? 64 : arg;
            break;
        case 0x0E: // Exy: Extended effects (tick 0)
            switch (ext_cmd) {
            case 0x00: // E0y: Set filter (Amiga hardware - no-op)
                break;
            case 0x01: // E1y: Fine portamento up
                period_slide(c, -ext_arg);
                break;
            case 0x02: // E2y: Fine portamento down
                period_slide(c, ext_arg);
                break;
            case 0x04: // E4y: Set vibrato waveform
                c->vibrato_waveform = ext_arg;
                break;
            case 0x05: // E5y: Set finetune override
                c->finetune = ext_arg > 7 ? ext_arg - 16 : ext_arg;
                break;
            case 0x06: // E6y: Pattern loop
                if (ext_arg == 0) {
                    ps->loop_row = ps->current_row;
                } else {
                    if (ps->loop_count == 0) {
                        ps->loop_count = ext_arg;
                        ps->break_row = ps->loop_row;
                        ps->break_position = ps->current_position;
                    } else {
                        ps->loop_count--;
                        if (ps->loop_count > 0) {
                            ps->break_row = ps->loop_row;
                            ps->break_position = ps->current_position;
                        }
                    }
                }
                break;
            case 0x0A: { // EAy: Fine volume slide up
                int v = c->volume + ext_arg;
                c->volume = (v > 64) ? 64 : (uint8_t)v;
                break;
            }
            case 0x07: // E7y: Set tremolo waveform
                c->tremolo_waveform = ext_arg;
                break;
            case 0x0B: { // EBy: Fine volume slide down
                int v = c->volume - ext_arg;
                c->volume = (v < 0) ? 0 : (uint8_t)v;
                break;
            }
            case 0x0C: // EC0: Note cut at tick 0 (y > 0 handled per-tick)
                if (ext_arg == 0)
                    c->volume = 0;
                break;
            case 0x0E: // EEy: Pattern delay
                if (ps->pattern_delay == 0)
                    ps->pattern_delay = ext_arg;
                break;
            default:
                break;
            }
            break;
        case 0x0F: // Fxx: Set speed/tempo
            if (arg == 0) break;
            if (arg < 32)
                ps->speed = arg;
            else
                ps->tempo = arg;
            break;
        case 0x0B: // Bxx: Position jump
            ps->break_position = arg;
            ps->break_row = 0;
            break;
        case 0x0D: // Dxx: Pattern break
            ps->break_row = (arg >> 4) * 10 + (arg & 0x0f);
            if (ps->break_row > ROWS_PER_PATTERN - 1) ps->break_row = 0;
            if (ps->break_position < 0)
                ps->break_position = ps->current_position + 1;
            break;
        default:
            break;
        }
    }
}

static void process_tick(struct PlayerState *ps, const struct ModNote notes[MOD_NUM_CHANNELS]) {
    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        struct Channel *c = &ps->channels[ch];
        const struct ModNote *n = &notes[ch];
        uint8_t fx = n->effect;
        uint8_t arg = n->effect_arg;

        switch (fx) {
        case 0x00: // 0xy: Arpeggio
            if (arg != 0 && c->period != 0) {
                int tick_phase = ps->current_tick % 3;
                uint16_t p = c->period;
                if (tick_phase == 1)
                    p = period_add_semitones(c->period, (arg >> 4) & 0x0f);
                else if (tick_phase == 2)
                    p = period_add_semitones(c->period, arg & 0x0f);
                c->increment = calc_increment(p);
            }
            break;
        case 0x01: // 1xx: Portamento up
            period_slide(c, -arg);
            break;
        case 0x02: // 2xx: Portamento down
            period_slide(c, arg);
            break;
        case 0x03: // 3xx: Tone portamento
            tone_portamento(c);
            break;
        case 0x04: // 4xy: Vibrato
            vibrato(c);
            break;
        case 0x05: // 5xy: Tone portamento + volume slide
            tone_portamento(c);
            volume_slide(c, arg);
            break;
        case 0x06: // 6xy: Vibrato + volume slide
            vibrato(c);
            volume_slide(c, arg);
            break;
        case 0x07: { // 7xy: Tremolo - modulates mixer volume, base volume unchanged
            int delta = waveform_value(c->tremolo_waveform, c->tremolo_pos) * c->tremolo_depth / 64;
            if (c->tremolo_pos & 32) delta = -delta;
            c->tremolo_delta = (int8_t)delta;
            c->tremolo_pos += c->tremolo_speed;
            break;
        }
        case 0x0A: // Axy: Volume slide
            volume_slide(c, arg);
            break;
        case 0x0E: { // Exy: Extended effects (tick 1+)
            uint8_t ext_cmd = (arg >> 4) & 0x0f;
            uint8_t ext_arg = arg & 0x0f;
            switch (ext_cmd) {
            case 0x09: // E9y: Retrigger note
                if (ext_arg > 0 && (ps->current_tick % ext_arg) == 0)
                    c->position = 0;
                break;
            case 0x0C: // ECy: Note cut
                if (ps->current_tick == ext_arg)
                    c->volume = 0;
                break;
            case 0x0D: // EDy: Note delay
                if (ps->current_tick == ext_arg)
                    trigger_note(ps, ch, n);
                break;
            default:
                break;
            }
            break;
        }
        default:
            break;
        }
    }
}

static void render_tick(struct PlayerState *ps, int16_t *buffer, int num_samples) {
    for (int i = 0; i < num_samples; ++i) {
        int32_t mix = 0;   // sum of 4 channels' Q(24).8 filter_state

        for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
            struct Channel *c = &ps->channels[ch];
            int32_t input = 0;   // Q(24).8, matches filter_state's format

            // Silent/inactive channels still feed 0 into the filter below
            // instead of skipping it outright - letting filter_state decay
            // smoothly avoids an audible click each time a channel starts or
            // stops (same reasoning as mplay-rs's mix_sample()).
            bool active = c->sample_data && c->period != 0;
            if (active && c->loop_length > 2) {
                // Computed fresh each sample rather than cached on the
                // channel: it's just a shift+add (cheap even without an
                // FPU), and only the multiply-heavy interpolation/filter
                // math below needed precomputing to stay real-time.
                uint32_t loop_end_fp = (c->loop_start + c->loop_length) << POS_FRAC_BITS;
                uint32_t loop_length_fp = c->loop_length << POS_FRAC_BITS;
                while (c->position >= loop_end_fp) {
                    c->position -= loop_length_fp;
                }
            } else if (active && (c->position >> POS_FRAC_BITS) >= c->sample_length) {
                active = false; // past the end of a non-looping sample
            }

            if (active) {
                uint32_t pos = c->position >> POS_FRAC_BITS;
                uint32_t frac = c->position & (POS_SCALE - 1);   // Q0.15, 0..32767

                int32_t s0 = c->sample_data[pos];
                int32_t s1;
                if (c->loop_length > 2 && pos + 1 >= c->loop_start + c->loop_length) {
                    // Last byte of the loop: the neighbour is the loop start,
                    // even when the loop ends before the sample data does -
                    // reading the byte past the loop end there put a
                    // periodic glitch on short chip loops
                    s1 = c->sample_data[c->loop_start];
                } else if (pos + 1 < c->sample_length) {
                    s1 = c->sample_data[pos + 1];
                } else {
                    s1 = s0; // last sample of a non-looping voice: hold, don't extrapolate
                }

                int vol = c->volume + c->tremolo_delta;
                if (vol < 0) vol = 0;
                if (vol > 64) vol = 64;

                // Linear interpolation between adjacent samples (plain
                // integers, still in the -128..127 sample range), volume
                // applied before the filter (matches real Paula behaviour:
                // volume is set in hardware, then the RC/LED filter smooths
                // the result - so volume changes don't step the output).
                //
                // vol's divide-by-64 and the shift into Q(24).8 are combined
                // into one left shift (64 = 2^6, so /64 then <<8 is exactly
                // <<2) rather than computed as separate operations: dividing
                // first would truncate interp*vol to a handful of levels at
                // low volume (e.g. vol=1 collapses interp's whole -128..127
                // range down to about -2..1) before the shift ever ran,
                // destroying real signal precisely during quiet fade-ins -
                // the same bug independently found in ~/devel/mplay's
                // render_tick() (mix += raw*vol/64, divided per-channel
                // before summing). Scaling up never discards bits, so
                // computing it this way is exactly the true interp*vol/64
                // value scaled into Q8, not an approximation of it. Written
                // as a multiply, not a shift: left-shifting a negative value
                // is undefined behaviour in C11 (same instruction on ARM).
                int32_t interp = s0 + (((int32_t)frac * (s1 - s0)) >> POS_FRAC_BITS);
                input = (interp * vol) * (FILTER_SCALE / 64);
                c->position += c->increment;
            }

            // One-pole low-pass filter, per channel (not on the final mix):
            // necessary rather than stylistic, since a future per-channel
            // E0y LED-filter toggle would give channels different cutoffs,
            // and a linear filter only commutes with summation when every
            // input shares the same coefficient. alpha (Q0.14) * a Q(24).8
            // delta fits comfortably in int32 - see the fixed-point format
            // comment near POS_FRAC_BITS for the range check.
            // Round the step to nearest: a bare >> floors, which for negative
            // deltas biases every step downward (a small DC offset, and the
            // reason the limit cycle below only ever stuck on the negative side).
            int32_t delta = input - c->filter_state;
            c->filter_state += (ps->filter_alpha * delta + (1 << (ALPHA_FRAC_BITS - 1))) >> ALPHA_FRAC_BITS;
            // A fixed-point one-pole filter can settle into a permanent
            // nonzero "limit cycle" instead of ever reaching exact silence:
            // once the decay step (alpha*delta >> ALPHA_FRAC_BITS) rounds to
            // zero before filter_state itself reaches zero, it gets stuck
            // there forever (with rounding: at +-1, since alpha < 0.5) -
            // confirmed empirically as a constant -1 LSB DC floor during
            // quiet passages (tools/mod_to_wav.c A/B render).
            // Snapping small residuals to exact zero when the true target is
            // silence (input==0) fixes it without affecting real low-level
            // audio content, which never has input pinned at exactly 0.
            if (input == 0 && c->filter_state > -8 && c->filter_state < 8)
                c->filter_state = 0;
            mix += c->filter_state;
        }

        // Scale 8-bit to 16-bit (plus undo the Q.8 filter scaling); up to 4
        // channels can still sum past full scale on loud passages, which is
        // what the clamp below is for.
        int32_t out = (mix * MIX_SCALE) >> FILTER_FRAC_BITS;
        if (out > 32767) out = 32767;
        if (out < -32768) out = -32768;
        buffer[i] = (int16_t)out;
    }
}

// --- Incremental playback driver -------------------------------------------
//
// Replaces the desktop version's nested "for each position: for each row:
// for each tick" main-loop with a resumable state machine: begin_row() and
// enter_row() mirror the original loop body exactly (decode notes,
// process_row, set up this row's tick/sample-per-tick counts; handle
// break/position-jump and pattern-table bounds); advance_tick() is called
// whenever the current tick has no samples left, and moves to the next
// tick (process_tick) or the next row/position. mod_player_produce() is
// the only new public surface: it just calls render_tick() in
// tick-sized-or-smaller chunks, calling advance_tick() between chunks as
// needed, so callers can request any buffer size they want.

static void begin_row(struct PlayerState *ps) {
    struct ModNote *notes = ps->current_notes;
    int pattern_idx = ps->header->patterns[ps->current_position];
    const uint8_t *pattern = ps->pattern_data + pattern_idx * ROWS_PER_PATTERN * NUM_CHANNELS * 4;
    int offset = ps->current_row * NUM_CHANNELS * 4;
    for (int ch = 0; ch < NUM_CHANNELS; ++ch)
        notes[ch] = mod_note_decode(pattern + offset + ch * 4);

    ps->break_row = -1;
    ps->break_position = -1;
    process_row(ps, notes);

    ps->samples_per_tick = OUTPUT_RATE * 5 / (ps->tempo * 2);
    ps->total_ticks_in_row = ps->speed * (1 + ps->pattern_delay);
    ps->pattern_delay = 0;
    ps->current_tick = 0;
    ps->tick_samples_remaining = ps->samples_per_tick;
}

static void restart_song(struct PlayerState *ps);

// Bounds-checks current_position, applies loop-point detection (E6y-aware,
// same as the original), and either starts the row or restarts the song.
static void enter_row(struct PlayerState *ps) {
    if (ps->current_position >= ps->header->pattern_count || ps->current_position >= 128) {
        restart_song(ps);
        return;
    }
    if (ps->loop_count == 0) {
        int key = ps->current_position * ROWS_PER_PATTERN + ps->current_row;
        if (ps->visited[key]) {
            restart_song(ps);
            return;
        }
        ps->visited[key] = true;
    }
    begin_row(ps);
}

static void restart_song(struct PlayerState *ps) {
    ps->restarts++;
    ps->current_position = 0;
    ps->current_row = 0;
    ps->loop_count = 0;
    // The desktop version's main() exits once it detects the song looping,
    // so it never had to handle this. Embedded playback runs forever, so
    // this restart is a real recurring event: without resetting speed/tempo,
    // any Fxx effect encountered mid-song (and never reset by a later Fxx
    // before the loop) would carry over into every subsequent pass, playing
    // faster/slower than the song was authored for.
    ps->speed = DEFAULT_SPEED;
    ps->tempo = DEFAULT_TEMPO;
    // Full channel reinit, not just left alone for row 0 to re-trigger:
    // a channel that doesn't happen to get a fresh note on row 0 would
    // otherwise carry every bit of its old state into the new pass -
    // volume, vibrato/tremolo phase, portamento target, and critically
    // filter_state (the low-pass filter's decay tail). A channel that was
    // loud right when this restart fires would keep ringing into whatever
    // section follows the loop, which can land in a quiet part of the song
    // and sound like unexplained noise with no apparent source. This only
    // affects this synthetic "no explicit loop found" restart - real
    // in-song loop/jump effects (Bxx/Dxx/E6y) go through break_position/
    // break_row directly, never call restart_song(), and correctly keep
    // channel state exactly like a real tracker would.
    memset(ps->channels, 0, sizeof(ps->channels));
    memset(ps->visited, 0, sizeof(ps->visited));
    enter_row(ps);
}

// Called once tick_samples_remaining has reached 0: moves to the next tick
// within the row, or to the next row/position if the row just finished.
static void advance_tick(struct PlayerState *ps) {
    if (!ps->started) {
        ps->started = true;
        enter_row(ps);
        return;
    }

    ps->current_tick++;
    if (ps->current_tick < ps->total_ticks_in_row) {
        process_tick(ps, ps->current_notes);
        ps->tick_samples_remaining = ps->samples_per_tick;
        return;
    }

    if (ps->break_position >= 0 || ps->break_row >= 0) {
        ps->current_position = (ps->break_position >= 0) ? ps->break_position : ps->current_position + 1;
        ps->current_row = (ps->break_row >= 0) ? ps->break_row : 0;
    } else {
        ps->current_row++;
        if (ps->current_row >= ROWS_PER_PATTERN) {
            ps->current_position++;
            ps->current_row = 0;
        }
    }
    enter_row(ps);
}

void mod_player_produce(struct PlayerState *ps, int16_t *out, int n) {
    int produced = 0;
    while (produced < n) {
        if (ps->tick_samples_remaining <= 0) {
            advance_tick(ps);
        }
        int chunk = n - produced;
        if (chunk > ps->tick_samples_remaining) chunk = ps->tick_samples_remaining;
        render_tick(ps, out + produced, chunk);
        ps->tick_samples_remaining -= chunk;
        produced += chunk;
    }
}
