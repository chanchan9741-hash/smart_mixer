#pragma once

#include <vector>
#include <mutex>
#include <algorithm>
#include <cstddef>

namespace SmartMixer {

class AudioRingBuffer {
public:
    AudioRingBuffer(size_t capacity = 4096, int channels = 2)
        : m_capacity(capacity), m_channels(channels),
          m_buffer(capacity * channels, 0.0f),
          m_write_idx(0), m_read_idx(0), m_available(0),
          m_underrun_count(0), m_overrun_count(0) {}

    // Input Handler: 오디오 샘플 기록
    size_t write(const float* data, size_t num_samples) {
        if (!data || num_samples == 0) return 0;

        std::lock_guard<std::mutex> lock(m_mutex);

        // 전체 용량 초과 시 최근 데이터로 클리핑
        if (num_samples > m_capacity) {
            m_overrun_count++;
            data += (num_samples - m_capacity) * m_channels;
            num_samples = m_capacity;
            m_read_idx = m_write_idx;
            m_available = 0;
        }

        size_t free_space = m_capacity - m_available;
        if (num_samples > free_space) {
            m_overrun_count++;
            size_t dropped = num_samples - free_space;
            m_read_idx = (m_read_idx + dropped) % m_capacity;
            m_available -= dropped;
        }

        size_t first_chunk = std::min(num_samples, m_capacity - m_write_idx);
        std::copy(data, data + first_chunk * m_channels, m_buffer.data() + m_write_idx * m_channels);

        size_t second_chunk = num_samples - first_chunk;
        if (second_chunk > 0) {
            std::copy(data + first_chunk * m_channels,
                      data + num_samples * m_channels,
                      m_buffer.data());
        }

        m_write_idx = (m_write_idx + num_samples) % m_capacity;
        m_available = std::min(m_capacity, m_available + num_samples);
        return num_samples;
    }

    // Output Handler: 오디오 샘플 읽기 (언더런 시 0.0 패딩)
    size_t read(float* out_data, size_t num_samples) {
        if (!out_data || num_samples == 0) return 0;

        std::lock_guard<std::mutex> lock(m_mutex);

        size_t read_samples = num_samples;
        if (m_available < num_samples) {
            m_underrun_count++;
            read_samples = m_available;
        }

        if (read_samples > 0) {
            size_t first_chunk = std::min(read_samples, m_capacity - m_read_idx);
            std::copy(m_buffer.data() + m_read_idx * m_channels,
                      m_buffer.data() + (m_read_idx + first_chunk) * m_channels,
                      out_data);

            size_t second_chunk = read_samples - first_chunk;
            if (second_chunk > 0) {
                std::copy(m_buffer.data(),
                          m_buffer.data() + second_chunk * m_channels,
                          out_data + first_chunk * m_channels);
            }

            m_read_idx = (m_read_idx + read_samples) % m_capacity;
            m_available -= read_samples;
        }

        // 부족한 샘플 0으로 채움 (Zero-padding)
        if (read_samples < num_samples) {
            std::fill(out_data + read_samples * m_channels,
                      out_data + num_samples * m_channels,
                      0.0f);
        }

        return num_samples;
    }

    size_t get_available() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_available;
    }

    size_t get_underruns() const { return m_underrun_count; }
    size_t get_overruns() const { return m_overrun_count; }

    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_write_idx = 0;
        m_read_idx = 0;
        m_available = 0;
        std::fill(m_buffer.begin(), m_buffer.end(), 0.0f);
    }

private:
    size_t m_capacity;
    int m_channels;
    std::vector<float> m_buffer;
    size_t m_write_idx;
    size_t m_read_idx;
    size_t m_available;
    mutable std::mutex m_mutex;
    size_t m_underrun_count;
    size_t m_overrun_count;
};

} // namespace SmartMixer

