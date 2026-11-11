#pragma once

#include <vector>
#include <string>
#include <functional>
#include <algorithm>
#include <cmath>

namespace SmartMixer {

class InputSmoothingFilter {
public:
    InputSmoothingFilter(float alpha = 0.25f)
        : m_alpha(alpha), m_filtered_val(0.0f), m_initialized(false) {}

    void reset(float val) {
        m_filtered_val = val;
        m_initialized = true;
    }

    float update(float raw_val) {
        if (!m_initialized) {
            m_filtered_val = raw_val;
            m_initialized = true;
        } else {
            m_filtered_val = (1.0f - m_alpha) * m_filtered_val + m_alpha * raw_val;
        }
        return m_filtered_val;
    }

private:
    float m_alpha;
    float m_filtered_val;
    bool m_initialized;
};

class ParameterManager {
public:
    using ParamCallback = std::function<void(const std::string&, float)>;

    ParameterManager()
        : m_faders(5, 0.8f),
          m_afs_enabled(true),
          m_stem_enabled(true),
          m_loudness_enabled(true),
          m_encoder_val(0) {
        for (int i = 0; i < 5; ++i) {
            m_fader_filters.emplace_back(0.3f);
        }
    }

    void set_callback(ParamCallback cb) { m_callback = cb; }

    void update_fader_adc(int ch, int raw_adc, int max_adc = 1023) {
        if (ch >= 0 && ch < 5) {
            float norm = std::clamp(static_cast<float>(raw_adc) / max_adc, 0.0f, 1.0f);
            float smoothed = m_fader_filters[ch].update(norm);
            m_faders[ch] = smoothed;
            if (m_callback) m_callback("fader_" + std::to_string(ch), smoothed);
        }
    }

    void set_fader_direct(int ch, float norm) {
        if (ch >= 0 && ch < 5) {
            norm = std::clamp(norm, 0.0f, 1.0f);
            m_faders[ch] = norm;
            m_fader_filters[ch].reset(norm);
            if (m_callback) m_callback("fader_" + std::to_string(ch), norm);
        }
    }

    uint16_t get_fader_adc(int ch) const {
        if (ch >= 0 && ch < 5) {
            return static_cast<uint16_t>(std::clamp(m_faders[ch], 0.0f, 1.0f) * 1023.0f);
        }
        return 820;
    }

    bool toggle_afs() {
        m_afs_enabled = !m_afs_enabled;
        if (m_callback) m_callback("afs_enabled", m_afs_enabled ? 1.0f : 0.0f);
        return m_afs_enabled;
    }

    bool toggle_stem() {
        m_stem_enabled = !m_stem_enabled;
        if (m_callback) m_callback("stem_enabled", m_stem_enabled ? 1.0f : 0.0f);
        return m_stem_enabled;
    }

    bool toggle_loudness() {
        m_loudness_enabled = !m_loudness_enabled;
        if (m_callback) m_callback("loudness_enabled", m_loudness_enabled ? 1.0f : 0.0f);
        return m_loudness_enabled;
    }

    void update_encoder(int delta) {
        m_encoder_val += delta;
        if (m_callback) m_callback("encoder", static_cast<float>(m_encoder_val));
    }

    float get_fader(int ch) const { return m_faders[ch]; }
    bool is_afs_enabled() const { return m_afs_enabled; }
    bool is_stem_enabled() const { return m_stem_enabled; }
    bool is_loudness_enabled() const { return m_loudness_enabled; }

private:
    std::vector<float> m_faders;
    std::vector<InputSmoothingFilter> m_fader_filters;
    bool m_afs_enabled;
    bool m_stem_enabled;
    bool m_loudness_enabled;
    int m_encoder_val;
    ParamCallback m_callback;
};

} // namespace SmartMixer

