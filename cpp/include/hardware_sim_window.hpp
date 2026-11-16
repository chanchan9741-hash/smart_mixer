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
#include <mutex>
#include <array>
#include <functional>

#include "hardware_ipc.hpp"
#include "hardware_audio_synth.hpp"
#include "wasapi_capture.hpp"
#include "wasapi_render.hpp"

namespace SmartMixer {

constexpr int IDC_HW_MIC_COMBO = 2001;
constexpr int IDC_HW_SPK_COMBO = 2002;

struct RawHwInputs {
    uint16_t fader_adc[5];
    uint16_t eq_adc[3];
    bool btn_afs_down;
    bool btn_stem_down;
    bool btn_loud_down;
    int8_t encoder_delta;
    int8_t dragging_fader;
};

struct McuUiTelemetry {
    uint8_t led_afs = 1;
    uint8_t led_stem = 1;
    uint8_t led_loud = 1;
    uint8_t active_notches = 0;
    uint32_t suppressed_count = 0;
    float measured_lufs = -70.0f;
    float target_lufs = -14.0f;
    float eq_db[3] = { 0.0f, 0.0f, 0.0f };
    float mic_rms = 0.0f;
    float out_peak_l = 0.0f;
    float out_peak_r = 0.0f;
    float spk_wave[32] = { 0.0f };
    bool pc_host_linked = false;
    bool howling_alert = false;
};

class HardwareSimWindow {
public:
    using SelectCallback = std::function<void(int)>;
    using ActionCallback = std::function<void()>;

    HardwareSimWindow(HINSTANCE hInstance,
                      HardwareAudioSynthesizer& synth,
                      HardwareIpcSocket& ipc,
                      WasapiCaptureEngine& mic_cap,
                      WasapiRenderEngine& spk_render)
        : m_hInstance(hInstance),
          m_hWnd(nullptr),
          m_hComboMic(nullptr),
          m_hComboSpk(nullptr),
          m_synth(synth),
          m_ipc(ipc),
          m_mic_cap(mic_cap),
          m_spk_render(spk_render),
          m_dragging_fader(-1),
          m_dragging_eq(-1),
          m_eq_drag_start_y(0),
          m_eq_drag_start_adc(512),
          m_pressed_btn(-1),
          m_encoder_accum(0),
          m_led_afs(1),
          m_led_stem(1),
          m_led_loud(1) {
        m_raw_fader_adc.fill(819);
        m_display_fader_adc.fill(819);
        m_raw_eq_adc.fill(512); // 0 dB center
        m_btn_pressed.fill(false);
        m_oled_buffer.resize(1024, 0);
    }

    void set_mic_selected_callback(SelectCallback cb) { m_mic_selected_cb = std::move(cb); }
    void set_spk_selected_callback(SelectCallback cb) { m_spk_selected_cb = std::move(cb); }
    void set_howl_inject_callback(ActionCallback cb) { m_howl_inject_cb = std::move(cb); }

    RawHwInputs snapshot_inputs() {
        std::lock_guard<std::mutex> lock(m_mtx);
        RawHwInputs out;
        for (int i = 0; i < 5; ++i) {
            out.fader_adc[i] = static_cast<uint16_t>(m_raw_fader_adc[i]);
        }
        for (int k = 0; k < 3; ++k) {
            out.eq_adc[k] = static_cast<uint16_t>(m_raw_eq_adc[k]);
        }
        out.btn_afs_down = m_btn_pressed[0];
        out.btn_stem_down = m_btn_pressed[1];
        out.btn_loud_down = m_btn_pressed[2];
        out.encoder_delta = static_cast<int8_t>(std::clamp(m_encoder_accum, -12, 12));
        m_encoder_accum = 0;
        out.dragging_fader = static_cast<int8_t>(m_dragging_fader);
        return out;
    }

