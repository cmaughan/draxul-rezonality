#pragma once

#include <filesystem>
#include <string>

namespace rezonality
{

// Native std::filesystem::path values remain the filesystem identity. These
// helpers produce text for serialization, hashing, diagnostics, and
// presentation. Unlike path::string(), they never use the Windows active
// code page, which cannot represent most international names and throws for
// unmappable characters. On POSIX the native bytes are returned unchanged.
//
// path_utf8/generic_path_utf8 throw only when the native name is not valid
// Unicode (for example, an unpaired UTF-16 surrogate on Windows).
inline std::string path_utf8(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

inline std::string generic_path_utf8(const std::filesystem::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

// Display-only variant for status text: never throws for an unrepresentable
// native name.
inline std::string display_path_utf8(const std::filesystem::path& path)
{
    try
    {
        return path_utf8(path);
    }
    catch (const std::exception&)
    {
        return "<unrepresentable path>";
    }
}

} // namespace rezonality
