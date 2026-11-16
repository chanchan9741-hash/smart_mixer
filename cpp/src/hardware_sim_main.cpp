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
#include <cstring>

#include "../include/hardware_ipc.hpp"
#include "../include/hardware_audio_synth.hpp"
#include "../include/hardware_sim_window.hpp"
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

    // 2. 오디오 반주 합성기 (CH2 비트 & CH4 라인 2) & 물리 패널 UI 생성
    HardwareAudioSynthesizer synth(44100);
    HardwareSimWindow sim_window(hInstance, synth, ipc);
    if (!sim_window.create(960, 620)) {
        MessageBoxW(nullptr, L"Failed to create Hardware Simulator Window!", L"Error", MB_ICONERROR);
        timeEndPeriod(1);
        return 1;
    }

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

    // 4. 백그라운드 실시간 STM32 DSP & HAL 스캔 스레드
    std::atomic<bool> is_running(true);
    std::thread audio_stream_thread([&]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        constexpr size_t BLOCK_SIZE = SmartMixerMCU::Stm32MixerFirmware::BLOCK_SIZE;
        constexpr auto STANDALONE_PERIOD = std::chrono::microseconds(11610);

        std::vector<float> beat_f(BLOCK_SIZE * 2, 0.0f);
        std::vector<float> line2_f(BLOCK_SIZE * 2, 0.0f);
        std::vector<int16_t> beat_i16(BLOCK_SIZE * 2, 0);
        std::vector<int16_t> line2_i16(BLOCK_SIZE * 2, 0);
        std::vector<int16_t> standalone_line_i16(BLOCK_SIZE * 2, 0);
        std::vector<int16_t> mixed_out_i16(BLOCK_SIZE * 2, 0);
        std::vector<uint8_t> rx_buffer(16384);

        auto last_host_audio_tp = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        auto last_standalone_tp = std::chrono::steady_clock::now();
        auto last_oled_tp = std::chrono::steady_clock::now();

        auto render_synth_pair = [&]() {
            synth.render_beat_block(beat_f.data(), BLOCK_SIZE);
            synth.render_line2_block(line2_f.data(), BLOCK_SIZE);
            for (size_t i = 0; i < BLOCK_SIZE * 2; ++i) {
                float b = std::clamp(beat_f[i], -1.0f, 1.0f);
                float l = std::clamp(line2_f[i], -1.0f, 1.0f);
                beat_i16[i] = static_cast<int16_t>(b * 32767.0f);
                line2_i16[i] = static_cast<int16_t>(l * 32767.0f);
                float comb = std::clamp(b + l, -1.0f, 1.0f);
                standalone_line_i16[i] = static_cast<int16_t>(comb * 32767.0f);
            }
        };

        while (is_running) {
            auto now = std::chrono::steady_clock::now();

            // A. 하드웨어 패널(ADC 페이더/EQ 노브, GPIO 택트 버튼, EC11 엔코더) 스캔 -> STM32 HAL 입력
            RawHwInputs hw_in = sim_window.snapshot_inputs();
            mcu.on_adc_dma_scan(hw_in.fader_adc, hw_in.eq_adc, 1023);
            mcu.on_gpio_buttons(hw_in.btn_afs_down, hw_in.btn_stem_down, hw_in.btn_loud_down, hw_in.encoder_delta);

            // B. PC 호스트로부터 수신되는 VCP 명령 및 오디오 블록 처리
            int bytes_read = 0;
            while ((bytes_read = ipc.receive_packet(rx_buffer.data(), static_cast<int>(rx_buffer.size()))) > 0) {
                uint8_t pkt_type = rx_buffer[0];

                if (pkt_type == PKT_TYPE_VCP_COMMAND && bytes_read >= static_cast<int>(sizeof(uint8_t) + sizeof(uint16_t))) {
                    const VcpCommandPacket* vp = reinterpret_cast<const VcpCommandPacket*>(rx_buffer.data());
                    uint16_t vlen = std::min<uint16_t>(vp->len, static_cast<uint16_t>(sizeof(vp->text) - 1));
                    mcu.on_vcp_rx_bytes(reinterpret_cast<const uint8_t*>(vp->text), vlen);

                    // PC GUI 슬라이더 드래그 동기화 ([FADER:...]) 시 물리 모터 페이더도 함께 이동
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
                    last_host_audio_tp = now;

                    // 1. 하드웨어 내장 반주 합성기 렌더링
                    render_synth_pair();

                    // 2. STM32 온보드 DSP(AFS + STFT/AI 스템 라우팅 + 5ch Gain-Ramp Mixer + 3-Band EQ + LUFS + Limiter) 실행
                    bool ext_stems = (hp->use_external_stems != 0);
                    mcu.process_multitrack_audio_block(
                        hp->mic_i16,
                        ext_stems ? hp->vocal_i16 : nullptr,
                        ext_stems ? hp->beat_i16 : beat_i16.data(),
                        ext_stems ? hp->inst_i16 : line2_i16.data(),
                        mixed_out_i16.data(),
                        BLOCK_SIZE,
                        ext_stems
                    );

                    // 3. 약 30ms 주기로 STM32 OLED 스펙트럼 & LED 펌웨어 틱 실행
                    if (now - last_oled_tp >= std::chrono::milliseconds(30)) {
                        mcu.on_ui_tick();
                        last_oled_tp = now;
                    }

                    // 4. STM32 DSP가 최종 처리한 오디오 및 텔레메트리를 PC 호스트로 회신
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
                    const float* bars = mcu.get_oled().get_bars();
                    std::memcpy(out_pkt.spectrum_bars, bars, 16 * sizeof(float));
                    std::memcpy(out_pkt.mixed_out_i16, mixed_out_i16.data(), BLOCK_SIZE * 2 * sizeof(int16_t));

                    ipc.send_packet(HardwareIpcSocket::MIXER_PORT, &out_pkt, sizeof(out_pkt));
                }
            }

            // C. PC 호스트가 실행 중이지 않을 때(Standalone Hardware Mode): 보드 단독 11.6ms DMA 오디오/OLED 구동
            bool pc_linked = (now - last_host_audio_tp) < std::chrono::milliseconds(200);
            if (!pc_linked && (now - last_standalone_tp) >= STANDALONE_PERIOD) {
                last_standalone_tp = now;
                render_synth_pair();
                mcu.on_audio_dma_block(nullptr, nullptr, standalone_line_i16.data(), mixed_out_i16.data(), nullptr, BLOCK_SIZE);
                if (now - last_oled_tp >= std::chrono::milliseconds(30)) {
                    mcu.on_ui_tick();
                    last_oled_tp = now;
                }
            }

            // D. 시뮬레이터 창 UI에 STM32 실시간 텔레메트리 반영
            McuUiTelemetry tel;
            tel.led_afs = mcu.get_led_afs() ? 1 : 0;
            tel.led_stem = mcu.get_led_stem() ? 1 : 0;
            tel.led_loud = mcu.get_led_loud() ? 1 : 0;
            tel.active_notches = static_cast<uint8_t>(mcu.get_afs().get_active_filter_count());
            tel.suppressed_count = mcu.get_afs().get_suppressed_count();
            tel.measured_lufs = mcu.get_loudness().get_measured_lufs();
            tel.target_lufs = mcu.get_loudness().get_target_lufs();
            tel.eq_db[0] = mcu.get_eq_db(0);
            tel.eq_db[1] = mcu.get_eq_db(1);
            tel.eq_db[2] = mcu.get_eq_db(2);
            tel.pc_host_linked = pc_linked;
            tel.howling_alert = mcu.is_howling_alert();

            uint16_t smoothed[5];
            for (int ch = 0; ch < 5; ++ch) {
                smoothed[ch] = mcu.get_fader_adc_smoothed(ch);
            }
            sim_window.update_mcu_telemetry(tel, smoothed);

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
    if (audio_stream_thread.joinable()) {
        audio_stream_thread.join();
    }
    ipc.close();
    g_sim_ipc = nullptr;
    g_sim_win = nullptr;
    timeEndPeriod(1);

    return 0;
}
