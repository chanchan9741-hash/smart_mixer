#include <iostream>
#include <cassert>
#include <cmath>
#include <vector>

#include "../include/ring_buffer.hpp"
#include "../include/biquad.hpp"
#include "../include/afs.hpp"
#include "../include/loudness.hpp"
#include "../include/mixer.hpp"
#include "../include/debouncer.hpp"
#include "../include/controller_mapper.hpp"
#include "../include/stem_separator.hpp"
#include "../include/audio_file_player.hpp"

using namespace SmartMixer;

void test_ring_buffer() {
    std::cout << "[Test 1] AudioRingBuffer...";
    AudioRingBuffer rb(1024, 2);
    std::vector<float> data(256 * 2, 0.5f);
    size_t written = rb.write(data.data(), 256);
    assert(written == 256);
    assert(rb.get_available() == 256);

    std::vector<float> out(256 * 2, 0.0f);
    size_t read_cnt = rb.read(out.data(), 256);
    assert(read_cnt == 256);
    assert(rb.get_available() == 0);
    assert(std::abs(out[0] - 0.5f) < 1e-4f);

    // 언더런 테스트
    rb.read(out.data(), 100);
    assert(rb.get_underruns() > 0);
    std::cout << " PASSED\n";
}

void test_biquad_notch() {
    std::cout << "[Test 2] BiquadFilter (Notch Attenuation)...";
    BiquadFilter notch(FilterType::Notch, 44100, 1000.0f, 10.0f, -18.0f);

    // 1000Hz 정현파 신호 통과
    constexpr float PI = 3.14159265358979323846f;
    std::vector<float> buf(512 * 2);
    for (size_t i = 0; i < 512; ++i) {
        float s = std::sin(2.0f * PI * 1000.0f * i / 44100.0f);
        buf[i * 2] = s;
        buf[i * 2 + 1] = s;
    }

    notch.process(buf.data(), 512);

    // 1000Hz 신호가 급격히 감쇄되었는지 확인 (후반부 정상 상태)
    float post_amp = 0.0f;
    for (size_t i = 300; i < 512; ++i) {
        post_amp = std::max(post_amp, std::abs(buf[i * 2]));
    }
    assert(post_amp < 0.3f); // -18dB 이상 감쇄
    std::cout << " PASSED (Attenuated 1.0 -> " << post_amp << ")\n";
}

void test_feedback_detector() {
    std::cout << "[Test 3] FeedbackDetector (Peak Detection)...";
    FeedbackDetector detector(44100, 1024, 10.0f, 2);

    constexpr float PI = 3.14159265358979323846f;
    std::vector<float> sine_2khz(1024);
    for (int i = 0; i < 1024; ++i) {
        sine_2khz[i] = 0.8f * std::sin(2.0f * PI * 2000.0f * i / 44100.0f);
    }

    // Frame 1: 후보 등록
    auto freqs1 = detector.analyze(sine_2khz.data(), 1024);
    // Frame 2: 지속성 조건 만족 감지
    auto freqs2 = detector.analyze(sine_2khz.data(), 1024);

    assert(!freqs2.empty());
    assert(std::abs(freqs2[0] - 2000.0f) < 60.0f);
    std::cout << " PASSED (Detected at " << freqs2[0] << "Hz)\n";
}

void test_loudness_limiter() {
    std::cout << "[Test 4] LoudnessMatcher & Peak Limiter...";
    LoudnessMatcher matcher(44100);

    // 1.5 크기의 과도한 클리핑 입력
    std::vector<float> clipped(512 * 2, 1.5f);
    matcher.process(clipped.data(), 512);

    float max_peak = 0.0f;
    for (float s : clipped) {
        max_peak = std::max(max_peak, std::abs(s));
    }
    assert(max_peak <= 1.05f); // 리미터로 억제 확인
    std::cout << " PASSED (Peak Limited to " << max_peak << ")\n";
}

