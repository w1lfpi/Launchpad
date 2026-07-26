#include "frame_diagnostics.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>

namespace launchpad {
namespace {

constexpr double kAggregateSeconds = 1.0;
constexpr double kTimelineResetSeconds = 0.25;
constexpr double kMinimumPlausibleRefreshHz = 20.0;
constexpr double kMaximumPlausibleRefreshHz = 1000.0;
constexpr double kFallbackValidationRefreshHz = 500.0;
constexpr double kCounterDeltaMultiplier = 3.0;
constexpr double kCounterDeltaSlack = 6.0;
constexpr double kSyncDeltaMultiplier = 4.0;
constexpr double kSyncDeltaSlackFrames = 2.0;

std::size_t percentile_index(
    std::size_t count,
    double percentile) noexcept {
    if (count == 0) {
        return 0;
    }
    const double rank = std::ceil(
        percentile * static_cast<double>(count));
    const std::size_t index = rank > 1.0
        ? static_cast<std::size_t>(rank - 1.0)
        : 0;
    return std::min(index, count - 1);
}

} // namespace

FrameDiagnostics::FrameDiagnostics() noexcept = default;

FrameDiagnostics::~FrameDiagnostics() noexcept {
    shutdown();
}

void FrameDiagnostics::MetricSamples::add(double value) noexcept {
    if (!std::isfinite(value) || value < 0.0) {
        return;
    }
    if (count < values.size()) {
        values[count++] = value;
    } else {
        ++overflow;
    }
}

void FrameDiagnostics::MetricSamples::clear() noexcept {
    count = 0;
    overflow = 0;
}

void FrameDiagnostics::copy_text(
    wchar_t* destination,
    std::size_t capacity,
    const wchar_t* source) noexcept {
    if (!destination || capacity == 0) {
        return;
    }
    destination[0] = L'\0';
    if (!source) {
        return;
    }
    wcsncpy_s(destination, capacity, source, _TRUNCATE);
}

FrameDiagnostics::MetricSummary FrameDiagnostics::summarize(
    const MetricSamples& samples) noexcept {
    MetricSummary result{};
    if (samples.count == 0) {
        return result;
    }

    std::array<double, kMaximumSamples> sorted{};
    std::copy_n(
        samples.values.begin(),
        samples.count,
        sorted.begin());
    std::sort(sorted.begin(), sorted.begin() + samples.count);

    result.median =
        sorted[percentile_index(samples.count, 0.50)];
    result.p95 =
        sorted[percentile_index(samples.count, 0.95)];
    result.p99 =
        sorted[percentile_index(samples.count, 0.99)];
    result.maximum = sorted[samples.count - 1];
    return result;
}

void FrameDiagnostics::configure(
    const FrameDiagnosticsConfig& config) noexcept {
    shutdown();

    enabled_ = config.enabled;
    if (!enabled_) {
        return;
    }

    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) ||
        frequency.QuadPart <= 0) {
        enabled_ = false;
        return;
    }
    counter_frequency_ =
        static_cast<double>(frequency.QuadPart);
    target_refresh_hz_ = std::max(
        0.0,
        config.target_refresh_hz);
    copy_text(
        adapter_name_.data(),
        adapter_name_.size(),
        config.adapter_name);
    copy_text(
        synchronization_.data(),
        synchronization_.size(),
        config.synchronization);
    copy_text(
        output_file_path_.data(),
        output_file_path_.size(),
        config.output_file_path);

    if (output_file_path_[0] != L'\0') {
        output_file_ = CreateFileW(
            output_file_path_.data(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ |
                FILE_SHARE_WRITE |
                FILE_SHARE_DELETE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
    }

    aggregate_origin_ = query_counter();
    reset_aggregate();

    std::array<wchar_t, 768> line{};
    swprintf_s(
        line.data(),
        line.size(),
        L"[LaunchpadFrame] configured adapter=\"%ls\" "
        L"target=%.3fHz sync=\"%ls\" file=%ls\r\n",
        adapter_name_.data(),
        target_refresh_hz_,
        synchronization_.data(),
        output_file_ != INVALID_HANDLE_VALUE
            ? output_file_path_.data()
            : L"disabled");
    write_line(line.data());
}

void FrameDiagnostics::shutdown() noexcept {
    if (enabled_) {
        flush();
    }
    if (output_file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(output_file_);
        output_file_ = INVALID_HANDLE_VALUE;
    }
    enabled_ = false;
    counter_frequency_ = 0.0;
    target_refresh_hz_ = 0.0;
    aggregate_origin_ = 0;
    frame_origin_ = 0;
    previous_frame_origin_ = 0;
    update_origin_ = 0;
    draw_origin_ = 0;
    present_origin_ = 0;
    previous_statistics_query_ = 0;
    previous_statistics_valid_ = false;
}

bool FrameDiagnostics::enabled() const noexcept {
    return enabled_;
}

std::int64_t FrameDiagnostics::query_counter() const noexcept {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

double FrameDiagnostics::milliseconds_between(
    std::int64_t begin,
    std::int64_t end) const noexcept {
    if (counter_frequency_ <= 0.0 ||
        begin <= 0 ||
        end < begin) {
        return 0.0;
    }
    return static_cast<double>(end - begin) *
        1000.0 /
        counter_frequency_;
}

bool FrameDiagnostics::is_continuous_gap(
    std::int64_t begin,
    std::int64_t end) const noexcept {
    if (begin <= 0 || end <= begin ||
        counter_frequency_ <= 0.0) {
        return false;
    }
    return static_cast<double>(end - begin) /
        counter_frequency_ <= kTimelineResetSeconds;
}

void FrameDiagnostics::begin_frame() noexcept {
    if (!enabled_) {
        return;
    }
    const std::int64_t now = query_counter();
    if (is_continuous_gap(previous_frame_origin_, now)) {
        frame_intervals_.add(
            milliseconds_between(
                previous_frame_origin_,
                now));
    }
    previous_frame_origin_ = now;
    frame_origin_ = now;
    update_origin_ = 0;
    draw_origin_ = 0;
    present_origin_ = 0;
}

void FrameDiagnostics::begin_update() noexcept {
    if (enabled_) {
        update_origin_ = query_counter();
    }
}

void FrameDiagnostics::end_update() noexcept {
    end_phase(update_origin_, update_durations_);
}

void FrameDiagnostics::begin_draw() noexcept {
    if (enabled_) {
        draw_origin_ = query_counter();
    }
}

void FrameDiagnostics::end_draw() noexcept {
    end_phase(draw_origin_, draw_durations_);
}

void FrameDiagnostics::begin_present() noexcept {
    if (enabled_) {
        present_origin_ = query_counter();
    }
}

void FrameDiagnostics::end_present(HRESULT result) noexcept {
    if (!enabled_) {
        return;
    }
    end_phase(present_origin_, present_durations_);
    last_present_result_ = result;
    if (result == S_OK) {
        ++successful_presents_;
    } else if (SUCCEEDED(result)) {
        ++present_statuses_;
    } else {
        ++failed_presents_;
    }
}

void FrameDiagnostics::end_phase(
    std::int64_t& phase_origin,
    MetricSamples& samples) noexcept {
    if (!enabled_ || phase_origin <= 0) {
        return;
    }
    const std::int64_t now = query_counter();
    samples.add(milliseconds_between(phase_origin, now));
    phase_origin = 0;
}

void FrameDiagnostics::record_frame_statistics(
    HRESULT statistics_result,
    const DXGI_FRAME_STATISTICS* statistics) noexcept {
    if (!enabled_) {
        return;
    }

    last_statistics_result_ = statistics_result;
    const std::int64_t now = query_counter();
    if (statistics_result ==
        DXGI_ERROR_FRAME_STATISTICS_DISJOINT) {
        ++statistics_disjoint_;
        previous_statistics_valid_ = false;
        previous_statistics_query_ = now;
        return;
    }
    if (FAILED(statistics_result) || !statistics) {
        ++statistics_failures_;
        previous_statistics_valid_ = false;
        previous_statistics_query_ = now;
        return;
    }

    ++statistics_samples_;
    const bool timeline_continuous =
        previous_statistics_valid_ &&
        is_continuous_gap(previous_statistics_query_, now);
    if (timeline_continuous) {
        const bool counters_monotonic =
            statistics->PresentCount >=
                previous_statistics_.PresentCount &&
            statistics->PresentRefreshCount >=
                previous_statistics_.PresentRefreshCount &&
            statistics->SyncRefreshCount >=
                previous_statistics_.SyncRefreshCount &&
            statistics->SyncQPCTime.QuadPart >=
                previous_statistics_.SyncQPCTime.QuadPart;
        const double query_seconds =
            static_cast<double>(
                now - previous_statistics_query_) /
            counter_frequency_;
        const double validation_refresh_hz =
            std::isfinite(target_refresh_hz_) &&
                    target_refresh_hz_ >=
                        kMinimumPlausibleRefreshHz &&
                    target_refresh_hz_ <=
                        kMaximumPlausibleRefreshHz
                ? target_refresh_hz_
                : kFallbackValidationRefreshHz;
        const UINT maximum_counter_delta =
            static_cast<UINT>(std::ceil(
                query_seconds *
                    validation_refresh_hz *
                    kCounterDeltaMultiplier +
                kCounterDeltaSlack));

        UINT present_delta = 0;
        UINT refresh_delta = 0;
        UINT sync_refresh_delta = 0;
        LONGLONG sync_delta = 0;
        bool plausible = counters_monotonic;
        if (plausible) {
            present_delta =
                statistics->PresentCount -
                previous_statistics_.PresentCount;
            refresh_delta =
                statistics->PresentRefreshCount -
                previous_statistics_.PresentRefreshCount;
            sync_refresh_delta =
                statistics->SyncRefreshCount -
                previous_statistics_.SyncRefreshCount;
            sync_delta =
                statistics->SyncQPCTime.QuadPart -
                previous_statistics_.SyncQPCTime.QuadPart;
            plausible =
                present_delta <= maximum_counter_delta &&
                refresh_delta <= maximum_counter_delta &&
                sync_refresh_delta <= maximum_counter_delta &&
                !(present_delta > 0 && refresh_delta == 0) &&
                !(sync_refresh_delta > 0 && sync_delta <= 0);
        }

        if (plausible && sync_delta > 0) {
            const double sync_seconds =
                static_cast<double>(sync_delta) /
                counter_frequency_;
            const double refresh_period_seconds =
                1.0 / validation_refresh_hz;
            const double reported_refresh_seconds =
                static_cast<double>(std::max(
                    refresh_delta,
                    sync_refresh_delta)) *
                refresh_period_seconds;
            const double maximum_sync_seconds = std::max(
                query_seconds * kSyncDeltaMultiplier +
                    refresh_period_seconds *
                        kSyncDeltaSlackFrames,
                (reported_refresh_seconds +
                 refresh_period_seconds *
                     kSyncDeltaSlackFrames) *
                    2.0);
            plausible =
                sync_seconds <= maximum_sync_seconds;
        }

        if (!plausible) {
            // Parallels and some virtual drivers can return a valid HRESULT
            // while resetting or mixing counter epochs. Treat the current
            // record as a new baseline; never turn it into a percentile or
            // a large skipped-refresh count.
            ++statistics_disjoint_;
            ++statistics_rejected_;
        } else if (present_delta > 0) {
            if (refresh_delta > present_delta) {
                skipped_refreshes_ +=
                    static_cast<std::uint64_t>(
                        refresh_delta - present_delta);
            }
            if (sync_delta > 0) {
                displayed_intervals_.add(
                    static_cast<double>(sync_delta) *
                    1000.0 /
                    counter_frequency_ /
                    static_cast<double>(present_delta));
            }
        }
    }

    previous_statistics_ = *statistics;
    previous_statistics_valid_ = true;
    previous_statistics_query_ = now;
}

void FrameDiagnostics::end_frame() noexcept {
    if (!enabled_) {
        return;
    }
    const std::int64_t now = query_counter();
    if (frame_origin_ > 0) {
        frame_work_.add(
            milliseconds_between(frame_origin_, now));
    }
    frame_origin_ = 0;
    ++frames_;
    maybe_flush(now, false);
}

void FrameDiagnostics::flush() noexcept {
    if (!enabled_) {
        return;
    }
    maybe_flush(query_counter(), true);
}

void FrameDiagnostics::maybe_flush(
    std::int64_t now,
    bool force) noexcept {
    if (!enabled_ || aggregate_origin_ <= 0 ||
        counter_frequency_ <= 0.0) {
        return;
    }
    const double seconds =
        static_cast<double>(now - aggregate_origin_) /
        counter_frequency_;
    if (!force && seconds < kAggregateSeconds) {
        return;
    }
    if (frames_ == 0 &&
        successful_presents_ == 0 &&
        present_statuses_ == 0 &&
        failed_presents_ == 0) {
        aggregate_origin_ = now;
        reset_aggregate();
        return;
    }

    const MetricSummary interval =
        summarize(frame_intervals_);
    const MetricSummary work =
        summarize(frame_work_);
    const MetricSummary update =
        summarize(update_durations_);
    const MetricSummary draw =
        summarize(draw_durations_);
    const MetricSummary present =
        summarize(present_durations_);
    const MetricSummary displayed =
        summarize(displayed_intervals_);
    const double safe_seconds = std::max(seconds, 0.000001);
    const double frames_per_second =
        static_cast<double>(successful_presents_) /
        safe_seconds;
    const std::uint64_t sample_overflow =
        frame_intervals_.overflow +
        frame_work_.overflow +
        update_durations_.overflow +
        draw_durations_.overflow +
        present_durations_.overflow +
        displayed_intervals_.overflow;

    std::array<wchar_t, 3072> line{};
    swprintf_s(
        line.data(),
        line.size(),
        L"[LaunchpadFrame] adapter=\"%ls\" target=%.3fHz "
        L"sync=\"%ls\" window=%.3fs frames=%llu fps=%.2f "
        L"present_ok=%llu status=%llu failed=%llu "
        L"interval_ms[p50=%.3f p95=%.3f p99=%.3f max=%.3f] "
        L"work_ms[p50=%.3f p95=%.3f p99=%.3f max=%.3f] "
        L"update_ms[p50=%.3f p95=%.3f max=%.3f] "
        L"draw_ms[p50=%.3f p95=%.3f max=%.3f] "
        L"present_ms[p50=%.3f p95=%.3f max=%.3f] "
        L"display_ms[p50=%.3f p95=%.3f p99=%.3f max=%.3f] "
        L"stats=%llu stats_failed=%llu disjoint=%llu "
        L"stats_rejected=%llu "
        L"refresh_skips=%llu sample_overflow=%llu "
        L"last_present=0x%08lX last_stats=0x%08lX\r\n",
        adapter_name_.data(),
        target_refresh_hz_,
        synchronization_.data(),
        safe_seconds,
        static_cast<unsigned long long>(frames_),
        frames_per_second,
        static_cast<unsigned long long>(
            successful_presents_),
        static_cast<unsigned long long>(present_statuses_),
        static_cast<unsigned long long>(failed_presents_),
        interval.median,
        interval.p95,
        interval.p99,
        interval.maximum,
        work.median,
        work.p95,
        work.p99,
        work.maximum,
        update.median,
        update.p95,
        update.maximum,
        draw.median,
        draw.p95,
        draw.maximum,
        present.median,
        present.p95,
        present.maximum,
        displayed.median,
        displayed.p95,
        displayed.p99,
        displayed.maximum,
        static_cast<unsigned long long>(statistics_samples_),
        static_cast<unsigned long long>(statistics_failures_),
        static_cast<unsigned long long>(statistics_disjoint_),
        static_cast<unsigned long long>(statistics_rejected_),
        static_cast<unsigned long long>(skipped_refreshes_),
        static_cast<unsigned long long>(sample_overflow),
        static_cast<unsigned long>(
            last_present_result_),
        static_cast<unsigned long>(
            last_statistics_result_));
    write_line(line.data());

    aggregate_origin_ = now;
    reset_aggregate();
}

void FrameDiagnostics::reset_aggregate() noexcept {
    frame_intervals_.clear();
    frame_work_.clear();
    update_durations_.clear();
    draw_durations_.clear();
    present_durations_.clear();
    displayed_intervals_.clear();
    frames_ = 0;
    successful_presents_ = 0;
    present_statuses_ = 0;
    failed_presents_ = 0;
    statistics_samples_ = 0;
    statistics_failures_ = 0;
    statistics_disjoint_ = 0;
    statistics_rejected_ = 0;
    skipped_refreshes_ = 0;
    last_present_result_ = S_OK;
    last_statistics_result_ = S_OK;
}

void FrameDiagnostics::write_line(
    const wchar_t* line) noexcept {
    if (!enabled_ || !line) {
        return;
    }
    OutputDebugStringW(line);
    if (output_file_ == INVALID_HANDLE_VALUE) {
        return;
    }

    const int wide_length =
        static_cast<int>(wcsnlen_s(line, 3072));
    if (wide_length <= 0) {
        return;
    }
    std::array<char, 8192> utf8{};
    const int utf8_length = WideCharToMultiByte(
        CP_UTF8,
        0,
        line,
        wide_length,
        utf8.data(),
        static_cast<int>(utf8.size()),
        nullptr,
        nullptr);
    if (utf8_length <= 0) {
        return;
    }
    DWORD written = 0;
    WriteFile(
        output_file_,
        utf8.data(),
        static_cast<DWORD>(utf8_length),
        &written,
        nullptr);
}

} // namespace launchpad
