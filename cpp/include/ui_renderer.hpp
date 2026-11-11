#pragma once

#include <vector>
#include <string>
#include <iostream>
#include <sstream>
#include <cmath>
#include <chrono>
#include <algorithm>
#include "afs.hpp"

namespace SmartMixer {

class SpectrumRenderer {
public:
    SpectrumRenderer(int num_bars = 16, int fft_size = 1024, int sample_rate = 44100)
        : m_num_bars(num_bars), m_fft_size(fft_size), m_sample_rate(sample_rate),
          m_window(fft_size), m_fft_buf(fft_size), m_bar_heights(num_bars, 0.0f) {
        constexpr float PI = 3.14159265358979323846f;
        for (int i = 0; i < fft_size; ++i) {
            m_window[i] = 0.5f * (1.0f - std::cos(2.0f * PI * i / (fft_size - 1)));
        }

        // 100Hz ~ 12kHz 로그 간격 밴드 경계
        float bin_res = static_cast<float>(sample_rate) / fft_size;
        float log_min = std::log10(100.0f);
        float log_max = std::log10(12000.0f);
        float step = (log_max - log_min) / num_bars;

        for (int i = 0; i < num_bars; ++i) {
            float f_start = std::pow(10.0f, log_min + i * step);
            float f_end = std::pow(10.0f, log_min + (i + 1) * step);
            int start_bin = static_cast<int>(f_start / bin_res);
            int end_bin = static_cast<int>(f_end / bin_res);
            start_bin = std::clamp(start_bin, 0, fft_size / 2 - 1);
            end_bin = std::clamp(end_end(start_bin, end_bin), start_bin + 1, fft_size / 2);
            m_band_bins.emplace_back(start_bin, end_bin);
        }
    }

    std::vector<float> compute_bars(const float* stereo_buffer, size_t num_frames) {
        size_t n = std::min(num_frames, static_cast<size_t>(m_fft_size));
        for (size_t i = 0; i < n; ++i) {
            float mono = 0.5f * (stereo_buffer[i * 2] + stereo_buffer[i * 2 + 1]);
            m_fft_buf[i] = std::complex<float>(mono * m_window[i], 0.0f);
        }
        for (size_t i = n; i < static_cast<size_t>(m_fft_size); ++i) {
            m_fft_buf[i] = std::complex<float>(0.0f, 0.0f);
        }

        FastFFT::fft(m_fft_buf);

        std::vector<float> new_heights(m_num_bars, 0.0f);
        for (int i = 0; i < m_num_bars; ++i) {
            int start_b = m_band_bins[i].first;
            int end_b = m_band_bins[i].second;
            float sum_mag = 0.0f;
            for (int b = start_b; b < end_b; ++b) {
                sum_mag += std::abs(m_fft_buf[b]);
            }
            float avg_mag = sum_mag / std::max(1, end_b - start_b) + 1e-12f;
            float mag_db = 20.0f * std::log10(avg_mag);
            // -60dB ~ 0dB -> 0.0 ~ 1.0 정규화
            float norm = (mag_db + 60.0f) / 60.0f;
            new_heights[i] = std::clamp(norm, 0.0f, 1.0f);
        }

        // 피크 스무딩
        constexpr float DECAY = 0.85f;
        for (int i = 0; i < m_num_bars; ++i) {
            m_bar_heights[i] = std::max(new_heights[i], m_bar_heights[i] * DECAY);
        }
        return m_bar_heights;
    }

    std::string render_dashboard(const float* stereo_buffer, size_t num_frames,
                                 bool afs_on, bool stem_on, bool loud_on,
                                 float lufs, const std::string& toast_msg,
                                 const std::vector<float>& notches, float fps) {
        auto bars = compute_bars(stereo_buffer, num_frames);

        std::ostringstream ss;
        ss << "==================================================================\n";
        ss << "  [C++ CORE] SMART AUDIO MIXER  |  Render: " << static_cast<int>(fps) << " FPS (Target: 30+)\n";
        ss << "------------------------------------------------------------------\n";
        ss << " Status: " << (afs_on ? "[AFS: ACTIVE]  " : "[AFS: OFF]     ")
           << (stem_on ? "[STEM: 2-WAY]  " : "[STEM: OFF]    ")
           << (loud_on ? "[LOUD: -14 LUFS]" : "[LOUD: OFF]     ") << "\n";
        ss << " Measured LUFS: " << lufs << " dB\n";
        ss << "------------------------------------------------------------------\n";
        ss << " Real-Time FFT Audio Spectrum (100Hz ~ 12kHz):\n";

        constexpr int MAX_H = 6;
        for (int row = MAX_H; row >= 1; --row) {
            float thresh = static_cast<float>(row) / MAX_H;
            ss << "   ";
            for (float val : bars) {
                ss << (val >= thresh ? "# " : ". ");
            }
            ss << "\n";
        }
        ss << "   1 2 3 4 5 6 7 8 9 A B C D E F G\n";
        ss << "------------------------------------------------------------------\n";
        if (!toast_msg.empty()) {
            ss << " >>> NOTIFICATION: " << toast_msg << "\n";
        } else if (!notches.empty()) {
            ss << " Active Notches: ";
            for (float f : notches) ss << static_cast<int>(f) << "Hz ";
            ss << "\n";
        } else {
            ss << " System Status: Normal (No howling detected)\n";
        }
        ss << "==================================================================\n";
        return ss.str();
    }

    // 0.96인치 128x64 OLED SSD1306 SPI/I2C 프레임버퍼 (1024 바이트) 생성 (S-FR-6.6)
    void generate_oled_buffer(const std::vector<float>& bars, uint8_t* out_fb_1024) {
        std::fill(out_fb_1024, out_fb_1024 + 1024, 0);
        // SSD1306: 8 pages x 128 columns
        for (int bar_i = 0; bar_i < m_num_bars && bar_i < 16; ++bar_i) {
            int bar_h = static_cast<int>(bars[bar_i] * 48.0f);
            int col_start = bar_i * 8;
            for (int c = col_start; c < col_start + 6 && c < 128; ++c) {
                for (int y = 0; y < bar_h; ++y) {
                    int pixel_y = 63 - y;
                    int page = pixel_y / 8;
                    int bit = pixel_y % 8;
                    out_fb_1024[page * 128 + c] |= (1 << bit);
                }
            }
        }
    }

private:
    int end_end(int s, int e) { return (e > s) ? e : (s + 1); }

    int m_num_bars;
    int m_fft_size;
    int m_sample_rate;
    std::vector<float> m_window;
    std::vector<std::complex<float>> m_fft_buf;
    std::vector<std::pair<int, int>> m_band_bins;
    std::vector<float> m_bar_heights;
};

} // namespace SmartMixer

