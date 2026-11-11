/**
 * @file packet_protocol.h
 * @brief STM32 MCU <-> PC USB VCP 데이터 통신 프로토콜 정의
 * 
 * [STM32 -> PC]:
 *  - 5채널 슬라이드 페이더 ADC: "F:<CH0>,<CH1>,<CH2>,<CH3>,<CH4>\n" (예: "F:1023,800,750,900,850\n")
 *  - 기능 버튼 클릭:           "B:AFS\n", "B:STEM\n", "B:LOUD\n"
 *  - 로터리 엔코더 회전:        "E:+1\n", "E:-1\n"
 * 
 * [PC -> STM32]:
 *  - 기능 상태 LED 점등:        "[LED:<AFS>,<STEM>,<LOUD>]\n" (예: "[LED:1,1,0]\n")
 *  - OLED 화면 업데이트:        "[OLED:<PAGE_IDX>:<HEX_BYTES...>]\n"
 */

#ifndef PACKET_PROTOCOL_H
#define PACKET_PROTOCOL_H

#include <stdint.h>

#define PACKET_MAX_LEN          128
#define ADC_CHANNELS_COUNT      5

typedef struct {
    uint16_t fader_adc[ADC_CHANNELS_COUNT];
    uint8_t  btn_afs_pressed;
    uint8_t  btn_stem_pressed;
    uint8_t  btn_loud_pressed;
    int8_t   encoder_delta;
} MixerSensorData_t;

typedef struct {
    uint8_t led_afs_state;
    uint8_t led_stem_state;
    uint8_t led_loud_state;
} MixerFeedbackData_t;

#endif // PACKET_PROTOCOL_H

