#pragma once

#include <filesystem>
#include <string>

enum class SortKey
{
    Name,  // natural filename order (StrCmpLogicalW)
    Date,  // last write time
    Size,  // file size
};

// Persistent user settings. Stored as an INI file under %APPDATA% (roams
// with the profile, which is intended); never in the registry.
struct Settings
{
    // "", "ja", or "en"; empty follows the OS UI language.
    std::wstring language;

    SortKey sortKey = SortKey::Name;
    bool sortDescending = false;

    static std::filesystem::path FilePath();  // %APPDATA%\chirami\chirami.ini
    static Settings Load();
    void Save() const;
};
