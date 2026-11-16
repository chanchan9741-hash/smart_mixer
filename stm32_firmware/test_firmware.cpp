#include <iostream>
#include <cassert>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "stm32_mixer_app.hpp"

using namespace SmartMixerMCU;

// Mock STM32 HAL 상태
static uint32_t g_mock_tick_ms = 0;
static std::string g_mock_vcp_tx;
static uint8_t g_mock_leds[3] = {0, 0, 0};
static uint8_t g_mock_oled[OLED_FRAMEBUFFER_BYTES] = {0};
static uint32_t g_oled_flush_count = 0;

static void mock_tx_vcp(const char* data, uint16_t len) {
    g_mock_vcp_tx.append(data, len);
}

static void mock_set_leds(uint8_t afs, uint8_t stem, uint8_t loud) {
    g_mock_leds[0] = afs;
    g_mock_leds[1] = stem;
    g_mock_leds[2] = loud;
}

static void mock_oled_flush(const uint8_t* fb) {
    std::memcpy(g_mock_oled, fb, OLED_FRAMEBUFFER_BYTES);
    g_oled_flush_count++;
}

static uint32_t mock_get_tick_ms() {
    return g_mock_tick_ms;
}

static HwCallbacks_t make_mock_cb() {
    HwCallbacks_t cb;
    cb.tx_vcp = mock_tx_vcp;
    cb.set_leds = mock_set_leds;
    cb.oled_flush = mock_oled_flush;
    cb.get_tick_ms = mock_get_tick_ms;
    return cb;
}

void test_static_sram_footprint() {
    std::cout << "[MCU Test 1] Static SRAM Footprint (STM32H743 1MB Limit)...";
    size_t bytes = sizeof(Stm32MixerFirmware);
    // AXI SRAM(512KB) 내에 여유롭게 들어가야 함 (목표 < 200KB)
    assert(bytes < 200 * 1024);
    std::cout << " PASSED (" << (bytes / 1024.0) << " KB static, 0 heap)\n";
}

void test_adc_smoothing_and_vcp_tx() {
    std::cout << "[MCU Test 2] ADC Smoothing & VCP Packet Tx...";
    static Stm32MixerFirmware fw;
    g_mock_vcp_tx.clear();
    fw.init(make_mock_cb());

    uint16_t faders[5] = { 1023, 512, 256, 768, 0 };
    uint16_t eq[3] = { 512, 512, 512 };
    for (int i = 0; i < 15; ++i) {
        fw.on_adc_dma_scan(faders, eq, 1023);
    }
    assert(g_mock_vcp_tx.find("F:") != std::string::npos);
    assert(fw.get_mixer().get_gain_linear(0) > 0.0f);
    std::cout << " PASSED\n";
}

void test_button_debouncer_and_encoder() {
    std::cout << "[MCU Test 3] 25ms GPIO Debouncer & EC11 Encoder...";
    static Stm32MixerFirmware fw;
    g_mock_vcp_tx.clear();
    g_mock_tick_ms = 0;
    fw.init(make_mock_cb());
    assert(fw.get_afs().is_enabled() == true);

    // 10ms 채터링 노이즈 (무시되어야 함)
    fw.on_gpio_buttons(true, false, false, 0);
    g_mock_tick_ms = 10;
    fw.on_gpio_buttons(false, false, false, 0);
    assert(fw.get_afs().is_enabled() == true);

    // 정상 누름 (40ms 유지 후 뗌 -> AFS OFF 토글 + B:AFS 송신)
    g_mock_tick_ms = 50;
    fw.on_gpio_buttons(true, false, false, 0);
    g_mock_tick_ms = 90;
    fw.on_gpio_buttons(true, false, false, 0);
    g_mock_tick_ms = 110;
    fw.on_gpio_buttons(false, false, false, 0);
    g_mock_tick_ms = 140;
    fw.on_gpio_buttons(false, false, false, 0);

    assert(fw.get_afs().is_enabled() == false);
    assert(g_mock_leds[0] == 0);
    assert(g_mock_vcp_tx.find("B:AFS\n") != std::string::npos);

    // 엔코더 회전 -> 타겟 LUFS 변경
    float before_lufs = fw.get_loudness().get_target_lufs();
    fw.on_gpio_buttons(false, false, false, -2);
    assert(fw.get_loudness().get_target_lufs() < before_lufs);
    assert(g_mock_vcp_tx.find("E:-1\n") != std::string::npos);
    std::cout << " PASSED\n";
}

