#pragma once

#include <string>
#include <string_view>

// The names chirami writes under HKCU\Software\Classes when registering
// file associations. They stay in the registry of everyone who registered,
// so the mapping must not drift; the tests pin it. See DESIGN.md,
// "File association".
//
// Nothing here includes framework.h, so the tests can link it.

// ".jpg" -> "JPG", the label the registered type name is built from.
std::wstring ExtensionLabel(std::wstring_view extension);

// ".jpg" -> "chirami.AssocFile.JPG"
std::wstring ProgIdForExtension(std::wstring_view extension);

// True for a class name chirami created, which is what makes cleanup safe
// to run over every key under Software\Classes.
bool IsChiramiProgId(std::wstring_view name);
