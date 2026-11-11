#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace SmartMixer {

#pragma pack(push, 1)

// 패킷 타입 ID
enum PacketType : uint8_t {
    PKT_TYPE_CONTROL = 0x01,       // 페이더, 버튼, 엔코더 상태 (하드웨어 -> 메인 믹서)
    PKT_TYPE_AUDIO_BEAT = 0x02,    // 비트/드럼 채널 오디오 블록 (하드웨어 -> 메인 믹서 CH 2)
    PKT_TYPE_AUDIO_LINE2 = 0x03,   // 라인 2/악기 채널 오디오 블록 (하드웨어 -> 메인 믹서 CH 4)
    PKT_TYPE_FEEDBACK = 0x04       // OLED 화면(1024B) & LED 상태 (메인 믹서 -> 하드웨어)
};

// 제어 신호 패킷 구조체
struct ControlPacket {
    uint8_t packet_type;           // PKT_TYPE_CONTROL
    uint16_t fader_adc[5];         // 5채널 ADC (0 ~ 1023)
    int8_t eq_low;                 // -12 ~ +12 dB
    int8_t eq_mid;                 // -12 ~ +12 dB
    int8_t eq_high;                // -12 ~ +12 dB
    uint8_t btn_afs;               // 0 or 1
    uint8_t btn_stem;              // 0 or 1
    uint8_t btn_loud;              // 0 or 1
    int8_t encoder_delta;          // -1, 0, +1
    int8_t active_channel;         // 현재 하드웨어에서 드래그 중인 채널 (-1: 없음, 0~4)
};

// 오디오 스트림 패킷 헤더 (512샘플 스테레오 = 1024 float = 4096 bytes)
struct AudioPacketHeader {
    uint8_t packet_type;           // PKT_TYPE_AUDIO_BEAT or PKT_TYPE_AUDIO_LINE2
    uint16_t num_frames;           // 512
    uint8_t channels;              // 2
};

// 메인 믹서 -> 하드웨어 피드백 (LED & OLED 디스플레이 & 모터 페이더 위치 동기화)
struct FeedbackPacket {
    uint8_t packet_type;           // PKT_TYPE_FEEDBACK
    uint8_t led_afs;               // 1: ON, 0: OFF
    uint8_t led_stem;
    uint8_t led_loud;
    uint16_t fader_adc[5];         // 5채널 슬라이더 ADC (0 ~ 1023) - 소프트웨어 -> 하드웨어 동기화
    int8_t active_channel;         // 메인 믹서 GUI에서 드래그 중인 채널 (-1: 없음, 0~4)
    uint8_t oled_framebuffer[1024];// 128x64 픽셀 모노크롬 (SSD1306)
};

#pragma pack(pop)

// UDP 소켓 기반 초저지연 로컬 IPC 통신 모듈
class HardwareIpcSocket {
public:
    static constexpr int MIXER_PORT = 9123;   // 메인 믹서 수신 포트
    static constexpr int SIMULATOR_PORT = 9124; // 하드웨어 시뮬레이터 수신 포트

    HardwareIpcSocket() : m_sock(INVALID_SOCKET), m_is_open(false) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }

    ~HardwareIpcSocket() {
        close();
        WSACleanup();
    }

    bool bind_port(int port) {
        close();
        m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_sock == INVALID_SOCKET) return false;

        // 논블로킹 모드 설정
        u_long mode = 1;
        ioctlsocket(m_sock, FIONBIO, &mode);

        sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1

        if (bind(m_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            closesocket(m_sock);
            m_sock = INVALID_SOCKET;
            return false;
        }

        m_is_open = true;
        return true;
    }

    void close() {
        if (m_sock != INVALID_SOCKET) {
            closesocket(m_sock);
            m_sock = INVALID_SOCKET;
        }
        m_is_open = false;
    }

    // 패킷 송신 (target_port: MIXER_PORT or SIMULATOR_PORT)
    bool send_packet(int target_port, const void* data, int size) {
        if (!m_is_open || m_sock == INVALID_SOCKET) return false;

        sockaddr_in target_addr = {0};
        target_addr.sin_family = AF_INET;
        target_addr.sin_port = htons(target_port);
        target_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        int sent = sendto(m_sock, reinterpret_cast<const char*>(data), size, 0,
                          reinterpret_cast<sockaddr*>(&target_addr), sizeof(target_addr));
        return (sent == size);
    }

    // 논블로킹 패킷 수신 (수신 데이터 없으면 0 반환)
    int receive_packet(void* out_buffer, int max_size) {
        if (!m_is_open || m_sock == INVALID_SOCKET) return 0;

        sockaddr_in sender_addr = {0};
        int addr_len = sizeof(sender_addr);
        int bytes_read = recvfrom(m_sock, reinterpret_cast<char*>(out_buffer), max_size, 0,
                                  reinterpret_cast<sockaddr*>(&sender_addr), &addr_len);

        if (bytes_read == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) return 0;
            return -1;
        }
        return bytes_read;
    }

    bool is_open() const { return m_is_open; }

private:
    SOCKET m_sock;
    bool m_is_open;
};

} // namespace SmartMixer

