#pragma once

#include <vector>
#include <complex>
#include <cmath>
#include <unordered_map>
#include <functional>
#include <algorithm>
#include <memory>
#include "config.hpp"
#include "biquad.hpp"

namespace SmartMixer {

// 고속 Radix-2 FFT (Zero-dependency)
class FastFFT {
public:
    static void fft(std::vector<std::complex<float>>& a) {
        size_t n = a.size();
        if (n <= 1) return;

        // Bit-reversal permutation
        for (size_t i = 1, j = 0; i < n; ++i) {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) {
                j ^= bit;
            }
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }

        // Cooley-Tukey butterfly
        constexpr float PI = 3.14159265358979323846f;
        for (size_t len = 2; len <= n; len <<= 1) {
            float ang = -2.0f * PI / len;
            std::complex<float> wlen(std::cos(ang), std::sin(ang));
            for (size_t i = 0; i < n; i += len) {
                std::complex<float> w(1.0f, 0.0f);
                for (size_t j = 0; j < len / 2; ++j) {
                    std::complex<float> u = a[i + j];
                    std::complex<float> v = a[i + j + len / 2] * w;
                    a[i + j] = u + v;
                    a[i + j + len / 2] = u - v;
                    w *= wlen;
                }
            }
        }
    }
};

class FeedbackDetector {
public:
    FeedbackDetector(int sample_rate, int fft_size, float peak_threshold_db, int persistence)
        : m_sample_rate(sample_rate), m_fft_size(fft_size),
          m_peak_threshold_db(peak_threshold_db), m_persistence(persistence),
          m_bin_res(static_cast<float>(sample_rate) / fft_size),
          m_window(fft_size), m_fft_buffer(fft_size) {
        constexpr float PI = 3.14159265358979323846f;
        for (int i = 0; i < fft_size; ++i) {
            m_window[i] = 0.5f * (1.0f - std::cos(2.0f * PI * i / (fft_size - 1)));
        }
    }

    std::vector<float> analyze(const float* mono_signal, size_t num_samples) {
        std::vector<float> detected_freqs;
        if (num_samples < static_cast<size_t>(m_fft_size)) return detected_freqs;

        for (int i = 0; i < m_fft_size; ++i) {
            m_fft_buffer[i] = std::complex<float>(mono_signal[i] * m_window[i], 0.0f);
        }

        FastFFT::fft(m_fft_buffer);

        int half_n = m_fft_size / 2;
        std::vector<float> mag_db(half_n);
        for (int i = 0; i < half_n; ++i) {
            float mag = std::abs(m_fft_buffer[i]) + 1e-12f;
            mag_db[i] = 20.0f * std::log10(mag);
        }

        int min_bin = static_cast<int>(150.0f / m_bin_res);
        int max_bin = static_cast<int>(8000.0f / m_bin_res);
        max_bin = std::min(max_bin, half_n - 2);

        // Moving Average 스펙트럼 기준선
        constexpr int KERNEL = 15;
        std::vector<float> smoothed(half_n);
        for (int i = 0; i < half_n; ++i) {
            float sum = 0.0f;
            int count = 0;
            for (int k = -KERNEL; k <= KERNEL; ++k) {
                int idx = i + k;
                if (idx >= 0 && idx < half_n) {
                    sum += mag_db[idx];
                    count++;
                }
            }
            smoothed[i] = sum / count;
        }

        std::vector<int> current_peaks;
        for (int b = min_bin; b <= max_bin; ++b) {
            if (mag_db[b] > mag_db[b - 1] && mag_db[b] > mag_db[b + 1]) {
                float prom = mag_db[b] - smoothed[b];
                if (prom > m_peak_threshold_db && mag_db[b] > -40.0f) {
                    current_peaks.push_back(b);
                }
            }
        }

        for (int b : current_peaks) {
            m_candidate_counts[b]++;
            if (m_candidate_counts[b] >= m_persistence) {
                detected_freqs.push_back(b * m_bin_res);
            }
        }

        // 미감지 후보 감쇄
        for (auto it = m_candidate_counts.begin(); it != m_candidate_counts.end();) {
            if (std::find(current_peaks.begin(), current_peaks.end(), it->first) == current_peaks.end()) {
                it->second--;
                if (it->second <= 0) {
                    it = m_candidate_counts.erase(it);
                    continue;
                }
            }
            ++it;
        }

        return detected_freqs;
    }

private:
    int m_sample_rate;
    int m_fft_size;
    float m_peak_threshold_db;
    int m_persistence;
    float m_bin_res;
    std::vector<float> m_window;
    std::vector<std::complex<float>> m_fft_buffer;
    std::unordered_map<int, int> m_candidate_counts;
};

