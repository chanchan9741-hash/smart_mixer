#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include "config.hpp"
#include "biquad.hpp"

namespace SmartMixer {

class KWeightingFilter {
public:
    KWeightingFilter(int sample_rate = 44100)
        : m_shelf(FilterType::Peaking, sample_rate, 1680.0f, 0.7f, 4.0f),
          m_hp(FilterType::Highpass, sample_rate, 100.0f, 0.5f, 0.0f) {}

    void process(float* buffer, size_t num_frames) {
        m_shelf.process(buffer, num_frames);
        m_hp.process(buffer, num_frames);
    }

private:
    BiquadFilter m_shelf;
    BiquadFilter m_hp;
};

class LoudnessMatcher {
public:
    LoudnessMatcher(int sample_rate = 44100, const LoudnessConfig& config = LoudnessConfig())
        : m_sample_rate(sample_rate), m_config(config),
          m_k_filter(sample_rate),
          m_current_linear_gain(1.0f),
          m_measured_lufs(-70.0f),
          m_measured_rms_db(-70.0f) {
        m_limiter_linear_threshold = std::pow(10.0f, m_config.limiter_threshold_db / 20.0f);
    }

    void set_enabled(bool enabled) { m_config.enabled = enabled; }
    bool is_enabled() const { return m_config.enabled; }

    void set_target_lufs(float lufs) { m_config.target_lufs = lufs; }
    float get_target_lufs() const { return m_config.target_lufs; }

    float measure_lufs(const float* stereo_buffer, size_t num_frames) {
        if (!stereo_buffer || num_frames == 0) return -70.0f;

        std::vector<float> temp(stereo_buffer, stereo_buffer + num_frames * 2);
        m_k_filter.process(temp.data(), num_frames);

        double sum_sq = 0.0;
        for (size_t i = 0; i < num_frames * 2; ++i) {
            sum_sq += static_cast<double>(temp[i]) * temp[i];
        }
        double mean_sq = sum_sq / (num_frames * 2);

        if (mean_sq <= 1e-10) return -70.0f;

        float lufs = static_cast<float>(-0.691 + 10.0 * std::log10(mean_sq));
        return std::clamp(lufs, -70.0f, 0.0f);
    }

    float measure_rms_db(const float* stereo_buffer, size_t num_frames) {
        double sum_sq = 0.0;
        for (size_t i = 0; i < num_frames * 2; ++i) {
            sum_sq += static_cast<double>(stereo_buffer[i]) * stereo_buffer[i];
        }
        double rms = std::sqrt(sum_sq / (num_frames * 2)) + 1e-12;
        return static_cast<float>(20.0 * std::log10(rms));
    }

    void process(float* stereo_buffer, size_t num_frames) {
        if (!m_config.enabled || !stereo_buffer || num_frames == 0) return;

        // 1. 측정
        m_measured_lufs = measure_lufs(stereo_buffer, num_frames);
        m_measured_rms_db = measure_rms_db(stereo_buffer, num_frames);

        // 2. 보정 목표 게인 산출
        float target_linear_gain = 1.0f;
        if (m_measured_lufs > -50.0f) {
            float diff_db = m_config.target_lufs - m_measured_lufs;
            float clamped_db = std::clamp(diff_db, m_config.max_cut_db, m_config.max_boost_db);
            target_linear_gain = std::pow(10.0f, clamped_db / 20.0f);
        }

        // 3. 지수 이동 평균 스무딩 (볼륨 펌핑 방지)
        float alpha = m_config.gain_smoothing_alpha;
        m_current_linear_gain = (1.0f - alpha) * m_current_linear_gain + alpha * target_linear_gain;

        // 4. 게인 적용
        for (size_t i = 0; i < num_frames * 2; ++i) {
            stereo_buffer[i] *= m_current_linear_gain;
        }

        // 5. 피크 리미터 (Anti-clipping)
        float max_peak = 0.0f;
        for (size_t i = 0; i < num_frames * 2; ++i) {
            max_peak = std::max(max_peak, std::abs(stereo_buffer[i]));
        }

        if (max_peak > m_limiter_linear_threshold) {
            float limiter_scale = m_limiter_linear_threshold / (max_peak + 1e-6f);
            for (size_t i = 0; i < num_frames * 2; ++i) {
                stereo_buffer[i] *= limiter_scale;
            }
        }
    }

    float get_measured_lufs() const { return m_measured_lufs; }
    float get_measured_rms_db() const { return m_measured_rms_db; }
    float get_current_gain_db() const {
        return 20.0f * std::log10(std::max(m_current_linear_gain, 1e-6f));
    }

private:
    int m_sample_rate;
    LoudnessConfig m_config;
    KWeightingFilter m_k_filter;
    float m_limiter_linear_threshold;
    float m_current_linear_gain;
    float m_measured_lufs;
    float m_measured_rms_db;
};

} // namespace SmartMixer

