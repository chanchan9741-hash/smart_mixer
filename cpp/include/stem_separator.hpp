#pragma once

#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>
#include "afs.hpp"

namespace SmartMixer {

class StemSeparator {
public:
    StemSeparator(int sample_rate = 44100, bool enabled = true)
        : m_sample_rate(sample_rate), m_enabled(enabled),
          m_fft_size(1024), m_hop_size(512),
          m_window(m_fft_size),
          m_prev_in_l(m_hop_size, 0.0f), m_prev_in_r(m_hop_size, 0.0f),
          m_tail_v_l(m_hop_size, 0.0f), m_tail_v_r(m_hop_size, 0.0f),
          m_tail_b_l(m_hop_size, 0.0f), m_tail_b_r(m_hop_size, 0.0f),
          m_tail_i_l(m_hop_size, 0.0f), m_tail_i_r(m_hop_size, 0.0f),
          m_fft_l(m_fft_size), m_fft_r(m_fft_size),
          m_voc_l(m_fft_size), m_voc_r(m_fft_size),
          m_b_l(m_fft_size), m_b_r(m_fft_size),
          m_in_l(m_fft_size), m_in_r(m_fft_size) {

        // 제곱근 Hann 윈도우 생성 (완전 무결 COLA: w_analysis * w_synthesis = Hann -> Overlap-Add 시 1.0)
        constexpr float PI = 3.14159265358979323846f;
        for (int i = 0; i < m_fft_size; ++i) {
            float h = 0.5f * (1.0f - std::cos(2.0f * PI * i / m_fft_size));
            m_window[i] = std::sqrt(h);
        }
    }

    void set_enabled(bool enabled) { m_enabled = enabled; }
    bool is_enabled() const { return m_enabled; }

    // 3채널 실시간 스템 분리 (Vocal, Beat, Inst/Harmonic)
    // 원본 음원 = Vocal + Beat + Inst 가 완전 무결하게 유지되는 50% Overlap-Add STFT 직교 분해
    void process(const float* input_stereo, float* vocal_stereo, float* beat_stereo, float* inst_stereo, size_t num_frames) {
        if (!m_enabled) {
            std::copy(input_stereo, input_stereo + num_frames * 2, vocal_stereo);
            std::copy(input_stereo, input_stereo + num_frames * 2, beat_stereo);
            if (inst_stereo) {
                std::copy(input_stereo, input_stereo + num_frames * 2, inst_stereo);
            }
            return;
        }

        if (num_frames == static_cast<size_t>(m_hop_size)) {
            process_block_512(input_stereo, vocal_stereo, beat_stereo, inst_stereo);
            return;
        }

        // 512샘플이 아닌 임의 길이 버퍼(파일 일괄 내보내기/테스트 등) 대응
        size_t processed = 0;
        std::vector<float> in_512(m_hop_size * 2, 0.0f);
        std::vector<float> v_512(m_hop_size * 2, 0.0f);
        std::vector<float> b_512(m_hop_size * 2, 0.0f);
        std::vector<float> i_512(m_hop_size * 2, 0.0f);

        while (processed < num_frames) {
            size_t chunk = std::min<size_t>(m_hop_size, num_frames - processed);
            std::copy(input_stereo + processed * 2, input_stereo + (processed + chunk) * 2, in_512.data());
            if (chunk < static_cast<size_t>(m_hop_size)) {
                std::fill(in_512.begin() + chunk * 2, in_512.end(), 0.0f);
            }

            process_block_512(in_512.data(), v_512.data(), b_512.data(), i_512.data());

            std::copy(v_512.data(), v_512.data() + chunk * 2, vocal_stereo + processed * 2);
            std::copy(b_512.data(), b_512.data() + chunk * 2, beat_stereo + processed * 2);
            if (inst_stereo) {
                std::copy(i_512.data(), i_512.data() + chunk * 2, inst_stereo + processed * 2);
            }
            processed += chunk;
        }
    }

    // 2채널 스템 분리 오버로드 (기존 호환성)
    void process(const float* input_stereo, float* vocal_stereo, float* beat_stereo, size_t num_frames) {
        process(input_stereo, vocal_stereo, beat_stereo, nullptr, num_frames);
    }

private:
    int m_sample_rate;
    bool m_enabled;
    int m_fft_size;
    int m_hop_size;
    std::vector<float> m_window;
    std::vector<float> m_prev_in_l, m_prev_in_r;
    std::vector<float> m_tail_v_l, m_tail_v_r;
    std::vector<float> m_tail_b_l, m_tail_b_r;
    std::vector<float> m_tail_i_l, m_tail_i_r;
    std::vector<std::complex<float>> m_fft_l, m_fft_r;
    std::vector<std::complex<float>> m_voc_l, m_voc_r;
    std::vector<std::complex<float>> m_b_l, m_b_r;
    std::vector<std::complex<float>> m_in_l, m_in_r;