class AutomaticFeedbackSuppression {
public:
    using HowlingCallback = std::function<void(float)>;

    AutomaticFeedbackSuppression(int sample_rate = 44100, const AFSConfig& config = AFSConfig(),
                                 HowlingCallback callback = nullptr)
        : m_sample_rate(sample_rate), m_config(config),
          m_callback(callback),
          m_detector(sample_rate, config.fft_size, config.peak_threshold_db, config.persistence),
          m_analysis_buf(config.fft_size, 0.0f), m_analysis_idx(0),
          m_suppressed_count(0), m_last_suppressed_freq(0.0f) {}

    void set_enabled(bool enabled) { m_config.enabled = enabled; }
    bool is_enabled() const { return m_config.enabled; }

    void process(float* stereo_buffer, size_t num_frames) {
        if (!m_config.enabled) return;

        // 1. Mono 신호 생성 후 FFT 버퍼 축적
        for (size_t i = 0; i < num_frames; ++i) {
            float mono = 0.5f * (stereo_buffer[i * 2] + stereo_buffer[i * 2 + 1]);
            m_analysis_buf[m_analysis_idx++] = mono;

            if (m_analysis_idx >= static_cast<size_t>(m_config.fft_size)) {
                run_detection();
                m_analysis_idx = 0;
            }
        }

        // 2. 할당된 동적 노치 필터들을 통과시켜 하울링 주파수 제거
        for (auto& filter : m_active_filters) {
            filter.process(stereo_buffer, num_frames);
        }
    }

    void run_detection() {
        auto freqs = m_detector.analyze(m_analysis_buf.data(), m_analysis_buf.size());
        for (float freq : freqs) {
            allocate_notch_filter(freq);
        }
    }

    void allocate_notch_filter(float freq) {
        // 이미 억제 중인 대역(±5%)인지 확인
        for (const auto& f : m_active_filters) {
            if (std::abs(f.get_freq() - freq) / freq < 0.05f) {
                return;
            }
        }

        BiquadFilter notch(FilterType::Notch, m_sample_rate, freq, m_config.notch_q, m_config.notch_attenuation_db);
        if (m_active_filters.size() >= static_cast<size_t>(m_config.max_notch_filters)) {
            m_active_filters.erase(m_active_filters.begin());
        }

        m_active_filters.push_back(notch);
        m_suppressed_count++;
        m_last_suppressed_freq = freq;

        if (m_callback) {
            m_callback(freq);
        }
    }

    void clear_filters() {
        m_active_filters.clear();
    }

    size_t get_active_filter_count() const { return m_active_filters.size(); }
    int get_suppressed_count() const { return m_suppressed_count; }
    float get_last_suppressed_freq() const { return m_last_suppressed_freq; }

    std::vector<float> get_notch_frequencies() const {
        std::vector<float> freqs;
        for (const auto& f : m_active_filters) {
            freqs.push_back(f.get_freq());
        }
        return freqs;
    }

private:
    int m_sample_rate;
    AFSConfig m_config;
    HowlingCallback m_callback;
    FeedbackDetector m_detector;
    std::vector<float> m_analysis_buf;
    size_t m_analysis_idx;
    std::vector<BiquadFilter> m_active_filters;
    int m_suppressed_count;
    float m_last_suppressed_freq;
};

} // namespace SmartMixer

