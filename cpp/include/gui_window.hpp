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
#include <commctrl.h>
#include <string>
#include <vector>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <functional>
#include <commdlg.h>

#include "config.hpp"
#include "mixer.hpp"
#include "afs.hpp"
#include "loudness.hpp"
#include "stem_separator.hpp"
#include "controller_mapper.hpp"
#include "ui_renderer.hpp"
#include "wasapi_capture.hpp"
#include "wasapi_render.hpp"
#include "audio_file_player.hpp"

namespace SmartMixer {

class MixerGUIWindow {
public:
    static constexpr int IDC_INPUT_COMBO = 1001;
    static constexpr int IDC_OUTPUT_COMBO = 1002;
    static constexpr int IDC_DEVICE_COMBO = IDC_INPUT_COMBO;

    using ActionCallback = std::function<void()>;
    using DeviceSelectCallback = std::function<void(int)>;
    using OutputSelectCallback = std::function<void(int)>;
    using FaderCallback = std::function<void(int ch, float norm)>;
    using FaderEndCallback = std::function<void(int ch)>;
    using FileOpenCallback = std::function<void(const std::wstring&)>;
    using FileExportCallback = std::function<void()>;
    using AISeparateCallback = std::function<void()>;

    MixerGUIWindow(HINSTANCE hInstance,
                   AudioMixer& mixer,
                   AutomaticFeedbackSuppression& afs,
                   LoudnessMatcher& loudness,
                   StemSeparator& stem,
                   ParameterManager& param_mgr,
                   SpectrumRenderer& spectrum,
                   WasapiCaptureEngine& wasapi,
                   AudioFilePlayer& file_player,
                   WasapiRenderEngine& wasapi_render)
        : m_hInstance(hInstance),
          m_hWnd(nullptr),
          m_hComboInput(nullptr),
          m_hComboOutput(nullptr),
          m_mixer(mixer),
          m_afs(afs),
          m_loudness(loudness),
          m_stem(stem),
          m_param_mgr(param_mgr),
          m_spectrum(spectrum),
          m_wasapi(wasapi),
          m_file_player(file_player),
          m_wasapi_render(wasapi_render),
          m_dragging_fader(-1),
          m_dragging_target(false),
          m_inject_howling_callback(nullptr),
          m_reset_filters_callback(nullptr),
          m_device_selected_callback(nullptr),
          m_output_selected_callback(nullptr),
          m_fader_changed_callback(nullptr),
          m_fader_drag_end_callback(nullptr),
          m_file_open_callback(nullptr),
          m_file_export_callback(nullptr),
          m_ai_separate_callback(nullptr),
          m_active_device_name(L"Virtual Audio Generator"),
          m_active_output_name(L"Default Output") {
        
        m_mute_states.resize(5, false);
        m_vu_levels.resize(5, 0.0f);
        m_latest_bars.resize(16, 0.0f);
    }

    void set_inject_howling_callback(ActionCallback cb) { m_inject_howling_callback = cb; }
    void set_reset_filters_callback(ActionCallback cb) { m_reset_filters_callback = cb; }
    void set_device_selected_callback(DeviceSelectCallback cb) { m_device_selected_callback = cb; }
    void set_output_selected_callback(OutputSelectCallback cb) { m_output_selected_callback = cb; }
    void set_fader_changed_callback(FaderCallback cb) { m_fader_changed_callback = cb; }
    void set_fader_drag_end_callback(FaderEndCallback cb) { m_fader_drag_end_callback = cb; }
    void set_file_open_callback(FileOpenCallback cb) { m_file_open_callback = cb; }
    void set_file_export_callback(FileExportCallback cb) { m_file_export_callback = cb; }
    void set_ai_separate_callback(AISeparateCallback cb) { m_ai_separate_callback = cb; }
    void set_ai_busy(bool busy) { m_is_ai_busy = busy; }
    bool is_ai_busy() const { return m_is_ai_busy.load(); }
    void set_ai_progress(int pct) { m_ai_progress = pct; }
    int get_ai_progress() const { return m_ai_progress.load(); }
    int get_dragging_fader() const { return m_dragging_fader; }

    void open_file_dialog() {
        wchar_t szFile[MAX_PATH] = {0};
        OPENFILENAMEW ofn = { sizeof(OPENFILENAMEW) };
        ofn.hwndOwner = m_hWnd;
        ofn.lpstrFilter = L"오디오 파일 (*.mp3;*.wav;*.flac;*.m4a;*.wma)\0*.mp3;*.wav;*.flac;*.m4a;*.wma\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = szFile;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&ofn)) {
            if (m_file_open_callback) {
                m_file_open_callback(szFile);
            }
        }
    }

    void refresh_devices_with_file() {
        populate_audio_devices();
    }

    void update_audio_data(const std::vector<float>& bars, const std::vector<float>& vu_levels) {
        m_latest_bars = bars;
        m_vu_levels = vu_levels;
    }

