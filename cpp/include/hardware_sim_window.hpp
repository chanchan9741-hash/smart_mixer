#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <windowsx.h>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>

#include "hardware_ipc.hpp"
#include "hardware_audio_synth.hpp"

namespace SmartMixer {

class HardwareSimWindow {
public:
    HardwareSimWindow(HINSTANCE hInstance, HardwareAudioSynthesizer& synth, HardwareIpcSocket& ipc)
        : m_hInstance(hInstance),
          m_hWnd(nullptr),
          m_synth(synth),
          m_ipc(ipc),
          m_dragging_fader(-1),
          m_led_afs(1),
          m_led_stem(1),
          m_led_loud(1) {
        
        m_fader_adc.resize(5, 820); // 기본값 약 0.8 (820 / 1023)
        m_eq_knobs[0] = 0; // Low (dB)
        m_eq_knobs[1] = 0; // Mid (dB)
        m_eq_knobs[2] = 0; // High (dB)

        // OLED 프레임버퍼 (128 x 64 bits = 1024 bytes)
        m_oled_buffer.resize(1024, 0);
    }

    void update_feedback(uint8_t led_afs, uint8_t led_stem, uint8_t led_loud,
                         const uint16_t* fader_adc, int8_t gui_channel, const uint8_t* oled_fb) {
        (void)gui_channel;
        m_led_afs = led_afs;
        m_led_stem = led_stem;
        m_led_loud = led_loud;
        if (fader_adc) {
            for (int i = 0; i < 5; ++i) {
                // 사용자가 하드웨어 창에서 직접 마우스로 드래그 중인 채널이 아니라면
                // 메인 믹서의 슬라이더 위치로 실시간 동기화 (모터 페이더 에뮬레이션)
                if (m_dragging_fader != i) {
                    m_fader_adc[i] = fader_adc[i];
                }
            }
        }
        if (oled_fb) {
            std::copy(oled_fb, oled_fb + 1024, m_oled_buffer.begin());
        }
    }

    void send_control_update() {
        ControlPacket pkt;
        pkt.packet_type = PKT_TYPE_CONTROL;
        for (int i = 0; i < 5; ++i) {
            pkt.fader_adc[i] = static_cast<uint16_t>(m_fader_adc[i]);
        }
        pkt.eq_low = m_eq_knobs[0];
        pkt.eq_mid = m_eq_knobs[1];
        pkt.eq_high = m_eq_knobs[2];
        pkt.btn_afs = m_led_afs;
        pkt.btn_stem = m_led_stem;
        pkt.btn_loud = m_led_loud;
        pkt.encoder_delta = 0;
        pkt.active_channel = static_cast<int8_t>(m_dragging_fader);

        m_ipc.send_packet(HardwareIpcSocket::MIXER_PORT, &pkt, sizeof(pkt));
    }

    bool create(int width = 960, int height = 620) {
        WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = StaticWndProc;
        wc.hInstance = m_hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(RGB(22, 23, 26)); // Brushed metal dark
        wc.lpszClassName = L"HardwareSimClass";

        RegisterClassExW(&wc);

        RECT rc = { 0, 0, width, height };
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME, FALSE);

        m_hWnd = CreateWindowExW(
            0,
            wc.lpszClassName,
            L"🎛️ Physical Hardware Simulator (STM32H743 Audio Board)",
            (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME) | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rc.right - rc.left, rc.bottom - rc.top,
            nullptr, nullptr, m_hInstance, this
        );

        if (!m_hWnd) return false;

        // 60 FPS 렌더링 타이머
        SetTimer(m_hWnd, 1, 16, nullptr);

        // 초기 제어값 전송
        send_control_update();
        return true;
    }

    HWND get_hwnd() const { return m_hWnd; }

private:
    HINSTANCE m_hInstance;
    HWND m_hWnd;
    HardwareAudioSynthesizer& m_synth;
    HardwareIpcSocket& m_ipc;