    void process_block_512(const float* in, float* v_out, float* b_out, float* i_out) {
        // 1. 1024-샘플 분석 프레임 = 이전 512샘플 + 현재 512샘플
        for (size_t i = 0; i < static_cast<size_t>(m_hop_size); ++i) {
            m_fft_l[i] = std::complex<float>(m_prev_in_l[i] * m_window[i], 0.0f);
            m_fft_r[i] = std::complex<float>(m_prev_in_r[i] * m_window[i], 0.0f);
        }
        for (size_t i = 0; i < static_cast<size_t>(m_hop_size); ++i) {
            float l = in[i * 2];
            float r = in[i * 2 + 1];
            m_fft_l[m_hop_size + i] = std::complex<float>(l * m_window[m_hop_size + i], 0.0f);
            m_fft_r[m_hop_size + i] = std::complex<float>(r * m_window[m_hop_size + i], 0.0f);
            m_prev_in_l[i] = l;
            m_prev_in_r[i] = r;
        }

        // 고속 FFT
        FastFFT::fft(m_fft_l);
        FastFFT::fft(m_fft_r);

        float bin_hz = static_cast<float>(m_sample_rate) / m_fft_size;

        // 2. 주파수 빈(Frequency Bin)별 직교 분해
        for (int k = 0; k < m_fft_size; ++k) {
            int sym_k = (k <= m_fft_size / 2) ? k : (m_fft_size - k);
            float freq = sym_k * bin_hz;

            std::complex<float> mid  = 0.5f * (m_fft_l[k] + m_fft_r[k]);
            std::complex<float> side = 0.5f * (m_fft_l[k] - m_fft_r[k]);

            float mag_m = std::abs(mid) + 1e-8f;
            float mag_s = std::abs(side) + 1e-8f;

            // 각 주파수 빈에서의 센터 집중도 (Center Dominance Ratio)
            // 사이드 에너지가 없으면 1.0 (순수 센터 보컬), 사이드 에너지가 크면 0.0 (스테레오 악기/리버브)
            float center_ratio = std::max(0.0f, mag_m - 1.2f * mag_s) / mag_m;
            center_ratio = std::clamp(center_ratio, 0.0f, 1.0f);
            float voc_mask = center_ratio * center_ratio;

            float beat_w = 0.0f;
            float voc_w = 0.0f;

            if (freq < 200.0f) {
                // 서브 베이스 & 킥 드럼 (< 200Hz)
                beat_w = 1.0f;
            } else if (freq >= 200.0f && freq <= 3800.0f) {
                // 인간 보컬 주파수 대역
                voc_w = voc_mask;
            }

            // Beat: 저역 센터 킥/베이스
            m_b_l[k] = mid * beat_w;
            m_b_r[k] = mid * beat_w;

            // Vocal: 센터 정위 리드 보컬
            m_voc_l[k] = mid * voc_w;
            m_voc_r[k] = mid * voc_w;

            // Inst: 완전 직교 잔여 성분 (원본 - 보컬 - 비트)
            // 완벽한 재합성 보장: Vocal + Beat + Inst = Original (0.000001%의 주파수 왜곡/먹먹함 없는 100% 음질)
            m_in_l[k] = m_fft_l[k] - m_voc_l[k] - m_b_l[k];
            m_in_r[k] = m_fft_r[k] - m_voc_r[k] - m_b_r[k];
        }

        // 역 FFT
        FastFFT::ifft(m_voc_l); FastFFT::ifft(m_voc_r);
        FastFFT::ifft(m_b_l);   FastFFT::ifft(m_b_r);
        FastFFT::ifft(m_in_l);  FastFFT::ifft(m_in_r);

        // 3. 합성 윈도우 적용 및 Overlap-Add (COLA)
        for (size_t i = 0; i < static_cast<size_t>(m_hop_size); ++i) {
            float v_l = m_tail_v_l[i] + m_voc_l[i].real() * m_window[i];
            float v_r = m_tail_v_r[i] + m_voc_r[i].real() * m_window[i];
            v_out[i * 2]     = v_l;
            v_out[i * 2 + 1] = v_r;
            m_tail_v_l[i] = m_voc_l[m_hop_size + i].real() * m_window[m_hop_size + i];
            m_tail_v_r[i] = m_voc_r[m_hop_size + i].real() * m_window[m_hop_size + i];

            float b_l = m_tail_b_l[i] + m_b_l[i].real() * m_window[i];
            float b_r = m_tail_b_r[i] + m_b_r[i].real() * m_window[i];
            b_out[i * 2]     = b_l;
            b_out[i * 2 + 1] = b_r;
            m_tail_b_l[i] = m_b_l[m_hop_size + i].real() * m_window[m_hop_size + i];
            m_tail_b_r[i] = m_b_r[m_hop_size + i].real() * m_window[m_hop_size + i];

            if (i_out) {
                float inst_l = m_tail_i_l[i] + m_in_l[i].real() * m_window[i];
                float inst_r = m_tail_i_r[i] + m_in_r[i].real() * m_window[i];
                i_out[i * 2]     = inst_l;
                i_out[i * 2 + 1] = inst_r;
                m_tail_i_l[i] = m_in_l[m_hop_size + i].real() * m_window[m_hop_size + i];
                m_tail_i_r[i] = m_in_r[m_hop_size + i].real() * m_window[m_hop_size + i];
            }
        }
    }
};

} // namespace SmartMixer
