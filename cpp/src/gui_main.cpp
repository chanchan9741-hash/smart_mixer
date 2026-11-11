#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <timeapi.h>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>
#include <cmath>

#include "../include/config.hpp"
#include "../include/ring_buffer.hpp"
#include "../include/biquad.hpp"
#include "../include/afs.hpp"
#include "../include/loudness.hpp"
#include "../include/stem_separator.hpp"
#include "../include/mixer.hpp"
#include "../include/controller_mapper.hpp"
#include "../include/ui_renderer.hpp"
#include "../include/wasapi_capture.hpp"
#include "../include/wasapi_render.hpp"
#include "../include/audio_file_player.hpp"
#include "../include/hardware_ipc.hpp"
#include "../include/gui_window.hpp"

using namespace SmartMixer;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    // 고해상도 타이머 활성화 (1ms 주기) - 윈도우 스케줄링 오디오 지연 및 지터 방지
    timeBeginPeriod(1);

    // 1. 코어 DSP 및 설정 초기화
    SystemConfig config;
    ParameterManager param_mgr;
    AudioMixer mixer(config.audio.num_mixer_channels);
    StemSeparator stem_separator(config.audio.sample_rate);
    LoudnessMatcher loudness(config.audio.sample_rate, config.loudness);
    SpectrumRenderer spectrum(16, config.afs.fft_size, config.audio.sample_rate);
    WasapiCaptureEngine wasapi;
    WasapiRenderEngine wasapi_render;
    AudioFilePlayer file_player(config.audio.sample_rate);

    // 2. 하드웨어 IPC 소켓 바인딩 (포트: 9123)
    HardwareIpcSocket mixer_ipc;
    mixer_ipc.bind_port(HardwareIpcSocket::MIXER_PORT);

    // 하드웨어 시뮬레이터 수신용 오디오 링 버퍼
    AudioRingBuffer hw_beat_ring_buffer(16384, 2);
    AudioRingBuffer hw_line2_ring_buffer(16384, 2);

    // 3. AFS 엔진 초기화
    std::unique_ptr<AutomaticFeedbackSuppression> afs;
    MixerGUIWindow* pWindow = nullptr;

    afs = std::make_unique<AutomaticFeedbackSuppression>(
        config.audio.sample_rate, config.afs,
        [&pWindow](float freq) {
            if (pWindow) {
                std::ostringstream ss;
                ss << "⚠️ Howling Detected at " << std::fixed << std::setprecision(1) << freq << "Hz! Notch Filter Applied.";
                pWindow->show_toast(ss.str(), 3.5f);
            }
        }
    );

    // 4. GUI 윈도우 생성
    MixerGUIWindow gui_window(hInstance, mixer, *afs, loudness, stem_separator, param_mgr, spectrum, wasapi, file_player, wasapi_render);
    pWindow = &gui_window;

    // 하울링 강제 주입 및 필터 리셋 콜백
    std::atomic<bool> inject_howling(false);
    std::atomic<size_t> howl_samples_remaining(0);
    float howl_phase = 0.0f;
    float howl_freq = 2500.0f;

    gui_window.set_inject_howling_callback([&]() {
        howl_freq = 2500.0f;
        howl_samples_remaining.store(static_cast<size_t>(config.audio.sample_rate * 0.8f));
        howl_phase = 0.0f;
        inject_howling.store(true);
    });

    gui_window.set_reset_filters_callback([&]() {
        afs->clear_filters();
    });

    // 슬라이더 조작 시 하드웨어 시뮬레이터로 즉각 전송 (초저지연 양방향 동기화)
    gui_window.set_fader_changed_callback([&](int ch, float norm) {
        (void)norm;
        FeedbackPacket pkt;
        std::memset(&pkt, 0, sizeof(pkt));
        pkt.packet_type = PKT_TYPE_FEEDBACK;
        pkt.led_afs = afs->is_enabled() ? 1 : 0;
        pkt.led_stem = stem_separator.is_enabled() ? 1 : 0;
        pkt.led_loud = loudness.is_enabled() ? 1 : 0;
        for (int i = 0; i < 5; ++i) {
            pkt.fader_adc[i] = param_mgr.get_fader_adc(i);
        }
        pkt.active_channel = static_cast<int8_t>(ch);
        mixer_ipc.send_packet(HardwareIpcSocket::SIMULATOR_PORT, &pkt, sizeof(pkt));
    });

    gui_window.set_fader_drag_end_callback([&](int ch) {
        (void)ch;
        FeedbackPacket pkt;
        std::memset(&pkt, 0, sizeof(pkt));
        pkt.packet_type = PKT_TYPE_FEEDBACK;
        pkt.led_afs = afs->is_enabled() ? 1 : 0;
        pkt.led_stem = stem_separator.is_enabled() ? 1 : 0;
        pkt.led_loud = loudness.is_enabled() ? 1 : 0;
        for (int i = 0; i < 5; ++i) {
            pkt.fader_adc[i] = param_mgr.get_fader_adc(i);
        }
        pkt.active_channel = -1;
        mixer_ipc.send_packet(HardwareIpcSocket::SIMULATOR_PORT, &pkt, sizeof(pkt));
    });

    // 5. 음원 파일 로드 및 스템 파일 일괄 내보내기 콜백
    std::atomic<bool> use_audio_file(false);
    std::atomic<bool> is_demucs_running(false);

    auto trigger_demucs_separation = [&]() {
        if (!file_player.is_loaded()) {
            gui_window.show_toast("음원 파일을 먼저 불러와주세요!", 3.0f);
            return;
        }

        if (is_demucs_running.load()) {
            gui_window.show_toast("AI 스템 분리가 이미 진행 중입니다. 잠시만 기다려주세요...", 3.0f);
            return;
        }

        std::thread([&]() {
            is_demucs_running.store(true);
            gui_window.show_toast("🤖 Demucs AI 분리 시작... (약 10~15초 소요, 음악 재생은 유지됩니다)", 10.0f);

            std::wstring in_path = file_player.get_file_path();
            size_t slash = in_path.find_last_of(L"\\/");
            std::wstring dir = (slash != std::wstring::npos) ? in_path.substr(0, slash) : L".";
            size_t dot = in_path.find_last_of(L".");
            std::wstring base_name = (dot != std::wstring::npos && dot > slash)
                                   ? in_path.substr(slash + 1, dot - slash - 1)
                                   : in_path.substr(slash + 1);

            std::wstring out_dir = dir + L"\\demucs_stems";

            // demucs_runner.py 스크립트 경로 탐색
            std::wstring script_path = L"scripts\\demucs_runner.py";
            if (GetFileAttributesW(script_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                script_path = L"..\\scripts\\demucs_runner.py";
            }

            // python "scripts\demucs_runner.py" "<in_path>" "<out_dir>"
            std::wstring cmd = L"python \"" + script_path + L"\" \"" + in_path + L"\" \"" + out_dir + L"\"";

            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi = { 0 };
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;

            std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
            cmd_buf.push_back(L'\0');

            BOOL created = CreateProcessW(
                nullptr,
                cmd_buf.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &si,
                &pi
            );

            bool success = false;
            if (created) {
                WaitForSingleObject(pi.hProcess, 180000); // 최대 3분 대기
                DWORD exit_code = 1;
                GetExitCodeProcess(pi.hProcess, &exit_code);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                if (exit_code == 0) {
                    success = true;
                }
            }

            if (success) {
                std::wstring v_path = out_dir + L"\\" + base_name + L"_vocal.wav";
                std::wstring b_path = out_dir + L"\\" + base_name + L"_beat.wav";
                std::wstring i_path = out_dir + L"\\" + base_name + L"_inst.wav";

                bool loaded = file_player.load_ai_stems(v_path, b_path, i_path);
                if (loaded) {
                    gui_window.show_toast("🎉 Demucs AI 스템 분리 완료! 스튜디오급 음원으로 자동 전환되었습니다.", 5.0f);
                } else {
                    gui_window.show_toast("❌ AI 스템 파일 로드 실패 (파일 손상 또는 미생성)", 4.0f);
                }
            } else {
                gui_window.show_toast("❌ Demucs 실행 실패. Python 환경 및 demucs 패키지를 확인하세요.", 5.0f);
            }

            is_demucs_running.store(false);
        }).detach();
    };

    gui_window.set_file_open_callback([&](const std::wstring& path) {
        bool ok = file_player.load_file(path);
        if (ok) {
            file_player.play();
            use_audio_file.store(true);
            gui_window.refresh_devices_with_file();

            // AI 스템이 이미 존재하는지 확인 (예: demucs_stems 폴더 또는 동일 폴더)
            size_t slash = path.find_last_of(L"\\/");
            std::wstring dir = (slash != std::wstring::npos) ? path.substr(0, slash) : L".";
            size_t dot = path.find_last_of(L".");
            std::wstring base_name = (dot != std::wstring::npos && dot > slash)
                                   ? path.substr(slash + 1, dot - slash - 1)
                                   : path.substr(slash + 1);

            std::wstring demucs_dir = dir + L"\\demucs_stems";
            std::wstring v1 = demucs_dir + L"\\" + base_name + L"_vocal.wav";
            std::wstring b1 = demucs_dir + L"\\" + base_name + L"_beat.wav";
            std::wstring i1 = demucs_dir + L"\\" + base_name + L"_inst.wav";

            std::wstring v2 = dir + L"\\" + base_name + L"_vocal.wav";
            std::wstring b2 = dir + L"\\" + base_name + L"_beat.wav";
            std::wstring i2 = dir + L"\\" + base_name + L"_inst.wav";

            bool loaded_ai = false;
            if (GetFileAttributesW(v1.c_str()) != INVALID_FILE_ATTRIBUTES &&
                GetFileAttributesW(b1.c_str()) != INVALID_FILE_ATTRIBUTES &&
                GetFileAttributesW(i1.c_str()) != INVALID_FILE_ATTRIBUTES) {
                loaded_ai = file_player.load_ai_stems(v1, b1, i1);
            } else if (GetFileAttributesW(v2.c_str()) != INVALID_FILE_ATTRIBUTES &&
                       GetFileAttributesW(b2.c_str()) != INVALID_FILE_ATTRIBUTES &&
                       GetFileAttributesW(i2.c_str()) != INVALID_FILE_ATTRIBUTES) {
                loaded_ai = file_player.load_ai_stems(v2, b2, i2);
            }

            if (loaded_ai) {
                gui_window.show_toast("기존 AI 분리 스템이 발견되어 자동 로드되었습니다! (Demucs)", 4.0f);
            } else {
                gui_window.show_toast("음원 재생 시작! 백그라운드 Demucs AI 분리가 자동 실행됩니다...", 4.0f);
                trigger_demucs_separation();
            }
        } else {
            gui_window.show_toast("Failed to load audio file! (Format unsupported)", 3.0f);
        }
    });

    gui_window.set_ai_separate_callback([&]() {
        trigger_demucs_separation();
    });

    gui_window.set_file_export_callback([&]() {
        if (!file_player.is_loaded()) return;
        std::thread([&]() {
            gui_window.show_toast("⏳ Separating stems into WAV files... Please wait.", 10.0f);
            std::wstring in_path = file_player.get_file_path();
            size_t dot = in_path.find_last_of(L".");
            std::wstring base = (dot != std::wstring::npos) ? in_path.substr(0, dot) : in_path;
            std::wstring v_path = base + L"_vocal.wav";
            std::wstring b_path = base + L"_beat.wav";
            std::wstring i_path = base + L"_inst.wav";

            bool ok = file_player.export_separated_stems(v_path, b_path, i_path, stem_separator);
            if (ok) {
                gui_window.show_toast("✅ Successfully exported _vocal.wav, _beat.wav, _inst.wav!", 6.0f);
            } else {
                gui_window.show_toast("❌ Failed to export stem files.", 4.0f);
            }
        }).detach();
    });

    // 6. 입력 장치 전환 핸들러 (WASAPI 실시간 마이크 vs 가상 신호 vs 음원 파일)
    std::atomic<bool> use_real_mic(false);
    AudioRingBuffer mic_ring_buffer(16384, 2);

    gui_window.set_device_selected_callback([&](int sel) {
        auto devices = wasapi.enumerate_capture_devices();
        if (sel == 0) {
            wasapi.stop();
            mic_ring_buffer.clear();
            use_real_mic.store(false);
            use_audio_file.store(false);
            file_player.pause();
            gui_window.show_toast("Switched to Virtual Vocal Generator", 2.0f);
        } else if (file_player.is_loaded() && sel == static_cast<int>(devices.size() + 1)) {
            use_audio_file.store(true);
            file_player.play();
            gui_window.show_toast("Switched to Music Track Player", 2.0f);
        } else {
            use_audio_file.store(false);
            file_player.pause();
            int dev_idx = sel - 1;
            mic_ring_buffer.clear();
            bool ok = wasapi.start_device(dev_idx, [&](const float* data, size_t frames) {
                mic_ring_buffer.write(data, frames);
            });

            if (ok) {
                use_real_mic.store(true);
                gui_window.show_toast("Vocal Microphone Connected (WASAPI Real-Time Input)", 3.0f);
            } else {
                use_real_mic.store(false);
                gui_window.show_toast("Failed to open microphone, reverting to Virtual mode", 3.0f);
            }
        }
    });

    // 7. 출력 장치 전환 핸들러 (WASAPI 실시간 스피커 / 이어폰 / DAC 재생)
    gui_window.set_output_selected_callback([&](int sel) {
        if (sel == 0) {
            wasapi_render.stop();
            gui_window.show_toast("Audio Output Muted (No Sound Output)", 2.0f);
        } else {
            int dev_idx = sel - 1;
            bool ok = wasapi_render.start_device(dev_idx);
            if (ok) {
                auto devs = wasapi_render.enumerate_render_devices();
                std::string name = (dev_idx >= 0 && dev_idx < static_cast<int>(devs.size())) ? devs[dev_idx].name_utf8 : "Selected Speaker";
                gui_window.show_toast("Speaker Connected: " + name, 3.0f);
            } else {
                gui_window.show_toast("Failed to open audio output device!", 3.0f);
            }
        }
    });

    // 기본 오디오 출력 장치 자동 연결
    wasapi_render.start_device(-1);

    if (!gui_window.create(1060, 690)) {
        MessageBoxW(nullptr, L"Failed to create GUI Window!", L"Error", MB_ICONERROR);
        return 1;
    }

    // 6. 백그라운드 실시간 오디오 DSP & 하드웨어 IPC 스레드 가동
    std::atomic<bool> is_running(true);
    std::thread audio_thread([&]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

        size_t block_size = config.audio.block_size;
        int sample_rate = config.audio.sample_rate;
        float dt = 1.0f / sample_rate;
        float t = 0.0f;
        constexpr float PI = 3.14159265358979323846f;

        std::vector<float> in_block(block_size * 2);
        std::vector<float> vocal_buf(block_size * 2);
        std::vector<float> beat_buf(block_size * 2);
        std::vector<float> inst_buf(block_size * 2);
        std::vector<float> line2_buf(block_size * 2);
        std::vector<float> file_block(block_size * 2, 0.0f);
        std::vector<float> mixed_out(block_size * 2);
        std::vector<float> vu_levels(5, 0.0f);

        uint8_t ipc_rx_buf[8192];
        FeedbackPacket fb_pkt;
        fb_pkt.packet_type = PKT_TYPE_FEEDBACK;

        while (is_running) {
            // --- 버퍼 수위 조절 (Adaptive Flow Control) ---
            if (use_real_mic.load()) {
                // 마이크 모드: 마이크 입력 버퍼에 1블록 이상 들어왔을 때만 진행, 없으면 1ms 대기
                if (mic_ring_buffer.get_available() < block_size) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
            } else {
                // 가상 신호 / 음악 파일 재생 모드:
                // 재생 출력 링 버퍼에 안전 쿠션(~70ms, 3072샘플) 이상 차있으면 2ms 양보
                if (wasapi_render.is_running() && wasapi_render.get_buffered_samples() >= 3072) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
            }

            if (!use_real_mic.load() && !wasapi_render.is_running()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            // --- A. 하드웨어 시뮬레이터로부터 IPC 패킷 논블로킹 수신 ---
            int bytes_read = 0;
            while ((bytes_read = mixer_ipc.receive_packet(ipc_rx_buf, sizeof(ipc_rx_buf))) > 0) {
                uint8_t pkt_type = ipc_rx_buf[0];

                if (pkt_type == PKT_TYPE_CONTROL && bytes_read >= static_cast<int>(sizeof(ControlPacket))) {
                    ControlPacket* cp = reinterpret_cast<ControlPacket*>(ipc_rx_buf);
                    int gui_dragging = gui_window.get_dragging_fader();
                    // 5채널 물리 페이더 동기화 (GUI에서 직접 드래그 중인 채널은 덮어쓰지 않음)
                    for (int ch = 0; ch < 5; ++ch) {
                        if (gui_dragging != ch) {
                            float pos = cp->fader_adc[ch] / 1023.0f;
                            param_mgr.set_fader_direct(ch, pos);
                            mixer.set_fader(ch, pos);
                        }
                    }
                    // 물리 버튼 반영
                    afs->set_enabled(cp->btn_afs != 0);
                    stem_separator.set_enabled(cp->btn_stem != 0);
                    loudness.set_enabled(cp->btn_loud != 0);
                } else if (pkt_type == PKT_TYPE_AUDIO_BEAT) {
                    AudioPacketHeader* hdr = reinterpret_cast<AudioPacketHeader*>(ipc_rx_buf);
                    const float* fData = reinterpret_cast<const float*>(ipc_rx_buf + sizeof(AudioPacketHeader));
                    hw_beat_ring_buffer.write(fData, hdr->num_frames);
                } else if (pkt_type == PKT_TYPE_AUDIO_LINE2) {
                    AudioPacketHeader* hdr = reinterpret_cast<AudioPacketHeader*>(ipc_rx_buf);
                    const float* fData = reinterpret_cast<const float*>(ipc_rx_buf + sizeof(AudioPacketHeader));
                    hw_line2_ring_buffer.write(fData, hdr->num_frames);
                }
            }

            // --- B. 보컬(Vocal) 마이크 입력 획득 ---
            bool is_file_active = (use_audio_file.load() && file_player.is_playing());

            if (use_real_mic.load()) {
                // 실제 마이크 스트림에서 보컬 오디오 획득
                mic_ring_buffer.read(in_block.data(), block_size);
            } else if (!is_file_active) {
                // 음악 파일이 재생 중이지 않을 때만 가상 보컬 신호 생성 (음성 기본 주파수 440Hz 및 배음)
                for (size_t i = 0; i < block_size; ++i) {
                    float sample = 0.08f * std::sin(2.0f * PI * 440.0f * t)
                                 + 0.03f * std::sin(2.0f * PI * 880.0f * t);
                    t += dt;
                    in_block[i * 2] = sample;
                    in_block[i * 2 + 1] = sample;
                }
            } else {
                // 음악 파일 재생 중이고 마이크가 연결되지 않았으면 CH3(Mic) 채널은 무음(0) 처리
                std::fill(in_block.begin(), in_block.end(), 0.0f);
            }

            // 하울링 주입 테스트 활성화 시 피드백 정현파 가산
            if (inject_howling.load()) {
                size_t rem = howl_samples_remaining.load();
                if (rem > 0) {
                    size_t chunk = std::min(block_size, rem);
                    float phase_step = 2.0f * PI * howl_freq / sample_rate;
                    for (size_t i = 0; i < chunk; ++i) {
                        float h_sample = 0.75f * std::sin(howl_phase);
                        howl_phase += phase_step;
                        in_block[i * 2] += h_sample;
                        in_block[i * 2 + 1] += h_sample;
                    }
                    howl_samples_remaining.fetch_sub(chunk);
                } else {
                    inject_howling.store(false);
                }
            }

            // --- B-2. AFS 하울링 자동 억제 (마이크 입력 채널에 적용) ---
            // 마이크 하울링 억제를 마이크 입력단에 적용하여 음악 원본 음질(노치 필터로 인한 먹먹함)을 100% 보존
            afs->process(in_block.data(), block_size);

            // --- C. 스템 분리 (S-FR-4) ---
            if (is_file_active) {
                if (file_player.has_ai_stems()) {
                    // Demucs AI 사전 분리 스템 완벽 동기화 재생
                    file_player.read_frames_stems(vocal_buf.data(), beat_buf.data(), inst_buf.data(), block_size);
                    for (size_t i = 0; i < block_size * 2; ++i) {
                        file_block[i] = vocal_buf[i] + beat_buf[i] + inst_buf[i];
                    }
                } else {
                    // 실시간 STFT 분리 (COLA 50% Overlap-Add)
                    file_player.read_frames(file_block.data(), block_size);
                    stem_separator.process(file_block.data(), vocal_buf.data(), beat_buf.data(), inst_buf.data(), block_size);
                }
            } else {
                stem_separator.process(in_block.data(), vocal_buf.data(), beat_buf.data(), inst_buf.data(), block_size);

                // 하드웨어 시뮬레이터로부터 비트/드럼 오디오가 수신되고 있으면 CH 2에 우선 매핑
                if (hw_beat_ring_buffer.get_available() >= block_size) {
                    hw_beat_ring_buffer.read(beat_buf.data(), block_size);
                }

                // 하드웨어 시뮬레이터로부터 라인 2(악기/코드) 오디오 수신하여 CH 4에 매핑
                if (hw_line2_ring_buffer.get_available() >= block_size) {
                    hw_line2_ring_buffer.read(line2_buf.data(), block_size);
                } else {
                    std::fill(line2_buf.begin(), line2_buf.end(), 0.0f);
                }
            }

            // --- D. 5채널 믹서 입력 라우팅 (S-FR-1) ---
            // 음악 재생 모드:
            //   CH0: Master (원곡 전체)
            //   CH1: Vocal Stem (음악 보컬 -> 페이더 내리면 노래방 반주 MR!)
            //   CH2: Beat Stem (음악 드럼/베이스)
            //   CH3: Mic In 1 (사용자 실시간 마이크 -> 반주에 맞춰 노래 부르기)
            //   CH4: Line In 2 / Inst Stem (음악 악기/코드 반주)
            // 일반 모드:
            //   CH0: Master Bus (마이크 원음)
            //   CH1: Vocal Stem (마이크 보컬)
            //   CH2: Beat Stem (HW 시뮬레이터 비트)
            //   CH3: Mic In 1 (마이크 원음)
            //   CH4: Line In 2 (HW 시뮬레이터 코드)
            std::vector<const float*> mixer_inputs = {
                is_file_active ? file_block.data() : in_block.data(), // CH0: Master
                vocal_buf.data(),                                     // CH1: Vocal
                beat_buf.data(),                                      // CH2: Beat
                in_block.data(),                                      // CH3: Mic In 1
                is_file_active ? inst_buf.data() : line2_buf.data()   // CH4: Line In 2 / Inst
            };

            // 채널별 RMS 레벨 측정 (VU 미터용)
            for (int ch = 0; ch < 5; ++ch) {
                const float* src = mixer_inputs[ch];
                float sum_sq = 0.0f;
                for (size_t i = 0; i < block_size * 2; ++i) {
                    sum_sq += src[i] * src[i];
                }
                float rms = std::sqrt(sum_sq / (block_size * 2));
                vu_levels[ch] = rms * mixer.get_channel(ch).get_gain_linear();
            }

            // 믹싱 합산
            mixer.mix(mixer_inputs, mixed_out.data(), block_size);

            // --- F. 라우드니스 매치 & 피크 리미터 (S-FR-5) ---
            loudness.process(mixed_out.data(), block_size);

            // --- F-2. 실시간 오디오 스피커 출력 (WASAPI Render) ---
            wasapi_render.write_samples(mixed_out.data(), block_size);

            // --- G. 16밴드 스펙트럼 계산 및 GUI 전달 (S-FR-6, PR-3) ---
            auto bars = spectrum.compute_bars(mixed_out.data(), block_size);
            gui_window.update_audio_data(bars, vu_levels);

            // --- H. 하드웨어 시뮬레이터로 피드백 전송 (LED 상태, 5채널 페이더 동기화 & OLED 화면 버퍼) ---
            fb_pkt.led_afs = afs->is_enabled() ? 1 : 0;
            fb_pkt.led_stem = stem_separator.is_enabled() ? 1 : 0;
            fb_pkt.led_loud = loudness.is_enabled() ? 1 : 0;
            for (int ch = 0; ch < 5; ++ch) {
                fb_pkt.fader_adc[ch] = param_mgr.get_fader_adc(ch);
            }
            fb_pkt.active_channel = static_cast<int8_t>(gui_window.get_dragging_fader());
            spectrum.generate_oled_buffer(bars, fb_pkt.oled_framebuffer);
            mixer_ipc.send_packet(HardwareIpcSocket::SIMULATOR_PORT, &fb_pkt, sizeof(fb_pkt));
        }
    });

    // 7. Windows 메시지 루프 (UI 스레드)
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 8. 종료 시 정리
    is_running.store(false);
    wasapi.stop();
    wasapi_render.stop();
    mixer_ipc.close();
    if (audio_thread.joinable()) {
        audio_thread.join();
    }

    // 고해상도 타이머 해제
    timeEndPeriod(1);

    return 0;
}