void test_onboard_afs_howling_and_oled() {
    std::cout << "[MCU Test 4] Onboard AFS 2.5kHz Suppression + OLED Spectrum...";
    static Stm32MixerFirmware fw;
    g_mock_vcp_tx.clear();
    g_mock_oled[0] = 0;
    g_oled_flush_count = 0;
    g_mock_tick_ms = 100;
    fw.init(make_mock_cb());

    std::vector<int16_t> mic_in(512 * 2);
    std::vector<int16_t> dac_out(512 * 2);
    std::vector<int16_t> usb_out(512 * 2);

    constexpr float PI = 3.14159265358979323846f;
    // 10블록 (약 116ms) 2.5kHz 강한 피드백 신호 인가
    for (int blk = 0; blk < 10; ++blk) {
        for (size_t i = 0; i < 512; ++i) {
            size_t sample_idx = blk * 512 + i;
            float s = 0.7f * std::sin(2.0f * PI * 2500.0f * sample_idx / 44100.0f);
            int16_t pcm = static_cast<int16_t>(s * 32767.0f);
            mic_in[i * 2] = pcm;
            mic_in[i * 2 + 1] = pcm;
        }
        g_mock_tick_ms += 12;
        fw.on_audio_dma_block(mic_in.data(), nullptr, nullptr, dac_out.data(), usb_out.data(), 512);
    }

    // AFS가 2.5kHz 대역 노치 필터를 자동 배치하고 PC에 이벤트 송신했는지 검증
    assert(fw.get_afs().get_active_filter_count() >= 1);
    assert(std::fabs(fw.get_afs().get_last_suppressed_freq() - 2500.0f) < 100.0f);
    assert(g_mock_vcp_tx.find("[AFS_EVENT:") != std::string::npos);

    // OLED 스펙트럼 렌더링 검증
    fw.on_ui_tick();
    assert(g_oled_flush_count == 1);
    uint32_t non_zero = 0;
    for (size_t i = 0; i < OLED_FRAMEBUFFER_BYTES; ++i) {
        if (g_mock_oled[i] != 0) non_zero++;
    }
    assert(non_zero > 10);
    std::cout << " PASSED (Detected " << fw.get_afs().get_last_suppressed_freq()
              << "Hz, OLED bytes=" << non_zero << ")\n";
}

void test_host_vcp_rx_sync() {
    std::cout << "[MCU Test 5] PC Host -> STM32 Bidirectional VCP Sync...";
    static Stm32MixerFirmware fw;
    fw.init(make_mock_cb());

    const char* cmds = "[LED:1,0,1]\n[LUFS:-16.5]\n[FADER:1023,512,256,128,0]\n";
    fw.on_vcp_rx_bytes(reinterpret_cast<const uint8_t*>(cmds), static_cast<uint16_t>(std::strlen(cmds)));

    assert(fw.get_afs().is_enabled() == true);
    assert(fw.is_stem_enabled() == false);
    assert(fw.get_loudness().is_enabled() == true);
    assert(std::fabs(fw.get_loudness().get_target_lufs() - (-16.5f)) < 0.01f);
    std::cout << " PASSED\n";
}

void test_multitrack_hil_simulator_block() {
    std::cout << "[MCU Test 6] HIL Multitrack Block (Mic+Beat+Line2 + HTDemucs + VU)...";
    static Stm32MixerFirmware fw;
    g_mock_vcp_tx.clear();
    g_mock_tick_ms = 200;
    fw.init(make_mock_cb());

    std::vector<int16_t> mic(512 * 2, 0);
    std::vector<int16_t> vocal(512 * 2, 0);
    std::vector<int16_t> beat(512 * 2, 0);
    std::vector<int16_t> inst(512 * 2, 0);
    std::vector<int16_t> dac_out(512 * 2, 0);

    constexpr float PI = 3.14159265358979323846f;
    for (int blk = 0; blk < 10; ++blk) {
        for (size_t i = 0; i < 512; ++i) {
            size_t sidx = blk * 512 + i;
            float m = 0.65f * std::sin(2.0f * PI * 2500.0f * sidx / 44100.0f);
            float v = 0.25f * std::sin(2.0f * PI * 440.0f * sidx / 44100.0f);
            float b = 0.20f * std::sin(2.0f * PI * 100.0f * sidx / 44100.0f);
            float n = 0.15f * std::sin(2.0f * PI * 330.0f * sidx / 44100.0f);
            mic[i * 2] = mic[i * 2 + 1] = static_cast<int16_t>(m * 32767.0f);
            vocal[i * 2] = vocal[i * 2 + 1] = static_cast<int16_t>(v * 32767.0f);
            beat[i * 2] = beat[i * 2 + 1] = static_cast<int16_t>(b * 32767.0f);
            inst[i * 2] = inst[i * 2 + 1] = static_cast<int16_t>(n * 32767.0f);
        }
        g_mock_tick_ms += 12;
        fw.process_multitrack_audio_block(mic.data(), vocal.data(), beat.data(), inst.data(), dac_out.data(), 512, true);
    }

    assert(fw.get_afs().get_active_filter_count() >= 1);
    assert(fw.get_vu_level(1) > 0.05f);
    assert(fw.get_vu_level(2) > 0.05f);
    assert(fw.get_vu_level(4) > 0.03f);
    fw.on_ui_tick();
    assert(fw.is_howling_alert() == true);
    std::cout << " PASSED\n";
}

int main() {
    std::cout << "\n=== Running STM32 Hardware Firmware Verification ===\n";
    test_static_sram_footprint();
    test_adc_smoothing_and_vcp_tx();
    test_button_debouncer_and_encoder();
    test_onboard_afs_howling_and_oled();
    test_host_vcp_rx_sync();
    test_multitrack_hil_simulator_block();
    std::cout << "=== All 6 MCU Firmware Tests Passed Successfully! ===\n\n";
    return 0;
}

