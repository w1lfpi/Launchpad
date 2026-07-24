#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "launchpad_model.h"
#include "launchpad_layout.h"
#include "resource.h"

#include <windows.h>
#include <windowsx.h>

#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

constexpr wchar_t kWindowClass[] = L"WindowsLaunchpad.Window";
constexpr wchar_t kAppUserModelId[] =
    L"DaniilGorchakov.WindowsLaunchpad";
constexpr wchar_t kShortcutName[] = L"Launchpad.lnk";
constexpr UINT_PTR kAnimationTimer = 1;
constexpr UINT_PTR kExternalDropRescanTimer = 2;
constexpr UINT_PTR kApplicationsWatchTimer = 3;
constexpr int kGlobalHotkeyId = 1;
constexpr UINT kShowLaunchpadMessage = WM_APP + 42;
constexpr UINT kExternalDropCompletedMessage = WM_APP + 43;
constexpr UINT kExternalDropRescanDelayMs = 160;
constexpr int kExternalDropRescanAttempts = 20;
constexpr UINT kApplicationsWatchIntervalMs = 800;
constexpr int kApplicationsRemovalConfirmations = 3;
constexpr wchar_t kCatalogDirectoryName[] = L"WindowsLaunchpad";
constexpr wchar_t kCatalogMarkerName[] = L".catalog-initialized";
constexpr UINT kBaseIconRequestPixels = 256;
constexpr float kLargestIconSlotDips = 112.0F;
constexpr float kSwipeThresholdDips = 56.0F;
constexpr float kClickSlopDips = 8.0F;
constexpr double kOpenAnimationSeconds = 0.36;
constexpr double kCloseAnimationSeconds = 0.20;
constexpr float kPageSettleSpeed = 1.10F;
constexpr double kPageSettleMaxSeconds =
    1.20 / kPageSettleSpeed;
constexpr float kPageSettleOmega =
    8.0F * kPageSettleSpeed;
constexpr float kPageSettleDistanceDips = 1.5F;
constexpr float kPageSettleVelocityDipsPerSecond =
    12.0F * kPageSettleSpeed;
constexpr float kPageFlickVelocityDipsPerSecond = 650.0F;
constexpr float kPageMaxVelocityPagesPerSecond = 1.6F;
constexpr float kIconCornerRatio = 0.225F;
constexpr double kFolderAnimationSeconds = 0.28;
constexpr double kFolderDropAnimationSeconds = 0.26;
constexpr double kRootReflowAnimationSeconds = 0.30;
constexpr double kLiveReorderAnimationSeconds = 0.20;
constexpr double kSearchFocusAnimationSeconds = 0.22;
constexpr double kSearchCaretBlinkSeconds = 1.00;
constexpr double kSearchCaretRevealDelaySeconds = 0.35;
constexpr double kSearchCaretRevealSeconds = 0.23;
constexpr double kFolderHoverActivationSeconds = 0.55;
constexpr double kLongPressSeconds = 0.34;
constexpr float kDragSlopDips = 6.0F;
constexpr float kDragEdgeZoneDips = 48.0F;
constexpr double kDragEdgeHoverSeconds = 0.48;
constexpr float kFolderExtractionMarginDips = 18.0F;
constexpr DWORD kHighResolutionTimerFlag = 0x00000002;
constexpr std::size_t kFolderColumns = 7;
constexpr std::size_t kFolderRows = 4;
constexpr std::size_t kFolderPageCapacity =
    kFolderColumns * kFolderRows;

struct AppEntry {
    std::wstring name;
    std::wstring path;
    std::uint32_t color = 0;
    wchar_t glyph = L'?';
    ComPtr<ID2D1Bitmap> icon;
    bool icon_attempted = false;
};

struct HitRegion {
    D2D1_RECT_F bounds{};
    D2D1_RECT_F icon_bounds{};
    std::size_t visible_position = 0;
};

struct PageDotRegion {
    D2D1_RECT_F bounds{};
    std::size_t page = 0;
};

struct PageDragSample {
    std::int64_t counter = 0;
    float offset = 0.0F;
};

struct PageDragVisual {
    int direction = 1;
    std::size_t neighbor =
        std::numeric_limits<std::size_t>::max();
    float offset = 0.0F;
    float velocity_scale = 1.0F;
};

enum class VisibleItemKind {
    app,
    folder,
};

enum class DeleteModalButton {
    none,
    cancel,
    confirm,
};

struct DeleteModalGeometry {
    D2D1_ROUNDED_RECT panel{};
    D2D1_ROUNDED_RECT cancel_button{};
    D2D1_ROUNDED_RECT confirm_button{};
};

struct FolderGeometry {
    D2D1_ROUNDED_RECT panel{};
    D2D1_RECT_F title{};
    D2D1_ROUNDED_RECT title_editor{};
    float grid_left = 0.0F;
    float horizontal_padding = 0.0F;
    float grid_top = 0.0F;
    float cell_width = 0.0F;
    float cell_height = 0.0F;
    float icon_size = 0.0F;
    std::size_t visible_rows = 1;
};

struct VisibleItem {
    VisibleItemKind kind = VisibleItemKind::app;
    std::size_t layout_index = 0;
    std::size_t child_index = 0;
    std::size_t app_index = 0;
};

struct FolderExtractionTransaction {
    launchpad::LayoutDocument snapshot;
    std::wstring app_path;
    std::size_t folder_index = 0;
    std::size_t child_position = 0;
    std::size_t folder_page = 0;
    std::size_t root_page = 0;
};

struct PendingExternalDrop {
    std::unordered_set<std::wstring> previous_paths;
    std::size_t target_page = 0;
    std::size_t target_layout_index =
        std::numeric_limits<std::size_t>::max();
    int attempts_remaining = kExternalDropRescanAttempts;
};

float smooth_step(float value) {
    value = std::clamp(value, 0.0F, 1.0F);
    return value * value * (3.0F - 2.0F * value);
}

float ease_out_cubic(float value) {
    value = std::clamp(value, 0.0F, 1.0F);
    const float inverse = 1.0F - value;
    return 1.0F - inverse * inverse * inverse;
}

float spring_ease_out(float value) {
    value = std::clamp(value, 0.0F, 1.0F);
    if (value <= 0.0F || value >= 1.0F) {
        return value;
    }
    constexpr float damping = 0.78F;
    constexpr float frequency = 1.4F;
    constexpr float pi = 3.14159265358979323846F;
    const float omega = 2.0F * pi * frequency;
    const float root = std::sqrt(1.0F - damping * damping);
    const float omega_d = omega * root;
    const float decay = std::exp(-damping * omega * value);
    return 1.0F - decay *
        (std::cos(omega_d * value) +
         damping / root * std::sin(omega_d * value));
}

void clear_startup_feedback_cursor() {
    MSG message{};
    PeekMessageW(
        &message,
        nullptr,
        WM_USER,
        WM_USER,
        PM_NOREMOVE);
    if (PostThreadMessageW(
            GetCurrentThreadId(),
            WM_NULL,
            0,
            0)) {
        GetMessageW(&message, nullptr, 0, 0);
    }
}

float lerp(float from, float to, float progress) {
    return from + (to - from) * progress;
}

D2D1_RECT_F lerp_rect(
    const D2D1_RECT_F& from,
    const D2D1_RECT_F& to,
    float progress) {
    return D2D1::RectF(
        lerp(from.left, to.left, progress),
        lerp(from.top, to.top, progress),
        lerp(from.right, to.right, progress),
        lerp(from.bottom, to.bottom, progress));
}

std::size_t projected_reorder_position(
    std::size_t position,
    std::size_t source,
    std::size_t destination) {
    if (source == destination ||
        source == std::numeric_limits<std::size_t>::max() ||
        destination == std::numeric_limits<std::size_t>::max()) {
        return position;
    }
    if (position == source) {
        return destination;
    }
    if (source < destination &&
        position > source &&
        position <= destination) {
        return position - 1;
    }
    if (destination < source &&
        position >= destination &&
        position < source) {
        return position + 1;
    }
    return position;
}

void box_blur_bgra(
    std::uint32_t* pixels,
    int width,
    int height,
    int radius,
    int passes) {
    if (!pixels || width <= 0 || height <= 0 || radius <= 0) {
        return;
    }

    const auto pack = [](std::uint32_t red,
                         std::uint32_t green,
                         std::uint32_t blue) {
        return 0xFF000000U |
            (red << 16U) |
            (green << 8U) |
            blue;
    };
    std::vector<std::uint32_t> temporary(
        static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height));
    const int window = radius * 2 + 1;

    for (int pass = 0; pass < passes; ++pass) {
        for (int y = 0; y < height; ++y) {
            std::uint32_t red = 0;
            std::uint32_t green = 0;
            std::uint32_t blue = 0;
            for (int offset = -radius; offset <= radius; ++offset) {
                const int x = std::clamp(offset, 0, width - 1);
                const std::uint32_t pixel =
                    pixels[static_cast<std::size_t>(y) * width + x];
                blue += pixel & 0xFFU;
                green += (pixel >> 8U) & 0xFFU;
                red += (pixel >> 16U) & 0xFFU;
            }
            for (int x = 0; x < width; ++x) {
                temporary[static_cast<std::size_t>(y) * width + x] =
                    pack(
                        red / static_cast<std::uint32_t>(window),
                        green / static_cast<std::uint32_t>(window),
                        blue / static_cast<std::uint32_t>(window));

                const int remove_x =
                    std::clamp(x - radius, 0, width - 1);
                const int add_x =
                    std::clamp(x + radius + 1, 0, width - 1);
                const std::uint32_t remove_pixel =
                    pixels[static_cast<std::size_t>(y) * width + remove_x];
                const std::uint32_t add_pixel =
                    pixels[static_cast<std::size_t>(y) * width + add_x];
                blue += (add_pixel & 0xFFU) -
                    (remove_pixel & 0xFFU);
                green += ((add_pixel >> 8U) & 0xFFU) -
                    ((remove_pixel >> 8U) & 0xFFU);
                red += ((add_pixel >> 16U) & 0xFFU) -
                    ((remove_pixel >> 16U) & 0xFFU);
            }
        }

        for (int x = 0; x < width; ++x) {
            std::uint32_t red = 0;
            std::uint32_t green = 0;
            std::uint32_t blue = 0;
            for (int offset = -radius; offset <= radius; ++offset) {
                const int y = std::clamp(offset, 0, height - 1);
                const std::uint32_t pixel =
                    temporary[static_cast<std::size_t>(y) * width + x];
                blue += pixel & 0xFFU;
                green += (pixel >> 8U) & 0xFFU;
                red += (pixel >> 16U) & 0xFFU;
            }
            for (int y = 0; y < height; ++y) {
                pixels[static_cast<std::size_t>(y) * width + x] =
                    pack(
                        red / static_cast<std::uint32_t>(window),
                        green / static_cast<std::uint32_t>(window),
                        blue / static_cast<std::uint32_t>(window));

                const int remove_y =
                    std::clamp(y - radius, 0, height - 1);
                const int add_y =
                    std::clamp(y + radius + 1, 0, height - 1);
                const std::uint32_t remove_pixel =
                    temporary[
                        static_cast<std::size_t>(remove_y) * width + x];
                const std::uint32_t add_pixel =
                    temporary[
                        static_cast<std::size_t>(add_y) * width + x];
                blue += (add_pixel & 0xFFU) -
                    (remove_pixel & 0xFFU);
                green += ((add_pixel >> 8U) & 0xFFU) -
                    ((remove_pixel >> 8U) & 0xFFU);
                red += ((add_pixel >> 16U) & 0xFFU) -
                    ((remove_pixel >> 16U) & 0xFFU);
            }
        }
    }
}

std::uint32_t accent_color(std::wstring_view value) {
    constexpr std::array<std::uint32_t, 12> colors{
        0x2E86DE, 0x7B61D1, 0x35B779, 0xE45D61,
        0xE2AA3A, 0xD85D9A, 0x4E83BA, 0x2CB7BC,
        0x6C7A90, 0x4DA866, 0xA765CF, 0xD8734D,
    };
    std::uint32_t hash = 2166136261U;
    for (const wchar_t character : value) {
        hash ^= static_cast<std::uint32_t>(character);
        hash *= 16777619U;
    }
    return colors[hash % colors.size()];
}

wchar_t first_glyph(std::wstring_view name) {
    for (const wchar_t character : name) {
        if (!std::iswspace(character)) {
            return static_cast<wchar_t>(std::towupper(character));
        }
    }
    return L'?';
}

void add_unique_app(
    std::vector<AppEntry>& apps,
    std::unordered_set<std::wstring>& paths,
    std::wstring name,
    std::wstring path) {
    if (name.empty() || path.empty()) {
        return;
    }
    const std::wstring normalized_path =
        launchpad::lowercase(
            fs::path(path).lexically_normal().wstring());
    if (!paths.insert(normalized_path).second) {
        return;
    }
    const std::wstring normalized_name =
        launchpad::lowercase(name);
    apps.push_back(AppEntry{
        .name = std::move(name),
        .path = std::move(path),
        .color = accent_color(normalized_name),
        .glyph = first_glyph(normalized_name),
    });
}

struct CatalogSetup {
    fs::path applications_directory;
    fs::path layout_path;
    fs::path marker_path;
    fs::path legacy_layout_path;
    fs::path legacy_layout_applications;
    std::vector<fs::path> legacy_applications;
    bool needs_initialization = false;
    bool migration_succeeded = true;
};

std::optional<std::string> utf8_from_wide(
    std::wstring_view value) {
    if (value.empty()) {
        return std::string{};
    }
    const int byte_count = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (byte_count <= 0) {
        return std::nullopt;
    }
    std::string result(static_cast<std::size_t>(byte_count), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            byte_count,
            nullptr,
            nullptr) != byte_count) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::wstring> wide_from_utf8(
    std::string_view value) {
    if (value.empty()) {
        return std::wstring{};
    }
    const int character_count = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (character_count <= 0) {
        return std::nullopt;
    }
    std::wstring result(
        static_cast<std::size_t>(character_count),
        L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            character_count) != character_count) {
        return std::nullopt;
    }
    return result;
}

std::optional<fs::path> catalog_marker_applications_directory(
    const fs::path& marker) {
    const HANDLE file = CreateFileW(
        marker.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) ||
        size.QuadPart <= 0 ||
        size.QuadPart > 131072) {
        CloseHandle(file);
        return std::nullopt;
    }
    std::string bytes(
        static_cast<std::size_t>(size.QuadPart),
        '\0');
    DWORD bytes_read = 0;
    const bool read =
        ReadFile(
            file,
            bytes.data(),
            static_cast<DWORD>(bytes.size()),
            &bytes_read,
            nullptr) != FALSE;
    CloseHandle(file);
    if (!read) {
        return std::nullopt;
    }
    bytes.resize(bytes_read);
    if (bytes.starts_with("\xEF\xBB\xBF")) {
        bytes.erase(0, 3);
    }
    while (!bytes.empty() &&
           (bytes.back() == '\r' ||
            bytes.back() == '\n' ||
            bytes.back() == '\0')) {
        bytes.pop_back();
    }
    const auto stored = wide_from_utf8(bytes);
    if (!stored || stored->empty()) {
        return std::nullopt;
    }
    return fs::path(*stored);
}

bool catalog_marker_matches(
    const fs::path& marker,
    const fs::path& applications_directory) {
    const auto stored =
        catalog_marker_applications_directory(marker);
    return stored &&
        launchpad::lowercase(
            stored->lexically_normal().wstring()) ==
        launchpad::lowercase(
            applications_directory.lexically_normal().wstring());
}

bool mark_catalog_initialized(
    const fs::path& marker,
    const fs::path& applications_directory) {
    if (marker.empty()) {
        return false;
    }
    const auto contents = utf8_from_wide(
        applications_directory.lexically_normal().wstring());
    if (!contents) {
        return false;
    }
    const HANDLE file = CreateFileW(
        marker.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_HIDDEN,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool succeeded =
        WriteFile(
            file,
            contents->data(),
            static_cast<DWORD>(contents->size()),
            &written,
            nullptr) != FALSE &&
        written == contents->size();
    CloseHandle(file);
    return succeeded;
}

std::optional<fs::path> executable_directory() {
    std::array<wchar_t, 32768> executable_buffer{};
    const DWORD length = GetModuleFileNameW(
        nullptr,
        executable_buffer.data(),
        static_cast<DWORD>(executable_buffer.size()));
    if (length == 0 || length >= executable_buffer.size()) {
        return std::nullopt;
    }
    return fs::path(std::wstring_view(
        executable_buffer.data(),
        length)).parent_path();
}

std::optional<fs::path> local_app_data_directory() {
    PWSTR local_app_data = nullptr;
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_LocalAppData,
            KF_FLAG_CREATE,
            nullptr,
            &local_app_data)) ||
        !local_app_data) {
        return std::nullopt;
    }
    fs::path result(local_app_data);
    CoTaskMemFree(local_app_data);
    return result;
}

std::optional<fs::path> desktop_applications_directory() {
    PWSTR desktop = nullptr;
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_Desktop,
            KF_FLAG_CREATE,
            nullptr,
            &desktop)) ||
        !desktop) {
        return std::nullopt;
    }
    fs::path result =
        fs::path(desktop) /
        L"Launchpad Applications";
    CoTaskMemFree(desktop);
    return result;
}

std::optional<fs::path> nearest_applications_directory(
    fs::path cursor) {
    for (int depth = 0; depth < 8 && !cursor.empty(); ++depth) {
        const fs::path candidate = cursor / L"Applications";
        std::error_code error;
        if (fs::is_directory(candidate, error)) {
            return candidate;
        }
        const fs::path parent = cursor.parent_path();
        if (parent == cursor) {
            break;
        }
        cursor = parent;
    }
    return std::nullopt;
}

void add_unique_catalog_directory(
    std::vector<fs::path>& directories,
    const fs::path& candidate,
    const fs::path& canonical) {
    std::error_code error;
    if (!fs::is_directory(candidate, error)) {
        return;
    }
    const std::wstring normalized =
        launchpad::lowercase(
            candidate.lexically_normal().wstring());
    if (normalized ==
        launchpad::lowercase(
            canonical.lexically_normal().wstring())) {
        return;
    }
    const bool duplicate = std::ranges::any_of(
        directories,
        [&normalized](const fs::path& existing) {
            return launchpad::lowercase(
                       existing.lexically_normal().wstring()) ==
                normalized;
        });
    if (!duplicate) {
        directories.push_back(candidate);
    }
}

std::vector<fs::path> legacy_applications_directories(
    const fs::path& canonical,
    const std::optional<fs::path>& local_app_data) {
    std::vector<fs::path> directories;
    if (local_app_data) {
        add_unique_catalog_directory(
            directories,
            *local_app_data /
                kCatalogDirectoryName /
                L"Applications",
            canonical);
        add_unique_catalog_directory(
            directories,
            *local_app_data /
                L"Programs" /
                L"Windows Launchpad" /
                L"Applications",
            canonical);
    }
    if (const auto desktop =
            desktop_applications_directory()) {
        add_unique_catalog_directory(
            directories,
            *desktop,
            canonical);
    }
    if (const auto executable = executable_directory()) {
        if (const auto nearest =
                nearest_applications_directory(*executable)) {
            add_unique_catalog_directory(
                directories,
                *nearest,
                canonical);
        }
    }
    std::error_code current_error;
    if (!current_error) {
        const fs::path current = fs::current_path(current_error);
        if (!current_error) {
            if (const auto nearest =
                    nearest_applications_directory(current)) {
                add_unique_catalog_directory(
                    directories,
                    *nearest,
                    canonical);
            }
        }
    }
    return directories;
}

bool copy_catalog_apps(
    const fs::path& source,
    const fs::path& destination,
    bool overwrite_existing = false) {
    std::error_code error;
    fs::recursive_directory_iterator iterator(
        source,
        fs::directory_options::skip_permission_denied,
        error);
    if (error) {
        return false;
    }
    const fs::recursive_directory_iterator end;
    bool succeeded = true;
    while (iterator != end) {
        if (!error && iterator->is_regular_file(error)) {
            const fs::path source_path = iterator->path();
            if (launchpad::is_supported_app_extension(
                    source_path.extension().wstring())) {
                error.clear();
                const fs::path relative =
                    fs::relative(source_path, source, error);
                if (!error &&
                    !relative.empty() &&
                    !relative.is_absolute()) {
                    const fs::path destination_path =
                        destination / relative;
                    fs::create_directories(
                        destination_path.parent_path(),
                        error);
                    error.clear();
                    fs::copy_file(
                        source_path,
                        destination_path,
                        overwrite_existing
                            ? fs::copy_options::overwrite_existing
                            : fs::copy_options::skip_existing,
                        error);
                    if (error) {
                        succeeded = false;
                    }
                }
            }
        }
        error.clear();
        iterator.increment(error);
        if (error) {
            return false;
        }
    }
    return succeeded;
}

CatalogSetup prepare_applications_catalog() {
    const std::optional<fs::path> local_app_data =
        local_app_data_directory();
    if (!local_app_data) {
        std::error_code error;
        fs::path fallback = fs::current_path(error) / L"Applications";
        if (error) {
            fallback = L"Applications";
        }
        fs::create_directories(fallback, error);
        return CatalogSetup{
            .applications_directory = fallback,
            .layout_path =
                fallback.parent_path() /
                L"LaunchpadLayout.store",
        };
    }

    const fs::path catalog_root =
        *local_app_data / kCatalogDirectoryName;
    std::error_code catalog_error;
    fs::create_directories(catalog_root, catalog_error);

    const fs::path applications =
        catalog_root / L"Applications";
    std::error_code create_error;
    fs::create_directories(applications, create_error);

    if (catalog_error || create_error) {
        if (const auto executable = executable_directory()) {
            if (const auto nearest =
                    nearest_applications_directory(*executable)) {
                return CatalogSetup{
                    .applications_directory = *nearest,
                    .layout_path =
                        nearest->parent_path() /
                        L"LaunchpadLayout.store",
                };
            }
        }
    }

    const fs::path marker =
        catalog_root / kCatalogMarkerName;

    CatalogSetup setup{
        .applications_directory = applications,
        .layout_path =
            catalog_root / L"LaunchpadLayout.store",
        .marker_path = marker,
    };
    setup.needs_initialization =
        !catalog_marker_matches(marker, applications);
    if (!setup.needs_initialization) {
        return setup;
    }

    bool authoritative_catalog_found = false;
    if (const auto previous_catalog =
            catalog_marker_applications_directory(marker);
        previous_catalog &&
        launchpad::lowercase(
            previous_catalog->filename().wstring()) ==
            L"launchpad applications") {
        setup.legacy_applications.push_back(
            *previous_catalog);
        std::error_code previous_error;
        if (fs::is_directory(
                *previous_catalog,
                previous_error) &&
            !previous_error) {
            authoritative_catalog_found = true;
            setup.migration_succeeded =
                copy_catalog_apps(
                    *previous_catalog,
                    applications,
                    true);
        }
    }
    if (!authoritative_catalog_found) {
        const std::vector<fs::path> legacy_directories =
            legacy_applications_directories(
                applications,
                local_app_data);
        for (const fs::path& legacy : legacy_directories) {
            const std::wstring normalized =
                launchpad::lowercase(
                    legacy.lexically_normal().wstring());
            const bool already_added =
                std::ranges::any_of(
                    setup.legacy_applications,
                    [&normalized](const fs::path& existing) {
                        return launchpad::lowercase(
                                   existing.lexically_normal()
                                       .wstring()) ==
                            normalized;
                    });
            if (!already_added) {
                setup.legacy_applications.push_back(legacy);
            }
            setup.migration_succeeded =
                copy_catalog_apps(legacy, applications) &&
                setup.migration_succeeded;
        }
    }

    std::error_code layout_error;
    if (!fs::is_regular_file(setup.layout_path, layout_error)) {
        for (const fs::path& legacy :
             setup.legacy_applications) {
            const fs::path candidate =
                legacy.parent_path() /
                L"LaunchpadLayout.store";
            layout_error.clear();
            if (fs::is_regular_file(candidate, layout_error)) {
                setup.legacy_layout_path = candidate;
                setup.legacy_layout_applications = legacy;
                break;
            }
        }
    }
    return setup;
}

bool remap_catalog_path(
    std::wstring& value,
    const fs::path& source,
    const fs::path& destination) {
    const std::wstring original =
        fs::path(value).lexically_normal().wstring();
    std::wstring source_prefix =
        source.lexically_normal().wstring();
    if (!source_prefix.empty() &&
        source_prefix.back() != L'\\' &&
        source_prefix.back() != L'/') {
        source_prefix.push_back(fs::path::preferred_separator);
    }
    const std::wstring normalized_original =
        launchpad::lowercase(original);
    const std::wstring normalized_prefix =
        launchpad::lowercase(source_prefix);
    if (!normalized_original.starts_with(normalized_prefix)) {
        return false;
    }
    const fs::path relative(
        original.substr(source_prefix.size()));
    value = (destination / relative)
        .lexically_normal()
        .wstring();
    return true;
}

bool remap_catalog_layout(
    launchpad::LayoutDocument& layout,
    const fs::path& source,
    const fs::path& destination) {
    bool changed = false;
    for (launchpad::LayoutItem& item : layout.items()) {
        if (item.kind == launchpad::LayoutItemKind::app) {
            changed =
                remap_catalog_path(
                    item.app_path,
                    source,
                    destination) ||
                changed;
            continue;
        }
        if (item.kind != launchpad::LayoutItemKind::folder) {
            continue;
        }
        for (std::wstring& child : item.children) {
            changed =
                remap_catalog_path(
                    child,
                    source,
                    destination) ||
                changed;
        }
    }
    return changed;
}

bool scan_applications_directory(
    const fs::path& root,
    std::vector<AppEntry>& apps,
    std::unordered_set<std::wstring>& paths) {
    std::error_code error;
    if (!fs::is_directory(root, error) || error) {
        return false;
    }
    fs::recursive_directory_iterator iterator(
        root,
        fs::directory_options::skip_permission_denied,
        error);
    if (error) {
        return false;
    }
    const fs::recursive_directory_iterator end;
    while (iterator != end) {
        if (iterator->is_regular_file(error)) {
            const fs::path path = iterator->path();
            if (launchpad::is_supported_app_extension(
                    path.extension().wstring())) {
                add_unique_app(
                    apps,
                    paths,
                    path.stem().wstring(),
                    path.wstring());
            }
        }
        if (error) {
            return false;
        }
        error.clear();
        iterator.increment(error);
        if (error) {
            return false;
        }
    }
    return true;
}

bool load_apps(
    const fs::path& applications_directory,
    std::vector<AppEntry>& apps) {
    apps.clear();
    std::unordered_set<std::wstring> paths;
    if (!scan_applications_directory(
            applications_directory,
            apps,
            paths)) {
        apps.clear();
        return false;
    }

    std::ranges::sort(
        apps,
        [](const AppEntry& left, const AppEntry& right) {
            const std::wstring left_name =
                launchpad::lowercase(left.name);
            const std::wstring right_name =
                launchpad::lowercase(right.name);
            if (left_name != right_name) {
                return left_name < right_name;
            }
            return launchpad::lowercase(left.path) <
                launchpad::lowercase(right.path);
        });
    return true;
}

std::optional<std::uint64_t> applications_directory_signature(
    const fs::path& applications_directory) {
    std::vector<std::wstring> entries;
    std::error_code error;
    if (!fs::is_directory(applications_directory, error) || error) {
        return std::nullopt;
    }
    fs::recursive_directory_iterator iterator(
        applications_directory,
        fs::directory_options::skip_permission_denied,
        error);
    if (error) {
        return std::nullopt;
    }
    const fs::recursive_directory_iterator end;
    while (iterator != end) {
        if (iterator->is_regular_file(error)) {
            const fs::path path = iterator->path();
            if (launchpad::is_supported_app_extension(
                    path.extension().wstring())) {
                error.clear();
                fs::path relative = fs::relative(
                    path,
                    applications_directory,
                    error);
                if (error) {
                    relative = path.filename();
                    error.clear();
                }
                const std::uintmax_t size =
                    iterator->file_size(error);
                if (error) {
                    return std::nullopt;
                }
                const fs::file_time_type modified =
                    iterator->last_write_time(error);
                if (error) {
                    return std::nullopt;
                }
                const auto modified_count =
                    modified.time_since_epoch().count();
                entries.push_back(
                    launchpad::lowercase(
                        relative.lexically_normal().wstring()) +
                    L"\n" +
                    std::to_wstring(size) +
                    L"\n" +
                    std::to_wstring(modified_count));
            }
        }
        if (error) {
            return std::nullopt;
        }
        error.clear();
        iterator.increment(error);
        if (error) {
            return std::nullopt;
        }
    }
    std::ranges::sort(entries);

    constexpr std::uint64_t offset = 1469598103934665603ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t signature = offset;
    for (const std::wstring& entry : entries) {
        for (const wchar_t character : entry) {
            signature ^=
                static_cast<std::uint64_t>(
                    static_cast<std::uint32_t>(character));
            signature *= prime;
        }
        signature ^= 0xFFU;
        signature *= prime;
    }
    return signature;
}

