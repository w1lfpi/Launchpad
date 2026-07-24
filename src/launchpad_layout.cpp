#include "launchpad_layout.h"

#include "launchpad_model.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace launchpad {
namespace {

constexpr std::string_view kHeaderV1 = "WindowsLaunchpadLayout/1";
constexpr std::string_view kHeaderV2 = "WindowsLaunchpadLayout/2";

bool normalize_page_break_items(std::vector<LayoutItem>& items) {
    std::vector<LayoutItem> normalized;
    normalized.reserve(items.size());
    bool changed = false;

    for (LayoutItem& item : items) {
        if (item.kind != LayoutItemKind::page_break) {
            normalized.push_back(std::move(item));
            continue;
        }

        if (normalized.empty() ||
            normalized.back().kind == LayoutItemKind::page_break) {
            changed = true;
            continue;
        }
        if (!item.name.empty() ||
            !item.app_path.empty() ||
            !item.children.empty()) {
            changed = true;
        }
        normalized.push_back(LayoutItem{
            .kind = LayoutItemKind::page_break,
        });
    }

    if (!normalized.empty() &&
        normalized.back().kind == LayoutItemKind::page_break) {
        normalized.pop_back();
        changed = true;
    }

    items = std::move(normalized);
    return changed;
}

std::string encode_wide(std::wstring_view value) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size() * 8);
    for (const wchar_t character : value) {
        const auto code = static_cast<std::uint32_t>(character);
        for (int shift = 28; shift >= 0; shift -= 4) {
            result.push_back(digits[(code >> shift) & 0x0FU]);
        }
    }
    return result;
}

bool decode_wide(std::string_view value, std::wstring& result) {
    if (value.size() % 8 != 0) {
        return false;
    }
    result.clear();
    result.reserve(value.size() / 8);
    for (std::size_t offset = 0; offset < value.size(); offset += 8) {
        std::uint32_t code = 0;
        const char* first = value.data() + offset;
        const char* last = first + 8;
        const auto parsed = std::from_chars(first, last, code, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != last) {
            return false;
        }
        result.push_back(static_cast<wchar_t>(code));
    }
    return true;
}

std::vector<std::string_view> split_tabs(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (start <= line.size()) {
        const std::size_t separator = line.find('\t', start);
        if (separator == std::string_view::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, separator - start));
        start = separator + 1;
    }
    return fields;
}

} // namespace

const std::vector<LayoutItem>& LayoutDocument::items() const noexcept {
    return items_;
}

std::vector<LayoutItem>& LayoutDocument::items() noexcept {
    return items_;
}

bool LayoutDocument::normalize_page_breaks() {
    return normalize_page_break_items(items_);
}

bool LayoutDocument::reconcile(
    const std::vector<std::pair<std::wstring, std::wstring>>& apps) {
    std::unordered_map<std::wstring, std::pair<std::wstring, std::wstring>>
        available;
    available.reserve(apps.size());
    for (const auto& [path, name] : apps) {
        available.emplace(
            lowercase(path),
            std::pair<std::wstring, std::wstring>{path, name});
    }

    bool changed = false;
    std::unordered_set<std::wstring> placed;
    placed.reserve(apps.size());
    std::vector<LayoutItem> reconciled;
    reconciled.reserve(items_.size() + apps.size());

    for (LayoutItem item : items_) {
        if (item.kind == LayoutItemKind::page_break) {
            reconciled.push_back(LayoutItem{
                .kind = LayoutItemKind::page_break,
            });
            continue;
        }

        if (item.kind == LayoutItemKind::app) {
            const auto found = available.find(lowercase(item.app_path));
            if (found == available.end() ||
                !placed.insert(found->first).second) {
                changed = true;
                continue;
            }
            if (item.app_path != found->second.first ||
                item.name != found->second.second) {
                item.app_path = found->second.first;
                item.name = found->second.second;
                changed = true;
            }
            reconciled.push_back(std::move(item));
            continue;
        }

        std::vector<std::wstring> children;
        children.reserve(item.children.size());
        for (const std::wstring& child : item.children) {
            const auto found = available.find(lowercase(child));
            if (found == available.end() ||
                !placed.insert(found->first).second) {
                changed = true;
                continue;
            }
            children.push_back(found->second.first);
            if (children.back() != child) {
                changed = true;
            }
        }
        if (children.empty()) {
            changed = true;
            continue;
        }
        if (children.size() == 1) {
            const auto found = available.find(lowercase(children.front()));
            if (found != available.end()) {
                reconciled.push_back(LayoutItem{
                    .kind = LayoutItemKind::app,
                    .name = found->second.second,
                    .app_path = found->second.first,
                });
            }
            changed = true;
            continue;
        }
        if (children != item.children) {
            item.children = std::move(children);
            changed = true;
        }
        reconciled.push_back(std::move(item));
    }

    for (const auto& [path, name] : apps) {
        if (!placed.insert(lowercase(path)).second) {
            continue;
        }
        reconciled.push_back(LayoutItem{
            .kind = LayoutItemKind::app,
            .name = name,
            .app_path = path,
        });
        changed = true;
    }

    changed =
        normalize_page_break_items(reconciled) ||
        changed;
    if (reconciled != items_) {
        items_ = std::move(reconciled);
        return true;
    }
    return changed;
}

