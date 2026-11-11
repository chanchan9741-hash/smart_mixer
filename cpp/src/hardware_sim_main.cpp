#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>

#include "../include/hardware_ipc.hpp"
#include "../include/hardware_audio_synth.hpp"
#include "../include/hardware_sim_window.hpp"

using namespace SmartMixer;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    // 1. IPC 소켓 초기화 및 시뮬레이터 포트(9124) 바인딩
    HardwareIpcSocket ipc;
    if (!ipc.bind_port(HardwareIpcSocket::SIMULATOR_PORT)) {
        MessageBoxW(nullptr, L"Failed to bind Hardware Simulator IPC port (9124)!", L"Error", MB_ICONERROR);
        return 1;
    }

    // 2. 오디오 반주 합성기 (CH2 비트 & CH4 라인 2)
    HardwareAudioSynthesizer synth(44100);

    // 3. 하드웨어 시뮬레이터 윈도우 생성
    HardwareSimWindow sim_window(hInstance, synth, ipc);
    if (!sim_window.create(960, 620)) {
        MessageBoxW(nullptr, L"Failed to create Hardware Simulator Window!", L"Error", MB_ICONERROR);
        return 1;
    }

    // 4. 백그라운드 실시간 오디오 스트리밍 & IPC 스레드 (512샘플 주기: ~11.6ms)
    std::atomic<bool> is_running(true);
    std::thread audio_stream_thread([&]() {
        constexpr size_t BLOCK_SIZE = 512;
        auto block_duration = std::chrono::microseconds(static_cast<int>(BLOCK_SIZE * 1000000.0 / 44100.0));

        std::vector<float> beat_block(BLOCK_SIZE * 2);
        std::vector<float> line2_block(BLOCK_SIZE * 2);

        // 오디오 패킷 버퍼 (헤더 + 512 * 2 * float = 4 + 4096 = 4100 bytes)
        std::vector<uint8_t> beat_packet_buf(sizeof(AudioPacketHeader) + BLOCK_SIZE * 2 * sizeof(float));
        std::vector<uint8_t> line2_packet_buf(sizeof(AudioPacketHeader) + BLOCK_SIZE * 2 * sizeof(float));

        AudioPacketHeader* beat_hdr = reinterpret_cast<AudioPacketHeader*>(beat_packet_buf.data());
        beat_hdr->packet_type = PKT_TYPE_AUDIO_BEAT;
        beat_hdr->num_frames = static_cast<uint16_t>(BLOCK_SIZE);
        beat_hdr->channels = 2;

        AudioPacketHeader* line2_hdr = reinterpret_cast<AudioPacketHeader*>(line2_packet_buf.data());
        line2_hdr->packet_type = PKT_TYPE_AUDIO_LINE2;
        line2_hdr->num_frames = static_cast<uint16_t>(BLOCK_SIZE);
        line2_hdr->channels = 2;

        uint8_t rx_buffer[2048];

        while (is_running) {
            auto start_tp = std::chrono::steady_clock::now();

            // 1. Channel 2 Beat 오디오 합성 및 메인 믹서로 UDP 전송
            synth.render_beat_block(beat_block.data(), BLOCK_SIZE);
            std::memcpy(beat_packet_buf.data() + sizeof(AudioPacketHeader),
                        beat_block.data(), BLOCK_SIZE * 2 * sizeof(float));
            ipc.send_packet(HardwareIpcSocket::MIXER_PORT, beat_packet_buf.data(), static_cast<int>(beat_packet_buf.size()));

            // 2. Channel 4 Line In 2 오디오 합성 및 메인 믹서로 UDP 전송
            synth.render_line2_block(line2_block.data(), BLOCK_SIZE);
            std::memcpy(line2_packet_buf.data() + sizeof(AudioPacketHeader),
                        line2_block.data(), BLOCK_SIZE * 2 * sizeof(float));
            ipc.send_packet(HardwareIpcSocket::MIXER_PORT, line2_packet_buf.data(), static_cast<int>(line2_packet_buf.size()));

            // 3. 메인 믹서로부터 피드백 수신 (LED 상태, OLED 128x64 화면 & 5채널 모터 페이더 동기화)
            int bytes_read = 0;
            while ((bytes_read = ipc.receive_packet(rx_buffer, sizeof(rx_buffer))) > 0) {
                if (bytes_read >= static_cast<int>(sizeof(FeedbackPacket))) {
                    FeedbackPacket* fb = reinterpret_cast<FeedbackPacket*>(rx_buffer);
                    if (fb->packet_type == PKT_TYPE_FEEDBACK) {
                        sim_window.update_feedback(fb->led_afs, fb->led_stem, fb->led_loud,
                                                   fb->fader_adc, fb->active_channel, fb->oled_framebuffer);
                    }
                }
            }

            // 11.6ms 주기 동기화
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_tp);
            if (elapsed < block_duration) {
                std::this_thread::sleep_for(block_duration - elapsed);
            }
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

    return 0;
}
