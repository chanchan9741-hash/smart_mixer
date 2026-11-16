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
└── stm32_firmware/                 # STM32H743VIT6 하드웨어 펌웨어 통신 규격
    └── packet_protocol.h           # PC <-> STM32 공유 USB VCP 통신 패킷 규격
```

---

## 2. 설계서 요구사항 구현 상세

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

## 3. 빌드 및 실행 방법

### 1) 필수 환경 및 빌드
* **C++ 코어**: MinGW GCC (`g++` C++17)만으로 외부 C++ 라이브러리 없이 즉시 컴파일됩니다.
* **AI 스템 분리(HTDemucs)**: Python 환경에 `torch`, `demucs`, `numpy`가 설치되어 있으면 `smart_mixer_gui.exe`가 자동으로 실행 경로를 탐색해 연동합니다 (`ffmpeg` 불필요).

```cmd
cd cpp
build.bat
```
*(메인 GUI `smart_mixer_gui.exe`, 하드웨어 시뮬레이터 `hardware_sim_gui.exe`, CLI 파이프라인 `smart_mixer.exe`, 테스트 슈트 `test_suite.exe` 일괄 빌드)*

### 2) 하드웨어 연동 시뮬레이션 실행 (2개 창 동시 실행)

두 개의 프로그램을 함께 실행하면 실제 STM32 하드웨어 믹서 보드가 PC에 연결된 것처럼 상호작용합니다:

```powershell
cd cpp

# 창 1: 메인 믹서 애플리케이션 실행 (마이크 수신, 음악 파일 HTDemucs 분리, WASAPI 출력)
start .\smart_mixer_gui.exe

# 창 2: 물리 하드웨어 시뮬레이터 실행 (비트/악기 음원 스트리밍 및 물리 페이더 조작)
start .\hardware_sim_gui.exe
```

* **다채널 오디오 라우팅 구조**:
  * **음원 파일 재생 모드 (HTDemucs)**: `CH 0` Master / `CH 1` AI Vocal (내리면 MR 반주) / `CH 2` AI Beat (드럼) / `CH 3` Live Mic In 1 (AFS 보호) / `CH 4` AI Inst (베이스·반주)
  * **하드웨어 시뮬레이터 연동 모드**: `CH 2` 드럼 합성 음원 및 `CH 4` 코드 반주 음원을 UDP(`0.05ms`)로 스트리밍하고, PC WASAPI 마이크로 `CH 3` 보컬을 실시간 합산
* **하드웨어 <-> 소프트웨어 양방향 동기화 (모터 페이더 에뮬레이션)**:
  * 양쪽 창의 5개 페이더 및 `[AFS]`, `[STEM]`, `[LOUD]` 버튼·LED·128×64 OLED 디스플레이가 실시간으로 상호 동기화됩니다.
  * 드래그 중인 채널은 우선 보호되어 양방향 동시 조작 시에도 충돌이나 떨림이 발생하지 않습니다.

### 3) 콘솔 파이프라인 데모 실행
```powershell
cd cpp
.\smart_mixer.exe --duration 5.0
```

### 4) 단위 테스트 (Unit Tests - 8대 핵심 테스트)
```powershell
cd cpp
.\test_suite.exe
```
