#pragma once
#include "Common.h"
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

class Shader
{
public:
    static Shader LoadFromFile(const fs::path& path);

    D3D12_SHADER_BYTECODE GetBytecode() const
    {
        D3D12_SHADER_BYTECODE bc{};
        bc.pShaderBytecode = m_bytes.data();
        bc.BytecodeLength = m_bytes.size();
        return bc;
    }

private:
    std::vector<uint8_t> m_bytes;
};
