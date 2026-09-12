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
    float position;
    float increment;
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
    bool visited[128 * MOD_ROWS_PER_PATTERN];
};

// Parses a MOD file already sitting in memory (flash or RAM). Returns 0 on
// success, -1 if the data is too small, unrecognized, or corrupt (pattern
// data extending past the end of the buffer).
int mod_player_init(struct PlayerState *ps, const uint8_t *mod_data, size_t mod_size);

// Renders exactly `n` mono 16-bit samples at 48 kHz, advancing playback
// state as needed - including across tick/row/pattern boundaries within a
// single call, and looping back to the start of the song when it ends.
// `n` does not need to align with tick or row boundaries in any way.
void mod_player_produce(struct PlayerState *ps, int16_t *out, int n);
