/**
 * @file stm32_mixer_app.hpp
 * @brief STM32H743VIT6 스마트 오디오 믹서 온보드 통합 펌웨어 애플리케이션
 *
 * 하드웨어에서 즉석(Hard Real-time < 5ms)으로 실행 가능한 모든 기능을 담당합니다:
 *  - 마이크/라인/USB 오디오 5채널 믹싱 & Gain Ramping 안티-팝
 *  - 마이크 입력(CH3) 실시간 AFS 하울링 자동 억제
 *  - K-Weighting 라우드니스 매치 & 피크 리미터
 *  - 3-Band 마스터 EQ (Low/Mid/High)
 *  - 단독 동작 시 실시간 STFT 스템 분리
 *  - Bourns 페이더/노브 ADC 스무딩 & 25ms 스위치 디바운싱
 *  - SSD1306 128x64 OLED 스펙트럼/하울링 알림 렌더링 & LED 구동
 *  - USB VCP 시리얼을 통한 PC 호스트(HTDemucs AI / GUI)와의 양방향 동기화
 */

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "hw_hal.h"
#include "embedded_dsp.hpp"

namespace SmartMixerMCU {

class Stm32MixerFirmware {
public:
    static constexpr size_t BLOCK_SIZE = 512;
    static constexpr int SAMPLE_RATE = 44100;

    Stm32MixerFirmware()
        : m_afs(SAMPLE_RATE, 12.0f, 3),
          m_stft(SAMPLE_RATE, true),
          m_loudness(SAMPLE_RATE, -14.0f),
          m_mixer(SAMPLE_RATE),
          m_oled(SAMPLE_RATE),
          m_howling_alert_until_ms(0),
          m_rx_len(0),
          m_has_latest_audio(false) {
        std::memset(&m_cb, 0, sizeof(m_cb));
        m_fader_adc_last.fill(819); // ~80% (0dB 부근 초기값)
        m_fader_adc_tx_last.fill(819);
        for (int i = 0; i < 5; ++i) {
            m_fader_filters[i].reset(0.8f);
            m_mixer.set_fader(i, 0.8f);
        }
        for (int b = 0; b < EQ_BANDS_COUNT; ++b) {
            m_eq_filters[b].reset(0.5f);
        }
        m_vu_levels.fill(0.0f);
        m_eq_db.fill(0.0f);
        m_latest_out.fill(0.0f);
        m_rx_buf.fill(0);
    }

    void init(const HwCallbacks_t& callbacks) {
        m_cb = callbacks;
        flush_leds();
    }

    /**
     * @brief I2S / SAI 오디오 DMA 블록 콜백 (실시간 인터럽트/고우선순위 태스크)
     * @param mic_in_i16  마이크 입력(CH3, 스테레오 인터리빙 int16, NULL 가능)
     * @param usb_in_i16  PC USB Audio 입력(HTDemucs 반주/보컬, NULL 가능)
     * @param line_in_i16 외부 라인 악기 입력(CH4, NULL 가능)
     * @param dac_out_i16 스피커/헤드폰 DAC 출력 버퍼 (int16)
     * @param usb_out_i16 PC USB Audio 캡처 송신 버퍼 (NULL 가능)
     * @param num_frames  프레임 수 (최대 512)
     */
    void on_audio_dma_block(const int16_t* mic_in_i16,
                            const int16_t* usb_in_i16,
                            const int16_t* line_in_i16,
                            int16_t* dac_out_i16,
                            int16_t* usb_out_i16,
                            size_t num_frames) {
        if (num_frames == 0 || num_frames > BLOCK_SIZE) return;

        // 1. int16 PCM -> float 정규화
        for (size_t i = 0; i < num_frames * 2; ++i) {
            m_mic_f[i]  = mic_in_i16  ? (static_cast<float>(mic_in_i16[i])  / 32768.0f) : 0.0f;
            m_usb_f[i]  = usb_in_i16  ? (static_cast<float>(usb_in_i16[i])  / 32768.0f) : 0.0f;
            m_line_f[i] = line_in_i16 ? (static_cast<float>(line_in_i16[i]) / 32768.0f) : 0.0f;
        }

        // 2. 마이크 채널(CH3) 온보드 실시간 AFS 하울링 억제
        float howling_freq = m_afs.process(m_mic_f.data(), num_frames);
        if (howling_freq > 0.0f) {
            uint32_t now = get_now_ms();
            m_howling_alert_until_ms = now + 3000;
            char msg[48];
            int n = std::snprintf(msg, sizeof(msg), "[AFS_EVENT:%.1f]\n", static_cast<double>(howling_freq));
            if (n > 0) send_vcp(msg, static_cast<uint16_t>(n));
        }

        // 3. 스템 라우팅: PC USB 오디오가 있으면 CH1/CH2로, 없으면 하드웨어 단독 실시간 STFT 분리
        if (usb_in_i16 && num_frames == BLOCK_SIZE) {
            std::memcpy(m_vocal_f.data(), m_usb_f.data(), num_frames * 2 * sizeof(float));
            std::memset(m_beat_f.data(), 0, num_frames * 2 * sizeof(float));
        } else if (num_frames == BLOCK_SIZE) {
            m_stft.process_block_512(m_line_f.data(), m_vocal_f.data(), m_beat_f.data(), m_inst_f.data());
        } else {
            std::memcpy(m_vocal_f.data(), m_mic_f.data(), num_frames * 2 * sizeof(float));
            std::memset(m_beat_f.data(), 0, num_frames * 2 * sizeof(float));
            std::memcpy(m_inst_f.data(), m_line_f.data(), num_frames * 2 * sizeof(float));
        }

        const float* ch_inputs[EmbeddedMixer::NUM_CHANNELS] = {
            m_mixed_f.data(),
            m_vocal_f.data(),
            m_beat_f.data(),
            m_mic_f.data(),
            (usb_in_i16 ? m_line_f.data() : m_inst_f.data())
        };

        finish_audio_block(ch_inputs, dac_out_i16, usb_out_i16, num_frames);
    }

