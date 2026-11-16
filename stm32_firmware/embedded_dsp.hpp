/**
 * @file embedded_dsp.hpp
 * @brief STM32H743VIT6 (Cortex-M7 FPU) 온보드 즉석 실시간 DSP 코어
 *
 * 특징:
 *  - 동적 힙 할당(malloc / new / std::vector / std::mutex) 0 bytes (완전 정적 SRAM 할당)
 *  - I2S/SAI DMA 인터럽트 및 초저지연(< 5ms) 실시간 루프에서 안전하게 구동
 *  - 온보드 AFS(하울링 자동 억제), 3-Band EQ, 5채널 안티-팝 믹서, 라우드니스 매치/리미터,
 *    단독 모드 STFT 스템 분리, ADC/스위치 디바운싱, SSD1306 128x64 OLED 렌더링 내장
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <array>

#include "packet_protocol.h"

namespace SmartMixerMCU {

constexpr float MCU_PI = 3.14159265358979323846f;

template <typename T>
inline T mcu_clamp(T v, T lo, T hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

// ============================================================================
// 1. S-FR-1: Lock-Free SPSC 정적 원형 링 버퍼 (I2S DMA ISR <-> DSP Loop)
// ============================================================================
template <size_t CapacityFrames, size_t Channels = 2>
class StaticRingBuffer {
public:
    StaticRingBuffer() : m_write_idx(0), m_read_idx(0), m_underruns(0), m_overruns(0) {
        m_buffer.fill(0.0f);
    }

    size_t available() const {
        size_t w = m_write_idx;
        size_t r = m_read_idx;
        return (w >= r) ? (w - r) : (CapacityFrames - r + w);
    }

    size_t free_space() const {
        return (CapacityFrames - 1) - available();
    }

    size_t write(const float* data, size_t num_frames) {
        if (!data || num_frames == 0) return 0;
        size_t space = free_space();
        if (num_frames > space) {
            m_overruns++;
            num_frames = space;
        }
        for (size_t i = 0; i < num_frames; ++i) {
            size_t idx = m_write_idx;
            for (size_t c = 0; c < Channels; ++c) {
                m_buffer[idx * Channels + c] = data[i * Channels + c];
            }
            m_write_idx = (idx + 1) % CapacityFrames;
        }
        return num_frames;
    }

    size_t read(float* out_data, size_t num_frames) {
        if (!out_data || num_frames == 0) return 0;
        size_t avail = available();
        size_t to_read = (num_frames <= avail) ? num_frames : avail;
        if (to_read < num_frames) {
            m_underruns++;
        }
        for (size_t i = 0; i < to_read; ++i) {
            size_t idx = m_read_idx;
            for (size_t c = 0; c < Channels; ++c) {
                out_data[i * Channels + c] = m_buffer[idx * Channels + c];
            }
            m_read_idx = (idx + 1) % CapacityFrames;
        }
        for (size_t i = to_read; i < num_frames; ++i) {
            for (size_t c = 0; c < Channels; ++c) {
                out_data[i * Channels + c] = 0.0f;
            }
        }
        return to_read;
    }

    uint32_t get_underruns() const { return m_underruns; }
    uint32_t get_overruns() const { return m_overruns; }

private:
    std::array<float, CapacityFrames * Channels> m_buffer;
    volatile size_t m_write_idx;
    volatile size_t m_read_idx;
    uint32_t m_underruns;
    uint32_t m_overruns;
};

// ============================================================================
// 2. S-FR-3 / S-FR-5: IIR Direct Form II Transposed Biquad 필터
// ============================================================================
enum class BiquadType : uint8_t {
    Notch = 0,
    Peaking,
    Highpass,
    Lowpass,
    Passthrough
};

class EmbeddedBiquad {
public:
    EmbeddedBiquad(BiquadType type = BiquadType::Passthrough, int sample_rate = 44100,
                   float freq = 1000.0f, float q = 10.0f, float gain_db = 0.0f)
        : m_type(type), m_sample_rate(sample_rate), m_freq(freq),
          m_q(std::max(0.1f, q)), m_gain_db(gain_db),
          m_b0(1.0f), m_b1(0.0f), m_b2(0.0f), m_a1(0.0f), m_a2(0.0f),
          m_z1_l(0.0f), m_z2_l(0.0f), m_z1_r(0.0f), m_z2_r(0.0f) {
        compute_coefficients();
    }

    void configure(BiquadType type, int sample_rate, float freq, float q, float gain_db = 0.0f) {
        m_type = type;
        m_sample_rate = sample_rate;
        m_freq = freq;
        m_q = std::max(0.1f, q);
        m_gain_db = gain_db;
        compute_coefficients();
    }

    void set_gain_db(float gain_db) {
        if (std::fabs(m_gain_db - gain_db) > 0.05f) {
            m_gain_db = gain_db;
            compute_coefficients();
        }
    }

    void reset() {
        m_z1_l = m_z2_l = m_z1_r = m_z2_r = 0.0f;
    }

    void process(float* stereo_buffer, size_t num_frames) {
        if (m_type == BiquadType::Passthrough) return;
        for (size_t i = 0; i < num_frames; ++i) {
            float xl = stereo_buffer[i * 2];
            float yl = m_b0 * xl + m_z1_l;
            m_z1_l = m_b1 * xl - m_a1 * yl + m_z2_l;
            m_z2_l = m_b2 * xl - m_a2 * yl;
            stereo_buffer[i * 2] = yl;

            float xr = stereo_buffer[i * 2 + 1];
            float yr = m_b0 * xr + m_z1_r;
            m_z1_r = m_b1 * xr - m_a1 * yr + m_z2_r;
            m_z2_r = m_b2 * xr - m_a2 * yr;
            stereo_buffer[i * 2 + 1] = yr;
        }
    }

    float get_freq() const { return m_freq; }

private:
    void compute_coefficients() {
        if (m_type == BiquadType::Passthrough) {
            m_b0 = 1.0f; m_b1 = m_b2 = m_a1 = m_a2 = 0.0f;
            return;
        }
        float f0 = mcu_clamp(m_freq, 20.0f, m_sample_rate * 0.49f);
        float w0 = 2.0f * MCU_PI * f0 / static_cast<float>(m_sample_rate);
        float cos_w0 = std::cos(w0);
        float sin_w0 = std::sin(w0);
        float alpha = sin_w0 / (2.0f * m_q);

        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
        float a0 = 1.0f, a1 = 0.0f, a2 = 0.0f;

        switch (m_type) {
            case BiquadType::Notch:
                b0 = 1.0f; b1 = -2.0f * cos_w0; b2 = 1.0f;
                a0 = 1.0f + alpha; a1 = -2.0f * cos_w0; a2 = 1.0f - alpha;
                break;
            case BiquadType::Peaking: {
                float A = std::pow(10.0f, m_gain_db / 40.0f);
                b0 = 1.0f + alpha * A; b1 = -2.0f * cos_w0; b2 = 1.0f - alpha * A;
                a0 = 1.0f + alpha / A; a1 = -2.0f * cos_w0; a2 = 1.0f - alpha / A;
                break;
            }
            case BiquadType::Highpass:
                b0 = (1.0f + cos_w0) * 0.5f; b1 = -(1.0f + cos_w0); b2 = (1.0f + cos_w0) * 0.5f;
                a0 = 1.0f + alpha; a1 = -2.0f * cos_w0; a2 = 1.0f - alpha;
                break;
            case BiquadType::Lowpass:
                b0 = (1.0f - cos_w0) * 0.5f; b1 = 1.0f - cos_w0; b2 = (1.0f - cos_w0) * 0.5f;
                a0 = 1.0f + alpha; a1 = -2.0f * cos_w0; a2 = 1.0f - alpha;
                break;
            default:
                break;
        }

        m_b0 = b0 / a0; m_b1 = b1 / a0; m_b2 = b2 / a0;
        m_a1 = a1 / a0; m_a2 = a2 / a0;
    }

    BiquadType m_type;
    int m_sample_rate;
    float m_freq;
    float m_q;
    float m_gain_db;
    float m_b0, m_b1, m_b2, m_a1, m_a2;
    float m_z1_l, m_z2_l, m_z1_r, m_z2_r;
};

// ============================================================================
// 3. 정적 Radix-2 Cooley-Tukey FFT/IFFT (Zero Heap)
// ============================================================================
struct ComplexF {
    float re;
    float im;
};

template <size_t N>
class EmbeddedFFT {
public:
    static void fft(ComplexF* a, bool inverse = false) {
        // Bit-reversal permutation
        for (size_t i = 1, j = 0; i < N; ++i) {
            size_t bit = N >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) {
                ComplexF tmp = a[i];
                a[i] = a[j];
                a[j] = tmp;
            }
        }

        for (size_t len = 2; len <= N; len <<= 1) {
            float ang = (inverse ? 2.0f : -2.0f) * MCU_PI / static_cast<float>(len);
            float wlen_re = std::cos(ang);
            float wlen_im = std::sin(ang);

            for (size_t i = 0; i < N; i += len) {
                float w_re = 1.0f;
                float w_im = 0.0f;
                for (size_t j = 0; j < len / 2; ++j) {
                    ComplexF u = a[i + j];
                    ComplexF t = a[i + j + len / 2];
                    float v_re = t.re * w_re - t.im * w_im;
                    float v_im = t.re * w_im + t.im * w_re;

                    a[i + j] = { u.re + v_re, u.im + v_im };
                    a[i + j + len / 2] = { u.re - v_re, u.im - v_im };

                    float next_w_re = w_re * wlen_re - w_im * wlen_im;
                    w_im = w_re * wlen_im + w_im * wlen_re;
                    w_re = next_w_re;
                }
            }
        }

        if (inverse) {
            float inv_n = 1.0f / static_cast<float>(N);
            for (size_t i = 0; i < N; ++i) {
                a[i].re *= inv_n;
                a[i].im *= inv_n;
            }
        }
    }
};

// ============================================================================
// 4. S-FR-3: MCU 온보드 자동 피드백 억제 (AFS) - 마이크 채널 전용 (< 0.1ms)
// ============================================================================
class EmbeddedAFS {
public:
    static constexpr size_t FFT_SIZE = 1024;
    static constexpr size_t HALF_N = FFT_SIZE / 2;
    static constexpr size_t MAX_NOTCHES = 8;

    EmbeddedAFS(int sample_rate = 44100, float threshold_db = 12.0f, int persistence = 3)
        : m_sample_rate(sample_rate), m_enabled(true),
          m_threshold_db(threshold_db), m_persistence(persistence),
          m_bin_res(static_cast<float>(sample_rate) / FFT_SIZE),
          m_analysis_idx(0), m_active_filter_count(0),
          m_filter_write_head(0), m_suppressed_count(0), m_last_freq(0.0f) {
        for (size_t i = 0; i < FFT_SIZE; ++i) {
            m_window[i] = 0.5f * (1.0f - std::cos(2.0f * MCU_PI * i / (FFT_SIZE - 1)));
        }
        m_analysis_buf.fill(0.0f);
        m_candidate_counts.fill(0);
    }

    void set_enabled(bool en) { m_enabled = en; }
    bool is_enabled() const { return m_enabled; }

    void clear_filters() {
        m_active_filter_count = 0;
        m_filter_write_head = 0;
        m_candidate_counts.fill(0);
    }

    /**
     * @return 새로 하울링이 감지되어 노치 필터가 배치되었으면 해당 주파수(Hz), 아니면 0.0f
     */
    float process(float* stereo_buffer, size_t num_frames) {
        if (!m_enabled) return 0.0f;

        float newly_detected_freq = 0.0f;

        for (size_t i = 0; i < num_frames; ++i) {
            m_analysis_buf[m_analysis_idx++] = 0.5f * (stereo_buffer[i * 2] + stereo_buffer[i * 2 + 1]);
            if (m_analysis_idx >= FFT_SIZE) {
                float f = analyze_and_allocate();
                if (f > 0.0f) newly_detected_freq = f;
                m_analysis_idx = 0;
            }
        }

        for (size_t k = 0; k < m_active_filter_count; ++k) {
            m_notches[k].process(stereo_buffer, num_frames);
        }

        return newly_detected_freq;
    }

    size_t get_active_filter_count() const { return m_active_filter_count; }
    uint32_t get_suppressed_count() const { return m_suppressed_count; }
    float get_last_suppressed_freq() const { return m_last_freq; }

