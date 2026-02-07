#include "Shader.h"
#include <fstream>

Shader Shader::LoadFromFile(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Shader::LoadFromFile: failed to open.");

    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    Shader s;
    s.m_bytes.resize((size_t)size);

    if (!file.read(reinterpret_cast<char*>(s.m_bytes.data()), size))
        throw std::runtime_error("Shader::LoadFromFile: failed to read.");

    return s;
}
