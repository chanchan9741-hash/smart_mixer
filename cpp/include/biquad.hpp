#pragma once

#include <cmath>
#include <vector>
#include <algorithm>

namespace SmartMixer {

enum class FilterType {
    Notch,
    Peaking,
    Highpass,
    Lowpass,
    Passthrough
};

class BiquadFilter {
public:
    BiquadFilter(FilterType type = FilterType::Notch, int sample_rate = 44100,
                 float freq = 1000.0f, float q = 10.0f, float gain_db = -18.0f)
        : m_type(type), m_sample_rate(sample_rate), m_freq(freq), m_q(std::max(0.1f, q)),
          m_gain_db(gain_db), m_b0(1.0f), m_b1(0.0f), m_b2(0.0f),
          m_a1(0.0f), m_a2(0.0f), m_z1_l(0.0f), m_z2_l(0.0f),
          m_z1_r(0.0f), m_z2_r(0.0f) {
        compute_coefficients();
    }

    void set_params(float freq, float q = -1.0f, float gain_db = 0.0f) {
        m_freq = freq;
        if (q > 0.0f) m_q = q;
        m_gain_db = gain_db;
        compute_coefficients();
    }

    void compute_coefficients() {
        constexpr float PI = 3.14159265358979323846f;
        float f0 = std::clamp(m_freq, 20.0f, m_sample_rate * 0.49f);
        float w0 = 2.0f * PI * f0 / m_sample_rate;
        float cos_w0 = std::cos(w0);
        float sin_w0 = std::sin(w0);
        float alpha = sin_w0 / (2.0f * m_q);

        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
        float a0 = 1.0f, a1 = 0.0f, a2 = 0.0f;

        switch (m_type) {
            case FilterType::Notch:
                b0 = 1.0f;
                b1 = -2.0f * cos_w0;
                b2 = 1.0f;
                a0 = 1.0f + alpha;
                a1 = -2.0f * cos_w0;
                a2 = 1.0f - alpha;
                break;
            case FilterType::Peaking: {
                float A = std::pow(10.0f, m_gain_db / 40.0f);
                b0 = 1.0f + alpha * A;
                b1 = -2.0f * cos_w0;
                b2 = 1.0f - alpha * A;
                a0 = 1.0f + alpha / A;
                a1 = -2.0f * cos_w0;
                a2 = 1.0f - alpha / A;
                break;
            }
            case FilterType::Highpass:
                b0 = (1.0f + cos_w0) * 0.5f;
                b1 = -(1.0f + cos_w0);
                b2 = (1.0f + cos_w0) * 0.5f;
                a0 = 1.0f + alpha;
                a1 = -2.0f * cos_w0;
                a2 = 1.0f - alpha;
                break;
            default:
                break;
        }

        m_b0 = b0 / a0;
        m_b1 = b1 / a0;
        m_b2 = b2 / a0;
        m_a1 = a1 / a0;
        m_a2 = a2 / a0;
    }

    // Direct Form II Transposed 스테레오 블록 필터링
    void process(float* buffer, size_t num_frames) {
        for (size_t i = 0; i < num_frames; ++i) {
            // Left Channel
            float xn_l = buffer[i * 2];
            float yn_l = m_b0 * xn_l + m_z1_l;
            m_z1_l = m_b1 * xn_l - m_a1 * yn_l + m_z2_l;
            m_z2_l = m_b2 * xn_l - m_a2 * yn_l;
            buffer[i * 2] = yn_l;

            // Right Channel
            float xn_r = buffer[i * 2 + 1];
            float yn_r = m_b0 * xn_r + m_z1_r;
            m_z1_r = m_b1 * xn_r - m_a1 * yn_r + m_z2_r;
            m_z2_r = m_b2 * xn_r - m_a2 * yn_r;
            buffer[i * 2 + 1] = yn_r;
        }
    }

    float get_freq() const { return m_freq; }

    void reset() {
        m_z1_l = m_z2_l = 0.0f;
        m_z1_r = m_z2_r = 0.0f;
    }

private:
    FilterType m_type;
    int m_sample_rate;
    float m_freq;
    float m_q;
    float m_gain_db;

    float m_b0, m_b1, m_b2;
    float m_a1, m_a2;
    float m_z1_l, m_z2_l;
    float m_z1_r, m_z2_r;
};

} // namespace SmartMixer