private:
    float analyze_and_allocate() {
        for (size_t i = 0; i < FFT_SIZE; ++i) {
            m_fft_buf[i] = { m_analysis_buf[i] * m_window[i], 0.0f };
        }
        EmbeddedFFT<FFT_SIZE>::fft(m_fft_buf.data(), false);

        for (size_t i = 0; i < HALF_N; ++i) {
            float mag = std::sqrt(m_fft_buf[i].re * m_fft_buf[i].re + m_fft_buf[i].im * m_fft_buf[i].im) + 1e-12f;
            m_mag_db[i] = 20.0f * std::log10(mag);
        }

        constexpr int KERNEL = 15;
        for (size_t i = 0; i < HALF_N; ++i) {
            float sum = 0.0f;
            int cnt = 0;
            int lo = std::max(0, static_cast<int>(i) - KERNEL);
            int hi = std::min(static_cast<int>(HALF_N) - 1, static_cast<int>(i) + KERNEL);
            for (int k = lo; k <= hi; ++k) {
                sum += m_mag_db[k];
                cnt++;
            }
            m_smoothed[i] = sum / static_cast<float>(cnt);
        }

        size_t min_bin = static_cast<size_t>(150.0f / m_bin_res);
        size_t max_bin = std::min(HALF_N - 2, static_cast<size_t>(8000.0f / m_bin_res));

        float detected = 0.0f;
        for (size_t b = min_bin; b <= max_bin; ++b) {
            bool is_peak = (m_mag_db[b] > m_mag_db[b - 1]) && (m_mag_db[b] > m_mag_db[b + 1]);
            bool is_prominent = ((m_mag_db[b] - m_smoothed[b]) > m_threshold_db) && (m_mag_db[b] > -40.0f);
            if (is_peak && is_prominent) {
                if (m_candidate_counts[b] < 100) m_candidate_counts[b]++;
                if (m_candidate_counts[b] >= m_persistence) {
                    float freq = static_cast<float>(b) * m_bin_res;
                    if (allocate_notch(freq)) {
                        detected = freq;
                    }
                }
            } else if (m_candidate_counts[b] > 0) {
                m_candidate_counts[b]--;
            }
        }
        return detected;
    }

    bool allocate_notch(float freq) {
        for (size_t i = 0; i < m_active_filter_count; ++i) {
            if (std::fabs(m_notches[i].get_freq() - freq) / freq < 0.05f) {
                return false;
            }
        }
        size_t slot = m_filter_write_head;
        m_notches[slot].configure(BiquadType::Notch, m_sample_rate, freq, 12.0f, -18.0f);
        m_notches[slot].reset();
        m_filter_write_head = (m_filter_write_head + 1) % MAX_NOTCHES;
        if (m_active_filter_count < MAX_NOTCHES) m_active_filter_count++;
        m_suppressed_count++;
        m_last_freq = freq;
        return true;
    }

    int m_sample_rate;
    bool m_enabled;
    float m_threshold_db;
    int m_persistence;
    float m_bin_res;
    size_t m_analysis_idx;
    size_t m_active_filter_count;
    size_t m_filter_write_head;
    uint32_t m_suppressed_count;
    float m_last_freq;

    std::array<float, FFT_SIZE> m_window;
    std::array<float, FFT_SIZE> m_analysis_buf;
    std::array<ComplexF, FFT_SIZE> m_fft_buf;
    std::array<float, HALF_N> m_mag_db;
    std::array<float, HALF_N> m_smoothed;
    std::array<uint8_t, HALF_N> m_candidate_counts;
    std::array<EmbeddedBiquad, MAX_NOTCHES> m_notches;
};