    /**
     * @brief PC 호스트(WASAPI Mic + HTDemucs AI 스템) + 보드 합성기 입력을 온보드 DSP로 처리
     */
    void process_multitrack_audio_block(const int16_t* mic_in_i16,
                                        const int16_t* vocal_in_i16,
                                        const int16_t* beat_in_i16,
                                        const int16_t* line_inst_in_i16,
                                        int16_t* dac_out_i16,
                                        size_t num_frames,
                                        bool use_external_stems) {
        if (num_frames == 0 || num_frames > BLOCK_SIZE) return;

        for (size_t i = 0; i < num_frames * 2; ++i) {
            m_mic_f[i]   = mic_in_i16       ? (static_cast<float>(mic_in_i16[i]) / 32768.0f)       : 0.0f;
            m_vocal_f[i] = vocal_in_i16     ? (static_cast<float>(vocal_in_i16[i]) / 32768.0f)     : 0.0f;
            m_beat_f[i]  = beat_in_i16      ? (static_cast<float>(beat_in_i16[i]) / 32768.0f)      : 0.0f;
            m_inst_f[i]  = line_inst_in_i16 ? (static_cast<float>(line_inst_in_i16[i]) / 32768.0f) : 0.0f;
        }

        // CH3 마이크 입력단 실시간 AFS 하울링 억제 (음악 원음 음질 보존)
        float howling_freq = m_afs.process(m_mic_f.data(), num_frames);
        if (howling_freq > 0.0f) {
            uint32_t now = get_now_ms();
            m_howling_alert_until_ms = now + 3000;
            char msg[48];
            int n = std::snprintf(msg, sizeof(msg), "[AFS_EVENT:%.1f]\n", static_cast<double>(howling_freq));
            if (n > 0) send_vcp(msg, static_cast<uint16_t>(n));
        }

        if (use_external_stems) {
            for (size_t i = 0; i < num_frames * 2; ++i) {
                m_usb_f[i] = m_vocal_f[i] + m_beat_f[i] + m_inst_f[i];
            }
            if (!m_stft.is_enabled()) {
                std::memcpy(m_vocal_f.data(), m_usb_f.data(), num_frames * 2 * sizeof(float));
                std::memset(m_beat_f.data(), 0, num_frames * 2 * sizeof(float));
                std::memset(m_inst_f.data(), 0, num_frames * 2 * sizeof(float));
            }
        } else {
            if (!vocal_in_i16 && num_frames == BLOCK_SIZE) {
                m_stft.process_block_512(m_mic_f.data(), m_vocal_f.data(), m_line_f.data(), m_usb_f.data());
            } else if (!vocal_in_i16) {
                std::memcpy(m_vocal_f.data(), m_mic_f.data(), num_frames * 2 * sizeof(float));
            }
            std::memcpy(m_usb_f.data(), m_mic_f.data(), num_frames * 2 * sizeof(float));
        }

        const float* ch_inputs[EmbeddedMixer::NUM_CHANNELS] = {
            m_usb_f.data(),   // CH0: Master reference
            m_vocal_f.data(), // CH1: Vocal (STFT or HTDemucs Vocal)
            m_beat_f.data(),  // CH2: Beat (Synth Drum or HTDemucs Beat)
            m_mic_f.data(),   // CH3: Mic In 1 (AFS Protected)
            m_inst_f.data()   // CH4: Line 2 / Inst
        };

        finish_audio_block(ch_inputs, dac_out_i16, nullptr, num_frames);
    }

