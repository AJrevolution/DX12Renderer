#pragma once
#include <chrono>

class Timer
{
public:
    void Reset()
    {
        m_start = clock::now();
        m_last = m_start;
        m_total = 0.0f;
        m_delta = 0.0f;
    }

    void Tick()
    {
        const auto now = clock::now();
        m_delta = std::chrono::duration<float>(now - m_last).count();
        m_total = std::chrono::duration<float>(now - m_start).count();
        m_last = now;
    }

    float TotalSeconds() const { return m_total; }
    float DeltaSeconds() const { return m_delta; }

private:
    using clock = std::chrono::high_resolution_clock;

    clock::time_point m_start{};
    clock::time_point m_last{};
    float m_total = 0.0f;
    float m_delta = 0.0f;
};