// ============================================================================
// 5. S-FR-4 (하드웨어 단독 모드용): 실시간 50% OLA COLA STFT 3-Way 분리기
// ============================================================================
class EmbeddedStftSeparator {
public:
    static constexpr size_t FFT_SIZE = 1024;
    static constexpr size_t HOP_SIZE = 512;

    EmbeddedStftSeparator(int sample_rate = 44100, bool enabled = true)
        : m_sample_rate(sample_rate), m_enabled(enabled) {
        for (size_t i = 0; i < FFT_SIZE; ++i) {
            float h = 0.5f * (1.0f - std::cos(2.0f * MCU_PI * i / FFT_SIZE));
            m_window[i] = std::sqrt(h);
        }
        m_prev_in_l.fill(0.0f); m_prev_in_r.fill(0.0f);
        m_tail_v_l.fill(0.0f);  m_tail_v_r.fill(0.0f);
        m_tail_b_l.fill(0.0f);  m_tail_b_r.fill(0.0f);
        m_tail_i_l.fill(0.0f);  m_tail_i_r.fill(0.0f);
    }

    void set_enabled(bool en) { m_enabled = en; }
    bool is_enabled() const { return m_enabled; }

    void process_block_512(const float* in_stereo, float* v_out, float* b_out, float* i_out) {
        if (!m_enabled) {
            std::memcpy(v_out, in_stereo, HOP_SIZE * 2 * sizeof(float));
            std::memcpy(b_out, in_stereo, HOP_SIZE * 2 * sizeof(float));
            if (i_out) std::memcpy(i_out, in_stereo, HOP_SIZE * 2 * sizeof(float));
            return;
        }

        for (size_t i = 0; i < HOP_SIZE; ++i) {
            m_fft_l[i] = { m_prev_in_l[i] * m_window[i], 0.0f };
            m_fft_r[i] = { m_prev_in_r[i] * m_window[i], 0.0f };
            float l = in_stereo[i * 2];
            float r = in_stereo[i * 2 + 1];
            m_fft_l[HOP_SIZE + i] = { l * m_window[HOP_SIZE + i], 0.0f };
            m_fft_r[HOP_SIZE + i] = { r * m_window[HOP_SIZE + i], 0.0f };
            m_prev_in_l[i] = l;
            m_prev_in_r[i] = r;
        }

        EmbeddedFFT<FFT_SIZE>::fft(m_fft_l.data(), false);
        EmbeddedFFT<FFT_SIZE>::fft(m_fft_r.data(), false);

        float bin_hz = static_cast<float>(m_sample_rate) / FFT_SIZE;
        size_t half = FFT_SIZE / 2;

        for (size_t k = 0; k <= half; ++k) {
            float freq = k * bin_hz;
            ComplexF L = m_fft_l[k];
            ComplexF R = m_fft_r[k];
            ComplexF M = { 0.5f * (L.re + R.re), 0.5f * (L.im + R.im) };
            ComplexF S = { 0.5f * (L.re - R.re), 0.5f * (L.im - R.im) };

            float mag_m = std::sqrt(M.re * M.re + M.im * M.im) + 1e-12f;
            float mag_s = std::sqrt(S.re * S.re + S.im * S.im) + 1e-12f;

            float w_beat = 0.0f;
            if (freq < 180.0f) w_beat = 1.0f;
            else if (freq < 260.0f) w_beat = 1.0f - (freq - 180.0f) / 80.0f;

            float w_vocal = 0.0f;
            if (freq >= 220.0f && freq <= 4500.0f) {
                float ratio = mag_m / (mag_m + mag_s * 1.4f);
                float center_dom = mcu_clamp((ratio - 0.52f) / 0.30f, 0.0f, 1.0f);
                float f_env = 1.0f;
                if (freq < 320.0f) f_env = (freq - 220.0f) / 100.0f;
                else if (freq > 3500.0f) f_env = (4500.0f - freq) / 1000.0f;
                w_vocal = center_dom * f_env * (1.0f - w_beat);
            }

            ComplexF vl = { M.re * w_vocal, M.im * w_vocal };
            ComplexF vr = vl;
            ComplexF bl = { L.re * w_beat, L.im * w_beat };
            ComplexF br = { R.re * w_beat, R.im * w_beat };
            ComplexF il = { L.re - vl.re - bl.re, L.im - vl.im - bl.im };
            ComplexF ir = { R.re - vr.re - br.re, R.im - vr.im - br.im };

            m_voc_l[k] = vl; m_voc_r[k] = vr;
            m_b_l[k] = bl;   m_b_r[k] = br;
            m_in_l[k] = il;  m_in_r[k] = ir;

            if (k > 0 && k < half) {
                size_t mk = FFT_SIZE - k;
                m_voc_l[mk] = { vl.re, -vl.im }; m_voc_r[mk] = { vr.re, -vr.im };
                m_b_l[mk]   = { bl.re, -bl.im }; m_b_r[mk]   = { br.re, -br.im };
                m_in_l[mk]  = { il.re, -il.im }; m_in_r[mk]  = { ir.re, -ir.im };
            }
        }

        EmbeddedFFT<FFT_SIZE>::fft(m_voc_l.data(), true);
        EmbeddedFFT<FFT_SIZE>::fft(m_voc_r.data(), true);
        EmbeddedFFT<FFT_SIZE>::fft(m_b_l.data(), true);
        EmbeddedFFT<FFT_SIZE>::fft(m_b_r.data(), true);
        EmbeddedFFT<FFT_SIZE>::fft(m_in_l.data(), true);
        EmbeddedFFT<FFT_SIZE>::fft(m_in_r.data(), true);

        for (size_t i = 0; i < HOP_SIZE; ++i) {
            float w0 = m_window[i];
            float w1 = m_window[HOP_SIZE + i];
            v_out[i * 2]     = m_tail_v_l[i] + m_voc_l[i].re * w0;
            v_out[i * 2 + 1] = m_tail_v_r[i] + m_voc_r[i].re * w0;
            b_out[i * 2]     = m_tail_b_l[i] + m_b_l[i].re * w0;
            b_out[i * 2 + 1] = m_tail_b_r[i] + m_b_r[i].re * w0;
            if (i_out) {
                i_out[i * 2]     = m_tail_i_l[i] + m_in_l[i].re * w0;
                i_out[i * 2 + 1] = m_tail_i_r[i] + m_in_r[i].re * w0;
            }
            m_tail_v_l[i] = m_voc_l[HOP_SIZE + i].re * w1;
            m_tail_v_r[i] = m_voc_r[HOP_SIZE + i].re * w1;
            m_tail_b_l[i] = m_b_l[HOP_SIZE + i].re * w1;
            m_tail_b_r[i] = m_b_r[HOP_SIZE + i].re * w1;
            m_tail_i_l[i] = m_in_l[HOP_SIZE + i].re * w1;
            m_tail_i_r[i] = m_in_r[HOP_SIZE + i].re * w1;
        }
    }

private:
    int m_sample_rate;
    bool m_enabled;
    std::array<float, FFT_SIZE> m_window;
    std::array<float, HOP_SIZE> m_prev_in_l, m_prev_in_r;
    std::array<float, HOP_SIZE> m_tail_v_l, m_tail_v_r;
    std::array<float, HOP_SIZE> m_tail_b_l, m_tail_b_r;
    std::array<float, HOP_SIZE> m_tail_i_l, m_tail_i_r;
    std::array<ComplexF, FFT_SIZE> m_fft_l, m_fft_r;
    std::array<ComplexF, FFT_SIZE> m_voc_l, m_voc_r;
    std::array<ComplexF, FFT_SIZE> m_b_l, m_b_r;
    std::array<ComplexF, FFT_SIZE> m_in_l, m_in_r;
};

