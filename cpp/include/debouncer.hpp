#pragma once

#include <chrono>
#include <string>

namespace SmartMixer {

enum class ButtonEvent {
    None,
    Click,
    LongPress,
    Release
};

class ButtonDebouncer {
public:
    ButtonDebouncer(float debounce_ms = 25.0f, float long_press_ms = 600.0f)
        : m_debounce_ms(debounce_ms), m_long_press_ms(long_press_ms),
          m_last_raw(false), m_stable(false),
          m_last_change_time(0.0), m_press_start_time(0.0),
          m_long_press_fired(false) {}

    ButtonEvent update(bool raw_pressed, double now_ms) {
        ButtonEvent event = ButtonEvent::None;

        if (raw_pressed != m_last_raw) {
            m_last_change_time = now_ms;
            m_last_raw = raw_pressed;
        }

        if ((now_ms - m_last_change_time) > m_debounce_ms) {
            if (raw_pressed != m_stable) {
                m_stable = raw_pressed;
                if (m_stable) {
                    m_press_start_time = now_ms;
                    m_long_press_fired = false;
                } else {
                    event = (!m_long_press_fired) ? ButtonEvent::Click : ButtonEvent::Release;
                }
            }
        }

        if (m_stable && !m_long_press_fired) {
            if ((now_ms - m_press_start_time) >= m_long_press_ms) {
                m_long_press_fired = true;
                event = ButtonEvent::LongPress;
            }
        }

        return event;
    }

private:
    float m_debounce_ms;
    float m_long_press_ms;
    bool m_last_raw;
    bool m_stable;
    double m_last_change_time;
    double m_press_start_time;
    bool m_long_press_fired;
};

} // namespace SmartMixer

