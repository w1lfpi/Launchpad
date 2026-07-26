#pragma once

#include <windows.h>

#include <dxgi.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace launchpad {

// FrameDiagnostics is intentionally single-threaded. Call it from the same
// UI/render thread that owns the DXGI swap chain.
struct FrameDiagnosticsConfig {
    bool enabled = false;
    const wchar_t* adapter_name = L"unknown";
    double target_refresh_hz = 0.0;
    const wchar_t* synchronization = L"unknown";

    // nullptr or an empty string keeps diagnostics in OutputDebugString only.
    // When supplied, the file is opened once and receives one aggregate line
    // per second rather than per-frame writes.
    const wchar_t* output_file_path = nullptr;
};

class FrameDiagnostics final {
public:
    FrameDiagnostics() noexcept;
    ~FrameDiagnostics() noexcept;

    FrameDiagnostics(const FrameDiagnostics&) = delete;
    FrameDiagnostics& operator=(const FrameDiagnostics&) = delete;

    void configure(const FrameDiagnosticsConfig& config) noexcept;
    void shutdown() noexcept;

    [[nodiscard]] bool enabled() const noexcept;

    void begin_frame() noexcept;
    void begin_update() noexcept;
    void end_update() noexcept;
    void begin_draw() noexcept;
    void end_draw() noexcept;
    void begin_present() noexcept;
    void end_present(HRESULT result) noexcept;

    // Call after IDXGISwapChain::GetFrameStatistics. Pass nullptr unless
    // statistics_result succeeded. DISJOINT resets the comparison baseline.
    void record_frame_statistics(
        HRESULT statistics_result,
        const DXGI_FRAME_STATISTICS* statistics) noexcept;

    // Completes the frame and emits an aggregate only if one second elapsed.
    // Call this after record_frame_statistics.
    void end_frame() noexcept;

    // Emits a partial aggregate, if any. Useful before hiding or destroying
    // the render target. This still performs no work when diagnostics are off.
    void flush() noexcept;

private:
    static constexpr std::size_t kMaximumSamples = 512;
    static constexpr std::size_t kAdapterTextCapacity = 160;
    static constexpr std::size_t kSyncTextCapacity = 96;
    static constexpr std::size_t kPathCapacity = 512;

    struct MetricSamples {
        std::array<double, kMaximumSamples> values{};
        std::size_t count = 0;
        std::uint64_t overflow = 0;

        void add(double value) noexcept;
        void clear() noexcept;
    };

    struct MetricSummary {
        double median = 0.0;
        double p95 = 0.0;
        double p99 = 0.0;
        double maximum = 0.0;
    };

    static void copy_text(
        wchar_t* destination,
        std::size_t capacity,
        const wchar_t* source) noexcept;
    static MetricSummary summarize(
        const MetricSamples& samples) noexcept;

    [[nodiscard]] std::int64_t query_counter() const noexcept;
    [[nodiscard]] double milliseconds_between(
        std::int64_t begin,
        std::int64_t end) const noexcept;
    [[nodiscard]] bool is_continuous_gap(
        std::int64_t begin,
        std::int64_t end) const noexcept;

    void end_phase(
        std::int64_t& phase_origin,
        MetricSamples& samples) noexcept;
    void maybe_flush(std::int64_t now, bool force) noexcept;
    void reset_aggregate() noexcept;
    void write_line(const wchar_t* line) noexcept;

    bool enabled_ = false;
    double counter_frequency_ = 0.0;
    double target_refresh_hz_ = 0.0;
    HANDLE output_file_ = INVALID_HANDLE_VALUE;

    std::array<wchar_t, kAdapterTextCapacity> adapter_name_{};
    std::array<wchar_t, kSyncTextCapacity> synchronization_{};
    std::array<wchar_t, kPathCapacity> output_file_path_{};

    std::int64_t aggregate_origin_ = 0;
    std::int64_t frame_origin_ = 0;
    std::int64_t previous_frame_origin_ = 0;
    std::int64_t update_origin_ = 0;
    std::int64_t draw_origin_ = 0;
    std::int64_t present_origin_ = 0;
    std::int64_t previous_statistics_query_ = 0;

    MetricSamples frame_intervals_{};
    MetricSamples frame_work_{};
    MetricSamples update_durations_{};
    MetricSamples draw_durations_{};
    MetricSamples present_durations_{};
    MetricSamples displayed_intervals_{};

    std::uint64_t frames_ = 0;
    std::uint64_t successful_presents_ = 0;
    std::uint64_t present_statuses_ = 0;
    std::uint64_t failed_presents_ = 0;
    std::uint64_t statistics_samples_ = 0;
    std::uint64_t statistics_failures_ = 0;
    std::uint64_t statistics_disjoint_ = 0;
    std::uint64_t statistics_rejected_ = 0;
    std::uint64_t skipped_refreshes_ = 0;

    HRESULT last_present_result_ = S_OK;
    HRESULT last_statistics_result_ = S_OK;
    bool previous_statistics_valid_ = false;
    DXGI_FRAME_STATISTICS previous_statistics_{};
};

} // namespace launchpad
