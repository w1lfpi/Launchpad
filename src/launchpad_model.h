#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace launchpad {

inline constexpr std::size_t kGridColumns = 7;
inline constexpr std::size_t kGridRows = 5;
inline constexpr std::size_t kPageCapacity = kGridColumns * kGridRows;
inline constexpr int kNoSearchMatch = -1;

std::wstring lowercase(std::wstring_view value);
std::wstring normalize_search_text(std::wstring_view value);
bool contains_insensitive(std::wstring_view text, std::wstring_view query);
int search_match_score(
    std::wstring_view text,
    std::wstring_view query);
bool is_supported_app_extension(std::wstring_view extension);
std::size_t page_count(std::size_t item_count);
bool should_show_page_indicator(std::size_t item_count);

} // namespace launchpad