// ============================================================================
// 6. S-FR-5: MCU 온보드 라우드니스 매치 (K-Weighting + EMA + 피크 리미터)
// ============================================================================
class EmbeddedLoudness {
public:
    static constexpr size_t MAX_BLOCK = 512;

    EmbeddedLoudness(int sample_rate = 44100, float target_lufs = -14.0f)
        : m_enabled(true), m_target_lufs(target_lufs),
          m_alpha(0.05f), m_current_gain(1.0f), m_measured_lufs(-70.0f),
          m_k_shelf(BiquadType::Peaking, sample_rate, 1680.0f, 0.7f, 4.0f),
          m_k_hp(BiquadType::Highpass, sample_rate, 100.0f, 0.5f, 0.0f) {
        m_limiter_thresh = std::pow(10.0f, -0.5f / 20.0f);
    }

    void set_enabled(bool en) { m_enabled = en; }
    bool is_enabled() const { return m_enabled; }
    void set_target_lufs(float t) { m_target_lufs = mcu_clamp(t, -24.0f, -8.0f); }
    float get_target_lufs() const { return m_target_lufs; }
    float get_measured_lufs() const { return m_measured_lufs; }
    float get_current_gain_db() const { return 20.0f * std::log10(m_current_gain + 1e-6f); }