    void sync_faders_from_host(const uint16_t fader_adc[5], int8_t host_active_ch) {
        (void)host_active_ch;
        if (!fader_adc) return;
        std::lock_guard<std::mutex> lock(m_mtx);
        for (int i = 0; i < 5; ++i) {
            if (m_dragging_fader != i) {
                int clamped = std::clamp(static_cast<int>(fader_adc[i]), 0, 1023);
                m_raw_fader_adc[i] = clamped;
                m_display_fader_adc[i] = clamped;
            }
        }
    }

    void set_mcu_leds(uint8_t afs, uint8_t stem, uint8_t loud) {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_led_afs = afs;
        m_led_stem = stem;
        m_led_loud = loud;
    }

    void set_mcu_oled(const uint8_t* oled_fb) {
        if (!oled_fb) return;
        std::lock_guard<std::mutex> lock(m_mtx);
        std::copy(oled_fb, oled_fb + 1024, m_oled_buffer.begin());
    }

    void update_mcu_telemetry(const McuUiTelemetry& tel, const uint16_t smoothed_faders[5]) {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_telemetry = tel;
        m_led_afs = tel.led_afs;
        m_led_stem = tel.led_stem;
        m_led_loud = tel.led_loud;
        if (smoothed_faders) {
            for (int i = 0; i < 5; ++i) {
                m_display_fader_adc[i] = smoothed_faders[i];
            }
        }
    }

    bool create(int width = 960, int height = 620) {
        WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = StaticWndProc;
        wc.hInstance = m_hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(RGB(22, 23, 26));
        wc.lpszClassName = L"HardwareSimClass";

        RegisterClassExW(&wc);

        RECT rc = { 0, 0, width, height };
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME, FALSE);

        m_hWnd = CreateWindowExW(
            0,
            wc.lpszClassName,
            L"🎛️ Physical Hardware Simulator (STM32H743 Live Firmware)",
            (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME) | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rc.right - rc.left, rc.bottom - rc.top,
            nullptr, nullptr, m_hInstance, this
        );

        if (!m_hWnd) return false;

        // 기기 전용 마이크 입력 선택 콤보박스 (x: 472, y: 472, w: 208, h: 180)
        m_hComboMic = CreateWindowExW(
            0, L"COMBOBOX", L"",
            CBS_DROPDOWNLIST | WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP,
            472, 472, 208, 180,
            m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_HW_MIC_COMBO)),
            m_hInstance, nullptr
        );

        // 기기 가상 스피커 / 출력 브릿지 선택 콤보박스 (x: 714, y: 472, w: 208, h: 180)
        m_hComboSpk = CreateWindowExW(
            0, L"COMBOBOX", L"",
            CBS_DROPDOWNLIST | WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP,
            714, 472, 208, 180,
            m_hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_HW_SPK_COMBO)),
            m_hInstance, nullptr
        );

        HFONT hComboFont = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH, L"Segoe UI");
        if (m_hComboMic) SendMessageW(m_hComboMic, WM_SETFONT, reinterpret_cast<WPARAM>(hComboFont), TRUE);
        if (m_hComboSpk) SendMessageW(m_hComboSpk, WM_SETFONT, reinterpret_cast<WPARAM>(hComboFont), TRUE);

        populate_hw_mic_combo();
        populate_hw_spk_combo();

        // 60 FPS 렌더링 타이머
        SetTimer(m_hWnd, 1, 16, nullptr);
        return true;
    }

    HWND get_hwnd() const { return m_hWnd; }

