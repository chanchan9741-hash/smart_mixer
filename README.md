# 스마트 오디오 믹서 (Smart Audio Mixer) - C++17 & HTDemucs 하이브리드 시스템

본 프로젝트는 **[스마트 오디오 믹서 설계서]**의 기능 및 성능 요구사항(S-FR-1 ~ S-FR-7, PR-1 ~ PR-3)을 충족하도록 **순수 C++(C++17) 실시간 DSP/GUI 코어**와 **Meta HTDemucs AI 딥러닝 스템 분리 파이프라인**을 결합해 개발한 하이브리드 오디오 믹싱 시스템입니다.

---

## 1. 프로젝트 아키텍처 (C++17 Core + Python HTDemucs)

```text
project/
├── 졸업작품설계서_양식.docx         # 졸업작품 원본 설계서 양식
├── README.md                       # 프로젝트 아키텍처 및 빌드/실행 가이드
│
├── cpp/                            # C++17 실시간 DSP 및 Win32 GUI 코어
│   ├── include/                    # 모듈별 헤더 파일
│   │   ├── config.hpp              # [PR-1, PR-2] 오디오 설정 (44.1kHz, 512샘플, 지연 < 20ms)
│   │   ├── ring_buffer.hpp         # [S-FR-1] Thread-safe 원형 링 버퍼 & 언더런/오버런 모니터
│   │   ├── biquad.hpp              # [S-FR-3] IIR Direct Form II Transposed 노치/EQ 필터
│   │   ├── afs.hpp                 # [S-FR-3] Radix-2 FFT 기반 자동 피드백(하울링) 감지 및 억제
│   │   ├── loudness.hpp            # [S-FR-5] ITU-R BS.1770 K-weighting LUFS, 안티-펌핑, 피크 리미터
│   │   ├── stem_separator.hpp      # [S-FR-4] 50% Overlap-Add(COLA) 실시간 STFT 직교 분리기
│   │   ├── mixer.hpp               # [S-FR-1, 7] 5채널(Master, Vocal, Beat, Mic 1, Line 2) 게인 램프 믹서
│   │   ├── debouncer.hpp           # [S-FR-7] 물리 버튼 25ms 채터링 필터 및 클릭/롱프레스 감지
│   │   ├── controller_mapper.hpp   # [S-FR-2] ADC 센서 노이즈 저감(EMA 필터) 및 파라미터 매니저
│   │   ├── serial_port.hpp         # [S-FR-2, 6] Win32 USB VCP 시리얼 통신 드라이버 (STM32 연동)
│   │   ├── ui_renderer.hpp         # [S-FR-6, PR-3] 60 FPS 실시간 스펙트럼 & 128x64 OLED 비트맵 버퍼
│   │   ├── wasapi_capture.hpp      # [WASAPI] 실시간 마이크 캡처 엔진 (지연 < 12ms)
│   │   ├── wasapi_render.hpp       # [WASAPI] 이벤트 기반(EVENTCALLBACK) 실시간 출력 렌더링 엔진
│   │   ├── audio_file_player.hpp   # [음원 & AI 스템] Media Foundation 디코더 & 3-Way 동기화 멀티트랙 재생기
│   │   ├── hardware_ipc.hpp        # [IPC] 메인 믹서 <-> 하드웨어 시뮬레이터 간 초저지연 UDP 루프백 통신
│   │   ├── hardware_audio_synth.hpp# [음원 합성] CH 2 비트(드럼) & CH 4 라인 2(코드 반주) 합성기
│   │   ├── hardware_sim_window.hpp # [하드웨어 UI] 물리 보드(OLED, 택트 버튼, 노브, 5채널 페이더) 시뮬레이터 UI
│   │   └── gui_window.hpp          # [메인 콘솔 UI] 5채널 믹서 콘솔, 입출력 장치 선택 및 60 FPS 스펙트럼 GUI
│   │
│   ├── src/
│   │   ├── gui_main.cpp            # [메인 믹서 GUI] 실시간 DSP, WASAPI 입출력 및 HTDemucs 오케스트레이션
│   │   ├── hardware_sim_main.cpp   # [하드웨어 시뮬레이터] 물리 보드 및 반주 음원 스트리밍 프로그램
│   │   └── main.cpp                # [CLI 파이프라인] 콘솔 데모
│   ├── tests/
│   │   └── test_main.cpp           # 8대 핵심 모듈 단위 테스트 슈트
│   ├── Makefile                    # make 빌드 스크립트
│   └── build.bat                   # 윈도우 원클릭 빌드 배치 파일
│
├── scripts/
│   └── demucs_runner.py            # [S-FR-4] PyTorch 기반 Meta HTDemucs 3-Way 스템 분리 러너
│
└── stm32_firmware/                 # STM32H743VIT6 실시간 임베디드 펌웨어 (Zero-Heap, 155.055 KB 정적 SRAM)
    ├── packet_protocol.h           # PC <-> STM32 공유 USB VCP 양방향 통신 프로토콜 (C 구조체 및 ASCII 라인 규격)
    ├── hw_hal.h                    # CubeMX DMA/GPIO/OLED/USB VCP HAL 추상화 콜백 인터페이스
    ├── embedded_dsp.hpp            # 동적 할당(Heap) 없는 정적 SRAM C++17 실시간 DSP 코어 (FFT/AFS/EQ/LUFS/STFT/OLED)
    ├── stm32_mixer_app.hpp         # MCU 온보드 상태 머신 (오디오 DMA, ADC 스캔, 디바운스/롱프레스, VCP 송수신)
    ├── main_mcu.cpp                # STM32 CubeMX C 링키지 진입점 (MCU_Mixer_Init, MCU_Audio_Process_I16 등)
    ├── test_firmware.cpp           # MCU 펌웨어 6대 검증 단위 테스트 (정적 메모리, 온보드 AFS, HIL 멀티트랙, VCP 동기화 등)
    └── Makefile                    # PC 단위 테스트(make test) 및 ARM Cortex-M7 크로스 컴파일(make arm) 빌드 스크립트
```

