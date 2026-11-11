# 스마트 오디오 믹서 (Smart Audio Mixer) - C++ 시스템

본 프로젝트는 **[스마트 오디오 믹서 설계서]**의 모든 기능 및 성능 요구사항(S-FR-1 ~ S-FR-7, PR-1 ~ PR-3)을 충족하도록 **순수 C++(C++17 표준)**로 개발된 고성능 실시간 디지털 오디오 신호 처리(DSP) 및 하이브리드 제어 시스템입니다.

---

## 1. 프로젝트 아키텍처 (100% Pure C++)

```
project/
├── 졸업작품설계서_양식.docx        # 졸업작품 원본 설계서 문서
├── README.md                      # 프로젝트 아키텍처 및 빌드 가이드
│
├── cpp/                           # C++ 코어 소프트웨어
│   ├── include/                   # 모듈별 헤더 파일 (Header-only / Zero-dependency)
│   │   ├── config.hpp             # [PR-1, PR-2] 오디오 설정 (44.1kHz, 512샘플, 지연 < 20ms)
│   │   ├── ring_buffer.hpp        # [S-FR-1] Thread-safe 원형 링 버퍼 & 언더런/오버런 모니터
│   │   ├── biquad.hpp             # [S-FR-3] IIR Direct Form II Transposed 노치/EQ 필터
│   │   ├── afs.hpp                # [S-FR-3] Radix-2 FFT 기반 자동 피드백(하울링) 감지 및 억제
│   │   ├── loudness.hpp           # [S-FR-5] ITU-R BS.1770 K-weighting LUFS, 안티-펌핑, 피크 리미터
│   │   ├── stem_separator.hpp     # [S-FR-4] Mid-Side 및 스펙트럼 마스킹 기반 보컬/비트 분리
│   │   ├── mixer.hpp              # [S-FR-1, 7] 5채널(Master, Vocal, Beat, Mic 1, Line 2) 게인 램프 믹서
│   │   ├── debouncer.hpp          # [S-FR-7] 물리 버튼 25ms 채터링 필터 및 클릭/롱프레스 감지
│   │   ├── controller_mapper.hpp  # [S-FR-2] ADC 센서 노이즈 저감(EMA 필터) 및 파라미터 매니저
│   │   ├── serial_port.hpp        # [S-FR-2, 6] Win32 USB VCP 시리얼 통신 드라이버 (STM32 연동)
│   │   ├── ui_renderer.hpp        # [S-FR-6, PR-3] 60+ FPS 실시간 스펙트럼 & 128x64 OLED 비트맵 버퍼
│   │   ├── wasapi_capture.hpp     # [WASAPI] 실시간 마이크 캡처 엔진 (지연 < 12ms)
│   │   ├── audio_file_player.hpp  # [음원 파일 & 스템 추출] MP3/WAV/FLAC 로더 및 3-Way WAV 일괄 내보내기
│   │   ├── hardware_ipc.hpp       # [IPC] 메인 믹서 <-> 하드웨어 시뮬레이터 간 초저지연 UDP 루프백 통신
│   │   ├── hardware_audio_synth.hpp # [음원 합성] CH 2 비트(드럼) & CH 4 라인 2(코드 반주) 합성기
│   │   ├── hardware_sim_window.hpp  # [하드웨어 UI] 물리 보드(OLED, 택트 버튼, 노브, 5채널 페이더) 시뮬레이터 UI
│   │   └── gui_window.hpp         # [메인 콘솔 UI] 5채널 믹서 콘솔 및 60 FPS 스펙트럼 GUI
│   │
│   ├── src/
│   │   ├── gui_main.cpp           # [메인 믹서 GUI] 스마트 오디오 믹서 호스트 프로그램
│   │   ├── hardware_sim_main.cpp  # [하드웨어 시뮬레이터] 물리 보드 및 반주 음원 스트리밍 프로그램
│   │   └── main.cpp               # [CLI 파이프라인] 콘솔 데모
│   ├── tests/
│   │   └── test_main.cpp          # 6대 핵심 모듈 단위 테스트 슈트
│   ├── Makefile                   # make 빌드 스크립트
│   └── build.bat                  # 윈도우 원클릭 빌드 배치 파일
│
└── stm32_firmware/                # STM32H743VIT6 하드웨어 펌웨어
    └── packet_protocol.h          # PC <-> STM32 공유 USB VCP 통신 패킷 규격
```

