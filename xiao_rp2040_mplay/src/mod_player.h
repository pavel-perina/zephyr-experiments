// MOD player core, ported from ~/devel/mplay/mod_player.c (same author, MIT).
// Parsing, effects, and mixing logic is reused near-verbatim; only the
// driving loop and the byte-order-sensitive sample-header handling changed,
// see mod_player.c for why.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MOD_MAX_SAMPLES      32   // 1-indexed: slot 0 unused, 1..31 are real samples
#define MOD_NUM_CHANNELS     4
#define MOD_ROWS_PER_PATTERN 64

struct __attribute__((packed)) ModSample {
    char name[22];
    uint16_t length_words;
    int8_t fine_tune;
    uint8_t volume;
    uint16_t repeat_offset;
    uint16_t repeat_length;
};

struct __attribute__((packed)) ModHeader {
    char title[20];
    struct ModSample samples[31];
    uint8_t pattern_count;
    uint8_t idk;
    uint8_t patterns[128];
    char magic[4];
};

struct ModNote {
    uint8_t  instrument;
    uint16_t period;
    uint8_t  effect;
    uint8_t  effect_arg;
};

struct Channel {
    const int8_t *sample_data;
    uint32_t sample_length;
    uint32_t loop_start;
    uint32_t loop_length;
    // position/increment are Q17.15 fixed point (see POS_FRAC_BITS in
    // mod_player.c), not float - the RP2040 (Cortex-M0+) has no hardware
    // FPU, and this pair sits in the hottest loop in the program (every
    // output sample, every channel). Integer/fixed-point math there is
    // what keeps real-time playback glitch-free on that chip.
    uint32_t position;
    uint32_t increment;
    uint16_t period;
    uint8_t volume;
    uint8_t instrument;
    uint16_t target_period;
    uint8_t portamento_speed;
    uint8_t vibrato_speed;
    uint8_t vibrato_depth;
    uint8_t vibrato_pos;
    uint8_t vibrato_waveform;
    uint8_t tremolo_speed;
    uint8_t tremolo_depth;
    uint8_t tremolo_pos;
    uint8_t tremolo_waveform;
    int8_t tremolo_delta;
    int8_t finetune;
    uint8_t offset_memory;
    // One-pole low-pass filter state (Amiga RC/LED filter emulation),
    // Q(24).8 fixed point - same "no FPU on RP2040" reasoning as position.
    int32_t filter_state;
    // Stereo position, 0 = hard left .. 255 = hard right. Default: Amiga's
    // L R R L softened to 48/207 (as mplay-rs), overridden by 8xx / E8x -
    // see mod_mix_stereo().
    uint8_t pan;
};

struct PlayerState {
    const void *mod_data;
    size_t mod_size;
    const struct ModHeader *header;
    const uint8_t *pattern_data;
    const int8_t *sample_base;

    // Precomputed per-sample metadata. The source MOD may live in flash
    // (read-only XIP memory), so unlike the original desktop player these
    // are computed once here rather than byte-swapped in place over the
    // source buffer - see mod_player.c's mod_player_init().
    const int8_t *sample_offsets[MOD_MAX_SAMPLES];
    uint32_t sample_lengths[MOD_MAX_SAMPLES];
    uint32_t sample_loop_start[MOD_MAX_SAMPLES];
    uint32_t sample_loop_length[MOD_MAX_SAMPLES];
    int8_t sample_finetune[MOD_MAX_SAMPLES];

    int32_t filter_alpha; // one-pole low-pass coefficient, Q0.14 fixed point, set once in mod_player_init()

    int speed;
    int tempo;
    int current_position;
    int current_row;
    int current_tick;
    struct Channel channels[MOD_NUM_CHANNELS];
    int break_row;
    int break_position;
    int pattern_delay;
    int loop_row;
    int loop_count;

    // Incremental-render state: replaces the desktop player's nested
    // position/row/tick loop so samples can be pulled in arbitrary-sized
    // chunks (DMA buffer sized, not tick sized) - see mod_player_produce().
    struct ModNote current_notes[MOD_NUM_CHANNELS];
    int total_ticks_in_row;
    int samples_per_tick;
    int tick_samples_remaining;
    bool started;
    // Incremented each time the song ends and restart_song() loops it back
    // to the start (end of the order list, or a jump back to a row already
    // played) - lets a playlist caller advance instead of looping forever.
    uint32_t restarts;

    // Mixer-side (tier 2) setting, not song state: bit ch set = channel ch
    // left out of mod_mix_stereo()/mod_mix_mono() (and so mod_player_
    // produce()). Muted channels still play internally - effects, position
    // and their mod_player_render() output all continue - so unmuting is
    // seamless and visualizers can still show them. Cleared by
    // mod_player_init().
    uint8_t mute_mask;
    bool visited[128 * MOD_ROWS_PER_PATTERN];
};

// Parses a MOD file already sitting in memory (flash or RAM). Returns 0 on
// success, -1 if the data is too small, unrecognized, or corrupt (pattern
// data extending past the end of the buffer).
int mod_player_init(struct PlayerState *ps, const uint8_t *mod_data, size_t mod_size);

// --- Tier 1: per-channel rendering ------------------------------------------
//
// Renders exactly `n` samples at 48 kHz for every channel separately into
// ch[0..MOD_NUM_CHANNELS-1][0..n-1], advancing playback state as needed -
// including across tick/row/pattern boundaries within a single call, and
// looping back to the start of the song when it ends (ps->restarts counts
// that). `n` does not need to align with tick or row boundaries in any way.
//
// Each sample is the channel's output after volume and the Amiga-style
// low-pass filter, in the mixer's internal fixed-point format: 8-bit sample
// units in Q.8, i.e. roughly -32768..32767 for a full-volume, full-scale
// channel (MOD_CHANNEL_FRAC_BITS fractional bits). Kept at full precision
// (int32, not int16) so mixing them afterwards is bit-exact with the old
// all-in-one mixer.
#define MOD_CHANNEL_FRAC_BITS 8
void mod_player_render(struct PlayerState *ps, int32_t *const ch[MOD_NUM_CHANNELS], int n);

// --- Tier 2: mixers ----------------------------------------------------------
//
// Mix n samples of mod_player_render() output down to 16-bit PCM, honouring
// each channel's pan (ps->channels[ch].pan) and ps->mute_mask. Pan is linear:
// weights (256 - p) and p, p = 0..256, always summing to 256 - so
// mod_mix_mono() is literally the sum of mod_mix_stereo()'s two
// accumulators (before scaling/clamping), and bit-identical to the old mono
// mixer. Pan is read once per call, so an 8xx mid-buffer takes effect at
// the next call (one buffer, ~5ms at 256 samples).
void mod_mix_stereo(const struct PlayerState *ps, const int32_t *const ch[MOD_NUM_CHANNELS],
                    int16_t *left, int16_t *right, int n);
void mod_mix_mono(const struct PlayerState *ps, const int32_t *const ch[MOD_NUM_CHANNELS],
                  int16_t *out, int n);

// --- Convenience: the original one-call API ----------------------------------
//
// Renders exactly `n` mono 16-bit samples: mod_player_render() +
// mod_mix_mono() in small chunks on the stack (no allocation, any `n`).
// Output is bit-identical to the pre-split mixer.
void mod_player_produce(struct PlayerState *ps, int16_t *out, int n);
