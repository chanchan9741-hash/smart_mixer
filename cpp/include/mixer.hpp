#pragma once

#include <vector>
#include <string>
#include <cmath>
#include <algorithm>

namespace SmartMixer {

class MixerChannel {
public:
    MixerChannel(const std::string& name = "Ch", int index = 0)
        : m_name(name), m_index(index),
          m_gain_linear(1.0f), m_target_gain_linear(1.0f),
          m_pan(0.0f), m_muted(false), m_solo(false) {}

    void set_fader_position(float pos) {
        pos = std::clamp(pos, 0.0f, 1.0f);
        if (pos < 0.001f) {
            m_target_gain_linear = 0.0f;
        } else {
            // 0.0 -> -60dB, 0.75 -> 0dB, 1.0 -> +6dB
            float db = (pos <= 0.75f) ? (-60.0f + (pos / 0.75f) * 60.0f)
                                      : (((pos - 0.75f) / 0.25f) * 6.0f);
            m_target_gain_linear = std::pow(10.0f, db / 20.0f);
        }
    }

    void set_mute(bool mute) { m_muted = mute; }
    bool is_muted() const { return m_muted; }

    void set_solo(bool solo) { m_solo = solo; }
    bool is_solo() const { return m_solo; }

    void set_pan(float pan) { m_pan = std::clamp(pan, -1.0f, 1.0f); }
    float get_pan() const { return m_pan; }

    float get_gain_linear() const { return m_gain_linear; }
    const std::string& get_name() const { return m_name; }

    // S-FR-7: 클릭 노이즈 방지 Gain Ramp 적용
    void process(const float* in_buffer, float* out_buffer, size_t num_frames) {
        if (m_muted) {
            std::fill(out_buffer, out_buffer + num_frames * 2, 0.0f);
            return;
        }

        constexpr float PI = 3.14159265358979323846f;
        float left_pan_gain = std::cos((m_pan + 1.0f) * PI / 4.0f);
        float right_pan_gain = std::sin((m_pan + 1.0f) * PI / 4.0f);

        float gain_step = (m_target_gain_linear - m_gain_linear) / static_cast<float>(num_frames);
        float current_g = m_gain_linear;

        for (size_t i = 0; i < num_frames; ++i) {
            current_g += gain_step;
            out_buffer[i * 2] = in_buffer[i * 2] * current_g * left_pan_gain;
            out_buffer[i * 2 + 1] = in_buffer[i * 2 + 1] * current_g * right_pan_gain;
        }

        m_gain_linear = m_target_gain_linear;
    }

private:
    std::string m_name;
    int m_index;
    float m_gain_linear;
    float m_target_gain_linear;
    float m_pan;
    bool m_muted;
    bool m_solo;
};

class AudioMixer {
public:
    AudioMixer(int num_channels = 5) : m_num_channels(num_channels) {
        const std::string names[] = {"Master", "Vocal Stem", "Beat Stem", "Mic In 1", "Line In 2"};
        for (int i = 0; i < num_channels; ++i) {
            std::string n = (i < 5) ? names[i] : ("Ch " + std::to_string(i));
            m_channels.emplace_back(n, i);
        }
    }

    MixerChannel& get_channel(int idx) { return m_channels[idx]; }
    const MixerChannel& get_channel(int idx) const { return m_channels[idx]; }

    void set_fader(int ch_idx, float pos) {
        if (ch_idx >= 0 && ch_idx < m_num_channels) {
            m_channels[ch_idx].set_fader_position(pos);
        }
    }

    void set_mute(int ch_idx, bool mute) {
        if (ch_idx >= 0 && ch_idx < m_num_channels) {
            m_channels[ch_idx].set_mute(mute);
        }
    }

    // 5채널 입력 버퍼 리스트를 합산하여 마스터 출력 산출
    void mix(const std::vector<const float*>& inputs, float* master_out, size_t num_frames) {
        std::fill(master_out, master_out + num_frames * 2, 0.0f);
        std::vector<float> temp_buf(num_frames * 2);

        bool any_solo = false;
        for (const auto& ch : m_channels) {
            if (ch.is_solo()) { any_solo = true; break; }
        }

        // CH1 ~ CH4 채널 처리 및 합산
        for (size_t idx = 1; idx < inputs.size() && idx < m_channels.size(); ++idx) {
            auto& ch = m_channels[idx];
            if (any_solo && !ch.is_solo()) continue;

            ch.process(inputs[idx], temp_buf.data(), num_frames);
            for (size_t i = 0; i < num_frames * 2; ++i) {
                master_out[i] += temp_buf[i];
            }
        }

        // CH0 (Master) 페이더 적용
        m_channels[0].process(master_out, temp_buf.data(), num_frames);
        std::copy(temp_buf.begin(), temp_buf.end(), master_out);
    }

private:
    int m_num_channels;
    std::vector<MixerChannel> m_channels;
};

} // namespace SmartMixer

