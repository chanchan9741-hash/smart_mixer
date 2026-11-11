#pragma once

#include <initguid.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <iostream>
#include <algorithm>
#include <cmath>

namespace SmartMixer {

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT GUID: {00000003-0000-0010-8000-00aa00389b71}
static const GUID GUID_SUBTYPE_IEEE_FLOAT = 
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

struct AudioDeviceInfo {
    int index;
    std::wstring id;
    std::string name_utf8;
    std::wstring name_wide;
};

class WasapiCaptureEngine {
public:
    // 콜백: 44.1kHz 스테레오 float 오디오 블록 수신 (samples, num_frames)
    using AudioCaptureCallback = std::function<void(const float* stereo_data, size_t num_frames)>;

    WasapiCaptureEngine()
        : m_is_running(false),
          m_current_device_idx(-1),
          m_pEnum(nullptr),
          m_pDevice(nullptr),
          m_pAudioClient(nullptr),
          m_pCaptureClient(nullptr),
          m_pwfx(nullptr) {
        CoInitialize(nullptr);
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                         __uuidof(IMMDeviceEnumerator), (void**)&m_pEnum);
    }

    ~WasapiCaptureEngine() {
        stop();
        if (m_pEnum) {
            m_pEnum->Release();
            m_pEnum = nullptr;
        }
        CoUninitialize();
    }

    // 활성화된 마이크(Capture) 장치 목록 탐색
    std::vector<AudioDeviceInfo> enumerate_capture_devices() {
        std::vector<AudioDeviceInfo> devices;
        if (!m_pEnum) return devices;

        IMMDeviceCollection* pCol = nullptr;
        HRESULT hr = m_pEnum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pCol);
        if (FAILED(hr) || !pCol) return devices;

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

            AudioDeviceInfo info;
            info.index = static_cast<int>(i);
            info.id = pId ? pId : L"";

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
        return devices;
    }

    bool start_device(int device_index, AudioCaptureCallback callback) {
        stop();

        if (device_index < 0 || !m_pEnum) return false;

        IMMDeviceCollection* pCol = nullptr;
        HRESULT hr = m_pEnum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pCol);
        if (FAILED(hr) || !pCol) return false;

        UINT count = 0;
        pCol->GetCount(&count);
        if (static_cast<UINT>(device_index) >= count) {
            pCol->Release();
            return false;
        }

        hr = pCol->Item(device_index, &m_pDevice);
        pCol->Release();
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

        // 지연 시간 ~15ms (150,000 * 100ns)
        REFERENCE_TIME hnsRequestedDuration = 150000;
        hr = m_pAudioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            0,
            hnsRequestedDuration,
            0,
            m_pwfx,
            nullptr
        );
        if (FAILED(hr)) {
            cleanup_com();
            return false;
        }

        hr = m_pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&m_pCaptureClient);
        if (FAILED(hr) || !m_pCaptureClient) {
            cleanup_com();
            return false;
        }

        hr = m_pAudioClient->Start();
        if (FAILED(hr)) {
            cleanup_com();
            return false;
        }

        m_callback = callback;
        m_current_device_idx = device_index;
        m_is_running = true;
        m_thread = std::thread(&WasapiCaptureEngine::capture_loop, this);
        return true;
    }

    void stop() {
        m_is_running = false;
        if (m_thread.joinable()) {
            m_thread.join();
        }
        cleanup_com();
        m_current_device_idx = -1;
    }

    bool is_running() const { return m_is_running; }
    int get_current_device_idx() const { return m_current_device_idx; }

private:
    void cleanup_com() {
        if (m_pAudioClient) {
            m_pAudioClient->Stop();
        }
        if (m_pCaptureClient) {
            m_pCaptureClient->Release();
            m_pCaptureClient = nullptr;
        }
        if (m_pAudioClient) {
            m_pAudioClient->Release();
            m_pAudioClient = nullptr;
        }
        if (m_pwfx) {
            CoTaskMemFree(m_pwfx);
            m_pwfx = nullptr;
        }
        if (m_pDevice) {
            m_pDevice->Release();
            m_pDevice = nullptr;
        }
    }

    void capture_loop() {
        UINT32 packetLength = 0;
        std::vector<float> resampled_stereo;

        int src_sample_rate = m_pwfx->nSamplesPerSec;
        int src_channels = m_pwfx->nChannels;
        bool is_float = (m_pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                        (m_pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                         reinterpret_cast<WAVEFORMATEXTENSIBLE*>(m_pwfx)->SubFormat == GUID_SUBTYPE_IEEE_FLOAT);

        while (m_is_running) {
            HRESULT hr = m_pCaptureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;

            if (packetLength == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            BYTE* pData = nullptr;
            UINT32 numFramesAvailable = 0;
            DWORD flags = 0;

            hr = m_pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            if (numFramesAvailable > 0 && m_callback) {
                resampled_stereo.resize(numFramesAvailable * 2);

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::fill(resampled_stereo.begin(), resampled_stereo.end(), 0.0f);
                } else if (is_float) {
                    const float* fData = reinterpret_cast<const float*>(pData);
                    for (UINT32 i = 0; i < numFramesAvailable; ++i) {
                        float l = fData[i * src_channels];
                        float r = (src_channels > 1) ? fData[i * src_channels + 1] : l;
                        resampled_stereo[i * 2] = l;
                        resampled_stereo[i * 2 + 1] = r;
                    }
                } else {
                    // 16-bit PCM 변환
                    const int16_t* sData = reinterpret_cast<const int16_t*>(pData);
                    for (UINT32 i = 0; i < numFramesAvailable; ++i) {
                        float l = sData[i * src_channels] / 32768.0f;
                        float r = (src_channels > 1) ? (sData[i * src_channels + 1] / 32768.0f) : l;
                        resampled_stereo[i * 2] = l;
                        resampled_stereo[i * 2 + 1] = r;
                    }
                }

                // 48kHz -> 44.1kHz 리샘플링 처리 (필요 시)
                if (src_sample_rate == 48000) {
                    size_t target_frames = static_cast<size_t>(numFramesAvailable * 44100.0 / 48000.0);
                    std::vector<float> downsampled(target_frames * 2);
                    double ratio = 48000.0 / 44100.0;
                    for (size_t i = 0; i < target_frames; ++i) {
                        double src_idx = i * ratio;
                        size_t idx0 = static_cast<size_t>(src_idx);
                        size_t idx1 = std::min(idx0 + 1, static_cast<size_t>(numFramesAvailable - 1));
                        float frac = static_cast<float>(src_idx - idx0);

                        downsampled[i * 2] = (1.0f - frac) * resampled_stereo[idx0 * 2] + frac * resampled_stereo[idx1 * 2];
                        downsampled[i * 2 + 1] = (1.0f - frac) * resampled_stereo[idx0 * 2 + 1] + frac * resampled_stereo[idx1 * 2 + 1];
                    }
                    m_callback(downsampled.data(), target_frames);
                } else {
                    m_callback(resampled_stereo.data(), numFramesAvailable);
                }
            }

            m_pCaptureClient->ReleaseBuffer(numFramesAvailable);
        }
    }

    std::atomic<bool> m_is_running;
    int m_current_device_idx;
    AudioCaptureCallback m_callback;
    std::thread m_thread;

    IMMDeviceEnumerator* m_pEnum;
    IMMDevice* m_pDevice;
    IAudioClient* m_pAudioClient;
    IAudioCaptureClient* m_pCaptureClient;
    WAVEFORMATEX* m_pwfx;
};

} // namespace SmartMixer

