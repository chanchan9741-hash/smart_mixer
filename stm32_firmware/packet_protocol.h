/**
 * @file packet_protocol.h
 * @brief STM32H743VIT6 MCU <-> PC Host USB VCP 공용 통신 프로토콜 규격
 *
 * [STM32 -> PC] (ASCII Line Packets, 115200 bps / USB CDC VCP):
 *  - 5채널 슬라이드 페이더 ADC : "F:<CH0>,<CH1>,<CH2>,<CH3>,<CH4>\n" (0~1023)
 *  - 기능 버튼 클릭 이벤트     : "B:AFS\n", "B:STEM\n", "B:LOUD\n"
 *  - 로터리 엔코더 회전        : "E:+1\n", "E:-1\n"
 *  - 온보드 AFS 하울링 알림    : "[AFS_EVENT:<FREQ_HZ>]\n"
 *
 * [PC -> STM32] (ASCII Line Packets):
 *  - 기능 상태 LED/모드 동기화 : "[LED:<AFS>,<STEM>,<LOUD>]\n" (예: "[LED:1,1,0]\n")
 *  - 양방향 페이더 동기화      : "[FADER:<CH0>,<CH1>,<CH2>,<CH3>,<CH4>]\n"
 *  - 타겟 라우드니스 설정      : "[LUFS:<TARGET_LUFS>]\n" (예: "[LUFS:-14.0]\n")
 *  - AFS 노치 필터 리셋        : "[RESET_AFS]\n"
 *  - OLED 화면 외부 전송(옵션) : "[OLED:<PAGE_IDX>:<HEX_BYTES...>]\n"
 */

#ifndef PACKET_PROTOCOL_H
#define PACKET_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PACKET_MAX_LEN          128
#define ADC_CHANNELS_COUNT      5
#define EQ_BANDS_COUNT          3
#define OLED_WIDTH              128
#define OLED_HEIGHT             64
#define OLED_FRAMEBUFFER_BYTES  ((OLED_WIDTH * OLED_HEIGHT) / 8) /* 1024 bytes */

typedef struct {
    uint16_t fader_adc[ADC_CHANNELS_COUNT]; /* 0 ~ 1023 */
    int8_t   eq_db[EQ_BANDS_COUNT];         /* Low, Mid, High: -12 ~ +12 dB */
    uint8_t  btn_afs_pressed;
    uint8_t  btn_stem_pressed;
    uint8_t  btn_loud_pressed;
    int8_t   encoder_delta;
} MixerSensorData_t;

typedef struct {
    uint8_t led_afs_state;
    uint8_t led_stem_state;
    uint8_t led_loud_state;
    float   target_lufs;
} MixerFeedbackData_t;

#ifdef __cplusplus
}
#endif

#endif /* PACKET_PROTOCOL_H */
