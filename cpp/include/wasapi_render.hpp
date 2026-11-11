#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#include <initguid.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <iostream>

#include "ring_buffer.hpp"

namespace SmartMixer {

struct AudioOutputDeviceInfo {
    int index;
    std::wstring id;
    std::string name_utf8;
    std::wstring name_wide;
    bool is_default;
};

class WasapiRenderEngine {
public:
    static constexpr int SOURCE_RATE = 44100;

    WasapiRenderEngine()
        : m_is_running(false),
          m_current_device_idx(-1),
          m_pEnum(nullptr),
          m_pDevice(nullptr),
          m_pAudioClient(nullptr),
          m_pRenderClient(nullptr),
          m_pwfx(nullptr),
          m_buffer_frame_count(0),
          m_ring_buffer(32768, 2),
          m_resample_phase(0.0f) {
        
        CoInitialize(nullptr);
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                         __uuidof(IMMDeviceEnumerator), (void**)&m_pEnum);
    }

    ~WasapiRenderEngine() {
        stop();
        if (m_pEnum) {
            m_pEnum->Release();
            m_pEnum = nullptr;
        }
        CoUninitialize();
    }

    // 활성화된 오디오 출력(Render) 장치 목록 탐색
    std::vector<AudioOutputDeviceInfo> enumerate_render_devices() {
        std::vector<AudioOutputDeviceInfo> devices;
        if (!m_pEnum) return devices;

        IMMDevice* pDefaultDev = nullptr;
        LPWSTR pDefaultId = nullptr;
        if (SUCCEEDED(m_pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDefaultDev))) {
            pDefaultDev->GetId(&pDefaultId);
            pDefaultDev->Release();
        }

        IMMDeviceCollection* pCol = nullptr;
        HRESULT hr = m_pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCol);
        if (FAILED(hr) || !pCol) {
            if (pDefaultId) CoTaskMemFree(pDefaultId);
            return devices;
        }

        UINT count = 0;
        pCol->GetCount(&count);

        for (UINT i = 0; i < count; ++i) {
            IMMDevice* pDev = nullptr;
            pCol->Item(i, &pDev);
            if (!pDev) continue;

            LPWSTR pId = nullptr;
            pDev->GetId(&pId);

            IPropertyStore* pProps = nullptr;
            pDev->OpenPropertyStore(STGM_READ, &pProps);

            AudioOutputDeviceInfo info;
            info.index = static_cast<int>(i);
            info.id = pId ? pId : L"";
            info.is_default = (pDefaultId && pId && (wcscmp(pDefaultId, pId) == 0));

            if (pProps) {
                PROPVARIANT varName;
                PropVariantInit(&varName);
                pProps->GetValue(PKEY_Device_FriendlyName, &varName);
                if (varName.pwszVal) {
                    info.name_wide = varName.pwszVal;
                    char utf8[256] = {0};
                    WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, utf8, sizeof(utf8), nullptr, nullptr);
                    info.name_utf8 = utf8;
                }
                PropVariantClear(&varName);
                pProps->Release();
            }

            if (pId) CoTaskMemFree(pId);
            pDev->Release();
            devices.push_back(info);
        }

        pCol->Release();
        if (pDefaultId) CoTaskMemFree(pDefaultId);
        return devices;
    }

    // 선택된 출력 장치로 재생 시작 (device_idx = -1이면 기본 출력 장치)
    bool start_device(int device_idx = -1) {
        stop();

        if (!m_pEnum) return false;

        HRESULT hr;
        if (device_idx < 0) {
            hr = m_pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &m_pDevice);
        } else {
            IMMDeviceCollection* pCol = nullptr;
            hr = m_pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCol);
            if (FAILED(hr) || !pCol) return false;
            hr = pCol->Item(static_cast<UINT>(device_idx), &m_pDevice);
            pCol->Release();
        }

        if (FAILED(hr) || !m_pDevice) return false;

        hr = m_pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_pAudioClient);
        if (FAILED(hr) || !m_pAudioClient) {
            cleanup_com();
            return false;
        }

        hr = m_pAudioClient->GetMixFormat(&m_pwfx);
        if (FAILED(hr) || !m_pwfx) {
            cleanup_com();
            return false;
        }

        // 20ms 버퍼 (~200,000 hns)
        REFERENCE_TIME hnsBufferDuration = 200000;
        hr = m_pAudioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            0,
            hnsBufferDuration,
            0,
            m_pwfx,
            nullptr
        );

        if (FAILED(hr)) {
            cleanup_com();
            return false;
        }

        hr = m_pAudioClient->GetService(__uuidof(IAudioRenderClient), (void**)&m_pRenderClient);
        if (FAILED(hr) || !m_pRenderClient) {
            cleanup_com();
            return false;
        }

        hr = m_pAudioClient->GetBufferSize(&m_buffer_frame_count);
        if (FAILED(hr) || m_buffer_frame_count == 0) {
            cleanup_com();
            return false;
        }

        m_ring_buffer.clear();
        m_resample_phase = 0.0f;
        m_current_device_idx = device_idx;
        m_is_running = true;

        hr = m_pAudioClient->Start();
        if (FAILED(hr)) {
            cleanup_com();
            return false;
        }

        m_render_thread = std::thread(&WasapiRenderEngine::render_loop, this);
        return true;
    }

    void stop() {
        m_is_running = false;
        if (m_render_thread.joinable()) {
            m_render_thread.join();
        }

        if (m_pAudioClient) {
            m_pAudioClient->Stop();
        }

        cleanup_com();
        m_ring_buffer.clear();
        m_current_device_idx = -1;
    }

    // 44.1kHz 스테레오 오디오 블록 공급 (AudioMixer의 마스터 출력 공급)
    void write_samples(const float* stereo_data, size_t num_frames) {
        if (m_is_running) {
            m_ring_buffer.write(stereo_data, num_frames);
        }
    }

    bool is_running() const { return m_is_running; }
    int get_current_device_index() const { return m_current_device_idx; }