    std::vector<int> m_fader_adc;
    int8_t m_eq_knobs[3];
    int m_dragging_fader;

    uint8_t m_led_afs;
    uint8_t m_led_stem;
    uint8_t m_led_loud;
    std::vector<uint8_t> m_oled_buffer;

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        HardwareSimWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = reinterpret_cast<HardwareSimWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->m_hWnd = hwnd;
        } else {
            self = reinterpret_cast<HardwareSimWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (self) {
            return self->handle_message(hwnd, msg, wParam, lParam);
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT handle_message(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);
                render(hdc);
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_TIMER: {
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            case WM_LBUTTONDOWN: {
                int x = GET_X_LPARAM(lParam);
                int y = GET_Y_LPARAM(lParam);
                on_lbutton_down(x, y);
                return 0;
            }
            case WM_MOUSEMOVE: {
                int x = GET_X_LPARAM(lParam);
                int y = GET_Y_LPARAM(lParam);
                on_mouse_move(x, y);
                return 0;
            }
            case WM_LBUTTONUP: {
                on_lbutton_up();
                return 0;
            }
            case WM_DESTROY: {
                KillTimer(hwnd, 1);
                PostQuitMessage(0);
                return 0;
            }
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    void on_lbutton_down(int x, int y) {
        SetCapture(m_hWnd);

        // 1. 5개 물리 페이더 드래그 (정확한 렌더링 좌표 일치: fx = 485 + ch * 92, center = fx + 33)
        for (int ch = 0; ch < 5; ++ch) {
            int fx = 485 + ch * 92;
            int slider_center_x = fx + 33;
            if (x >= slider_center_x - 25 && x <= slider_center_x + 25 && y >= 115 && y <= 385) {
                m_dragging_fader = ch;
                update_fader_from_mouse(ch, y);
                return;
            }
        }

        // 2. 물리 택트 스위치 버튼 (AFS, STEM, LOUD)
        // AFS 버튼 (x: 40 ~ 130, y: 250 ~ 290)
        if (x >= 40 && x <= 130 && y >= 250 && y <= 290) {
            m_led_afs = !m_led_afs;
            send_control_update();
            return;
        }
        // STEM 버튼 (x: 150 ~ 240, y: 250 ~ 290)
        if (x >= 150 && x <= 240 && y >= 250 && y <= 290) {
            m_led_stem = !m_led_stem;
            send_control_update();
            return;
        }
        // LOUD 버튼 (x: 260 ~ 350, y: 250 ~ 290)
        if (x >= 260 && x <= 350 && y >= 250 && y <= 290) {
            m_led_loud = !m_led_loud;
            send_control_update();
            return;
        }

        // 3. 로터리 엔코더 버튼 (x: 375 ~ 435, y: 250 ~ 290)
        if (x >= 375 && x <= 435 && y >= 250 && y <= 290) {
            // 엔코더 클릭
            ControlPacket pkt;
            std::memset(&pkt, 0, sizeof(pkt));
            pkt.packet_type = PKT_TYPE_CONTROL;
            for (int i = 0; i < 5; ++i) pkt.fader_adc[i] = m_fader_adc[i];
            pkt.encoder_delta = 1;
            m_ipc.send_packet(HardwareIpcSocket::MIXER_PORT, &pkt, sizeof(pkt));
            return;
        }

        // 4. CH2 Beat 음원 재생/일시정지 토글 버튼 (x: 40 ~ 230, y: 460 ~ 495)
        if (x >= 40 && x <= 230 && y >= 460 && y <= 495) {
            m_synth.set_beat_playing(!m_synth.is_beat_playing());
            return;
        }

        // 5. CH2 BPM 조절 버튼
        if (x >= 240 && x <= 270 && y >= 460 && y <= 495) {
            m_synth.set_bpm(m_synth.get_bpm() - 5.0f);
            return;
        }
        if (x >= 310 && x <= 340 && y >= 460 && y <= 495) {
            m_synth.set_bpm(m_synth.get_bpm() + 5.0f);
            return;
        }

        // 6. CH4 Line In 2 (악기/코드) 재생/일시정지 토글 버튼 (x: 40 ~ 230, y: 520 ~ 555)
        if (x >= 40 && x <= 230 && y >= 520 && y <= 555) {
            m_synth.set_line2_playing(!m_synth.is_line2_playing());
            return;
        }
    }

    void on_mouse_move(int x, int y) {
        if (m_dragging_fader >= 0) {
            update_fader_from_mouse(m_dragging_fader, y);
        }
    }

    void on_lbutton_up() {
        if (m_dragging_fader >= 0) {
            m_dragging_fader = -1;
            send_control_update();
            ReleaseCapture();
        }
    }

    void update_fader_from_mouse(int ch, int y) {
        constexpr int top = 125;
        constexpr int bottom = 365;
        float norm = 1.0f - static_cast<float>(y - top) / (bottom - top);
        norm = std::clamp(norm, 0.0f, 1.0f);
        m_fader_adc[ch] = static_cast<int>(norm * 1023);
        send_control_update();
    }

    void render(HDC hdc) {
        RECT client_rc;
        GetClientRect(m_hWnd, &client_rc);
        int w = client_rc.right;
        int h = client_rc.bottom;

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBitmap = CreateCompatibleBitmap(hdc, w, h);
        HBITMAP oldBitmap = reinterpret_cast<HBITMAP>(SelectObject(memDC, memBitmap));

        // 하드웨어 알루미늄 다크 섀시 (#1E2024)
        HBRUSH bgBrush = CreateSolidBrush(RGB(28, 30, 35));
        FillRect(memDC, &client_rc, bgBrush);
        DeleteObject(bgBrush);

        SetBkMode(memDC, TRANSPARENT);

        // 상단 헤더
        draw_header(memDC, w);

        // 좌측 상단: 0.96인치 128x64 OLED 디스플레이 모듈 시뮬레이터
        draw_oled_screen(memDC);

        // 좌측 중단: 물리 택트 버튼 & LED 링 & 로터리 엔코더
        draw_physical_controls(memDC);

        // 좌측 하단: 하드웨어 음원 생성기 (CH2 비트 & CH4 라인 2)
        draw_audio_sources_card(memDC);

        // 우측: 5개 물리 슬라이드 페이더 (PTA 10k 슬라이더)
        draw_physical_faders(memDC);

        BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBitmap);
        DeleteObject(memBitmap);
        DeleteDC(memDC);
    }

    void draw_header(HDC hdc, int w) {
        RECT rc = { 0, 0, w, 48 };
        HBRUSH hBr = CreateSolidBrush(RGB(18, 19, 22));
        FillRect(hdc, &rc, hBr);
        DeleteObject(hBr);

        HPEN linePen = CreatePen(PS_SOLID, 1, RGB(45, 48, 54));
        HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, linePen));
        MoveToEx(hdc, 0, 47, nullptr);
        LineTo(hdc, w, 47);

        HFONT hTitleFont = CreateFontW(17, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH, L"Segoe UI");
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, hTitleFont));

        SetTextColor(hdc, RGB(240, 242, 245));
        TextOutW(hdc, 20, 12, L"🎛️ STM32H743 PHYSICAL HARDWARE SIMULATOR", 41);

        // 통신 연결 뱃지
        SetTextColor(hdc, RGB(57, 255, 20));
        TextOutW(hdc, w - 370, 14, L"● IPC LINKED (127.0.0.1:9123 / UDP 0.05ms)", 43);

        SelectObject(hdc, oldFont);
        SelectObject(hdc, oldPen);
        DeleteObject(linePen);
        DeleteObject(hTitleFont);
    }

    void draw_oled_screen(HDC hdc) {
        RECT panelRc = { 20, 60, 440, 230 };
        draw_module_card(hdc, panelRc, L"0.96\" OLED DISPLAY (SSD1306 128x64)");

        // OLED 유리 영역 (x: 40, y: 92, w: 256, h: 128 -> 2배 확대 픽셀)
        RECT screenRc = { 35, 90, 425, 218 };
        HBRUSH oledBg = CreateSolidBrush(RGB(6, 8, 12));
        FillRect(hdc, &screenRc, oledBg);
        DeleteObject(oledBg);

        HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(0, 180, 230));
        HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, borderPen));
        MoveToEx(hdc, screenRc.left - 1, screenRc.top - 1, nullptr);
        LineTo(hdc, screenRc.right + 1, screenRc.top - 1);
        LineTo(hdc, screenRc.right + 1, screenRc.bottom + 1);
        LineTo(hdc, screenRc.left - 1, screenRc.bottom + 1);
        LineTo(hdc, screenRc.left - 1, screenRc.top - 1);
        SelectObject(hdc, oldPen);
        DeleteObject(borderPen);

        // 128x64 프레임버퍼 픽셀 렌더링 (2배 스케일: 가로 3px, 세로 2px)
        HBRUSH pixelBrush = CreateSolidBrush(RGB(0, 220, 255)); // Amber/Cyan OLED
        for (int page = 0; page < 8; ++page) {
            for (int col = 0; col < 128; ++col) {
                uint8_t b = m_oled_buffer[page * 128 + col];
                if (b == 0) continue;
                for (int bit = 0; bit < 8; ++bit) {
                    if (b & (1 << bit)) {
                        int py = page * 8 + bit;
                        int px = col;
                        int dx = screenRc.left + px * 3;
                        int dy = screenRc.top + py * 2;
                        RECT prc = { dx, dy, dx + 2, dy + 2 };
                        FillRect(hdc, &prc, pixelBrush);
                    }
                }
            }
        }
        DeleteObject(pixelBrush);
    }

    void draw_physical_controls(HDC hdc) {
        RECT panelRc = { 20, 240, 440, 395 };
        draw_module_card(hdc, panelRc, L"TACT SWITCHES, LEDS & EC11 ENCODER (H-FR-6, 7)");

        // 1. AFS 택트 스위치 & LED
        draw_tact_button(hdc, 40, 275, 130, 325, L"AFS", m_led_afs, RGB(57, 255, 20));

        // 2. STEM 택트 스위치 & LED
        draw_tact_button(hdc, 150, 275, 240, 325, L"STEM", m_led_stem, RGB(0, 210, 255));

        // 3. LOUD 택트 스위치 & LED
        draw_tact_button(hdc, 260, 275, 350, 325, L"LOUD", m_led_loud, RGB(255, 187, 0));

        // 4. EC11 무한 로터리 엔코더 (푸시 스위치 내장)
        draw_rotary_encoder(hdc, 375, 275, 430, 325);

        // 5. EQ 아날로그 가변저항 노브 (Low, Mid, High)
        draw_eq_knobs(hdc, 40, 335);
    }

    void draw_tact_button(HDC hdc, int x0, int y0, int x1, int y1, const wchar_t* label, bool led_on, COLORREF led_color) {
        // 스위치 베젤
        RECT btnRc = { x0, y0, x1, y1 };
        HBRUSH btnBr = CreateSolidBrush(RGB(45, 48, 54));
        FillRect(hdc, &btnRc, btnBr);
        DeleteObject(btnBr);

        // LED 램프 (상단 작은 원)
        int led_cx = (x0 + x1) / 2;
        int led_cy = y0 + 10;
        HBRUSH ledBr = CreateSolidBrush(led_on ? led_color : RGB(30, 32, 36));
        HPEN ledPen = CreatePen(PS_SOLID, 1, led_on ? RGB(255, 255, 255) : RGB(50, 52, 58));
        SelectObject(hdc, ledBr);
        SelectObject(hdc, ledPen);
        Ellipse(hdc, led_cx - 5, led_cy - 5, led_cx + 5, led_cy + 5);
        DeleteObject(ledBr);
        DeleteObject(ledPen);

        // 라벨 텍스트
        HFONT hFont = CreateFontW(13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH, L"Segoe UI");
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, hFont));
        SetTextColor(hdc, RGB(230, 235, 245));
        RECT txtRc = { x0, y0 + 20, x1, y1 };
        DrawTextW(hdc, label, -1, &txtRc, DT_CENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
        DeleteObject(hFont);
    }

    void draw_rotary_encoder(HDC hdc, int x0, int y0, int x1, int y1) {
        RECT rc = { x0, y0, x1, y1 };
        HBRUSH bg = CreateSolidBrush(RGB(35, 38, 44));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        int cx = (x0 + x1) / 2;
        int cy = (y0 + y1) / 2;
        HBRUSH dialBr = CreateSolidBrush(RGB(65, 70, 80));
        HPEN dialPen = CreatePen(PS_SOLID, 1, RGB(180, 185, 195));
        SelectObject(hdc, dialBr);
        SelectObject(hdc, dialPen);
        Ellipse(hdc, cx - 18, cy - 18, cx + 18, cy + 18);
        DeleteObject(dialBr);
        DeleteObject(dialPen);

        HFONT hFont = CreateFontW(10, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH, L"Segoe UI");
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, hFont));
        SetTextColor(hdc, RGB(255, 255, 255));
        RECT txtRc = { x0, cy - 6, x1, cy + 6 };
        DrawTextW(hdc, L"PUSH", -1, &txtRc, DT_CENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
        DeleteObject(hFont);
    }

    void draw_eq_knobs(HDC hdc, int start_x, int y0) {
        const wchar_t* eq_labels[] = { L"EQ LOW", L"EQ MID", L"EQ HIGH" };
        HFONT hFont = CreateFontW(11, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH, L"Segoe UI");
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, hFont));

        for (int k = 0; k < 3; ++k) {
            int cx = start_x + 60 + k * 130;
            int cy = y0 + 24;

            // 노브 원형 바디
            HBRUSH knobBr = CreateSolidBrush(RGB(50, 54, 62));
            HPEN knobPen = CreatePen(PS_SOLID, 2, RGB(0, 210, 255));
            SelectObject(hdc, knobBr);
            SelectObject(hdc, knobPen);
            Ellipse(hdc, cx - 16, cy - 16, cx + 16, cy + 16);
            DeleteObject(knobBr);
            DeleteObject(knobPen);

            // 포인터 라인 (12시 방향 센터)
            HPEN ptrPen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
            SelectObject(hdc, ptrPen);
            MoveToEx(hdc, cx, cy, nullptr);
            LineTo(hdc, cx, cy - 13);
            DeleteObject(ptrPen);

            // 라벨
            SetTextColor(hdc, RGB(180, 185, 195));
            RECT lblRc = { cx - 50, cy + 18, cx + 50, cy + 34 };
            DrawTextW(hdc, eq_labels[k], -1, &lblRc, DT_CENTER | DT_SINGLELINE);
        }

        SelectObject(hdc, oldFont);
        DeleteObject(hFont);
    }

    void draw_audio_sources_card(HDC hdc) {
        RECT panelRc = { 20, 410, 440, 595 };
        draw_module_card(hdc, panelRc, L"AUDIO ACCOMPANIMENT GENERATORS (CH 2 & CH 4)");

        // 1. Channel 2: Beat / Drum Machine
        bool beat_on = m_synth.is_beat_playing();
        RECT beatBtnRc = { 35, 445, 225, 485 };
        HBRUSH bBr = CreateSolidBrush(beat_on ? RGB(0, 104, 71) : RGB(45, 48, 54));
        FillRect(hdc, &beatBtnRc, bBr);
        DeleteObject(bBr);

        HFONT hFont = CreateFontW(13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH, L"Segoe UI");
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, hFont));
        SetTextColor(hdc, RGB(255, 255, 255));
        DrawTextW(hdc, beat_on ? L"▶ CH 2 BEAT RUNNING" : L"⏸ CH 2 BEAT PAUSED", -1, &beatBtnRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // BPM 조절 버튼 [-] [BPM] [+]
        RECT bpmMinusRc = { 240, 445, 270, 485 };
        RECT bpmPlusRc = { 315, 445, 345, 485 };
        HBRUSH bpmBr = CreateSolidBrush(RGB(40, 44, 52));
        FillRect(hdc, &bpmMinusRc, bpmBr);
        FillRect(hdc, &bpmPlusRc, bpmBr);
        DeleteObject(bpmBr);
        DrawTextW(hdc, L"-", -1, &bpmMinusRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextW(hdc, L"+", -1, &bpmPlusRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        wchar_t bpmBuf[32];
        swprintf_s(bpmBuf, L"%.0f BPM", m_synth.get_bpm());
        RECT bpmTxtRc = { 270, 445, 315, 485 };
        SetTextColor(hdc, RGB(0, 210, 255));
        DrawTextW(hdc, bpmBuf, -1, &bpmTxtRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // 2. Channel 4: Line In 2 (Guitar / Synth Chords)
        bool line2_on = m_synth.is_line2_playing();
        RECT lineBtnRc = { 35, 500, 225, 540 };
        HBRUSH lBr = CreateSolidBrush(line2_on ? RGB(0, 104, 71) : RGB(45, 48, 54));
        FillRect(hdc, &lineBtnRc, lBr);
        DeleteObject(lBr);
        SetTextColor(hdc, RGB(255, 255, 255));
        DrawTextW(hdc, line2_on ? L"▶ CH 4 LINE 2 RUNNING" : L"⏸ CH 4 LINE 2 PAUSED", -1, &lineBtnRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        RECT descRc = { 240, 502, 420, 540 };
        SetTextColor(hdc, RGB(170, 175, 185));
        DrawTextW(hdc, L"Chords: C - G - Am - F\nAux Stereo Line Input", -1, &descRc, DT_LEFT);

        // 오디오 라우팅 설명
        RECT infoRc = { 35, 555, 425, 585 };
        SetTextColor(hdc, RGB(57, 255, 20));
        DrawTextW(hdc, L"● Live Audio Stream -> Main Mixer CH 2 & CH 4 over UDP", -1, &infoRc, DT_LEFT);

        SelectObject(hdc, oldFont);
        DeleteObject(hFont);
    }

    void draw_physical_faders(HDC hdc) {
        RECT panelRc = { 460, 60, 935, 595 };
        draw_module_card(hdc, panelRc, L"5-CHANNEL PHYSICAL SLIDE FADERS (PTA 10K LINEAR)");

        const wchar_t* ch_names[] = { L"CH 0 MASTER", L"CH 1 VOCAL", L"CH 2 BEAT", L"CH 3 MIC 1", L"CH 4 LINE 2" };
        COLORREF ch_colors[] = {
            RGB(255, 51, 102), // Master (Red)
            RGB(0, 210, 255),  // Vocal (Cyan)
            RGB(255, 187, 0),  // Beat (Gold)
            RGB(57, 255, 20),  // Mic 1 (Green)
            RGB(179, 102, 255) // Line 2 (Purple)
        };

        HFONT hFont = CreateFontW(12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH, L"Segoe UI");
        HFONT hAdcFont = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH, L"Consolas");
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, hFont));

        for (int i = 0; i < 5; ++i) {
            int fx = 485 + i * 92;

            // 채널 이름
            SetTextColor(hdc, ch_colors[i]);
            RECT nameRc = { fx - 10, 95, fx + 75, 115 };
            DrawTextW(hdc, ch_names[i], -1, &nameRc, DT_CENTER | DT_SINGLELINE);

            // 물리 눈금 마크 (0, 2, 4, 6, 8, 10)
            HPEN tickPen = CreatePen(PS_SOLID, 1, RGB(70, 75, 85));
            HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, tickPen));
            for (int t = 0; t <= 10; ++t) {
                int ty = 365 - t * (365 - 125) / 10;
                MoveToEx(hdc, fx + 16, ty, nullptr);
                LineTo(hdc, fx + 24, ty);
            }
            SelectObject(hdc, oldPen);
            DeleteObject(tickPen);

            // 슬라이더 물리 슬롯 (가운데 홈)
            RECT slotRc = { fx + 30, 125, fx + 36, 365 };
            HBRUSH slotBr = CreateSolidBrush(RGB(15, 16, 18));
            FillRect(hdc, &slotRc, slotBr);
            DeleteObject(slotBr);

            // 페이더 노브 (실제 믹서 금속 손잡이 형태)
            float norm = m_fader_adc[i] / 1023.0f;
            int thumb_y = static_cast<int>(365 - norm * (365 - 125));

            RECT thumbRc = { fx + 18, thumb_y - 10, fx + 48, thumb_y + 10 };
            HBRUSH thumbBr = CreateSolidBrush(RGB(50, 54, 62));
            HPEN thumbPen = CreatePen(PS_SOLID, 1, RGB(220, 225, 235));
            SelectObject(hdc, thumbBr);
            SelectObject(hdc, thumbPen);
            RoundRect(hdc, thumbRc.left, thumbRc.top, thumbRc.right, thumbRc.bottom, 4, 4);
            DeleteObject(thumbBr);
            DeleteObject(thumbPen);

            // 노브 중앙 컬러 라인
            HPEN linePen = CreatePen(PS_SOLID, 2, ch_colors[i]);
            SelectObject(hdc, linePen);
            MoveToEx(hdc, thumbRc.left + 4, thumb_y, nullptr);
            LineTo(hdc, thumbRc.right - 4, thumb_y);
            DeleteObject(linePen);

            // ADC 10비트 수치 출력 (0 ~ 1023)
            SelectObject(hdc, hAdcFont);
            SetTextColor(hdc, RGB(240, 245, 255));
            wchar_t adcBuf[32];
            swprintf_s(adcBuf, L"%d", m_fader_adc[i]);
            RECT adcRc = { fx - 10, 385, fx + 75, 405 };
            DrawTextW(hdc, adcBuf, -1, &adcRc, DT_CENTER | DT_SINGLELINE);

            // 퍼센트 (%)
            wchar_t pctBuf[32];
            swprintf_s(pctBuf, L"%.0f%%", norm * 100.0f);
            RECT pctRc = { fx - 10, 405, fx + 75, 425 };
            SetTextColor(hdc, RGB(160, 165, 175));
            DrawTextW(hdc, pctBuf, -1, &pctRc, DT_CENTER | DT_SINGLELINE);
            SelectObject(hdc, hFont);
        }

        SelectObject(hdc, oldFont);
        DeleteObject(hFont);
        DeleteObject(hAdcFont);
    }

    void draw_module_card(HDC hdc, const RECT& rc, const wchar_t* title) {
        HBRUSH cardBr = CreateSolidBrush(RGB(24, 26, 30));
        HPEN cardPen = CreatePen(PS_SOLID, 1, RGB(45, 48, 54));
        HBRUSH oldBr = reinterpret_cast<HBRUSH>(SelectObject(hdc, cardBr));
        HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, cardPen));

        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 6, 6);

        SelectObject(hdc, oldBr);
        SelectObject(hdc, oldPen);
        DeleteObject(cardBr);
        DeleteObject(cardPen);

        HFONT hTitleFont = CreateFontW(12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH, L"Segoe UI");
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, hTitleFont));
        SetTextColor(hdc, RGB(0, 210, 255));
        TextOutW(hdc, rc.left + 12, rc.top + 7, title, static_cast<int>(wcslen(title)));

        SelectObject(hdc, oldFont);
        DeleteObject(hTitleFont);
    }
};

} // namespace SmartMixer