---

## 2. 하드웨어(STM32) vs PC 호스트 역할 분할 (SW/HW Partitioning)

| 구분 | 실행 주체 | 담당 기능 및 이유 |
|---|---|---|
| **하드웨어 상주 펌웨어**<br>(`stm32_firmware/` & `hardware_sim_gui.exe` 내장) | **STM32H743VIT6** (MCU)<br>(Cortex-M7, 1MB SRAM 중 **155.055 KB** 정적 사용) | • I2S 오디오 DMA 블록 입출력, 링 버퍼(`StaticRingBuffer`), 5채널 Gain-Ramp 믹서, 3-Band EQ<br>• 마이크 입력(CH3) 온보드 실시간 **AFS 하울링 억제** (1024-pt Radix-2 FFT + 최대 8노치 필터)<br>• **라우드니스 매치**(K-weighting LUFS + `-0.5 dBFS` 피크 리미터) & 스탠드얼론 실시간 **STFT 분리**(50% OLA COLA)<br>• 5채널 12-bit ADC EMA 필터(`alpha=0.3`), `25ms` 채터링 방지/`600ms` 롱프레스, 128×64 OLED 스펙트럼 렌더링 |
| **PC 호스트 소프트웨어**<br>(`cpp/` + `scripts/`) | **Windows PC**<br>(WASAPI + PyTorch) | • **Meta HTDemucs** (`htdemucs`) AI 4-Stem 딥러닝 분리 (`demucs_runner.py`, MCU 실행 불가 대용량 모델)<br>• MP3/WAV/FLAC/M4A Media Foundation 디코딩 및 3-Way AI 스템(`_ai_vocal/beat/inst.wav`) 동기 재생<br>• PC WASAPI 마이크/스피커 입출력 라우팅 및 60 FPS Win32 데스크톱 콘솔 UI<br>• `--com COMx` 지정 시 실제 STM32 보드와 USB VCP 양방향 실시간 동기화 (`Win32SerialPort`) |

---

## 3. 설계서 요구사항 구현 상세

