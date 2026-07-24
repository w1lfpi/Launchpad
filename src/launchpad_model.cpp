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
#include <cstdint>
#include <cwctype>
#include <vector>

namespace {

constexpr int kExactScore = 100000;
constexpr int kPrefixScore = 95000;
constexpr int kWordPrefixScore = 90000;
constexpr int kSubstringScore = 85000;
constexpr int kAcronymScore = 82000;
constexpr int kSubsequenceScore = 70000;
constexpr int kNgramScore = 52000;

bool is_search_separator(wchar_t character) {
#ifdef _WIN32
    WORD character_type = 0;
    if (GetStringTypeW(
            CT_CTYPE1,
            &character,
            1,
            &character_type)) {
        return (character_type &
                (C1_SPACE | C1_PUNCT | C1_CNTRL | C1_BLANK)) != 0;
    }
#endif
    return std::iswspace(character) != 0 ||
        std::iswpunct(character) != 0;
}

std::vector<std::wstring_view> split_search_tokens(
    std::wstring_view value) {
    std::vector<std::wstring_view> result;
    std::size_t offset = 0;
    while (offset < value.size()) {
        while (offset < value.size() && value[offset] == L' ') {
            ++offset;
        }
        const std::size_t start = offset;
        while (offset < value.size() && value[offset] != L' ') {
            ++offset;
        }
        if (start != offset) {
            result.push_back(value.substr(start, offset - start));
        }
    }
    return result;
}

int length_penalty(std::size_t length) {
    return static_cast<int>(
        std::min<std::size_t>(length, 2000));
}

int basic_phrase_score(
    std::wstring_view text,
    std::wstring_view query) {
    if (query.empty()) {
        return 0;
    }
    if (text == query) {
        return kExactScore;
    }
    if (text.starts_with(query)) {
        return kPrefixScore -
            length_penalty(text.size() - query.size());
    }

    std::size_t position = text.find(query);
    if (position == std::wstring_view::npos) {
        return launchpad::kNoSearchMatch;
    }
    const int position_penalty = length_penalty(position);
    if (position == 0 || text[position - 1] == L' ') {
        return kWordPrefixScore - position_penalty;
    }
    return kSubstringScore - position_penalty;
}

std::wstring search_acronym(std::wstring_view text) {
    std::wstring result;
    bool at_word_start = true;
    for (const wchar_t character : text) {
        if (character == L' ') {
            at_word_start = true;
        } else if (at_word_start) {
            result.push_back(character);
            at_word_start = false;
        }
    }
    return result;
}

int acronym_score(
    std::wstring_view text,
    std::wstring_view query) {
    if (query.empty() || query.find(L' ') != std::wstring_view::npos) {
        return launchpad::kNoSearchMatch;
    }
    const std::wstring acronym = search_acronym(text);
    if (acronym == query) {
        return kAcronymScore;
    }
    if (acronym.size() > query.size() &&
        acronym.starts_with(query)) {
        return kAcronymScore - 1000 -
            length_penalty(acronym.size() - query.size());
    }
    return launchpad::kNoSearchMatch;
}

int ordered_subsequence_score(
    std::wstring_view text,
    std::wstring_view query) {
    if (query.size() < 3 || text.empty()) {
        return launchpad::kNoSearchMatch;
    }

    std::size_t cursor = 0;
    std::size_t first = std::wstring_view::npos;
    std::size_t previous = std::wstring_view::npos;
    std::size_t last = 0;
    int adjacent_matches = 0;
    int word_start_matches = 0;

    for (const wchar_t wanted : query) {
        while (cursor < text.size() &&
               (text[cursor] == L' ' || text[cursor] != wanted)) {
            ++cursor;
        }
        if (cursor == text.size()) {
            return launchpad::kNoSearchMatch;
        }
        if (first == std::wstring_view::npos) {
            first = cursor;
        }
        if (previous != std::wstring_view::npos &&
            cursor == previous + 1) {
            ++adjacent_matches;
        }
        if (cursor == 0 || text[cursor - 1] == L' ') {
            ++word_start_matches;
        }
        previous = cursor;
        last = cursor;
        ++cursor;
    }

    const std::size_t span = last - first + 1;
    const std::size_t maximum_span =
        query.size() * 4;
    if (span > maximum_span &&
        word_start_matches <
            static_cast<int>(query.size())) {
        return launchpad::kNoSearchMatch;
    }

    const std::size_t gaps = span - query.size();
    return kSubsequenceScore +
        adjacent_matches * 500 +
        word_start_matches * 350 -
        length_penalty(gaps * 70) -
        length_penalty(first * 2);
}

std::vector<std::uint64_t> search_bigrams(
    std::wstring_view value) {
    std::vector<std::uint64_t> result;
    if (value.size() < 2) {
        return result;
    }
    result.reserve(value.size() - 1);
    for (std::size_t index = 1; index < value.size(); ++index) {
        const auto first = static_cast<std::uint32_t>(
            value[index - 1]);
        const auto second = static_cast<std::uint32_t>(
            value[index]);
        result.push_back(
            (static_cast<std::uint64_t>(first) << 32U) |
            second);
    }
    std::ranges::sort(result);
    return result;
}

int ngram_score(
    std::wstring_view text,
    std::wstring_view query) {
    if (query.size() < 3 || text.size() < 2) {
        return launchpad::kNoSearchMatch;
    }
    const std::vector<std::uint64_t> text_bigrams =
        search_bigrams(text);
    const std::vector<std::uint64_t> query_bigrams =
        search_bigrams(query);
    std::size_t text_index = 0;
    std::size_t query_index = 0;
    std::size_t intersection = 0;
    while (text_index < text_bigrams.size() &&
           query_index < query_bigrams.size()) {
        if (text_bigrams[text_index] ==
            query_bigrams[query_index]) {
            ++intersection;
            ++text_index;
            ++query_index;
        } else if (
            text_bigrams[text_index] <
            query_bigrams[query_index]) {
            ++text_index;
        } else {
            ++query_index;
        }
    }
    const std::size_t denominator =
        text_bigrams.size() + query_bigrams.size();
    if (denominator == 0) {
        return launchpad::kNoSearchMatch;
    }
    const int dice_per_mille = static_cast<int>(
        2000 * intersection / denominator);
    const int threshold =
        query.size() == 3 ? 650 :
        query.size() == 4 ? 550 :
        450;
    if (dice_per_mille < threshold) {
        return launchpad::kNoSearchMatch;
    }
    return kNgramScore + dice_per_mille * 15 -
        length_penalty(
            text.size() > query.size()
                ? text.size() - query.size()
                : query.size() - text.size());
}

int best_term_score(
    std::wstring_view text,
    const std::vector<std::wstring_view>& text_tokens,
    std::wstring_view query) {
    int best = basic_phrase_score(text, query);
    best = std::max(best, acronym_score(text, query));

    if (query.size() < 3) {
        return best;
    }

    best = std::max(
        best,
        ordered_subsequence_score(text, query));
    for (const std::wstring_view token : text_tokens) {
        best = std::max(
            best,
            ordered_subsequence_score(token, query));
        best = std::max(best, ngram_score(token, query));
    }
    return best;
}

} // namespace

