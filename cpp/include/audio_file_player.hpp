#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <cmath>
#include <fstream>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <functional>

#include "stem_separator.hpp"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace SmartMixer {

#pragma pack(push, 1)
struct WavHeaderRiff {
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t file_size = 0;
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmt[4] = {'f', 'm', 't', ' '};
    uint32_t fmt_size = 16;
    uint16_t audio_format = 1; // PCM
    uint16_t num_channels = 2; // Stereo
    uint32_t sample_rate = 44100;
    uint32_t byte_rate = 44100 * 2 * 2;
    uint16_t block_align = 4;
    uint16_t bits_per_sample = 16;
    char data[4] = {'d', 'a', 't', 'a'};
    uint32_t data_size = 0;
};
#pragma pack(pop)

class AudioFilePlayer {
public:
    AudioFilePlayer(int target_sample_rate = 44100)
        : m_target_sample_rate(target_sample_rate),
          m_current_frame(0),
          m_is_playing(false),
          m_loop(true) {}

    ~AudioFilePlayer() {
        stop();
    }

    bool load_file(const std::wstring& file_path) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_is_playing = false;
        m_current_frame = 0;
        m_samples.clear();

        // 1. Windows Media Foundation을 활용하여 MP3, WAV, FLAC, M4A 등 임의 포맷 로드
        bool success = load_via_media_foundation(file_path);

        if (!success) {
            // MF 실패 시 직접 WAV 파서 시도
            success = load_via_direct_wav(file_path);
        }

        if (success && !m_samples.empty()) {
            m_file_path = file_path;
            // 파일명 추출
            size_t slash_pos = file_path.find_last_of(L"\\/");
            if (slash_pos != std::wstring::npos) {
                m_file_name = file_path.substr(slash_pos + 1);
            } else {
                m_file_name = file_path;
            }
            return true;
        }

        return false;
    }

    void play() {
        if (!m_samples.empty()) {
            m_is_playing = true;
        }
    }

    void pause() {
        m_is_playing = false;
    }

    void stop() {
        m_is_playing = false;
        m_current_frame = 0;
    }

    void seek(float position_seconds) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_samples.empty()) return;
        size_t total_frames = m_samples.size() / 2;
        size_t target_frame = static_cast<size_t>(position_seconds * m_target_sample_rate);
        m_current_frame = std::min(target_frame, total_frames);
    }

    // 512샘플 실시간 읽기 (스테레오 인터리빙)
    size_t read_frames(float* out_buffer, size_t num_frames) {
        if (!m_is_playing || m_samples.empty()) {
            std::fill(out_buffer, out_buffer + num_frames * 2, 0.0f);
            return 0;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        size_t total_frames = m_samples.size() / 2;
        size_t frames_read = 0;

        for (size_t i = 0; i < num_frames; ++i) {
            if (m_current_frame >= total_frames) {
                if (m_loop) {
                    m_current_frame = 0;
                } else {
                    m_is_playing = false;
                    for (size_t k = i; k < num_frames; ++k) {
                        out_buffer[k * 2] = 0.0f;
                        out_buffer[k * 2 + 1] = 0.0f;
                    }
                    return frames_read;
                }
            }

            out_buffer[i * 2] = m_samples[m_current_frame * 2];
            out_buffer[i * 2 + 1] = m_samples[m_current_frame * 2 + 1];
            m_current_frame++;
            frames_read++;
        }

        return frames_read;
    }

    bool is_loaded() const {
        return !m_samples.empty();
    }

    bool is_playing() const {
        return m_is_playing;
    }

    float get_duration_seconds() const {
        if (m_samples.empty()) return 0.0f;
        return static_cast<float>(m_samples.size() / 2) / m_target_sample_rate;
    }

    float get_current_seconds() const {
        return static_cast<float>(m_current_frame) / m_target_sample_rate;
    }

    std::wstring get_file_name() const {
        return m_file_name;
    }

    std::wstring get_file_path() const {
        return m_file_path;
    }

    // 전체 로드된 음악 파일을 스템 분리하여 3개의 WAV 파일로 일괄 저장
    bool export_separated_stems(const std::wstring& out_vocal_path,
                                const std::wstring& out_beat_path,
                                const std::wstring& out_inst_path,
                                StemSeparator& separator,
                                std::function<void(float progress)> progress_cb = nullptr) {
        if (m_samples.empty()) return false;

        std::vector<float> all_samples;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            all_samples = m_samples;
        }

        size_t total_samples = all_samples.size();
        size_t total_frames = total_samples / 2;
        constexpr size_t BLOCK_SIZE = 512;

        std::vector<float> vocal_samples(total_samples);
        std::vector<float> beat_samples(total_samples);
        std::vector<float> inst_samples(total_samples);

        std::vector<float> in_block(BLOCK_SIZE * 2);
        std::vector<float> v_block(BLOCK_SIZE * 2);
        std::vector<float> b_block(BLOCK_SIZE * 2);
        std::vector<float> i_block(BLOCK_SIZE * 2);

        size_t processed_frames = 0;

        while (processed_frames < total_frames) {
            size_t chunk = std::min(BLOCK_SIZE, total_frames - processed_frames);

            std::copy(all_samples.data() + processed_frames * 2,
                      all_samples.data() + (processed_frames + chunk) * 2,
                      in_block.data());

            if (chunk < BLOCK_SIZE) {
                std::fill(in_block.data() + chunk * 2, in_block.data() + BLOCK_SIZE * 2, 0.0f);
            }

            separator.process(in_block.data(), v_block.data(), b_block.data(), i_block.data(), chunk);

            std::copy(v_block.data(), v_block.data() + chunk * 2,
                      vocal_samples.data() + processed_frames * 2);
            std::copy(b_block.data(), b_block.data() + chunk * 2,
                      beat_samples.data() + processed_frames * 2);
            std::copy(i_block.data(), i_block.data() + chunk * 2,
                      inst_samples.data() + processed_frames * 2);

            processed_frames += chunk;
            if (progress_cb) {
                progress_cb(static_cast<float>(processed_frames) / total_frames);
            }
        }

        bool ok_v = write_wav_file(out_vocal_path, vocal_samples, m_target_sample_rate);
        bool ok_b = write_wav_file(out_beat_path, beat_samples, m_target_sample_rate);
        bool ok_i = write_wav_file(out_inst_path, inst_samples, m_target_sample_rate);

        return ok_v && ok_b && ok_i;
    }

    static bool write_wav_file(const std::wstring& path, const std::vector<float>& samples, int sample_rate = 44100, int channels = 2) {
        FILE* fp = nullptr;
        _wfopen_s(&fp, path.c_str(), L"wb");
        if (!fp) return false;

        uint32_t num_samples = static_cast<uint32_t>(samples.size());
        uint32_t data_bytes = num_samples * sizeof(int16_t);

        WavHeaderRiff hdr;
        hdr.file_size = sizeof(WavHeaderRiff) - 8 + data_bytes;
        hdr.num_channels = channels;
        hdr.sample_rate = sample_rate;
        hdr.bits_per_sample = 16;
        hdr.block_align = channels * (hdr.bits_per_sample / 8);
        hdr.byte_rate = sample_rate * hdr.block_align;
        hdr.data_size = data_bytes;

        fwrite(&hdr, sizeof(hdr), 1, fp);

        std::vector<int16_t> pcm16(num_samples);
        for (size_t i = 0; i < num_samples; ++i) {
            float s = std::max(-1.0f, std::min(1.0f, samples[i]));
            pcm16[i] = static_cast<int16_t>(s * 32767.0f);
        }

        fwrite(pcm16.data(), sizeof(int16_t), num_samples, fp);
        fclose(fp);
        return true;
    }

