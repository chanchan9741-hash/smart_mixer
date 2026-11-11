#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include "afs.hpp"

namespace SmartMixer {

class StemSeparator {
public:
    StemSeparator(int sample_rate = 44100, bool enabled = true)
        : m_sample_rate(sample_rate), m_enabled(enabled), m_n_fft(1024),
          m_window(m_n_fft), m_fft_buf(m_n_fft) {
        constexpr float PI = 3.14159265358979323846f;
        for (int i = 0; i < m_n_fft; ++i) {
            m_window[i] = 0.5f * (1.0f - std::cos(2.0f * PI * i / (m_n_fft - 1)));
        }
    }

    void set_enabled(bool enabled) { m_enabled = enabled; }
    bool is_enabled() const { return m_enabled; }

    // 3채널 실시간 스템 분리 (Vocal, Beat, Inst/Harmonic)
    void process(const float* input_stereo, float* vocal_stereo, float* beat_stereo, float* inst_stereo, size_t num_frames) {
        if (!m_enabled) {
            std::copy(input_stereo, input_stereo + num_frames * 2, vocal_stereo);
            std::copy(input_stereo, input_stereo + num_frames * 2, beat_stereo);
            if (inst_stereo) {
                std::copy(input_stereo, input_stereo + num_frames * 2, inst_stereo);
            }
            return;
        }

        // 실시간 Mid/Side 및 스펙트럼 마스킹 3-Way 분리
        // 1. Vocal : Center(Mid) 및 300Hz ~ 3400Hz 주파수 포먼트
        // 2. Beat  : Low-end Kick/Bass (<250Hz), High-end Cymbals (>4000Hz)
        // 3. Inst  : Stereo Side 성분(기타/신스/스트링) 및 잔여 하모닉스
        float bin_res = static_cast<float>(m_sample_rate) / m_n_fft;
        size_t proc_len = std::min(num_frames, static_cast<size_t>(m_n_fft));

        for (size_t i = 0; i < proc_len; ++i) {
            float l = input_stereo[i * 2];
            float r = input_stereo[i * 2 + 1];
            float mid = 0.5f * (l + r);
            m_fft_buf[i] = std::complex<float>(mid * m_window[i], 0.0f);
        }
        for (size_t i = proc_len; i < static_cast<size_t>(m_n_fft); ++i) {
            m_fft_buf[i] = std::complex<float>(0.0f, 0.0f);
        }

        FastFFT::fft(m_fft_buf);

        std::vector<std::complex<float>> vocal_fft = m_fft_buf;
        std::vector<std::complex<float>> beat_fft = m_fft_buf;
        std::vector<std::complex<float>> inst_fft = m_fft_buf;

        for (int i = 0; i < m_n_fft; ++i) {
            int sym_i = (i <= m_n_fft / 2) ? i : (m_n_fft - i);
            float freq = sym_i * bin_res;

            float vocal_w = 0.0f;
            float beat_w = 0.0f;
            float inst_w = 0.0f;

            if (freq < 240.0f) {
                // 저역 킥/베이스: 비트 우세
                beat_w = 0.85f;
                inst_w = 0.15f;
                vocal_w = 0.0f;
            } else if (freq >= 240.0f && freq <= 3600.0f) {
                // 보컬 음성 대역
                float diff = (freq - 1400.0f) / 750.0f;
                vocal_w = std::exp(-0.5f * diff * diff) * 0.85f;
                vocal_w = std::clamp(vocal_w, 0.05f, 0.85f);
                float remain = 1.0f - vocal_w;
                beat_w = remain * 0.35f;
                inst_w = remain * 0.65f;
            } else {
                // 고역 타악기/공명
                beat_w = 0.65f;
                inst_w = 0.30f;
                vocal_w = 0.05f;
            }

            vocal_fft[i] *= vocal_w;
            beat_fft[i] *= beat_w;
            inst_fft[i] *= inst_w;
        }

        // IFFT
        for (int i = 0; i < m_n_fft; ++i) {
            vocal_fft[i] = std::conj(vocal_fft[i]);
            beat_fft[i] = std::conj(beat_fft[i]);
            inst_fft[i] = std::conj(inst_fft[i]);
        }
        FastFFT::fft(vocal_fft);
        FastFFT::fft(beat_fft);
        FastFFT::fft(inst_fft);

        float inv_n = 1.0f / m_n_fft;
        for (size_t i = 0; i < num_frames; ++i) {
            float side = 0.5f * (input_stereo[i * 2] - input_stereo[i * 2 + 1]);
            float v_mid = (i < proc_len) ? (vocal_fft[i].real() * inv_n) : input_stereo[i * 2];
            float b_mid = (i < proc_len) ? (beat_fft[i].real() * inv_n) : input_stereo[i * 2];
            float i_mid = (i < proc_len) ? (inst_fft[i].real() * inv_n) : input_stereo[i * 2];

            // 스테레오 사이드 성분 복원
            vocal_stereo[i * 2]     = v_mid + side * 0.10f;
            vocal_stereo[i * 2 + 1] = v_mid - side * 0.10f;

            beat_stereo[i * 2]     = b_mid + side * 0.20f;
            beat_stereo[i * 2 + 1] = b_mid - side * 0.20f;

            if (inst_stereo) {
                inst_stereo[i * 2]     = i_mid + side * 0.70f;
                inst_stereo[i * 2 + 1] = i_mid - side * 0.70f;
            }
        }
    }

    // 2채널 스템 분리 오버로드 (기존 호환성)
    void process(const float* input_stereo, float* vocal_stereo, float* beat_stereo, size_t num_frames) {
        process(input_stereo, vocal_stereo, beat_stereo, nullptr, num_frames);
    }

private:
    int m_sample_rate;
    bool m_enabled;
    int m_n_fft;
    std::vector<float> m_window;
    std::vector<std::complex<float>> m_fft_buf;
};

} // namespace SmartMixer