    void process(float* stereo_buffer, size_t num_frames) {
        if (!m_enabled || num_frames == 0 || num_frames > MAX_BLOCK) return;

        std::memcpy(m_temp.data(), stereo_buffer, num_frames * 2 * sizeof(float));
        m_k_shelf.process(m_temp.data(), num_frames);
        m_k_hp.process(m_temp.data(), num_frames);

        double sum_sq = 0.0;
        for (size_t i = 0; i < num_frames * 2; ++i) {
            sum_sq += static_cast<double>(m_temp[i]) * m_temp[i];
        }
        double mean_sq = sum_sq / static_cast<double>(num_frames * 2);
        m_measured_lufs = (mean_sq <= 1e-10)
            ? -70.0f
            : mcu_clamp(static_cast<float>(-0.691 + 10.0 * std::log10(mean_sq)), -70.0f, 0.0f);

        float target_gain = 1.0f;
        if (m_measured_lufs > -50.0f) {
            float diff_db = mcu_clamp(m_target_lufs - m_measured_lufs, -24.0f, 12.0f);
            target_gain = std::pow(10.0f, diff_db / 20.0f);
        }

        m_current_gain = (1.0f - m_alpha) * m_current_gain + m_alpha * target_gain;

        float max_peak = 0.0f;
        for (size_t i = 0; i < num_frames * 2; ++i) {
            float s = stereo_buffer[i] * m_current_gain;
            stereo_buffer[i] = s;
            float abs_s = std::fabs(s);
            if (abs_s > max_peak) max_peak = abs_s;
        }

        if (max_peak > m_limiter_thresh) {
            float scale = m_limiter_thresh / (max_peak + 1e-6f);
            for (size_t i = 0; i < num_frames * 2; ++i) {
                stereo_buffer[i] *= scale;
            }
        }
    }

private:
    bool m_enabled;
    float m_target_lufs;
    float m_alpha;
    float m_limiter_thresh;
    float m_current_gain;
    float m_measured_lufs;
    EmbeddedBiquad m_k_shelf;
    EmbeddedBiquad m_k_hp;
    std::array<float, MAX_BLOCK * 2> m_temp;
};