bool LayoutDocument::move_item(
    std::size_t source,
    std::size_t destination) {
    if (source >= items_.size() || destination >= items_.size() ||
        source == destination ||
        items_[source].kind == LayoutItemKind::page_break ||
        items_[destination].kind == LayoutItemKind::page_break) {
        return false;
    }
    const std::vector<LayoutItem> previous = items_;
    LayoutItem moving = std::move(items_[source]);
    items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(source));
    destination = std::min(destination, items_.size());
    items_.insert(
        items_.begin() + static_cast<std::ptrdiff_t>(destination),
        std::move(moving));
    normalize_page_breaks();
    return items_ != previous;
}

bool LayoutDocument::move_item_to_new_page(std::size_t source) {
    if (source >= items_.size() ||
        items_[source].kind == LayoutItemKind::page_break) {
        return false;
    }

    const std::vector<LayoutItem> previous = items_;
    LayoutItem moving = std::move(items_[source]);
    items_.erase(
        items_.begin() + static_cast<std::ptrdiff_t>(source));
    normalize_page_breaks();
    if (!items_.empty()) {
        items_.push_back(LayoutItem{
            .kind = LayoutItemKind::page_break,
        });
    }
    items_.push_back(std::move(moving));
    normalize_page_breaks();
    return items_ != previous;
}

bool LayoutDocument::create_folder(
    std::size_t source_app,
    std::size_t target_app,
    std::wstring name) {
    if (source_app >= items_.size() ||
        target_app >= items_.size() ||
        source_app == target_app ||
        items_[source_app].kind != LayoutItemKind::app ||
        items_[target_app].kind != LayoutItemKind::app) {
        return false;
    }

    const std::wstring source_path = items_[source_app].app_path;
    const std::wstring target_path = items_[target_app].app_path;
    const std::vector<LayoutItem> previous = items_;
    items_.erase(
        items_.begin() + static_cast<std::ptrdiff_t>(source_app));
    if (target_app > source_app) {
        --target_app;
    }
    items_[target_app] = LayoutItem{
        .kind = LayoutItemKind::folder,
        .name = std::move(name),
        .children = {target_path, source_path},
    };
    normalize_page_breaks();
    return items_ != previous;
}

bool LayoutDocument::add_app_to_folder(
    std::size_t source_app,
    std::size_t target_folder) {
    if (source_app >= items_.size() ||
        target_folder >= items_.size() ||
        source_app == target_folder ||
        items_[source_app].kind != LayoutItemKind::app ||
        items_[target_folder].kind != LayoutItemKind::folder) {
        return false;
    }
    const std::wstring path = items_[source_app].app_path;
    items_.erase(
        items_.begin() + static_cast<std::ptrdiff_t>(source_app));
    if (target_folder > source_app) {
        --target_folder;
    }
    items_[target_folder].children.push_back(path);
    normalize_page_breaks();
    return true;
}

bool LayoutDocument::move_folder_app(
    std::size_t folder,
    std::size_t source,
    std::size_t destination) {
    if (folder >= items_.size() ||
        items_[folder].kind != LayoutItemKind::folder) {
        return false;
    }
    auto& children = items_[folder].children;
    if (source >= children.size() ||
        destination >= children.size() ||
        source == destination) {
        return false;
    }
    std::wstring moving = std::move(children[source]);
    children.erase(
        children.begin() + static_cast<std::ptrdiff_t>(source));
    destination = std::min(destination, children.size());
    children.insert(
        children.begin() + static_cast<std::ptrdiff_t>(destination),
        std::move(moving));
    return true;
}