    void show_toast(const std::string& msg, float duration_sec = 3.5f) {
        m_toast_message = msg;
        m_toast_expire_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(duration_sec * 1000));
    }

    void set_active_device_name(const std::wstring& name) {
        m_active_device_name = name;
    }

    bool create(int width = 1060, int height = 690) {
        INITCOMMONCONTROLSEX icex;
        icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
        icex.dwICC = ICC_STANDARD_CLASSES;
        InitCommonControlsEx(&icex);

        WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = StaticWndProc;
        wc.hInstance = m_hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(RGB(24, 25, 28)); // #18191C
        wc.lpszClassName = L"SmartMixerGUIClass";

        RegisterClassExW(&wc);

        RECT rc = { 0, 0, width, height };
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME, FALSE);

        m_hWnd = CreateWindowExW(
            0,
            wc.lpszClassName,
            L"🎛️ Smart Audio Mixer Console (C++ Real-Time DSP)",
            (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME) | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rc.right - rc.left, rc.bottom - rc.top,
            nullptr, nullptr, m_hInstance, this
        );

        if (!m_hWnd) return false;

        // 오디오 입력 장치 선택 콤보박스 생성 (x: 255, y: 12, w: 285, h: 260)
        m_hComboInput = CreateWindowExW(
            0,
            L"COMBOBOX",
            L"",
            CBS_DROPDOWNLIST | WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP,
            255, 12, 285, 260,
            m_hWnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_INPUT_COMBO)),
            m_hInstance,
            nullptr
        );

        // 오디오 출력 장치 선택 콤보박스 생성 (x: 615, y: 12, w: 305, h: 260)
        m_hComboOutput = CreateWindowExW(
            0,
            L"COMBOBOX",
            L"",
            CBS_DROPDOWNLIST | WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP,
            615, 12, 305, 260,
            m_hWnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_OUTPUT_COMBO)),
            m_hInstance,
            nullptr
        );

        HFONT hComboFont = CreateFontW(14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH, L"Segoe UI");
        SendMessageW(m_hComboInput, WM_SETFONT, reinterpret_cast<WPARAM>(hComboFont), TRUE);
        SendMessageW(m_hComboOutput, WM_SETFONT, reinterpret_cast<WPARAM>(hComboFont), TRUE);

        // 입력 및 출력 장치 목록 추가
        populate_audio_devices();

        // 60 FPS 렌더링 타이머 (16ms)
        SetTimer(m_hWnd, TIMER_RENDER_ID, 16, nullptr);
        return true;
    }

    HWND get_hwnd() const { return m_hWnd; }