// ============================================================================
// 7. S-FR-1 / S-FR-7: 5채널 안티-팝 Gain Ramping 믹서 & 3-Band EQ
// ============================================================================
class EmbeddedMixer {
public:
    static constexpr size_t NUM_CHANNELS = 5;
    static constexpr size_t MAX_BLOCK = 512;

    EmbeddedMixer(int sample_rate = 44100) {
        m_gain.fill(1.0f);
        m_target_gain.fill(1.0f);
        m_muted.fill(false);
        m_eq_low.configure(BiquadType::Peaking, sample_rate, 120.0f, 0.7f, 0.0f);
        m_eq_mid.configure(BiquadType::Peaking, sample_rate, 1000.0f, 1.0f, 0.0f);
        m_eq_high.configure(BiquadType::Peaking, sample_rate, 6000.0f, 0.7f, 0.0f);
    }

    void set_fader(int ch, float pos) {
        if (ch < 0 || ch >= static_cast<int>(NUM_CHANNELS)) return;
        pos = mcu_clamp(pos, 0.0f, 1.0f);
        if (pos < 0.001f) {
            m_target_gain[ch] = 0.0f;
        } else {
            float db = (pos <= 0.75f) ? (-60.0f + (pos / 0.75f) * 60.0f)
                                      : (((pos - 0.75f) / 0.25f) * 6.0f);
            m_target_gain[ch] = std::pow(10.0f, db / 20.0f);
        }
    }

    void set_mute(int ch, bool muted) {
        if (ch >= 0 && ch < static_cast<int>(NUM_CHANNELS)) m_muted[ch] = muted;
    }

    void set_master_eq_db(float low_db, float mid_db, float high_db) {
        m_eq_low.set_gain_db(mcu_clamp(low_db, -12.0f, 12.0f));
        m_eq_mid.set_gain_db(mcu_clamp(mid_db, -12.0f, 12.0f));
        m_eq_high.set_gain_db(mcu_clamp(high_db, -12.0f, 12.0f));
    }

