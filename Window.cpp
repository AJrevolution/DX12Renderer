#include "Window.h"

namespace
{
    bool IsTrackedInputKey(uint32_t key)
    {
        switch (key)
        {
        case 'A':
        case 'D':
        case 'W':
        case 'S':
        case 'O':
        case 'F':
        case 'R':
        case 'Q':
        case '0':
        case VK_F1:
        case VK_F6:
        case VK_F7:
        case VK_F8:
        case VK_F9:
            return true;

        default:
            return false;
        }
    }
}

Window::~Window()
{
    if (m_mouseCaptured)
    {
        SetMouseCaptured(false);
    }

    if (m_hwnd)
    {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}
bool Window::Create(uint32_t width, uint32_t height, const wchar_t* title)
{
    m_width = width;
    m_height = height;

    // Register the window class once per process (prevents repeated registration noise).
    static bool s_registered = false;
    if (!s_registered)
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;         // Redraw on horizontal/vertical resize
        wc.lpfnWndProc = &Window::WndProc;               // Static thunk -> instance handler
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"DX12RendererWindowClass";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

        // RegisterClassEx returns 0 on failure, but also fails if already registered.
        // We only attempt once, so treat failure as fatal here.
        if (RegisterClassExW(&wc) == 0)
            return false;

        s_registered = true;
    }

    // Convert desired client area to full window rect (accounts for title bar/borders).
    RECT r = { 0, 0, (LONG)width, (LONG)height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

    m_hwnd = CreateWindowExW(
        0,
        L"DX12RendererWindowClass",
        title,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        nullptr, nullptr,
        GetModuleHandleW(nullptr),
        this // WM_NCCREATE will receive this; stored in GWLP_USERDATA
    );

    if (!m_hwnd)
        return false;

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
    return true;
}


bool Window::PumpMessages()
{
    MSG msg = {};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

bool Window::ConsumeResize(uint32_t& outW, uint32_t& outH)
{
    if (!m_pendingResize) return false;
    m_pendingResize = false;
    outW = m_pendingW;
    outH = m_pendingH;
    return true;
}

bool Window::ConsumeKeyPress(uint32_t virtualKey)
{
    if (virtualKey >= m_keyPressed.size())
        return false;

    const bool pressed = m_keyPressed[virtualKey];
    m_keyPressed[virtualKey] = false;
    return pressed;
}

bool Window::IsKeyDown(uint32_t virtualKey) const
{
    if (virtualKey >= m_keyDown.size())
        return false;

    return m_keyDown[virtualKey];
}

void Window::SetMouseCaptured(bool enabled)
{
    if (!m_hwnd || m_mouseCaptured == enabled)
        return;

    m_mouseDeltaX = 0.0f;
    m_mouseDeltaY = 0.0f;
    m_ignoreNextMouseMove = false;

    if (enabled)
    {
        m_mouseCaptured = true;

        SetCapture(m_hwnd);
        UpdateCursorClip();
        CentreCapturedCursor();
        m_ignoreNextMouseMove = true;
        SetCursorVisible(false);
    }
    else
    {
        m_mouseCaptured = false;

        ClipCursor(nullptr);

        if (GetCapture() == m_hwnd)
        {
            ReleaseCapture();
        }

        SetCursorVisible(true);
    }
}

bool Window::IsMouseCaptured() const
{
    return m_mouseCaptured;
}

void Window::ConsumeMouseDelta(float& outDeltaX, float& outDeltaY)
{
    outDeltaX = m_mouseDeltaX;
    outDeltaY = m_mouseDeltaY;

    m_mouseDeltaX = 0.0f;
    m_mouseDeltaY = 0.0f;
}

void Window::CentreCapturedCursor()
{
    if (!m_hwnd)
        return;

    RECT client{};
    GetClientRect(m_hwnd, &client);

    POINT centre{};
    centre.x = (client.right - client.left) / 2;
    centre.y = (client.bottom - client.top) / 2;

    ClientToScreen(m_hwnd, &centre);
    SetCursorPos(centre.x, centre.y);
}

void Window::UpdateCursorClip()
{
    if (!m_hwnd)
        return;

    RECT client{};
    GetClientRect(m_hwnd, &client);

    POINT topLeft{ client.left, client.top };
    POINT bottomRight{ client.right, client.bottom };

    ClientToScreen(m_hwnd, &topLeft);
    ClientToScreen(m_hwnd, &bottomRight);

    RECT clip{};
    clip.left = topLeft.x;
    clip.top = topLeft.y;
    clip.right = bottomRight.x;
    clip.bottom = bottomRight.y;

    ClipCursor(&clip);
}

void Window::SetCursorVisible(bool visible)
{
    if (m_cursorVisible == visible)
        return;

    m_cursorVisible = visible;

    if (visible)
    {
        while (ShowCursor(TRUE) < 0)
        {
        }
    }
    else
    {
        while (ShowCursor(FALSE) >= 0)
        {
        }
    }
}

LRESULT CALLBACK Window::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    Window* self = nullptr;

    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = reinterpret_cast<Window*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    }
    else
    {
        self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) return self->HandleMessage(hwnd, msg, wparam, lparam);
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT Window::HandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_DESTROY:
        SetMouseCaptured(false);
        PostQuitMessage(0);
        return 0;

    case WM_MOUSEMOVE:
    {
        if (!m_mouseCaptured)
            break;

        RECT client{};
        GetClientRect(hwnd, &client);

        const int centreX = (client.right - client.left) / 2;
        const int centreY = (client.bottom - client.top) / 2;

        const int x = static_cast<int>(static_cast<short>(LOWORD(lparam)));
        const int y = static_cast<int>(static_cast<short>(HIWORD(lparam)));

        if (m_ignoreNextMouseMove && x == centreX && y == centreY)
        {
            m_ignoreNextMouseMove = false;
            return 0;
        }

        m_ignoreNextMouseMove = false;

        const int dx = x - centreX;
        const int dy = y - centreY;

        if (dx != 0 || dy != 0)
        {
            m_mouseDeltaX += static_cast<float>(dx);
            m_mouseDeltaY += static_cast<float>(dy);

            CentreCapturedCursor();
            m_ignoreNextMouseMove = true;
        }

        return 0;
    }

    case WM_CAPTURECHANGED:
    {
        if (m_mouseCaptured && reinterpret_cast<HWND>(lparam) != m_hwnd)
        {
            m_mouseCaptured = false;
            m_mouseDeltaX = 0.0f;
            m_mouseDeltaY = 0.0f;
            m_ignoreNextMouseMove = false;

            ClipCursor(nullptr);
            SetCursorVisible(true);
        }

        return 0;
    }
    case WM_KEYDOWN:
    {
        const uint32_t key = static_cast<uint32_t>(wparam);

        if (!IsTrackedInputKey(key))
            break;

        // Track held state for continuous input, and record one edge-triggered
        // press for toggle-style controls.
        constexpr LPARAM kWasDownMask = (LPARAM(1) << 30);

        if (key < m_keyDown.size())
        {
            m_keyDown[key] = true;

            if ((lparam & kWasDownMask) == 0)
            {
                m_keyPressed[key] = true;
            }
        }

        // These are application-owned keys. Do not let movement/debug shortcuts reach
        // DefWindowProcW, where they may trigger default system or accelerator handling.
        return 0;
    }

    case WM_KEYUP:
    {
        const uint32_t key = static_cast<uint32_t>(wparam);

        if (!IsTrackedInputKey(key))
            break;

        if (key < m_keyDown.size())
        {
            m_keyDown[key] = false;
        }

        return 0;
    }

    case WM_KILLFOCUS:
    {
        m_keyPressed.fill(false);
        m_keyDown.fill(false);
        SetMouseCaptured(false);
        return 0;
    }

    case WM_SIZE:
    {
        // Ignore minimization (width/height become 0)
        const uint32_t w = (uint32_t)LOWORD(lparam);
        const uint32_t h = (uint32_t)HIWORD(lparam);

        m_width = w;
        m_height = h;

        if (w > 0 && h > 0)
        {
            m_pendingResize = true;
            m_pendingW = w;
            m_pendingH = h;
        }

        if (m_mouseCaptured)
        {
            UpdateCursorClip();
            CentreCapturedCursor();
            m_ignoreNextMouseMove = true;
        }
        return 0;
    }

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}