class ApplicationsDropTarget final : public IDropTarget {
public:
    static HRESULT create(
        HWND owner,
        const fs::path& applications_directory,
        UINT completed_message,
        IDropTarget** result) {
        if (!result) {
            return E_POINTER;
        }
        *result = nullptr;
        auto* target = new (std::nothrow) ApplicationsDropTarget(
            owner,
            completed_message);
        if (!target) {
            return E_OUTOFMEMORY;
        }
        const HRESULT initialized =
            target->initialize(applications_directory);
        if (FAILED(initialized)) {
            target->Release();
            return initialized;
        }
        *result = target;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID interface_id,
        void** result) override {
        if (!result) {
            return E_POINTER;
        }
        *result = nullptr;
        if (interface_id == IID_IUnknown ||
            interface_id == IID_IDropTarget) {
            *result = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(
            InterlockedIncrement(&reference_count_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const LONG remaining =
            InterlockedDecrement(&reference_count_);
        if (remaining == 0) {
            delete this;
        }
        return static_cast<ULONG>(remaining);
    }

    HRESULT STDMETHODCALLTYPE DragEnter(
        IDataObject* data_object,
        DWORD key_state,
        POINTL point,
        DWORD* effect) override {
        if (!data_object || !effect) {
            return E_INVALIDARG;
        }
        drag_active_ = false;
        source_from_recycle_bin_ = false;
        const DataInspection inspection =
            inspect_data_object(data_object);
        if (!inspection.supported) {
            *effect = DROPEFFECT_NONE;
            return S_OK;
        }

        drag_active_ = true;
        source_from_recycle_bin_ =
            inspection.from_recycle_bin;
        DWORD delegated_effect = allowed_effect(*effect);
        const HRESULT result = delegate_->DragEnter(
            data_object,
            delegated_key_state(key_state),
            point,
            &delegated_effect);
        delegated_effect &= allowed_effect(DROPEFFECT_COPY |
            DROPEFFECT_MOVE | DROPEFFECT_LINK);
        if (FAILED(result) ||
            delegated_effect == DROPEFFECT_NONE) {
            if (SUCCEEDED(result)) {
                delegate_->DragLeave();
            }
            drag_active_ = false;
            source_from_recycle_bin_ = false;
            *effect = DROPEFFECT_NONE;
            return result;
        }
        *effect = delegated_effect;
        return result;
    }

    HRESULT STDMETHODCALLTYPE DragOver(
        DWORD key_state,
        POINTL point,
        DWORD* effect) override {
        if (!effect) {
            return E_POINTER;
        }
        if (!drag_active_) {
            *effect = DROPEFFECT_NONE;
            return S_OK;
        }
        DWORD delegated_effect = allowed_effect(*effect);
        const HRESULT result = delegate_->DragOver(
            delegated_key_state(key_state),
            point,
            &delegated_effect);
        *effect = SUCCEEDED(result)
            ? delegated_effect &
                allowed_effect(DROPEFFECT_COPY |
                    DROPEFFECT_MOVE | DROPEFFECT_LINK)
            : DROPEFFECT_NONE;
        return result;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override {
        const HRESULT result = drag_active_
            ? delegate_->DragLeave()
            : S_OK;
        drag_active_ = false;
        source_from_recycle_bin_ = false;
        return result;
    }

    HRESULT STDMETHODCALLTYPE Drop(
        IDataObject* data_object,
        DWORD key_state,
        POINTL point,
        DWORD* effect) override {
        if (!data_object || !effect) {
            return E_INVALIDARG;
        }
        if (!drag_active_) {
            *effect = DROPEFFECT_NONE;
            return S_OK;
        }

        DWORD delegated_effect = allowed_effect(*effect);
        const HRESULT result = delegate_->Drop(
            data_object,
            delegated_key_state(key_state),
            point,
            &delegated_effect);
        delegated_effect = SUCCEEDED(result)
            ? delegated_effect &
                allowed_effect(DROPEFFECT_COPY |
                    DROPEFFECT_MOVE | DROPEFFECT_LINK)
            : DROPEFFECT_NONE;
        drag_active_ = false;
        source_from_recycle_bin_ = false;
        *effect = delegated_effect;

        if (SUCCEEDED(result)) {
            POINT client_point{
                static_cast<LONG>(point.x),
                static_cast<LONG>(point.y),
            };
            ScreenToClient(owner_, &client_point);
            PostMessageW(
                owner_,
                completed_message_,
                0,
                MAKELPARAM(
                    static_cast<short>(client_point.x),
                    static_cast<short>(client_point.y)));
        }
        return result;
    }

private:
    struct DataInspection {
        bool supported = false;
        bool from_recycle_bin = false;
    };

    ApplicationsDropTarget(HWND owner, UINT completed_message)
        : owner_(owner),
          completed_message_(completed_message) {}

    ~ApplicationsDropTarget() = default;

    HRESULT initialize(const fs::path& applications_directory) {
        ComPtr<IShellItem> directory_item;
        HRESULT result = SHCreateItemFromParsingName(
            applications_directory.c_str(),
            nullptr,
            IID_PPV_ARGS(
                directory_item.ReleaseAndGetAddressOf()));
        ComPtr<IShellFolder> directory_folder;
        if (SUCCEEDED(result)) {
            result = directory_item->BindToHandler(
                nullptr,
                BHID_SFObject,
                IID_PPV_ARGS(
                    directory_folder.ReleaseAndGetAddressOf()));
        }
        if (SUCCEEDED(result)) {
            result = directory_folder->CreateViewObject(
                owner_,
                IID_PPV_ARGS(
                    delegate_.ReleaseAndGetAddressOf()));
        }
        return result;
    }

    static bool shell_item_has_supported_extension(
        IShellItem* item) {
        constexpr std::array<SIGDN, 3> display_names{
            SIGDN_FILESYSPATH,
            SIGDN_PARENTRELATIVEPARSING,
            SIGDN_NORMALDISPLAY,
        };
        for (const SIGDN display_name : display_names) {
            PWSTR text = nullptr;
            const HRESULT result =
                item->GetDisplayName(display_name, &text);
            if (FAILED(result) || !text) {
                continue;
            }
            const std::wstring_view name(text);
            const std::size_t separator =
                name.find_last_of(L"\\/");
            const std::size_t dot = name.find_last_of(L'.');
            const bool supported =
                dot != std::wstring_view::npos &&
                (separator == std::wstring_view::npos ||
                 dot > separator) &&
                launchpad::is_supported_app_extension(
                    name.substr(dot));
            CoTaskMemFree(text);
            if (supported) {
                return true;
            }
        }
        return false;
    }

    static bool shell_item_has_recycle_bin_backing_path(
        IShellItem* item) {
        PWSTR text = nullptr;
        if (FAILED(item->GetDisplayName(
                SIGDN_FILESYSPATH,
                &text)) ||
            !text) {
            return false;
        }
        const std::wstring path =
            launchpad::lowercase(text);
        CoTaskMemFree(text);
        return path.find(L"\\$recycle.bin\\") !=
            std::wstring::npos;
    }

    static bool pidl_is_well_formed(
        const BYTE* bytes,
        SIZE_T size) {
        SIZE_T offset = 0;
        while (offset + sizeof(USHORT) <= size) {
            const USHORT item_size =
                static_cast<USHORT>(bytes[offset]) |
                static_cast<USHORT>(
                    static_cast<USHORT>(bytes[offset + 1]) << 8U);
            if (item_size == 0) {
                return true;
            }
            if (item_size < sizeof(USHORT) ||
                item_size > size - offset) {
                return false;
            }
            offset += item_size;
        }
        return false;
    }

    static bool data_object_parent_is_recycle_bin(
        IDataObject* data_object) {
        const CLIPFORMAT shell_id_list_format =
            static_cast<CLIPFORMAT>(
                RegisterClipboardFormatW(CFSTR_SHELLIDLIST));
        if (shell_id_list_format == 0) {
            return false;
        }
        FORMATETC format{
            .cfFormat = shell_id_list_format,
            .ptd = nullptr,
            .dwAspect = DVASPECT_CONTENT,
            .lindex = -1,
            .tymed = TYMED_HGLOBAL,
        };
        STGMEDIUM medium{};
        if (FAILED(data_object->GetData(&format, &medium)) ||
            medium.tymed != TYMED_HGLOBAL ||
            !medium.hGlobal) {
            return false;
        }

        bool matches = false;
        const SIZE_T size = GlobalSize(medium.hGlobal);
        const auto* bytes = static_cast<const BYTE*>(
            GlobalLock(medium.hGlobal));
        if (bytes && size >= offsetof(CIDA, aoffset) +
                sizeof(UINT)) {
            const auto* id_list =
                reinterpret_cast<const CIDA*>(bytes);
            const SIZE_T offset_count =
                static_cast<SIZE_T>(id_list->cidl) + 1;
            const SIZE_T maximum_offset_count =
                (size - offsetof(CIDA, aoffset)) /
                sizeof(UINT);
            if (offset_count <= maximum_offset_count) {
                const SIZE_T offsets_end =
                    offsetof(CIDA, aoffset) +
                    offset_count * sizeof(UINT);
                const SIZE_T parent_offset =
                    id_list->aoffset[0];
                if (parent_offset >= offsets_end &&
                    parent_offset < size &&
                    pidl_is_well_formed(
                        bytes + parent_offset,
                        size - parent_offset)) {
                    PIDLIST_ABSOLUTE recycle_bin = nullptr;
                    if (SUCCEEDED(SHGetKnownFolderIDList(
                            FOLDERID_RecycleBinFolder,
                            0,
                            nullptr,
                            &recycle_bin))) {
                        matches = ILIsEqual(
                            reinterpret_cast<PCIDLIST_ABSOLUTE>(
                                bytes + parent_offset),
                            recycle_bin) == TRUE;
                        CoTaskMemFree(recycle_bin);
                    }
                }
            }
        }
        if (bytes) {
            GlobalUnlock(medium.hGlobal);
        }
        ReleaseStgMedium(&medium);
        return matches;
    }

    static DataInspection inspect_data_object(
        IDataObject* data_object) {
        ComPtr<IShellItemArray> items;
        if (FAILED(SHCreateShellItemArrayFromDataObject(
                data_object,
                IID_PPV_ARGS(items.ReleaseAndGetAddressOf())))) {
            return {};
        }
        DWORD count = 0;
        if (FAILED(items->GetCount(&count)) || count == 0) {
            return {};
        }
        bool all_items_have_recycle_backing = true;
        for (DWORD index = 0; index < count; ++index) {
            ComPtr<IShellItem> item;
            if (FAILED(items->GetItemAt(
                    index,
                    item.ReleaseAndGetAddressOf()))) {
                return {};
            }
            SFGAOF attributes = 0;
            if (FAILED(item->GetAttributes(
                    SFGAO_FOLDER,
                    &attributes)) ||
                (attributes & SFGAO_FOLDER) != 0 ||
                !shell_item_has_supported_extension(item.Get())) {
                return {};
            }
            all_items_have_recycle_backing =
                all_items_have_recycle_backing &&
                shell_item_has_recycle_bin_backing_path(
                    item.Get());
        }
        return DataInspection{
            .supported = true,
            .from_recycle_bin =
                data_object_parent_is_recycle_bin(data_object) ||
                all_items_have_recycle_backing,
        };
    }

    DWORD allowed_effect(DWORD effect) const {
        return source_from_recycle_bin_
            ? effect
            : effect & DROPEFFECT_COPY;
    }

    DWORD delegated_key_state(DWORD key_state) const {
        if (source_from_recycle_bin_) {
            return key_state;
        }
        return (key_state & ~MK_SHIFT) | MK_CONTROL;
    }

    volatile LONG reference_count_ = 1;
    HWND owner_ = nullptr;
    UINT completed_message_ = 0;
    bool drag_active_ = false;
    bool source_from_recycle_bin_ = false;
    ComPtr<IDropTarget> delegate_;
};

bool has_command_line_switch(std::wstring_view switch_name) {
    int count = 0;
    PWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) {
        return false;
    }
    bool found = false;
    for (int index = 1; index < count; ++index) {
        if (CompareStringOrdinal(
                arguments[index],
                -1,
                switch_name.data(),
                static_cast<int>(switch_name.size()),
                TRUE) == CSTR_EQUAL) {
            found = true;
            break;
        }
    }
    LocalFree(arguments);
    return found;
}

std::optional<fs::path> current_executable_path() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return std::nullopt;
    }
    return fs::path(std::wstring_view(buffer.data(), length));
}

std::optional<fs::path> known_folder_path(
    REFKNOWNFOLDERID folder_id) {
    PWSTR raw_path = nullptr;
    if (FAILED(SHGetKnownFolderPath(
            folder_id,
            KF_FLAG_CREATE,
            nullptr,
            &raw_path)) ||
        !raw_path) {
        return std::nullopt;
    }
    fs::path path(raw_path);
    CoTaskMemFree(raw_path);
    return path;
}

bool create_shell_shortcut(
    const fs::path& shortcut_path,
    const fs::path& executable_path) {
    std::error_code directory_error;
    fs::create_directories(
        shortcut_path.parent_path(),
        directory_error);
    if (directory_error) {
        return false;
    }

    ComPtr<IShellLinkW> shell_link;
    HRESULT result = CoCreateInstance(
        CLSID_ShellLink,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(shell_link.ReleaseAndGetAddressOf()));
    if (FAILED(result)) {
        return false;
    }
    result = shell_link->SetPath(executable_path.c_str());
    if (SUCCEEDED(result)) {
        result = shell_link->SetWorkingDirectory(
            executable_path.parent_path().c_str());
    }
    if (SUCCEEDED(result)) {
        result = shell_link->SetDescription(
            L"Launchpad для Windows");
    }
    if (SUCCEEDED(result)) {
        result = shell_link->SetIconLocation(
            executable_path.c_str(),
            0);
    }

    ComPtr<IPropertyStore> property_store;
    if (SUCCEEDED(result)) {
        result = shell_link.As(&property_store);
    }
    PROPVARIANT app_id{};
    if (SUCCEEDED(result)) {
        result = InitPropVariantFromString(
            kAppUserModelId,
            &app_id);
    }
    if (SUCCEEDED(result)) {
        result = property_store->SetValue(
            PKEY_AppUserModel_ID,
            app_id);
    }
    if (SUCCEEDED(result)) {
        result = property_store->Commit();
    }
    PropVariantClear(&app_id);

    ComPtr<IPersistFile> persist_file;
    if (SUCCEEDED(result)) {
        result = shell_link.As(&persist_file);
    }
    if (SUCCEEDED(result)) {
        result = persist_file->Save(
            shortcut_path.c_str(),
            TRUE);
    }
    if (SUCCEEDED(result)) {
        SHChangeNotify(
            SHCNE_CREATE,
            SHCNF_PATHW,
            shortcut_path.c_str(),
            nullptr);
    }
    return SUCCEEDED(result);
}

bool create_application_shortcuts(bool include_desktop) {
    const std::optional<fs::path> executable =
        current_executable_path();
    const std::optional<fs::path> programs =
        known_folder_path(FOLDERID_Programs);
    if (!executable || !programs ||
        !create_shell_shortcut(
            *programs / kShortcutName,
            *executable)) {
        return false;
    }
    if (!include_desktop) {
        return true;
    }
    const std::optional<fs::path> desktop =
        known_folder_path(FOLDERID_Desktop);
    return desktop &&
        create_shell_shortcut(
            *desktop / kShortcutName,
            *executable);
}

bool remove_shortcut_from(REFKNOWNFOLDERID folder_id) {
    const std::optional<fs::path> directory =
        known_folder_path(folder_id);
    if (!directory) {
        return false;
    }
    const fs::path shortcut = *directory / kShortcutName;
    std::error_code exists_error;
    if (!fs::exists(shortcut, exists_error)) {
        return !exists_error;
    }
    std::error_code remove_error;
    const bool removed = fs::remove(shortcut, remove_error);
    if (removed && !remove_error) {
        SHChangeNotify(
            SHCNE_DELETE,
            SHCNF_PATHW,
            shortcut.c_str(),
            nullptr);
    }
    return removed && !remove_error;
}

bool remove_application_shortcuts() {
    const bool start_removed =
        remove_shortcut_from(FOLDERID_Programs);
    const bool desktop_removed =
        remove_shortcut_from(FOLDERID_Desktop);
    return start_removed && desktop_removed;
}

class LaunchpadWindow {
public:
    bool create(
        HINSTANCE instance,
        bool background_mode,
        bool start_hidden) {
        instance_ = instance;
        background_mode_ = background_mode;
        diagnostics_enabled_ =
            has_command_line_switch(L"--diagnostics");
        animations_enabled_ = query_animations_enabled();
        const CatalogSetup catalog =
            prepare_applications_catalog();
        applications_directory_ =
            catalog.applications_directory;
        layout_path_ = catalog.layout_path;
        const bool applications_loaded =
            load_apps(applications_directory_, apps_);
        const bool loaded_current_layout =
            launchpad::load_layout(layout_path_, layout_);
        bool loaded_legacy_layout = false;
        if (!loaded_current_layout &&
            !catalog.legacy_layout_path.empty()) {
            loaded_legacy_layout = launchpad::load_layout(
                catalog.legacy_layout_path,
                layout_);
        }
        bool layout_remapped = false;
        if (loaded_current_layout || loaded_legacy_layout) {
            if (loaded_legacy_layout) {
                layout_remapped =
                    remap_catalog_layout(
                        layout_,
                        catalog.legacy_layout_applications,
                        applications_directory_) ||
                    layout_remapped;
            }
            for (const fs::path& legacy :
                 catalog.legacy_applications) {
                layout_remapped =
                    remap_catalog_layout(
                        layout_,
                        legacy,
                        applications_directory_) ||
                    layout_remapped;
            }
        }
        bool layout_saved = true;
        const bool catalog_ready =
            applications_loaded &&
            (!catalog.needs_initialization ||
             catalog.migration_succeeded);
        if (catalog_ready) {
            layout_saved = reconcile_layout();
            if (loaded_legacy_layout || layout_remapped) {
                layout_saved =
                    save_layout_state() &&
                    layout_saved;
            }
        }
        if (catalog.needs_initialization &&
            catalog.migration_succeeded &&
            catalog_ready &&
            layout_saved) {
            mark_catalog_initialized(
                catalog.marker_path,
                applications_directory_);
        }
        const auto initial_signature =
            applications_directory_signature(
                applications_directory_);
        applications_signature_ =
            initial_signature.value_or(0);
        rebuild_filter();

        if (FAILED(D2D1CreateFactory(
                D2D1_FACTORY_TYPE_SINGLE_THREADED,
                factory_.ReleaseAndGetAddressOf()))) {
            return false;
        }
        if (FAILED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(
                    write_factory_.ReleaseAndGetAddressOf())))) {
            return false;
        }
        if (FAILED(CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(wic_factory_.ReleaseAndGetAddressOf())))) {
            return false;
        }
        if (!create_text_formats()) {
            return false;
        }

        HICON large_icon = static_cast<HICON>(LoadImageW(
            instance_,
            MAKEINTRESOURCEW(IDI_LAUNCHPAD),
            IMAGE_ICON,
            GetSystemMetrics(SM_CXICON),
            GetSystemMetrics(SM_CYICON),
            LR_DEFAULTCOLOR | LR_SHARED));
        HICON small_icon = static_cast<HICON>(LoadImageW(
            instance_,
            MAKEINTRESOURCEW(IDI_LAUNCHPAD),
            IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON),
            LR_DEFAULTCOLOR | LR_SHARED));
        if (!large_icon) {
            large_icon = LoadIconW(nullptr, IDI_APPLICATION);
        }
        if (!small_icon) {
            small_icon = large_icon;
        }

        WNDCLASSEXW window_class{
            .cbSize = sizeof(WNDCLASSEXW),
            .style = CS_HREDRAW | CS_VREDRAW,
            .lpfnWndProc = &LaunchpadWindow::window_proc,
            .cbClsExtra = 0,
            .cbWndExtra = 0,
            .hInstance = instance_,
            .hIcon = large_icon,
            .hCursor = LoadCursorW(nullptr, IDC_ARROW),
            .hbrBackground = nullptr,
            .lpszMenuName = nullptr,
            .lpszClassName = kWindowClass,
            .hIconSm = small_icon,
        };

        if (!RegisterClassExW(&window_class) &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        const RECT bounds = active_monitor_bounds();
        hwnd_ = CreateWindowExW(
            WS_EX_TOOLWINDOW,
            kWindowClass,
            L"Windows Launchpad",
            WS_POPUP,
            bounds.left,
            bounds.top,
            bounds.right - bounds.left,
            bounds.bottom - bounds.top,
            nullptr,
            nullptr,
            instance_,
            this);
        if (!hwnd_) {
            return false;
        }

        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        clock_frequency_ = static_cast<double>(frequency.QuadPart);
        reset_animation_clock();
        const DWORD refresh_rate = query_refresh_rate();
        fallback_frame_interval_ms_ = static_cast<UINT>(
            std::clamp(
                std::lround(
                    1000.0 /
                    static_cast<double>(refresh_rate)),
                2L,
                16L));
        frame_period_qpc_ = std::max<std::int64_t>(
            1,
            static_cast<std::int64_t>(std::llround(
                clock_frequency_ /
                static_cast<double>(refresh_rate))));
        frame_timer_ = CreateWaitableTimerExW(
            nullptr,
            nullptr,
            kHighResolutionTimerFlag,
            TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!frame_timer_) {
            frame_timer_ = CreateWaitableTimerExW(
                nullptr,
                nullptr,
                0,
                TIMER_MODIFY_STATE | SYNCHRONIZE);
            if (frame_timer_ &&
                timeBeginPeriod(1) == TIMERR_NOERROR) {
                timer_period_raised_ = true;
            }
        }

        const BOOL dark_mode = TRUE;
        DwmSetWindowAttribute(
            hwnd_,
            DWMWA_USE_IMMERSIVE_DARK_MODE,
            &dark_mode,
            sizeof(dark_mode));
        const DWM_SYSTEMBACKDROP_TYPE backdrop =
            DWMSBT_TRANSIENTWINDOW;
        DwmSetWindowAttribute(
            hwnd_,
            DWMWA_SYSTEMBACKDROP_TYPE,
            &backdrop,
            sizeof(backdrop));
        const MARGINS glass_margins{-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(hwnd_, &glass_margins);

        if (SUCCEEDED(ApplicationsDropTarget::create(
                hwnd_,
                applications_directory_,
                kExternalDropCompletedMessage,
                external_drop_target_.ReleaseAndGetAddressOf()))) {
            if (SUCCEEDED(RegisterDragDrop(
                    hwnd_,
                    external_drop_target_.Get()))) {
                external_drop_target_registered_ = true;
            } else {
                external_drop_target_.Reset();
            }
        }
        SetTimer(
            hwnd_,
            kApplicationsWatchTimer,
            kApplicationsWatchIntervalMs,
            nullptr);

        preload_visible_icons();
        if (background_mode_) {
            RegisterHotKey(
                hwnd_,
                kGlobalHotkeyId,
                MOD_ALT | MOD_WIN | MOD_NOREPEAT,
                VK_SPACE);
        }
        if (!start_hidden) {
            show_launchpad();
        }
        return true;
    }

    int run() {
        MSG message{};
        for (;;) {
            HANDLE handles[1]{frame_timer_};
            const DWORD handle_count =
                frame_timer_ && frame_pump_active_ ? 1U : 0U;
            const DWORD wait_result = MsgWaitForMultipleObjectsEx(
                handle_count,
                handle_count != 0 ? handles : nullptr,
                INFINITE,
                QS_ALLINPUT,
                MWMO_INPUTAVAILABLE);
            if (wait_result == WAIT_FAILED) {
                return 1;
            }
            if (handle_count == 1 &&
                wait_result == WAIT_OBJECT_0) {
                on_frame_deadline();
            }
            while (PeekMessageW(
                       &message,
                       nullptr,
                       0,
                       0,
                       PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    return static_cast<int>(message.wParam);
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }

private:
    static LRESULT CALLBACK window_proc(
        HWND hwnd,
        UINT message,
        WPARAM wparam,
        LPARAM lparam) {
        LaunchpadWindow* self = nullptr;
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<LaunchpadWindow*>(create->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(
                hwnd,
                GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<LaunchpadWindow*>(
                GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return self
            ? self->handle_message(message, wparam, lparam)
            : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
        if (delete_confirmation_active_) {
            switch (message) {
            case WM_CHAR:
                return 0;
            case WM_KEYDOWN:
                on_key_down(wparam);
                return 0;
            case WM_MOUSEMOVE:
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:
                return 0;
            case WM_LBUTTONDOWN:
                SetFocus(hwnd_);
                delete_modal_pressed_button_ =
                    hit_test_delete_modal_button(
                        GET_X_LPARAM(lparam),
                        GET_Y_LPARAM(lparam));
                if (delete_modal_pressed_button_ !=
                    DeleteModalButton::none) {
                    SetCapture(hwnd_);
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            case WM_LBUTTONUP: {
                const DeleteModalButton pressed =
                    delete_modal_pressed_button_;
                const DeleteModalButton released =
                    hit_test_delete_modal_button(
                        GET_X_LPARAM(lparam),
                        GET_Y_LPARAM(lparam));
                delete_modal_pressed_button_ =
                    DeleteModalButton::none;
                if (GetCapture() == hwnd_) {
                    ReleaseCapture();
                }
                if (pressed == released) {
                    if (released == DeleteModalButton::cancel) {
                        dismiss_delete_confirmation();
                    } else if (
                        released == DeleteModalButton::confirm) {
                        confirm_pending_delete();
                    }
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            case WM_CAPTURECHANGED:
                delete_modal_pressed_button_ =
                    DeleteModalButton::none;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            default:
                break;
            }
        }

        switch (message) {
        case WM_PAINT:
            paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (render_target_) {
                render_target_->Resize(
                    D2D1::SizeU(LOWORD(lparam), HIWORD(lparam)));
            }
            return 0;
        case WM_KILLFOCUS:
            set_search_focused(false, false);
            return 0;
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<RECT*>(lparam);
            const UINT dpi = HIWORD(wparam);
            SetWindowPos(
                hwnd_,
                nullptr,
                suggested->left,
                suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOACTIVATE | SWP_NOZORDER);
            if (render_target_) {
                render_target_->SetDpi(
                    static_cast<float>(dpi),
                    static_cast<float>(dpi));
            }
            update_icon_request_size(dpi);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT &&
                open_folder_index_ == kNoPage &&
                !delete_confirmation_active_ &&
                !closing_ &&
                !drag_active_ &&
                !drag_candidate_ &&
                !page_drag_active_) {
                POINT point{};
                if (GetCursorPos(&point) &&
                    ScreenToClient(hwnd_, &point) &&
                    hit_test_search(point.x, point.y)) {
                    SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                    return TRUE;
                }
            }
            break;
        case WM_TIMER:
            if (wparam == kExternalDropRescanTimer) {
                KillTimer(hwnd_, kExternalDropRescanTimer);
                finish_external_drop_rescan();
                return 0;
            }
            if (wparam == kApplicationsWatchTimer) {
                refresh_apps_if_changed();
                return 0;
            }
            if (!frame_timer_ && wparam == kAnimationTimer) {
                animation_tick();
            }
            return 0;
        case WM_CHAR:
            on_character(static_cast<wchar_t>(wparam));
            return 0;
        case WM_KEYDOWN:
            if (on_key_down(wparam)) {
                return 0;
            }
            break;
        case WM_MOUSEMOVE:
            if (folder_drag_active_) {
                update_folder_drag(
                    GET_X_LPARAM(lparam),
                    GET_Y_LPARAM(lparam));
            } else if (folder_drag_candidate_) {
                folder_drag_current_x_ = GET_X_LPARAM(lparam);
                folder_drag_current_y_ = GET_Y_LPARAM(lparam);
                const D2D1_POINT_2F delta = client_delta_to_dips(
                    folder_drag_current_x_ - left_mouse_down_x_,
                    folder_drag_current_y_ - left_mouse_down_y_);
                if (std::hypot(delta.x, delta.y) >=
                        kDragSlopDips &&
                    elapsed_since(folder_drag_press_origin_) >=
                        0.10) {
                    start_folder_drag();
                }
            } else if (drag_active_) {
                update_drag(
                    GET_X_LPARAM(lparam),
                    GET_Y_LPARAM(lparam));
            } else if (drag_candidate_) {
                drag_current_x_ = GET_X_LPARAM(lparam);
                drag_current_y_ = GET_Y_LPARAM(lparam);
                const D2D1_POINT_2F delta = client_delta_to_dips(
                    drag_current_x_ - left_mouse_down_x_,
                    drag_current_y_ - left_mouse_down_y_);
                if (std::hypot(delta.x, delta.y) >=
                        kDragSlopDips &&
                    elapsed_since(drag_press_origin_) >= 0.10) {
                    start_drag();
                }
            } else if (page_drag_active_) {
                page_drag_current_x_ = GET_X_LPARAM(lparam);
                page_drag_current_y_ = GET_Y_LPARAM(lparam);
                update_page_drag_moved();
                record_page_drag_sample(page_drag_raw_offset());
            } else if (open_folder_index_ != kNoPage) {
                update_folder_pointer_selection(
                    GET_X_LPARAM(lparam),
                    GET_Y_LPARAM(lparam));
            } else {
                update_pointer_selection(
                    GET_X_LPARAM(lparam),
                    GET_Y_LPARAM(lparam));
            }
            return 0;
        case WM_LBUTTONDOWN:
            SetFocus(hwnd_);
            left_mouse_down_x_ = GET_X_LPARAM(lparam);
            left_mouse_down_y_ = GET_Y_LPARAM(lparam);
            mouse_down_delete_position_ = kNoPage;
            mouse_down_folder_delete_position_ = kNoPage;
            if (open_folder_index_ != kNoPage) {
                if (edit_mode_) {
                    mouse_down_folder_delete_position_ =
                        hit_test_folder_delete(
                            left_mouse_down_x_,
                            left_mouse_down_y_);
                    if (mouse_down_folder_delete_position_ !=
                        kNoPage) {
                        SetCapture(hwnd_);
                        return 0;
                    }
                }
                if (hit_test_folder_title(
                        GET_X_LPARAM(lparam),
                        GET_Y_LPARAM(lparam))) {
                    begin_folder_name_edit();
                    return 0;
                }
                if (folder_name_editing_) {
                    commit_folder_name_edit();
                    return 0;
                }
                mouse_down_on_folder_item_ =
                    update_folder_pointer_selection(
                        GET_X_LPARAM(lparam),
                        GET_Y_LPARAM(lparam));
                mouse_down_folder_position_ =
                    mouse_down_on_folder_item_
                        ? folder_selected_position_
                        : kNoPage;
                if (mouse_down_on_folder_item_) {
                    folder_drag_candidate_ = true;
                    folder_drag_source_position_ =
                        folder_selected_position_;
                    folder_drag_target_position_ =
                        folder_selected_position_;
                    folder_drag_current_x_ =
                        left_mouse_down_x_;
                    folder_drag_current_y_ =
                        left_mouse_down_y_;
                    LARGE_INTEGER counter{};
                    QueryPerformanceCounter(&counter);
                    folder_drag_press_origin_ =
                        counter.QuadPart;
                    SetCapture(hwnd_);
                }
                mouse_down_on_folder_background_ =
                    !mouse_down_on_folder_item_;
                return 0;
            }
            if (hit_test_search(
                    left_mouse_down_x_,
                    left_mouse_down_y_)) {
                mouse_down_page_ = kNoPage;
                mouse_down_on_item_ = false;
                mouse_down_on_background_ = false;
                selection_visible_ = false;
                edit_mode_ = false;
                set_search_focused(true);
                return 0;
            }
            if (search_focused_) {
                set_search_focused(false);
            }
            if (edit_mode_) {
                mouse_down_delete_position_ =
                    hit_test_root_delete(
                        left_mouse_down_x_,
                        left_mouse_down_y_);
                if (mouse_down_delete_position_ != kNoPage) {
                    SetCapture(hwnd_);
                    return 0;
                }
            }
            mouse_down_page_ = hit_test_page_dot(
                GET_X_LPARAM(lparam),
                GET_Y_LPARAM(lparam));
            mouse_down_on_item_ =
                mouse_down_page_ == kNoPage &&
                update_pointer_selection(
                    GET_X_LPARAM(lparam),
                    GET_Y_LPARAM(lparam));
            write_drag_diagnostic(
                L"down",
                left_mouse_down_x_,
                left_mouse_down_y_,
                mouse_down_on_item_
                    ? selected_position_
                    : kNoPage);
            if (mouse_down_on_item_ &&
                search_.empty() &&
                !page_transition_active_) {
                drag_candidate_ = true;
                drag_source_visible_position_ =
                    selected_position_;
                drag_source_layout_index_ =
                    visible_items_[selected_position_].layout_index;
                drag_source_kind_ =
                    visible_items_[selected_position_].kind;
                drag_current_x_ = left_mouse_down_x_;
                drag_current_y_ = left_mouse_down_y_;
                LARGE_INTEGER counter{};
                QueryPerformanceCounter(&counter);
                drag_press_origin_ = counter.QuadPart;
                SetCapture(hwnd_);
            }
            mouse_down_on_background_ =
                mouse_down_page_ == kNoPage &&
                !mouse_down_on_item_ &&
                !hit_test_search(
                    GET_X_LPARAM(lparam),
                    GET_Y_LPARAM(lparam));
            if (mouse_down_on_background_ &&
                !page_transition_active_ &&
                !closing_) {
                page_drag_active_ = true;
                page_drag_start_x_ = left_mouse_down_x_;
                page_drag_start_y_ = left_mouse_down_y_;
                page_drag_current_x_ = page_drag_start_x_;
                page_drag_current_y_ = page_drag_start_y_;
                page_drag_moved_ = false;
                reset_page_drag_samples();
                record_page_drag_sample(0.0F);
                SetCapture(hwnd_);
            }
            return 0;
        case WM_LBUTTONUP: {
            const int x = GET_X_LPARAM(lparam);
            const int y = GET_Y_LPARAM(lparam);
            if (mouse_down_folder_delete_position_ != kNoPage) {
                const std::size_t pressed =
                    mouse_down_folder_delete_position_;
                mouse_down_folder_delete_position_ = kNoPage;
                const bool activate =
                    hit_test_folder_delete(x, y) == pressed &&
                    is_click_without_drag(
                        left_mouse_down_x_,
                        left_mouse_down_y_,
                        x,
                        y);
                if (GetCapture() == hwnd_) {
                    ReleaseCapture();
                }
                if (activate) {
                    delete_folder_app(pressed);
                }
                return 0;
            }
            if (mouse_down_delete_position_ != kNoPage) {
                const std::size_t pressed =
                    mouse_down_delete_position_;
                mouse_down_delete_position_ = kNoPage;
                const bool activate =
                    hit_test_root_delete(x, y) == pressed &&
                    is_click_without_drag(
                        left_mouse_down_x_,
                        left_mouse_down_y_,
                        x,
                        y);
                if (GetCapture() == hwnd_) {
                    ReleaseCapture();
                }
                if (activate) {
                    delete_root_app(pressed);
                }
                return 0;
            }
            if (folder_drag_active_) {
                update_folder_drag(x, y);
                if (drag_active_) {
                    finish_drag();
                } else {
                    finish_folder_drag();
                }
                if (GetCapture() == hwnd_) {
                    ReleaseCapture();
                }
                mouse_down_on_folder_item_ = false;
                mouse_down_on_folder_background_ = false;
                mouse_down_folder_position_ = kNoPage;
                return 0;
            }
            if (folder_drag_candidate_) {
                folder_drag_candidate_ = false;
                folder_drag_source_position_ = kNoPage;
                folder_drag_target_position_ = kNoPage;
                if (GetCapture() == hwnd_) {
                    ReleaseCapture();
                }
            }
            if (drag_active_) {
                update_drag(x, y);
                finish_drag();
                if (GetCapture() == hwnd_) {
                    ReleaseCapture();
                }
                mouse_down_on_item_ = false;
                mouse_down_on_background_ = false;
                mouse_down_on_folder_item_ = false;
                mouse_down_on_folder_background_ = false;
                mouse_down_folder_position_ = kNoPage;
                return 0;
            }
            if (open_folder_index_ != kNoPage) {
                const bool released_on_folder_item =
                    update_folder_pointer_selection(x, y);
                if (mouse_down_on_folder_item_ &&
                    released_on_folder_item &&
                    mouse_down_folder_position_ ==
                        folder_selected_position_) {
                    launch_folder_selected();
                } else if (
                    mouse_down_on_folder_background_ &&
                    !released_on_folder_item &&
                    is_click_without_drag(
                        left_mouse_down_x_,
                        left_mouse_down_y_,
                        x,
                        y)) {
                    close_folder();
                }
                mouse_down_on_folder_item_ = false;
                mouse_down_on_folder_background_ = false;
                mouse_down_folder_position_ = kNoPage;
                return 0;
            }
            if (drag_candidate_) {
                drag_candidate_ = false;
                drag_source_visible_position_ = kNoPage;
                drag_source_layout_index_ = kNoPage;
                if (GetCapture() == hwnd_) {
                    ReleaseCapture();
                }
            }
            if (page_drag_active_) {
                page_drag_current_x_ = x;
                page_drag_current_y_ = y;
                const D2D1_POINT_2F delta = client_delta_to_dips(
                    x - page_drag_start_x_,
                    y - page_drag_start_y_);
                update_page_drag_moved();
                const bool page_drag_moved =
                    page_drag_moved_;
                record_page_drag_sample(delta.x);
                const float page_width = page_width_dips();
                const float raw_velocity =
                    page_drag_velocity(page_width);
                const PageDragVisual visual =
                    page_drag_visual(
                        delta.x,
                        page_width,
                        raw_velocity);
                page_drag_active_ = false;
                page_drag_moved_ = false;
                if (GetCapture() == hwnd_) {
                    ReleaseCapture();
                }
                const float travel =
                    -static_cast<float>(visual.direction) * delta.x;
                const float toward =
                    -static_cast<float>(visual.direction) *
                    raw_velocity;
                const bool horizontal =
                    std::abs(delta.x) >
                    std::abs(delta.y) * 1.25F;
                const bool distance_commit =
                    travel >= kSwipeThresholdDips &&
                    travel + toward * 0.10F >=
                        kSwipeThresholdDips * 0.5F;
                const bool flick_commit =
                    travel >= 18.0F &&
                    toward >=
                        kPageFlickVelocityDipsPerSecond &&
                    std::abs(delta.x) >
                        std::abs(delta.y) * 0.50F;
                if (visual.neighbor != kNoPage &&
                    (horizontal || flick_commit) &&
                    (distance_commit || flick_commit)) {
                    mouse_down_page_ = kNoPage;
                    mouse_down_on_item_ = false;
                    mouse_down_on_background_ = false;
                    go_to_page(
                        visual.neighbor,
                        visual.offset,
                        raw_velocity * visual.velocity_scale);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                if (page_drag_moved) {
                    begin_page_return(
                        visual,
                        raw_velocity *
                            visual.velocity_scale,
                        page_width);
                    mouse_down_page_ = kNoPage;
                    mouse_down_on_item_ = false;
                    mouse_down_on_background_ = false;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            const std::size_t released_page = hit_test_page_dot(x, y);
            const bool released_on_item =
                released_page == kNoPage &&
                update_pointer_selection(x, y);
            if (mouse_down_page_ != kNoPage &&
                released_page == mouse_down_page_) {
                go_to_page(mouse_down_page_);
            } else if (mouse_down_on_item_ &&
                       released_on_item) {
                launch_selected();
            } else if (mouse_down_on_background_ &&
                       !released_on_item &&
                       released_page == kNoPage &&
                       !hit_test_search(x, y) &&
                       is_click_without_drag(
                           left_mouse_down_x_,
                           left_mouse_down_y_,
                           x,
                           y)) {
                if (edit_mode_) {
                    edit_mode_ = false;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                } else {
                    request_close();
                }
            }
            mouse_down_page_ = kNoPage;
            mouse_down_on_item_ = false;
            mouse_down_on_background_ = false;
            return 0;
        }
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            return 0;
        case WM_CAPTURECHANGED:
            if (page_drag_active_) {
                const D2D1_POINT_2F delta = client_delta_to_dips(
                    page_drag_current_x_ - page_drag_start_x_,
                    page_drag_current_y_ - page_drag_start_y_);
                update_page_drag_moved();
                const bool page_drag_moved =
                    page_drag_moved_;
                record_page_drag_sample(delta.x);
                const float page_width = page_width_dips();
                const float raw_velocity =
                    page_drag_velocity(page_width);
                const PageDragVisual visual =
                    page_drag_visual(
                        delta.x,
                        page_width,
                        raw_velocity);
                page_drag_active_ = false;
                page_drag_moved_ = false;
                if (page_drag_moved) {
                    begin_page_return(
                        visual,
                        raw_velocity *
                            visual.velocity_scale,
                        page_width);
                }
                mouse_down_page_ = kNoPage;
                mouse_down_on_item_ = false;
                mouse_down_on_background_ = false;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            if (drag_active_ || drag_candidate_) {
                cancel_drag();
            }
            if (folder_drag_active_ || folder_drag_candidate_) {
                cancel_folder_drag();
            }
            mouse_down_delete_position_ = kNoPage;
            mouse_down_folder_delete_position_ = kNoPage;
            return 0;
        case WM_CONTEXTMENU:
            return 0;
        case WM_MOUSEWHEEL:
            if (open_folder_index_ != kNoPage) {
                change_folder_page(
                    GET_WHEEL_DELTA_WPARAM(wparam) < 0 ? 1 : -1);
                return 0;
            }
            accumulate_wheel(
                GET_WHEEL_DELTA_WPARAM(wparam),
                false);
            return 0;
        case WM_MOUSEHWHEEL:
            if (open_folder_index_ != kNoPage) {
                change_folder_page(
                    GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? 1 : -1);
                return 0;
            }
            accumulate_wheel(
                GET_WHEEL_DELTA_WPARAM(wparam),
                true);
            return 0;
        case WM_HOTKEY:
            if (wparam == kGlobalHotkeyId) {
                if (IsWindowVisible(hwnd_)) {
                    request_close();
                } else {
                    show_launchpad();
                }
            }
            return 0;
        case kShowLaunchpadMessage:
            show_launchpad();
            return 0;
        case kExternalDropCompletedMessage:
            begin_external_drop_rescan(
                GET_X_LPARAM(lparam),
                GET_Y_LPARAM(lparam));
            return 0;
        case WM_CLOSE:
            request_close();
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd_, kExternalDropRescanTimer);
            KillTimer(hwnd_, kApplicationsWatchTimer);
            pending_external_drop_.reset();
            if (external_drop_target_registered_) {
                RevokeDragDrop(hwnd_);
                external_drop_target_registered_ = false;
            }
            external_drop_target_.Reset();
            stop_frame_pump();
            if (frame_timer_) {
                CloseHandle(frame_timer_);
                frame_timer_ = nullptr;
            }
            UnregisterHotKey(hwnd_, kGlobalHotkeyId);
            if (timer_period_raised_) {
                timeEndPeriod(1);
                timer_period_raised_ = false;
            }
            discard_device_resources();
            release_background_capture();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    bool create_text_formats() {
        if (FAILED(write_factory_->CreateTextFormat(
                L"Segoe UI Variable",
                nullptr,
                DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                14.5F,
                L"ru-RU",
                label_format_.ReleaseAndGetAddressOf()))) {
            return false;
        }
        label_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        label_format_->SetParagraphAlignment(
            DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        label_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        DWRITE_TRIMMING trimming{
            .granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER,
            .delimiter = 0,
            .delimiterCount = 0,
        };
        if (SUCCEEDED(write_factory_->CreateEllipsisTrimmingSign(
                label_format_.Get(),
                ellipsis_.ReleaseAndGetAddressOf()))) {
            label_format_->SetTrimming(&trimming, ellipsis_.Get());
        }

        if (FAILED(write_factory_->CreateTextFormat(
                L"Segoe UI Variable",
                nullptr,
                DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                15.0F,
                L"ru-RU",
                search_format_.ReleaseAndGetAddressOf()))) {
            return false;
        }
        search_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        search_format_->SetParagraphAlignment(
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        search_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        if (FAILED(write_factory_->CreateTextFormat(
                L"Segoe UI Variable Display",
                nullptr,
                DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                38.0F,
                L"ru-RU",
                glyph_format_.ReleaseAndGetAddressOf()))) {
            return false;
        }
        glyph_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        glyph_format_->SetParagraphAlignment(
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        if (FAILED(write_factory_->CreateTextFormat(
                L"Segoe UI Variable",
                nullptr,
                DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                18.0F,
                L"ru-RU",
                empty_format_.ReleaseAndGetAddressOf()))) {
            return false;
        }
        empty_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        empty_format_->SetParagraphAlignment(
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        if (FAILED(write_factory_->CreateTextFormat(
                L"Segoe UI Variable",
                nullptr,
                DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                26.0F,
                L"ru-RU",
                folder_title_format_.ReleaseAndGetAddressOf()))) {
            return false;
        }
        folder_title_format_->SetTextAlignment(
            DWRITE_TEXT_ALIGNMENT_CENTER);
        folder_title_format_->SetParagraphAlignment(
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        folder_title_format_->SetWordWrapping(
            DWRITE_WORD_WRAPPING_NO_WRAP);
        return true;
    }

    std::vector<std::pair<std::wstring, std::wstring>>
    available_apps() const {
        std::vector<std::pair<std::wstring, std::wstring>> available;
        available.reserve(apps_.size());
        for (const AppEntry& app : apps_) {
            available.emplace_back(app.path, app.name);
        }
        return available;
    }

    bool reconcile_layout() {
        app_by_path_.clear();
        for (std::size_t index = 0; index < apps_.size(); ++index) {
            app_by_path_.emplace(
                launchpad::lowercase(apps_[index].path),
                index);
        }
        const auto available = available_apps();
        if (layout_.reconcile(available)) {
            return save_layout_state();
        }
        return true;
    }

    std::size_t app_index_for_path(std::wstring_view path) const {
        const auto found = app_by_path_.find(
            launchpad::lowercase(path));
        return found == app_by_path_.end()
            ? kNoPage
            : found->second;
    }

    std::size_t visible_position_for_root_app(
        std::wstring_view path) const {
        const std::wstring normalized =
            launchpad::lowercase(path);
        for (std::size_t position = 0;
             position < visible_items_.size();
             ++position) {
            const VisibleItem& visible =
                visible_items_[position];
            if (visible.kind != VisibleItemKind::app ||
                visible.layout_index >= layout_.items().size()) {
                continue;
            }
            const launchpad::LayoutItem& item =
                layout_.items()[visible.layout_index];
            if (item.kind == launchpad::LayoutItemKind::app &&
                launchpad::lowercase(item.app_path) == normalized) {
                return position;
            }
        }
        return kNoPage;
    }

    const AppEntry* app_for_visible(const VisibleItem& item) const {
        return item.kind == VisibleItemKind::app &&
                item.app_index < apps_.size()
            ? &apps_[item.app_index]
            : nullptr;
    }

    AppEntry* app_for_visible(const VisibleItem& item) {
        return item.kind == VisibleItemKind::app &&
                item.app_index < apps_.size()
            ? &apps_[item.app_index]
            : nullptr;
    }

    bool save_layout_state() {
        if (launchpad::save_layout(layout_path_, layout_)) {
            return true;
        }
        MessageBoxW(
            hwnd_,
            L"Не удалось сохранить раскладку. Изменение останется "
            L"видимым только до перезапуска Launchpad.",
            L"Windows Launchpad",
            MB_OK | MB_ICONERROR);
        return false;
    }

    std::size_t visible_page_count() const noexcept {
        return std::max<std::size_t>(
            1,
            visible_page_starts_.size());
    }

    std::size_t effective_page_count() const noexcept {
        return visible_page_count() +
            (drag_provisional_page_ ? 1 : 0);
    }

    std::pair<std::size_t, std::size_t> visible_page_range(
        std::size_t page) const noexcept {
        if (page >= visible_page_starts_.size()) {
            return {visible_items_.size(), visible_items_.size()};
        }
        const std::size_t first = visible_page_starts_[page];
        const std::size_t last =
            page + 1 < visible_page_starts_.size()
            ? visible_page_starts_[page + 1]
            : visible_items_.size();
        return {first, last};
    }

    std::size_t visible_page_for_position(
        std::size_t position) const noexcept {
        if (visible_page_starts_.empty()) {
            return 0;
        }
        const auto next = std::upper_bound(
            visible_page_starts_.begin(),
            visible_page_starts_.end(),
            position);
        return next == visible_page_starts_.begin()
            ? 0
            : static_cast<std::size_t>(
                  std::distance(
                      visible_page_starts_.begin(),
                      next) -
                  1);
    }

    void select_first_item_on_page(std::size_t page) {
        const auto [first, last] = visible_page_range(page);
        if (first < last) {
            selected_position_ = first;
        } else if (!visible_items_.empty()) {
            selected_position_ = visible_items_.size() - 1;
        } else {
            selected_position_ = 0;
        }
    }

    void preload_icons() {
        if (!ensure_device_resources()) {
            return;
        }
        for (AppEntry& app : apps_) {
            ensure_icon(app);
        }
        icons_pending_ = false;
    }

    void preload_visible_icons() {
        if (!ensure_device_resources()) {
            return;
        }
        const auto [first, last] =
            visible_page_range(current_page_);
        for (std::size_t position = first;
             position < last;
             ++position) {
            if (position >= visible_items_.size()) {
                break;
            }
            const VisibleItem& visible =
                visible_items_[position];
            if (AppEntry* app = app_for_visible(visible)) {
                ensure_icon(*app);
                continue;
            }
            if (visible.kind != VisibleItemKind::folder ||
                visible.layout_index >= layout_.items().size()) {
                continue;
            }
            const launchpad::LayoutItem& folder =
                layout_.items()[visible.layout_index];
            for (std::size_t child = 0;
                 child < folder.children.size();
                 ++child) {
                const std::size_t app_index =
                    app_index_for_path(folder.children[child]);
                if (app_index != kNoPage) {
                    ensure_icon(apps_[app_index]);
                }
            }
        }
        icons_pending_ = std::ranges::any_of(
            apps_,
            [](const AppEntry& app) {
                return !app.icon &&
                    !app.icon_attempted;
            });
    }

    void capture_background(const RECT& bounds) {
        release_background_capture();

        constexpr int downsample = 8;
        const int source_width =
            std::max(1L, bounds.right - bounds.left);
        const int source_height =
            std::max(1L, bounds.bottom - bounds.top);
        const int width = std::max(1, source_width / downsample);
        const int height = std::max(1, source_height / downsample);

        HDC screen = GetDC(nullptr);
        if (!screen) {
            return;
        }
        HDC memory = CreateCompatibleDC(screen);
        if (!memory) {
            ReleaseDC(nullptr, screen);
            return;
        }

        BITMAPINFO bitmap_info{};
        bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap_info.bmiHeader.biWidth = width;
        bitmap_info.bmiHeader.biHeight = -height;
        bitmap_info.bmiHeader.biPlanes = 1;
        bitmap_info.bmiHeader.biBitCount = 32;
        bitmap_info.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(
            screen,
            &bitmap_info,
            DIB_RGB_COLORS,
            &bits,
            nullptr,
            0);
        if (!bitmap || !bits) {
            if (bitmap) {
                DeleteObject(bitmap);
            }
            DeleteDC(memory);
            ReleaseDC(nullptr, screen);
            return;
        }

        const HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
        SetStretchBltMode(memory, HALFTONE);
        SetBrushOrgEx(memory, 0, 0, nullptr);
        const BOOL captured = StretchBlt(
            memory,
            0,
            0,
            width,
            height,
            screen,
            bounds.left,
            bounds.top,
            source_width,
            source_height,
            SRCCOPY | CAPTUREBLT);
        SelectObject(memory, old_bitmap);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);

        if (!captured) {
            DeleteObject(bitmap);
            return;
        }

        box_blur_bgra(
            static_cast<std::uint32_t*>(bits),
            width,
            height,
            5,
            3);
        background_capture_ = bitmap;
        background_bitmap_size_ = D2D1::SizeU(
            static_cast<UINT32>(width),
            static_cast<UINT32>(height));
    }

    bool ensure_background_bitmap() {
        if (background_bitmap_) {
            return true;
        }
        if (!background_capture_ || !render_target_) {
            return false;
        }

        ComPtr<IWICBitmap> wic_bitmap;
        if (FAILED(wic_factory_->CreateBitmapFromHBITMAP(
                background_capture_,
                nullptr,
                WICBitmapIgnoreAlpha,
                wic_bitmap.ReleaseAndGetAddressOf()))) {
            return false;
        }
        return SUCCEEDED(render_target_->CreateBitmapFromWicBitmap(
            wic_bitmap.Get(),
            nullptr,
            background_bitmap_.ReleaseAndGetAddressOf()));
    }

    void release_background_capture() {
        background_bitmap_.Reset();
        if (background_capture_) {
            DeleteObject(background_capture_);
            background_capture_ = nullptr;
        }
        background_bitmap_size_ = D2D1::SizeU(0, 0);
    }

    void update_icon_request_size(UINT dpi) {
        const UINT safe_dpi = std::max<UINT>(96, dpi);
        const UINT required_pixels = static_cast<UINT>(std::ceil(
            kLargestIconSlotDips *
            static_cast<float>(safe_dpi) / 96.0F));
        UINT request_pixels = kBaseIconRequestPixels;
        while (request_pixels < required_pixels &&
               request_pixels < 1024) {
            request_pixels *= 2;
        }
        if (request_pixels == icon_request_pixels_) {
            return;
        }
        icon_request_pixels_ = request_pixels;
        for (AppEntry& app : apps_) {
            app.icon.Reset();
            app.icon_attempted = false;
        }
        icons_pending_ = true;
    }

    bool ensure_device_resources() {
        if (render_target_) {
            return true;
        }

        RECT client{};
        GetClientRect(hwnd_, &client);
        const D2D1_SIZE_U size = D2D1::SizeU(
            static_cast<UINT32>(std::max(1L, client.right)),
            static_cast<UINT32>(std::max(1L, client.bottom)));
        if (FAILED(factory_->CreateHwndRenderTarget(
                D2D1::RenderTargetProperties(),
                D2D1::HwndRenderTargetProperties(hwnd_, size),
                render_target_.ReleaseAndGetAddressOf()))) {
            return false;
        }
        const float dpi = static_cast<float>(GetDpiForWindow(hwnd_));
        render_target_->SetDpi(dpi, dpi);
        update_icon_request_size(static_cast<UINT>(dpi));

        if (FAILED(render_target_->CreateSolidColorBrush(
                D2D1::ColorF(0xFFFFFF),
                white_brush_.ReleaseAndGetAddressOf()))) {
            return false;
        }
        if (FAILED(render_target_->CreateSolidColorBrush(
                D2D1::ColorF(0x000000),
                color_brush_.ReleaseAndGetAddressOf()))) {
            return false;
        }
        return true;
    }

    void discard_device_resources() {
        for (AppEntry& app : apps_) {
            app.icon.Reset();
            app.icon_attempted = false;
        }
        color_brush_.Reset();
        white_brush_.Reset();
        background_bitmap_.Reset();
        render_target_.Reset();
    }

    bool ensure_icon(AppEntry& app) {
        if (app.icon) {
            return true;
        }
        if (app.icon_attempted || !render_target_) {
            return false;
        }
        app.icon_attempted = true;

        ComPtr<IShellItem> shell_item;
        if (FAILED(SHCreateItemFromParsingName(
                app.path.c_str(),
                nullptr,
                IID_PPV_ARGS(shell_item.ReleaseAndGetAddressOf())))) {
            return false;
        }
        ComPtr<IShellItemImageFactory> image_factory;
        if (FAILED(shell_item.As(&image_factory))) {
            return false;
        }

        HBITMAP bitmap_handle = nullptr;
        const SIZE requested{
            static_cast<LONG>(icon_request_pixels_),
            static_cast<LONG>(icon_request_pixels_),
        };
        const auto flags = static_cast<SIIGBF>(
            SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY);
        if (FAILED(image_factory->GetImage(
                requested,
                flags,
                &bitmap_handle)) ||
            !bitmap_handle) {
            return false;
        }

        ComPtr<IWICBitmap> wic_bitmap;
        const HRESULT wic_result = wic_factory_->CreateBitmapFromHBITMAP(
            bitmap_handle,
            nullptr,
            WICBitmapUsePremultipliedAlpha,
            wic_bitmap.ReleaseAndGetAddressOf());
        DeleteObject(bitmap_handle);
        if (FAILED(wic_result)) {
            return false;
        }
        return SUCCEEDED(render_target_->CreateBitmapFromWicBitmap(
            wic_bitmap.Get(),
            nullptr,
            app.icon.ReleaseAndGetAddressOf()));
    }

    bool draw_icon_bitmap(
        ID2D1Bitmap* bitmap,
        const D2D1_RECT_F& destination,
        float opacity) {
        if (!bitmap || !render_target_) {
            return false;
        }
        const D2D1_SIZE_U pixel_size = bitmap->GetPixelSize();
        if (pixel_size.width == 0 || pixel_size.height == 0) {
            return false;
        }
        const float destination_width =
            destination.right - destination.left;
        const float destination_height =
            destination.bottom - destination.top;
        if (destination_width <= 0.0F ||
            destination_height <= 0.0F) {
            return false;
        }
        const float scale = std::min(
            destination_width /
                static_cast<float>(pixel_size.width),
            destination_height /
                static_cast<float>(pixel_size.height));
        const float width =
            static_cast<float>(pixel_size.width) * scale;
        const float height =
            static_cast<float>(pixel_size.height) * scale;
        const float left =
            destination.left +
            (destination_width - width) * 0.5F;
        const float top =
            destination.top +
            (destination_height - height) * 0.5F;
        render_target_->DrawBitmap(
            bitmap,
            D2D1::RectF(left, top, left + width, top + height),
            opacity,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        return true;
    }

    void paint() {
        PAINTSTRUCT paint_struct{};
        BeginPaint(hwnd_, &paint_struct);
        if (!ensure_device_resources()) {
            EndPaint(hwnd_, &paint_struct);
            return;
        }

        const float opening_progress = animations_enabled_
            ? std::clamp(
                  spring_ease_out(static_cast<float>(
                      elapsed_seconds() /
                      kOpenAnimationSeconds)),
                  0.0F,
                  1.0F)
            : 1.0F;
        intro_complete_ =
            !animations_enabled_ ||
            elapsed_seconds() >= kOpenAnimationSeconds;
        const float close_progress = closing_
            ? smooth_step(static_cast<float>(
                  elapsed_since(close_animation_origin_) /
                  kCloseAnimationSeconds))
            : 0.0F;
        const float visibility = closing_
            ? close_start_visibility_ * (1.0F - close_progress)
            : opening_progress;
        icons_pending_ = false;
        icon_load_budget_ =
            page_transition_active_ || page_drag_active_ ||
                    drag_active_ || folder_drag_active_ ||
                    folder_animation_active_ ||
                    folder_closing_
                ? 0
                : 1;
        render_target_->BeginDraw();
        render_target_->SetTransform(D2D1::Matrix3x2F::Identity());
        render_target_->Clear(D2D1::ColorF(0x0B0B0D));

        const D2D1_SIZE_F size = render_target_->GetSize();
        const float folder_progress = current_folder_progress();
        const float chrome_visibility =
            visibility * (1.0F - folder_progress);
        draw_background(size, visibility);
        if (chrome_visibility > 0.001F) {
            draw_search(size, chrome_visibility);
            draw_grid(size, chrome_visibility, opening_progress);
            draw_folder_drop_animation(chrome_visibility);
            if (drag_active_ &&
                !(folder_extraction_ &&
                  open_folder_index_ != kNoPage)) {
                draw_drag_preview(visibility);
            }
            warm_adjacent_icons();
            draw_page_dots(size, chrome_visibility);
        } else {
            hit_regions_.clear();
            root_drop_regions_.clear();
            delete_hit_regions_.clear();
            page_dot_regions_.clear();
        }
        if (open_folder_index_ != kNoPage) {
            draw_folder_overlay(
                size,
                visibility * folder_progress);
            if (folder_drag_active_) {
                draw_folder_drag_preview(
                    visibility * folder_progress);
            } else if (drag_active_ && folder_extraction_) {
                draw_drag_preview(visibility);
            }
        }
        if (delete_confirmation_active_) {
            draw_delete_confirmation(size, visibility);
        }

        const HRESULT result = render_target_->EndDraw();
        if (result == D2DERR_RECREATE_TARGET) {
            discard_device_resources();
        }
        EndPaint(hwnd_, &paint_struct);
    }

    void draw_background(const D2D1_SIZE_F& size, float visibility) {
        const D2D1_RECT_F full =
            D2D1::RectF(0.0F, 0.0F, size.width, size.height);
        if (ensure_background_bitmap()) {
            render_target_->DrawBitmap(
                background_bitmap_.Get(),
                full,
                1.0F,
                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            color_brush_->SetColor(D2D1::ColorF(0x151316));
            color_brush_->SetOpacity(1.0F);
            render_target_->FillRectangle(full, color_brush_.Get());
        }

        color_brush_->SetColor(D2D1::ColorF(0x080708));
        color_brush_->SetOpacity(0.38F + 0.14F * visibility);
        render_target_->FillRectangle(full, color_brush_.Get());

        color_brush_->SetColor(D2D1::ColorF(0x241D20));
        color_brush_->SetOpacity(0.035F + 0.045F * visibility);
        render_target_->FillRectangle(full, color_brush_.Get());
    }

    D2D1_ROUNDED_RECT search_box(
        const D2D1_SIZE_F& size) const {
        const float width =
            std::clamp(size.width * 0.17F, 260.0F, 330.0F);
        constexpr float height = 32.0F;
        const float left = (size.width - width) * 0.5F;
        constexpr float top = 36.0F;
        return D2D1_ROUNDED_RECT{
            D2D1::RectF(left, top, left + width, top + height),
            5.0F,
            5.0F,
        };
    }

    float current_search_focus_progress() {
        if (!search_focus_animation_active_) {
            return search_focus_progress_;
        }
        const float raw = static_cast<float>(
            elapsed_since(search_focus_animation_origin_) /
            kSearchFocusAnimationSeconds);
        if (raw >= 1.0F) {
            search_focus_progress_ = search_focus_target_;
            search_focus_animation_active_ = false;
            return search_focus_progress_;
        }
        search_focus_progress_ = lerp(
            search_focus_from_,
            search_focus_target_,
            smooth_step(raw));
        return search_focus_progress_;
    }

    void reset_search_caret_blink(
        bool delayed_reveal = false) {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        search_caret_origin_ = counter.QuadPart;
        search_caret_delayed_reveal_ = delayed_reveal;
        search_caret_last_opacity_ =
            delayed_reveal ? 0.0F : 1.0F;
    }

    float current_search_caret_opacity() const {
        if (!search_focused_) {
            return 0.0F;
        }
        double elapsed = elapsed_since(search_caret_origin_);
        if (search_caret_delayed_reveal_) {
            if (elapsed < kSearchCaretRevealDelaySeconds) {
                return 0.0F;
            }
            elapsed -= kSearchCaretRevealDelaySeconds;
            if (elapsed < kSearchCaretRevealSeconds) {
                return smooth_step(static_cast<float>(
                    elapsed / kSearchCaretRevealSeconds));
            }
            elapsed -= kSearchCaretRevealSeconds;
        }

        const double phase = std::fmod(
            elapsed,
            kSearchCaretBlinkSeconds);
        if (phase < 0.45) {
            return 1.0F;
        }
        if (phase < 0.55) {
            return 1.0F - smooth_step(static_cast<float>(
                (phase - 0.45) / 0.10));
        }
        if (phase < 0.90) {
            return 0.0F;
        }
        return smooth_step(static_cast<float>(
            (phase - 0.90) / 0.10));
    }

    void set_search_focused(
        bool focused,
        bool animate = true) {
        const float target = focused ? 1.0F : 0.0F;
        const float current = current_search_focus_progress();
        if (search_focused_ == focused &&
            std::abs(search_focus_target_ - target) < 0.001F) {
            if (!animate) {
                search_focus_progress_ = target;
                search_focus_from_ = target;
                search_focus_animation_active_ = false;
            }
            if (focused) {
                search_all_selected_ = false;
                reset_search_caret_blink();
            }
            if (hwnd_ && (!animate || focused)) {
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }

        search_focused_ = focused;
        search_focus_from_ = current;
        search_focus_target_ = target;
        if (focused) {
            reset_search_caret_blink(true);
        } else {
            search_all_selected_ = false;
            search_caret_delayed_reveal_ = false;
            search_caret_last_opacity_ = 0.0F;
        }

        const bool should_animate =
            animate &&
            animations_enabled_ &&
            std::abs(target - current) >= 0.001F;
        if (should_animate) {
            LARGE_INTEGER counter{};
            QueryPerformanceCounter(&counter);
            search_focus_animation_origin_ = counter.QuadPart;
            search_focus_animation_active_ = true;
        } else {
            search_focus_progress_ = target;
            search_focus_from_ = target;
            search_focus_animation_active_ = false;
        }
        if (hwnd_) {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void draw_search(const D2D1_SIZE_F& size, float visibility) {
        const D2D1_ROUNDED_RECT box = search_box(size);
        const float width = box.rect.right - box.rect.left;
        const float height = box.rect.bottom - box.rect.top;
        const float focus_progress =
            current_search_focus_progress();

        color_brush_->SetColor(D2D1::ColorF(0x2B292B));
        color_brush_->SetOpacity(0.55F * visibility);
        render_target_->FillRoundedRectangle(box, color_brush_.Get());
        white_brush_->SetOpacity(0.14F * visibility);
        render_target_->DrawRoundedRectangle(
            box,
            white_brush_.Get(),
            1.0F);

        constexpr std::wstring_view search_icon = L"⌕";
        constexpr std::wstring_view placeholder = L"Поиск";
        const std::wstring_view text = search_.empty()
            ? placeholder
            : std::wstring_view(search_);
        constexpr float content_padding = 11.0F;
        constexpr float icon_text_gap = 7.0F;

        ComPtr<IDWriteTextLayout> icon_layout;
        ComPtr<IDWriteTextLayout> text_layout;
        const bool icon_ready =
            SUCCEEDED(write_factory_->CreateTextLayout(
                search_icon.data(),
                static_cast<UINT32>(search_icon.size()),
                search_format_.Get(),
                width,
                height,
                icon_layout.ReleaseAndGetAddressOf())) &&
            SUCCEEDED(icon_layout->SetTextAlignment(
                DWRITE_TEXT_ALIGNMENT_LEADING));
        const bool text_ready =
            SUCCEEDED(write_factory_->CreateTextLayout(
                text.data(),
                static_cast<UINT32>(text.size()),
                search_format_.Get(),
                width,
                height,
                text_layout.ReleaseAndGetAddressOf())) &&
            SUCCEEDED(text_layout->SetTextAlignment(
                DWRITE_TEXT_ALIGNMENT_LEADING));

        DWRITE_TEXT_METRICS icon_metrics{};
        DWRITE_TEXT_METRICS text_metrics{};
        const bool metrics_ready =
            icon_ready &&
            text_ready &&
            SUCCEEDED(icon_layout->GetMetrics(&icon_metrics)) &&
            SUCCEEDED(text_layout->GetMetrics(&text_metrics));
        if (metrics_ready) {
            const float icon_width =
                icon_metrics.widthIncludingTrailingWhitespace;
            const float max_text_width = std::max(
                1.0F,
                width -
                    (content_padding * 2.0F +
                     icon_width + icon_text_gap));
            if (SUCCEEDED(
                    text_layout->SetMaxWidth(max_text_width))) {
                text_layout->GetMetrics(&text_metrics);
            }
            const float text_width =
                text_metrics.widthIncludingTrailingWhitespace;
            const float group_width =
                icon_width + icon_text_gap + text_width;
            const float idle_left =
                box.rect.left +
                std::max(0.0F, (width - group_width) * 0.5F);
            const float focused_left =
                box.rect.left + content_padding;
            const float content_progress = search_.empty()
                ? focus_progress
                : 1.0F;
            const float icon_left = lerp(
                idle_left,
                focused_left,
                content_progress);
            const float text_left =
                icon_left + icon_width + icon_text_gap;
            const float text_scroll =
                search_.empty()
                    ? 0.0F
                    : std::max(
                          0.0F,
                          text_width - max_text_width);
            const float text_draw_left =
                text_left - text_scroll;

            white_brush_->SetOpacity(0.92F * visibility);
            render_target_->DrawTextLayout(
                D2D1::Point2F(icon_left, box.rect.top),
                icon_layout.Get(),
                white_brush_.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);

            render_target_->PushAxisAlignedClip(
                D2D1::RectF(
                    std::max(
                        box.rect.left + content_padding,
                        text_left - 2.5F),
                    box.rect.top,
                    box.rect.right - content_padding,
                    box.rect.bottom),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

            if (!search_.empty() &&
                search_all_selected_ &&
                text_width > 0.0F) {
                const float selection_left =
                    std::max(
                        text_left,
                        text_draw_left +
                            text_metrics.left - 2.0F);
                const float selection_top =
                    std::max(
                        box.rect.top + 4.0F,
                        box.rect.top +
                            text_metrics.top + 2.0F);
                const float selection_right =
                    std::min(
                        box.rect.right - content_padding,
                        text_draw_left +
                            text_metrics.left +
                            text_width + 2.0F);
                const float selection_bottom =
                    std::min(
                        box.rect.bottom - 4.0F,
                        box.rect.top +
                            text_metrics.top +
                            text_metrics.height - 2.0F);
                if (selection_right > selection_left &&
                    selection_bottom > selection_top) {
                    const D2D1_ROUNDED_RECT selection{
                        D2D1::RectF(
                            selection_left,
                            selection_top,
                            selection_right,
                            selection_bottom),
                        3.0F,
                        3.0F,
                    };
                    color_brush_->SetColor(
                        D2D1::ColorF(0x0A84FF));
                    color_brush_->SetOpacity(
                        0.72F * visibility);
                    render_target_->FillRoundedRectangle(
                        selection,
                        color_brush_.Get());
                }
            }

            white_brush_->SetOpacity(
                (search_.empty() ? 0.58F : 0.96F) *
                visibility);
            render_target_->DrawTextLayout(
                D2D1::Point2F(text_draw_left, box.rect.top),
                text_layout.Get(),
                white_brush_.Get(),
                D2D1_DRAW_TEXT_OPTIONS_NONE);

            const float caret_opacity =
                current_search_caret_opacity();
            if (!search_all_selected_ &&
                caret_opacity > 0.001F) {
                const float unclamped_caret_x =
                    search_.empty()
                    ? text_left - 2.5F
                    : text_draw_left +
                        text_metrics.left +
                        text_width + 1.0F;
                const float caret_x = std::clamp(
                    unclamped_caret_x,
                    text_left - 2.5F,
                    box.rect.right - content_padding);
                white_brush_->SetOpacity(
                    0.96F * visibility * caret_opacity);
                render_target_->DrawLine(
                    D2D1::Point2F(
                        caret_x,
                        box.rect.top + 5.5F),
                    D2D1::Point2F(
                        caret_x,
                        box.rect.bottom - 5.5F),
                    white_brush_.Get(),
                    1.4F);
            }
            render_target_->PopAxisAlignedClip();
            return;
        }

        const std::wstring fallback = search_.empty()
            ? L"⌕  Поиск"
            : search_;
        white_brush_->SetOpacity(
            (search_.empty() ? 0.58F : 0.96F) * visibility);
        render_target_->DrawTextW(
            fallback.c_str(),
            static_cast<UINT32>(fallback.size()),
            search_format_.Get(),
            box.rect,
            white_brush_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    DeleteModalGeometry delete_modal_geometry(
        const D2D1_SIZE_F& size) const {
        const float panel_width =
            std::clamp(size.width * 0.31F, 460.0F, 560.0F);
        constexpr float panel_height = 238.0F;
        const float left =
            (size.width - panel_width) * 0.5F;
        const float top =
            (size.height - panel_height) * 0.5F;
        constexpr float button_width = 132.0F;
        constexpr float button_height = 38.0F;
        constexpr float button_gap = 12.0F;
        const float buttons_width =
            button_width * 2.0F + button_gap;
        const float buttons_left =
            left + (panel_width - buttons_width) * 0.5F;
        const float buttons_top =
            top + panel_height - 58.0F;
        return DeleteModalGeometry{
            .panel = D2D1::RoundedRect(
                D2D1::RectF(
                    left,
                    top,
                    left + panel_width,
                    top + panel_height),
                18.0F,
                18.0F),
            .cancel_button = D2D1::RoundedRect(
                D2D1::RectF(
                    buttons_left,
                    buttons_top,
                    buttons_left + button_width,
                    buttons_top + button_height),
                8.0F,
                8.0F),
            .confirm_button = D2D1::RoundedRect(
                D2D1::RectF(
                    buttons_left + button_width + button_gap,
                    buttons_top,
                    buttons_left + button_width * 2.0F +
                        button_gap,
                    buttons_top + button_height),
                8.0F,
                8.0F),
        };
    }

    void draw_delete_confirmation(
        const D2D1_SIZE_F& size,
        float visibility) {
        render_target_->SetTransform(
            D2D1::Matrix3x2F::Identity());
        const DeleteModalGeometry geometry =
            delete_modal_geometry(size);
        const D2D1_RECT_F full =
            D2D1::RectF(0.0F, 0.0F, size.width, size.height);

        color_brush_->SetColor(D2D1::ColorF(0x020203));
        color_brush_->SetOpacity(0.56F * visibility);
        render_target_->FillRectangle(full, color_brush_.Get());

        D2D1_ROUNDED_RECT shadow = geometry.panel;
        shadow.rect.left -= 7.0F;
        shadow.rect.right += 7.0F;
        shadow.rect.top += 7.0F;
        shadow.rect.bottom += 15.0F;
        color_brush_->SetColor(D2D1::ColorF(0x000000));
        color_brush_->SetOpacity(0.46F * visibility);
        render_target_->FillRoundedRectangle(
            shadow,
            color_brush_.Get());

        color_brush_->SetColor(D2D1::ColorF(0x29272B));
        color_brush_->SetOpacity(0.98F * visibility);
        render_target_->FillRoundedRectangle(
            geometry.panel,
            color_brush_.Get());
        white_brush_->SetOpacity(0.16F * visibility);
        render_target_->DrawRoundedRectangle(
            geometry.panel,
            white_brush_.Get(),
            1.0F);

        const std::wstring title =
            L"Удалить «" + pending_delete_name_ + L"»?";
        const D2D1_RECT_F title_bounds = D2D1::RectF(
            geometry.panel.rect.left + 28.0F,
            geometry.panel.rect.top + 22.0F,
            geometry.panel.rect.right - 28.0F,
            geometry.panel.rect.top + 62.0F);
        white_brush_->SetOpacity(0.98F * visibility);
        render_target_->DrawTextW(
            title.c_str(),
            static_cast<UINT32>(title.size()),
            empty_format_.Get(),
            title_bounds,
            white_brush_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);

        const bool executable_file =
            launchpad::lowercase(
                fs::path(pending_delete_path_)
                    .extension()
                    .wstring()) == L".exe";
        const std::wstring body = executable_file
            ? L"Сам файл приложения будет удалён.\n"
                L"Приложение может перестать запускаться."
            : L"Ярлык будет удалён из Launchpad.\n"
                L"Программа останется на компьютере.";
        const D2D1_RECT_F body_bounds = D2D1::RectF(
            geometry.panel.rect.left + 42.0F,
            geometry.panel.rect.top + 68.0F,
            geometry.panel.rect.right - 42.0F,
            geometry.panel.rect.top + 144.0F);
        white_brush_->SetOpacity(0.68F * visibility);
        render_target_->DrawTextW(
            body.c_str(),
            static_cast<UINT32>(body.size()),
            search_format_.Get(),
            body_bounds,
            white_brush_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);

        const bool cancel_pressed =
            delete_modal_pressed_button_ ==
            DeleteModalButton::cancel;
        color_brush_->SetColor(D2D1::ColorF(
            cancel_pressed ? 0x4A474D : 0x3A383D));
        color_brush_->SetOpacity(0.94F * visibility);
        render_target_->FillRoundedRectangle(
            geometry.cancel_button,
            color_brush_.Get());
        white_brush_->SetOpacity(
            (cancel_pressed ? 0.28F : 0.16F) * visibility);
        render_target_->DrawRoundedRectangle(
            geometry.cancel_button,
            white_brush_.Get(),
            1.0F);

        const bool confirm_pressed =
            delete_modal_pressed_button_ ==
            DeleteModalButton::confirm;
        color_brush_->SetColor(D2D1::ColorF(
            confirm_pressed ? 0xB93838 : 0xD94A48));
        color_brush_->SetOpacity(0.98F * visibility);
        render_target_->FillRoundedRectangle(
            geometry.confirm_button,
            color_brush_.Get());

        constexpr std::wstring_view cancel_text = L"Отмена";
        constexpr std::wstring_view confirm_text = L"Удалить";
        white_brush_->SetOpacity(0.96F * visibility);
        render_target_->DrawTextW(
            cancel_text.data(),
            static_cast<UINT32>(cancel_text.size()),
            search_format_.Get(),
            geometry.cancel_button.rect,
            white_brush_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
        render_target_->DrawTextW(
            confirm_text.data(),
            static_cast<UINT32>(confirm_text.size()),
            search_format_.Get(),
            geometry.confirm_button.rect,
            white_brush_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    float current_folder_progress() {
        if (open_folder_index_ == kNoPage) {
            return 0.0F;
        }
        if (!folder_animation_active_) {
            return folder_closing_ ? 0.0F : 1.0F;
        }
        const float raw = static_cast<float>(
            elapsed_since(folder_animation_origin_) /
            kFolderAnimationSeconds);
        if (raw >= 1.0F && !folder_closing_) {
            folder_animation_active_ = false;
            return 1.0F;
        }
        const float progress = folder_closing_
            ? folder_close_start_progress_ *
                (1.0F - smooth_step(raw))
            : ease_out_cubic(raw);
        return std::clamp(progress, 0.0F, 1.0F);
    }

    void open_folder(std::size_t layout_index) {
        if (layout_index >= layout_.items().size() ||
            layout_.items()[layout_index].kind !=
                launchpad::LayoutItemKind::folder ||
            closing_) {
            return;
        }
        set_search_focused(false, false);
        folder_origin_bounds_valid_ = false;
        folder_closing_visual_.reset();
        folder_closing_hidden_position_ = kNoPage;
        for (const HitRegion& region : hit_regions_) {
            if (region.visible_position >= visible_items_.size()) {
                continue;
            }
            const VisibleItem& visible =
                visible_items_[region.visible_position];
            if (visible.kind == VisibleItemKind::folder &&
                visible.layout_index == layout_index) {
                folder_origin_bounds_ = region.icon_bounds;
                folder_origin_bounds_valid_ = true;
                break;
            }
        }
        const launchpad::LayoutItem& folder =
            layout_.items()[layout_index];
        open_folder_app_indices_.clear();
        open_folder_app_indices_.reserve(folder.children.size());
        for (const std::wstring& path : folder.children) {
            open_folder_app_indices_.push_back(
                app_index_for_path(path));
        }
        folder_hit_regions_.clear();
        folder_hit_regions_.reserve(kFolderPageCapacity);
        folder_drop_regions_.clear();
        folder_delete_hit_regions_.clear();
        folder_panel_bounds_valid_ = false;
        folder_drag_candidate_ = false;
        folder_drag_active_ = false;
        folder_drag_source_position_ = kNoPage;
        folder_drag_target_position_ = kNoPage;
        clear_folder_live_reflow();
        open_folder_index_ = layout_index;
        folder_name_editing_ = false;
        folder_name_buffer_.clear();
        folder_selected_position_ = 0;
        folder_page_ = 0;
        folder_selection_visible_ = false;
        folder_closing_ = false;
        folder_close_start_progress_ = 1.0F;
        folder_animation_active_ = animations_enabled_;
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        folder_animation_origin_ = counter.QuadPart;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void close_folder() {
        if (open_folder_index_ == kNoPage || folder_closing_) {
            return;
        }
        if (folder_name_editing_) {
            commit_folder_name_edit();
        }
        if (!animations_enabled_) {
            finish_folder_close();
            return;
        }
        folder_close_start_progress_ =
            current_folder_progress();
        folder_closing_ = true;
        folder_animation_active_ = true;
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        folder_animation_origin_ = counter.QuadPart;
        folder_hit_regions_.clear();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void finish_folder_close() {
        open_folder_index_ = kNoPage;
        folder_selected_position_ = 0;
        folder_page_ = 0;
        folder_closing_ = false;
        folder_animation_active_ = false;
        folder_name_editing_ = false;
        folder_name_buffer_.clear();
        folder_hit_regions_.clear();
        folder_drop_regions_.clear();
        folder_delete_hit_regions_.clear();
        open_folder_app_indices_.clear();
        folder_closing_visual_.reset();
        folder_closing_hidden_position_ = kNoPage;
        folder_panel_bounds_valid_ = false;
        folder_origin_bounds_valid_ = false;
        folder_drag_active_ = false;
        folder_drag_candidate_ = false;
        folder_drag_source_position_ = kNoPage;
        folder_drag_target_position_ = kNoPage;
        clear_folder_live_reflow();
        folder_close_start_progress_ = 1.0F;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    static FolderGeometry calculate_folder_geometry(
        const D2D1_SIZE_F& size,
        std::size_t child_count,
        float reveal,
        const D2D1_RECT_F* origin) {
        const std::size_t visible_count =
            std::min(child_count, kFolderPageCapacity);
        const std::size_t visible_rows = std::clamp<std::size_t>(
            (visible_count + kFolderColumns - 1) /
                kFolderColumns,
            1,
            kFolderRows);
        const float target_width = std::clamp(
            size.width * 0.92F,
            720.0F,
            1760.0F);
        constexpr float top_padding = 28.0F;
        constexpr float bottom_padding = 64.0F;
        constexpr float row_height = 170.0F;
        const float desired_height =
            top_padding + bottom_padding +
            row_height * static_cast<float>(visible_rows);
        const float available_height =
            std::max(240.0F, size.height - 132.0F);
        const float target_height =
            std::min(desired_height, available_height);
        const float panel_width = target_width;
        const float panel_height = target_height;
        const float panel_left =
            (size.width - panel_width) * 0.5F;
        const float panel_top =
            (size.height - panel_height) * 0.5F;
        const D2D1_RECT_F target_panel = D2D1::RectF(
            panel_left,
            panel_top,
            panel_left + panel_width,
            panel_top + panel_height);
        const float morph_progress =
            std::clamp(reveal, 0.0F, 1.0F);
        const D2D1_RECT_F current_panel = origin
            ? lerp_rect(*origin, target_panel, morph_progress)
            : lerp_rect(
                  D2D1::RectF(
                      size.width * 0.5F - panel_width * 0.47F,
                      size.height * 0.5F - panel_height * 0.47F,
                      size.width * 0.5F + panel_width * 0.47F,
                      size.height * 0.5F + panel_height * 0.47F),
                  target_panel,
                  morph_progress);
        const float vertical_padding =
            std::min(
                top_padding + bottom_padding,
                panel_height * 0.35F);
        const float scaled_top_padding =
            vertical_padding *
            (top_padding / (top_padding + bottom_padding));
        const float cell_height =
            (panel_height - vertical_padding) /
            static_cast<float>(visible_rows);
        const float horizontal_padding = std::clamp(
            panel_width * 0.03F,
            32.0F,
            52.0F);
        const float cell_width =
            (panel_width - horizontal_padding * 2.0F) /
            static_cast<float>(kFolderColumns);
        const float icon_size = std::clamp(
            std::min(
                cell_width * 0.52F,
                cell_height * 0.57F),
            54.0F,
            98.0F);
        const float title_bottom =
            panel_top - 18.0F;
        const float title_height = 46.0F;
        const D2D1_RECT_F title = D2D1::RectF(
            panel_left + 32.0F,
            title_bottom - title_height,
            panel_left + panel_width - 32.0F,
            title_bottom);
        const float editor_half_width =
            std::min(240.0F, panel_width * 0.24F);

        return FolderGeometry{
            .panel = D2D1_ROUNDED_RECT{
                current_panel,
                lerp(
                    origin
                        ? std::max(
                              8.0F,
                              (origin->right - origin->left) *
                                  kIconCornerRatio)
                        : 28.0F,
                    34.0F,
                    morph_progress),
                lerp(
                    origin
                        ? std::max(
                              8.0F,
                              (origin->bottom - origin->top) *
                                  kIconCornerRatio)
                        : 28.0F,
                    34.0F,
                    morph_progress),
            },
            .title = title,
            .title_editor = D2D1_ROUNDED_RECT{
                D2D1::RectF(
                    size.width * 0.5F - editor_half_width,
                    title.top - 2.0F,
                    size.width * 0.5F + editor_half_width,
                    title.bottom + 2.0F),
                8.0F,
                8.0F,
            },
            .grid_left = panel_left,
            .horizontal_padding = horizontal_padding,
            .grid_top = panel_top + scaled_top_padding,
            .cell_width = cell_width,
            .cell_height = cell_height,
            .icon_size = icon_size,
            .visible_rows = visible_rows,
        };
    }

    void draw_folder_overlay(
        const D2D1_SIZE_F& size,
        float visibility) {
        folder_hit_regions_.clear();
        folder_drop_regions_.clear();
        folder_delete_hit_regions_.clear();
        folder_panel_bounds_valid_ = false;
        const launchpad::LayoutItem* folder = nullptr;
        if (folder_closing_ && folder_closing_visual_) {
            folder = &*folder_closing_visual_;
        } else if (
            open_folder_index_ < layout_.items().size() &&
            layout_.items()[open_folder_index_].kind ==
                launchpad::LayoutItemKind::folder) {
            folder = &layout_.items()[open_folder_index_];
        }
        if (!folder) {
            return;
        }

        const float content_visibility = smooth_step(
            (visibility - 0.10F) / 0.78F);
        const float title_visibility = smooth_step(
            (visibility - 0.24F) / 0.58F);
        const D2D1_RECT_F full =
            D2D1::RectF(0.0F, 0.0F, size.width, size.height);
        color_brush_->SetColor(D2D1::ColorF(0x050506));
        color_brush_->SetOpacity(0.34F * visibility);
        render_target_->FillRectangle(full, color_brush_.Get());

        const FolderGeometry geometry =
            calculate_folder_geometry(
                size,
                folder->children.size(),
                visibility,
                folder_origin_bounds_valid_
                    ? &folder_origin_bounds_
                    : nullptr);
        const D2D1_ROUNDED_RECT& panel = geometry.panel;
        folder_panel_bounds_ = panel.rect;
        folder_panel_bounds_valid_ = true;
        color_brush_->SetColor(D2D1::ColorF(0xA7A4A5));
        color_brush_->SetOpacity(0.78F * visibility);
        render_target_->FillRoundedRectangle(
            panel,
            color_brush_.Get());
        white_brush_->SetOpacity(0.08F * visibility);
        render_target_->DrawRoundedRectangle(
            panel,
            white_brush_.Get(),
            1.0F);

        const D2D1_RECT_F& title_rect = geometry.title;
        if (folder_name_editing_) {
            color_brush_->SetColor(D2D1::ColorF(0x111012));
            color_brush_->SetOpacity(
                0.58F * title_visibility);
            render_target_->FillRoundedRectangle(
                geometry.title_editor,
                color_brush_.Get());
            white_brush_->SetOpacity(
                0.28F * title_visibility);
            render_target_->DrawRoundedRectangle(
                geometry.title_editor,
                white_brush_.Get(),
                1.0F);
        }
        std::wstring editing_title;
        std::wstring_view title = folder->name;
        if (folder_name_editing_) {
            editing_title = folder_name_buffer_;
            if (std::fmod(elapsed_seconds(), 1.0) < 0.58) {
                editing_title += L"│";
            }
            title = editing_title;
        }
        white_brush_->SetOpacity(
            0.96F * title_visibility);
        render_target_->DrawTextW(
            title.data(),
            static_cast<UINT32>(title.size()),
            folder_title_format_.Get(),
            title_rect,
            white_brush_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);

        const float horizontal_padding =
            geometry.horizontal_padding;
        const float grid_top = geometry.grid_top;
        const float cell_width = geometry.cell_width;
        const float cell_height = geometry.cell_height;
        const float icon_size = geometry.icon_size;
        const std::size_t first =
            folder_page_ * kFolderPageCapacity;
        const std::size_t last = std::min(
            first + kFolderPageCapacity,
            folder->children.size());

        for (std::size_t position = first;
             position < last;
             ++position) {
            if (folder_closing_ &&
                position == folder_closing_hidden_position_) {
                continue;
            }
            const std::size_t logical_local = position - first;
            const std::size_t logical_column =
                logical_local % kFolderColumns;
            const std::size_t logical_row =
                logical_local / kFolderColumns;
            const float logical_center_x =
                geometry.grid_left + horizontal_padding +
                cell_width *
                    (static_cast<float>(logical_column) + 0.5F);
            const float logical_center_y =
                grid_top +
                cell_height *
                    (static_cast<float>(logical_row) + 0.5F);
            const std::size_t visual_position =
                projected_folder_position(position);
            const std::size_t local = visual_position - first;
            const std::size_t column = local % kFolderColumns;
            const std::size_t row = local / kFolderColumns;
            const std::size_t app_index = app_index_for_path(
                folder->children[position]);
            if (app_index == kNoPage) {
                continue;
            }
            AppEntry& app = apps_[app_index];
            bool icon_ready = app.icon.Get() != nullptr;
            if (!icon_ready && !app.icon_attempted) {
                icons_pending_ = true;
                if (icon_load_budget_ > 0) {
                    --icon_load_budget_;
                    icon_ready = ensure_icon(app);
                }
            }
            float center_x =
                geometry.grid_left + horizontal_padding +
                cell_width * (static_cast<float>(column) + 0.5F);
            float center_y =
                grid_top +
                cell_height * (static_cast<float>(row) + 0.5F);
            const std::wstring drag_key =
                launchpad::lowercase(folder->children[position]);
            if (folder_drag_reflow_active_ &&
                !drag_key.empty()) {
                const auto previous =
                    folder_drag_reflow_from_centers_.find(
                        drag_key);
                if (previous !=
                    folder_drag_reflow_from_centers_.end()) {
                    const float reflow_progress =
                        live_reorder_progress(
                            folder_drag_reflow_active_,
                            folder_drag_reflow_origin_);
                    center_x = lerp(
                        previous->second.x,
                        center_x,
                        reflow_progress);
                    center_y = lerp(
                        previous->second.y,
                        center_y,
                        reflow_progress);
                }
            }
            if (folder_drag_active_ && !drag_key.empty()) {
                folder_drag_current_centers_.insert_or_assign(
                    drag_key,
                    D2D1::Point2F(center_x, center_y));
            }
            const bool selected =
                folder_selection_visible_ &&
                position == folder_selected_position_;
            const bool drag_source =
                folder_drag_active_ &&
                position == folder_drag_source_position_;
            const float item_scale =
                selected ? 1.04F : 1.0F;
            const float half = icon_size * item_scale * 0.5F;
            const D2D1_RECT_F target_icon_rect = D2D1::RectF(
                center_x - half,
                center_y - half,
                center_x + half,
                center_y + half);
            D2D1_RECT_F source_icon_rect{};
            if (folder_origin_bounds_valid_ &&
                folder_page_ == 0 &&
                local < 9) {
                const float origin_size =
                    folder_origin_bounds_.right -
                    folder_origin_bounds_.left;
                const float padding = origin_size * 0.16F;
                const float gap = origin_size * 0.055F;
                const float mini_size =
                    (origin_size - padding * 2.0F -
                     gap * 2.0F) /
                    3.0F;
                const float left =
                    folder_origin_bounds_.left + padding +
                    static_cast<float>(local % 3) *
                        (mini_size + gap);
                const float top =
                    folder_origin_bounds_.top + padding +
                    static_cast<float>(local / 3) *
                        (mini_size + gap);
                source_icon_rect = D2D1::RectF(
                    left,
                    top,
                    left + mini_size,
                    top + mini_size);
            } else {
                const float source_half = icon_size * 0.12F;
                const D2D1_POINT_2F source_center =
                    folder_origin_bounds_valid_
                    ? D2D1::Point2F(
                          (folder_origin_bounds_.left +
                           folder_origin_bounds_.right) *
                              0.5F,
                          (folder_origin_bounds_.top +
                           folder_origin_bounds_.bottom) *
                              0.5F)
                    : D2D1::Point2F(center_x, center_y);
                source_icon_rect = D2D1::RectF(
                    source_center.x - source_half,
                    source_center.y - source_half,
                    source_center.x + source_half,
                    source_center.y + source_half);
            }
            const float child_delay =
                std::min(0.22F, static_cast<float>(local) * 0.010F);
            const float child_progress = std::clamp(
                (visibility - child_delay) /
                    (1.0F - child_delay),
                0.0F,
                1.0F);
            const D2D1_RECT_F icon_rect = lerp_rect(
                source_icon_rect,
                target_icon_rect,
                child_progress);
            const float animated_center_x =
                (icon_rect.left + icon_rect.right) * 0.5F;
            const float animated_center_y =
                (icon_rect.top + icon_rect.bottom) * 0.5F;
            const float item_opacity =
                (local < 9 && folder_page_ == 0
                     ? visibility
                     : content_visibility) *
                (drag_source ? 0.05F : 1.0F);
            const float animated_size =
                icon_rect.right - icon_rect.left;
            const D2D1_ROUNDED_RECT rounded{
                icon_rect,
                animated_size * kIconCornerRatio,
                animated_size * kIconCornerRatio,
            };
            if (edit_mode_ && !drag_source) {
                const float phase =
                    static_cast<float>(elapsed_seconds() * 12.0) +
                    static_cast<float>(position) * 1.73F;
                const float angle = std::sin(phase) * 1.15F;
                render_target_->SetTransform(
                    D2D1::Matrix3x2F::Rotation(
                        angle,
                        D2D1::Point2F(
                            animated_center_x,
                            animated_center_y)));
            }
            if (selected) {
                white_brush_->SetOpacity(
                    0.12F * item_opacity);
                const D2D1_ROUNDED_RECT highlight{
                    D2D1::RectF(
                        icon_rect.left - 6.0F,
                        icon_rect.top - 6.0F,
                        icon_rect.right + 6.0F,
                        icon_rect.bottom + 6.0F),
                    icon_size * 0.26F,
                    icon_size * 0.26F,
                };
                render_target_->FillRoundedRectangle(
                    highlight,
                    white_brush_.Get());
            }
            if (icon_ready) {
                draw_icon_bitmap(
                    app.icon.Get(),
                    icon_rect,
                    item_opacity);
            } else {
                color_brush_->SetColor(D2D1::ColorF(app.color));
                color_brush_->SetOpacity(0.90F * item_opacity);
                render_target_->FillRoundedRectangle(
                    rounded,
                    color_brush_.Get());
                const wchar_t glyph[] = {app.glyph, L'\0'};
                white_brush_->SetOpacity(0.94F * item_opacity);
                render_target_->DrawTextW(
                    glyph,
                    1,
                    glyph_format_.Get(),
                    icon_rect,
                    white_brush_.Get());
            }
            const bool interactions_ready =
                visibility >= 0.985F && !folder_closing_;
            if (edit_mode_ && !drag_source &&
                interactions_ready) {
                const D2D1_RECT_F delete_bounds =
                    draw_delete_button(icon_rect, item_opacity);
                if (!folder_drag_reflow_active_) {
                    folder_delete_hit_regions_.push_back(HitRegion{
                        .bounds = delete_bounds,
                        .icon_bounds = icon_rect,
                        .visible_position = position,
                    });
                }
            }
            white_brush_->SetOpacity(
                0.94F * content_visibility *
                (drag_source ? 0.05F : 1.0F));
            const D2D1_RECT_F label = D2D1::RectF(
                animated_center_x - cell_width * 0.46F,
                animated_center_y + half + 8.0F,
                animated_center_x + cell_width * 0.46F,
                animated_center_y + half + 32.0F);
            render_target_->DrawTextW(
                app.name.c_str(),
                static_cast<UINT32>(app.name.size()),
                label_format_.Get(),
                label,
                white_brush_.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
            if (interactions_ready &&
                !folder_drag_reflow_active_) {
                folder_hit_regions_.push_back(HitRegion{
                    .bounds = icon_rect,
                    .icon_bounds = icon_rect,
                    .visible_position = position,
                });
            }
            if (folder_drag_active_ && interactions_ready) {
                folder_drop_regions_.push_back(HitRegion{
                    .bounds = D2D1::RectF(
                        logical_center_x - cell_width * 0.5F,
                        logical_center_y - cell_height * 0.5F,
                        logical_center_x + cell_width * 0.5F,
                        logical_center_y + cell_height * 0.5F),
                    .icon_bounds = D2D1::RectF(
                        logical_center_x - icon_size * 0.5F,
                        logical_center_y - icon_size * 0.5F,
                        logical_center_x + icon_size * 0.5F,
                        logical_center_y + icon_size * 0.5F),
                    .visible_position = position,
                });
            }
            render_target_->SetTransform(
                D2D1::Matrix3x2F::Identity());
        }

        const std::size_t page_count = std::max<std::size_t>(
            1,
            (folder->children.size() + kFolderPageCapacity - 1) /
                kFolderPageCapacity);
        if (page_count > 1) {
            const float spacing = 16.0F;
            const float first_x =
                size.width * 0.5F -
                spacing * static_cast<float>(page_count - 1) * 0.5F;
            const float y = panel.rect.bottom - 22.0F;
            for (std::size_t page = 0; page < page_count; ++page) {
                white_brush_->SetOpacity(
                    (page == folder_page_ ? 0.90F : 0.28F) *
                    content_visibility);
                render_target_->FillEllipse(
                    D2D1::Ellipse(
                        D2D1::Point2F(
                            first_x + spacing *
                                static_cast<float>(page),
                            y),
                        3.5F,
                        3.5F),
                    white_brush_.Get());
            }
        }
    }

    void draw_folder_drag_preview(float visibility) {
        if (!folder_drag_active_ ||
            open_folder_index_ >= layout_.items().size()) {
            return;
        }
        const launchpad::LayoutItem& folder =
            layout_.items()[open_folder_index_];
        if (folder.kind != launchpad::LayoutItemKind::folder ||
            folder_drag_source_position_ >= folder.children.size()) {
            return;
        }
        const std::size_t app_index = app_index_for_path(
            folder.children[folder_drag_source_position_]);
        if (app_index == kNoPage) {
            return;
        }
        AppEntry& app = apps_[app_index];
        const D2D1_POINT_2F point = client_point_to_dips(
            folder_drag_current_x_,
            folder_drag_current_y_);
        constexpr float icon_size = 90.0F;
        constexpr float half = icon_size * 0.5F;
        const D2D1_RECT_F icon_rect = D2D1::RectF(
            point.x - half,
            point.y - half,
            point.x + half,
            point.y + half);
        const D2D1_ROUNDED_RECT rounded{
            icon_rect,
            icon_size * kIconCornerRatio,
            icon_size * kIconCornerRatio,
        };
        if (app.icon) {
            draw_icon_bitmap(
                app.icon.Get(),
                icon_rect,
                visibility);
        } else {
            color_brush_->SetColor(D2D1::ColorF(app.color));
            color_brush_->SetOpacity(0.98F * visibility);
            render_target_->FillRoundedRectangle(
                rounded,
                color_brush_.Get());
            const wchar_t glyph[] = {app.glyph, L'\0'};
            white_brush_->SetOpacity(0.96F * visibility);
            render_target_->DrawTextW(
                glyph,
                1,
                glyph_format_.Get(),
                icon_rect,
                white_brush_.Get());
        }
        const D2D1_RECT_F label = D2D1::RectF(
            point.x - 110.0F,
            icon_rect.bottom + 8.0F,
            point.x + 110.0F,
            icon_rect.bottom + 32.0F);
        white_brush_->SetOpacity(0.98F * visibility);
        render_target_->DrawTextW(
            app.name.c_str(),
            static_cast<UINT32>(app.name.size()),
            label_format_.Get(),
            label,
            white_brush_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void draw_drag_preview(float visibility) {
        if (!drag_active_ ||
            drag_source_visible_position_ >= visible_items_.size()) {
            return;
        }
        const VisibleItem& visible =
            visible_items_[drag_source_visible_position_];
        AppEntry* app = app_for_visible(visible);
        const launchpad::LayoutItem* folder =
            visible.kind == VisibleItemKind::folder &&
                    visible.layout_index < layout_.items().size()
                ? &layout_.items()[visible.layout_index]
                : nullptr;
        if (!app && !folder) {
            return;
        }

        const D2D1_POINT_2F point = client_point_to_dips(
            drag_current_x_,
            drag_current_y_);
        constexpr float icon_size = 102.0F;
        constexpr float half = icon_size * 0.5F;
        const D2D1_RECT_F icon_rect = D2D1::RectF(
            point.x - half,
            point.y - half,
            point.x + half,
            point.y + half);
        const D2D1_ROUNDED_RECT rounded{
            icon_rect,
            icon_size * kIconCornerRatio,
            icon_size * kIconCornerRatio,
        };
        const D2D1_ROUNDED_RECT shadow{
            D2D1::RectF(
                icon_rect.left - 5.0F,
                icon_rect.top + 7.0F,
                icon_rect.right + 5.0F,
                icon_rect.bottom + 15.0F),
            icon_size * kIconCornerRatio,
            icon_size * kIconCornerRatio,
        };
        if (folder) {
            color_brush_->SetColor(D2D1::ColorF(0x000000));
            color_brush_->SetOpacity(0.42F * visibility);
            render_target_->FillRoundedRectangle(
                shadow,
                color_brush_.Get());
            color_brush_->SetColor(D2D1::ColorF(0x77747B));
            color_brush_->SetOpacity(0.66F * visibility);
            render_target_->FillRoundedRectangle(
                rounded,
                color_brush_.Get());
            const float padding = icon_size * 0.16F;
            const float gap = icon_size * 0.055F;
            const float mini_size =
                (icon_size - padding * 2.0F - gap * 2.0F) / 3.0F;
            const std::size_t preview_count =
                std::min<std::size_t>(9, folder->children.size());
            for (std::size_t child = 0;
                 child < preview_count;
                 ++child) {
                const std::size_t app_index =
                    app_index_for_path(folder->children[child]);
                if (app_index == kNoPage) {
                    continue;
                }
                AppEntry& child_app = apps_[app_index];
                const float left =
                    icon_rect.left + padding +
                    static_cast<float>(child % 3) *
                        (mini_size + gap);
                const float top =
                    icon_rect.top + padding +
                    static_cast<float>(child / 3) *
                        (mini_size + gap);
                const D2D1_RECT_F mini_rect = D2D1::RectF(
                    left,
                    top,
                    left + mini_size,
                    top + mini_size);
                const D2D1_ROUNDED_RECT mini{
                    mini_rect,
                    mini_size * kIconCornerRatio,
                    mini_size * kIconCornerRatio,
                };
                if (child_app.icon) {
                    draw_icon_bitmap(
                        child_app.icon.Get(),
                        mini_rect,
                        visibility);
                } else {
                    color_brush_->SetColor(
                        D2D1::ColorF(child_app.color));
                    color_brush_->SetOpacity(0.96F * visibility);
                    render_target_->FillRoundedRectangle(
                        mini,
                        color_brush_.Get());
                }
            }
            white_brush_->SetOpacity(0.22F * visibility);
            render_target_->DrawRoundedRectangle(
                rounded,
                white_brush_.Get(),
                1.0F);
        } else if (app->icon) {
            draw_icon_bitmap(
                app->icon.Get(),
                icon_rect,
                visibility);
        } else {
            color_brush_->SetColor(D2D1::ColorF(app->color));
            color_brush_->SetOpacity(0.98F * visibility);
            render_target_->FillRoundedRectangle(
                rounded,
                color_brush_.Get());
            const wchar_t glyph[] = {app->glyph, L'\0'};
            white_brush_->SetOpacity(0.96F * visibility);
            render_target_->DrawTextW(
                glyph,
                1,
                glyph_format_.Get(),
                icon_rect,
                white_brush_.Get());
        }

        const std::wstring& name =
            folder ? folder->name : app->name;
        const D2D1_RECT_F label = D2D1::RectF(
            point.x - 120.0F,
            icon_rect.bottom + 10.0F,
            point.x + 120.0F,
            icon_rect.bottom + 34.0F);
        white_brush_->SetOpacity(0.98F * visibility);
        render_target_->DrawTextW(
            name.c_str(),
            static_cast<UINT32>(name.size()),
            label_format_.Get(),
            label,
            white_brush_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    D2D1_RECT_F draw_delete_button(
        const D2D1_RECT_F& icon_rect,
        float visibility) {
        const D2D1_POINT_2F center = D2D1::Point2F(
            icon_rect.right - 1.5F,
            icon_rect.top + 1.5F);
        constexpr float radius = 10.5F;
        color_brush_->SetColor(D2D1::ColorF(0x5D5B61));
        color_brush_->SetOpacity(0.98F * visibility);
        render_target_->FillEllipse(
            D2D1::Ellipse(center, radius, radius),
            color_brush_.Get());
        white_brush_->SetOpacity(0.46F * visibility);
        render_target_->DrawEllipse(
            D2D1::Ellipse(center, radius, radius),
            white_brush_.Get(),
            0.8F);
        white_brush_->SetOpacity(0.96F * visibility);
        constexpr float arm = 3.8F;
        render_target_->DrawLine(
            D2D1::Point2F(center.x - arm, center.y - arm),
            D2D1::Point2F(center.x + arm, center.y + arm),
            white_brush_.Get(),
            1.8F);
        render_target_->DrawLine(
            D2D1::Point2F(center.x + arm, center.y - arm),
            D2D1::Point2F(center.x - arm, center.y + arm),
            white_brush_.Get(),
            1.8F);
        constexpr float hit_radius = 14.0F;
        return D2D1::RectF(
            center.x - hit_radius,
            center.y - hit_radius,
            center.x + hit_radius,
            center.y + hit_radius);
    }

    std::wstring root_visual_key(
        const VisibleItem& visible) const {
        if (visible.kind == VisibleItemKind::app) {
            if (visible.app_index >= apps_.size()) {
                return {};
            }
            return L"A|" +
                launchpad::lowercase(
                    apps_[visible.app_index].path);
        }
        if (visible.layout_index >= layout_.items().size()) {
            return {};
        }
        const launchpad::LayoutItem& folder =
            layout_.items()[visible.layout_index];
        if (folder.kind != launchpad::LayoutItemKind::folder) {
            return {};
        }
        std::wstring key =
            L"F|" + launchpad::lowercase(folder.name);
        if (!folder.children.empty()) {
            key += L"|" +
                launchpad::lowercase(folder.children.front());
        }
        return key;
    }

    bool root_live_reorder_active(
        std::size_t page) const noexcept {
        return drag_active_ &&
            folder_drop_target_visible_position_ == kNoPage &&
            !drag_folder_intent_locked_ &&
            drag_source_visible_position_ < visible_items_.size() &&
            drag_target_visible_position_ < visible_items_.size() &&
            visible_page_for_position(
                drag_source_visible_position_) == page &&
            visible_page_for_position(
                drag_target_visible_position_) == page;
    }

    std::size_t projected_root_position(
        std::size_t position,
        std::size_t page) const noexcept {
        if (!root_live_reorder_active(page)) {
            return position;
        }
        return projected_reorder_position(
            position,
            drag_source_visible_position_,
            drag_target_visible_position_);
    }

    float live_reorder_progress(
        bool active,
        std::int64_t origin) const {
        if (!active || !animations_enabled_) {
            return 1.0F;
        }
        return ease_out_cubic(static_cast<float>(
            elapsed_since(origin) /
            kLiveReorderAnimationSeconds));
    }

    void seed_root_drag_centers() {
        if (!root_drag_current_centers_.empty()) {
            return;
        }
        for (const HitRegion& region : hit_regions_) {
            if (region.visible_position >= visible_items_.size()) {
                continue;
            }
            const std::wstring key = root_visual_key(
                visible_items_[region.visible_position]);
            if (key.empty()) {
                continue;
            }
            root_drag_current_centers_.insert_or_assign(
                key,
                D2D1::Point2F(
                    (region.icon_bounds.left +
                     region.icon_bounds.right) *
                        0.5F,
                    (region.icon_bounds.top +
                     region.icon_bounds.bottom) *
                        0.5F));
        }
    }

    void begin_root_live_reflow() {
        seed_root_drag_centers();
        root_drag_reflow_from_centers_ =
            root_drag_current_centers_;
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        root_drag_reflow_origin_ = counter.QuadPart;
        root_drag_reflow_active_ =
            animations_enabled_ &&
            !root_drag_reflow_from_centers_.empty();
    }

    void clear_root_live_reflow() {
        root_drag_reflow_active_ = false;
        root_drag_reflow_from_centers_.clear();
        root_drag_current_centers_.clear();
    }

    void seed_folder_drag_centers() {
        if (!folder_drag_current_centers_.empty() ||
            open_folder_index_ >= layout_.items().size()) {
            return;
        }
        const launchpad::LayoutItem& folder =
            layout_.items()[open_folder_index_];
        if (folder.kind != launchpad::LayoutItemKind::folder) {
            return;
        }
        for (const HitRegion& region : folder_hit_regions_) {
            if (region.visible_position >= folder.children.size()) {
                continue;
            }
            folder_drag_current_centers_.insert_or_assign(
                launchpad::lowercase(
                    folder.children[region.visible_position]),
                D2D1::Point2F(
                    (region.icon_bounds.left +
                     region.icon_bounds.right) *
                        0.5F,
                    (region.icon_bounds.top +
                     region.icon_bounds.bottom) *
                        0.5F));
        }
    }

    void begin_folder_live_reflow() {
        seed_folder_drag_centers();
        folder_drag_reflow_from_centers_ =
            folder_drag_current_centers_;
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        folder_drag_reflow_origin_ = counter.QuadPart;
        folder_drag_reflow_active_ =
            animations_enabled_ &&
            !folder_drag_reflow_from_centers_.empty();
    }

    void clear_folder_live_reflow() {
        folder_drag_reflow_active_ = false;
        folder_drag_reflow_from_centers_.clear();
        folder_drag_current_centers_.clear();
    }

    bool folder_live_reorder_active() const noexcept {
        return folder_drag_active_ &&
            folder_drag_source_position_ != kNoPage &&
            folder_drag_target_position_ != kNoPage &&
            folder_drag_source_position_ /
                    kFolderPageCapacity ==
                folder_page_ &&
            folder_drag_target_position_ /
                    kFolderPageCapacity ==
                folder_page_;
    }

    std::size_t projected_folder_position(
        std::size_t position) const noexcept {
        if (!folder_live_reorder_active()) {
            return position;
        }
        return projected_reorder_position(
            position,
            folder_drag_source_position_,
            folder_drag_target_position_);
    }

    void capture_root_reflow_positions() {
        root_reflow_from_centers_.clear();
        for (const HitRegion& region : hit_regions_) {
            if (region.visible_position >= visible_items_.size()) {
                continue;
            }
            const std::wstring key = root_visual_key(
                visible_items_[region.visible_position]);
            if (key.empty()) {
                continue;
            }
            root_reflow_from_centers_.insert_or_assign(
                key,
                D2D1::Point2F(
                    (region.icon_bounds.left +
                     region.icon_bounds.right) *
                        0.5F,
                    (region.icon_bounds.top +
                     region.icon_bounds.bottom) *
                        0.5F));
        }
    }

    float current_folder_drop_progress() const {
        if (!folder_drop_animation_active_) {
            return 1.0F;
        }
        return std::clamp(
            static_cast<float>(
                elapsed_since(folder_drop_animation_origin_) /
                kFolderDropAnimationSeconds),
            0.0F,
            1.0F);
    }

    void draw_folder_drop_animation(float visibility) {
        if (!folder_drop_animation_active_ ||
            !folder_drop_target_bounds_valid_) {
            return;
        }
        const std::size_t app_index = app_index_for_path(
            folder_drop_animation_path_);
        if (app_index == kNoPage) {
            return;
        }
        AppEntry& app = apps_[app_index];
        const float raw = current_folder_drop_progress();
        const float progress = ease_out_cubic(raw);
        const D2D1_RECT_F icon_rect = lerp_rect(
            folder_drop_animation_from_bounds_,
            folder_drop_animation_target_bounds_,
            progress);
        const float opacity =
            (1.0F -
             smooth_step((raw - 0.68F) / 0.32F)) *
            visibility;
        if (opacity <= 0.001F) {
            return;
        }
        if (app.icon) {
            draw_icon_bitmap(
                app.icon.Get(),
                icon_rect,
                opacity);
            return;
        }
        const float size = icon_rect.right - icon_rect.left;
        const D2D1_ROUNDED_RECT rounded{
            icon_rect,
            size * kIconCornerRatio,
            size * kIconCornerRatio,
        };
        color_brush_->SetColor(D2D1::ColorF(app.color));
        color_brush_->SetOpacity(0.96F * opacity);
        render_target_->FillRoundedRectangle(
            rounded,
            color_brush_.Get());
    }

    void draw_page(
        std::size_t page,
        float page_offset,
        float page_opacity,
        float visibility,
        float opening_progress,
        bool register_hit_regions,
        float horizontal_margin,
        float top,
        float cell_width,
        float cell_height,
        float icon_size) {
        const auto [first, last] = visible_page_range(page);
        const float motion_scale = closing_
            ? 0.82F + 0.18F * visibility
            : 0.78F + 0.22F * opening_progress;
        const float base_item_opacity =
            visibility * page_opacity;

        for (std::size_t position = first; position < last; ++position) {
            const std::size_t logical_page_position =
                position - first;
            const int logical_column = static_cast<int>(
                logical_page_position %
                launchpad::kGridColumns);
            const int logical_row = static_cast<int>(
                logical_page_position /
                launchpad::kGridColumns);
            const float logical_center_x =
                horizontal_margin +
                cell_width * (logical_column + 0.5F) +
                page_offset;
            const float logical_center_y =
                top + cell_height * (logical_row + 0.43F);
            const std::size_t visual_position =
                projected_root_position(position, page);
            const std::size_t page_position =
                visual_position - first;
            const int column = static_cast<int>(
                page_position % launchpad::kGridColumns);
            const int row = static_cast<int>(
                page_position / launchpad::kGridColumns);
            const VisibleItem& visible = visible_items_[position];
            const float target_center_x =
                horizontal_margin +
                cell_width * (column + 0.5F) +
                page_offset;
            const float target_center_y =
                top + cell_height * (row + 0.43F);
            float center_x = target_center_x;
            float center_y = target_center_y;
            if (root_reflow_animation_active_ &&
                page == current_page_ &&
                std::abs(page_offset) < 0.01F) {
                const auto previous =
                    root_reflow_from_centers_.find(
                        root_visual_key(visible));
                if (previous !=
                    root_reflow_from_centers_.end()) {
                    const float reflow_progress =
                        ease_out_cubic(static_cast<float>(
                            elapsed_since(
                                root_reflow_animation_origin_) /
                            kRootReflowAnimationSeconds));
                    center_x = lerp(
                        previous->second.x,
                        target_center_x,
                        reflow_progress);
                    center_y = lerp(
                        previous->second.y,
                        target_center_y,
                        reflow_progress);
                }
            }
            const std::wstring visual_key =
                root_visual_key(visible);
            if (root_drag_reflow_active_ &&
                drag_active_ &&
                !visual_key.empty()) {
                const auto previous =
                    root_drag_reflow_from_centers_.find(
                        visual_key);
                if (previous !=
                    root_drag_reflow_from_centers_.end()) {
                    const float reflow_progress =
                        live_reorder_progress(
                            root_drag_reflow_active_,
                            root_drag_reflow_origin_);
                    center_x = lerp(
                        previous->second.x,
                        center_x,
                        reflow_progress);
                    center_y = lerp(
                        previous->second.y,
                        center_y,
                        reflow_progress);
                }
            }
            if (drag_active_ && !visual_key.empty()) {
                root_drag_current_centers_.insert_or_assign(
                    visual_key,
                    D2D1::Point2F(center_x, center_y));
            }
            const bool selected =
                selection_visible_ && position == selected_position_;
            const bool drag_source =
                drag_active_ &&
                position == drag_source_visible_position_;
            const bool folder_drop_target =
                drag_active_ &&
                position ==
                    folder_drop_target_visible_position_;
            const bool folder_intent_target =
                drag_active_ &&
                drag_folder_intent_locked_ &&
                position ==
                    folder_hover_candidate_visible_position_;
            const bool accepting_folder =
                folder_drop_animation_active_ &&
                visible.kind == VisibleItemKind::folder &&
                visible.layout_index ==
                    folder_drop_animation_target_layout_index_;
            constexpr float pi = 3.14159265358979323846F;
            const float accept_pulse = accepting_folder
                ? 1.0F +
                    std::sin(
                        pi * current_folder_drop_progress()) *
                        0.075F
                : 1.0F;
            const float scale =
                motion_scale *
                accept_pulse *
                (folder_drop_target
                     ? 1.12F
                     : (folder_intent_target
                            ? 1.065F
                            : (selected ? 1.035F : 1.0F)));
            const float item_opacity =
                base_item_opacity *
                (drag_source ? 0.06F : 1.0F);
            const float half = icon_size * scale * 0.5F;
            const D2D1_RECT_F icon_rect = D2D1::RectF(
                center_x - half,
                center_y - half,
                center_x + half,
                center_y + half);
            const float actual_icon_size =
                icon_rect.right - icon_rect.left;
            const D2D1_ROUNDED_RECT rounded_icon{
                icon_rect,
                actual_icon_size * kIconCornerRatio,
                actual_icon_size * kIconCornerRatio,
            };

            AppEntry* app = app_for_visible(visible);
            const launchpad::LayoutItem* folder =
                visible.kind == VisibleItemKind::folder &&
                        visible.layout_index < layout_.items().size()
                    ? &layout_.items()[visible.layout_index]
                    : nullptr;
            if (!app && !folder) {
                continue;
            }
            if (edit_mode_ && !drag_source &&
                !page_transition_active_) {
                const float phase =
                    static_cast<float>(elapsed_seconds() * 12.0) +
                    static_cast<float>(position) * 1.73F;
                const float angle = std::sin(phase) * 1.15F;
                render_target_->SetTransform(
                    D2D1::Matrix3x2F::Rotation(
                        angle,
                        D2D1::Point2F(center_x, center_y)));
            }
            if (selected || folder_drop_target ||
                folder_intent_target) {
                white_brush_->SetOpacity(
                    (folder_drop_target
                         ? 0.18F
                         : (folder_intent_target
                                ? 0.12F
                                : 0.10F)) *
                    item_opacity);
                const D2D1_ROUNDED_RECT highlight{
                    D2D1::RectF(
                        icon_rect.left - 6.0F,
                        icon_rect.top - 6.0F,
                        icon_rect.right + 6.0F,
                        icon_rect.bottom + 6.0F),
                    icon_size * 0.26F,
                    icon_size * 0.26F,
                };
                render_target_->FillRoundedRectangle(
                    highlight,
                    white_brush_.Get());
            }

            bool icon_ready = app && app->icon.Get() != nullptr;
            if (app && !icon_ready &&
                !app->icon_attempted &&
                open_folder_index_ == kNoPage) {
                icons_pending_ = true;
                if (icon_load_budget_ > 0) {
                    --icon_load_budget_;
                    icon_ready = ensure_icon(*app);
                }
            }

            if (folder) {
                const D2D1_RECT_F shadow_rect = D2D1::RectF(
                    icon_rect.left - 1.0F,
                    icon_rect.top + 2.0F,
                    icon_rect.right + 1.0F,
                    icon_rect.bottom + 4.0F);
                const D2D1_ROUNDED_RECT shadow{
                    shadow_rect,
                    actual_icon_size * kIconCornerRatio,
                    actual_icon_size * kIconCornerRatio,
                };
                color_brush_->SetColor(D2D1::ColorF(0x000000));
                color_brush_->SetOpacity(0.24F * item_opacity);
                render_target_->FillRoundedRectangle(
                    shadow,
                    color_brush_.Get());
                color_brush_->SetColor(D2D1::ColorF(0x77747B));
                color_brush_->SetOpacity(0.56F * item_opacity);
                render_target_->FillRoundedRectangle(
                    rounded_icon,
                    color_brush_.Get());
                if (accepting_folder) {
                    const float fallback_half =
                        actual_icon_size * 0.12F;
                    folder_drop_animation_target_bounds_ =
                        D2D1::RectF(
                            center_x - fallback_half,
                            center_y - fallback_half,
                            center_x + fallback_half,
                            center_y + fallback_half);
                    folder_drop_target_bounds_valid_ = true;
                }
                const float padding = actual_icon_size * 0.16F;
                const float gap = actual_icon_size * 0.055F;
                const float mini_size =
                    (actual_icon_size - padding * 2.0F - gap * 2.0F) /
                    3.0F;
                const std::size_t preview_count =
                    std::min<std::size_t>(9, folder->children.size());
                for (std::size_t child = 0;
                     child < preview_count;
                     ++child) {
                    const std::size_t child_app_index =
                        app_index_for_path(
                            folder->children[child]);
                    if (child_app_index == kNoPage) {
                        continue;
                    }
                    AppEntry& child_app = apps_[child_app_index];
                    bool child_ready = child_app.icon.Get() != nullptr;
                    if (!child_ready &&
                        !child_app.icon_attempted &&
                        open_folder_index_ == kNoPage) {
                        icons_pending_ = true;
                        if (icon_load_budget_ > 0) {
                            --icon_load_budget_;
                            child_ready = ensure_icon(child_app);
                        }
                    }
                    const float mini_left =
                        icon_rect.left + padding +
                        static_cast<float>(child % 3) *
                            (mini_size + gap);
                    const float mini_top =
                        icon_rect.top + padding +
                        static_cast<float>(child / 3) *
                            (mini_size + gap);
                    const D2D1_RECT_F mini_rect = D2D1::RectF(
                        mini_left,
                        mini_top,
                        mini_left + mini_size,
                        mini_top + mini_size);
                    const D2D1_ROUNDED_RECT mini_rounded{
                        mini_rect,
                        mini_size * kIconCornerRatio,
                        mini_size * kIconCornerRatio,
                    };
                    const bool animated_drop_child =
                        folder_drop_animation_active_ &&
                        visible.layout_index ==
                            folder_drop_animation_target_layout_index_ &&
                        launchpad::lowercase(
                            folder->children[child]) ==
                            launchpad::lowercase(
                                folder_drop_animation_path_);
                    float child_opacity = item_opacity;
                    if (animated_drop_child) {
                        folder_drop_animation_target_bounds_ =
                            mini_rect;
                        folder_drop_target_bounds_valid_ = true;
                        child_opacity *= smooth_step(
                            (current_folder_drop_progress() -
                             0.52F) /
                            0.48F);
                    }
                    if (child_ready) {
                        draw_icon_bitmap(
                            child_app.icon.Get(),
                            mini_rect,
                            child_opacity);
                    } else {
                        color_brush_->SetColor(
                            D2D1::ColorF(child_app.color));
                        color_brush_->SetOpacity(
                            0.92F * child_opacity);
                        render_target_->FillRoundedRectangle(
                            mini_rounded,
                            color_brush_.Get());
                    }
                }
                white_brush_->SetOpacity(0.14F * item_opacity);
                render_target_->DrawRoundedRectangle(
                    rounded_icon,
                    white_brush_.Get(),
                    1.0F);
            } else if (icon_ready) {
                draw_icon_bitmap(
                    app->icon.Get(),
                    icon_rect,
                    item_opacity);
            } else {
                color_brush_->SetColor(D2D1::ColorF(app->color));
                color_brush_->SetOpacity(0.88F * item_opacity);
                render_target_->FillRoundedRectangle(
                    rounded_icon,
                    color_brush_.Get());
                const wchar_t glyph[] = {app->glyph, L'\0'};
                white_brush_->SetOpacity(0.94F * item_opacity);
                render_target_->DrawTextW(
                    glyph,
                    1,
                    glyph_format_.Get(),
                    icon_rect,
                    white_brush_.Get());
            }

            if (edit_mode_ && app && !drag_source &&
                search_.empty()) {
                const D2D1_RECT_F delete_bounds =
                    draw_delete_button(icon_rect, item_opacity);
                if (register_hit_regions) {
                    delete_hit_regions_.push_back(HitRegion{
                        .bounds = delete_bounds,
                        .icon_bounds = icon_rect,
                        .visible_position = position,
                    });
                }
            }

            const D2D1_RECT_F label = D2D1::RectF(
                center_x - cell_width * 0.46F,
                center_y + half + 9.0F,
                center_x + cell_width * 0.46F,
                center_y + half + 34.0F);
            white_brush_->SetOpacity(0.94F * item_opacity);
            render_target_->DrawTextW(
                folder ? folder->name.c_str() : app->name.c_str(),
                static_cast<UINT32>(
                    folder ? folder->name.size() : app->name.size()),
                label_format_.Get(),
                label,
                white_brush_.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);

            if (register_hit_regions) {
                hit_regions_.push_back(HitRegion{
                    .bounds = icon_rect,
                    .icon_bounds = icon_rect,
                    .visible_position = position,
                });
                root_drop_regions_.push_back(HitRegion{
                    .bounds = D2D1::RectF(
                        logical_center_x - cell_width * 0.5F,
                        logical_center_y - cell_height * 0.5F,
                        logical_center_x + cell_width * 0.5F,
                        logical_center_y + cell_height * 0.5F),
                    .icon_bounds = D2D1::RectF(
                        logical_center_x - icon_size * 0.5F,
                        logical_center_y - icon_size * 0.5F,
                        logical_center_x + icon_size * 0.5F,
                        logical_center_y + icon_size * 0.5F),
                    .visible_position = position,
                });
            }
            render_target_->SetTransform(
                D2D1::Matrix3x2F::Identity());
        }
    }

    void draw_grid(
        const D2D1_SIZE_F& size,
        float visibility,
        float opening_progress) {
        hit_regions_.clear();
        root_drop_regions_.clear();
        delete_hit_regions_.clear();
        if (folder_drop_animation_active_) {
            folder_drop_target_bounds_valid_ = false;
        }
        if (visible_items_.empty()) {
            white_brush_->SetOpacity(0.72F * visibility);
            const std::wstring message = apps_.empty()
                ? L"Папка Launchpad Applications пуста\n"
                  L"Переместите в неё ярлыки .lnk или нажмите Ctrl+O"
                : L"Приложения не найдены";
            render_target_->DrawTextW(
                message.c_str(),
                static_cast<UINT32>(message.size()),
                empty_format_.Get(),
                D2D1::RectF(
                    0.0F,
                    size.height * 0.43F,
                    size.width,
                    size.height * 0.57F),
                white_brush_.Get());
            return;
        }

        const float horizontal_margin =
            std::max(72.0F, size.width * 0.095F);
        const float top = 122.0F;
        const float bottom = 105.0F;
        const float cell_width =
            (size.width - horizontal_margin * 2.0F) /
            static_cast<float>(launchpad::kGridColumns);
        const float cell_height =
            (size.height - top - bottom) /
            static_cast<float>(launchpad::kGridRows);
        const float icon_size = std::clamp(
            std::min(cell_width * 0.52F, cell_height * 0.56F),
            62.0F,
            98.0F);

        if (page_transition_active_) {
            const float offset =
                current_page_transition_offset();
            if (page_transition_active_) {
                draw_page(
                    transition_from_page_,
                    offset,
                    1.0F,
                    visibility,
                    opening_progress,
                    false,
                    horizontal_margin,
                    top,
                    cell_width,
                    cell_height,
                    icon_size);
                if (page_transition_neighbor_page_ != kNoPage) {
                    draw_page(
                        page_transition_neighbor_page_,
                        offset +
                            static_cast<float>(
                                page_transition_direction_) *
                                page_transition_width_,
                        1.0F,
                        visibility,
                        opening_progress,
                        false,
                        horizontal_margin,
                        top,
                        cell_width,
                        cell_height,
                        icon_size);
                }
                return;
            }
        }

        if (page_drag_active_) {
            const float raw_offset = page_drag_raw_offset();
            const PageDragVisual visual = page_drag_visual(
                raw_offset,
                size.width,
                page_drag_velocity(size.width));
            draw_page(
                current_page_,
                visual.offset,
                1.0F,
                visibility,
                opening_progress,
                false,
                horizontal_margin,
                top,
                cell_width,
                cell_height,
                icon_size);
            if (visual.neighbor != kNoPage) {
                draw_page(
                    visual.neighbor,
                    visual.offset +
                        static_cast<float>(visual.direction) *
                            size.width,
                    1.0F,
                    visibility,
                    opening_progress,
                    false,
                    horizontal_margin,
                    top,
                    cell_width,
                    cell_height,
                    icon_size);
            }
            return;
        }

        draw_page(
            current_page_,
            0.0F,
            1.0F,
            visibility,
            opening_progress,
            !closing_,
            horizontal_margin,
            top,
            cell_width,
            cell_height,
            icon_size);
    }

    void warm_adjacent_icons() {
        if (icon_load_budget_ <= 0 ||
            visible_items_.empty() ||
            page_transition_active_ ||
            page_drag_active_ ||
            open_folder_index_ != kNoPage ||
            closing_) {
            return;
        }
        const std::size_t count = visible_page_count();
        std::array<std::size_t, 2> candidates{
            current_page_ + 1 < count
                ? current_page_ + 1
                : kNoPage,
            current_page_ > 0
                ? current_page_ - 1
                : kNoPage,
        };
        for (const std::size_t page : candidates) {
            if (page == kNoPage) {
                continue;
            }
            const auto [first, last] =
                visible_page_range(page);
            for (std::size_t position = first;
                 position < last;
                 ++position) {
                AppEntry* app =
                    app_for_visible(visible_items_[position]);
                if (!app) {
                    continue;
                }
                if (app->icon || app->icon_attempted) {
                    continue;
                }
                icons_pending_ = true;
                --icon_load_budget_;
                ensure_icon(*app);
                return;
            }
        }
    }

    void draw_page_dots(
        const D2D1_SIZE_F& size,
        float visibility) {
        page_dot_regions_.clear();
        const std::size_t count = effective_page_count();
        if (count <= 1) {
            return;
        }
        const float spacing = 18.0F;
        const float first_x =
            size.width * 0.5F -
            spacing * static_cast<float>(count - 1) * 0.5F;
        const float y = size.height - 64.0F;
        for (std::size_t index = 0; index < count; ++index) {
            const float center_x =
                first_x + spacing * static_cast<float>(index);
            white_brush_->SetOpacity(
                (index == current_page_ ? 0.90F : 0.28F) *
                visibility);
            render_target_->FillEllipse(
                D2D1::Ellipse(
                    D2D1::Point2F(center_x, y),
                    4.0F,
                    4.0F),
                white_brush_.Get());
            page_dot_regions_.push_back(PageDotRegion{
                .bounds = D2D1::RectF(
                    center_x - 8.0F,
                    y - 10.0F,
                    center_x + 8.0F,
                    y + 10.0F),
                .page = index,
            });
        }
    }

    void rebuild_filter(std::size_t preferred_page = 0) {
        visible_items_.clear();
        visible_page_starts_.clear();
        visible_page_starts_.push_back(0);
        bool pending_page_break = false;
        const auto append_visible =
            [this, &pending_page_break](VisibleItem visible) {
                const std::size_t page_size =
                    visible_items_.size() -
                    visible_page_starts_.back();
                if ((pending_page_break && page_size != 0) ||
                    page_size >= launchpad::kPageCapacity) {
                    visible_page_starts_.push_back(
                        visible_items_.size());
                }
                pending_page_break = false;
                visible_items_.push_back(std::move(visible));
            };
        const std::wstring normalized_search =
            launchpad::normalize_search_text(search_);
        if (normalized_search.empty()) {
            for (std::size_t layout_index = 0;
                 layout_index < layout_.items().size();
                 ++layout_index) {
                const launchpad::LayoutItem& item =
                    layout_.items()[layout_index];
                if (item.kind ==
                    launchpad::LayoutItemKind::page_break) {
                    pending_page_break = true;
                    continue;
                }
                if (item.kind == launchpad::LayoutItemKind::app) {
                    const std::size_t app_index =
                        app_index_for_path(item.app_path);
                    if (app_index != kNoPage) {
                        append_visible(VisibleItem{
                            .kind = VisibleItemKind::app,
                            .layout_index = layout_index,
                            .app_index = app_index,
                        });
                    }
                } else {
                    const bool has_visible_child =
                        std::ranges::any_of(
                            item.children,
                            [this](const std::wstring& path) {
                                return app_index_for_path(path) != kNoPage;
                            });
                    if (has_visible_child) {
                        append_visible(VisibleItem{
                            .kind = VisibleItemKind::folder,
                            .layout_index = layout_index,
                        });
                    }
                }
            }
        } else {
            struct SearchResult {
                VisibleItem visible;
                int score = launchpad::kNoSearchMatch;
                std::wstring normalized_name;
                std::wstring normalized_path;
                std::size_t source_order = 0;
            };
            std::unordered_set<std::wstring> seen;
            std::vector<SearchResult> results;
            results.reserve(apps_.size());
            const auto add_search_result =
                [this, &seen, &results](
                    std::size_t layout_index,
                    std::size_t child_index,
                    const std::wstring& path,
                    std::size_t source_order) {
                    const std::size_t app_index =
                        app_index_for_path(path);
                    if (app_index == kNoPage) {
                        return;
                    }
                    const int score =
                        launchpad::search_match_score(
                            apps_[app_index].name,
                            search_);
                    const std::wstring normalized_path =
                        launchpad::lowercase(path);
                    if (score == launchpad::kNoSearchMatch ||
                        !seen.insert(normalized_path).second) {
                        return;
                    }
                    results.push_back(SearchResult{
                        .visible = VisibleItem{
                            .kind = VisibleItemKind::app,
                            .layout_index = layout_index,
                            .child_index = child_index,
                            .app_index = app_index,
                        },
                        .score = score,
                        .normalized_name =
                            launchpad::normalize_search_text(
                                apps_[app_index].name),
                        .normalized_path = normalized_path,
                        .source_order = source_order,
                    });
                };
            std::size_t source_order = 0;
            for (std::size_t layout_index = 0;
                 layout_index < layout_.items().size();
                 ++layout_index) {
                const launchpad::LayoutItem& item =
                    layout_.items()[layout_index];
                if (item.kind == launchpad::LayoutItemKind::app) {
                    add_search_result(
                        layout_index,
                        0,
                        item.app_path,
                        source_order++);
                } else if (
                    item.kind ==
                    launchpad::LayoutItemKind::folder) {
                    for (std::size_t child = 0;
                         child < item.children.size();
                         ++child) {
                        add_search_result(
                            layout_index,
                            child,
                            item.children[child],
                            source_order++);
                    }
                }
            }
            std::ranges::stable_sort(
                results,
                [](const SearchResult& left,
                   const SearchResult& right) {
                    if (left.score != right.score) {
                        return left.score > right.score;
                    }
                    if (left.normalized_name !=
                        right.normalized_name) {
                        return left.normalized_name <
                            right.normalized_name;
                    }
                    if (left.normalized_path !=
                        right.normalized_path) {
                        return left.normalized_path <
                            right.normalized_path;
                    }
                    return left.source_order < right.source_order;
                });
            for (SearchResult& result : results) {
                append_visible(std::move(result.visible));
            }
        }
        current_page_ = std::min(
            preferred_page,
            visible_page_count() - 1);
        select_first_item_on_page(current_page_);
        reset_page_motion();
        if (hwnd_) {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void on_character(wchar_t character) {
        if (delete_confirmation_active_) {
            return;
        }
        if (drag_active_ || drag_candidate_ ||
            folder_drag_active_ || folder_drag_candidate_) {
            return;
        }
        if (open_folder_index_ != kNoPage) {
            if (!folder_name_editing_) {
                return;
            }
            if (character == L'\b') {
                if (!folder_name_buffer_.empty()) {
                    folder_name_buffer_.pop_back();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return;
            }
            if (character >= 32 &&
                character != 127 &&
                folder_name_buffer_.size() < 48) {
                folder_name_buffer_.push_back(character);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        if (!search_focused_) {
            return;
        }
        if (character == L'\b') {
            if (!search_.empty()) {
                if (search_all_selected_) {
                    search_.clear();
                } else {
                    search_.pop_back();
                }
                search_all_selected_ = false;
                selection_visible_ = false;
                reset_search_caret_blink();
                rebuild_filter();
            }
            return;
        }
        if (character >= 32 && character != 127) {
            if (search_all_selected_) {
                search_.clear();
                search_all_selected_ = false;
            }
            search_.push_back(character);
            selection_visible_ = false;
            reset_search_caret_blink();
            rebuild_filter();
        }
    }

    bool on_key_down(WPARAM key) {
        if (delete_confirmation_active_) {
            if (key == VK_ESCAPE) {
                dismiss_delete_confirmation();
            }
            return true;
        }
        if (key == VK_ESCAPE &&
            (drag_active_ || drag_candidate_ ||
             folder_drag_active_ || folder_drag_candidate_)) {
            cancel_drag();
            cancel_folder_drag();
            if (GetCapture() == hwnd_) {
                ReleaseCapture();
            }
            return true;
        }
        if (drag_active_ || drag_candidate_ ||
            folder_drag_active_ || folder_drag_candidate_) {
            return true;
        }
        if (key == VK_F5) {
            rescan_apps();
            return true;
        }
        if (key == L'O' &&
            (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            open_applications_directory();
            return true;
        }
        if (open_folder_index_ != kNoPage) {
            if (folder_name_editing_) {
                switch (key) {
                case VK_ESCAPE:
                    cancel_folder_name_edit();
                    return true;
                case VK_RETURN:
                    commit_folder_name_edit();
                    return true;
                case VK_BACK:
                    return false;
                default:
                    return false;
                }
            }
            switch (key) {
            case VK_ESCAPE:
                close_folder();
                return true;
            case VK_F2:
                begin_folder_name_edit();
                return true;
            case VK_RETURN:
                launch_folder_selected();
                return true;
            case VK_LEFT:
                move_folder_selection(-1);
                return true;
            case VK_RIGHT:
                move_folder_selection(1);
                return true;
            case VK_UP:
                move_folder_selection(
                    -static_cast<int>(kFolderColumns));
                return true;
            case VK_DOWN:
                move_folder_selection(
                    static_cast<int>(kFolderColumns));
                return true;
            case VK_PRIOR:
                change_folder_page(-1);
                return true;
            case VK_NEXT:
                change_folder_page(1);
                return true;
            case VK_HOME:
                folder_selection_visible_ = true;
                folder_selected_position_ = 0;
                folder_page_ = 0;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return true;
            case VK_END:
                if (open_folder_index_ < layout_.items().size() &&
                    !layout_.items()[open_folder_index_]
                         .children.empty()) {
                    folder_selection_visible_ = true;
                    folder_selected_position_ =
                        layout_.items()[open_folder_index_]
                            .children.size() - 1;
                    folder_page_ =
                        folder_selected_position_ /
                        kFolderPageCapacity;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return true;
            default:
                return false;
            }
        }
        if (search_focused_ &&
            (GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
            key == L'A') {
            if (!search_.empty()) {
                search_all_selected_ = true;
                selection_visible_ = false;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return true;
        }
        if (search_focused_ &&
            key == VK_DELETE &&
            search_all_selected_) {
            search_.clear();
            search_all_selected_ = false;
            selection_visible_ = false;
            reset_search_caret_blink();
            rebuild_filter();
            return true;
        }
        switch (key) {
        case VK_ESCAPE:
            if (search_focused_) {
                if (!search_.empty()) {
                    search_.clear();
                    search_all_selected_ = false;
                    reset_search_caret_blink();
                    rebuild_filter();
                } else {
                    set_search_focused(false);
                }
                return true;
            }
            if (!search_.empty()) {
                search_.clear();
                search_all_selected_ = false;
                rebuild_filter();
                return true;
            }
            request_close();
            return true;
        case VK_RETURN:
            launch_selected();
            return true;
        case VK_LEFT:
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                change_page(-1);
            } else {
                move_selection(-1);
            }
            return true;
        case VK_RIGHT:
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                change_page(1);
            } else {
                move_selection(1);
            }
            return true;
        case VK_UP:
            move_selection(
                -static_cast<int>(launchpad::kGridColumns));
            return true;
        case VK_DOWN:
            move_selection(
                static_cast<int>(launchpad::kGridColumns));
            return true;
        case VK_PRIOR:
            change_page(-1);
            return true;
        case VK_NEXT:
            change_page(1);
            return true;
        case VK_HOME:
            if (!visible_items_.empty() &&
                !page_transition_active_ &&
                !closing_) {
                selection_visible_ = true;
                go_to_page(0);
                selected_position_ = 0;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return true;
        case VK_END:
            if (!visible_items_.empty() &&
                !page_transition_active_ &&
                !closing_) {
                selection_visible_ = true;
                const std::size_t last_position =
                    visible_items_.size() - 1;
                go_to_page(
                    visible_page_for_position(last_position));
                selected_position_ = last_position;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return true;
        default:
            return false;
        }
    }

    void move_selection(int delta) {
        if (visible_items_.empty() ||
            page_transition_active_ ||
            closing_) {
            return;
        }
        selection_visible_ = true;
        const auto current =
            static_cast<std::ptrdiff_t>(selected_position_);
        const auto last =
            static_cast<std::ptrdiff_t>(visible_items_.size() - 1);
        const std::size_t old_page = current_page_;
        selected_position_ = static_cast<std::size_t>(
            std::clamp<std::ptrdiff_t>(current + delta, 0, last));
        current_page_ =
            visible_page_for_position(selected_position_);
        if (current_page_ != old_page) {
            begin_page_transition(
                old_page,
                current_page_ > old_page ? 1 : -1,
                0.0F);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    float page_width_dips() const {
        if (render_target_) {
            return std::max(
                1.0F,
                render_target_->GetSize().width);
        }
        RECT client{};
        GetClientRect(hwnd_, &client);
        const float scale =
            96.0F /
            static_cast<float>(GetDpiForWindow(hwnd_));
        return std::max(
            1.0F,
            static_cast<float>(
                std::max(1L, client.right - client.left)) *
                scale);
    }

    float page_drag_raw_offset() const {
        return client_delta_to_dips(
                   page_drag_current_x_ - page_drag_start_x_,
                   page_drag_current_y_ - page_drag_start_y_)
            .x;
    }

    void update_page_drag_moved() {
        if (page_drag_moved_) {
            return;
        }
        const D2D1_POINT_2F delta = client_delta_to_dips(
            page_drag_current_x_ - page_drag_start_x_,
            page_drag_current_y_ - page_drag_start_y_);
        page_drag_moved_ =
            std::hypot(delta.x, delta.y) >
            kClickSlopDips;
    }

    void reset_page_drag_samples() {
        page_drag_sample_count_ = 0;
    }

    void record_page_drag_sample(float offset) {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        if (page_drag_sample_count_ != 0) {
            PageDragSample& latest =
                page_drag_samples_[page_drag_sample_count_ - 1];
            const double spacing =
                clock_frequency_ > 0.0
                ? static_cast<double>(
                      counter.QuadPart - latest.counter) /
                      clock_frequency_
                : 0.0;
            if (spacing < 0.001) {
                latest = PageDragSample{
                    .counter = counter.QuadPart,
                    .offset = offset,
                };
                return;
            }
        }
        if (page_drag_sample_count_ == page_drag_samples_.size()) {
            std::move(
                page_drag_samples_.begin() + 1,
                page_drag_samples_.end(),
                page_drag_samples_.begin());
            --page_drag_sample_count_;
        }
        page_drag_samples_[page_drag_sample_count_++] =
            PageDragSample{
                .counter = counter.QuadPart,
                .offset = offset,
            };
    }

    float page_drag_velocity(float page_width) const {
        if (clock_frequency_ <= 0.0 ||
            page_drag_sample_count_ < 2) {
            return 0.0F;
        }
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        const PageDragSample& newest =
            page_drag_samples_[page_drag_sample_count_ - 1];
        const double newest_age =
            static_cast<double>(
                counter.QuadPart - newest.counter) /
            clock_frequency_;
        if (newest_age > 0.080) {
            return 0.0F;
        }

        const PageDragSample* oldest = &newest;
        for (std::size_t index = 0;
             index + 1 < page_drag_sample_count_;
             ++index) {
            const PageDragSample& candidate =
                page_drag_samples_[index];
            const double age =
                static_cast<double>(
                    newest.counter - candidate.counter) /
                clock_frequency_;
            if (age <= 0.080) {
                oldest = &candidate;
                break;
            }
        }
        const double span =
            static_cast<double>(
                newest.counter - oldest->counter) /
            clock_frequency_;
        if (span < 0.012) {
            return 0.0F;
        }
        const float velocity = static_cast<float>(
            static_cast<double>(
                newest.offset - oldest->offset) /
            span);
        const float limit = std::max(
            1.0F,
            page_width *
                kPageMaxVelocityPagesPerSecond);
        return std::clamp(velocity, -limit, limit);
    }

    PageDragVisual page_drag_visual(
        float raw_offset,
        float page_width,
        float raw_velocity) const {
        const float direction_source =
            std::abs(raw_offset) >= 4.0F
                ? raw_offset
                : raw_velocity;
        const int direction =
            direction_source < 0.0F ? 1 : -1;
        const std::size_t count = effective_page_count();
        std::size_t neighbor = kNoPage;
        if (direction > 0 && current_page_ + 1 < count) {
            neighbor = current_page_ + 1;
        } else if (direction < 0 && current_page_ > 0) {
            neighbor = current_page_ - 1;
        }

        if (neighbor != kNoPage) {
            constexpr float linear_fraction = 0.80F;
            const float linear_limit =
                page_width * linear_fraction;
            const float tail =
                page_width - linear_limit;
            const float magnitude =
                std::abs(raw_offset);
            if (magnitude <= linear_limit) {
                return PageDragVisual{
                    .direction = direction,
                    .neighbor = neighbor,
                    .offset = raw_offset,
                    .velocity_scale = 1.0F,
                };
            }
            const float decay = std::exp(
                -(magnitude - linear_limit) /
                std::max(1.0F, tail));
            return PageDragVisual{
                .direction = direction,
                .neighbor = neighbor,
                .offset = std::copysign(
                    linear_limit +
                        tail * (1.0F - decay),
                    raw_offset),
                .velocity_scale = decay,
            };
        }

        const float limit = page_width * 0.12F;
        constexpr float slope = 0.22F;
        const float magnitude = std::abs(raw_offset);
        const float decay = std::exp(
            -slope * magnitude /
            std::max(1.0F, limit));
        return PageDragVisual{
            .direction = direction,
            .neighbor = kNoPage,
            .offset = std::copysign(
                limit * (1.0F - decay),
                raw_offset),
            .velocity_scale = slope * decay,
        };
    }

    void change_page(
        int delta,
        float initial_drag_offset = 0.0F,
        float initial_velocity = 0.0F) {
        if (page_transition_active_ || closing_) {
            return;
        }
        const std::size_t count = effective_page_count();
        const auto page = static_cast<std::ptrdiff_t>(current_page_);
        const auto last = static_cast<std::ptrdiff_t>(count - 1);
        const std::size_t new_page = static_cast<std::size_t>(
            std::clamp<std::ptrdiff_t>(page + delta, 0, last));
        go_to_page(
            new_page,
            initial_drag_offset,
            initial_velocity);
    }

    void accumulate_wheel(int delta, bool horizontal) {
        int& remainder = horizontal
            ? horizontal_wheel_remainder_
            : vertical_wheel_remainder_;
        remainder += delta;
        if (std::abs(remainder) < WHEEL_DELTA ||
            page_transition_active_) {
            return;
        }
        if (horizontal) {
            change_page(remainder > 0 ? 1 : -1);
        } else {
            change_page(remainder < 0 ? 1 : -1);
        }
        remainder = 0;
    }

    void go_to_page(
        std::size_t new_page,
        float initial_drag_offset = 0.0F,
        float initial_velocity = 0.0F) {
        if (page_transition_active_ || closing_) {
            return;
        }
        const std::size_t count = effective_page_count();
        new_page = std::min(new_page, count - 1);
        if (new_page == current_page_) {
            return;
        }
        const std::size_t old_page = current_page_;
        const int direction = new_page > old_page ? 1 : -1;
        current_page_ = new_page;
        select_first_item_on_page(current_page_);
        begin_page_transition(
            old_page,
            direction,
            initial_drag_offset,
            initial_velocity);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    float sanitize_page_motion_velocity(
        float start,
        float target,
        float velocity,
        float width) const {
        const float limit = std::max(
            1.0F,
            width *
                kPageMaxVelocityPagesPerSecond);
        velocity = std::clamp(velocity, -limit, limit);
        const float displacement = target - start;
        if (displacement * velocity > 0.0F) {
            const float maximum_toward =
                kPageSettleOmega *
                std::abs(displacement);
            velocity = std::copysign(
                std::min(
                    std::abs(velocity),
                    maximum_toward),
                velocity);
        }
        return velocity;
    }

    void start_page_motion(
        std::size_t from_page,
        std::size_t neighbor_page,
        int direction,
        float width,
        float start,
        float velocity,
        float target) {
        transition_from_page_ = from_page;
        page_transition_neighbor_page_ = neighbor_page;
        page_transition_direction_ = direction;
        page_transition_width_ = std::max(1.0F, width);
        page_transition_start_offset_ = start;
        page_transition_target_offset_ = target;
        page_transition_initial_velocity_ =
            sanitize_page_motion_velocity(
                start,
                target,
                velocity,
                page_transition_width_);
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        page_transition_origin_ = counter.QuadPart;
        page_transition_active_ =
            animations_enabled_ &&
            (std::abs(target - start) > 0.01F ||
             std::abs(page_transition_initial_velocity_) >
                 kPageSettleVelocityDipsPerSecond);
    }

    void reset_page_motion() {
        page_transition_active_ = false;
        transition_from_page_ = current_page_;
        page_transition_neighbor_page_ = kNoPage;
        page_transition_direction_ = 1;
        page_transition_width_ = 1.0F;
        page_transition_start_offset_ = 0.0F;
        page_transition_target_offset_ = 0.0F;
        page_transition_initial_velocity_ = 0.0F;
    }

    void begin_page_transition(
        std::size_t from_page,
        int direction,
        float initial_drag_offset,
        float initial_velocity = 0.0F) {
        const float width = page_width_dips();
        start_page_motion(
            from_page,
            current_page_,
            direction,
            width,
            std::clamp(
                initial_drag_offset,
                -width,
                width),
            initial_velocity,
            -static_cast<float>(direction) * width);
    }

    void begin_page_return(
        const PageDragVisual& visual,
        float initial_velocity,
        float width) {
        start_page_motion(
            current_page_,
            visual.neighbor,
            visual.direction,
            width,
            visual.offset,
            initial_velocity,
            0.0F);
    }

    float current_page_transition_offset() {
        if (!page_transition_active_) {
            return page_transition_target_offset_;
        }
        const float seconds = static_cast<float>(
            elapsed_since(page_transition_origin_));
        const float initial_delta =
            page_transition_start_offset_ -
            page_transition_target_offset_;
        const float coefficient =
            page_transition_initial_velocity_ +
            kPageSettleOmega * initial_delta;
        const float decay =
            std::exp(-kPageSettleOmega * seconds);
        const float offset =
            page_transition_target_offset_ +
            (initial_delta + coefficient * seconds) * decay;
        const float velocity =
            (page_transition_initial_velocity_ -
             kPageSettleOmega * coefficient * seconds) *
            decay;
        if (seconds >= kPageSettleMaxSeconds ||
            (std::abs(
                 offset -
                 page_transition_target_offset_) <=
                 kPageSettleDistanceDips &&
             std::abs(velocity) <=
                 kPageSettleVelocityDipsPerSecond)) {
            page_transition_active_ = false;
            page_transition_neighbor_page_ = kNoPage;
            page_transition_start_offset_ =
                page_transition_target_offset_;
            page_transition_initial_velocity_ = 0.0F;
            rearm_drag_edge_after_transition();
            return page_transition_target_offset_;
        }
        return offset;
    }

    bool update_pointer_selection(int x, int y) {
        if (!render_target_ ||
            page_transition_active_ ||
            page_drag_active_ ||
            closing_) {
            return false;
        }
        const D2D1_POINT_2F point = client_point_to_dips(x, y);
        for (const HitRegion& region : hit_regions_) {
            if (point.x >= region.bounds.left &&
                point.x <= region.bounds.right &&
                point.y >= region.bounds.top &&
                point.y <= region.bounds.bottom) {
                const bool needs_repaint =
                    selected_position_ != region.visible_position ||
                    selection_visible_;
                if (selected_position_ != region.visible_position) {
                    selected_position_ = region.visible_position;
                }
                selection_visible_ = false;
                if (needs_repaint) {
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return true;
            }
        }
        return false;
    }

    bool update_folder_pointer_selection(int x, int y) {
        if (!render_target_ ||
            open_folder_index_ == kNoPage ||
            folder_closing_ ||
            closing_) {
            return false;
        }
        const D2D1_POINT_2F point = client_point_to_dips(x, y);
        for (const HitRegion& region : folder_hit_regions_) {
            if (point.x >= region.bounds.left &&
                point.x <= region.bounds.right &&
                point.y >= region.bounds.top &&
                point.y <= region.bounds.bottom) {
                const bool needs_repaint =
                    folder_selection_visible_;
                if (folder_selected_position_ !=
                    region.visible_position) {
                    folder_selected_position_ =
                        region.visible_position;
                }
                folder_selection_visible_ = false;
                if (needs_repaint) {
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return true;
            }
        }
        return false;
    }

    void refresh_open_folder_apps() {
        open_folder_app_indices_.clear();
        if (open_folder_index_ >= layout_.items().size() ||
            layout_.items()[open_folder_index_].kind !=
                launchpad::LayoutItemKind::folder) {
            return;
        }
        const auto& children =
            layout_.items()[open_folder_index_].children;
        open_folder_app_indices_.reserve(children.size());
        for (const std::wstring& path : children) {
            open_folder_app_indices_.push_back(
                app_index_for_path(path));
        }
    }

    void start_folder_drag() {
        if (!folder_drag_candidate_ ||
            open_folder_index_ >= layout_.items().size() ||
            folder_closing_ || closing_) {
            folder_drag_candidate_ = false;
            folder_drag_source_position_ = kNoPage;
            folder_drag_target_position_ = kNoPage;
            return;
        }
        const launchpad::LayoutItem& folder =
            layout_.items()[open_folder_index_];
        if (folder.kind != launchpad::LayoutItemKind::folder ||
            folder_drag_source_position_ >= folder.children.size()) {
            folder_drag_candidate_ = false;
            folder_drag_source_position_ = kNoPage;
            folder_drag_target_position_ = kNoPage;
            return;
        }
        folder_drag_candidate_ = false;
        folder_drag_active_ = true;
        edit_mode_ = true;
        clear_folder_live_reflow();
        seed_folder_drag_centers();
        folder_drag_target_position_ =
            folder_drag_source_position_;
        folder_selection_visible_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void restore_folder_extraction() {
        if (!folder_extraction_) {
            return;
        }
        folder_closing_visual_.reset();
        folder_closing_hidden_position_ = kNoPage;
        FolderExtractionTransaction transaction =
            std::move(*folder_extraction_);
        folder_extraction_.reset();
        layout_ = std::move(transaction.snapshot);
        rebuild_filter(transaction.root_page);

        if (transaction.folder_index < layout_.items().size() &&
            layout_.items()[transaction.folder_index].kind ==
                launchpad::LayoutItemKind::folder) {
            open_folder(transaction.folder_index);
            folder_animation_active_ = false;
            folder_closing_ = false;
            const auto& children =
                layout_.items()[transaction.folder_index].children;
            if (!children.empty()) {
                folder_selected_position_ = std::min(
                    transaction.child_position,
                    children.size() - 1);
                const std::size_t page_count =
                    std::max<std::size_t>(
                        1,
                        (children.size() +
                         kFolderPageCapacity - 1) /
                            kFolderPageCapacity);
                folder_page_ = std::min(
                    transaction.folder_page,
                    page_count - 1);
            }
            folder_selection_visible_ = false;
        }
        mouse_down_on_folder_item_ = false;
        mouse_down_on_folder_background_ = false;
        mouse_down_folder_position_ = kNoPage;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    bool promote_folder_drag_to_root(int x, int y) {
        if (!folder_drag_active_ ||
            open_folder_index_ >= layout_.items().size() ||
            folder_drag_source_position_ == kNoPage) {
            return false;
        }
        const std::size_t folder_index =
            open_folder_index_;
        const launchpad::LayoutItem& folder =
            layout_.items()[folder_index];
        if (folder.kind != launchpad::LayoutItemKind::folder ||
            folder_drag_source_position_ >=
                folder.children.size()) {
            return false;
        }

        FolderExtractionTransaction transaction{
            .snapshot = layout_,
            .app_path =
                folder.children[folder_drag_source_position_],
            .folder_index = folder_index,
            .child_position =
                folder_drag_source_position_,
            .folder_page = folder_page_,
            .root_page = current_page_,
        };
        folder_closing_visual_ = folder;
        folder_closing_hidden_position_ =
            folder_drag_source_position_;
        if (!layout_.extract_folder_app(
                folder_index,
                folder_drag_source_position_,
                available_apps())) {
            folder_closing_visual_.reset();
            folder_closing_hidden_position_ = kNoPage;
            return false;
        }

        folder_extraction_ = std::move(transaction);
        const std::wstring extracted_path =
            folder_extraction_->app_path;
        const std::size_t root_page =
            folder_extraction_->root_page;
        folder_drag_active_ = false;
        folder_drag_candidate_ = false;
        folder_drag_source_position_ = kNoPage;
        folder_drag_target_position_ = kNoPage;
        clear_folder_live_reflow();
        close_folder();
        rebuild_filter(root_page);
        const std::size_t source_position =
            visible_position_for_root_app(extracted_path);
        if (source_position == kNoPage ||
            source_position >= visible_items_.size()) {
            restore_folder_extraction();
            return false;
        }

        drag_active_ = true;
        drag_candidate_ = false;
        drag_source_visible_position_ = source_position;
        drag_source_layout_index_ =
            visible_items_[source_position].layout_index;
        drag_source_kind_ = VisibleItemKind::app;
        drag_source_page_ =
            visible_page_for_position(source_position);
        drag_current_x_ = x;
        drag_current_y_ = y;
        drag_target_visible_position_ = kNoPage;
        folder_drop_target_visible_position_ = kNoPage;
        folder_hover_candidate_visible_position_ = kNoPage;
        folder_hover_origin_ = 0;
        drag_folder_intent_locked_ = false;
        clear_root_live_reflow();
        drag_provisional_page_ = false;
        drag_edge_direction_ = 0;
        drag_edge_latched_ = false;
        selection_visible_ = false;
        hit_regions_.clear();
        delete_hit_regions_.clear();
        mouse_down_on_folder_item_ = false;
        mouse_down_on_folder_background_ = false;
        mouse_down_folder_position_ = kNoPage;
        write_drag_diagnostic(
            L"folder-extract",
            x,
            y,
            source_position);
        update_drag_edge_hover_state();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    void update_folder_drag(int x, int y) {
        if (!folder_drag_active_) {
            return;
        }
        folder_drag_current_x_ = x;
        folder_drag_current_y_ = y;
        const D2D1_POINT_2F point = client_point_to_dips(x, y);
        if (folder_panel_bounds_valid_ &&
            !folder_animation_active_ &&
            (point.x <
                 folder_panel_bounds_.left -
                     kFolderExtractionMarginDips ||
             point.x >
                 folder_panel_bounds_.right +
                     kFolderExtractionMarginDips ||
             point.y <
                 folder_panel_bounds_.top -
                     kFolderExtractionMarginDips ||
             point.y >
                 folder_panel_bounds_.bottom +
                     kFolderExtractionMarginDips)) {
            promote_folder_drag_to_root(x, y);
            return;
        }
        std::size_t next_target = kNoPage;
        for (const HitRegion& region : folder_drop_regions_) {
            if (point.x >= region.bounds.left &&
                point.x <= region.bounds.right &&
                point.y >= region.bounds.top &&
                point.y <= region.bounds.bottom) {
                next_target = region.visible_position;
                break;
            }
        }
        if (next_target != folder_drag_target_position_) {
            begin_folder_live_reflow();
            folder_drag_target_position_ = next_target;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void finish_folder_drag() {
        bool changed = false;
        if (folder_drag_active_ &&
            open_folder_index_ < layout_.items().size() &&
            folder_drag_source_position_ != kNoPage &&
            folder_drag_target_position_ != kNoPage) {
            changed = layout_.move_folder_app(
                open_folder_index_,
                folder_drag_source_position_,
                folder_drag_target_position_);
        }
        const std::size_t target =
            folder_drag_target_position_;
        folder_drag_active_ = false;
        folder_drag_candidate_ = false;
        folder_drag_source_position_ = kNoPage;
        folder_drag_target_position_ = kNoPage;
        folder_drop_regions_.clear();
        clear_folder_live_reflow();
        if (changed) {
            save_layout_state();
            refresh_open_folder_apps();
            const auto& children =
                layout_.items()[open_folder_index_].children;
            if (!children.empty()) {
                folder_selected_position_ = std::min(
                    target,
                    children.size() - 1);
                folder_page_ =
                    folder_selected_position_ /
                    kFolderPageCapacity;
            }
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void cancel_folder_drag() {
        folder_drag_active_ = false;
        folder_drag_candidate_ = false;
        folder_drag_source_position_ = kNoPage;
        folder_drag_target_position_ = kNoPage;
        folder_drop_regions_.clear();
        clear_folder_live_reflow();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void start_drag() {
        if (!drag_candidate_ ||
            drag_source_visible_position_ >= visible_items_.size() ||
            drag_source_layout_index_ >= layout_.items().size() ||
            !search_.empty() ||
            open_folder_index_ != kNoPage ||
            closing_) {
            write_drag_diagnostic(
                L"start-rejected",
                drag_current_x_,
                drag_current_y_,
                drag_source_visible_position_);
            drag_candidate_ = false;
            drag_source_visible_position_ = kNoPage;
            drag_source_layout_index_ = kNoPage;
            return;
        }
        drag_candidate_ = false;
        drag_active_ = true;
        edit_mode_ = true;
        clear_root_live_reflow();
        seed_root_drag_centers();
        drag_source_page_ = visible_page_for_position(
            drag_source_visible_position_);
        drag_provisional_page_ = false;
        drag_edge_direction_ = 0;
        drag_edge_latched_ = false;
        drag_target_visible_position_ =
            drag_source_visible_position_;
        folder_drop_target_visible_position_ = kNoPage;
        folder_hover_candidate_visible_position_ = kNoPage;
        folder_hover_origin_ = 0;
        drag_folder_intent_locked_ = false;
        selection_visible_ = false;
        write_drag_diagnostic(
            L"start",
            drag_current_x_,
            drag_current_y_,
            drag_source_visible_position_);
        update_drag_edge_hover_state();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void update_drag(int x, int y) {
        if (!drag_active_) {
            return;
        }
        drag_current_x_ = x;
        drag_current_y_ = y;
        const D2D1_POINT_2F point = client_point_to_dips(x, y);
        std::size_t next_target = kNoPage;
        const HitRegion* target_region = nullptr;
        for (const HitRegion& region : root_drop_regions_) {
            if (point.x < region.bounds.left ||
                point.x > region.bounds.right ||
                point.y < region.bounds.top ||
                point.y > region.bounds.bottom) {
                continue;
            }
            next_target = region.visible_position;
            target_region = &region;
            break;
        }

        bool folder_hover = false;
        bool existing_folder_target = false;
        if (target_region &&
            next_target != drag_source_visible_position_ &&
            drag_source_kind_ == VisibleItemKind::app &&
            drag_source_visible_position_ <
                visible_items_.size() &&
            next_target < visible_items_.size()) {
            const VisibleItem& target =
                visible_items_[next_target];
            existing_folder_target =
                target.kind == VisibleItemKind::folder;
            if (existing_folder_target ||
                target.kind == VisibleItemKind::app) {
                const float width =
                    target_region->icon_bounds.right -
                    target_region->icon_bounds.left;
                const float height =
                    target_region->icon_bounds.bottom -
                    target_region->icon_bounds.top;
                // Existing folders get a generous magnetic area,
                // while the outer edge of the cell still remains
                // available for ordinary reordering.
                const bool already_locked =
                    drag_folder_intent_locked_ &&
                    folder_hover_candidate_visible_position_ ==
                        next_target;
                const float expansion =
                    existing_folder_target
                    ? (already_locked ? 0.48F : 0.38F)
                    : (already_locked ? 0.28F : 0.20F);
                const D2D1_RECT_F intent_bounds = D2D1::RectF(
                    target_region->icon_bounds.left -
                        width * expansion,
                    target_region->icon_bounds.top -
                        height * expansion,
                    target_region->icon_bounds.right +
                        width * expansion,
                    target_region->icon_bounds.bottom +
                        height * expansion);
                folder_hover =
                    point.x >= intent_bounds.left &&
                    point.x <= intent_bounds.right &&
                    point.y >= intent_bounds.top &&
                    point.y <= intent_bounds.bottom;
            }
        }

        if (folder_hover) {
            if (folder_hover_candidate_visible_position_ !=
                next_target) {
                folder_hover_candidate_visible_position_ =
                    next_target;
                LARGE_INTEGER counter{};
                QueryPerformanceCounter(&counter);
                folder_hover_origin_ = counter.QuadPart;
            }
        } else {
            folder_hover_candidate_visible_position_ = kNoPage;
            folder_hover_origin_ = 0;
        }

        std::size_t next_folder_target = kNoPage;
        if (existing_folder_target && folder_hover) {
            next_folder_target = next_target;
        } else if (folder_hover &&
            folder_hover_candidate_visible_position_ ==
                next_target &&
            elapsed_since(folder_hover_origin_) >=
                kFolderHoverActivationSeconds) {
            next_folder_target = next_target;
        }

        if (next_target != drag_target_visible_position_ ||
            next_folder_target !=
                folder_drop_target_visible_position_ ||
            folder_hover != drag_folder_intent_locked_) {
            begin_root_live_reflow();
            drag_target_visible_position_ = next_target;
            folder_drop_target_visible_position_ =
                next_folder_target;
            drag_folder_intent_locked_ = folder_hover;
        }
        write_drag_diagnostic(
            folder_drop_target_visible_position_ != kNoPage
                ? L"move-folder"
                : L"move",
            x,
            y,
            drag_target_visible_position_);
        update_drag_edge_hover_state();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void maybe_activate_folder_hover() {
        if (!drag_active_ ||
            folder_drop_target_visible_position_ != kNoPage ||
            !drag_folder_intent_locked_ ||
            folder_hover_candidate_visible_position_ == kNoPage ||
            folder_hover_candidate_visible_position_ !=
                drag_target_visible_position_ ||
            elapsed_since(folder_hover_origin_) <
                kFolderHoverActivationSeconds) {
            return;
        }
        begin_root_live_reflow();
        folder_drop_target_visible_position_ =
            folder_hover_candidate_visible_position_;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void update_drag_edge_hover_state() {
        if (!drag_active_ || !render_target_) {
            drag_edge_direction_ = 0;
            drag_edge_latched_ = false;
            return;
        }
        const D2D1_POINT_2F point = client_point_to_dips(
            drag_current_x_,
            drag_current_y_);
        const float width = render_target_->GetSize().width;
        int direction = 0;
        if (point.x <= kDragEdgeZoneDips &&
            current_page_ > 0) {
            direction = -1;
        } else if (
            point.x >= width - kDragEdgeZoneDips &&
            (current_page_ + 1 < effective_page_count() ||
             (!drag_provisional_page_ &&
              current_page_ + 1 == visible_page_count()))) {
            direction = 1;
        }
        if (direction == 0) {
            drag_edge_direction_ = 0;
            drag_edge_latched_ = false;
            return;
        }
        if (direction != drag_edge_direction_) {
            drag_edge_direction_ = direction;
            drag_edge_latched_ = false;
            LARGE_INTEGER counter{};
            QueryPerformanceCounter(&counter);
            drag_edge_hover_origin_ = counter.QuadPart;
        }
    }

    void rearm_drag_edge_after_transition() {
        if (!drag_active_) {
            return;
        }
        drag_edge_direction_ = 0;
        drag_edge_latched_ = false;
        update_drag_edge_hover_state();
    }

    void maybe_advance_drag_page() {
        if (!drag_active_ ||
            drag_edge_direction_ == 0 ||
            drag_edge_latched_ ||
            page_transition_active_ ||
            elapsed_since(drag_edge_hover_origin_) <
                kDragEdgeHoverSeconds) {
            return;
        }
        const auto next_signed =
            static_cast<std::ptrdiff_t>(current_page_) +
            drag_edge_direction_;
        if (next_signed < 0) {
            return;
        }
        const std::size_t next =
            static_cast<std::size_t>(next_signed);
        if (next == visible_page_count() &&
            drag_edge_direction_ > 0 &&
            !drag_provisional_page_) {
            drag_provisional_page_ = true;
        }
        if (next >= effective_page_count()) {
            return;
        }
        const std::size_t old_page = current_page_;
        current_page_ = next;
        drag_target_visible_position_ = kNoPage;
        folder_drop_target_visible_position_ = kNoPage;
        folder_hover_candidate_visible_position_ = kNoPage;
        folder_hover_origin_ = 0;
        drag_folder_intent_locked_ = false;
        clear_root_live_reflow();
        drag_edge_latched_ = true;
        begin_page_transition(
            old_page,
            drag_edge_direction_,
            0.0F);
        if (!page_transition_active_) {
            rearm_drag_edge_after_transition();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void finish_drag() {
        if (!drag_active_ ||
            drag_source_layout_index_ >= layout_.items().size()) {
            cancel_drag();
            return;
        }

        const std::size_t destination_page = current_page_;
        const bool drop_on_new_page =
            drag_provisional_page_ &&
            destination_page >= visible_page_count();
        const bool dropping_into_folder =
            !drop_on_new_page &&
            folder_drop_target_visible_position_ != kNoPage &&
            folder_drop_target_visible_position_ <
                visible_items_.size() &&
            drag_source_layout_index_ < layout_.items().size() &&
            layout_.items()[drag_source_layout_index_].kind ==
                launchpad::LayoutItemKind::app;
        std::wstring dropped_app_path;
        D2D1_RECT_F folder_drop_from_bounds{};
        if (dropping_into_folder) {
            dropped_app_path =
                layout_.items()[drag_source_layout_index_].app_path;
            const D2D1_POINT_2F point = client_point_to_dips(
                drag_current_x_,
                drag_current_y_);
            constexpr float drop_icon_half = 51.0F;
            folder_drop_from_bounds = D2D1::RectF(
                point.x - drop_icon_half,
                point.y - drop_icon_half,
                point.x + drop_icon_half,
                point.y + drop_icon_half);
        }
        const bool animate_root_reflow =
            animations_enabled_ &&
            destination_page == drag_source_page_ &&
            !page_transition_active_;
        if (animate_root_reflow) {
            capture_root_reflow_positions();
        } else {
            root_reflow_from_centers_.clear();
        }
        bool changed = folder_extraction_.has_value();
        if (drop_on_new_page) {
            changed =
                layout_.move_item_to_new_page(
                    drag_source_layout_index_) ||
                changed;
        } else if (
            folder_drop_target_visible_position_ != kNoPage &&
            folder_drop_target_visible_position_ <
                visible_items_.size()) {
            const VisibleItem target =
                visible_items_[
                    folder_drop_target_visible_position_];
            if (target.kind == VisibleItemKind::folder) {
                changed =
                    layout_.add_app_to_folder(
                        drag_source_layout_index_,
                        target.layout_index) ||
                    changed;
            } else {
                changed =
                    layout_.create_folder(
                        drag_source_layout_index_,
                        target.layout_index,
                        L"Папка") ||
                    changed;
            }
        } else if (
            drag_target_visible_position_ != kNoPage &&
            drag_target_visible_position_ < visible_items_.size()) {
            const VisibleItem target =
                visible_items_[drag_target_visible_position_];
            changed =
                layout_.move_item(
                    drag_source_layout_index_,
                    target.layout_index) ||
                changed;
        } else if (destination_page != drag_source_page_) {
            const auto [first, last] =
                visible_page_range(destination_page);
            if (first < last) {
                const std::size_t target_position =
                    destination_page > drag_source_page_
                        ? last - 1
                        : first;
                changed =
                    layout_.move_item(
                        drag_source_layout_index_,
                        visible_items_[target_position]
                            .layout_index) ||
                    changed;
            }
        }
        write_drag_diagnostic(
            changed ? L"finish-changed" : L"finish-unchanged",
            drag_current_x_,
            drag_current_y_,
            folder_drop_target_visible_position_ != kNoPage
                ? folder_drop_target_visible_position_
                : drag_target_visible_position_);

        drag_active_ = false;
        drag_candidate_ = false;
        drag_source_visible_position_ = kNoPage;
        drag_source_layout_index_ = kNoPage;
        drag_target_visible_position_ = kNoPage;
        folder_drop_target_visible_position_ = kNoPage;
        folder_hover_candidate_visible_position_ = kNoPage;
        folder_hover_origin_ = 0;
        drag_folder_intent_locked_ = false;
        drag_edge_direction_ = 0;
        drag_edge_latched_ = false;
        drag_provisional_page_ = false;
        folder_extraction_.reset();
        clear_root_live_reflow();
        if (changed) {
            save_layout_state();
            rebuild_filter(destination_page);
            LARGE_INTEGER animation_counter{};
            QueryPerformanceCounter(&animation_counter);
            root_reflow_animation_active_ =
                animate_root_reflow &&
                !root_reflow_from_centers_.empty();
            root_reflow_animation_origin_ =
                animation_counter.QuadPart;

            folder_drop_animation_active_ = false;
            folder_drop_target_bounds_valid_ = false;
            folder_drop_animation_path_.clear();
            folder_drop_animation_target_layout_index_ =
                kNoPage;
            if (animations_enabled_ &&
                !dropped_app_path.empty()) {
                const std::wstring normalized_drop_path =
                    launchpad::lowercase(dropped_app_path);
                for (std::size_t index = 0;
                     index < layout_.items().size();
                     ++index) {
                    const launchpad::LayoutItem& item =
                        layout_.items()[index];
                    if (item.kind !=
                        launchpad::LayoutItemKind::folder) {
                        continue;
                    }
                    const bool contains_app =
                        std::ranges::any_of(
                            item.children,
                            [&normalized_drop_path](
                                const std::wstring& path) {
                                return launchpad::lowercase(path) ==
                                    normalized_drop_path;
                            });
                    if (!contains_app) {
                        continue;
                    }
                    folder_drop_animation_active_ = true;
                    folder_drop_animation_path_ =
                        dropped_app_path;
                    folder_drop_animation_target_layout_index_ =
                        index;
                    folder_drop_animation_from_bounds_ =
                        folder_drop_from_bounds;
                    folder_drop_animation_origin_ =
                        animation_counter.QuadPart;
                    break;
                }
            }
        } else {
            root_reflow_animation_active_ = false;
            root_reflow_from_centers_.clear();
            current_page_ = std::min(
                drag_source_page_,
                visible_page_count() - 1);
            select_first_item_on_page(current_page_);
            reset_page_motion();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void cancel_drag() {
        const bool restore_source_page = drag_active_;
        const bool restore_extraction =
            folder_extraction_.has_value();
        const auto snapback_centers =
            root_drag_current_centers_;
        drag_active_ = false;
        drag_candidate_ = false;
        drag_source_visible_position_ = kNoPage;
        drag_source_layout_index_ = kNoPage;
        drag_target_visible_position_ = kNoPage;
        folder_drop_target_visible_position_ = kNoPage;
        folder_hover_candidate_visible_position_ = kNoPage;
        folder_hover_origin_ = 0;
        drag_folder_intent_locked_ = false;
        drag_edge_direction_ = 0;
        drag_edge_latched_ = false;
        drag_provisional_page_ = false;
        clear_root_live_reflow();
        if (restore_extraction) {
            restore_folder_extraction();
            return;
        }
        if (restore_source_page) {
            current_page_ = std::min(
                drag_source_page_,
                visible_page_count() - 1);
            select_first_item_on_page(current_page_);
            reset_page_motion();
            if (animations_enabled_ &&
                !snapback_centers.empty()) {
                root_reflow_from_centers_ =
                    snapback_centers;
                LARGE_INTEGER counter{};
                QueryPerformanceCounter(&counter);
                root_reflow_animation_origin_ =
                    counter.QuadPart;
                root_reflow_animation_active_ = true;
            }
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void change_folder_page(int delta) {
        if (open_folder_index_ >= layout_.items().size() ||
            folder_closing_) {
            return;
        }
        const auto& children =
            layout_.items()[open_folder_index_].children;
        const std::size_t count = std::max<std::size_t>(
            1,
            (children.size() + kFolderPageCapacity - 1) /
                kFolderPageCapacity);
        const auto current =
            static_cast<std::ptrdiff_t>(folder_page_);
        const auto last = static_cast<std::ptrdiff_t>(count - 1);
        const std::size_t next = static_cast<std::size_t>(
            std::clamp<std::ptrdiff_t>(
                current + delta,
                0,
                last));
        if (next == folder_page_) {
            return;
        }
        folder_page_ = next;
        folder_drag_target_position_ = kNoPage;
        folder_drop_regions_.clear();
        folder_selected_position_ = std::min(
            folder_page_ * kFolderPageCapacity,
            children.empty() ? 0 : children.size() - 1);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void move_folder_selection(int delta) {
        if (open_folder_index_ >= layout_.items().size() ||
            folder_closing_) {
            return;
        }
        const auto& children =
            layout_.items()[open_folder_index_].children;
        if (children.empty()) {
            return;
        }
        folder_selection_visible_ = true;
        const auto current =
            static_cast<std::ptrdiff_t>(folder_selected_position_);
        const auto last =
            static_cast<std::ptrdiff_t>(children.size() - 1);
        folder_selected_position_ = static_cast<std::size_t>(
            std::clamp<std::ptrdiff_t>(
                current + delta,
                0,
                last));
        folder_page_ =
            folder_selected_position_ / kFolderPageCapacity;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void launch_folder_selected() {
        if (open_folder_index_ >= layout_.items().size() ||
            folder_closing_ ||
            closing_) {
            return;
        }
        const launchpad::LayoutItem& folder =
            layout_.items()[open_folder_index_];
        if (folder_selected_position_ >= folder.children.size()) {
            return;
        }
        const std::size_t app_index = app_index_for_path(
            folder.children[folder_selected_position_]);
        if (app_index == kNoPage) {
            return;
        }
        launch_app(apps_[app_index]);
    }

    std::size_t hit_test_page_dot(int x, int y) const {
        if (!render_target_) {
            return kNoPage;
        }
        const D2D1_POINT_2F point = client_point_to_dips(x, y);
        for (const PageDotRegion& region : page_dot_regions_) {
            if (point.x >= region.bounds.left &&
                point.x <= region.bounds.right &&
                point.y >= region.bounds.top &&
                point.y <= region.bounds.bottom) {
                return region.page;
            }
        }
        return kNoPage;
    }

    std::size_t hit_test_regions(
        const std::vector<HitRegion>& regions,
        int x,
        int y) const {
        if (!render_target_) {
            return kNoPage;
        }
        const D2D1_POINT_2F point = client_point_to_dips(x, y);
        for (const HitRegion& region : regions) {
            if (point.x >= region.bounds.left &&
                point.x <= region.bounds.right &&
                point.y >= region.bounds.top &&
                point.y <= region.bounds.bottom) {
                return region.visible_position;
            }
        }
        return kNoPage;
    }

    std::size_t hit_test_root_delete(int x, int y) const {
        return edit_mode_ && search_.empty()
            ? hit_test_regions(delete_hit_regions_, x, y)
            : kNoPage;
    }

    std::size_t hit_test_folder_delete(int x, int y) const {
        return edit_mode_ && open_folder_index_ != kNoPage
            ? hit_test_regions(folder_delete_hit_regions_, x, y)
            : kNoPage;
    }

    bool hit_test_search(int x, int y) const {
        if (!render_target_) {
            return false;
        }
        const D2D1_POINT_2F point = client_point_to_dips(x, y);
        const D2D1_ROUNDED_RECT box =
            search_box(render_target_->GetSize());
        return point.x >= box.rect.left &&
            point.x <= box.rect.right &&
            point.y >= box.rect.top &&
            point.y <= box.rect.bottom;
    }

    bool hit_test_folder_title(int x, int y) {
        if (!render_target_ ||
            open_folder_index_ == kNoPage ||
            folder_closing_ ||
            open_folder_index_ >= layout_.items().size()) {
            return false;
        }
        const D2D1_SIZE_F size = render_target_->GetSize();
        const FolderGeometry geometry =
            calculate_folder_geometry(
                size,
                layout_.items()[open_folder_index_].children.size(),
                current_folder_progress(),
                folder_origin_bounds_valid_
                    ? &folder_origin_bounds_
                    : nullptr);
        const D2D1_POINT_2F point = client_point_to_dips(x, y);
        return
            point.x >= geometry.title_editor.rect.left &&
            point.x <= geometry.title_editor.rect.right &&
            point.y >= geometry.title_editor.rect.top &&
            point.y <= geometry.title_editor.rect.bottom;
    }

    void begin_folder_name_edit() {
        if (open_folder_index_ >= layout_.items().size() ||
            layout_.items()[open_folder_index_].kind !=
                launchpad::LayoutItemKind::folder) {
            return;
        }
        folder_name_buffer_ =
            layout_.items()[open_folder_index_].name;
        folder_name_editing_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void commit_folder_name_edit() {
        if (!folder_name_editing_ ||
            open_folder_index_ >= layout_.items().size()) {
            return;
        }
        std::wstring name = folder_name_buffer_;
        while (!name.empty() && std::iswspace(name.front())) {
            name.erase(name.begin());
        }
        while (!name.empty() && std::iswspace(name.back())) {
            name.pop_back();
        }
        if (name.empty()) {
            name = L"Папка";
        }
        layout_.items()[open_folder_index_].name =
            std::move(name);
        folder_name_editing_ = false;
        folder_name_buffer_.clear();
        save_layout_state();
        rebuild_filter(current_page_);
    }

    void cancel_folder_name_edit() {
        folder_name_editing_ = false;
        folder_name_buffer_.clear();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    D2D1_POINT_2F client_point_to_dips(int x, int y) const {
        const float scale =
            96.0F / static_cast<float>(GetDpiForWindow(hwnd_));
        return D2D1::Point2F(
            static_cast<float>(x) * scale,
            static_cast<float>(y) * scale);
    }

    D2D1_POINT_2F client_delta_to_dips(int x, int y) const {
        return client_point_to_dips(x, y);
    }

    bool is_click_without_drag(
        int start_x,
        int start_y,
        int end_x,
        int end_y) const {
        const D2D1_POINT_2F delta = client_delta_to_dips(
            end_x - start_x,
            end_y - start_y);
        return std::hypot(delta.x, delta.y) <= kClickSlopDips;
    }

    bool is_managed_app_file(const fs::path& path) const {
        std::error_code error;
        const fs::path root =
            fs::weakly_canonical(applications_directory_, error);
        if (error) {
            return false;
        }
        const fs::path candidate =
            fs::weakly_canonical(path, error);
        if (error ||
            !fs::is_regular_file(candidate, error) ||
            error ||
            !launchpad::is_supported_app_extension(
                candidate.extension().wstring())) {
            return false;
        }
        const DWORD attributes = GetFileAttributesW(
            candidate.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return false;
        }
        std::wstring root_text = launchpad::lowercase(
            root.lexically_normal().wstring());
        const std::wstring candidate_text = launchpad::lowercase(
            candidate.lexically_normal().wstring());
        if (!root_text.empty() &&
            root_text.back() != L'\\' &&
            root_text.back() != L'/') {
            root_text.push_back(fs::path::preferred_separator);
        }
        return candidate_text.size() > root_text.size() &&
            candidate_text.starts_with(root_text);
    }

    DeleteModalButton hit_test_delete_modal_button(
        int x,
        int y) const {
        if (!delete_confirmation_active_ || !render_target_) {
            return DeleteModalButton::none;
        }
        const D2D1_POINT_2F point =
            client_point_to_dips(x, y);
        const DeleteModalGeometry geometry =
            delete_modal_geometry(render_target_->GetSize());
        const auto contains =
            [point](const D2D1_RECT_F& bounds) {
            return point.x >= bounds.left &&
                point.x <= bounds.right &&
                point.y >= bounds.top &&
                point.y <= bounds.bottom;
        };
        if (contains(geometry.cancel_button.rect)) {
            return DeleteModalButton::cancel;
        }
        if (contains(geometry.confirm_button.rect)) {
            return DeleteModalButton::confirm;
        }
        return DeleteModalButton::none;
    }

    void dismiss_delete_confirmation() {
        delete_confirmation_active_ = false;
        delete_modal_pressed_button_ =
            DeleteModalButton::none;
        pending_delete_name_.clear();
        pending_delete_path_.clear();
        pending_delete_page_ = 0;
        if (GetCapture() == hwnd_) {
            ReleaseCapture();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void request_delete_confirmation(const AppEntry& app) {
        if (!is_managed_app_file(fs::path(app.path))) {
            MessageBoxW(
                hwnd_,
                L"Этот объект находится вне управляемой папки "
                L"Applications и не может быть удалён из Launchpad.",
                L"Windows Launchpad",
                MB_OK | MB_ICONERROR);
            return;
        }
        pending_delete_name_ = app.name;
        pending_delete_path_ = app.path;
        pending_delete_page_ = current_page_;
        delete_modal_pressed_button_ =
            DeleteModalButton::none;
        delete_confirmation_active_ = true;
        mouse_down_delete_position_ = kNoPage;
        mouse_down_folder_delete_position_ = kNoPage;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void confirm_pending_delete() {
        if (!delete_confirmation_active_ ||
            pending_delete_path_.empty()) {
            dismiss_delete_confirmation();
            return;
        }
        AppEntry pending{
            .name = pending_delete_name_,
            .path = pending_delete_path_,
        };
        const std::size_t page = pending_delete_page_;
        const bool deleting_from_folder =
            open_folder_index_ < layout_.items().size() &&
            layout_.items()[open_folder_index_].kind ==
                launchpad::LayoutItemKind::folder;
        const bool animate_root_reflow =
            animations_enabled_ &&
            !deleting_from_folder &&
            !page_transition_active_;
        std::unordered_map<std::wstring, D2D1_POINT_2F>
            folder_reflow_from_centers;
        if (deleting_from_folder) {
            root_reflow_animation_active_ = false;
            folder_drag_reflow_active_ = false;
            folder_drag_reflow_from_centers_.clear();
            const launchpad::LayoutItem& folder =
                layout_.items()[open_folder_index_];
            for (const HitRegion& region : folder_hit_regions_) {
                if (region.visible_position >=
                    folder.children.size()) {
                    continue;
                }
                folder_reflow_from_centers.insert_or_assign(
                    launchpad::lowercase(
                        folder.children[region.visible_position]),
                    D2D1::Point2F(
                        (region.icon_bounds.left +
                         region.icon_bounds.right) *
                            0.5F,
                        (region.icon_bounds.top +
                         region.icon_bounds.bottom) *
                            0.5F));
            }
            root_reflow_from_centers_.clear();
        } else if (animate_root_reflow) {
            root_reflow_animation_active_ = false;
            capture_root_reflow_positions();
        } else {
            root_reflow_animation_active_ = false;
            root_reflow_from_centers_.clear();
        }
        dismiss_delete_confirmation();
        if (!recycle_app_file(pending)) {
            root_reflow_animation_active_ = false;
            root_reflow_from_centers_.clear();
            folder_drag_reflow_active_ = false;
            folder_drag_reflow_from_centers_.clear();
            return;
        }
        if (!rescan_apps(page, false) ||
            app_index_for_path(pending.path) != kNoPage) {
            root_reflow_animation_active_ = false;
            root_reflow_from_centers_.clear();
            folder_drag_reflow_active_ = false;
            folder_drag_reflow_from_centers_.clear();
            return;
        }

        LARGE_INTEGER animation_counter{};
        QueryPerformanceCounter(&animation_counter);
        if (deleting_from_folder) {
            folder_drag_reflow_from_centers_ =
                std::move(folder_reflow_from_centers);
            folder_drag_reflow_origin_ =
                animation_counter.QuadPart;
            folder_drag_reflow_active_ =
                animations_enabled_ &&
                open_folder_index_ < layout_.items().size() &&
                !folder_drag_reflow_from_centers_.empty();
            if (!folder_drag_reflow_active_) {
                folder_drag_reflow_from_centers_.clear();
            }
        } else {
            const bool has_surviving_item =
                std::ranges::any_of(
                    visible_items_,
                    [this](const VisibleItem& visible) {
                        return root_reflow_from_centers_.contains(
                            root_visual_key(visible));
                    });
            root_reflow_animation_origin_ =
                animation_counter.QuadPart;
            root_reflow_animation_active_ =
                animate_root_reflow &&
                current_page_ == page &&
                has_surviving_item;
            if (!root_reflow_animation_active_) {
                root_reflow_from_centers_.clear();
            }
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    bool move_app_to_quarantine(const fs::path& path) {
        const fs::path quarantine =
            layout_path_.parent_path() /
            L"Removed Items";
        std::error_code error;
        fs::create_directories(quarantine, error);
        if (error) {
            return false;
        }

        fs::path destination =
            quarantine / path.filename();
        for (int suffix = 1;
             fs::exists(destination, error) && !error;
             ++suffix) {
            destination =
                quarantine /
                (path.stem().wstring() +
                 L" (" + std::to_wstring(suffix) + L")" +
                 path.extension().wstring());
        }
        if (error) {
            return false;
        }

        fs::rename(path, destination, error);
        if (!error) {
            SHChangeNotify(
                SHCNE_RENAMEITEM,
                SHCNF_PATHW,
                path.c_str(),
                destination.c_str());
            return true;
        }

        error.clear();
        fs::copy_file(
            path,
            destination,
            fs::copy_options::none,
            error);
        if (error) {
            return false;
        }
        error.clear();
        if (!fs::remove(path, error) || error) {
            std::error_code cleanup_error;
            fs::remove(destination, cleanup_error);
            return false;
        }
        SHChangeNotify(
            SHCNE_DELETE,
            SHCNF_PATHW,
            path.c_str(),
            nullptr);
        SHChangeNotify(
            SHCNE_CREATE,
            SHCNF_PATHW,
            destination.c_str(),
            nullptr);
        return true;
    }

    bool recycle_app_file(const AppEntry& app) {
        const fs::path path = app.path;
        if (!is_managed_app_file(path)) {
            MessageBoxW(
                hwnd_,
                L"Этот объект находится вне управляемой папки "
                L"Applications и не может быть удалён из Launchpad.",
                L"Windows Launchpad",
                MB_OK | MB_ICONERROR);
            return false;
        }

        ComPtr<IFileOperation> operation;
        HRESULT result = CoCreateInstance(
            CLSID_FileOperation,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(operation.ReleaseAndGetAddressOf()));
        if (SUCCEEDED(result)) {
            result = operation->SetOwnerWindow(hwnd_);
        }
        if (SUCCEEDED(result)) {
            result = operation->SetOperationFlags(
                FOF_ALLOWUNDO |
                FOF_NOCONFIRMATION |
                FOF_NOERRORUI |
                FOFX_RECYCLEONDELETE);
        }
        ComPtr<IShellItem> shell_item;
        if (SUCCEEDED(result)) {
            result = SHCreateItemFromParsingName(
                path.c_str(),
                nullptr,
                IID_PPV_ARGS(
                    shell_item.ReleaseAndGetAddressOf()));
        }
        if (SUCCEEDED(result)) {
            result = operation->DeleteItem(
                shell_item.Get(),
                nullptr);
        }
        if (SUCCEEDED(result)) {
            const HRESULT perform_result =
                operation->PerformOperations();
            BOOL aborted = FALSE;
            const HRESULT aborted_result =
                operation->GetAnyOperationsAborted(&aborted);
            result = FAILED(perform_result)
                ? perform_result
                : aborted_result;
            if (SUCCEEDED(result) && aborted) {
                result = E_ABORT;
            }
        }

        std::error_code exists_error;
        const bool file_still_exists =
            fs::exists(path, exists_error);
        if (!exists_error && !file_still_exists) {
            return true;
        }
        if (move_app_to_quarantine(path)) {
            return true;
        }
        if (FAILED(result) ||
            file_still_exists ||
            exists_error) {
            const std::wstring code = result == E_ABORT
                ? L"E_ABORT (0x80004004)"
                : std::to_wstring(static_cast<long>(result));
            const std::wstring message =
                L"Не удалось убрать объект из Launchpad.\nКод: " +
                code;
            MessageBoxW(
                hwnd_,
                message.c_str(),
                L"Windows Launchpad",
                MB_OK | MB_ICONERROR);
            return false;
        }
        return false;
    }

    void delete_root_app(std::size_t visible_position) {
        if (visible_position >= visible_items_.size()) {
            return;
        }
        const VisibleItem visible =
            visible_items_[visible_position];
        if (visible.kind != VisibleItemKind::app) {
            return;
        }
        const AppEntry* app = app_for_visible(visible);
        if (!app) {
            return;
        }
        request_delete_confirmation(*app);
    }

    void delete_folder_app(std::size_t folder_position) {
        if (open_folder_index_ >= layout_.items().size()) {
            return;
        }
        const launchpad::LayoutItem& folder =
            layout_.items()[open_folder_index_];
        if (folder.kind != launchpad::LayoutItemKind::folder ||
            folder_position >= folder.children.size()) {
            return;
        }
        const std::size_t app_index = app_index_for_path(
            folder.children[folder_position]);
        if (app_index == kNoPage) {
            return;
        }
        request_delete_confirmation(apps_[app_index]);
    }

    void launch_app(const AppEntry& app) {
        SHELLEXECUTEINFOW execute_info{
            .cbSize = sizeof(SHELLEXECUTEINFOW),
            .fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC,
            .hwnd = hwnd_,
            .lpVerb = L"open",
            .lpFile = app.path.c_str(),
            .nShow = SW_SHOWNORMAL,
        };
        if (!ShellExecuteExW(&execute_info)) {
            const DWORD error = GetLastError();
            std::wstring message =
                L"Не удалось запустить «" + app.name +
                L"».\nКод ошибки: " + std::to_wstring(error);
            MessageBoxW(
                hwnd_,
                message.c_str(),
                L"Windows Launchpad",
                MB_OK | MB_ICONERROR);
            return;
        }
        set_search_focused(false, false);
        search_.clear();
        search_all_selected_ = false;
        finish_folder_close();
        rebuild_filter();
        request_close();
    }

    void launch_selected() {
        if (visible_items_.empty() ||
            selected_position_ >= visible_items_.size() ||
            page_transition_active_ ||
            page_drag_active_ ||
            closing_) {
            return;
        }
        const VisibleItem& visible =
            visible_items_[selected_position_];
        if (visible.kind == VisibleItemKind::folder) {
            open_folder(visible.layout_index);
            return;
        }
        const AppEntry* app = app_for_visible(visible);
        if (!app) {
            return;
        }
        launch_app(*app);
    }

    void begin_external_drop_rescan(int x, int y) {
        PendingExternalDrop pending{
            .target_page = current_page_,
        };
        pending.previous_paths.reserve(apps_.size());
        for (const AppEntry& app : apps_) {
            pending.previous_paths.insert(
                launchpad::lowercase(app.path));
        }

        if (open_folder_index_ == kNoPage &&
            search_.empty() &&
            !page_transition_active_ &&
            !page_drag_active_) {
            const std::size_t position =
                hit_test_regions(hit_regions_, x, y);
            if (position < visible_items_.size()) {
                pending.target_layout_index =
                    visible_items_[position].layout_index;
            }
        }

        if (delete_confirmation_active_) {
            dismiss_delete_confirmation();
        }
        if (open_folder_index_ != kNoPage) {
            finish_folder_close();
        }
        search_.clear();
        search_all_selected_ = false;
        set_search_focused(false, false);
        edit_mode_ = false;
        pending.target_page = std::min(
            pending.target_page,
            visible_page_count() - 1);
        rebuild_filter(pending.target_page);
        pending.target_page = current_page_;

        KillTimer(hwnd_, kExternalDropRescanTimer);
        pending_external_drop_ = std::move(pending);
        if (SetTimer(
                hwnd_,
                kExternalDropRescanTimer,
                kExternalDropRescanDelayMs,
                nullptr) == 0) {
            finish_external_drop_rescan();
        }
    }

    void finish_external_drop_rescan() {
        if (!pending_external_drop_) {
            return;
        }
        PendingExternalDrop& pending =
            *pending_external_drop_;
        const std::size_t target_page =
            pending.target_page;

        std::vector<AppEntry> discovered_apps;
        const bool scan_succeeded =
            load_apps(
                applications_directory_,
                discovered_apps);
        const bool new_file_visible =
            scan_succeeded &&
            std::ranges::any_of(
                discovered_apps,
                [&pending](const AppEntry& app) {
                    return !pending.previous_paths.contains(
                        launchpad::lowercase(app.path));
                });
        if (!new_file_visible &&
            --pending.attempts_remaining > 0) {
            if (SetTimer(
                hwnd_,
                kExternalDropRescanTimer,
                kExternalDropRescanDelayMs,
                nullptr) == 0) {
                pending_external_drop_.reset();
            }
            return;
        }
        if (!new_file_visible) {
            pending_external_drop_.reset();
            return;
        }

        rescan_apps(target_page, false);
        std::vector<std::wstring> added_paths;
        for (const AppEntry& app : apps_) {
            if (!pending.previous_paths.contains(
                    launchpad::lowercase(app.path))) {
                added_paths.push_back(app.path);
            }
        }

        const std::size_t requested_anchor =
            pending.target_layout_index;
        if (added_paths.empty()) {
            pending_external_drop_.reset();
            return;
        }

        std::size_t anchor = kNoPage;
        if (requested_anchor < layout_.items().size() &&
            layout_.items()[requested_anchor].kind !=
                launchpad::LayoutItemKind::page_break) {
            anchor = requested_anchor;
        } else {
            const auto [first, last] =
                visible_page_range(target_page);
            for (std::size_t position = last;
                 position > first;
                 --position) {
                const VisibleItem& visible =
                    visible_items_[position - 1];
                const launchpad::LayoutItem& item =
                    layout_.items()[visible.layout_index];
                const bool existed_before =
                    item.kind ==
                        launchpad::LayoutItemKind::folder ||
                    pending.previous_paths.contains(
                        launchpad::lowercase(item.app_path));
                if (existed_before) {
                    anchor = visible.layout_index;
                    break;
                }
            }
        }

        bool moved = false;
        if (anchor != kNoPage) {
            for (const std::wstring& path : added_paths) {
                const std::wstring normalized =
                    launchpad::lowercase(path);
                const auto found = std::ranges::find_if(
                    layout_.items(),
                    [&normalized](
                        const launchpad::LayoutItem& item) {
                        return item.kind ==
                                launchpad::LayoutItemKind::app &&
                            launchpad::lowercase(item.app_path) ==
                                normalized;
                    });
                if (found == layout_.items().end()) {
                    continue;
                }
                const std::size_t source =
                    static_cast<std::size_t>(
                        std::distance(
                            layout_.items().begin(),
                            found));
                if (source == anchor) {
                    ++anchor;
                    continue;
                }
                if (layout_.move_item(source, anchor)) {
                    moved = true;
                    ++anchor;
                }
            }
        }
        pending_external_drop_.reset();
        if (moved) {
            save_layout_state();
            rebuild_filter(target_page);
        }

        const std::size_t restored_position =
            visible_position_for_root_app(added_paths.front());
        if (restored_position != kNoPage) {
            const std::size_t restored_page =
                visible_page_for_position(restored_position);
            if (restored_page != current_page_) {
                rebuild_filter(restored_page);
            }
            selected_position_ =
                visible_position_for_root_app(added_paths.front());
            selection_visible_ = false;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    bool rescan_apps(
        std::size_t preferred_page = kNoPage,
        bool reveal_new_apps = true) {
        if (preferred_page == kNoPage) {
            preferred_page = current_page_;
        }
        std::unordered_set<std::wstring> previous_paths;
        previous_paths.reserve(apps_.size());
        for (const AppEntry& app : apps_) {
            previous_paths.insert(
                launchpad::lowercase(app.path));
        }
        const bool folder_was_open =
            open_folder_index_ != kNoPage;
        std::unordered_set<std::wstring> open_folder_paths;
        if (folder_was_open &&
            open_folder_index_ < layout_.items().size() &&
            layout_.items()[open_folder_index_].kind ==
                launchpad::LayoutItemKind::folder) {
            for (const std::wstring& path :
                 layout_.items()[open_folder_index_].children) {
                open_folder_paths.insert(
                    launchpad::lowercase(path));
            }
        }
        std::vector<AppEntry> rescanned_apps;
        if (!load_apps(
                applications_directory_,
                rescanned_apps)) {
            return false;
        }
        apps_ = std::move(rescanned_apps);
        std::vector<std::wstring> added_paths;
        for (const AppEntry& app : apps_) {
            if (!previous_paths.contains(
                    launchpad::lowercase(app.path))) {
                added_paths.push_back(app.path);
            }
        }
        reconcile_layout();
        if (folder_was_open) {
            std::size_t resolved_folder = kNoPage;
            for (std::size_t index = 0;
                 index < layout_.items().size();
                 ++index) {
                const launchpad::LayoutItem& item =
                    layout_.items()[index];
                if (item.kind !=
                    launchpad::LayoutItemKind::folder) {
                    continue;
                }
                const bool same_folder =
                    std::ranges::any_of(
                        item.children,
                        [&open_folder_paths](
                            const std::wstring& path) {
                            return open_folder_paths.contains(
                                launchpad::lowercase(path));
                        });
                if (same_folder) {
                    resolved_folder = index;
                    break;
                }
            }
            if (resolved_folder != kNoPage) {
                open_folder_index_ = resolved_folder;
                refresh_open_folder_apps();
                const auto& children =
                    layout_.items()[open_folder_index_].children;
                if (!children.empty()) {
                    folder_selected_position_ = std::min(
                        folder_selected_position_,
                        children.size() - 1);
                    folder_page_ =
                        folder_selected_position_ /
                        kFolderPageCapacity;
                }
            } else {
                finish_folder_close();
            }
        }
        preload_icons();
        selection_visible_ = false;
        rebuild_filter(preferred_page);
        if (reveal_new_apps &&
            !folder_was_open &&
            !added_paths.empty()) {
            const std::size_t position =
                visible_position_for_root_app(
                    added_paths.front());
            if (position != kNoPage) {
                const std::size_t page =
                    visible_page_for_position(position);
                if (page != current_page_) {
                    rebuild_filter(page);
                }
                selected_position_ =
                    visible_position_for_root_app(
                        added_paths.front());
                selection_visible_ = false;
            }
        }
        if (const auto signature =
                applications_directory_signature(
                    applications_directory_)) {
            applications_signature_ = *signature;
        }
        pending_removal_signature_.reset();
        pending_removal_checks_ = 0;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    void refresh_apps_if_changed() {
        if (!hwnd_ ||
            !IsWindowVisible(hwnd_) ||
            !intro_complete_ ||
            closing_ ||
            delete_confirmation_active_ ||
            pending_external_drop_ ||
            drag_active_ ||
            drag_candidate_ ||
            folder_drag_active_ ||
            folder_drag_candidate_ ||
            page_drag_active_ ||
            page_transition_active_ ||
            folder_animation_active_ ||
            folder_closing_ ||
            folder_drop_animation_active_ ||
            root_reflow_animation_active_ ||
            root_drag_reflow_active_ ||
            folder_drag_reflow_active_) {
            return;
        }
        const auto signature =
            applications_directory_signature(
                applications_directory_);
        if (!signature) {
            return;
        }
        if (*signature == applications_signature_) {
            pending_removal_signature_.reset();
            pending_removal_checks_ = 0;
            return;
        }

        std::vector<AppEntry> discovered_apps;
        if (!load_apps(
                applications_directory_,
                discovered_apps)) {
            return;
        }
        std::unordered_set<std::wstring> current_paths;
        std::unordered_set<std::wstring> discovered_paths;
        current_paths.reserve(apps_.size());
        discovered_paths.reserve(discovered_apps.size());
        for (const AppEntry& app : apps_) {
            current_paths.insert(
                launchpad::lowercase(app.path));
        }
        for (const AppEntry& app : discovered_apps) {
            discovered_paths.insert(
                launchpad::lowercase(app.path));
        }
        const bool has_additions =
            std::ranges::any_of(
                discovered_paths,
                [&current_paths](
                    const std::wstring& path) {
                    return !current_paths.contains(path);
                });
        const bool has_removals =
            std::ranges::any_of(
                current_paths,
                [&discovered_paths](
                    const std::wstring& path) {
                    return !discovered_paths.contains(path);
                });

        if (!has_additions && !has_removals) {
            applications_signature_ = *signature;
            pending_removal_signature_.reset();
            pending_removal_checks_ = 0;
            return;
        }
        if (!has_additions && has_removals) {
            if (!pending_removal_signature_ ||
                *pending_removal_signature_ != *signature) {
                pending_removal_signature_ = *signature;
                pending_removal_checks_ = 1;
                return;
            }
            ++pending_removal_checks_;
            if (pending_removal_checks_ <
                kApplicationsRemovalConfirmations) {
                return;
            }
        } else {
            pending_removal_signature_.reset();
            pending_removal_checks_ = 0;
        }
        rescan_apps(current_page_);
    }

    void open_applications_directory() {
        SHELLEXECUTEINFOW execute_info{
            .cbSize = sizeof(SHELLEXECUTEINFOW),
            .fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC,
            .hwnd = hwnd_,
            .lpVerb = L"open",
            .lpFile = applications_directory_.c_str(),
            .nShow = SW_SHOWNORMAL,
        };
        ShellExecuteExW(&execute_info);
    }

    void show_launchpad() {
        const RECT bounds = active_monitor_bounds();
        const bool was_hidden = !IsWindowVisible(hwnd_);
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        if (was_hidden) {
            capture_background(bounds);
            if (!search_.empty()) {
                search_.clear();
                search_all_selected_ = false;
                rebuild_filter(current_page_);
            }
        }
        if (delete_confirmation_active_) {
            dismiss_delete_confirmation();
        }
        closing_ = false;
        edit_mode_ = false;
        set_search_focused(false, false);
        close_start_visibility_ = 1.0F;
        reset_page_motion();
        if (was_hidden) {
            SetWindowPos(
                hwnd_,
                nullptr,
                bounds.left,
                bounds.top,
                width,
                height,
                SWP_NOACTIVATE | SWP_NOZORDER);
        }
        reset_animation_clock();
        intro_complete_ = !animations_enabled_;
        if (was_hidden) {
            const BOOL cloak = TRUE;
            const bool cloak_enabled =
                SUCCEEDED(DwmSetWindowAttribute(
                    hwnd_,
                    DWMWA_CLOAK,
                    &cloak,
                    sizeof(cloak)));
            SetWindowPos(
                hwnd_,
                HWND_TOP,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE |
                    SWP_NOACTIVATE | SWP_SHOWWINDOW);
            RedrawWindow(
                hwnd_,
                nullptr,
                nullptr,
                RDW_INVALIDATE | RDW_UPDATENOW |
                    RDW_NOERASE);
            reset_animation_clock();
            if (cloak_enabled) {
                const BOOL uncloak = FALSE;
                DwmSetWindowAttribute(
                    hwnd_,
                    DWMWA_CLOAK,
                    &uncloak,
                    sizeof(uncloak));
            }
        } else {
            SetWindowPos(
                hwnd_,
                HWND_TOP,
                bounds.left,
                bounds.top,
                width,
                height,
                SWP_SHOWWINDOW);
        }
        start_frame_pump();
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void request_close() {
        if (closing_ || !IsWindowVisible(hwnd_)) {
            return;
        }
        if (delete_confirmation_active_) {
            dismiss_delete_confirmation();
        }
        if (drag_active_ || drag_candidate_) {
            cancel_drag();
        }
        if (folder_drag_active_ || folder_drag_candidate_) {
            cancel_folder_drag();
        }
        set_search_focused(false, false);
        edit_mode_ = false;
        if (!animations_enabled_) {
            finish_close();
            return;
        }
        close_start_visibility_ = intro_complete_
            ? 1.0F
            : std::clamp(
                  spring_ease_out(static_cast<float>(
                      elapsed_seconds() /
                      kOpenAnimationSeconds)),
                  0.0F,
                  1.0F);
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        close_animation_origin_ = counter.QuadPart;
        closing_ = true;
        reset_page_motion();
        page_drag_active_ = false;
        page_drag_moved_ = false;
        mouse_down_on_item_ = false;
        mouse_down_on_background_ = false;
        mouse_down_page_ = kNoPage;
        hit_regions_.clear();
        if (GetCapture() == hwnd_) {
            ReleaseCapture();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void finish_close() {
        closing_ = false;
        edit_mode_ = false;
        set_search_focused(false, false);
        if (delete_confirmation_active_) {
            dismiss_delete_confirmation();
        }
        if (open_folder_index_ != kNoPage) {
            finish_folder_close();
        }
        stop_frame_pump();
        if (background_mode_) {
            ShowWindow(hwnd_, SW_HIDE);
            release_background_capture();
        } else {
            DestroyWindow(hwnd_);
        }
    }

    static RECT active_monitor_bounds() {
        POINT point{};
        GetCursorPos(&point);
        const HMONITOR monitor =
            MonitorFromPoint(point, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO monitor_info{sizeof(MONITORINFO)};
        GetMonitorInfoW(monitor, &monitor_info);
        return monitor_info.rcMonitor;
    }

    static DWORD query_refresh_rate() {
        POINT point{};
        GetCursorPos(&point);
        const HMONITOR monitor =
            MonitorFromPoint(point, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFOEXW monitor_info{};
        monitor_info.cbSize = sizeof(monitor_info);
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        DWORD refresh_rate = 60;
        if (GetMonitorInfoW(
                monitor,
                reinterpret_cast<MONITORINFO*>(&monitor_info)) &&
            EnumDisplaySettingsExW(
                monitor_info.szDevice,
                ENUM_CURRENT_SETTINGS,
                &mode,
                0) &&
            mode.dmDisplayFrequency >= 30 &&
            mode.dmDisplayFrequency <= 500) {
            refresh_rate = mode.dmDisplayFrequency;
        }
        return refresh_rate;
    }

    void animation_tick() {
        if (!IsWindowVisible(hwnd_)) {
            return;
        }

        if (folder_drop_animation_active_ &&
            elapsed_since(folder_drop_animation_origin_) >=
                kFolderDropAnimationSeconds) {
            folder_drop_animation_active_ = false;
            folder_drop_target_bounds_valid_ = false;
            folder_drop_animation_path_.clear();
            folder_drop_animation_target_layout_index_ =
                kNoPage;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (root_reflow_animation_active_ &&
            elapsed_since(root_reflow_animation_origin_) >=
                kRootReflowAnimationSeconds) {
            root_reflow_animation_active_ = false;
            root_reflow_from_centers_.clear();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (root_drag_reflow_active_ &&
            elapsed_since(root_drag_reflow_origin_) >=
                kLiveReorderAnimationSeconds) {
            root_drag_reflow_active_ = false;
            root_drag_reflow_from_centers_.clear();
        }
        if (folder_drag_reflow_active_ &&
            elapsed_since(folder_drag_reflow_origin_) >=
                kLiveReorderAnimationSeconds) {
            folder_drag_reflow_active_ = false;
            folder_drag_reflow_from_centers_.clear();
        }
        if (search_focus_animation_active_ &&
            elapsed_since(search_focus_animation_origin_) >=
                kSearchFocusAnimationSeconds) {
            search_focus_progress_ = search_focus_target_;
            search_focus_animation_active_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        const float caret_opacity =
            current_search_caret_opacity();
        if (std::abs(
                caret_opacity -
                search_caret_last_opacity_) >= 0.003F) {
            search_caret_last_opacity_ = caret_opacity;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }

        if (drag_candidate_ &&
            elapsed_since(drag_press_origin_) >=
                kLongPressSeconds) {
            start_drag();
        }
        if (folder_drag_candidate_ &&
            elapsed_since(folder_drag_press_origin_) >=
                kLongPressSeconds) {
            start_folder_drag();
        }
        if (drag_active_) {
            maybe_advance_drag_page();
            maybe_activate_folder_hover();
        }

        if (closing_ &&
            elapsed_since(close_animation_origin_) >=
                kCloseAnimationSeconds) {
            finish_close();
        } else if (
            folder_closing_ &&
            elapsed_since(folder_animation_origin_) >=
                kFolderAnimationSeconds) {
            finish_folder_close();
        } else if (
            closing_ ||
            (animations_enabled_ &&
             (!intro_complete_ ||
              page_transition_active_ ||
              folder_animation_active_ ||
              folder_name_editing_ ||
              folder_drop_animation_active_ ||
              root_reflow_animation_active_ ||
              search_focus_animation_active_)) ||
            page_drag_active_ ||
            drag_active_ ||
            folder_drag_active_ ||
            edit_mode_ ||
            icons_pending_) {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void update_frame_timing() {
        const DWORD refresh_rate = query_refresh_rate();
        fallback_frame_interval_ms_ = static_cast<UINT>(
            std::clamp(
                std::lround(
                    1000.0 /
                    static_cast<double>(refresh_rate)),
                2L,
                16L));
        frame_period_qpc_ = std::max<std::int64_t>(
            1,
            static_cast<std::int64_t>(std::llround(
                clock_frequency_ /
                static_cast<double>(refresh_rate))));
        vblank_qpc_ = 0;

        DWM_TIMING_INFO timing{};
        timing.cbSize = sizeof(timing);
        if (SUCCEEDED(DwmGetCompositionTimingInfo(
                nullptr,
                &timing)) &&
            timing.qpcRefreshPeriod > 0) {
            const std::int64_t period =
                static_cast<std::int64_t>(
                    timing.qpcRefreshPeriod);
            const std::int64_t minimum_period =
                static_cast<std::int64_t>(
                    clock_frequency_ / 500.0);
            const std::int64_t maximum_period =
                static_cast<std::int64_t>(
                    clock_frequency_ / 30.0);
            const std::int64_t monitor_period =
                frame_period_qpc_;
            const std::int64_t monitor_tolerance =
                std::max<std::int64_t>(
                    1,
                    monitor_period / 20);
            if (period >= minimum_period &&
                period <= maximum_period &&
                std::abs(period - monitor_period) <=
                    monitor_tolerance) {
                frame_period_qpc_ = period;
                vblank_qpc_ = static_cast<std::int64_t>(
                    timing.qpcVBlank);
            }
        }
    }

    bool arm_frame_timer(std::int64_t target_qpc) const {
        if (!frame_timer_ || clock_frequency_ <= 0.0) {
            return false;
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const std::int64_t delta = std::max<std::int64_t>(
            1,
            target_qpc - now.QuadPart);
        const long double delay_100ns =
            static_cast<long double>(delta) *
            10000000.0L /
            static_cast<long double>(clock_frequency_);
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<LONGLONG>(
            1,
            static_cast<LONGLONG>(
                std::llround(delay_100ns)));
        return SetWaitableTimerEx(
                   frame_timer_,
                   &due,
                   0,
                   nullptr,
                   nullptr,
                   nullptr,
                   0) != FALSE;
    }

    void start_fallback_frame_timer() {
        frame_pump_active_ =
            SetTimer(
                hwnd_,
                kAnimationTimer,
                std::max<UINT>(
                    USER_TIMER_MINIMUM,
                    fallback_frame_interval_ms_),
                nullptr) != 0;
    }

    bool replace_with_standard_frame_timer(
        std::int64_t target_qpc) {
        if (frame_timer_) {
            CloseHandle(frame_timer_);
            frame_timer_ = nullptr;
        }
        frame_timer_ = CreateWaitableTimerExW(
            nullptr,
            nullptr,
            0,
            TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!frame_timer_) {
            return false;
        }
        if (!timer_period_raised_ &&
            timeBeginPeriod(1) == TIMERR_NOERROR) {
            timer_period_raised_ = true;
        }
        if (arm_frame_timer(target_qpc)) {
            return true;
        }
        CloseHandle(frame_timer_);
        frame_timer_ = nullptr;
        return false;
    }

    void start_frame_pump() {
        stop_frame_pump();
        if (!frame_timer_) {
            start_fallback_frame_timer();
            return;
        }

        update_frame_timing();
        frame_pump_active_ = true;

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const std::int64_t maximum_lead =
            static_cast<std::int64_t>(std::llround(
                clock_frequency_ * 0.003));
        const std::int64_t render_lead =
            std::min(
                frame_period_qpc_ / 2,
                maximum_lead);

        if (vblank_qpc_ > 0) {
            std::int64_t next_vblank = vblank_qpc_;
            const std::int64_t minimum =
                now.QuadPart + render_lead;
            if (next_vblank <= minimum) {
                const std::int64_t count =
                    (minimum - next_vblank) /
                        frame_period_qpc_ +
                    1;
                next_vblank +=
                    count * frame_period_qpc_;
            }
            next_frame_qpc_ =
                next_vblank - render_lead;
        } else {
            next_frame_qpc_ =
                now.QuadPart + frame_period_qpc_;
        }

        if (!arm_frame_timer(next_frame_qpc_)) {
            if (!replace_with_standard_frame_timer(
                    next_frame_qpc_)) {
                frame_pump_active_ = false;
                start_fallback_frame_timer();
            }
        }
    }

    void stop_frame_pump() {
        frame_pump_active_ = false;
        if (frame_timer_) {
            CancelWaitableTimer(frame_timer_);
        } else if (hwnd_) {
            KillTimer(hwnd_, kAnimationTimer);
        }
    }

    void on_frame_deadline() {
        if (!frame_pump_active_ ||
            !frame_timer_) {
            return;
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        do {
            next_frame_qpc_ += frame_period_qpc_;
        } while (next_frame_qpc_ <= now.QuadPart);

        if (!arm_frame_timer(next_frame_qpc_)) {
            if (!replace_with_standard_frame_timer(
                    next_frame_qpc_)) {
                frame_pump_active_ = false;
                start_fallback_frame_timer();
            }
        }
        animation_tick();
    }

    void reset_animation_clock() {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        clock_origin_ = counter.QuadPart;
    }

    double elapsed_seconds() const {
        return elapsed_since(clock_origin_);
    }

    double elapsed_since(std::int64_t origin) const {
        if (clock_frequency_ <= 0.0) {
            return 0.0;
        }
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        return static_cast<double>(counter.QuadPart - origin) /
            clock_frequency_;
    }

    static bool query_animations_enabled() {
        BOOL enabled = TRUE;
        return !SystemParametersInfoW(
                   SPI_GETCLIENTAREAANIMATION,
                   0,
                   &enabled,
                   0)
            ? true
            : enabled == TRUE;
    }

    void write_drag_diagnostic(
        std::wstring_view event,
        int x,
        int y,
        std::size_t position) const {
        if (!diagnostics_enabled_) {
            return;
        }
        const fs::path path =
            applications_directory_.parent_path() /
            L"LaunchpadDrag.log";
        const HANDLE file = CreateFileW(
            path.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return;
        }
        std::wstring line(event);
        line += L" x=" + std::to_wstring(x);
        line += L" y=" + std::to_wstring(y);
        line += L" position=";
        line += position == kNoPage
            ? L"none"
            : std::to_wstring(position);
        line += L" regions=" +
            std::to_wstring(hit_regions_.size());
        line += L"\r\n";
        DWORD written = 0;
        WriteFile(
            file,
            line.data(),
            static_cast<DWORD>(
                line.size() * sizeof(wchar_t)),
            &written,
            nullptr);
        CloseHandle(file);
    }

private:
    static constexpr std::size_t kNoPage =
        std::numeric_limits<std::size_t>::max();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    bool background_mode_ = false;
    bool diagnostics_enabled_ = false;
    bool animations_enabled_ = true;
    bool intro_complete_ = false;
    bool page_transition_active_ = false;
    bool closing_ = false;
    bool mouse_down_on_item_ = false;
    bool mouse_down_on_background_ = false;
    bool mouse_down_on_folder_item_ = false;
    bool mouse_down_on_folder_background_ = false;
    bool page_drag_active_ = false;
    bool page_drag_moved_ = false;
    bool selection_visible_ = false;
    bool folder_selection_visible_ = false;
    bool folder_animation_active_ = false;
    bool folder_closing_ = false;
    bool folder_name_editing_ = false;
    bool drag_candidate_ = false;
    bool drag_active_ = false;
    bool folder_drag_candidate_ = false;
    bool folder_drag_active_ = false;
    bool folder_drop_animation_active_ = false;
    bool folder_drop_target_bounds_valid_ = false;
    bool root_reflow_animation_active_ = false;
    bool root_drag_reflow_active_ = false;
    bool folder_drag_reflow_active_ = false;
    bool drag_folder_intent_locked_ = false;
    bool drag_provisional_page_ = false;
    bool drag_edge_latched_ = false;
    bool edit_mode_ = false;
    bool delete_confirmation_active_ = false;
    bool folder_panel_bounds_valid_ = false;
    bool folder_origin_bounds_valid_ = false;
    bool icons_pending_ = true;
    bool frame_pump_active_ = false;
    bool timer_period_raised_ = false;
    bool external_drop_target_registered_ = false;
    bool search_all_selected_ = false;
    bool search_focused_ = false;
    bool search_focus_animation_active_ = false;
    bool search_caret_delayed_reveal_ = false;
    std::wstring search_;
    std::wstring folder_name_buffer_;
    std::wstring folder_drop_animation_path_;
    std::wstring pending_delete_name_;
    std::wstring pending_delete_path_;
    fs::path applications_directory_;
    fs::path layout_path_;
    std::uint64_t applications_signature_ = 0;
    std::optional<std::uint64_t> pending_removal_signature_;
    int pending_removal_checks_ = 0;
    std::vector<AppEntry> apps_;
    launchpad::LayoutDocument layout_;
    std::unordered_map<std::wstring, std::size_t> app_by_path_;
    std::vector<VisibleItem> visible_items_;
    std::vector<std::size_t> visible_page_starts_{0};
    std::vector<HitRegion> hit_regions_;
    std::vector<HitRegion> root_drop_regions_;
    std::vector<HitRegion> delete_hit_regions_;
    std::vector<HitRegion> folder_hit_regions_;
    std::vector<HitRegion> folder_drop_regions_;
    std::vector<HitRegion> folder_delete_hit_regions_;
    std::vector<PageDotRegion> page_dot_regions_;
    std::unordered_map<std::wstring, D2D1_POINT_2F>
        root_reflow_from_centers_;
    std::unordered_map<std::wstring, D2D1_POINT_2F>
        root_drag_reflow_from_centers_;
    std::unordered_map<std::wstring, D2D1_POINT_2F>
        root_drag_current_centers_;
    std::unordered_map<std::wstring, D2D1_POINT_2F>
        folder_drag_reflow_from_centers_;
    std::unordered_map<std::wstring, D2D1_POINT_2F>
        folder_drag_current_centers_;
    std::vector<std::size_t> open_folder_app_indices_;
    std::optional<launchpad::LayoutItem> folder_closing_visual_;
    std::optional<FolderExtractionTransaction> folder_extraction_;
    std::optional<PendingExternalDrop> pending_external_drop_;
    std::size_t current_page_ = 0;
    std::size_t transition_from_page_ = 0;
    std::size_t page_transition_neighbor_page_ = kNoPage;
    std::size_t selected_position_ = 0;
    std::size_t open_folder_index_ = kNoPage;
    std::size_t folder_selected_position_ = 0;
    std::size_t folder_page_ = 0;
    std::size_t mouse_down_page_ = kNoPage;
    std::size_t mouse_down_folder_position_ = kNoPage;
    std::size_t mouse_down_delete_position_ = kNoPage;
    std::size_t mouse_down_folder_delete_position_ = kNoPage;
    std::size_t pending_delete_page_ = 0;
    std::size_t drag_source_visible_position_ = kNoPage;
    std::size_t drag_source_layout_index_ = kNoPage;
    std::size_t drag_target_visible_position_ = kNoPage;
    std::size_t folder_drop_target_visible_position_ = kNoPage;
    std::size_t folder_hover_candidate_visible_position_ =
        kNoPage;
    std::size_t drag_source_page_ = 0;
    std::size_t folder_drag_source_position_ = kNoPage;
    std::size_t folder_drag_target_position_ = kNoPage;
    std::size_t folder_closing_hidden_position_ = kNoPage;
    std::size_t folder_drop_animation_target_layout_index_ =
        kNoPage;
    int left_mouse_down_x_ = 0;
    int left_mouse_down_y_ = 0;
    int page_drag_start_x_ = 0;
    int page_drag_start_y_ = 0;
    int page_drag_current_x_ = 0;
    int page_drag_current_y_ = 0;
    int drag_current_x_ = 0;
    int drag_current_y_ = 0;
    int folder_drag_current_x_ = 0;
    int folder_drag_current_y_ = 0;
    int drag_edge_direction_ = 0;
    VisibleItemKind drag_source_kind_ = VisibleItemKind::app;
    DeleteModalButton delete_modal_pressed_button_ =
        DeleteModalButton::none;
    int vertical_wheel_remainder_ = 0;
    int horizontal_wheel_remainder_ = 0;
    int icon_load_budget_ = 0;
    UINT icon_request_pixels_ = kBaseIconRequestPixels;
    UINT fallback_frame_interval_ms_ = 8;
    double clock_frequency_ = 0.0;
    std::int64_t clock_origin_ = 0;
    std::int64_t page_transition_origin_ = 0;
    std::int64_t close_animation_origin_ = 0;
    std::int64_t folder_animation_origin_ = 0;
    std::int64_t folder_drop_animation_origin_ = 0;
    std::int64_t root_reflow_animation_origin_ = 0;
    std::int64_t root_drag_reflow_origin_ = 0;
    std::int64_t folder_drag_reflow_origin_ = 0;
    std::int64_t search_focus_animation_origin_ = 0;
    std::int64_t search_caret_origin_ = 0;
    std::int64_t folder_hover_origin_ = 0;
    std::int64_t drag_press_origin_ = 0;
    std::int64_t folder_drag_press_origin_ = 0;
    std::int64_t drag_edge_hover_origin_ = 0;
    std::int64_t frame_period_qpc_ = 0;
    std::int64_t next_frame_qpc_ = 0;
    std::int64_t vblank_qpc_ = 0;
    int page_transition_direction_ = 1;
    float page_transition_width_ = 1.0F;
    float page_transition_start_offset_ = 0.0F;
    float page_transition_target_offset_ = 0.0F;
    float page_transition_initial_velocity_ = 0.0F;
    float close_start_visibility_ = 1.0F;
    float search_focus_progress_ = 0.0F;
    float search_focus_from_ = 0.0F;
    float search_focus_target_ = 0.0F;
    float search_caret_last_opacity_ = 0.0F;
    float folder_close_start_progress_ = 1.0F;
    D2D1_RECT_F folder_panel_bounds_{};
    D2D1_RECT_F folder_origin_bounds_{};
    D2D1_RECT_F folder_drop_animation_from_bounds_{};
    D2D1_RECT_F folder_drop_animation_target_bounds_{};
    std::array<PageDragSample, 8> page_drag_samples_{};
    std::size_t page_drag_sample_count_ = 0;
    HANDLE frame_timer_ = nullptr;

    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> write_factory_;
    ComPtr<IWICImagingFactory> wic_factory_;
    ComPtr<IDropTarget> external_drop_target_;
    ComPtr<ID2D1HwndRenderTarget> render_target_;
    ComPtr<ID2D1Bitmap> background_bitmap_;
    ComPtr<ID2D1SolidColorBrush> white_brush_;
    ComPtr<ID2D1SolidColorBrush> color_brush_;
    ComPtr<IDWriteTextFormat> label_format_;
    ComPtr<IDWriteTextFormat> search_format_;
    ComPtr<IDWriteTextFormat> glyph_format_;
    ComPtr<IDWriteTextFormat> empty_format_;
    ComPtr<IDWriteTextFormat> folder_title_format_;
    ComPtr<IDWriteInlineObject> ellipsis_;
    HBITMAP background_capture_ = nullptr;
    D2D1_SIZE_U background_bitmap_size_{};
};

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetCurrentProcessExplicitAppUserModelID(kAppUserModelId);
    SetProcessDpiAwarenessContext(
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    clear_startup_feedback_cursor();

    const bool create_shortcuts =
        has_command_line_switch(L"--create-shortcuts");
    const bool remove_shortcuts =
        has_command_line_switch(L"--remove-shortcuts");
    if (create_shortcuts || remove_shortcuts) {
        if (create_shortcuts && remove_shortcuts) {
            return 2;
        }
        const HRESULT maintenance_ole_result =
            OleInitialize(nullptr);
        if (FAILED(maintenance_ole_result)) {
            return 1;
        }
        const bool succeeded = create_shortcuts
            ? create_application_shortcuts(
                has_command_line_switch(
                    L"--desktop-shortcut"))
            : remove_application_shortcuts();
        OleUninitialize();
        return succeeded ? 0 : 1;
    }

    if (HWND existing = FindWindowW(kWindowClass, nullptr)) {
        PostMessageW(existing, kShowLaunchpadMessage, 0, 0);
        return 0;
    }

    const HRESULT ole_result = OleInitialize(nullptr);
    if (FAILED(ole_result)) {
        MessageBoxW(
            nullptr,
            L"Не удалось инициализировать OLE.",
            L"Windows Launchpad",
            MB_OK | MB_ICONERROR);
        return 1;
    }

    LaunchpadWindow window;
    const bool start_hidden =
        has_command_line_switch(L"--background");
    const bool keep_alive =
        !has_command_line_switch(L"--exit-on-close");
    if (!window.create(
            instance,
            keep_alive,
            start_hidden)) {
        MessageBoxW(
            nullptr,
            L"Не удалось создать окно Windows Launchpad.",
            L"Windows Launchpad",
            MB_OK | MB_ICONERROR);
        OleUninitialize();
        return 1;
    }

    const int result = window.run();
    OleUninitialize();
    return result;
}