    void mix(const float* const ch_inputs[NUM_CHANNELS], float* master_out, size_t num_frames) {
        if (num_frames == 0 || num_frames > MAX_BLOCK) return;
        std::memset(master_out, 0, num_frames * 2 * sizeof(float));

        // CH1 ~ CH4 합산 (각 채널 선형 Gain Ramp 적용 -> 팝 노이즈 원천 차단)
        for (size_t ch = 1; ch < NUM_CHANNELS; ++ch) {
            if (m_muted[ch] || !ch_inputs[ch]) continue;
            float g = m_gain[ch];
            float step = (m_target_gain[ch] - g) / static_cast<float>(num_frames);
            for (size_t i = 0; i < num_frames; ++i) {
                g += step;
                master_out[i * 2]     += ch_inputs[ch][i * 2] * g;
                master_out[i * 2 + 1] += ch_inputs[ch][i * 2 + 1] * g;
            }
            m_gain[ch] = m_target_gain[ch];
        }

        // 마스터 3-Band EQ 적용
        m_eq_low.process(master_out, num_frames);
        m_eq_mid.process(master_out, num_frames);
        m_eq_high.process(master_out, num_frames);

        // CH0 Master 페이더 적용
        if (m_muted[0]) {
            std::memset(master_out, 0, num_frames * 2 * sizeof(float));
        } else {
            float g0 = m_gain[0];
            float step0 = (m_target_gain[0] - g0) / static_cast<float>(num_frames);
            for (size_t i = 0; i < num_frames; ++i) {
                g0 += step0;
                master_out[i * 2]     *= g0;
                master_out[i * 2 + 1] *= g0;
            }
            m_gain[0] = m_target_gain[0];
        }
    }

    float get_gain_linear(int ch) const {
        return (ch >= 0 && ch < static_cast<int>(NUM_CHANNELS)) ? m_gain[ch] : 1.0f;
    }

private:
    std::array<float, NUM_CHANNELS> m_gain;
    std::array<float, NUM_CHANNELS> m_target_gain;
    std::array<bool, NUM_CHANNELS> m_muted;
    EmbeddedBiquad m_eq_low;
    EmbeddedBiquad m_eq_mid;
    EmbeddedBiquad m_eq_high;
};

// ============================================================================
// 8. S-FR-2 & S-FR-7: 하드웨어 ADC 스무딩 필터 & 25ms 스위치 디바운서
// ============================================================================
class EmbeddedSmoothingFilter {
public:
    EmbeddedSmoothingFilter(float alpha = 0.30f)
        : m_alpha(alpha), m_val(0.0f), m_init(false) {}

    void reset(float v) { m_val = v; m_init = true; }

    float update(float raw) {
        if (!m_init) {
            m_val = raw;
            m_init = true;
        } else {
            m_val = (1.0f - m_alpha) * m_val + m_alpha * raw;
        }
        return m_val;
    }

private:
    float m_alpha;
    float m_val;
    bool m_init;
};

enum class McuButtonEvent : uint8_t {
    None = 0,
    Click,
    LongPress,
    Release
};

class EmbeddedButtonDebouncer {
public:
    EmbeddedButtonDebouncer(uint32_t debounce_ms = 25, uint32_t long_press_ms = 600)
        : m_debounce_ms(debounce_ms), m_long_press_ms(long_press_ms),
          m_last_raw(false), m_stable(false),
          m_last_change_ms(0), m_press_start_ms(0), m_long_fired(false) {}

    McuButtonEvent update(bool raw_pressed, uint32_t now_ms) {
        McuButtonEvent ev = McuButtonEvent::None;
        if (raw_pressed != m_last_raw) {
            m_last_change_ms = now_ms;
            m_last_raw = raw_pressed;
        }
        if ((now_ms - m_last_change_ms) >= m_debounce_ms) {
            if (raw_pressed != m_stable) {
                m_stable = raw_pressed;
                if (m_stable) {
                    m_press_start_ms = now_ms;
                    m_long_fired = false;
                } else {
                    ev = (!m_long_fired) ? McuButtonEvent::Click : McuButtonEvent::Release;
                }
            }
        }
        if (m_stable && !m_long_fired && (now_ms - m_press_start_ms) >= m_long_press_ms) {
            m_long_fired = true;
            ev = McuButtonEvent::LongPress;
        }
        return ev;
    }

private:
    uint32_t m_debounce_ms;
    uint32_t m_long_press_ms;
    bool m_last_raw;
    bool m_stable;
    uint32_t m_last_change_ms;
    uint32_t m_press_start_ms;
    bool m_long_fired;
};

// ============================================================================
// 9. S-FR-6: MCU 온보드 16밴드 FFT 스펙트럼 & 128x64 SSD1306 OLED 프레임버퍼
// ============================================================================
class EmbeddedOledRenderer {
public:
    static constexpr size_t NUM_BARS = 16;
    static constexpr size_t FFT_SIZE = 1024;

