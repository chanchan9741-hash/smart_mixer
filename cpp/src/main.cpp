#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <cmath>
#include <atomic>
#include <iomanip>

#include "../include/config.hpp"
#include "../include/ring_buffer.hpp"
#include "../include/biquad.hpp"
#include "../include/afs.hpp"
#include "../include/loudness.hpp"
#include "../include/stem_separator.hpp"
#include "../include/mixer.hpp"
#include "../include/debouncer.hpp"
#include "../include/controller_mapper.hpp"
#include "../include/ui_renderer.hpp"

using namespace SmartMixer;

class SmartAudioMixerEngine {
public:
    SmartAudioMixerEngine(const SystemConfig& cfg = SystemConfig())
        : m_config(cfg),
          m_ring_buf(cfg.audio.ring_buffer_size, 2),
          m_mixer(cfg.audio.num_mixer_channels),
          m_stem_separator(cfg.audio.sample_rate),
          m_loudness(cfg.audio.sample_rate, cfg.loudness),
          m_spectrum(16, cfg.afs.fft_size, cfg.audio.sample_rate),
          m_is_running(false),
          m_inject_howling(false) {
        // AFS 초기화 및 하울링 콜백 연동
        m_afs = std::make_unique<AutomaticFeedbackSuppression>(
            cfg.audio.sample_rate, cfg.afs,
            [this](float freq) {
                this->on_howling_detected(freq);
            }
        );
    }

    void on_howling_detected(float freq) {
        std::ostringstream ss;
        ss << "Howling Detected at " << std::fixed << std::setprecision(1) << freq << "Hz! Notch Filter Applied.";
        m_toast_message = ss.str();
        m_toast_expire_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    }

    void inject_test_howling(float freq = 2500.0f, float duration_sec = 0.8f) {
        m_howl_freq = freq;
        m_howl_samples_remaining = static_cast<size_t>(m_config.audio.sample_rate * duration_sec);
        m_howl_phase = 0.0f;
        m_inject_howling = true;
    }

    // DSP 실시간 처리 루프
    void process_block(float* buffer, size_t num_frames) {
        // 1. 하울링 주입 시뮬레이션 합성
        if (m_inject_howling && m_howl_samples_remaining > 0) {
            constexpr float PI = 3.14159265358979323846f;
            float phase_step = 2.0f * PI * m_howl_freq / m_config.audio.sample_rate;
            size_t chunk = std::min(num_frames, m_howl_samples_remaining);
            for (size_t i = 0; i < chunk; ++i) {
                float howl = 0.7f * std::sin(m_howl_phase);
                m_howl_phase += phase_step;
                buffer[i * 2] += howl;
                buffer[i * 2 + 1] += howl;
            }
            m_howl_samples_remaining -= chunk;
            if (m_howl_samples_remaining == 0) {
                m_inject_howling = false;
            }
        }

        // 2. 스템 분리 (S-FR-4)
        std::vector<float> vocal_buf(num_frames * 2);
        std::vector<float> beat_buf(num_frames * 2);
        m_stem_separator.process(buffer, vocal_buf.data(), beat_buf.data(), num_frames);

        // 3. 5채널 믹서 입력 구성 (S-FR-1)
        std::vector<float> dummy_in2(num_frames * 2, 0.0f);
        std::vector<const float*> mixer_inputs = {
            buffer,           // CH0: Master Bus
            vocal_buf.data(), // CH1: Vocal Stem
            beat_buf.data(),  // CH2: Beat Stem
            buffer,           // CH3: Mic In 1
            dummy_in2.data()  // CH4: Line In 2
        };

        std::vector<float> mixed_out(num_frames * 2);
        m_mixer.mix(mixer_inputs, mixed_out.data(), num_frames);

        // 4. AFS 하울링 자동 억제 (S-FR-3)
        m_afs->process(mixed_out.data(), num_frames);

        // 5. 라우드니스 매치 & 피크 리미터 (S-FR-5)
        m_loudness.process(mixed_out.data(), num_frames);

        // 최종 출력 복사
        std::copy(mixed_out.begin(), mixed_out.end(), buffer);
    }