private:
    int m_target_sample_rate;
    std::wstring m_file_path;
    std::wstring m_file_name;
    std::vector<float> m_samples; // 44.1kHz Stereo interleaved
    size_t m_current_frame;
    std::atomic<bool> m_is_playing;
    bool m_loop;
    std::mutex m_mutex;

    bool load_via_media_foundation(const std::wstring& filePath) {
        HRESULT hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) return false;

        IMFSourceReader* pReader = nullptr;
        hr = MFCreateSourceReaderFromURL(filePath.c_str(), nullptr, &pReader);
        if (FAILED(hr)) {
            MFShutdown();
            return false;
        }

        IMFMediaType* pPartialType = nullptr;
        MFCreateMediaType(&pPartialType);
        pPartialType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pPartialType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        pPartialType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        pPartialType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, m_target_sample_rate);
        pPartialType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);

        hr = pReader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, pPartialType);
        pPartialType->Release();

        if (FAILED(hr)) {
            pReader->Release();
            MFShutdown();
            return false;
        }

        m_samples.clear();

        while (true) {
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            IMFSample* pSample = nullptr;

            hr = pReader->ReadSample(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
                0, nullptr, &flags, &timestamp, &pSample
            );

            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
                if (pSample) pSample->Release();
                break;
            }

            if (pSample) {
                IMFMediaBuffer* pBuffer = nullptr;
                hr = pSample->ConvertToContiguousBuffer(&pBuffer);
                if (SUCCEEDED(hr)) {
                    BYTE* pData = nullptr;
                    DWORD cbCurrentLen = 0;
                    hr = pBuffer->Lock(&pData, nullptr, &cbCurrentLen);
                    if (SUCCEEDED(hr)) {
                        size_t floatCount = cbCurrentLen / sizeof(float);
                        const float* fData = reinterpret_cast<const float*>(pData);
                        m_samples.insert(m_samples.end(), fData, fData + floatCount);
                        pBuffer->Unlock();
                    }
                    pBuffer->Release();
                }
                pSample->Release();
            }
        }

        pReader->Release();
        MFShutdown();
        return !m_samples.empty();
    }

    bool load_via_direct_wav(const std::wstring& filePath) {
        FILE* fp = nullptr;
        _wfopen_s(&fp, filePath.c_str(), L"rb");
        if (!fp) return false;

        WavHeaderRiff hdr;
        if (fread(&hdr, sizeof(hdr), 1, fp) != 1) {
            fclose(fp);
            return false;
        }

        if (std::memcmp(hdr.riff, "RIFF", 4) != 0 || std::memcmp(hdr.wave, "WAVE", 4) != 0) {
            fclose(fp);
            return false;
        }

        size_t num_samples = hdr.data_size / (hdr.bits_per_sample / 8);
        m_samples.resize(num_samples);

        if (hdr.bits_per_sample == 16) {
            std::vector<int16_t> raw(num_samples);
            fread(raw.data(), sizeof(int16_t), num_samples, fp);
            for (size_t i = 0; i < num_samples; ++i) {
                m_samples[i] = raw[i] / 32768.0f;
            }
        } else if (hdr.bits_per_sample == 32) {
            fread(m_samples.data(), sizeof(float), num_samples, fp);
        } else {
            fclose(fp);
            return false;
        }

        fclose(fp);
        return !m_samples.empty();
    }
};

} // namespace SmartMixer
