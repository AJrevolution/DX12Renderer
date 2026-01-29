#include "Application.h"
#include <exception>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    try
    {
        Application app;
        if (!app.Initialize(1280, 720, L"DX12 Renderer"))
            return -1;

        return app.Run();
    }
    catch (const std::exception& e)
    {
        MessageBoxA(nullptr, e.what(), "Fatal Error", MB_OK | MB_ICONERROR);
        return -1;
    }
}