    /**
     * @brief ADC DMA 스캔 완료 시 호출 (5개 페이더 + 3개 EQ 노브, 0~max_adc)
     */
    void on_adc_dma_scan(const uint16_t fader_adc[ADC_CHANNELS_COUNT],
                         const uint16_t eq_adc[EQ_BANDS_COUNT] = nullptr,
                         uint16_t max_adc = 1023) {
        bool changed = false;
        float inv_max = 1.0f / static_cast<float>(max_adc > 0 ? max_adc : 1023);

        for (int ch = 0; ch < ADC_CHANNELS_COUNT; ++ch) {
            float raw_norm = mcu_clamp(static_cast<float>(fader_adc[ch]) * inv_max, 0.0f, 1.0f);
            float smoothed = m_fader_filters[ch].update(raw_norm);
            m_mixer.set_fader(ch, smoothed);

            uint16_t scaled_1023 = static_cast<uint16_t>(smoothed * 1023.0f + 0.5f);
            int diff = static_cast<int>(scaled_1023) - static_cast<int>(m_fader_adc_tx_last[ch]);
            if (diff > 4 || diff < -4) {
                changed = true;
            }
            m_fader_adc_last[ch] = scaled_1023;
        }

        if (eq_adc) {
            for (int b = 0; b < EQ_BANDS_COUNT; ++b) {
                float norm = mcu_clamp(static_cast<float>(eq_adc[b]) * inv_max, 0.0f, 1.0f);
                float sm = m_eq_filters[b].update(norm);
                m_eq_db[b] = -12.0f + sm * 24.0f;
            }
            m_mixer.set_master_eq_db(m_eq_db[0], m_eq_db[1], m_eq_db[2]);
        }

        if (changed) {
            for (int ch = 0; ch < ADC_CHANNELS_COUNT; ++ch) {
                m_fader_adc_tx_last[ch] = m_fader_adc_last[ch];
            }
            char pkt[64];
            int n = std::snprintf(pkt, sizeof(pkt), "F:%u,%u,%u,%u,%u\n",
                                  m_fader_adc_last[0], m_fader_adc_last[1], m_fader_adc_last[2],
                                  m_fader_adc_last[3], m_fader_adc_last[4]);
            if (n > 0) send_vcp(pkt, static_cast<uint16_t>(n));
        }
    }

    /**
     * @brief GPIO 버튼 및 로터리 엔코더 폴링/인터럽트 처리 (25ms 디바운스)
     */
    void on_gpio_buttons(bool afs_pressed, bool stem_pressed, bool loud_pressed, int8_t encoder_delta) {
        uint32_t now = get_now_ms();

        McuButtonEvent ev_afs = m_deb_afs.update(afs_pressed, now);
        if (ev_afs == McuButtonEvent::Click) {
            m_afs.set_enabled(!m_afs.is_enabled());
            send_vcp("B:AFS\n", 6);
            flush_leds();
        } else if (ev_afs == McuButtonEvent::LongPress) {
            m_afs.clear_filters();
            send_vcp("B:AFS_RESET\n", 12);
        }

        if (m_deb_stem.update(stem_pressed, now) == McuButtonEvent::Click) {
            m_stft.set_enabled(!m_stft.is_enabled());
            send_vcp("B:STEM\n", 7);
            flush_leds();
        }

        if (m_deb_loud.update(loud_pressed, now) == McuButtonEvent::Click) {
            m_loudness.set_enabled(!m_loudness.is_enabled());
            send_vcp("B:LOUD\n", 7);
            flush_leds();
        }

        if (encoder_delta != 0) {
            float cur = m_loudness.get_target_lufs();
            m_loudness.set_target_lufs(cur + static_cast<float>(encoder_delta) * 0.5f);
            if (encoder_delta > 0) send_vcp("E:+1\n", 5);
            else                   send_vcp("E:-1\n", 5);
        }
    }

