#include "cuervo/jobs/job_system.hpp"

#include <charconv>

#include "cuervo/core/log.hpp"
#include "cuervo/math/simd.hpp"
#include "cuervo/platform/platform.hpp"

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

namespace cuervo::jobs {

namespace {

// Set while a thread executes tasks, so nested batches run inline instead of
// deadlocking on the pool they are part of.
thread_local bool t_in_task = false;

void cpu_relax() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    _mm_pause();
#endif
}

// How long an idle worker polls for the next batch before sleeping. A fixed
// tick starts several batches back to back (particles, hash build, queries);
// polling across those gaps avoids a futex wake-up per batch. Wall-clock
// timing here only affects speed, never results.
constexpr int spin_iterations = 4000;

} // namespace

JobSystem::JobSystem(unsigned thread_count) { set_thread_count(thread_count); }

JobSystem::~JobSystem() { stop_workers(); }

void JobSystem::set_thread_count(unsigned thread_count) {
    if constexpr (!platform::has_threads) thread_count = 1;
    thread_count = std::clamp(thread_count, 1u, 256u);
    if (thread_count == thread_count_ && workers_.size() + 1 == thread_count) return;
    stop_workers();
    thread_count_ = thread_count;
    start_workers();
}

void JobSystem::start_workers() {
    {
        std::lock_guard lock(mutex_);
        stop_ = false;
        stopping_.store(false, std::memory_order_relaxed);
    }
    workers_.reserve(thread_count_ - 1);
    for (unsigned i = 1; i < thread_count_; ++i) {
        workers_.emplace_back([this] { worker_main(); });
    }
}

void JobSystem::stop_workers() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        stopping_.store(true, std::memory_order_relaxed);
    }
    wake_.notify_all();
    for (std::thread& t : workers_) t.join();
    workers_.clear();
}

std::size_t JobSystem::chunk_count(std::size_t count, std::size_t min_grain) const {
    min_grain = std::max<std::size_t>(min_grain, 1);
    const std::size_t by_grain = (count + min_grain - 1) / min_grain;
    const std::size_t wanted = thread_count_ == 1 ? 1 : std::size_t{thread_count_} * 4;
    return std::max<std::size_t>(1, std::min(by_grain, wanted));
}

void JobSystem::drain(Batch& batch) {
    const bool was_in_task = t_in_task;
    t_in_task = true;
    for (;;) {
        const std::size_t task = batch.next.fetch_add(1, std::memory_order_relaxed);
        if (task >= batch.task_count) break;
        batch.fn(batch.fn_object, task);
        // Release: the task's writes happen-before the caller's acquire of
        // `done`, so the caller sees every task's results.
        batch.done.fetch_add(1, std::memory_order_release);
    }
    t_in_task = was_in_task;
}

void JobSystem::run_erased(std::size_t task_count, void* fn_object, TaskFn fn) {
    if (task_count == 0) return;
    if (workers_.empty() || task_count == 1 || t_in_task) {
        for (std::size_t task = 0; task < task_count; ++task) fn(fn_object, task);
        return;
    }
    Batch batch;
    batch.fn_object = fn_object;
    batch.fn = fn;
    batch.task_count = task_count;
    {
        std::lock_guard lock(mutex_);
        batch_ = &batch;
        epoch_.fetch_add(1, std::memory_order_release);
    }
    wake_.notify_all();
    ++parallel_batches_;

    drain(batch); // the caller works too
    while (batch.done.load(std::memory_order_acquire) < task_count) cpu_relax();
    {
        // Unpublish; workers that took the batch before this point are
        // counted in `active` and must leave it before it goes out of scope.
        std::lock_guard lock(mutex_);
        batch_ = nullptr;
    }
    while (batch.active.load(std::memory_order_acquire) != 0) cpu_relax();
}

void JobSystem::worker_main() {
    std::uint64_t seen = epoch_.load(std::memory_order_acquire);
    for (;;) {
        for (int i = 0; i < spin_iterations; ++i) {
            if (epoch_.load(std::memory_order_acquire) != seen || stopping_.load(std::memory_order_relaxed)) break;
            cpu_relax();
        }
        Batch* batch = nullptr;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stop_ || epoch_.load(std::memory_order_relaxed) != seen; });
            if (stop_) return;
            seen = epoch_.load(std::memory_order_relaxed);
            batch = batch_;
            if (batch == nullptr) continue; // finished before we got here
            batch->active.fetch_add(1, std::memory_order_relaxed);
        }
        drain(*batch);
        batch->active.fetch_sub(1, std::memory_order_release);
    }
}

// --- default pool and configuration -----------------------------------------

JobSystem& default_jobs() {
    static JobSystem jobs(1);
    return jobs;
}

void configure(unsigned thread_count) {
    default_jobs().set_thread_count(thread_count);
    LOG_INFO("Job system: {} thread(s){}; SIMD: {} ({} lanes){}", default_jobs().thread_count(),
             platform::has_threads ? "" : " (no thread support: inline)", simd::isa, simd::width,
             simd::available && !simd::enabled() ? ", off" : "");
}

std::optional<unsigned> parse_thread_count(std::string_view text) {
    unsigned value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || ptr != text.data() + text.size() || value == 0) return std::nullopt;
    return value;
}

unsigned auto_thread_count() { return std::clamp(platform::hardware_threads(), 1u, 8u); }

unsigned resolve_thread_count(std::optional<unsigned> cli, std::optional<unsigned> project) {
    if constexpr (!platform::has_threads) return 1;
    if (cli && *cli > 0) return *cli;
    if (const auto env = platform::env("CUERVO_JOBS"); env && !env->empty()) {
        if (const auto n = parse_thread_count(*env)) return *n;
        LOG_WARN("CUERVO_JOBS='{}' is not a positive thread count; ignored", *env);
    }
    if (project && *project > 0) return *project;
    return auto_thread_count();
}

} // namespace cuervo::jobs