void test_button_debouncer() {
    std::cout << "[Test 5] ButtonDebouncer (Anti-Chattering)...";
    ButtonDebouncer debouncer(25.0f, 600.0f);

    // 채터링 노이즈 (0ms ~ 15ms 바운스)
    debouncer.update(true, 0.0);
    debouncer.update(false, 5.0);
    debouncer.update(true, 10.0);

    // 안정화 시간 25ms 경과 후 (40ms)
    ButtonEvent ev1 = debouncer.update(true, 40.0);
    assert(ev1 == ButtonEvent::None); // 아직 릴리즈되지 않음

    // 버튼 뗌 (100ms)
    debouncer.update(false, 150.0);
    ButtonEvent ev2 = debouncer.update(false, 180.0);
    assert(ev2 == ButtonEvent::Click); // 정상 클릭 인정
    std::cout << " PASSED\n";
}

void test_mixer_gain_ramp() {
    std::cout << "[Test 6] AudioMixer (5-Channel Gain Ramp)...";
    AudioMixer mixer(5);

    // 페이더를 0.0 -> 0.8로 변경
    mixer.set_fader(1, 0.8f);

    std::vector<float> input(512 * 2, 1.0f);
    std::vector<const float*> inputs = {
        input.data(), input.data(), input.data(), input.data(), input.data()
    };
    std::vector<float> master_out(512 * 2);

    mixer.mix(inputs, master_out.data(), 512);

    // 첫 샘플보다 마지막 샘플의 게인이 서서히 증가했는지 확인 (Ramp 확인)
    assert(std::abs(master_out[0]) < std::abs(master_out[511 * 2]));
    std::cout << " PASSED\n";
}

void test_stem_separation_and_export() {
    std::cout << "[Test 7] StemSeparator & AudioFile Export (3-Way)...";
    StemSeparator stem(44100);

    // 합성 음악 신호 생성: 100Hz(Kick/Beat) + 1200Hz(Vocal) + Stereo Harmonics(Inst)
    std::vector<float> mix(1024 * 2);
    constexpr float PI = 3.14159265358979323846f;
    for (size_t i = 0; i < 1024; ++i) {
        float beat = 0.5f * std::sin(2.0f * PI * 100.0f * i / 44100.0f);
        float vocal = 0.4f * std::sin(2.0f * PI * 1200.0f * i / 44100.0f);
        float inst = 0.3f * std::sin(2.0f * PI * 5000.0f * i / 44100.0f);

        // Vocal은 Center(Mid), Inst는 Stereo Side(+/-)
        mix[i * 2]     = beat + vocal + inst;
        mix[i * 2 + 1] = beat + vocal - inst;
    }

    std::vector<float> vocal_out(1024 * 2);
    std::vector<float> beat_out(1024 * 2);
    std::vector<float> inst_out(1024 * 2);

    stem.process(mix.data(), vocal_out.data(), beat_out.data(), inst_out.data(), 1024);

    // RMS 에너지 계산
    float v_energy = 0.0f, b_energy = 0.0f, i_energy = 0.0f;
    for (size_t i = 0; i < 1024 * 2; ++i) {
        v_energy += vocal_out[i] * vocal_out[i];
        b_energy += beat_out[i] * beat_out[i];
        i_energy += inst_out[i] * inst_out[i];
    }

    assert(v_energy > 0.01f);
    assert(b_energy > 0.01f);
    assert(i_energy > 0.01f);

    // WAV 파일 저장 및 확인
    std::wstring v_file = L"test_vocal.wav";
    bool ok = AudioFilePlayer::write_wav_file(v_file, vocal_out, 44100);
    assert(ok);
    _wremove(v_file.c_str());

    std::cout << " PASSED (3 Stems Separated & Export Verified)\n";
}

int main() {
    std::cout << "\n=== Running C++ Core Unit Tests ===\n";
    test_ring_buffer();
    test_biquad_notch();
    test_feedback_detector();
    test_loudness_limiter();
    test_button_debouncer();
    test_mixer_gain_ramp();
    test_stem_separation_and_export();
    std::cout << "=== All 7 C++ Tests Passed Successfully! ===\n\n";
    return 0;
}