private:
    HINSTANCE m_hInstance;
    HWND m_hWnd;
    HWND m_hComboMic;
    HWND m_hComboSpk;
    HardwareAudioSynthesizer& m_synth;
    HardwareIpcSocket& m_ipc;
    WasapiCaptureEngine& m_mic_cap;
    WasapiRenderEngine& m_spk_render;
    SelectCallback m_mic_selected_cb;
    SelectCallback m_spk_selected_cb;
    ActionCallback m_howl_inject_cb;
    mutable std::mutex m_mtx;

    void populate_hw_mic_combo() {
        if (!m_hComboMic) return;
        SendMessageW(m_hComboMic, CB_RESETCONTENT, 0, 0);
        SendMessageW(m_hComboMic, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"💻 [가상 보컬] Virtual Mic (440Hz)"));
        SendMessageW(m_hComboMic, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"🔇 [무음] Mic Mute"));
        auto devs = m_mic_cap.enumerate_capture_devices();
        for (const auto& d : devs) {
            std::wstring label = L"🎤 " + d.name_wide;
            SendMessageW(m_hComboMic, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendMessageW(m_hComboMic, CB_SETCURSEL, 0, 0);
    }

    void populate_hw_spk_combo() {
        if (!m_hComboSpk) return;
        SendMessageW(m_hComboSpk, CB_RESETCONTENT, 0, 0);
        SendMessageW(m_hComboSpk, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"🔌 가상 스피커 → PC 입력 브릿지 (기본)"));
        auto devs = m_spk_render.enumerate_render_devices();
        for (const auto& d : devs) {
            std::wstring label = L"🔊 [직접 모니터] " + d.name_wide;
            SendMessageW(m_hComboSpk, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendMessageW(m_hComboSpk, CB_SETCURSEL, 0, 0);
    }

    std::array<int, 5> m_raw_fader_adc;
    std::array<int, 5> m_display_fader_adc;
    std::array<int, 3> m_raw_eq_adc;
    std::array<bool, 3> m_btn_pressed;

    int m_dragging_fader;
    int m_dragging_eq;
    int m_eq_drag_start_y;
    int m_eq_drag_start_adc;
    int m_pressed_btn;
    int m_encoder_accum;

    uint8_t m_led_afs;
    uint8_t m_led_stem;
    uint8_t m_led_loud;
    std::vector<uint8_t> m_oled_buffer;
    McuUiTelemetry m_telemetry;

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
            case WM_COMMAND: {
                if (LOWORD(wParam) == IDC_HW_MIC_COMBO && HIWORD(wParam) == CBN_SELCHANGE) {
                    int sel = static_cast<int>(SendMessageW(m_hComboMic, CB_GETCURSEL, 0, 0));
                    if (m_mic_selected_cb) m_mic_selected_cb(sel);
                    return 0;
                }
                if (LOWORD(wParam) == IDC_HW_SPK_COMBO && HIWORD(wParam) == CBN_SELCHANGE) {
                    int sel = static_cast<int>(SendMessageW(m_hComboSpk, CB_GETCURSEL, 0, 0));
                    if (m_spk_selected_cb) m_spk_selected_cb(sel);
                    return 0;
                }
                break;
            }
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
            case WM_RBUTTONDOWN: {
                int x = GET_X_LPARAM(lParam);
                int y = GET_Y_LPARAM(lParam);
                on_rbutton_down(x, y);
                return 0;
            }
            case WM_MOUSEWHEEL: {
                short delta = GET_WHEEL_DELTA_WPARAM(wParam);
                if (delta != 0) {
                    std::lock_guard<std::mutex> lock(m_mtx);
                    m_encoder_accum += (delta > 0) ? 1 : -1;
                }
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

        // 1. 5개 물리 페이더 드래그 (fx = 485 + ch * 92, center = fx + 33)
        for (int ch = 0; ch < 5; ++ch) {
            int fx = 485 + ch * 92;
            int slider_center_x = fx + 33;
            if (x >= slider_center_x - 25 && x <= slider_center_x + 25 && y >= 115 && y <= 385) {
                {
                    std::lock_guard<std::mutex> lock(m_mtx);
                    m_dragging_fader = ch;
                }
                update_fader_from_mouse(ch, y);
                return;
            }
        }

        // 2. 물리 택트 스위치 버튼 (좌표 정확히 일치: y = 275 ~ 325)
        // 누르는 동안 GPIO가 LOW/ACTIVE 유지되어 25ms 디바운스 및 600ms 롱프레스(AFS 리셋) 동작
        if (x >= 40 && x <= 130 && y >= 275 && y <= 325) {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_btn_pressed[0] = true;
            m_pressed_btn = 0;
            return;
        }
        if (x >= 150 && x <= 240 && y >= 275 && y <= 325) {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_btn_pressed[1] = true;
            m_pressed_btn = 1;
            return;
        }
        if (x >= 260 && x <= 350 && y >= 275 && y <= 325) {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_btn_pressed[2] = true;
            m_pressed_btn = 2;
            return;
        }

        // 3. EC11 로터리 엔코더 (좌클릭: +0.5 LUFS)
        if (x >= 375 && x <= 430 && y >= 275 && y <= 325) {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_encoder_accum += 1;
            return;
        }

        // 4. 3-Band EQ 아날로그 노브 (cx = 100, 230, 360, cy = 359, 반경 ~22)
        for (int k = 0; k < 3; ++k) {
            int cx = 40 + 60 + k * 130;
            int cy = 335 + 24;
            if (x >= cx - 24 && x <= cx + 24 && y >= cy - 22 && y <= cy + 28) {
                std::lock_guard<std::mutex> lock(m_mtx);
                m_dragging_eq = k;
                m_eq_drag_start_y = y;
                m_eq_drag_start_adc = m_raw_eq_adc[k];
                return;
            }
        }

        // 5. CH2 Beat 음원 재생/일시정지 토글 (y: 445 ~ 485)
        if (x >= 35 && x <= 225 && y >= 445 && y <= 485) {
            m_synth.set_beat_playing(!m_synth.is_beat_playing());
            return;
        }

        // 6. CH2 BPM 조절 버튼 ([-] 240~270, [+] 315~345, y: 445 ~ 485)
        if (x >= 240 && x <= 270 && y >= 445 && y <= 485) {
            m_synth.set_bpm(m_synth.get_bpm() - 5.0f);
            return;
        }
        if (x >= 315 && x <= 345 && y >= 445 && y <= 485) {
            m_synth.set_bpm(m_synth.get_bpm() + 5.0f);
            return;
        }

        // 7. CH4 Line In 2 재생/일시정지 토글 (x: 35 ~ 225, y: 500 ~ 540)
        if (x >= 35 && x <= 225 && y >= 500 && y <= 540) {
            m_synth.set_line2_playing(!m_synth.is_line2_playing());
            return;
        }

        // 8. 기기 마이크 하울링 주입 테스트 버튼 (x: 472 ~ 680, y: 554 ~ 588)
        if (x >= 472 && x <= 680 && y >= 554 && y <= 588) {
            if (m_howl_inject_cb) m_howl_inject_cb();
            return;
        }
    }

    void on_rbutton_down(int x, int y) {
        // 엔코더 우클릭: -0.5 LUFS
        if (x >= 375 && x <= 430 && y >= 275 && y <= 325) {
            std::lock_guard<std::mutex> lock(m_mtx);
            m_encoder_accum -= 1;
            return;
        }
        // EQ 노브 우클릭: 0.0 dB 센터 리셋
        for (int k = 0; k < 3; ++k) {
            int cx = 40 + 60 + k * 130;
            int cy = 335 + 24;
            if (x >= cx - 24 && x <= cx + 24 && y >= cy - 22 && y <= cy + 28) {
                std::lock_guard<std::mutex> lock(m_mtx);
                m_raw_eq_adc[k] = 512;
                return;
            }
        }
    }

    void on_mouse_move(int x, int y) {
        (void)x;
        int drag_fader = -1;
        int drag_eq = -1;
        int eq_start_y = 0;
        int eq_start_adc = 512;
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            drag_fader = m_dragging_fader;
            drag_eq = m_dragging_eq;
            eq_start_y = m_eq_drag_start_y;
            eq_start_adc = m_eq_drag_start_adc;
        }
        if (drag_fader >= 0) {
            update_fader_from_mouse(drag_fader, y);
        } else if (drag_eq >= 0) {
            int delta_y = eq_start_y - y;
            int next_adc = std::clamp(eq_start_adc + delta_y * 6, 0, 1023);
            std::lock_guard<std::mutex> lock(m_mtx);
            m_raw_eq_adc[drag_eq] = next_adc;
        }
    }

    void on_lbutton_up() {
        std::lock_guard<std::mutex> lock(m_mtx);
        bool had_capture = (m_dragging_fader >= 0 || m_dragging_eq >= 0 || m_pressed_btn >= 0);
        m_dragging_fader = -1;
        m_dragging_eq = -1;
        m_pressed_btn = -1;
        m_btn_pressed.fill(false);
        if (had_capture || GetCapture() == m_hWnd) {
            ReleaseCapture();
        }
    }

    void update_fader_from_mouse(int ch, int y) {
        constexpr int top = 125;
        constexpr int bottom = 365;
        float norm = 1.0f - static_cast<float>(y - top) / (bottom - top);
        norm = std::clamp(norm, 0.0f, 1.0f);
        std::lock_guard<std::mutex> lock(m_mtx);
        m_raw_fader_adc[ch] = static_cast<int>(norm * 1023);
        m_display_fader_adc[ch] = m_raw_fader_adc[ch];
    }

    void render(HDC hdc) {
        std::array<int, 5> fader_adc_snap;
        std::array<int, 3> eq_adc_snap;
        std::vector<uint8_t> oled_snap;
        McuUiTelemetry tel_snap;
        uint8_t led_a, led_s, led_l;
        {
            std::lock_guard<std::mutex> lock(m_mtx);
            for (int i = 0; i < 5; ++i) {
                fader_adc_snap[i] = (m_dragging_fader == i) ? m_raw_fader_adc[i] : m_display_fader_adc[i];
            }
            eq_adc_snap = m_raw_eq_adc;
            oled_snap = m_oled_buffer;
            tel_snap = m_telemetry;
            led_a = m_led_afs;
            led_s = m_led_stem;
            led_l = m_led_loud;
        }

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
        draw_header(memDC, w, tel_snap);

        // 좌측 상단: 0.96인치 128x64 OLED 디스플레이 모듈 시뮬레이터
        draw_oled_screen(memDC, oled_snap, tel_snap);

        // 좌측 중단: 물리 택트 버튼 & LED 링 & 로터리 엔코더 & EQ 노브
        draw_physical_controls(memDC, led_a != 0, led_s != 0, led_l != 0, eq_adc_snap, tel_snap);

        // 좌측 하단: 하드웨어 음원 생성기 (CH2 비트 & CH4 라인 2)
        draw_audio_sources_card(memDC);

        // 우측 상단: 5개 물리 슬라이드 페이더 (PTA 10k 슬라이더)
        draw_physical_faders(memDC, fader_adc_snap);

        // 우측 하단 좌: 기기 전용 마이크 입력 (CH 3 MIC IN) & 하울링 테스트
        draw_hw_mic_card(memDC, tel_snap);

        // 우측 하단 우: 가상 스피커 출력 & PC 입력 브릿지
        draw_virtual_speaker_card(memDC, tel_snap);

        BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBitmap);
        DeleteObject(memBitmap);
        DeleteDC(memDC);
    }

    void draw_header(HDC hdc, int w, const McuUiTelemetry& tel) {
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
        TextOutW(hdc, 20, 12, L"🎛️ STM32H743 FIRMWARE SIMULATOR (155KB SRAM)", 46);

        // 통신 연결 및 온보드 펌웨어 활성 뱃지
        SetTextColor(hdc, tel.pc_host_linked ? RGB(57, 255, 20) : RGB(0, 210, 255));
        const wchar_t* badge = tel.pc_host_linked
            ? L"● MCU DSP ACTIVE | PC HOST LINKED (UDP)"
            : L"● MCU DSP ACTIVE | STANDALONE HW MODE";
        TextOutW(hdc, w - 340, 14, badge, static_cast<int>(wcslen(badge)));

        SelectObject(hdc, oldFont);
        SelectObject(hdc, oldPen);
        DeleteObject(linePen);
        DeleteObject(hTitleFont);
    }

    void draw_oled_screen(HDC hdc, const std::vector<uint8_t>& oled_snap, const McuUiTelemetry& tel) {
        RECT panelRc = { 20, 60, 440, 230 };
        draw_module_card(hdc, panelRc, L"0.96\" OLED DISPLAY (STM32 SSD1306 128x64)");

        // OLED 유리 영역
        RECT screenRc = { 35, 90, 425, 218 };
        HBRUSH oledBg = CreateSolidBrush(RGB(6, 8, 12));
        FillRect(hdc, &screenRc, oledBg);
        DeleteObject(oledBg);

        HPEN borderPen = CreatePen(PS_SOLID, 1, tel.howling_alert ? RGB(255, 80, 80) : RGB(0, 180, 230));
        HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, borderPen));
        MoveToEx(hdc, screenRc.left - 1, screenRc.top - 1, nullptr);
        LineTo(hdc, screenRc.right + 1, screenRc.top - 1);
        LineTo(hdc, screenRc.right + 1, screenRc.bottom + 1);
        LineTo(hdc, screenRc.left - 1, screenRc.bottom + 1);
        LineTo(hdc, screenRc.left - 1, screenRc.top - 1);
        SelectObject(hdc, oldPen);
        DeleteObject(borderPen);

        // 128x64 프레임버퍼 픽셀 렌더링
        HBRUSH pixelBrush = CreateSolidBrush(RGB(0, 220, 255));
        for (int page = 0; page < 8; ++page) {
            for (int col = 0; col < 128; ++col) {
                uint8_t b = oled_snap[page * 128 + col];
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

        // OLED 보조 Telemetry 오버레이 (MCU 상태 수치)
        HFONT hFont = CreateFontW(11, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH, L"Consolas");
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, hFont));
        SetTextColor(hdc, tel.howling_alert ? RGB(255, 100, 100) : RGB(170, 230, 255));
        wchar_t info[96];
        swprintf_s(info, L"NOTCH:%u  LUFS:%.1f (TGT:%.1f)",
                   static_cast<unsigned>(tel.active_notches),
                   tel.measured_lufs, tel.target_lufs);
        TextOutW(hdc, screenRc.left + 135, screenRc.top + 3, info, static_cast<int>(wcslen(info)));
        SelectObject(hdc, oldFont);
        DeleteObject(hFont);
    }

    void draw_physical_controls(HDC hdc, bool led_afs, bool led_stem, bool led_loud,
                                const std::array<int, 3>& eq_adc, const McuUiTelemetry& tel) {
        RECT panelRc = { 20, 240, 440, 395 };
        draw_module_card(hdc, panelRc, L"TACT SWITCHES (25ms DEB), EC11 & EQ KNOBS");

        // 1. AFS 택트 스위치 & LED (600ms 홀드 시 노치 리셋)
        draw_tact_button(hdc, 40, 275, 130, 325, L"AFS (HOLD=RST)", led_afs, RGB(57, 255, 20));

        // 2. STEM 택트 스위치 & LED
        draw_tact_button(hdc, 150, 275, 240, 325, L"STEM", led_stem, RGB(0, 210, 255));

        // 3. LOUD 택트 스위치 & LED
        draw_tact_button(hdc, 260, 275, 350, 325, L"LOUD", led_loud, RGB(255, 187, 0));

        // 4. EC11 무한 로터리 엔코더 (좌:+ / 우:- / 휠)
        draw_rotary_encoder(hdc, 375, 275, 430, 325);

        // 5. EQ 아날로그 가변저항 노브 (Low, Mid, High)
        draw_eq_knobs(hdc, 40, 335, eq_adc, tel);
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

    void draw_eq_knobs(HDC hdc, int start_x, int y0, const std::array<int, 3>& eq_adc, const McuUiTelemetry& tel) {
        (void)eq_adc;
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

            // 포인터 라인 (-12dB = -135 deg, 0dB = 0 deg top, +12dB = +135 deg)
            float db = tel.eq_db[k];
            float angle_rad = (db / 12.0f) * (135.0f * 3.14159265f / 180.0f);
            int px = cx + static_cast<int>(std::sin(angle_rad) * 13.0f);
            int py = cy - static_cast<int>(std::cos(angle_rad) * 13.0f);

            HPEN ptrPen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
            SelectObject(hdc, ptrPen);
            MoveToEx(hdc, cx, cy, nullptr);
            LineTo(hdc, px, py);
            DeleteObject(ptrPen);

            // 라벨 + dB 수치
            wchar_t lbl[40];
            swprintf_s(lbl, L"%s (%+.1fdB)", eq_labels[k], db);
            SetTextColor(hdc, RGB(180, 185, 195));
            RECT lblRc = { cx - 58, cy + 18, cx + 58, cy + 34 };
            DrawTextW(hdc, lbl, -1, &lblRc, DT_CENTER | DT_SINGLELINE);
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
        DrawTextW(hdc, L"● Onboard STM32 DSP: AFS, 3-Band EQ, Mixer, LUFS & OLED", -1, &infoRc, DT_LEFT);

        SelectObject(hdc, oldFont);
        DeleteObject(hFont);
    }

    void draw_physical_faders(HDC hdc, const std::array<int, 5>& fader_adc) {
        RECT panelRc = { 460, 60, 935, 434 };
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
            float norm = fader_adc[i] / 1023.0f;
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
            swprintf_s(adcBuf, L"%d", fader_adc[i]);
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

    void draw_hw_mic_card(HDC hdc, const McuUiTelemetry& tel) {
        RECT panelRc = { 460, 442, 692, 600 };
        draw_module_card(hdc, panelRc, L"🎤 HW MIC IN (CH 3 ONLY)");

        HFONT hFont = CreateFontW(12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH, L"Segoe UI");
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, hFont));

        // 마이크 입력 ADC 레벨 바 (y: 512 ~ 530)
        SetTextColor(hdc, RGB(170, 175, 185));
        RECT lblRc = { 472, 502, 680, 518 };
        DrawTextW(hdc, L"CH 3 MIC ADC INPUT LEVEL:", -1, &lblRc, DT_LEFT | DT_SINGLELINE);

        RECT trackRc = { 472, 520, 680, 536 };
        HBRUSH trackBr = CreateSolidBrush(RGB(15, 16, 18));
        FillRect(hdc, &trackRc, trackBr);
        DeleteObject(trackBr);

        float lvl = std::clamp(tel.mic_rms * 2.5f, 0.0f, 1.0f);
        int fill_w = static_cast<int>(lvl * (trackRc.right - trackRc.left));
        if (fill_w > 0) {
            RECT fillRc = { trackRc.left, trackRc.top, trackRc.left + fill_w, trackRc.bottom };
            COLORREF col = (lvl > 0.8f) ? RGB(255, 60, 60) : ((lvl > 0.45f) ? RGB(255, 187, 0) : RGB(57, 255, 20));
            HBRUSH fBr = CreateSolidBrush(col);
            FillRect(hdc, &fillRc, fBr);
            DeleteObject(fBr);
        }

        // 2.5kHz 하울링 주입 테스트 버튼 (y: 554 ~ 588)
        RECT howlRc = { 472, 554, 680, 588 };
        HBRUSH hBr = CreateSolidBrush(RGB(115, 32, 32));
        FillRect(hdc, &howlRc, hBr);
        DeleteObject(hBr);
        SetTextColor(hdc, RGB(255, 225, 225));
        DrawTextW(hdc, L"⚡ 2.5kHz 기기 하울링 주입", -1, &howlRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(hdc, oldFont);
        DeleteObject(hFont);
    }

    void draw_virtual_speaker_card(HDC hdc, const McuUiTelemetry& tel) {
        RECT panelRc = { 702, 442, 935, 600 };
        draw_module_card(hdc, panelRc, L"🔊 VIRTUAL SPEAKER -> PC IN");

        HFONT hFont = CreateFontW(11, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH, L"Segoe UI");
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, hFont));

        // 32-샘플 실시간 가상 스피커 출력 파형 박스 (y: 504 ~ 544)
        RECT waveRc = { 714, 504, 922, 544 };
        HBRUSH wBr = CreateSolidBrush(RGB(10, 14, 18));
        FillRect(hdc, &waveRc, wBr);
        DeleteObject(wBr);

        int mid_y = (waveRc.top + waveRc.bottom) / 2;
        HPEN midPen = CreatePen(PS_SOLID, 1, RGB(35, 45, 55));
        HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, midPen));
        MoveToEx(hdc, waveRc.left, mid_y, nullptr);
        LineTo(hdc, waveRc.right, mid_y);
        DeleteObject(midPen);

        HPEN wavePen = CreatePen(PS_SOLID, 2, RGB(0, 220, 255));
        SelectObject(hdc, wavePen);
        int w_span = waveRc.right - waveRc.left;
        for (int i = 0; i < 32; ++i) {
            int x = waveRc.left + (i * w_span) / 31;
            int y = mid_y - static_cast<int>(std::clamp(tel.spk_wave[i], -1.0f, 1.0f) * 18.0f);
            if (i == 0) MoveToEx(hdc, x, y, nullptr);
            else        LineTo(hdc, x, y);
        }
        SelectObject(hdc, oldPen);
        DeleteObject(wavePen);

        // L / R 스테레오 가상 스피커 아웃풋 미터 (y: 550 ~ 568)
        float peaks[2] = { std::clamp(tel.out_peak_l * 1.4f, 0.0f, 1.0f),
                           std::clamp(tel.out_peak_r * 1.4f, 0.0f, 1.0f) };
        for (int ch = 0; ch < 2; ++ch) {
            int y0 = 550 + ch * 9;
            RECT bg = { 732, y0, 922, y0 + 6 };
            HBRUSH bgB = CreateSolidBrush(RGB(20, 24, 28));
            FillRect(hdc, &bg, bgB);
            DeleteObject(bgB);

            SetTextColor(hdc, RGB(170, 175, 185));
            RECT chTxt = { 714, y0 - 3, 730, y0 + 9 };
            DrawTextW(hdc, ch == 0 ? L"L" : L"R", -1, &chTxt, DT_LEFT | DT_SINGLELINE);

            int fw = static_cast<int>(peaks[ch] * (bg.right - bg.left));
            if (fw > 0) {
                RECT fg = { bg.left, bg.top, bg.left + fw, bg.bottom };
                HBRUSH fgB = CreateSolidBrush(RGB(0, 210, 255));
                FillRect(hdc, &fg, fgB);
                DeleteObject(fgB);
            }
        }

        // 상태 표시줄
        SetTextColor(hdc, tel.pc_host_linked ? RGB(57, 255, 20) : RGB(0, 210, 255));
        RECT stRc = { 714, 574, 924, 594 };
        DrawTextW(hdc, L"● Virtual Speaker Out -> PC Software Input", -1, &stRc, DT_LEFT | DT_SINGLELINE);

        SelectObject(hdc, oldFont);
        DeleteObject(hFont);
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
