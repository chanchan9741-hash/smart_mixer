#pragma once

#include <cstddef>

namespace SmartMixer {

struct AudioConfig {
    int sample_rate = 44100;          // PR-2: 44.1 kHz
    int block_size = 512;             // PR-1: 512 samples (~11.6ms latency < 20ms)
    int channels_in = 2;              // Stereo Input
    int channels_out = 2;             // Stereo Output
    size_t ring_buffer_size = 4096;   // Ring buffer capacity
    int num_mixer_channels = 5;       // H-FR-4, S-FR-1: 5 Channels
};

struct AFSConfig {
    bool enabled = true;
    int fft_size = 1024;              // Frequency resolution (~43 Hz/bin)
    float peak_threshold_db = 12.0f;  // Peak prominence threshold in dB
    int persistence = 3;              // Continuous detection frames to confirm feedback
    int max_notch_filters = 8;        // Maximum active notch filters
    float notch_q = 12.0f;            // Narrow Q-factor for tone preservation
    float notch_attenuation_db = -18.0f; // Attenuation depth
};

struct LoudnessConfig {
    bool enabled = true;
    float target_lufs = -14.0f;       // Target LUFS standard
    float gain_smoothing_alpha = 0.05f; // Anti-pumping smoothing
    float max_boost_db = 12.0f;       // Max gain boost limit
    float max_cut_db = -24.0f;        // Max gain cut limit
    float limiter_threshold_db = -0.5f; // Anti-clipping peak limiter
};

struct SystemConfig {
    AudioConfig audio;
    AFSConfig afs;
    LoudnessConfig loudness;
    int ui_target_fps = 30;           // PR-3: 30+ FPS rendering
};

} // namespace SmartMixer