private:
    std::atomic<bool> m_is_running;
    int m_current_device_idx;

    IMMDeviceEnumerator* m_pEnum;
    IMMDevice* m_pDevice;
    IAudioClient* m_pAudioClient;
    IAudioRenderClient* m_pRenderClient;
    WAVEFORMATEX* m_pwfx;
    UINT32 m_buffer_frame_count;

    AudioRingBuffer m_ring_buffer;
    std::thread m_render_thread;
    float m_resample_phase;

    void cleanup_com() {
        if (m_pwfx) {
            CoTaskMemFree(m_pwfx);
            m_pwfx = nullptr;
        }
        if (m_pRenderClient) {
            m_pRenderClient->Release();
            m_pRenderClient = nullptr;
        }
        if (m_pAudioClient) {
            m_pAudioClient->Release();
            m_pAudioClient = nullptr;
        }
        if (m_pDevice) {
            m_pDevice->Release();
            m_pDevice = nullptr;
        }
    }

    void render_loop() {
        int device_rate = m_pwfx ? m_pwfx->nSamplesPerSec : 48000;
        int channels = m_pwfx ? m_pwfx->nChannels : 2;
        bool is_float = false;

        if (m_pwfx) {
            if (m_pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
                is_float = true;
            } else if (m_pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                WAVEFORMATEXTENSIBLE* pExt = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(m_pwfx);
                // SubFormat GUID check for IEEE Float {00000003-0000-0010-8000-00aa00389b71}
                static const GUID GUID_FLOAT = 
                    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
                if (IsEqualGUID(pExt->SubFormat, GUID_FLOAT)) {
                    is_float = true;
                }
            }
        }

        std::vector<float> src_buf(2048 * 2);
        std::vector<float> resampled_buf(2048 * 2);

        while (m_is_running) {
            UINT32 padding = 0;
            HRESULT hr = m_pAudioClient->GetCurrentPadding(&padding);
            if (FAILED(hr)) break;

            UINT32 frames_needed = m_buffer_frame_count - padding;
            if (frames_needed == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            // 요청된 출력 프레임에 해당하는 44.1kHz 입력 프레임 수 계산
            float step = static_cast<float>(SOURCE_RATE) / static_cast<float>(device_rate);
            size_t src_frames_needed = static_cast<size_t>(std::ceil(frames_needed * step)) + 2;

            if (src_buf.size() < src_frames_needed * 2) {
                src_buf.resize(src_frames_needed * 2);
            }
            if (resampled_buf.size() < frames_needed * 2) {
                resampled_buf.resize(frames_needed * 2);
            }

            size_t available = m_ring_buffer.get_available();
            if (available < src_frames_needed) {
                // 링 버퍼에 충분한 샘플이 없으면 언더런 방지 무음 채우기
                std::fill(src_buf.begin(), src_buf.begin() + src_frames_needed * 2, 0.0f);
            } else {
                m_ring_buffer.read(src_buf.data(), src_frames_needed);
            }

            // 선형 보간 리샘플링 (44.1kHz -> Device Rate)
            for (UINT32 i = 0; i < frames_needed; ++i) {
                float pos = m_resample_phase + i * step;
                size_t idx0 = static_cast<size_t>(pos);
                size_t idx1 = idx0 + 1;
                float frac = pos - idx0;

                if (idx1 < src_frames_needed) {
                    resampled_buf[i * 2]     = (1.0f - frac) * src_buf[idx0 * 2]     + frac * src_buf[idx1 * 2];
                    resampled_buf[i * 2 + 1] = (1.0f - frac) * src_buf[idx0 * 2 + 1] + frac * src_buf[idx1 * 2 + 1];
                } else if (idx0 < src_frames_needed) {
                    resampled_buf[i * 2]     = src_buf[idx0 * 2];
                    resampled_buf[i * 2 + 1] = src_buf[idx0 * 2 + 1];
                } else {
                    resampled_buf[i * 2]     = 0.0f;
                    resampled_buf[i * 2 + 1] = 0.0f;
                }
            }

            m_resample_phase = (m_resample_phase + frames_needed * step) - static_cast<size_t>(m_resample_phase + frames_needed * step);

            // WASAPI 버퍼 획득 및 데이터 쓰기
            BYTE* pData = nullptr;
            hr = m_pRenderClient->GetBuffer(frames_needed, &pData);
            if (SUCCEEDED(hr) && pData) {
                if (is_float) {
                    float* dst = reinterpret_cast<float*>(pData);
                    if (channels == 2) {
                        std::memcpy(dst, resampled_buf.data(), frames_needed * 2 * sizeof(float));
                    } else {
                        // 다채널 (5.1 등) 대응: L/R 채널 복사, 나머지 0
                        for (UINT32 i = 0; i < frames_needed; ++i) {
                            dst[i * channels]     = resampled_buf[i * 2];
                            dst[i * channels + 1] = resampled_buf[i * 2 + 1];
                            for (int c = 2; c < channels; ++c) {
                                dst[i * channels + c] = 0.0f;
                            }
                        }
                    }
                } else {
                    // 16-bit PCM 포맷
                    int16_t* dst = reinterpret_cast<int16_t*>(pData);
                    for (UINT32 i = 0; i < frames_needed; ++i) {
                        float l = std::clamp(resampled_buf[i * 2], -1.0f, 1.0f);
                        float r = std::clamp(resampled_buf[i * 2 + 1], -1.0f, 1.0f);
                        dst[i * channels]     = static_cast<int16_t>(l * 32767.0f);
                        dst[i * channels + 1] = static_cast<int16_t>(r * 32767.0f);
                        for (int c = 2; c < channels; ++c) {
                            dst[i * channels + c] = 0;
                        }
                    }
                }

                m_pRenderClient->ReleaseBuffer(frames_needed, 0);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    }
};

} // namespace SmartMixer
