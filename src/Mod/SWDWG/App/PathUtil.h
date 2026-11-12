//! Path helpers for the SWDWG importer.
//!
//! Why this exists
//! ---------------
//! FreeCAD hands file names to the importer as UTF-8 (Python str -> std::string).
//! On Windows, however, both the C runtime (fopen / std::ifstream) and LibreDWG
//! interpret a `const char*` path using the *ANSI* code page.  A UTF-8 encoded
//! Chinese path therefore resolves to a different, non-existent file name:
//! dwg_read_file() then fails with DWG_ERR_INVALIDDWG and zero objects, while
//! the very same file opens fine from a pure-ASCII path.
//!
//! toLocalPath() converts the UTF-8 path into the encoding the native file APIs
//! expect.  It uses std::filesystem only (no windows.h), because the LibreDWG
//! headers pull in enough Win32 declarations to interfere with an explicit
//! <windows.h> include in the same translation unit.
//!
//! On non-Windows platforms, and for pure-ASCII paths, it is a no-op, so it is
//! safe to call unconditionally.

#pragma once

#include <string>

#ifdef _WIN32
#  include <filesystem>
#endif

namespace SWDWG
{

//! True when every byte is < 0x80, i.e. the path needs no conversion at all.
inline bool isAsciiPath(const std::string& s)
{
    for (unsigned char c : s) {
        if (c >= 0x80) {
            return false;
        }
    }
    return true;
}

//! Convert a UTF-8 path into the encoding the native file APIs expect.
//!
//! Returns \a utf8Path unchanged when it is empty, already ASCII, or when the
//! conversion throws -- a garbled path is never returned.
inline std::string toLocalPath(const std::string& utf8Path)
{
#ifdef _WIN32
    if (utf8Path.empty() || isAsciiPath(utf8Path)) {
        return utf8Path;
    }
    try {
        // Constructing the path from UTF-8 and reading it back with string()
        // re-encodes it using the native narrow encoding, i.e. the ANSI code
        // page -- exactly what fopen()/std::ifstream/LibreDWG expect.
#if defined(__cpp_char8_t)
        const auto* utf8Chars = reinterpret_cast<const char8_t*>(utf8Path.c_str());
        return std::filesystem::path(utf8Chars).string();
#else
        return std::filesystem::u8path(utf8Path).string();
#endif
    }
    catch (...) {
        return utf8Path;
    }
#else
    return utf8Path;
#endif
}

}  // namespace SWDWG
