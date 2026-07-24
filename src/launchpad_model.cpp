#include "launchpad_model.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cwctype>

namespace launchpad {

std::wstring lowercase(std::wstring_view value) {
    std::wstring result(value);
#ifdef _WIN32
    if (!result.empty()) {
        LCMapStringEx(
            LOCALE_NAME_INVARIANT,
            LCMAP_LOWERCASE,
            result.data(),
            static_cast<int>(result.size()),
            result.data(),
            static_cast<int>(result.size()),
            nullptr,
            nullptr,
            0);
    }
#else
    std::transform(
        result.begin(),
        result.end(),
        result.begin(),
        [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
#endif
    return result;
}

bool contains_insensitive(
    std::wstring_view text,
    std::wstring_view query) {
    if (query.empty()) {
        return true;
    }
    return lowercase(text).find(lowercase(query)) != std::wstring::npos;
}

bool is_supported_app_extension(std::wstring_view extension) {
    const std::wstring normalized = lowercase(extension);
    return normalized == L".lnk" ||
        normalized == L".url" ||
        normalized == L".appref-ms" ||
        normalized == L".exe";
}

std::size_t page_count(std::size_t item_count) {
    return std::max<std::size_t>(
        1,
        (item_count + kPageCapacity - 1) / kPageCapacity);
}

bool should_show_page_indicator(std::size_t item_count) {
    return page_count(item_count) > 1;
}

} // namespace launchpad
