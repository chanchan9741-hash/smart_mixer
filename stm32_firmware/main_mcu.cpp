/**
 * @file main_mcu.cpp
 * @brief STM32CubeIDE main.c 및 HAL 콜백 연동용 C-Linkage 어댑터
 *
 * STM32CubeIDE 프로젝트(main.c / usbd_cdc_if.c)에서의 연결 예시:
 *   1. 초기화: MCU_Mixer_Init(&hw_callbacks);
 *   2. I2S DMA 완료 콜백 (HAL_I2SEx_TxRxCpltCallback):
 *      MCU_Audio_Process_I16(mic_rx, usb_rx, line_rx, dac_tx, usb_tx, 512);
 *   3. ADC DMA 완료 콜백 (HAL_ADC_ConvCpltCallback):
 *      MCU_ADC_Update(adc_faders_5, adc_eq_3, 1023);
 *   4. 메인 루프 (10ms 버튼 스캔 / 33ms OLED 갱신):
 *      MCU_Buttons_Update(btn_afs, btn_stem, btn_loud, enc_delta);
 *      MCU_UI_Tick();
 *   5. USB VCP 수신 콜백 (CDC_Receive_FS):
 *      MCU_VCP_Rx(Buf, *Len);
 */

#include "stm32_mixer_app.hpp"

// STM32H743 AXI SRAM(512KB)에 정적 배치되는 단일 펌웨어 인스턴스 (힙 할당 0 bytes)
static SmartMixerMCU::Stm32MixerFirmware g_stm32_mixer;

extern "C" {

void MCU_Mixer_Init(const HwCallbacks_t* callbacks) {
    if (callbacks) {
        g_stm32_mixer.init(*callbacks);
    }
}

void MCU_Audio_Process_I16(const int16_t* mic_in,
                           const int16_t* usb_in,
                           const int16_t* line_in,
                           int16_t* dac_out,
                           int16_t* usb_out,
                           uint16_t num_frames) {
    g_stm32_mixer.on_audio_dma_block(mic_in, usb_in, line_in, dac_out, usb_out, num_frames);
}

void MCU_ADC_Update(const uint16_t fader_adc[ADC_CHANNELS_COUNT],
                    const uint16_t eq_adc[EQ_BANDS_COUNT],
                    uint16_t max_adc) {
    g_stm32_mixer.on_adc_dma_scan(fader_adc, eq_adc, max_adc);
}

void MCU_Buttons_Update(uint8_t afs_pressed,
                        uint8_t stem_pressed,
                        uint8_t loud_pressed,
                        int8_t encoder_delta) {
    g_stm32_mixer.on_gpio_buttons(afs_pressed != 0, stem_pressed != 0, loud_pressed != 0, encoder_delta);
}

void MCU_VCP_Rx(const uint8_t* data, uint16_t len) {
    g_stm32_mixer.on_vcp_rx_bytes(data, len);
}

void MCU_UI_Tick(void) {
    g_stm32_mixer.on_ui_tick();
}

uint32_t MCU_Get_Static_Ram_Bytes(void) {
    return static_cast<uint32_t>(sizeof(SmartMixerMCU::Stm32MixerFirmware));
}

} // extern "C"

