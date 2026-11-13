#pragma once

// Screen text: English source in code, Korean in the table in Strings.cpp (same approach as the engine editor's tr()). The language comes from the language line in editor settings.
#include <string>

namespace mkqa {

struct Language {
    const char* code; // editor_settings.txt value
    const char* name; // in its own language, so anyone finds theirs in the list whatever the current language is
};
inline constexpr Language kLanguages[] = {{"ko", "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4"}, {"en", "English"}}; // 한국어

void setLanguage(const std::string& language); // "ko", "en"
const char* language();                        // "ko" or "en"
const char* tr(const char* english);
std::string trf(const char* english, ...); // translates, then fills in as a printf format

} // namespace mkqa
