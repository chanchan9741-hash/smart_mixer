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
#include <algorithm>
#include <cmath>
#include <cstring>

#include "../include/hardware_ipc.hpp"
#include "../include/hardware_audio_synth.hpp"
#include "../include/hardware_sim_window.hpp"
#include "../include/ring_buffer.hpp"
#include "../include/wasapi_capture.hpp"
#include "../include/wasapi_render.hpp"
#include "../../stm32_firmware/stm32_mixer_app.hpp"

using namespace SmartMixer;

namespace {
HardwareIpcSocket* g_sim_ipc = nullptr;
HardwareSimWindow* g_sim_win = nullptr;
std::chrono::steady_clock::time_point g_boot_tp;

void sim_tx_vcp(const char* data, uint16_t len) {
    if (!g_sim_ipc || !data || len == 0) return;
    VcpCommandPacket pkt;
    std::memset(&pkt, 0, sizeof(pkt));
    pkt.packet_type = PKT_TYPE_VCP_COMMAND;
    uint16_t copy_len = std::min<uint16_t>(len, static_cast<uint16_t>(sizeof(pkt.text) - 1));
    pkt.len = copy_len;
    std::memcpy(pkt.text, data, copy_len);
    g_sim_ipc->send_packet(HardwareIpcSocket::MIXER_PORT, &pkt, sizeof(pkt));
}

void sim_set_leds(uint8_t afs, uint8_t stem, uint8_t loud) {
    if (g_sim_win) {
        g_sim_win->set_mcu_leds(afs, stem, loud);
    }
}

void sim_oled_flush(const uint8_t* framebuffer_1024b) {
    if (g_sim_win) {
        g_sim_win->set_mcu_oled(framebuffer_1024b);
    }
}

uint32_t sim_get_tick_ms() {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g_boot_tp);
    return static_cast<uint32_t>(elapsed.count());
}
} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    timeBeginPeriod(1);
    g_boot_tp = std::chrono::steady_clock::now();

    // 1. IPC 소켓 초기화 및 시뮬레이터 포트(9124) 바인딩
    HardwareIpcSocket ipc;
    if (!ipc.bind_port(HardwareIpcSocket::SIMULATOR_PORT)) {
        MessageBoxW(nullptr, L"Failed to bind Hardware Simulator IPC port (9124)!", L"Error", MB_ICONERROR);
        timeEndPeriod(1);
        return 1;
    }

    // 2. 기기 전용 오디오 I/O (CH3 마이크 캡처 & 선택적 직접 모니터 스피커) + 반주 합성기
    WasapiCaptureEngine hw_mic_cap;
    WasapiRenderEngine hw_spk_render;
    AudioRingBuffer hw_mic_ring(16384, 2);
    HardwareAudioSynthesizer synth(44100);

    HardwareSimWindow sim_window(hInstance, synth, ipc, hw_mic_cap, hw_spk_render);
    if (!sim_window.create(960, 620)) {
        MessageBoxW(nullptr, L"Failed to create Hardware Simulator Window!", L"Error", MB_ICONERROR);
        timeEndPeriod(1);
        return 1;
    }

    // 0: 가상 보컬 (440Hz), 1: 음소거, 2: 실제 WASAPI 마이크
    std::atomic<int> hw_mic_mode(0);
    std::atomic<bool> inject_howl(false);
    std::atomic<size_t> howl_rem_samples(0);
    std::atomic<float> howl_phase(0.0f);

    auto trigger_howling = [&]() {
        howl_phase.store(0.0f);
        howl_rem_samples.store(static_cast<size_t>(44100 * 0.8f));
        inject_howl.store(true);
    };

    sim_window.set_mic_selected_callback([&](int sel) {
        if (sel == 0) {
            hw_mic_cap.stop();
            hw_mic_ring.clear();
            hw_mic_mode.store(0);
        } else if (sel == 1) {
            hw_mic_cap.stop();
            hw_mic_ring.clear();
            hw_mic_mode.store(1);
        } else {
            int dev_idx = sel - 2;
            hw_mic_ring.clear();
            bool ok = hw_mic_cap.start_device(dev_idx, [&](const float* data, size_t frames) {
                hw_mic_ring.write(data, frames);
            });
            hw_mic_mode.store(ok ? 2 : 0);
        }
    });

    sim_window.set_spk_selected_callback([&](int sel) {
        if (sel == 0) {
            hw_spk_render.stop();
        } else {
            hw_spk_render.start_device(sel - 1);
        }
    });

    sim_window.set_howl_inject_callback([&]() {
        trigger_howling();
    });

    // 3. 실제 STM32H743 온보드 펌웨어 인스턴스 초기화 (Zero-Heap, 155KB 정적 SRAM)
    g_sim_ipc = &ipc;
    g_sim_win = &sim_window;

    SmartMixerMCU::Stm32MixerFirmware mcu;
    HwCallbacks_t hal_cb;
    hal_cb.tx_vcp = sim_tx_vcp;
    hal_cb.set_leds = sim_set_leds;
    hal_cb.oled_flush = sim_oled_flush;
    hal_cb.get_tick_ms = sim_get_tick_ms;
    mcu.init(hal_cb);

    // PC에서 전송된 HTDemucs AI 스템 수신 버퍼
    AudioRingBuffer host_vocal_ring(16384, 2);
    AudioRingBuffer host_beat_ring(16384, 2);
    AudioRingBuffer host_inst_ring(16384, 2);

    // 4. 백그라운드 실시간 STM32 DSP & 기기 마이크/가상 스피커 스레드
    std::atomic<bool> is_running(true);
    std::thread audio_stream_thread([&]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        constexpr size_t BLOCK_SIZE = SmartMixerMCU::Stm32MixerFirmware::BLOCK_SIZE;
        constexpr auto BLOCK_PERIOD = std::chrono::microseconds(11610);
        constexpr float PI = 3.14159265358979323846f;

        float virt_t = 0.0f;
        const float dt = 1.0f / 44100.0f;
        float h_phase = 0.0f;

        std::vector<float> mic_f(BLOCK_SIZE * 2, 0.0f);
        std::vector<float> beat_f(BLOCK_SIZE * 2, 0.0f);
        std::vector<float> line2_f(BLOCK_SIZE * 2, 0.0f);
        std::vector<float> ext_v_f(BLOCK_SIZE * 2, 0.0f);
        std::vector<float> ext_b_f(BLOCK_SIZE * 2, 0.0f);
        std::vector<float> ext_i_f(BLOCK_SIZE * 2, 0.0f);
        std::vector<float> spk_out_f(BLOCK_SIZE * 2, 0.0f);

        std::vector<int16_t> mic_i16(BLOCK_SIZE * 2, 0);
        std::vector<int16_t> beat_i16(BLOCK_SIZE * 2, 0);
        std::vector<int16_t> line2_i16(BLOCK_SIZE * 2, 0);
        std::vector<int16_t> ext_v_i16(BLOCK_SIZE * 2, 0);
        std::vector<int16_t> ext_b_i16(BLOCK_SIZE * 2, 0);
        std::vector<int16_t> ext_i_i16(BLOCK_SIZE * 2, 0);
        std::vector<int16_t> mixed_out_i16(BLOCK_SIZE * 2, 0);
        std::vector<uint8_t> rx_buffer(16384);

        auto last_host_msg_tp = std::chrono::steady_clock::now() - std::chrono::seconds(2);
        auto last_host_stems_tp = std::chrono::steady_clock::now() - std::chrono::seconds(2);
        auto last_oled_tp = std::chrono::steady_clock::now();
        auto next_block_tp = std::chrono::steady_clock::now();

        auto drain_host_packets = [&]() {
            int bytes_read = 0;
            while ((bytes_read = ipc.receive_packet(rx_buffer.data(), static_cast<int>(rx_buffer.size()))) > 0) {
                uint8_t pkt_type = rx_buffer[0];
                last_host_msg_tp = std::chrono::steady_clock::now();

                if (pkt_type == PKT_TYPE_VCP_COMMAND && bytes_read >= static_cast<int>(sizeof(uint8_t) + sizeof(uint16_t))) {
                    const VcpCommandPacket* vp = reinterpret_cast<const VcpCommandPacket*>(rx_buffer.data());
                    uint16_t vlen = std::min<uint16_t>(vp->len, static_cast<uint16_t>(sizeof(vp->text) - 1));
                    if (vlen >= 13 && std::strncmp(vp->text, "[INJECT_HOWL]", 13) == 0) {
                        trigger_howling();
                    } else {
                        mcu.on_vcp_rx_bytes(reinterpret_cast<const uint8_t*>(vp->text), vlen);
                        if (vlen > 7 && std::strncmp(vp->text, "[FADER:", 7) == 0) {
                            int c0, c1, c2, c3, c4;
                            if (std::sscanf(vp->text + 7, "%d,%d,%d,%d,%d", &c0, &c1, &c2, &c3, &c4) == 5) {
                                uint16_t adcs[5] = {
                                    static_cast<uint16_t>(std::clamp(c0, 0, 1023)),
                                    static_cast<uint16_t>(std::clamp(c1, 0, 1023)),
                                    static_cast<uint16_t>(std::clamp(c2, 0, 1023)),
                                    static_cast<uint16_t>(std::clamp(c3, 0, 1023)),
                                    static_cast<uint16_t>(std::clamp(c4, 0, 1023))
                                };
                                sim_window.sync_faders_from_host(adcs, -1);
                            }
                        }
                    }
                } else if (pkt_type == PKT_TYPE_FEEDBACK && bytes_read >= static_cast<int>(sizeof(FeedbackPacket))) {
                    const FeedbackPacket* fb = reinterpret_cast<const FeedbackPacket*>(rx_buffer.data());
                    sim_window.sync_faders_from_host(fb->fader_adc, fb->active_channel);
                    char cmd[96];
                    int n = std::snprintf(cmd, sizeof(cmd), "[FADER:%u,%u,%u,%u,%u]\n",
                                          fb->fader_adc[0], fb->fader_adc[1], fb->fader_adc[2],
                                          fb->fader_adc[3], fb->fader_adc[4]);
                    if (n > 0) mcu.on_vcp_rx_bytes(reinterpret_cast<const uint8_t*>(cmd), static_cast<uint16_t>(n));
                } else if (pkt_type == PKT_TYPE_HOST_AUDIO_BLOCK &&
                           bytes_read >= static_cast<int>(sizeof(HostAudioBlockPacket))) {
                    const HostAudioBlockPacket* hp = reinterpret_cast<const HostAudioBlockPacket*>(rx_buffer.data());
                    if (hp->use_external_stems != 0 && host_vocal_ring.get_available() < 4096) {
                        last_host_stems_tp = std::chrono::steady_clock::now();
                        size_t nf = std::min<size_t>(hp->num_frames, BLOCK_SIZE);
                        for (size_t i = 0; i < nf * 2; ++i) {
                            ext_v_f[i] = static_cast<float>(hp->vocal_i16[i]) / 32768.0f;
                            ext_b_f[i] = static_cast<float>(hp->beat_i16[i]) / 32768.0f;
                            ext_i_f[i] = static_cast<float>(hp->inst_i16[i]) / 32768.0f;
                        }
                        host_vocal_ring.write(ext_v_f.data(), nf);
                        host_beat_ring.write(ext_b_f.data(), nf);
                        host_inst_ring.write(ext_i_f.data(), nf);
                    }
                }
            }
        };

        while (is_running) {
            auto now = std::chrono::steady_clock::now();

            // A. 하드웨어 패널 스캔 -> STM32 HAL 입력
            RawHwInputs hw_in = sim_window.snapshot_inputs();
            mcu.on_adc_dma_scan(hw_in.fader_adc, hw_in.eq_adc, 1023);
            mcu.on_gpio_buttons(hw_in.btn_afs_down, hw_in.btn_stem_down, hw_in.btn_loud_down, hw_in.encoder_delta);

            // B. PC 호스트 VCP/스템 패킷 수신
            drain_host_packets();

            // C. 블록 페이싱 (실제 마이크 연결 시 마이크 링버퍼 기준, 미연결 시 정밀 11.61ms 주기)
            int mic_mode = hw_mic_mode.load();
            if (mic_mode == 2) {
                if (hw_mic_ring.get_available() < BLOCK_SIZE) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                hw_mic_ring.read(mic_f.data(), BLOCK_SIZE);
                next_block_tp = std::chrono::steady_clock::now() + BLOCK_PERIOD;
            } else {
                if (now < next_block_tp) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                next_block_tp += BLOCK_PERIOD;
                if (next_block_tp < now) next_block_tp = now + BLOCK_PERIOD;

                if (mic_mode == 0) {
                    for (size_t i = 0; i < BLOCK_SIZE; ++i) {
                        float sample = 0.08f * std::sin(2.0f * PI * 440.0f * virt_t)
                                     + 0.03f * std::sin(2.0f * PI * 880.0f * virt_t);
                        virt_t += dt;
                        mic_f[i * 2] = sample;
                        mic_f[i * 2 + 1] = sample;
                    }
                } else {
                    std::fill(mic_f.begin(), mic_f.end(), 0.0f);
                }
            }

            // D. 기기 마이크 ADC 입력단에 2.5kHz 하울링 주입 테스트
            if (inject_howl.load()) {
                size_t rem = howl_rem_samples.load();
                if (rem > 0) {
                    size_t chunk = std::min(BLOCK_SIZE, rem);
                    float step = 2.0f * PI * 2500.0f / 44100.0f;
                    for (size_t i = 0; i < chunk; ++i) {
                        float h = 0.75f * std::sin(h_phase);
                        h_phase += step;
                        mic_f[i * 2] += h;
                        mic_f[i * 2 + 1] += h;
                    }
                    howl_rem_samples.fetch_sub(chunk);
                } else {
                    inject_howl.store(false);
                }
            }

            float mic_sq = 0.0f;
            for (size_t i = 0; i < BLOCK_SIZE * 2; ++i) {
                mic_sq += mic_f[i] * mic_f[i];
                mic_i16[i] = static_cast<int16_t>(std::clamp(mic_f[i], -1.0f, 1.0f) * 32767.0f);
            }
            float mic_rms = std::sqrt(mic_sq / static_cast<float>(BLOCK_SIZE * 2));

            // E. AI 스템(PC 전송분) 또는 기기 내장 반주 합성기 준비
            bool use_ext = (now - last_host_stems_tp) < std::chrono::milliseconds(250) &&
                           host_vocal_ring.get_available() >= BLOCK_SIZE;
            if (use_ext) {
                host_vocal_ring.read(ext_v_f.data(), BLOCK_SIZE);
                host_beat_ring.read(ext_b_f.data(), BLOCK_SIZE);
                host_inst_ring.read(ext_i_f.data(), BLOCK_SIZE);
                for (size_t i = 0; i < BLOCK_SIZE * 2; ++i) {
                    ext_v_i16[i] = static_cast<int16_t>(std::clamp(ext_v_f[i], -1.0f, 1.0f) * 32767.0f);
                    ext_b_i16[i] = static_cast<int16_t>(std::clamp(ext_b_f[i], -1.0f, 1.0f) * 32767.0f);
                    ext_i_i16[i] = static_cast<int16_t>(std::clamp(ext_i_f[i], -1.0f, 1.0f) * 32767.0f);
                }
            } else {
                synth.render_beat_block(beat_f.data(), BLOCK_SIZE);
                synth.render_line2_block(line2_f.data(), BLOCK_SIZE);
                for (size_t i = 0; i < BLOCK_SIZE * 2; ++i) {
                    beat_i16[i] = static_cast<int16_t>(std::clamp(beat_f[i], -1.0f, 1.0f) * 32767.0f);
                    line2_i16[i] = static_cast<int16_t>(std::clamp(line2_f[i], -1.0f, 1.0f) * 32767.0f);
                }
            }

            // F. STM32 온보드 DSP(AFS + STFT/AI 스템 + 5ch Mixer + 3-Band EQ + LUFS + Limiter) 실행
            mcu.process_multitrack_audio_block(
                mic_i16.data(),
                use_ext ? ext_v_i16.data() : nullptr,
                use_ext ? ext_b_i16.data() : beat_i16.data(),
                use_ext ? ext_i_i16.data() : line2_i16.data(),
                mixed_out_i16.data(),
                BLOCK_SIZE,
                use_ext
            );

            // G. OLED 30ms 주기 틱 & 가상 스피커 출력 계산
            if (now - last_oled_tp >= std::chrono::milliseconds(30)) {
                mcu.on_ui_tick();
                last_oled_tp = now;
            }

            float pk_l = 0.0f, pk_r = 0.0f;
            for (size_t i = 0; i < BLOCK_SIZE; ++i) {
                float fl = static_cast<float>(mixed_out_i16[i * 2]) / 32768.0f;
                float fr = static_cast<float>(mixed_out_i16[i * 2 + 1]) / 32768.0f;
                spk_out_f[i * 2] = fl;
                spk_out_f[i * 2 + 1] = fr;
                pk_l = std::max(pk_l, std::fabs(fl));
                pk_r = std::max(pk_r, std::fabs(fr));
            }
            if (hw_spk_render.is_running()) {
                hw_spk_render.write_samples(spk_out_f.data(), BLOCK_SIZE);
            }

            // H. 가상 스피커 출력을 PC 소프트웨어(smart_mixer_gui.exe) 인풋으로 상시 스트리밍
            HwMixedAudioPacket out_pkt;
            std::memset(&out_pkt, 0, sizeof(out_pkt));
            out_pkt.packet_type = PKT_TYPE_HW_MIXED_AUDIO;
            out_pkt.num_frames = static_cast<uint16_t>(BLOCK_SIZE);
            out_pkt.led_afs = mcu.get_led_afs() ? 1 : 0;
            out_pkt.led_stem = mcu.get_led_stem() ? 1 : 0;
            out_pkt.led_loud = mcu.get_led_loud() ? 1 : 0;
            out_pkt.active_notches = static_cast<uint8_t>(mcu.get_afs().get_active_filter_count());
            out_pkt.suppressed_count = mcu.get_afs().get_suppressed_count();
            out_pkt.measured_lufs = mcu.get_loudness().get_measured_lufs();
            out_pkt.target_lufs = mcu.get_loudness().get_target_lufs();
            out_pkt.auto_gain_db = mcu.get_loudness().get_current_gain_db();
            for (int ch = 0; ch < 5; ++ch) {
                out_pkt.vu_levels[ch] = mcu.get_vu_level(ch);
                out_pkt.fader_adc[ch] = mcu.get_fader_adc_smoothed(ch);
            }
            std::memcpy(out_pkt.spectrum_bars, mcu.get_oled().get_bars(), 16 * sizeof(float));
            std::memcpy(out_pkt.mixed_out_i16, mixed_out_i16.data(), BLOCK_SIZE * 2 * sizeof(int16_t));
            ipc.send_packet(HardwareIpcSocket::MIXER_PORT, &out_pkt, sizeof(out_pkt));

            // I. 시뮬레이터 UI 가상 스피커 파형/레벨 & MCU 텔레메트리 반영
            McuUiTelemetry tel;
            tel.led_afs = out_pkt.led_afs;
            tel.led_stem = out_pkt.led_stem;
            tel.led_loud = out_pkt.led_loud;
            tel.active_notches = out_pkt.active_notches;
            tel.suppressed_count = out_pkt.suppressed_count;
            tel.measured_lufs = out_pkt.measured_lufs;
            tel.target_lufs = out_pkt.target_lufs;
            tel.eq_db[0] = mcu.get_eq_db(0);
            tel.eq_db[1] = mcu.get_eq_db(1);
            tel.eq_db[2] = mcu.get_eq_db(2);
            tel.mic_rms = mic_rms;
            tel.out_peak_l = pk_l;
            tel.out_peak_r = pk_r;
            for (int i = 0; i < 32; ++i) {
                size_t s = i * (BLOCK_SIZE / 32);
                tel.spk_wave[i] = 0.5f * (spk_out_f[s * 2] + spk_out_f[s * 2 + 1]);
            }
            tel.pc_host_linked = (now - last_host_msg_tp) < std::chrono::milliseconds(400);
            tel.howling_alert = mcu.is_howling_alert();

            uint16_t smoothed[5];
            for (int ch = 0; ch < 5; ++ch) smoothed[ch] = out_pkt.fader_adc[ch];
            sim_window.update_mcu_telemetry(tel, smoothed);
        }
    });

    // 5. Windows 메시지 루프
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 6. 종료 시 정리
    is_running.store(false);
    hw_mic_cap.stop();
    hw_spk_render.stop();
    if (audio_stream_thread.joinable()) {
        audio_stream_thread.join();
    }
    ipc.close();
    g_sim_ipc = nullptr;
    g_sim_win = nullptr;
    timeEndPeriod(1);

    return 0;
}