namespace launchpad {

std::wstring lowercase(std::wstring_view value) {
#ifdef _WIN32
    if (!value.empty()) {
        const int required = LCMapStringEx(
            LOCALE_NAME_INVARIANT,
            LCMAP_LOWERCASE,
            value.data(),
            static_cast<int>(value.size()),
            nullptr,
            0,
            nullptr,
            nullptr,
            0);
        if (required > 0) {
            std::wstring result(
                static_cast<std::size_t>(required),
                L'\0');
            const int written = LCMapStringEx(
                LOCALE_NAME_INVARIANT,
                LCMAP_LOWERCASE,
                value.data(),
                static_cast<int>(value.size()),
                result.data(),
                required,
                nullptr,
                nullptr,
                0);
            if (written > 0) {
                result.resize(
                    static_cast<std::size_t>(written));
                return result;
            }
        }
    }
    return std::wstring(value);
#else
    std::wstring result(value);
    std::transform(
        result.begin(),
        result.end(),
        result.begin(),
        [](wchar_t character) {
            if (character >= L'A' && character <= L'Z') {
                return static_cast<wchar_t>(
                    character + (L'a' - L'A'));
            }
            if (character >= L'А' && character <= L'Я') {
                return static_cast<wchar_t>(
                    character + (L'а' - L'А'));
            }
            if (character == L'Ё') {
                return L'ё';
            }
            return static_cast<wchar_t>(std::towlower(character));
        });
    return result;
#endif
}

std::wstring normalize_search_text(std::wstring_view value) {
    const std::wstring folded = lowercase(value);
    std::wstring result;
    result.reserve(folded.size());
    bool pending_separator = false;
    for (const wchar_t character : folded) {
        if (is_search_separator(character)) {
            pending_separator = !result.empty();
            continue;
        }
        if (pending_separator) {
            result.push_back(L' ');
            pending_separator = false;
        }
        result.push_back(character);
    }
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

int search_match_score(
    std::wstring_view text,
    std::wstring_view query) {
    const std::wstring normalized_text =
        normalize_search_text(text);
    const std::wstring normalized_query =
        normalize_search_text(query);
    if (normalized_query.empty()) {
        return 0;
    }
    if (normalized_text.empty()) {
        return kNoSearchMatch;
    }

    const std::vector<std::wstring_view> text_tokens =
        split_search_tokens(normalized_text);
    const std::vector<std::wstring_view> query_tokens =
        split_search_tokens(normalized_query);

    int best = basic_phrase_score(
        normalized_text,
        normalized_query);
    if (query_tokens.size() == 1) {
        return std::max(
            best,
            best_term_score(
                normalized_text,
                text_tokens,
                query_tokens.front()));
    }

    long long total = 0;
    for (const std::wstring_view query_token : query_tokens) {
        const int token_score = best_term_score(
            normalized_text,
            text_tokens,
            query_token);
        if (token_score == kNoSearchMatch) {
            return kNoSearchMatch;
        }
        total += token_score;
    }
    const int token_average = static_cast<int>(
        total / static_cast<long long>(query_tokens.size()));
    return std::max(best, token_average - 1000);
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