    EmbeddedOledRenderer(int sample_rate = 44100) {
        for (size_t i = 0; i < FFT_SIZE; ++i) {
            m_window[i] = 0.5f * (1.0f - std::cos(2.0f * MCU_PI * i / (FFT_SIZE - 1)));
        }
        float bin_res = static_cast<float>(sample_rate) / FFT_SIZE;
        float log_min = std::log10(100.0f);
        float log_max = std::log10(12000.0f);
        float step = (log_max - log_min) / NUM_BARS;

        for (size_t i = 0; i < NUM_BARS; ++i) {
            float f0 = std::pow(10.0f, log_min + i * step);
            float f1 = std::pow(10.0f, log_min + (i + 1) * step);
            size_t b0 = static_cast<size_t>(f0 / bin_res);
            size_t b1 = static_cast<size_t>(f1 / bin_res);
            b0 = mcu_clamp<size_t>(b0, 1, FFT_SIZE / 2 - 1);
            b1 = mcu_clamp<size_t>(b1, b0 + 1, FFT_SIZE / 2);
            m_band_start[i] = b0;
            m_band_end[i] = b1;
            m_bars[i] = 0.0f;
        }
        m_framebuffer.fill(0);
    }

    void compute_spectrum(const float* stereo_buf, size_t num_frames) {
        size_t n = std::min(num_frames, FFT_SIZE);
        for (size_t i = 0; i < n; ++i) {
            float mono = 0.5f * (stereo_buf[i * 2] + stereo_buf[i * 2 + 1]);
            m_fft_buf[i] = { mono * m_window[i], 0.0f };
        }
        for (size_t i = n; i < FFT_SIZE; ++i) {
            m_fft_buf[i] = { 0.0f, 0.0f };
        }

        EmbeddedFFT<FFT_SIZE>::fft(m_fft_buf.data(), false);

        for (size_t i = 0; i < NUM_BARS; ++i) {
            float sum_mag = 0.0f;
            for (size_t b = m_band_start[i]; b < m_band_end[i]; ++b) {
                sum_mag += std::sqrt(m_fft_buf[b].re * m_fft_buf[b].re + m_fft_buf[b].im * m_fft_buf[b].im);
            }
            float avg_mag = sum_mag / static_cast<float>(m_band_end[i] - m_band_start[i]) + 1e-12f;
            float norm = (20.0f * std::log10(avg_mag) + 60.0f) / 60.0f;
            norm = mcu_clamp(norm, 0.0f, 1.0f);
            m_bars[i] = std::max(norm, m_bars[i] * 0.85f);
        }
    }

    void render_framebuffer(bool afs_on, bool stem_on, bool loud_on, bool howling_alert) {
        m_framebuffer.fill(0);

        // 상단 상태바 (y: 0 ~ 7, Page 0): 기능 아이콘 배지 인디케이터
        if (afs_on)  fill_rect(2,  1, 18, 6);
        if (stem_on) fill_rect(22, 1, 38, 6);
        if (loud_on) fill_rect(42, 1, 58, 6);
        if (howling_alert) {
            // 우측 상단 깜박이는 하울링 억제 경고 박스
            fill_rect(76, 1, 126, 6);
        }

        // 하단 16밴드 스펙트럼 막대 (y: 12 ~ 63)
        for (size_t i = 0; i < NUM_BARS; ++i) {
            int bar_h = static_cast<int>(m_bars[i] * 50.0f);
            int x0 = static_cast<int>(i * 8 + 1);
            int x1 = x0 + 5;
            for (int y = 63; y > 63 - bar_h; --y) {
                for (int x = x0; x <= x1; ++x) {
                    set_pixel(x, y, true);
                }
            }
        }
    }

    const uint8_t* get_framebuffer() const { return m_framebuffer.data(); }
    const float* get_bars() const { return m_bars.data(); }

private:
    void set_pixel(int x, int y, bool on) {
        if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) return;
        size_t idx = static_cast<size_t>(x) + (static_cast<size_t>(y) / 8) * OLED_WIDTH;
        uint8_t mask = static_cast<uint8_t>(1u << (y & 7));
        if (on) m_framebuffer[idx] |= mask;
        else    m_framebuffer[idx] &= static_cast<uint8_t>(~mask);
    }

    void fill_rect(int x0, int y0, int x1, int y1) {
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) set_pixel(x, y, true);
        }
    }

    std::array<float, FFT_SIZE> m_window;
    std::array<ComplexF, FFT_SIZE> m_fft_buf;
    std::array<size_t, NUM_BARS> m_band_start;
    std::array<size_t, NUM_BARS> m_band_end;
    std::array<float, NUM_BARS> m_bars;
    std::array<uint8_t, OLED_FRAMEBUFFER_BYTES> m_framebuffer;
};

} // namespace SmartMixerMCU

