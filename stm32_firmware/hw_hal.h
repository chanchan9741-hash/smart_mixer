/**
 * @file hw_hal.h
 * @brief STM32 CubeMX HAL 추상화 인터페이스 (MCU / PC 테스트 공용)
 */

#ifndef STM32_HW_HAL_H
#define STM32_HW_HAL_H

#include <stdint.h>
#include "packet_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** USB VCP(CDC) 또는 UART를 통해 PC 호스트로 패킷 문자열 전송 */
    void (*tx_vcp)(const char* data, uint16_t len);

    /** 기능 버튼 옆 5mm 고휘도 LED(AFS, STEM, LOUD) GPIO 출력 제어 */
    void (*set_leds)(uint8_t afs_on, uint8_t stem_on, uint8_t loud_on);

    /** 0.96인치 128x64 SSD1306 OLED(1024바이트) SPI/I2C DMA 플러시 */
    void (*oled_flush)(const uint8_t* framebuffer_1024);

    /** 시스템 밀리초 타이머 (HAL_GetTick() 연결) */
    uint32_t (*get_tick_ms)(void);
} HwCallbacks_t;

#ifdef __cplusplus
}
#endif

#endif /* STM32_HW_HAL_H */

