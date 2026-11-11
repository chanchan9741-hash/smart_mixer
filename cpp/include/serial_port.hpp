#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <sstream>
#include <iostream>
#include "controller_mapper.hpp"

namespace SmartMixer {

class Win32SerialPort {
public:
    using PacketCallback = std::function<void(const std::string&)>;

    Win32SerialPort(ParameterManager& param_mgr)
        : m_param_mgr(param_mgr),
          m_handle(INVALID_HANDLE_VALUE),
          m_is_connected(false),
          m_is_running(false) {}

    ~Win32SerialPort() {
        close();
    }

    bool open(const std::string& port_name, int baudrate = 115200) {
        std::string full_path = "\\\\.\\" + port_name;
        m_handle = CreateFileA(
            full_path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );

        if (m_handle == INVALID_HANDLE_VALUE) {
            m_is_connected = false;
            return false;
        }

        DCB dcb = {0};
        dcb.DCBlength = sizeof(DCB);
        if (!GetCommState(m_handle, &dcb)) {
            close();
            return false;
        }

        dcb.BaudRate = baudrate;
        dcb.ByteSize = 8;
        dcb.StopBits = ONESTOPBIT;
        dcb.Parity = NOPARITY;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;

        if (!SetCommState(m_handle, &dcb)) {
            close();
            return false;
        }

        COMMTIMEOUTS timeouts = {0};
        timeouts.ReadIntervalTimeout = 50;
        timeouts.ReadTotalTimeoutConstant = 50;
        timeouts.ReadTotalTimeoutMultiplier = 10;
        timeouts.WriteTotalTimeoutConstant = 50;
        timeouts.WriteTotalTimeoutMultiplier = 10;
        SetCommTimeouts(m_handle, &timeouts);

        m_is_connected = true;
        m_is_running = true;
        m_thread = std::thread(&Win32SerialPort::read_loop, this);
        return true;
    }

    void close() {
        m_is_running = false;
        if (m_thread.joinable()) {
            m_thread.join();
        }
        if (m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
        m_is_connected = false;
    }

    bool is_connected() const { return m_is_connected; }

    // PC -> STM32: OLED 프레임버퍼 및 LED 상태 전송
    bool send_command(const std::string& cmd) {
        if (!m_is_connected || m_handle == INVALID_HANDLE_VALUE) return false;
        DWORD bytes_written = 0;
        return WriteFile(m_handle, cmd.c_str(), static_cast<DWORD>(cmd.length()), &bytes_written, nullptr);
    }

    void send_led_state(bool afs, bool stem, bool loud) {
        std::ostringstream ss;
        ss << "[LED:" << (afs ? "1" : "0") << "," << (stem ? "1" : "0") << "," << (loud ? "1" : "0") << "]\n";
        send_command(ss.str());
    }

private:
    void read_loop() {
        char buffer[256];
        std::string line_accum;

        while (m_is_running) {
            DWORD bytes_read = 0;
            if (ReadFile(m_handle, buffer, sizeof(buffer) - 1, &bytes_read, nullptr) && bytes_read > 0) {
                for (DWORD i = 0; i < bytes_read; ++i) {
                    char c = buffer[i];
                    if (c == '\n' || c == '\r') {
                        if (!line_accum.empty()) {
                            parse_packet(line_accum);
                            line_accum.clear();
                        }
                    } else {
                        line_accum += c;
                    }
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }

    // STM32 수신 패킷 파싱
    // F:1023,800,600,900,850 (5채널 ADC)
    // B:AFS, B:STEM, B:LOUD (기능 버튼)
    // E:+1, E:-1 (인코더 회전)
    void parse_packet(const std::string& packet) {
        if (packet.rfind("F:", 0) == 0) {
            std::stringstream ss(packet.substr(2));
            std::string token;
            int ch = 0;
            while (std::getline(ss, token, ',') && ch < 5) {
                try {
                    int val = std::stoi(token);
                    m_param_mgr.update_fader_adc(ch, val);
                } catch (...) {}
                ch++;
            }
        } else if (packet == "B:AFS") {
            m_param_mgr.toggle_afs();
        } else if (packet == "B:STEM") {
            m_param_mgr.toggle_stem();
        } else if (packet == "B:LOUD") {
            m_param_mgr.toggle_loudness();
        } else if (packet == "E:+1") {
            m_param_mgr.update_encoder(1);
        } else if (packet == "E:-1") {
            m_param_mgr.update_encoder(-1);
        }
    }

    ParameterManager& m_param_mgr;
    HANDLE m_handle;
    std::atomic<bool> m_is_connected;
    std::atomic<bool> m_is_running;
    std::thread m_thread;
};

} // namespace SmartMixer

