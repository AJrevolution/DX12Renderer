#pragma once
#include "Common.h"

class Window
{
public:
    Window() = default;
    ~Window();

    bool Create(uint32_t width, uint32_t height, const wchar_t* title);
    bool PumpMessages(); // returns false when quit requested

    HWND     GetHWND()   const { return m_hwnd; }
    uint32_t Width()     const { return m_width; }
    uint32_t Height()    const { return m_height; }

    // Resize tracking
    bool     ConsumeResize(uint32_t& outW, uint32_t& outH);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

private:
    HWND m_hwnd = nullptr;

    uint32_t m_width = 0;
    uint32_t m_height = 0;

    bool m_pendingResize = false;
    uint32_t m_pendingW = 0;
    uint32_t m_pendingH = 0;
};