---

## 2. 설계서 요구사항 구현 상세 (C++17)

| 요구사항 번호 | 요구사항 명칭 | C++ 구현 파일 | 핵심 기술 상세 |
|---|---|---|---|
| **S-FR-1** | 버퍼 관리 및 5채널 오디오 믹싱 | [ring_buffer.hpp](file:///g:/내 드라이브/project/cpp/include/ring_buffer.hpp)<br>[mixer.hpp](file:///g:/내 드라이브/project/cpp/include/mixer.hpp) | • `std::mutex` 기반 스레드 세이프 원형 링 버퍼<br>• 오버런/언더런 실시간 감시<br>• 5채널 독립 게인 제어 및 멀티채널 합산 |
| **S-FR-2** | 센서 입력 맵핑 | [controller_mapper.hpp](file:///g:/내 드라이브/project/cpp/include/controller_mapper.hpp)<br>[serial_port.hpp](file:///g:/내 드라이브/project/cpp/include/serial_port.hpp) | • 아날로그 ADC 노이즈 제거용 EMA 지수이동평균 필터<br>• 리니어 페이더 Log 스케일 dB 게인 맵핑<br>• Win32 시리얼 VCP 패킷 파서 |
| **S-FR-3** | 자동 피드백 억제 (AFS) | [afs.hpp](file:///g:/내 드라이브/project/cpp/include/afs.hpp)<br>[biquad.hpp](file:///g:/내 드라이브/project/cpp/include/biquad.hpp) | • Cooley-Tukey Radix-2 FFT(1024-point) 내장<br>• 지속성(Persistence) 검사로 타악기와 하울링 구분<br>• 협대역(Q=12.0) 디지털 노치 필터 동적 자동 할당 (-18dB 감쇄) |
| **S-FR-4** | 실시간 스템 분리 & 음원 분리 추출 | [stem_separator.hpp](file:///g:/내 드라이브/project/cpp/include/stem_separator.hpp)<br>[audio_file_player.hpp](file:///g:/내 드라이브/project/cpp/include/audio_file_player.hpp) | • MP3, WAV, FLAC, M4A 등 임의 음원 파일 로드 및 재생<br>• Mid/Side 스테레오 디코딩 및 STFT 1024-point 주파수 마스킹<br>• 3-Way 실시간 분리: 보컬(CH 1), 비트/드럼(CH 2), 악기/반주(CH 4)<br>• 페이더로 보컬을 줄여 즉시 노래방(MR) 반주 제작<br>• 원클릭 3개 WAV 파일(`_vocal`, `_beat`, `_inst`) 초고속 분리 저장 |
| **S-FR-5** | 라우드니스 매치 | [loudness.hpp](file:///g:/내 드라이브/project/cpp/include/loudness.hpp) | • ITU-R BS.1770 K-weighting 필터 기반 실시간 LUFS 산출<br>• 목표치(-14.0 LUFS) 부드러운 자동 보정 게인 산출 (Pumping 방지)<br>• 클리핑 방지 피크 리미터(Peak Limiter) |
| **S-FR-6** | UI 렌더링 코드 | [ui_renderer.hpp](file:///g:/내 드라이브/project/cpp/include/ui_renderer.hpp) | • 16밴드 실시간 오디오 스펙트럼 그래픽 도식화 (60+ FPS)<br>• 하울링 감지 이벤트 토스트 알림 팝업<br>• 0.96인치 128x64 OLED SSD1306 SPI/I2C 프레임버퍼 바이트 생성 |
| **S-FR-7** | 작업 안정화 & 안티-팝 | [debouncer.hpp](file:///g:/내 드라이브/project/cpp/include/debouncer.hpp)<br>[mixer.hpp](file:///g:/내 드라이브/project/cpp/include/mixer.hpp) | • 스위치 채터링 방지 디바운서 (25ms 필터)<br>• 페이더 조작 시 팝 노이즈 차단을 위한 샘플별 선형 게인 램프(Gain Ramp) |

---

## 3. 빌드 및 실행 방법

외부 패키지 설치 없이 **MinGW GCC (`g++`)** 하나만으로 즉시 컴파일됩니다.

### 1) 윈도우 원클릭 빌드 (All Targets)
```cmd
cd cpp
build.bat
```
*(메인 GUI `smart_mixer_gui.exe`, 하드웨어 시뮬레이터 `hardware_sim_gui.exe`, CLI 파이프라인 `smart_mixer.exe`, 테스트 슈트 `test_suite.exe` 일괄 빌드)*

### 2) 하드웨어 연동 시뮬레이션 실행 (추천: 2개 창 동시 실행)

두 개의 프로그램을 함께 실행하면 실제 하드웨어 믹서 보드가 PC에 연결된 것처럼 상호작용합니다:

```powershell
cd cpp

# 창 1: 메인 믹서 애플리케이션 실행 (보컬 마이크 수신 및 DSP 처리)
start .\smart_mixer_gui.exe

# 창 2: 물리 하드웨어 시뮬레이터 실행 (비트/악기 음원 스트리밍 및 물리 페이더 조작)
start .\hardware_sim_gui.exe
```

- **다채널 오디오 라우팅 구조**:
  - **마이크 입력 (PC WASAPI)**: 보컬(Vocal) 신호만 수신하여 **CH 1(Vocal Stem) 및 CH 3(Raw Mic)**로 전달
  - **하드웨어 시뮬레이터 (CH 2 Beat)**: 120 BPM 드럼/비트(Kick, Snare, Hi-Hat) 음원을 실시간 생성하여 UDP(0.05ms)로 메인 믹서의 **CH 2(Beat Stem)**로 스트리밍
  - **하드웨어 시뮬레이터 (CH 4 Line In 2)**: C-G-Am-F 악기 코드 반주를 실시간 생성하여 **CH 4(Line In 2)**로 스트리밍
- **하드웨어 <-> 소프트웨어 완벽 양방향 동기화 (모터 페이더 에뮬레이션)**:
  - **하드웨어 -> 메인 믹서**: 하드웨어 시뮬레이터에서 5개 물리 페이더를 움직이면 메인 믹서의 페이더가 실시간 연동되어 믹싱 볼륨이 조절됩니다.
  - **메인 믹서 -> 하드웨어**: 메인 믹서 GUI 콘솔에서 마우스로 페이더를 움직이면 하드웨어 시뮬레이터의 물리 페이더 노브와 ADC(0~1023) 수치가 즉시 모터 페이더처럼 따라 움직입니다.
  - **상호 간섭 방지 (Collision Avoidance)**: 양쪽 창에서 각각 다른 채널을 동시에 조작해도 드래그 중인 채널의 상태가 보호되어 끊김 없이 부드럽게 동기화됩니다.
  - **버튼 및 디스플레이 동기화**: `[AFS]`, `[STEM]`, `[LOUD]` 버튼 및 LED 링, 128×64 OLED 스펙트럼 디스플레이가 양방향으로 일치합니다.

### 3) 콘솔 파이프라인 데모 실행
```powershell
cd cpp
.\smart_mixer.exe --duration 5.0
```

### 4) 단위 테스트 (Unit Tests)
```powershell
cd cpp
.\test_suite.exe
```
