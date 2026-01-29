#include "Window.h"

Window::~Window()
{
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
        PostQuitMessage(0);
        return 0;

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
        return 0;
    }

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}
