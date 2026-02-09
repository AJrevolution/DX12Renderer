#include "Source\Core\Paths.h"

namespace
{
    bool DirExists(const std::filesystem::path& p)
    {
        std::error_code ec;
        return std::filesystem::exists(p, ec) && std::filesystem::is_directory(p, ec);
    }
}

namespace Paths
{
    std::filesystem::path ExecutableDir()
    {
        wchar_t buf[MAX_PATH]{};
        DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (len == 0)
            throw std::runtime_error("GetModuleFileNameW failed.");

        return std::filesystem::path(buf).parent_path();
    }

    std::filesystem::path ShaderDir()
    {
        return ExecutableDir() / L"Shaders" / L"Compiled";
    }

    std::filesystem::path ContentDir_DevOnly()
    {
#if defined(_DEBUG)
        // Dev convenience only.
        // Typical: <repo>\x64\Debug\  ->  <repo>\Assets via ../../Assets
        auto candidate = ExecutableDir() / L"..\\..\\Assets";
        candidate = candidate.lexically_normal();

        if (!DirExists(candidate))
        {
            DebugOutput("Paths::ContentDir_DevOnly: Assets directory not found (dev-only).");
            return {};
        }

        return candidate;
#else
        // In release, content discovery should be explicit (config/installer).
        return {};
#endif
    }
}