    /**
     * @brief PC 호스트로부터 USB VCP 패킷 수신 (CDC_Receive_FS에서 호출)
     */
    void on_vcp_rx_bytes(const uint8_t* data, uint16_t len) {
        if (!data || len == 0) return;
        for (uint16_t i = 0; i < len; ++i) {
            char c = static_cast<char>(data[i]);
            if (c == '\n' || c == '\r') {
                if (m_rx_len > 0) {
                    m_rx_buf[m_rx_len] = '\0';
                    parse_host_command(m_rx_buf.data());
                    m_rx_len = 0;
                }
            } else if (m_rx_len + 1 < m_rx_buf.size()) {
                m_rx_buf[m_rx_len++] = c;
            }
        }
    }

    /**
     * @brief 30 FPS(약 33ms) UI 태스크: OLED 스펙트럼 렌더링 및 LED 갱신
     */
    void on_ui_tick() {
        if (m_has_latest_audio) {
            m_oled.compute_spectrum(m_latest_out.data(), m_latest_frames);
        }
        bool alert = (get_now_ms() < m_howling_alert_until_ms);
        m_oled.render_framebuffer(m_afs.is_enabled(), m_stft.is_enabled(), m_loudness.is_enabled(), alert);
        if (m_cb.oled_flush) {
            m_cb.oled_flush(m_oled.get_framebuffer());
        }
        flush_leds();
    }

    const EmbeddedAFS& get_afs() const { return m_afs; }
    const EmbeddedLoudness& get_loudness() const { return m_loudness; }
    const EmbeddedMixer& get_mixer() const { return m_mixer; }
    const EmbeddedOledRenderer& get_oled() const { return m_oled; }
    bool is_stem_enabled() const { return m_stft.is_enabled(); }
    bool get_led_afs() const { return m_afs.is_enabled(); }
    bool get_led_stem() const { return m_stft.is_enabled(); }
    bool get_led_loud() const { return m_loudness.is_enabled(); }
    bool is_howling_alert() const { return get_now_ms() < m_howling_alert_until_ms; }
    float get_vu_level(int ch) const { return (ch >= 0 && ch < 5) ? m_vu_levels[ch] : 0.0f; }
    uint16_t get_fader_adc_smoothed(int ch) const { return (ch >= 0 && ch < ADC_CHANNELS_COUNT) ? m_fader_adc_last[ch] : 0; }
    float get_eq_db(int b) const { return (b >= 0 && b < EQ_BANDS_COUNT) ? m_eq_db[b] : 0.0f; }

private:
    void finish_audio_block(const float* const ch_inputs[EmbeddedMixer::NUM_CHANNELS],
                            int16_t* dac_out_i16,
                            int16_t* usb_out_i16,
                            size_t num_frames) {
        // 채널별 VU RMS 레벨 측정
        for (int ch = 0; ch < 5; ++ch) {
            const float* src = ch_inputs[ch];
            if (!src) {
                m_vu_levels[ch] = 0.0f;
                continue;
            }
            float sum_sq = 0.0f;
            for (size_t i = 0; i < num_frames * 2; ++i) {
                sum_sq += src[i] * src[i];
            }
            float rms = std::sqrt(sum_sq / static_cast<float>(num_frames * 2));
            m_vu_levels[ch] = rms * m_mixer.get_gain_linear(ch);
        }

        // 5채널 안티-팝 믹싱 & 3-Band EQ
        m_mixer.mix(ch_inputs, m_mixed_f.data(), num_frames);

        // 라우드니스 매치 & 피크 리미터
        m_loudness.process(m_mixed_f.data(), num_frames);

        // 최신 출력 스펙트럼용 스냅샷 저장 & int16 DAC/USB 출력 변환
        std::memcpy(m_latest_out.data(), m_mixed_f.data(), num_frames * 2 * sizeof(float));
        m_latest_frames = num_frames;
        m_has_latest_audio = true;

        for (size_t i = 0; i < num_frames * 2; ++i) {
            float s = mcu_clamp(m_mixed_f[i], -1.0f, 1.0f);
            int16_t pcm = static_cast<int16_t>(s * 32767.0f);
            if (dac_out_i16) dac_out_i16[i] = pcm;
            if (usb_out_i16) usb_out_i16[i] = pcm;
        }
    }
    void parse_host_command(const char* cmd) {
        if (std::strncmp(cmd, "[LED:", 5) == 0) {
            int a = 1, s = 1, l = 1;
            if (std::sscanf(cmd + 5, "%d,%d,%d", &a, &s, &l) == 3) {
                m_afs.set_enabled(a != 0);
                m_stft.set_enabled(s != 0);
                m_loudness.set_enabled(l != 0);
                flush_leds();
            }
        } else if (std::strncmp(cmd, "[FADER:", 7) == 0) {
            int c0, c1, c2, c3, c4;
            if (std::sscanf(cmd + 7, "%d,%d,%d,%d,%d", &c0, &c1, &c2, &c3, &c4) == 5) {
                int vals[5] = { c0, c1, c2, c3, c4 };
                for (int i = 0; i < 5; ++i) {
                    float norm = mcu_clamp(vals[i] / 1023.0f, 0.0f, 1.0f);
                    m_fader_filters[i].reset(norm);
                    m_mixer.set_fader(i, norm);
                    m_fader_adc_last[i] = static_cast<uint16_t>(norm * 1023.0f);
                    m_fader_adc_tx_last[i] = m_fader_adc_last[i];
                }
            }
        } else if (std::strncmp(cmd, "[LUFS:", 6) == 0) {
            float t = -14.0f;
            if (std::sscanf(cmd + 6, "%f", &t) == 1) {
                m_loudness.set_target_lufs(t);
            }
        } else if (std::strncmp(cmd, "[RESET_AFS]", 11) == 0) {
            m_afs.clear_filters();
        }
    }

