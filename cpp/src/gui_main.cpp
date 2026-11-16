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
#include <cstdio>
#include <algorithm>

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
#include "../include/serial_port.hpp"
#include "../include/gui_window.hpp"

using namespace SmartMixer;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
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

    auto send_sim_vcp = [&](const std::string& cmd) {
        VcpCommandPacket vp;
        std::memset(&vp, 0, sizeof(vp));
        vp.packet_type = PKT_TYPE_VCP_COMMAND;
        uint16_t n = static_cast<uint16_t>(std::min<size_t>(cmd.size(), sizeof(vp.text) - 1));
        vp.len = n;
        std::memcpy(vp.text, cmd.data(), n);
        mixer_ipc.send_packet(HardwareIpcSocket::SIMULATOR_PORT, &vp, sizeof(vp));
    };

    // 4-2. 실제 STM32 보드 USB VCP 시리얼 연동 객체 (--com COMx 옵션 전달 시 활성)
    Win32SerialPort hw_serial(param_mgr);
    hw_serial.set_afs_event_callback([&](float freq) {
        char msg_buf[128];
        std::snprintf(msg_buf, sizeof(msg_buf), "⚠️ STM32 HW Howling Suppressed at %.0fHz!", freq);
        gui_window.show_toast(msg_buf, 3.0f);
    });
    hw_serial.set_afs_reset_callback([&]() {
        afs->clear_filters();
        gui_window.show_toast("🔄 STM32 AFS Notch Filters Reset", 2.5f);
    });

    int last_hw_encoder_val = 0;
    param_mgr.set_callback([&](const std::string& param, float val) {
        if (param.rfind("fader_", 0) == 0 && param.size() == 7) {
            int ch = param[6] - '0';
            if (ch >= 0 && ch < 5 && gui_window.get_dragging_fader() != ch) {
                mixer.set_fader(ch, val);
            }
        } else if (param == "afs_enabled") {
            afs->set_enabled(val > 0.5f);
        } else if (param == "stem_enabled") {
            stem_separator.set_enabled(val > 0.5f);
        } else if (param == "loudness_enabled") {
            loudness.set_enabled(val > 0.5f);
        } else if (param == "encoder") {
            int cur_enc = static_cast<int>(std::lround(val));
            int delta = cur_enc - last_hw_encoder_val;
            last_hw_encoder_val = cur_enc;
            if (delta != 0) {
                float next_lufs = std::clamp(loudness.get_target_lufs() + delta * 0.5f, -24.0f, -8.0f);
                loudness.set_target_lufs(next_lufs);
            }
        }
    });

    // 하울링 강제 주입 및 필터 리셋 콜백
    gui_window.set_inject_howling_callback([&]() {
        send_sim_vcp("[INJECT_HOWL]\n");
        gui_window.show_toast("⚡ 기기 마이크 입력단에 2.5kHz 하울링 신호 주입!", 2.5f);
    });

    gui_window.set_reset_filters_callback([&]() {
        afs->clear_filters();
        send_sim_vcp(Win32SerialPort::format_reset_afs_cmd());
        if (hw_serial.is_connected()) {
            hw_serial.send_reset_afs();
        }
    });

    // 슬라이더 조작 시 하드웨어 시뮬레이터 및 실제 STM32 보드로 즉각 전송 (초저지연 양방향 동기화)
    gui_window.set_fader_changed_callback([&](int ch, float norm) {
        (void)ch;
        (void)norm;
        send_sim_vcp(Win32SerialPort::format_fader_cmd(param_mgr));
        if (hw_serial.is_connected()) {
            hw_serial.send_fader_sync();
        }
    });

    gui_window.set_fader_drag_end_callback([&](int ch) {
        (void)ch;
        send_sim_vcp(Win32SerialPort::format_fader_cmd(param_mgr));
        if (hw_serial.is_connected()) {
            hw_serial.send_fader_sync();
        }
    });

    // 5. 음원 파일 로드 및 Demucs HTDemucs AI 스템 분리 파이프라인
    std::atomic<bool> use_audio_file(false);
    std::atomic<bool> is_demucs_running(false);

    auto resolve_demucs_script = []() -> std::wstring {
        wchar_t exe_buf[MAX_PATH] = {0};
        GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
        std::wstring exe_path(exe_buf);
        size_t slash = exe_path.find_last_of(L"\\/");
        std::wstring exe_dir = (slash != std::wstring::npos) ? exe_path.substr(0, slash) : L".";

        std::vector<std::wstring> candidates = {
            exe_dir + L"\\scripts\\demucs_runner.py",
            exe_dir + L"\\..\\scripts\\demucs_runner.py",
            L"scripts\\demucs_runner.py",
            L"..\\scripts\\demucs_runner.py"
        };
        for (const auto& c : candidates) {
            if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) {
                wchar_t full_buf[MAX_PATH] = {0};
                if (GetFullPathNameW(c.c_str(), MAX_PATH, full_buf, nullptr)) {
                    return std::wstring(full_buf);
                }
                return c;
            }
        }
        return exe_dir + L"\\..\\scripts\\demucs_runner.py";
    };

    auto resolve_python_exe = []() -> std::wstring {
        const wchar_t* local_app_data = _wgetenv(L"LOCALAPPDATA");
        if (local_app_data) {
            std::wstring base(local_app_data);
            std::vector<std::wstring> py_candidates = {
                base + L"\\Python\\pythoncore-3.14-64\\python.exe",
                base + L"\\Python\\bin\\python.exe",
                base + L"\\Programs\\Python\\Python314\\python.exe",
                base + L"\\Programs\\Python\\Python313\\python.exe",
                base + L"\\Programs\\Python\\Python312\\python.exe",
                base + L"\\Programs\\Python\\Python311\\python.exe",
                base + L"\\Programs\\Python\\Python310\\python.exe"
            };
            for (const auto& p : py_candidates) {
                if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    return p;
                }
            }
        }
        return L"python";
    };

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
            gui_window.set_ai_busy(true);
            gui_window.set_ai_progress(1);
            file_player.pause(); // AI 스템 분리 완료 시까지 재생 대기

            gui_window.show_toast("⏳ HTDemucs AI 스템 분리 준비 중...", 15.0f);

            std::wstring in_path = file_player.get_file_path();
            size_t slash = in_path.find_last_of(L"\\/");
            std::wstring dir = (slash != std::wstring::npos) ? in_path.substr(0, slash) : L".";
            size_t dot = in_path.find_last_of(L".");
            std::wstring base_name = (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
                                   ? in_path.substr(slash + 1, dot - slash - 1)
                                   : in_path.substr(slash + 1);

            std::wstring out_dir = dir + L"\\demucs_stems";
            CreateDirectoryW(out_dir.c_str(), nullptr);

            std::wstring prog_file = out_dir + L"\\progress.txt";
            _wremove(prog_file.c_str());

            // Media Foundation으로 디코딩된 PCM을 표준 16-bit WAV로 임시 저장 (ffmpeg 없이 MP3/M4A/FLAC 완벽 지원)
            std::wstring temp_input_wav = out_dir + L"\\" + base_name + L"_demucs_input.wav";
            if (!file_player.save_loaded_to_wav(temp_input_wav)) {
                gui_window.set_ai_busy(false);
                is_demucs_running.store(false);
                gui_window.show_toast("❌ 임시 WAV 변환 실패 (디스크 권한을 확인하세요)", 4.0f);
                return;
            }

            std::wstring script_path = resolve_demucs_script();
            std::wstring py_exe = resolve_python_exe();

            std::wstring cmd = L"\"" + py_exe + L"\" \"" + script_path + L"\" \"" + temp_input_wav + L"\" \"" + out_dir + L"\" \"" + base_name + L"\"";

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
                // 250ms 주기로 progress.txt를 폴링하여 GUI 진행률 실시간 업데이트 (최대 5분 대기)
                for (int tick = 0; tick < 1200; ++tick) {
                    DWORD wait_res = WaitForSingleObject(pi.hProcess, 250);
                    FILE* pfp = nullptr;
                    if (_wfopen_s(&pfp, prog_file.c_str(), L"r") == 0 && pfp) {
                        int pct = 0;
                        if (fscanf_s(pfp, "%d", &pct) == 1) {
                            pct = std::clamp(pct, 1, 99);
                            gui_window.set_ai_progress(pct);
                            gui_window.show_toast("⏳ HTDemucs 딥러닝 스템 분리 중... (" + std::to_string(pct) + "%)", 5.0f);
                        }
                        fclose(pfp);
                    }
                    if (wait_res == WAIT_OBJECT_0) {
                        break;
                    }
                }

                DWORD exit_code = 1;
                GetExitCodeProcess(pi.hProcess, &exit_code);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                if (exit_code == 0) {
                    success = true;
                }
            }

            _wremove(temp_input_wav.c_str());
            _wremove(prog_file.c_str());

            gui_window.set_ai_progress(100);
            gui_window.set_ai_busy(false);
            is_demucs_running.store(false);

            if (success) {
                std::wstring v_path = out_dir + L"\\" + base_name + L"_ai_vocal.wav";
                std::wstring b_path = out_dir + L"\\" + base_name + L"_ai_beat.wav";
                std::wstring i_path = out_dir + L"\\" + base_name + L"_ai_inst.wav";

                bool loaded = file_player.load_ai_stems(v_path, b_path, i_path);
                if (loaded) {
                    file_player.seek(0.0f); // 00:00부터
                    file_player.play();     // 완벽 분리된 스튜디오 음원으로 재생 시작!
                    gui_window.show_toast("🎉 HTDemucs AI 스템 분리 완료! 00:00부터 재생을 시작합니다.", 5.0f);
                } else {
                    gui_window.show_toast("❌ AI 스템 파일 로드 실패 (demucs_log.txt 확인)", 5.0f);
                }
            } else {
                gui_window.show_toast("❌ HTDemucs 분리 실패 (demucs_stems/demucs_log.txt 확인)", 5.0f);
            }
        }).detach();
    };

    gui_window.set_file_open_callback([&](const std::wstring& path) {
        bool ok = file_player.load_file(path);
        if (ok) {
            use_audio_file.store(true);
            gui_window.refresh_devices_with_file();

            // HTDemucs 전용 AI 스템(_ai_vocal.wav 등)이 이미 캐시되어 있는지 확인
            size_t slash = path.find_last_of(L"\\/");
            std::wstring dir = (slash != std::wstring::npos) ? path.substr(0, slash) : L".";
            size_t dot = path.find_last_of(L".");
            std::wstring base_name = (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
                                   ? path.substr(slash + 1, dot - slash - 1)
                                   : path.substr(slash + 1);

            std::wstring demucs_dir = dir + L"\\demucs_stems";
            std::wstring v1 = demucs_dir + L"\\" + base_name + L"_ai_vocal.wav";
            std::wstring b1 = demucs_dir + L"\\" + base_name + L"_ai_beat.wav";
            std::wstring i1 = demucs_dir + L"\\" + base_name + L"_ai_inst.wav";

            bool loaded_ai = false;
            if (GetFileAttributesW(v1.c_str()) != INVALID_FILE_ATTRIBUTES &&
                GetFileAttributesW(b1.c_str()) != INVALID_FILE_ATTRIBUTES &&
                GetFileAttributesW(i1.c_str()) != INVALID_FILE_ATTRIBUTES) {
                loaded_ai = file_player.load_ai_stems(v1, b1, i1);
            }

            if (loaded_ai) {
                gui_window.show_toast("✨ 기존 HTDemucs AI 스템 캐시 로드 완료! 즉시 재생합니다.", 4.0f);
                file_player.seek(0.0f);
                file_player.play();
            } else {
                // 즉시 재생하지 않고 대기: HTDemucs AI 스템 분리 완료 후 00:00부터 재생
                file_player.pause();
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

    // 6. PC 입력 소스 전환 핸들러 (기기 가상 스피커 입력 vs 음원 AI 스템 송출)
    // 마이크(CH 3) 입력은 하드웨어 기기(hardware_sim_gui)에서만 전담합니다.
    wasapi.stop();

    gui_window.set_device_selected_callback([&](int sel) {
        if (sel == 0) {
            use_audio_file.store(false);
            file_player.pause();
            gui_window.show_toast("PC 입력: STM32 기기 가상 스피커 스트림 수신", 2.5f);
        } else if (file_player.is_loaded() && sel == 1) {
            use_audio_file.store(true);
            file_player.play();
            gui_window.show_toast("음원(HTDemucs) -> 기기 전송 + 기기 마이크 믹싱", 2.5f);
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

    // 선택적 --com COMx 인자 파싱 (실제 STM32 USB VCP 연결 시에만 시리얼 포트 개방, 미지정 시 지연 0ms)
    if (lpCmdLine && lpCmdLine[0] != '\0') {
        std::string cmd_str(lpCmdLine);
        std::string com_port;
        size_t pos = 0;
        while (pos < cmd_str.size()) {
            while (pos < cmd_str.size() && (cmd_str[pos] == ' ' || cmd_str[pos] == '\t')) ++pos;
            if (pos >= cmd_str.size()) break;
            size_t end = pos;
            while (end < cmd_str.size() && cmd_str[end] != ' ' && cmd_str[end] != '\t') ++end;
            std::string tok = cmd_str.substr(pos, end - pos);
            pos = end;

            if (tok.rfind("--com=", 0) == 0 && tok.size() > 6) {
                com_port = tok.substr(6);
            } else if (tok == "--com") {
                while (pos < cmd_str.size() && (cmd_str[pos] == ' ' || cmd_str[pos] == '\t')) ++pos;
                size_t end2 = pos;
                while (end2 < cmd_str.size() && cmd_str[end2] != ' ' && cmd_str[end2] != '\t') ++end2;
                if (end2 > pos) {
                    com_port = cmd_str.substr(pos, end2 - pos);
                    pos = end2;
                }
            }
        }
        if (!com_port.empty()) {
            if (hw_serial.open(com_port, 115200)) {
                hw_serial.send_led_state(afs->is_enabled(), stem_separator.is_enabled(), loudness.is_enabled());
                hw_serial.send_fader_sync();
                hw_serial.send_target_lufs(loudness.get_target_lufs());
                gui_window.show_toast("STM32 Hardware Connected (" + com_port + ")", 3.0f);
            } else {
                gui_window.show_toast("STM32 Serial Open Failed (" + com_port + ") - PC Mode", 3.5f);
            }
        }
    }

    // 6. 백그라운드 실시간 오디오 DSP & 하드웨어 IPC 스레드 가동
    std::atomic<bool> is_running(true);
    std::thread audio_thread([&]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

        size_t block_size = config.audio.block_size;

        std::vector<float> in_block(block_size * 2, 0.0f);
        std::vector<float> vocal_buf(block_size * 2, 0.0f);
        std::vector<float> beat_buf(block_size * 2, 0.0f);
        std::vector<float> inst_buf(block_size * 2, 0.0f);
        std::vector<float> line2_buf(block_size * 2, 0.0f);
        std::vector<float> file_block(block_size * 2, 0.0f);
        std::vector<float> mixed_out(block_size * 2, 0.0f);
        std::vector<float> vu_levels(5, 0.0f);

        std::vector<uint8_t> ipc_rx_buf(16384);
        std::vector<float> bars(16, 0.0f);
        HostAudioBlockPacket host_audio_pkt;

        bool last_led_afs = afs->is_enabled();
        bool last_led_stem = stem_separator.is_enabled();
        bool last_led_loud = loudness.is_enabled();
        float last_target_lufs = loudness.get_target_lufs();
        bool was_file_active = false;
        auto last_hw_sim_tp = std::chrono::steady_clock::now() - std::chrono::seconds(2);

        auto push_stems_to_device = [&]() {
            if (!file_player.has_ai_stems()) return;
            file_player.read_frames_stems(vocal_buf.data(), beat_buf.data(), inst_buf.data(), block_size);
            host_audio_pkt.packet_type = PKT_TYPE_HOST_AUDIO_BLOCK;
            host_audio_pkt.use_external_stems = 1;
            host_audio_pkt.num_frames = static_cast<uint16_t>(block_size);
            for (size_t i = 0; i < block_size * 2; ++i) {
                host_audio_pkt.mic_i16[i]   = 0; // 마이크 입력은 오직 기기(hardware_sim_gui)에서만 수신
                host_audio_pkt.vocal_i16[i] = static_cast<int16_t>(std::clamp(vocal_buf[i], -1.0f, 1.0f) * 32767.0f);
                host_audio_pkt.beat_i16[i]  = static_cast<int16_t>(std::clamp(beat_buf[i], -1.0f, 1.0f) * 32767.0f);
                host_audio_pkt.inst_i16[i]  = static_cast<int16_t>(std::clamp(inst_buf[i], -1.0f, 1.0f) * 32767.0f);
            }
            mixer_ipc.send_packet(HardwareIpcSocket::SIMULATOR_PORT, &host_audio_pkt, sizeof(host_audio_pkt));
        };

        auto handle_sim_rx_packet = [&](const uint8_t* data, int bytes, std::vector<HwMixedAudioPacket>& out_packets) {
            if (bytes <= 0) return;
            uint8_t pkt_type = data[0];
            if (pkt_type == PKT_TYPE_VCP_COMMAND && bytes >= static_cast<int>(sizeof(uint8_t) + sizeof(uint16_t))) {
                const VcpCommandPacket* vp = reinterpret_cast<const VcpCommandPacket*>(data);
                uint16_t vlen = std::min<uint16_t>(vp->len, static_cast<uint16_t>(sizeof(vp->text) - 1));
                std::string raw(vp->text, vlen);
                size_t s = 0;
                while (s < raw.size()) {
                    size_t e = raw.find_first_of("\r\n", s);
                    std::string line = (e == std::string::npos) ? raw.substr(s) : raw.substr(s, e - s);
                    s = (e == std::string::npos) ? raw.size() : e + 1;
                    if (line.empty()) continue;

                    if (line == "B:AFS") {
                        afs->set_enabled(!afs->is_enabled());
                        last_led_afs = afs->is_enabled();
                    } else if (line == "B:STEM") {
                        stem_separator.set_enabled(!stem_separator.is_enabled());
                        last_led_stem = stem_separator.is_enabled();
                    } else if (line == "B:LOUD") {
                        loudness.set_enabled(!loudness.is_enabled());
                        last_led_loud = loudness.is_enabled();
                    } else if (line == "E:+1" || line == "E:-1") {
                        float d = (line == "E:+1") ? 0.5f : -0.5f;
                        loudness.set_target_lufs(std::clamp(loudness.get_target_lufs() + d, -24.0f, -8.0f));
                        last_target_lufs = loudness.get_target_lufs();
                    } else {
                        hw_serial.process_vcp_line(line);
                    }
                }
            } else if (pkt_type == PKT_TYPE_HW_MIXED_AUDIO && bytes >= static_cast<int>(sizeof(HwMixedAudioPacket))) {
                HwMixedAudioPacket pkt;
                std::memcpy(&pkt, data, sizeof(HwMixedAudioPacket));
                out_packets.push_back(pkt);
            }
        };

        std::vector<HwMixedAudioPacket> rx_mixed_batch;
        rx_mixed_batch.reserve(8);

        while (is_running) {
            // --- A. PC GUI 상태 변화(AFS/STEM/LOUD/LUFS)를 하드웨어 시뮬레이터 및 STM32 보드로 동기화 ---
            bool cur_afs = afs->is_enabled();
            bool cur_stem = stem_separator.is_enabled();
            bool cur_loud = loudness.is_enabled();
            float cur_lufs = loudness.get_target_lufs();
            if (cur_afs != last_led_afs || cur_stem != last_led_stem || cur_loud != last_led_loud) {
                send_sim_vcp(Win32SerialPort::format_led_cmd(cur_afs, cur_stem, cur_loud));
                if (hw_serial.is_connected()) {
                    hw_serial.send_led_state(cur_afs, cur_stem, cur_loud);
                }
                last_led_afs = cur_afs;
                last_led_stem = cur_stem;
                last_led_loud = cur_loud;
            }
            if (std::fabs(cur_lufs - last_target_lufs) > 0.05f) {
                send_sim_vcp(Win32SerialPort::format_lufs_cmd(cur_lufs));
                if (hw_serial.is_connected()) {
                    hw_serial.send_target_lufs(cur_lufs);
                }
                last_target_lufs = cur_lufs;
            }

            // --- B. 하드웨어 시뮬레이터 가상 스피커 출력 & VCP 패킷 수신 ---
            rx_mixed_batch.clear();
            int bytes_read = 0;
            while ((bytes_read = mixer_ipc.receive_packet(ipc_rx_buf.data(), static_cast<int>(ipc_rx_buf.size()))) > 0) {
                handle_sim_rx_packet(ipc_rx_buf.data(), bytes_read, rx_mixed_batch);
            }

            bool is_file_active = (use_audio_file.load() && file_player.is_playing());
            auto now = std::chrono::steady_clock::now();

            if (!rx_mixed_batch.empty()) {
                last_hw_sim_tp = now;

                // 음원 파일 재생 시작 시 기기 버퍼 프리필 (2블록)
                if (is_file_active && !was_file_active && file_player.has_ai_stems()) {
                    push_stems_to_device();
                    push_stems_to_device();
                }
                was_file_active = is_file_active;

                // 수신된 모든 가상 스피커 오디오 블록을 PC 출력 & 시각화 반영
                for (size_t k = 0; k < rx_mixed_batch.size(); ++k) {
                    const HwMixedAudioPacket& pkt = rx_mixed_batch[k];
                    for (size_t i = 0; i < block_size * 2; ++i) {
                        mixed_out[i] = static_cast<float>(pkt.mixed_out_i16[i]) / 32768.0f;
                    }
                    wasapi_render.write_samples(mixed_out.data(), block_size);

                    if (is_file_active && file_player.has_ai_stems()) {
                        push_stems_to_device();
                    }
                }

                const HwMixedAudioPacket& latest = rx_mixed_batch.back();
                for (int ch = 0; ch < 5; ++ch) {
                    vu_levels[ch] = latest.vu_levels[ch];
                }
                for (int b = 0; b < 16; ++b) {
                    bars[b] = latest.spectrum_bars[b];
                }
                gui_window.set_hw_dsp_telemetry(true, latest.active_notches,
                                                latest.suppressed_count,
                                                latest.measured_lufs,
                                                latest.auto_gain_db);
                gui_window.update_audio_data(bars, vu_levels);
                continue;
            }

            // 시뮬레이터 가상 스피커가 살아있으면 11.61ms 패킷 도착을 1ms 단위로 대기
            if ((now - last_hw_sim_tp) < std::chrono::milliseconds(350)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            // --- C. 시뮬레이터 미실행 시 PC 오프라인 폴백 (마이크는 기기 전용이므로 PC 마이크 = 0 무음) ---
            was_file_active = is_file_active;
            gui_window.set_hw_dsp_telemetry(false, 0, 0, -70.0f, 0.0f);

            if (wasapi_render.is_running() && wasapi_render.get_buffered_samples() >= 3072) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            std::fill(in_block.begin(), in_block.end(), 0.0f);
            if (is_file_active && file_player.has_ai_stems()) {
                file_player.read_frames_stems(vocal_buf.data(), beat_buf.data(), inst_buf.data(), block_size);
                for (size_t i = 0; i < block_size * 2; ++i) {
                    file_block[i] = vocal_buf[i] + beat_buf[i] + inst_buf[i];
                }
                if (!stem_separator.is_enabled()) {
                    std::copy(file_block.begin(), file_block.end(), vocal_buf.begin());
                    std::fill(beat_buf.begin(), beat_buf.end(), 0.0f);
                    std::fill(inst_buf.begin(), inst_buf.end(), 0.0f);
                }

                std::vector<const float*> mixer_inputs = {
                    file_block.data(),
                    vocal_buf.data(),
                    beat_buf.data(),
                    in_block.data(), // CH3 Mic = 0 (기기 전용)
                    inst_buf.data()
                };
                for (int ch = 0; ch < 5; ++ch) {
                    const float* src = mixer_inputs[ch];
                    float sum_sq = 0.0f;
                    for (size_t i = 0; i < block_size * 2; ++i) sum_sq += src[i] * src[i];
                    vu_levels[ch] = std::sqrt(sum_sq / (block_size * 2)) * mixer.get_channel(ch).get_gain_linear();
                }
                mixer.mix(mixer_inputs, mixed_out.data(), block_size);
                loudness.process(mixed_out.data(), block_size);
                bars = spectrum.compute_bars(mixed_out.data(), block_size);
                wasapi_render.write_samples(mixed_out.data(), block_size);
                gui_window.update_audio_data(bars, vu_levels);
            } else {
                std::fill(vu_levels.begin(), vu_levels.end(), 0.0f);
                std::fill(bars.begin(), bars.end(), 0.0f);
                gui_window.update_audio_data(bars, vu_levels);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
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
    hw_serial.close();
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
