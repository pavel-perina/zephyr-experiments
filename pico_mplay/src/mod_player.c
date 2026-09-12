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
#define MIX_SCALE        32    // 8->16 bit output scale; lower = quieter overall (was 128)

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

static float calc_increment(uint16_t period) {
    if (period == 0) return 0.0f;
    return AMIGA_PAL_CLOCK / ((float)period * 2.0f * OUTPUT_RATE);
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
            c->position = 0.0f;
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
                    c->position = (float)off;
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
                    c->position = 0.0f;
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
        int32_t mix = 0;

        for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
            struct Channel *c = &ps->channels[ch];

            if (!c->sample_data || c->period == 0)
                continue;

            int pos = (int)c->position;

            // Check bounds
            if (c->loop_length > 2) {
                uint32_t loop_end = c->loop_start + c->loop_length;
                while (c->position >= (float)loop_end) {
                    c->position -= (float)c->loop_length;
                }
                pos = (int)c->position;
            } else {
                if ((uint32_t)pos >= c->sample_length) {
                    continue; // silence
                }
            }

            int vol = c->volume + c->tremolo_delta;
            if (vol < 0) vol = 0;
            if (vol > 64) vol = 64;

            int8_t raw = c->sample_data[pos];
            mix += (int32_t)raw * vol / 64;

            c->position += c->increment;
        }

        // Scale 8-bit to 16-bit; up to 4 channels can still sum past full
        // scale on loud passages, which is what the clamp below is for.
        int32_t out = mix * MIX_SCALE;
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
    ps->current_position = 0;
    ps->current_row = 0;
    ps->loop_count = 0;
    memset(ps->visited, 0, sizeof(ps->visited));
    // Channel state (sample positions, volume, etc.) intentionally left
    // alone - row 0's notes will re-trigger whichever channels have one.
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