    void flush_leds() {
        if (m_cb.set_leds) {
            m_cb.set_leds(m_afs.is_enabled() ? 1 : 0,
                          m_stft.is_enabled() ? 1 : 0,
                          m_loudness.is_enabled() ? 1 : 0);
        }
    }

    void send_vcp(const char* data, uint16_t len) {
        if (m_cb.tx_vcp && data && len > 0) {
            m_cb.tx_vcp(data, len);
        }
    }

    uint32_t get_now_ms() const {
        return m_cb.get_tick_ms ? m_cb.get_tick_ms() : 0;
    }

    HwCallbacks_t m_cb;
    EmbeddedAFS m_afs;
    EmbeddedStftSeparator m_stft;
    EmbeddedLoudness m_loudness;
    EmbeddedMixer m_mixer;
    EmbeddedOledRenderer m_oled;

    std::array<EmbeddedSmoothingFilter, ADC_CHANNELS_COUNT> m_fader_filters;
    std::array<EmbeddedSmoothingFilter, EQ_BANDS_COUNT> m_eq_filters;
    std::array<uint16_t, ADC_CHANNELS_COUNT> m_fader_adc_last;
    std::array<uint16_t, ADC_CHANNELS_COUNT> m_fader_adc_tx_last;

    EmbeddedButtonDebouncer m_deb_afs;
    EmbeddedButtonDebouncer m_deb_stem;
    EmbeddedButtonDebouncer m_deb_loud;

    uint32_t m_howling_alert_until_ms;
    std::array<char, PACKET_MAX_LEN> m_rx_buf;
    size_t m_rx_len;

    std::array<float, BLOCK_SIZE * 2> m_mic_f;
    std::array<float, BLOCK_SIZE * 2> m_usb_f;
    std::array<float, BLOCK_SIZE * 2> m_line_f;
    std::array<float, BLOCK_SIZE * 2> m_vocal_f;
    std::array<float, BLOCK_SIZE * 2> m_beat_f;
    std::array<float, BLOCK_SIZE * 2> m_inst_f;
    std::array<float, BLOCK_SIZE * 2> m_mixed_f;
    std::array<float, BLOCK_SIZE * 2> m_latest_out;
    std::array<float, 5> m_vu_levels;
    std::array<float, EQ_BANDS_COUNT> m_eq_db;
    size_t m_latest_frames = 0;
    bool m_has_latest_audio;
};

} // namespace SmartMixerMCU