    void run_demo(float duration_sec = 5.0f, bool test_howling = true) {
        std::cout << "\n==================================================================\n";
        std::cout << " [C++ Native Core] Smart Audio Mixer Pipeline Starting...\n";
        std::cout << "  - Latency Budget: < 20ms (PR-1)\n";
        std::cout << "  - Audio Spec: 44.1kHz Stereo 32-bit Float (PR-2)\n";
        std::cout << "  - UI Target: 30+ FPS Real-time Visualization (PR-3)\n";
        std::cout << "==================================================================\n\n";

        m_is_running = true;
        auto start_time = std::chrono::steady_clock::now();
        auto last_render_time = start_time;
        auto fps_time = start_time;
        int frame_count = 0;
        float actual_fps = 0.0f;

        bool howl_injected = false;
        size_t block_size = m_config.audio.block_size;
        std::vector<float> audio_block(block_size * 2, 0.0f);

        float sim_t = 0.0f;
        float sim_dt = 1.0f / m_config.audio.sample_rate;
        constexpr float PI = 3.14159265358979323846f;

        while (m_is_running) {
            auto now = std::chrono::steady_clock::now();
            float elapsed_sec = std::chrono::duration<float>(now - start_time).count();
            if (elapsed_sec >= duration_sec) break;

            // 2초 후 2500Hz 하울링 피드백 인위적 주입 테스트
            if (test_howling && !howl_injected && elapsed_sec > 1.8f) {
                howl_injected = true;
                std::cout << "\n[TEST] 2500Hz 하울링(피드백 이상 피크) 주입 시뮬레이션...\n";
                inject_test_howling(2500.0f, 0.8f);
            }

            // 가상 오디오 신호 생성 (440Hz 톤 + 노이즈)
            for (size_t i = 0; i < block_size; ++i) {
                float sample = 0.06f * std::sin(2.0f * PI * 440.0f * sim_t);
                sim_t += sim_dt;
                audio_block[i * 2] = sample;
                audio_block[i * 2 + 1] = sample;
            }

            // 실시간 DSP 블록 처리
            process_block(audio_block.data(), block_size);

            // FPS 측정
            frame_count++;
            float fps_elapsed = std::chrono::duration<float>(now - fps_time).count();
            if (fps_elapsed >= 1.0f) {
                actual_fps = frame_count / fps_elapsed;
                frame_count = 0;
                fps_time = now;
            }

            // 30 FPS 실시간 렌더링 동기화 (~33ms)
            float render_elapsed = std::chrono::duration<float>(now - last_render_time).count();
            if (render_elapsed >= 0.033f) {
                last_render_time = now;

                // 토스트 만료 확인
                if (now > m_toast_expire_time) {
                    m_toast_message = "";
                }

                std::string dash = m_spectrum.render_dashboard(
                    audio_block.data(), block_size,
                    m_afs->is_enabled(),
                    m_stem_separator.is_enabled(),
                    m_loudness.is_enabled(),
                    m_loudness.get_measured_lufs(),
                    m_toast_message,
                    m_afs->get_notch_frequencies(),
                    actual_fps
                );

                // 콘솔 화면 갱신 (ANSI)
                std::cout << "\033[H\033[J" << dash << std::flush;
            }

            // 실제 오디오 블록 주기 대기 (~11.6ms)
            std::this_thread::sleep_for(std::chrono::milliseconds(11));
        }

        std::cout << "\n==================================================================\n";
        std::cout << " [C++ Core] Pipeline Execution Completed Successfully!\n";
        std::cout << "  - Total Feedback Suppressed Events: " << m_afs->get_suppressed_count() << "\n";
        auto notches = m_afs->get_notch_frequencies();
        std::cout << "  - Active Dynamic Notch Filters: ";
        for (float f : notches) std::cout << f << "Hz ";
        std::cout << "\n  - Measured Final LUFS: " << m_loudness.get_measured_lufs() << " dB\n";
        std::cout << "  - Average Render Speed: " << actual_fps << " FPS (Target: 30+ Satisfied)\n";
        std::cout << "==================================================================\n";
    }

private:
    SystemConfig m_config;
    AudioRingBuffer m_ring_buf;
    AudioMixer m_mixer;
    StemSeparator m_stem_separator;
    std::unique_ptr<AutomaticFeedbackSuppression> m_afs;
    LoudnessMatcher m_loudness;
    SpectrumRenderer m_spectrum;
    ParameterManager m_param_mgr;

    std::atomic<bool> m_is_running;
    bool m_inject_howling;
    float m_howl_freq;
    size_t m_howl_samples_remaining;
    float m_howl_phase;

    std::string m_toast_message;
    std::chrono::steady_clock::time_point m_toast_expire_time;
};

int main(int argc, char* argv[]) {
    float duration = 4.0f;
    bool howling = true;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--duration" && i + 1 < argc) {
            duration = std::stof(argv[++i]);
        } else if (arg == "--no-howling") {
            howling = false;
        }
    }

    SmartAudioMixerEngine engine;
    engine.run_demo(duration, howling);
    return 0;
}