bool LayoutDocument::extract_folder_app(
    std::size_t folder,
    std::size_t source,
    const std::vector<std::pair<std::wstring, std::wstring>>& apps) {
    if (folder >= items_.size() ||
        items_[folder].kind != LayoutItemKind::folder ||
        source >= items_[folder].children.size()) {
        return false;
    }

    const auto find_app =
        [&apps](std::wstring_view path)
        -> const std::pair<std::wstring, std::wstring>* {
        const std::wstring normalized_path = lowercase(path);
        const auto found = std::ranges::find_if(
            apps,
            [&normalized_path](const auto& app) {
                return lowercase(app.first) == normalized_path;
            });
        return found == apps.end() ? nullptr : &*found;
    };

    const std::wstring selected_path =
        items_[folder].children[source];
    const auto* selected_app = find_app(selected_path);
    if (!selected_app) {
        return false;
    }

    const std::size_t remaining_count =
        items_[folder].children.size() - 1;
    const std::pair<std::wstring, std::wstring>* remaining_app =
        nullptr;
    if (remaining_count == 1) {
        const std::size_t remaining_index = source == 0 ? 1 : 0;
        remaining_app =
            find_app(items_[folder].children[remaining_index]);
        if (!remaining_app) {
            return false;
        }
    }

    auto& children = items_[folder].children;
    children.erase(
        children.begin() + static_cast<std::ptrdiff_t>(source));

    LayoutItem extracted{
        .kind = LayoutItemKind::app,
        .name = selected_app->second,
        .app_path = selected_app->first,
    };
    if (children.empty()) {
        items_[folder] = std::move(extracted);
    } else if (children.size() == 1) {
        items_[folder] = LayoutItem{
            .kind = LayoutItemKind::app,
            .name = remaining_app->second,
            .app_path = remaining_app->first,
        };
        items_.insert(
            items_.begin() +
                static_cast<std::ptrdiff_t>(folder + 1),
            std::move(extracted));
    } else {
        items_.insert(
            items_.begin() +
                static_cast<std::ptrdiff_t>(folder + 1),
            std::move(extracted));
    }
    normalize_page_breaks();
    return true;
}

bool load_layout(
    const std::filesystem::path& path,
    LayoutDocument& document) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return false;
    }
    std::string line;
    if (!std::getline(stream, line)) {
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    const bool version_1 = line == kHeaderV1;
    const bool version_2 = line == kHeaderV2;
    if (!version_1 && !version_2) {
        return false;
    }

    LayoutDocument loaded;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto fields = split_tabs(line);
        if (fields.empty()) {
            continue;
        }
        if (version_2 &&
            fields[0] == "P" &&
            fields.size() == 1) {
            loaded.items().push_back(LayoutItem{
                .kind = LayoutItemKind::page_break,
            });
        } else if (fields[0] == "A" && fields.size() == 3) {
            LayoutItem item;
            item.kind = LayoutItemKind::app;
            if (!decode_wide(fields[1], item.app_path) ||
                !decode_wide(fields[2], item.name)) {
                return false;
            }
            loaded.items().push_back(std::move(item));
        } else if (fields[0] == "F" && fields.size() >= 3) {
            LayoutItem item;
            item.kind = LayoutItemKind::folder;
            if (!decode_wide(fields[1], item.name)) {
                return false;
            }
            for (std::size_t index = 2; index < fields.size(); ++index) {
                std::wstring child;
                if (!decode_wide(fields[index], child)) {
                    return false;
                }
                item.children.push_back(std::move(child));
            }
            loaded.items().push_back(std::move(item));
        } else {
            return false;
        }
    }
    normalize_page_break_items(loaded.items());
    document = std::move(loaded);
    return true;
}

bool save_layout(
    const std::filesystem::path& path,
    const LayoutDocument& document) {
    const std::filesystem::path temporary =
        std::filesystem::path(path.wstring() + L".tmp");
    std::ofstream stream(
        temporary,
        std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    stream << kHeaderV2 << '\n';
    for (const LayoutItem& item : document.items()) {
        if (item.kind == LayoutItemKind::app) {
            stream << "A\t"
                   << encode_wide(item.app_path) << '\t'
                   << encode_wide(item.name) << '\n';
        } else if (item.kind == LayoutItemKind::folder) {
            stream << "F\t" << encode_wide(item.name);
            for (const std::wstring& child : item.children) {
                stream << '\t' << encode_wide(child);
            }
            stream << '\n';
        } else {
            stream << "P\n";
        }
    }
    stream.flush();
    if (!stream) {
        return false;
    }
    stream.close();

#ifdef _WIN32
    if (MoveFileExW(
            temporary.c_str(),
            path.c_str(),
            MOVEFILE_REPLACE_EXISTING |
                MOVEFILE_WRITE_THROUGH) != FALSE) {
        return true;
    }
    std::error_code cleanup_error;
    std::filesystem::remove(temporary, cleanup_error);
    return false;
#else
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (!error) {
        return true;
    }
    std::error_code cleanup_error;
    std::filesystem::remove(temporary, cleanup_error);
    return false;
#endif
}

} // namespace launchpad