| 요구사항 번호 | 요구사항 명칭 | 구현 파일 | 핵심 기술 상세 |
|---|---|---|---|
| **S-FR-1** | 버퍼 관리 및 5채널 오디오 믹싱 | [ring_buffer.hpp](file:///g:/내%20드라이브/project/cpp/include/ring_buffer.hpp)<br>[mixer.hpp](file:///g:/내%20드라이브/project/cpp/include/mixer.hpp)<br>[wasapi_capture.hpp](file:///g:/내%20드라이브/project/cpp/include/wasapi_capture.hpp)<br>[wasapi_render.hpp](file:///g:/내%20드라이브/project/cpp/include/wasapi_render.hpp) | • `std::mutex` 기반 스레드 세이프 원형 링 버퍼 및 언더런/오버런 실시간 감시<br>• WASAPI 이벤트 기반 출력 렌더링(`AUDCLNT_STREAMFLAGS_EVENTCALLBACK`) + `timeBeginPeriod(1)`<br>• 5채널(CH0 Master, CH1 Vocal, CH2 Beat, CH3 Mic In 1, CH4 Line In 2/Inst) 독립 제어 |
| **S-FR-2** | 센서 입력 맵핑 | [controller_mapper.hpp](file:///g:/내%20드라이브/project/cpp/include/controller_mapper.hpp)<br>[serial_port.hpp](file:///g:/내%20드라이브/project/cpp/include/serial_port.hpp)<br>[hardware_ipc.hpp](file:///g:/내%20드라이브/project/cpp/include/hardware_ipc.hpp) | • 아날로그 ADC 노이즈 제거용 `InputSmoothingFilter` (EMA 지수이동평균 필터, `alpha=0.3`)<br>• 리니어 페이더 Log 스케일 dB 게인(`-60dB ~ +6dB`) 맵핑<br>• Win32 USB VCP 시리얼 및 UDP 루프백 양방향 동기화 패킷 처리 |
| **S-FR-3** | 자동 피드백 억제 (AFS) | [afs.hpp](file:///g:/내%20드라이브/project/cpp/include/afs.hpp)<br>[biquad.hpp](file:///g:/내%20드라이브/project/cpp/include/biquad.hpp) | • Cooley-Tukey Radix-2 FFT(1024-point) 및 이동평균 기준선 대비 돌출 피크(12dB) 탐지<br>• 지속성(`persistence=3`) 검사로 타악기와 하울링 구분<br>• 마이크 입력단(CH 3)에 최대 8개의 협대역(`Q=12.0`, `-18dB`) 디지털 노치 필터 동적 할당 |
| **S-FR-4** | AI 스템 분리 & 실시간 직교 분리 | [demucs_runner.py](file:///g:/내%20드라이브/project/scripts/demucs_runner.py)<br>[audio_file_player.hpp](file:///g:/내%20드라이브/project/cpp/include/audio_file_player.hpp)<br>[stem_separator.hpp](file:///g:/내%20드라이브/project/cpp/include/stem_separator.hpp) | • MP3/WAV/FLAC/M4A 음원 열기 시 Media Foundation PCM 디코딩 → 백그라운드 **Meta HTDemucs**(`htdemucs`) AI 분리 실행 및 진행률(`0~100%`) 실시간 표시<br>• 분리된 3-Way 스템(`_ai_vocal.wav`, `_ai_beat.wav`, `_ai_inst.wav`)을 단일 재생 헤드로 완벽 동기화 재생(`read_frames_stems`) 및 캐시 자동 로드<br>• CH 1(Vocal), CH 2(Beat/Drums), CH 4(Inst/Bass+Other) 독립 믹싱 + CH 3 실시간 마이크 합산<br>• 실시간 스트림용 50% Overlap-Add(제곱근 Hann COLA) C++ STFT 분리기 및 3-Way WAV 저장 지원 |
| **S-FR-5** | 라우드니스 매치 | [loudness.hpp](file:///g:/내%20드라이브/project/cpp/include/loudness.hpp) | • ITU-R BS.1770 K-weighting 필터 기반 실시간 LUFS 산출<br>• 목표치(`-14.0 LUFS` 기본) 자동 게인 보정 및 EMA 스무딩(Pumping 방지)<br>• `-0.5 dBFS` 클리핑 방지 피크 리미터(Peak Limiter) |
| **S-FR-6** | UI 렌더링 코드 | [ui_renderer.hpp](file:///g:/내%20드라이브/project/cpp/include/ui_renderer.hpp)<br>[gui_window.hpp](file:///g:/내%20드라이브/project/cpp/include/gui_window.hpp) | • Win32 GDI 더블 버퍼링 60 FPS 실시간 16밴드 FFT 스펙트럼 & 5채널 VU 미터<br>• 입·출력 오디오 장치 선택 콤보박스, 하울링 감지 토스트 알림 및 HTDemucs 진행률 바<br>• 0.96인치 128×64 OLED SSD1306 프레임버퍼 바이트 생성(`generate_oled_buffer`) |
| **S-FR-7** | 작업 안정화 & 안티-팝 | [debouncer.hpp](file:///g:/내%20드라이브/project/cpp/include/debouncer.hpp)<br>[mixer.hpp](file:///g:/내%20드라이브/project/cpp/include/mixer.hpp) | • 스위치 채터링 방지 디바운서 (`25ms` 디바운스, `600ms` 롱프레스)<br>• 페이더 조작 시 팝 노이즈 차단을 위한 샘플별 선형 게인 램프(Gain Ramp) |

---

## 4. 빌드 및 실행 방법

### 1) 필수 환경 및 빌드
* **C++ 코어**: MinGW GCC (`g++` C++17)만으로 외부 C++ 라이브러리 없이 즉시 컴파일됩니다.
* **AI 스템 분리(HTDemucs)**: Python 환경에 `torch`, `demucs`, `numpy`가 설치되어 있으면 `smart_mixer_gui.exe`가 자동으로 실행 경로를 탐색해 연동합니다 (`ffmpeg` 불필요).

```cmd
cd cpp
build.bat
```
*(메인 GUI `smart_mixer_gui.exe`, 하드웨어 시뮬레이터 `hardware_sim_gui.exe`, CLI 파이프라인 `smart_mixer.exe`, 테스트 슈트 `test_suite.exe` 일괄 빌드)*

### 2) 실행 모드 선택: 실제 STM32 보드 연동 또는 하드웨어 시뮬레이터

#### A. 실제 STM32H743 하드웨어 보드(USB VCP)와 직접 연동
STM32 보드에 `stm32_firmware/`를 플래싱한 뒤, 장치 관리자에서 확인된 COM 포트를 `--com` 인자로 전달하면 PC 호스트 GUI가 즉시 USB VCP 시리얼(`115200 bps`)로 연결됩니다 (`--com` 생략 시 대기 지연 없이 시뮬레이터/단독 모드로 실행):

```powershell
cd cpp
.\smart_mixer_gui.exe --com COM3
```

#### B. STM32 하드웨어 시뮬레이터 HIL 연동 (2개 창 동시 실행)
실제 보드 없이도 `hardware_sim_gui.exe` 내부에 **실제 STM32 온보드 펌웨어(`Stm32MixerFirmware`)가 탑재**되어 DSP·AFS·3-Band EQ·라우드니스·OLED를 직접 연산하고, `smart_mixer_gui.exe`가 WASAPI 입출력 및 HTDemucs AI 스템을 공급해 하드웨어-인-더-루프(HIL)로 동작합니다:

```powershell
cd cpp

# 창 1: 물리 STM32 보드 시뮬레이터 (내부 STM32 펌웨어 DSP + 128x64 OLED + 물리 페이더/노브/버튼)
start .\hardware_sim_gui.exe

# 창 2: PC 호스트 콘솔 (WASAPI 마이크/스피커 라우팅 + Meta HTDemucs AI 스템 공급)
start .\smart_mixer_gui.exe
```

* **하드웨어 시뮬레이터 물리 조작**:
  * **5채널 페이더 / EQ 3-Band 노브(LOW/MID/HIGH)**: 마우스 드래그로 실시간 게인·EQ 조절 (EQ 노브 우클릭 시 `0.0 dB` 리셋)
  * **택트 버튼(`[AFS]`, `[STEM]`, `[LOUD]`)**: 클릭 토글 (`25ms` 채터링 필터 동작), `[AFS]` **600ms 롱프레스** 시 하울링 노치 필터 전체 리셋
  * **EC11 로터리 엔코더**: 좌클릭/휠 업(`+0.5 LUFS`), 우클릭/휠 다운(`-0.5 LUFS`)으로 목표 음압 조절
* **다채널 오디오 라우팅 및 양방향 동기화**:
  * **음원 파일 재생 모드 (HTDemucs)**: PC에서 분리된 AI 스템(`Vocal/Beat/Inst`)과 마이크를 시뮬레이터로 전송 → STM32 DSP 처리 후 PC 스피커로 출력
  * **실시간 반주 연동 모드**: 시뮬레이터 내장 드럼(`CH 2`)·코드(`CH 4`) 합성음과 PC 마이크(`CH 3`)를 시뮬레이터 내부 STM32 DSP가 믹싱·AFS·EQ·LUFS 처리

### 3) 콘솔 파이프라인 데모 실행
```powershell
cd cpp
.\smart_mixer.exe --duration 5.0
```

### 4) 단위 테스트 (PC 호스트 8종 + STM32 MCU 펌웨어 6종)
```powershell
# 1. PC 호스트 C++ 단위 테스트 (8대 핵심 테스트)
cd cpp
.\test_suite.exe

# 2. STM32 임베디드 펌웨어 단위 테스트 (정적 SRAM/Zero-Heap, 온보드 AFS, HIL 멀티트랙 등 6종)
cd ..\stm32_firmware
g++ -std=c++17 -O3 -Wall -Wextra -I. test_firmware.cpp main_mcu.cpp -o test_mcu_firmware.exe
.\test_mcu_firmware.exe

# (선택) ARM Cortex-M7 타겟 릴리즈 오브젝트 크로스 컴파일
make arm
```