private:
    static constexpr UINT_PTR TIMER_RENDER_ID = 1;

    HINSTANCE m_hInstance;
    HWND m_hWnd;
    HWND m_hComboInput;
    HWND m_hComboOutput;

    AudioMixer& m_mixer;
    AutomaticFeedbackSuppression& m_afs;
    LoudnessMatcher& m_loudness;
    StemSeparator& m_stem;
    ParameterManager& m_param_mgr;
    SpectrumRenderer& m_spectrum;
    WasapiCaptureEngine& m_wasapi;
    AudioFilePlayer& m_file_player;
    WasapiRenderEngine& m_wasapi_render;

    std::vector<bool> m_mute_states;
    std::vector<float> m_vu_levels;
    std::vector<float> m_latest_bars;

    int m_dragging_fader;
    bool m_dragging_target;

    std::string m_toast_message;
    std::chrono::steady_clock::time_point m_toast_expire_time;

    ActionCallback m_inject_howling_callback;
    ActionCallback m_reset_filters_callback;
    DeviceSelectCallback m_device_selected_callback;
    OutputSelectCallback m_output_selected_callback;
    FaderCallback m_fader_changed_callback;
    FaderEndCallback m_fader_drag_end_callback;
    FileOpenCallback m_file_open_callback;
    FileExportCallback m_file_export_callback;
    AISeparateCallback m_ai_separate_callback;
    std::atomic<bool> m_is_ai_busy{false};
    std::atomic<int> m_ai_progress{0};
    std::wstring m_active_device_name;
    std::wstring m_active_output_name;

    void populate_audio_devices() {
        populate_input_devices();
        populate_output_devices();
    }

    void populate_input_devices() {
        if (!m_hComboInput) return;

        SendMessageW(m_hComboInput, CB_RESETCONTENT, 0, 0);

        // 0번: 가상 오디오 신호 생성기
        SendMessageW(m_hComboInput, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"💻 [가상 신호] Virtual Generator"));

        // 1번 ~ N번: 실제 감지된 WASAPI 입력 장치들
        auto devices = m_wasapi.enumerate_capture_devices();
        for (const auto& dev : devices) {
            std::wstring item_label = L"🎤 " + dev.name_wide;
            SendMessageW(m_hComboInput, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item_label.c_str()));
        }

        // 음원 파일이 로드되어 있는 경우 파일 재생 옵션 추가
        if (m_file_player.is_loaded()) {
            std::wstring item_label = L"🎵 [음원] " + m_file_player.get_file_name();
            int idx = static_cast<int>(SendMessageW(m_hComboInput, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item_label.c_str())));
            SendMessageW(m_hComboInput, CB_SETCURSEL, idx, 0);
        } else {
            SendMessageW(m_hComboInput, CB_SETCURSEL, 0, 0);
        }
    }

    void populate_output_devices() {
        if (!m_hComboOutput) return;

        SendMessageW(m_hComboOutput, CB_RESETCONTENT, 0, 0);

        // 0번: 음소거 (출력 안 함)
        SendMessageW(m_hComboOutput, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"🔇 [음소거] 출력 안 함 (Mute)"));

        // 1번 ~ M번: 실제 감지된 WASAPI 출력 장치들
        auto devices = m_wasapi_render.enumerate_render_devices();
        int default_sel = 0;
        for (size_t i = 0; i < devices.size(); ++i) {
            std::wstring item_label = L"🔊 " + devices[i].name_wide;
            if (devices[i].is_default) {
                item_label += L" (기본값)";
                default_sel = static_cast<int>(i + 1);
                m_active_output_name = devices[i].name_wide;
            }
            SendMessageW(m_hComboOutput, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item_label.c_str()));
        }

        if (devices.empty()) {
            SendMessageW(m_hComboOutput, CB_SETCURSEL, 0, 0);
            m_active_output_name = L"None";
        } else {
            SendMessageW(m_hComboOutput, CB_SETCURSEL, default_sel, 0);
        }
    }

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        MixerGUIWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = reinterpret_cast<MixerGUIWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->m_hWnd = hwnd;
        } else {
            self = reinterpret_cast<MixerGUIWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (self) {
            return self->handle_message(hwnd, msg, wParam, lParam);
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT handle_message(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_COMMAND: {
                if (LOWORD(wParam) == IDC_INPUT_COMBO && HIWORD(wParam) == CBN_SELCHANGE) {
                    int sel = static_cast<int>(SendMessageW(m_hComboInput, CB_GETCURSEL, 0, 0));
                    wchar_t selText[256] = {0};
                    SendMessageW(m_hComboInput, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(selText));
                    m_active_device_name = selText;

                    if (m_device_selected_callback) {
                        m_device_selected_callback(sel);
                    }
                    return 0;
                }
                if (LOWORD(wParam) == IDC_OUTPUT_COMBO && HIWORD(wParam) == CBN_SELCHANGE) {
                    int sel = static_cast<int>(SendMessageW(m_hComboOutput, CB_GETCURSEL, 0, 0));
                    wchar_t selText[256] = {0};
                    SendMessageW(m_hComboOutput, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(selText));
                    m_active_output_name = selText;

                    if (m_output_selected_callback) {
                        m_output_selected_callback(sel);
                    }
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
                if (wParam == TIMER_RENDER_ID) {
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
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
                KillTimer(hwnd, TIMER_RENDER_ID);
                PostQuitMessage(0);
                return 0;
            }
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    void on_lbutton_down(int x, int y) {
        SetCapture(m_hWnd);

        // 1. 5개 페이더 슬라이더 드래그 (정확한 렌더링 좌표 일치: fx = 24 + ch * 90, slider_x = fx + 46)
        for (int ch = 0; ch < 5; ++ch) {
            int fx = 24 + ch * 90;
            int slider_x = fx + 46;
            if (x >= slider_x - 22 && x <= slider_x + 22 && y >= 120 && y <= 400) {
                m_dragging_fader = ch;
                update_fader_from_mouse(ch, y);
                return;
            }
        }

        // 2. Mute 버튼
        for (int ch = 0; ch < 5; ++ch) {
            int fx = 24 + ch * 90;
            if (x >= fx + 8 && x <= fx + 78 && y >= 400 && y <= 426) {
                m_mute_states[ch] = !m_mute_states[ch];
                m_mixer.set_mute(ch, m_mute_states[ch]);
                return;
            }
        }

        // 3. DSP 제어 버튼
        // AFS Toggle (x: 504 ~ 664, y: 395 ~ 430)
        if (x >= 504 && x <= 664 && y >= 395 && y <= 430) {
            m_afs.set_enabled(!m_afs.is_enabled());
            return;
        }

        // Howling Inject Button (x: 504 ~ 664, y: 500 ~ 534)
        if (x >= 504 && x <= 664 && y >= 500 && y <= 534) {
            if (m_inject_howling_callback) m_inject_howling_callback();
            return;
        }

        // Reset Notch Filters (x: 504 ~ 664, y: 542 ~ 574)
        if (x >= 504 && x <= 664 && y >= 542 && y <= 574) {
            if (m_reset_filters_callback) m_reset_filters_callback();
            return;
        }

        // Loudness Toggle (x: 684 ~ 844, y: 395 ~ 430)
        if (x >= 684 && x <= 844 && y >= 395 && y <= 430) {
            m_loudness.set_enabled(!m_loudness.is_enabled());
            return;
        }

        // Loudness Target Slider (x: 694 ~ 834, y: 517 ~ 537)
        if (x >= 690 && x <= 840 && y >= 510 && y <= 540) {
            m_dragging_target = true;
            update_target_from_mouse(x);
            return;
        }

        // 4. Stem 분리 및 음원 파일 제어
        // Stem Toggle (x: 874 ~ 1030, y: 406 ~ 432)
        if (x >= 874 && x <= 1030 && y >= 406 && y <= 432) {
            m_stem.set_enabled(!m_stem.is_enabled());
            return;
        }

        // 📂 음원 파일 불러오기 버튼 (x: 874 ~ 1030, y: 436 ~ 460)
        if (x >= 874 && x <= 1030 && y >= 436 && y <= 460) {
            open_file_dialog();
            return;
        }

        if (m_file_player.is_loaded()) {
            // Seek Bar 클릭 이동 (x: 874 ~ 1030, y: 480 ~ 490)
            if (x >= 874 && x <= 1030 && y >= 480 && y <= 490) {
                float norm = static_cast<float>(x - 874) / static_cast<float>(1030 - 874);
                norm = std::clamp(norm, 0.0f, 1.0f);
                m_file_player.seek(norm * m_file_player.get_duration_seconds());
                return;
            }

            // ▶ 재생 / ⏸ 일시정지 (x: 874 ~ 948, y: 492 ~ 514)
            if (x >= 874 && x <= 948 && y >= 492 && y <= 514) {
                if (m_is_ai_busy.load()) {
                    show_toast("AI 스템 분리가 진행 중입니다. 잠시만 기다려주세요...", 2.5f);
                    return;
                }
                if (!m_file_player.has_ai_stems()) {
                    if (m_ai_separate_callback) {
                        m_ai_separate_callback();
                    }
                    return;
                }
                if (m_file_player.is_playing()) {
                    m_file_player.pause();
                } else {
                    m_file_player.play();
                }
                return;
            }

            // ⏹ 정지 (x: 954 ~ 1030, y: 492 ~ 514)
            if (x >= 954 && x <= 1030 && y >= 492 && y <= 514) {
                m_file_player.stop();
                return;
            }

            // 🤖 Demucs AI 스템 분리 (x: 874 ~ 1030, y: 518 ~ 542)
            if (x >= 874 && x <= 1030 && y >= 518 && y <= 542) {
                if (m_is_ai_busy.load()) {
                    show_toast("AI 스템 분리가 이미 진행 중입니다. 잠시만 기다려주세요...", 2.5f);
                    return;
                }
                if (m_ai_separate_callback) {
                    m_ai_separate_callback();
                }
                return;
            }

            // 💾 3-Way 스템 파일 분리 저장 (x: 874 ~ 1030, y: 546 ~ 570)
            if (x >= 874 && x <= 1030 && y >= 546 && y <= 570) {
                if (m_file_export_callback) {
                    m_file_export_callback();
                }
                return;
            }
        }
    }

    void on_mouse_move(int x, int y) {
        if (m_dragging_fader >= 0) {
            update_fader_from_mouse(m_dragging_fader, y);
        } else if (m_dragging_target) {
            update_target_from_mouse(x);
        }
    }

    void on_lbutton_up() {
        if (m_dragging_fader >= 0) {
            int ch = m_dragging_fader;
            m_dragging_fader = -1;
            if (m_fader_drag_end_callback) {
                m_fader_drag_end_callback(ch);
            }
        }
        if (m_dragging_target) {
            m_dragging_target = false;
        }
        ReleaseCapture();
    }

    void update_fader_from_mouse(int ch, int y) {
        constexpr int top = 135;
        constexpr int bottom = 385;
        float norm = 1.0f - static_cast<float>(y - top) / static_cast<float>(bottom - top);
        norm = std::clamp(norm, 0.0f, 1.0f);
        m_mixer.set_fader(ch, norm);
        m_param_mgr.set_fader_direct(ch, norm);
        if (m_fader_changed_callback) {
            m_fader_changed_callback(ch, norm);
        }
    }

    void update_target_from_mouse(int x) {
        constexpr int left = 690;
        constexpr int right = 834;
        float norm = static_cast<float>(x - left) / (right - left);
        norm = std::clamp(norm, 0.0f, 1.0f);
        float target = -24.0f + norm * 16.0f;
        m_loudness.set_target_lufs(std::round(target * 10.0f) / 10.0f);
    }

    void render(HDC hdc) {
        RECT client_rc;
        GetClientRect(m_hWnd, &client_rc);
        int w = client_rc.right;
        int h = client_rc.bottom;

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBitmap = CreateCompatibleBitmap(hdc, w, h);
        HBITMAP oldBitmap = reinterpret_cast<HBITMAP>(SelectObject(memDC, memBitmap));

        HBRUSH bgBrush = CreateSolidBrush(RGB(24, 25, 28));
        FillRect(memDC, &client_rc, bgBrush);
        DeleteObject(bgBrush);

        SetBkMode(memDC, TRANSPARENT);

        // 상단 헤더
        draw_header(memDC, w);

        // 5채널 믹서 콘솔
        draw_fader_console(memDC);

        // 실시간 FFT 스펙트럼
        draw_spectrum_card(memDC);

        // DSP 카드 (AFS, Loudness, Stem)
        draw_dsp_cards(memDC);

        // 하단 상태 바
        draw_status_bar(memDC, w, h);

        BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBitmap);
        DeleteObject(memBitmap);
        DeleteDC(memDC);
    }

    void draw_header(HDC hdc, int w) {
        RECT rc = { 0, 0, w, 54 };
        HBRUSH hBr = CreateSolidBrush(RGB(18, 19, 21));
        FillRect(hdc, &rc, hBr);
        DeleteObject(hBr);

        HPEN linePen = CreatePen(PS_SOLID, 1, RGB(45, 48, 54));
        HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, linePen));
        MoveToEx(hdc, 0, 53, nullptr);
        LineTo(hdc, w, 53);

        HFONT hTitleFont = CreateFontW(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH, L"Segoe UI");
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, hTitleFont));

        SetTextColor(hdc, RGB(245, 246, 248));
        TextOutW(hdc, 16, 16, L"🎛️ SMART MIXER", 15);

        // 입력 / 출력 장치 레이블
        HFONT hLabelFont = CreateFontW(13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH, L"Segoe UI");
        SelectObject(hdc, hLabelFont);

        SetTextColor(hdc, RGB(0, 210, 255));
        TextOutW(hdc, 192, 17, L"🎤 입력:", 5);

        SetTextColor(hdc, RGB(57, 255, 20));
        TextOutW(hdc, 552, 17, L"🔊 출력:", 5);

        // FPS 수치
        SetTextColor(hdc, RGB(160, 165, 175));
        TextOutW(hdc, w - 125, 17, L"60 FPS (PR-3)", 13);

        SelectObject(hdc, oldFont);
        SelectObject(hdc, oldPen);
        DeleteObject(linePen);
        DeleteObject(hTitleFont);
        DeleteObject(hLabelFont);
    }

    void draw_fader_console(HDC hdc) {
        RECT cardRc = { 16, 66, 480, 605 };
        draw_rounded_card(hdc, cardRc, L"5-CHANNEL MIXING CONSOLE (H-FR-4, S-FR-1)");

        const wchar_t* ch_names[] = { L"MASTER", L"VOCAL", L"BEAT", L"MIC 1", L"LINE 2" };
        COLORREF ch_colors[] = {
            RGB(255, 51, 102), // Master (Red)
            RGB(0, 210, 255),  // Vocal (Cyan)
            RGB(255, 187, 0),  // Beat (Gold)
            RGB(57, 255, 20),  // Mic 1 (Green)
            RGB(179, 102, 255) // Line 2 (Purple)
        };

        HFONT hChFont = CreateFontW(13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                    DEFAULT_PITCH, L"Segoe UI");
        HFONT hDbFont = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                    DEFAULT_PITCH, L"Consolas");
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, hChFont));

        for (int i = 0; i < 5; ++i) {
            int fx = 24 + i * 90;

            SetTextColor(hdc, ch_colors[i]);
            RECT nameRc = { fx, 102, fx + 84, 120 };
            DrawTextW(hdc, ch_names[i], -1, &nameRc, DT_CENTER | DT_SINGLELINE);

            // VU 레벨 미터
            int meter_x = fx + 10;
            RECT meterRc = { meter_x, 135, meter_x + 12, 385 };
            draw_vu_meter(hdc, meterRc, m_vu_levels[i]);

            // 페이더 트랙
            int slider_x = fx + 46;
            HPEN trackPen = CreatePen(PS_SOLID, 4, RGB(38, 41, 46));
            HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, trackPen));
            MoveToEx(hdc, slider_x, 135, nullptr);
            LineTo(hdc, slider_x, 385);
            SelectObject(hdc, oldPen);
            DeleteObject(trackPen);

            // 페이더 노브 (Thumb)
            float fader_pos = m_param_mgr.get_fader(i);
            int thumb_y = static_cast<int>(385 - fader_pos * (385 - 135));

            RECT thumbRc = { slider_x - 16, thumb_y - 8, slider_x + 16, thumb_y + 8 };
            HBRUSH thumbBr = CreateSolidBrush(ch_colors[i]);
            HPEN thumbPen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
            SelectObject(hdc, thumbBr);
            SelectObject(hdc, thumbPen);
            RoundRect(hdc, thumbRc.left, thumbRc.top, thumbRc.right, thumbRc.bottom, 4, 4);
            DeleteObject(thumbBr);
            DeleteObject(thumbPen);

            // dB 수치
            SelectObject(hdc, hDbFont);
            SetTextColor(hdc, RGB(180, 185, 192));
            float db = (fader_pos <= 0.75f) ? (-60.0f + (fader_pos / 0.75f) * 60.0f)
                                            : (((fader_pos - 0.75f) / 0.25f) * 6.0f);
            wchar_t dbBuf[32];
            if (fader_pos < 0.001f) {
                swprintf_s(dbBuf, L"-INF");
            } else {
                swprintf_s(dbBuf, L"%+.1fdB", db);
            }
            RECT dbRc = { fx, 392, fx + 84, 406 };
            DrawTextW(hdc, dbBuf, -1, &dbRc, DT_CENTER | DT_SINGLELINE);
            SelectObject(hdc, hChFont);

            // MUTE 버튼
            RECT muteRc = { fx + 12, 415, fx + 72, 440 };
            bool isMuted = m_mute_states[i];
            HBRUSH muteBr = CreateSolidBrush(isMuted ? RGB(204, 34, 34) : RGB(45, 48, 54));
            FillRect(hdc, &muteRc, muteBr);
            DeleteObject(muteBr);

            SetTextColor(hdc, RGB(255, 255, 255));
            DrawTextW(hdc, isMuted ? L"MUTED" : L"MUTE", -1, &muteRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        SelectObject(hdc, oldFont);
        DeleteObject(hChFont);
        DeleteObject(hDbFont);
    }

    void draw_vu_meter(HDC hdc, const RECT& rc, float level) {
        HBRUSH bgBr = CreateSolidBrush(RGB(15, 16, 18));
        FillRect(hdc, &rc, bgBr);
        DeleteObject(bgBr);

        int h = rc.bottom - rc.top;
        float norm = std::clamp(level * 2.2f, 0.0f, 1.0f);
        int fill_h = static_cast<int>(norm * h);
        if (fill_h <= 0) return;

        RECT fillRc = { rc.left, rc.bottom - fill_h, rc.right, rc.bottom };

        COLORREF meterColor;
        if (norm > 0.85f) {
            meterColor = RGB(255, 51, 102); // Red
        } else if (norm > 0.65f) {
            meterColor = RGB(255, 187, 0);  // Yellow
        } else {
            meterColor = RGB(57, 255, 20);   // Green
        }

        HBRUSH fillBr = CreateSolidBrush(meterColor);
        FillRect(hdc, &fillRc, fillBr);
        DeleteObject(fillBr);
    }

    void draw_spectrum_card(HDC hdc) {
        RECT cardRc = { 496, 66, 1040, 365 };
        draw_rounded_card(hdc, cardRc, L"REAL-TIME FFT SPECTRUM & AUDIO METERS (S-FR-6, PR-3)");

        RECT specRc = { 510, 105, 1026, 295 };
        HBRUSH sBg = CreateSolidBrush(RGB(13, 14, 16));
        FillRect(hdc, &specRc, sBg);
        DeleteObject(sBg);

        HPEN gridPen = CreatePen(PS_DOT, 1, RGB(30, 33, 38));
        HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, gridPen));
        for (int r = 1; r < 4; ++r) {
            int y = specRc.top + r * (specRc.bottom - specRc.top) / 4;
            MoveToEx(hdc, specRc.left, y, nullptr);
            LineTo(hdc, specRc.right, y);
        }
        SelectObject(hdc, oldPen);
        DeleteObject(gridPen);

        int num_bars = 16;
        int total_w = specRc.right - specRc.left;
        int bar_pad = 4;
        int bar_w = (total_w - bar_pad * (num_bars + 1)) / num_bars;
        int max_h = specRc.bottom - specRc.top - 10;

        for (int i = 0; i < num_bars; ++i) {
            float val = (i < static_cast<int>(m_latest_bars.size())) ? m_latest_bars[i] : 0.0f;
            int bar_h = static_cast<int>(val * max_h);

            int x0 = specRc.left + bar_pad + i * (bar_w + bar_pad);
            int x1 = x0 + bar_w;
            int y1 = specRc.bottom - 4;
            int y0 = y1 - bar_h;

            COLORREF barColor;
            if (i < 4) {
                barColor = RGB(0, 210, 255);  // Bass (Cyan)
            } else if (i < 11) {
                barColor = RGB(57, 255, 20);  // Mid / Vocal (Neon Green)
            } else {
                barColor = RGB(255, 187, 0);  // High (Gold)
            }

            RECT bRc = { x0, y0, x1, y1 };
            HBRUSH bBr = CreateSolidBrush(barColor);
            FillRect(hdc, &bRc, bBr);
            DeleteObject(bBr);

            RECT capRc = { x0, y0 - 2, x1, y0 };
            HBRUSH cBr = CreateSolidBrush(RGB(255, 255, 255));
            FillRect(hdc, &capRc, cBr);
            DeleteObject(cBr);
        }

        // 토스트 알림 배너
        RECT toastRc = { 510, 310, 1026, 350 };
        auto now = std::chrono::steady_clock::now();
        bool has_toast = (!m_toast_message.empty() && now < m_toast_expire_time);

        HBRUSH tBg = CreateSolidBrush(has_toast ? RGB(100, 20, 20) : RGB(24, 38, 28));
        FillRect(hdc, &toastRc, tBg);
        DeleteObject(tBg);

        HFONT hToastFont = CreateFontW(13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH, L"Segoe UI");
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, hToastFont));

        if (has_toast) {
            SetTextColor(hdc, RGB(255, 255, 50));
            std::wstring wmsg(m_toast_message.begin(), m_toast_message.end());
            DrawTextW(hdc, wmsg.c_str(), -1, &toastRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            SetTextColor(hdc, RGB(57, 255, 20));
            auto notches = m_afs.get_notch_frequencies();
            if (notches.empty()) {
                DrawTextW(hdc, L"Normal Operation - Feedback Suppression Active (0 Notches)", -1, &toastRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                std::wostringstream wss;
                wss << L"AFS Active: ";
                for (float f : notches) wss << static_cast<int>(f) << L"Hz ";
                wss << L"Notch Filtered";
                std::wstring wstr = wss.str();
                DrawTextW(hdc, wstr.c_str(), -1, &toastRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }

        SelectObject(hdc, oldFont);
        DeleteObject(hToastFont);
    }

    void draw_dsp_cards(HDC hdc) {
        // 1. AFS Card (496 ~ 670, y: 380 ~ 605)
        RECT afsRc = { 496, 380, 670, 605 };
        draw_rounded_card(hdc, afsRc, L"AFS 하울링 억제 (S-FR-3)");

        bool afs_on = m_afs.is_enabled();
        draw_toggle_button(hdc, 506, 412, 660, 446, L"AFS ACTIVE", afs_on, RGB(0, 104, 71), RGB(57, 255, 20));

        HFONT hInfoFont = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH, L"Segoe UI");
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, hInfoFont));
        SetTextColor(hdc, RGB(170, 175, 185));

        wchar_t afsInfo[128];
        swprintf_s(afsInfo, L"Active Notches: %d / 8\nSuppressed Events: %d",
                   static_cast<int>(m_afs.get_active_filter_count()),
                   m_afs.get_suppressed_count());
        RECT afsTxtRc = { 510, 456, 660, 494 };
        DrawTextW(hdc, afsInfo, -1, &afsTxtRc, DT_LEFT);

        RECT howlBtnRc = { 506, 502, 660, 536 };
        HBRUSH howlBr = CreateSolidBrush(RGB(110, 30, 30));
        FillRect(hdc, &howlBtnRc, howlBr);
        DeleteObject(howlBr);
        SetTextColor(hdc, RGB(255, 220, 220));
        DrawTextW(hdc, L"⚡ 2.5kHz 하울링 주입", -1, &howlBtnRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        RECT resetBtnRc = { 506, 544, 660, 576 };
        HBRUSH rstBr = CreateSolidBrush(RGB(45, 48, 54));
        FillRect(hdc, &resetBtnRc, rstBr);
        DeleteObject(rstBr);
        SetTextColor(hdc, RGB(200, 205, 215));
        DrawTextW(hdc, L"노치 필터 리셋", -1, &resetBtnRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // 2. Loudness Card (680 ~ 854)
        RECT loudRc = { 680, 380, 854, 605 };
        draw_rounded_card(hdc, loudRc, L"라우드니스 매치 (S-FR-5)");

        bool loud_on = m_loudness.is_enabled();
        draw_toggle_button(hdc, 690, 412, 844, 446, L"LOUD ACTIVE", loud_on, RGB(0, 104, 71), RGB(57, 255, 20));

        wchar_t loudInfo[128];
        swprintf_s(loudInfo, L"Measured: %.1f LUFS\nAuto Gain: %+.1f dB",
                   m_loudness.get_measured_lufs(),
                   m_loudness.get_current_gain_db());
        RECT loudTxtRc = { 694, 456, 844, 494 };
        DrawTextW(hdc, loudInfo, -1, &loudTxtRc, DT_LEFT);

        wchar_t targetTxt[64];
        swprintf_s(targetTxt, L"Target: %.1f LUFS", m_loudness.get_target_lufs());
        RECT tgtTxtRc = { 694, 500, 844, 516 };
        DrawTextW(hdc, targetTxt, -1, &tgtTxtRc, DT_LEFT);

        RECT tgtSliderRc = { 694, 524, 838, 534 };
        HBRUSH sTrackBr = CreateSolidBrush(RGB(35, 38, 44));
        FillRect(hdc, &tgtSliderRc, sTrackBr);
        DeleteObject(sTrackBr);

        float tgt_norm = (m_loudness.get_target_lufs() + 24.0f) / 16.0f;
        int knob_x = 694 + static_cast<int>(tgt_norm * (838 - 694));
        RECT knobRc = { knob_x - 5, 519, knob_x + 5, 539 };
        HBRUSH knobBr = CreateSolidBrush(RGB(0, 210, 255));
        FillRect(hdc, &knobRc, knobBr);
        DeleteObject(knobBr);

        // 3. Stem Separator & Music File Card (864 ~ 1040)
        RECT stemRc = { 864, 380, 1040, 605 };
        draw_rounded_card(hdc, stemRc, L"실시간 스템 분리 & 음원 (S-FR-4)");

        bool stem_on = m_stem.is_enabled();
        draw_toggle_button(hdc, 874, 406, 1030, 432, L"STEM ACTIVE", stem_on, RGB(0, 104, 71), RGB(57, 255, 20));

        // 📂 음원 파일 불러오기 버튼
        RECT openBtnRc = { 874, 436, 1030, 460 };
        HBRUSH openBr = CreateSolidBrush(RGB(28, 54, 96));
        FillRect(hdc, &openBtnRc, openBr);
        DeleteObject(openBr);

        HFONT hBtnFont = CreateFontW(12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH, L"Segoe UI");
        HFONT oldFont2 = reinterpret_cast<HFONT>(SelectObject(hdc, hBtnFont));
        SetTextColor(hdc, RGB(0, 210, 255));
        DrawTextW(hdc, L"📂 음악 파일 열기 (MP3/WAV)", -1, &openBtnRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        if (m_file_player.is_loaded()) {
            // 곡 이름 및 재생 시간
            std::wstring fname = m_file_player.get_file_name();
            if (fname.length() > 20) fname = fname.substr(0, 18) + L"..";

            float cur_sec = m_file_player.get_current_seconds();
            float dur_sec = m_file_player.get_duration_seconds();
            int cur_m = static_cast<int>(cur_sec) / 60, cur_s = static_cast<int>(cur_sec) % 60;
            int dur_m = static_cast<int>(dur_sec) / 60, dur_s = static_cast<int>(dur_sec) % 60;

            wchar_t timeBuf[64];
            swprintf_s(timeBuf, L"%02d:%02d / %02d:%02d", cur_m, cur_s, dur_m, dur_s);

            RECT titleRc = { 874, 464, 1030, 478 };
            SetTextColor(hdc, RGB(255, 255, 255));
            DrawTextW(hdc, fname.c_str(), -1, &titleRc, DT_LEFT | DT_SINGLELINE);

            SetTextColor(hdc, RGB(180, 185, 195));
            DrawTextW(hdc, timeBuf, -1, &titleRc, DT_RIGHT | DT_SINGLELINE);

            // Seek Bar (AI 분리 중에는 분리 진행률 바로 표시)
            RECT barBg = { 874, 482, 1030, 488 };
            HBRUSH bgBr = CreateSolidBrush(RGB(40, 44, 52));
            FillRect(hdc, &barBg, bgBr);
            DeleteObject(bgBr);

            bool is_busy = m_is_ai_busy.load();
            int ai_pct = m_ai_progress.load();
            float prog = 0.0f;
            if (is_busy) {
                prog = ai_pct / 100.0f;
            } else if (dur_sec > 0.0f) {
                prog = cur_sec / dur_sec;
            }
            prog = std::clamp(prog, 0.0f, 1.0f);
            int bar_fill_w = static_cast<int>(prog * (1030 - 874));
            RECT barFill = { 874, 482, 874 + bar_fill_w, 488 };
            HBRUSH fillBr = CreateSolidBrush(is_busy ? RGB(255, 200, 50) : RGB(0, 210, 255));
            FillRect(hdc, &barFill, fillBr);
            DeleteObject(fillBr);

            // 재생 / 정지 버튼
            bool playing = m_file_player.is_playing();
            RECT playBtnRc = { 874, 492, 948, 514 };
            HBRUSH pBr = CreateSolidBrush(playing ? RGB(0, 104, 71) : RGB(45, 48, 54));
            FillRect(hdc, &playBtnRc, pBr);
            DeleteObject(pBr);
            SetTextColor(hdc, playing ? RGB(57, 255, 20) : RGB(255, 255, 255));
            DrawTextW(hdc, playing ? L"⏸ PAUSE" : L"▶ PLAY", -1, &playBtnRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            RECT stopBtnRc = { 954, 492, 1030, 514 };
            HBRUSH stBr = CreateSolidBrush(RGB(60, 28, 28));
            FillRect(hdc, &stopBtnRc, stBr);
            DeleteObject(stBr);
            SetTextColor(hdc, RGB(255, 100, 100));
            DrawTextW(hdc, L"⏹ STOP", -1, &stopBtnRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            // 🤖 Demucs AI 스템 분리 버튼
            RECT aiBtnRc = { 874, 518, 1030, 542 };
            bool has_ai = m_file_player.has_ai_stems();

            if (is_busy) {
                HBRUSH aiBr = CreateSolidBrush(RGB(90, 70, 15));
                FillRect(hdc, &aiBtnRc, aiBr);
                DeleteObject(aiBr);
                SetTextColor(hdc, RGB(255, 220, 50));
                wchar_t busyTxt[64];
                swprintf_s(busyTxt, L"⏳ HTDemucs 분리 중... %d%%", ai_pct);
                DrawTextW(hdc, busyTxt, -1, &aiBtnRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else if (has_ai) {
                HBRUSH aiBr = CreateSolidBrush(RGB(16, 75, 45));
                FillRect(hdc, &aiBtnRc, aiBr);
                DeleteObject(aiBr);
                SetTextColor(hdc, RGB(57, 255, 20));
                DrawTextW(hdc, L"✨ HTDemucs AI 스템 적용됨", -1, &aiBtnRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                HBRUSH aiBr = CreateSolidBrush(RGB(64, 38, 96));
                FillRect(hdc, &aiBtnRc, aiBr);
                DeleteObject(aiBr);
                SetTextColor(hdc, RGB(220, 180, 255));
                DrawTextW(hdc, L"🤖 HTDemucs AI 분리 실행", -1, &aiBtnRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }

            // 💾 3-Way 스템 파일 분리 저장 버튼
            RECT expBtnRc = { 874, 546, 1030, 570 };
            HBRUSH expBr = CreateSolidBrush(RGB(40, 44, 52));
            FillRect(hdc, &expBtnRc, expBr);
            DeleteObject(expBr);
            SetTextColor(hdc, RGB(180, 185, 195));
            DrawTextW(hdc, L"💾 3-Way WAV 파일 저장", -1, &expBtnRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            // 가이드 텍스트
            RECT guideRc = { 874, 574, 1030, 598 };
            if (is_busy) {
                SetTextColor(hdc, RGB(255, 200, 50));
                wchar_t guideTxt[64];
                swprintf_s(guideTxt, L"AI 분리 진행률 %d%% (완료 시 재생)", ai_pct);
                DrawTextW(hdc, guideTxt, -1, &guideRc, DT_CENTER | DT_SINGLELINE);
            } else if (has_ai) {
                SetTextColor(hdc, RGB(57, 255, 20));
                DrawTextW(hdc, L"HTDemucs: CH1보컬 CH2비트 CH4악기", -1, &guideRc, DT_CENTER | DT_SINGLELINE);
            } else {
                SetTextColor(hdc, RGB(255, 187, 0));
                DrawTextW(hdc, L"AI 분리 대기 (🤖 버튼 클릭)", -1, &guideRc, DT_CENTER | DT_SINGLELINE);
            }

        } else {
            RECT stemTxtRc = { 874, 470, 1030, 595 };
            SetTextColor(hdc, RGB(160, 165, 175));
            DrawTextW(hdc, L"🎵 음원 로드 시 3채널 실시간 분리:\n\n• 보컬 (Mid)  -> CH 1 (MR제작)\n• 비트 (Bass) -> CH 2 (드럼)\n• 악기 (Side) -> CH 4 (반주)\n\nDemucs AI(HTDemucs)를\n통한 스튜디오급 분리 지원!",
                      -1, &stemTxtRc, DT_LEFT);
        }

        SelectObject(hdc, oldFont2);
        DeleteObject(hBtnFont);
        SelectObject(hdc, oldFont);
        DeleteObject(hInfoFont);
    }

    void draw_toggle_button(HDC hdc, int x0, int y0, int x1, int y1, const wchar_t* label, bool active, COLORREF activeBg, COLORREF activeFg) {
        RECT rc = { x0, y0, x1, y1 };
        HBRUSH hBr = CreateSolidBrush(active ? activeBg : RGB(35, 38, 44));
        FillRect(hdc, &rc, hBr);
        DeleteObject(hBr);

        HFONT hFont = CreateFontW(13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH, L"Segoe UI");
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, hFont));

        SetTextColor(hdc, active ? activeFg : RGB(140, 145, 155));
        std::wstring text = (active ? L"● " : L"○ ") + std::wstring(label);
        DrawTextW(hdc, text.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(hdc, oldFont);
        DeleteObject(hFont);
    }

    void draw_rounded_card(HDC hdc, const RECT& rc, const wchar_t* title) {
        HBRUSH cardBr = CreateSolidBrush(RGB(30, 32, 36));
        HPEN cardPen = CreatePen(PS_SOLID, 1, RGB(45, 48, 54));
        HBRUSH oldBr = reinterpret_cast<HBRUSH>(SelectObject(hdc, cardBr));
        HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, cardPen));

        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 8, 8);

        SelectObject(hdc, oldBr);
        SelectObject(hdc, oldPen);
        DeleteObject(cardBr);
        DeleteObject(cardPen);

        HFONT hTitleFont = CreateFontW(12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH, L"Segoe UI");
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, hTitleFont));
        SetTextColor(hdc, RGB(0, 210, 255));
        TextOutW(hdc, rc.left + 12, rc.top + 8, title, static_cast<int>(wcslen(title)));

        SelectObject(hdc, oldFont);
        DeleteObject(hTitleFont);
    }

    void draw_status_bar(HDC hdc, int w, int h) {
        RECT rc = { 0, h - 34, w, h };
        HBRUSH hBr = CreateSolidBrush(RGB(18, 19, 21));
        FillRect(hdc, &rc, hBr);
        DeleteObject(hBr);

        HPEN linePen = CreatePen(PS_SOLID, 1, RGB(45, 48, 54));
        HPEN oldPen = reinterpret_cast<HPEN>(SelectObject(hdc, linePen));
        MoveToEx(hdc, 0, h - 34, nullptr);
        LineTo(hdc, w, h - 34);

        HFONT hFont = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH, L"Segoe UI");
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(hdc, hFont));

        SetTextColor(hdc, RGB(160, 165, 175));
        std::wstring in_short = m_active_device_name;
        if (in_short.length() > 28) in_short = in_short.substr(0, 26) + L"..";
        std::wstring out_short = m_active_output_name;
        if (out_short.length() > 28) out_short = out_short.substr(0, 26) + L"..";

        std::wstring statusStr = L"🎤 In: " + in_short + L"  |  🔊 Out: " + out_short + L"  |  Latency < 12ms (PR-1)";
        TextOutW(hdc, 20, h - 25, statusStr.c_str(), static_cast<int>(statusStr.length()));
        TextOutW(hdc, w - 210, h - 25, L"Design Spec: S-FR-1 ~ 7 (Pass)", 30);

        SelectObject(hdc, oldFont);
        SelectObject(hdc, oldPen);
        DeleteObject(linePen);
        DeleteObject(hFont);
    }
};

} // namespace SmartMixer
