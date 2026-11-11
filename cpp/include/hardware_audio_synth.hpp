#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdlib>

namespace SmartMixer {

class HardwareAudioSynthesizer {
public:
    HardwareAudioSynthesizer(int sample_rate = 44100)
        : m_sample_rate(sample_rate),
          m_beat_playing(true),
          m_line2_playing(true),
          m_bpm(120.0f),
          m_beat_sample_idx(0),
          m_chord_sample_idx(0),
          m_current_chord_idx(0) {}

    void set_beat_playing(bool play) { m_beat_playing = play; }
    bool is_beat_playing() const { return m_beat_playing; }

    void set_line2_playing(bool play) { m_line2_playing = play; }
    bool is_line2_playing() const { return m_line2_playing; }

    void set_bpm(float bpm) { m_bpm = std::clamp(bpm, 60.0f, 200.0f); }
    float get_bpm() const { return m_bpm; }

    // Channel 2: 비트/드럼 트랙 실시간 합성 (Kick, Snare, Hi-Hat)
    void render_beat_block(float* stereo_out, size_t num_frames) {
        std::fill(stereo_out, stereo_out + num_frames * 2, 0.0f);
        if (!m_beat_playing) return;

        constexpr float PI = 3.14159265358979323846f;
        int samples_per_beat = static_cast<int>(m_sample_rate * 60.0f / m_bpm);
        int total_bar_samples = samples_per_beat * 4; // 4/4 박자

        for (size_t i = 0; i < num_frames; ++i) {
            int beat_pos = m_beat_sample_idx % total_bar_samples;
            int beat_num = beat_pos / samples_per_beat; // 0, 1, 2, 3
            int pos_in_beat = beat_pos % samples_per_beat;

            float out_l = 0.0f;
            float out_r = 0.0f;

            // 1. Kick Drum (Beat 0, 2)
            if (beat_num == 0 || beat_num == 2) {
                float t = static_cast<float>(pos_in_beat) / m_sample_rate;
                if (t < 0.25f) {
                    float freq = 45.0f + 85.0f * std::exp(-t * 24.0f);
                    float env = std::exp(-t * 12.0f);
                    float kick = 0.65f * std::sin(2.0f * PI * freq * t) * env;
                    out_l += kick;
                    out_r += kick;
                }
            }

            // 2. Snare Drum (Beat 1, 3)
            if (beat_num == 1 || beat_num == 3) {
                float t = static_cast<float>(pos_in_beat) / m_sample_rate;
                if (t < 0.22f) {
                    float noise = (static_cast<float>(rand()) / RAND_MAX * 2.0f - 1.0f);
                    float body = std::sin(2.0f * PI * 185.0f * t);
                    float env = std::exp(-t * 16.0f);
                    float snare = 0.45f * (noise * 0.7f + body * 0.3f) * env;
                    out_l += snare;
                    out_r += snare;
                }
            }

            // 3. Hi-Hat (8분 음표마다 틱 소리)
            int half_beat = samples_per_beat / 2;
            int pos_in_half = beat_pos % half_beat;
            float hat_t = static_cast<float>(pos_in_half) / m_sample_rate;
            if (hat_t < 0.045f) {
                float noise = (static_cast<float>(rand()) / RAND_MAX * 2.0f - 1.0f);
                float env = std::exp(-hat_t * 65.0f);
                float hat = 0.18f * noise * env;
                out_l += hat * 0.8f;
                out_r += hat * 1.2f; // 패닝 살짝 우측
            }

            stereo_out[i * 2] = out_l;
            stereo_out[i * 2 + 1] = out_r;

            m_beat_sample_idx++;
        }
    }

    // Channel 4: 라인 2/악기 코드 반주 합성 (C - G - Am - F)
    void render_line2_block(float* stereo_out, size_t num_frames) {
        std::fill(stereo_out, stereo_out + num_frames * 2, 0.0f);
        if (!m_line2_playing) return;

        constexpr float PI = 3.14159265358979323846f;
        int chord_duration_samples = static_cast<int>(m_sample_rate * 60.0f / m_bpm) * 2; // 2비트마다 코드 변경

        // 코드별 3화음 주파수 (C, G, Am, F)
        const float chords[4][3] = {
            { 261.63f, 329.63f, 392.00f }, // C (C4, E4, G4)
            { 196.00f, 246.94f, 293.66f }, // G (G3, B3, D4)
            { 220.00f, 261.63f, 329.63f }, // Am (A3, C4, E4)
            { 174.61f, 220.00f, 261.63f }  // F (F3, A3, C4)
        };

        for (size_t i = 0; i < num_frames; ++i) {
            float t = static_cast<float>(m_chord_sample_idx) / m_sample_rate;
            float env = std::exp(-t * 2.2f); // 자연스러운 건반/기타 감쇄

            float sample = 0.0f;
            const float* current_notes = chords[m_current_chord_idx];
            for (int n = 0; n < 3; ++n) {
                sample += std::sin(2.0f * PI * current_notes[n] * t);
                // 2차 배음 추가로 풍부한 톤 형성
                sample += 0.35f * std::sin(4.0f * PI * current_notes[n] * t);
            }
            sample = (sample / 3.0f) * env * 0.35f;

            stereo_out[i * 2] = sample * 1.1f;
            stereo_out[i * 2 + 1] = sample * 0.9f;

            m_chord_sample_idx++;
            if (m_chord_sample_idx >= chord_duration_samples) {
                m_chord_sample_idx = 0;
                m_current_chord_idx = (m_current_chord_idx + 1) % 4;
            }
        }
    }

private:
    int m_sample_rate;
    bool m_beat_playing;
    bool m_line2_playing;
    float m_bpm;
    int m_beat_sample_idx;
    int m_chord_sample_idx;
    int m_current_chord_idx;
};

} // namespace SmartMixer

