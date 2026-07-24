#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace launchpad {

enum class LayoutItemKind {
    app,
    folder,
    page_break,
};

struct LayoutItem {
    LayoutItemKind kind = LayoutItemKind::app;
    std::wstring name;
    std::wstring app_path;
    std::vector<std::wstring> children;

    bool operator==(const LayoutItem&) const = default;
};

class LayoutDocument {
public:
    const std::vector<LayoutItem>& items() const noexcept;
    std::vector<LayoutItem>& items() noexcept;

    bool reconcile(
        const std::vector<std::pair<std::wstring, std::wstring>>& apps);
    bool move_item(std::size_t source, std::size_t destination);
    bool move_item_to_new_page(std::size_t source);
    bool create_folder(
        std::size_t source_app,
        std::size_t target_app,
        std::wstring name);
    bool add_app_to_folder(
        std::size_t source_app,
        std::size_t target_folder);
    bool move_folder_app(
        std::size_t folder,
        std::size_t source,
        std::size_t destination);
    bool extract_folder_app(
        std::size_t folder,
        std::size_t source,
        const std::vector<std::pair<std::wstring, std::wstring>>& apps);

private:
    bool normalize_page_breaks();

    std::vector<LayoutItem> items_;
};

bool load_layout(
    const std::filesystem::path& path,
    LayoutDocument& document);
bool save_layout(
    const std::filesystem::path& path,
    const LayoutDocument& document);

} // namespace launchpad
