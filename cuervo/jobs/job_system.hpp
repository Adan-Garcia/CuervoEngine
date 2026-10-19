#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

// The job system (DESIGN.md 4.14, 4.15): a fixed pool of worker threads that
// run fork-join batches for the simulation and render preparation.
//
// Determinism contract. A batch is `task_count` independent tasks; which
// thread runs which task, and in what order, is not defined. Callers keep
// results deterministic by construction:
//   - each task writes only its own disjoint range (its particles, its slice
//     of an output array, its own scratch vector);
//   - anything combined across tasks is merged afterwards by the caller, in
//     task-index order, on the calling thread;
//   - tasks that need random numbers use their own child stream
//     (derive_stream, keyed by the task index), never a shared one.
// Results then depend only on the task split, and every split used by the
// engine is chosen so the merged result is the same for any split (and so for
// any thread count): see ranges() below.
//
// Threads. `thread_count` counts the calling thread: 1 runs everything inline
// on the caller (no worker threads exist), N starts N - 1 workers that join
// the caller for each batch. Builds without threads (the web,
// platform::has_threads) are always 1. Batches are started from one thread at
// a time; a parallel_for called from inside a task runs inline.
namespace cuervo::jobs {

class JobSystem {
public:
    explicit JobSystem(unsigned thread_count = 1);
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    unsigned thread_count() const { return thread_count_; }
    // Restarts the pool with `thread_count` threads (clamped to 1 without
    // thread support). Not while a batch runs.
    void set_thread_count(unsigned thread_count);

    // Runs fn(task) for every task in [0, task_count) and returns when all are
    // done. With one thread (or one task, or from inside a task) the tasks run
    // inline, in index order.
    template<typename Fn>
    void run(std::size_t task_count, Fn&& fn) {
        using F = std::remove_reference_t<Fn>;
        run_erased(task_count, const_cast<void*>(static_cast<const void*>(&fn)),
                   [](void* f, std::size_t task) { (*static_cast<F*>(f))(task); });
    }

    // Splits [0, count) into contiguous ranges of at least `min_grain`
    // elements (except the last) and runs fn(begin, end) for each. The split
    // depends only on (count, min_grain, thread_count); per-element work
    // gives identical results for any split.
    template<typename Fn>
    void parallel_for(std::size_t count, std::size_t min_grain, Fn&& fn) {
        if (count == 0) return;
        const std::size_t chunks = chunk_count(count, min_grain);
        const std::size_t step = (count + chunks - 1) / chunks;
        run(chunks, [&](std::size_t c) {
            const std::size_t begin = c * step;
            const std::size_t end = std::min(count, begin + step);
            if (begin < end) fn(begin, end);
        });
    }

    // Number of ranges parallel_for would use: about 4 per thread (so uneven
    // ranges still balance), never smaller than `min_grain`.
    std::size_t chunk_count(std::size_t count, std::size_t min_grain) const;

    // Batches run so far that actually used worker threads (for tests/stats).
    std::uint64_t parallel_batches() const { return parallel_batches_; }

private:
    using TaskFn = void (*)(void*, std::size_t);
    struct Batch {
        void* fn_object = nullptr;
        TaskFn fn = nullptr;
        std::size_t task_count = 0;
        std::atomic<std::size_t> next{0};
        std::atomic<std::size_t> done{0};
        std::atomic<unsigned> active{0}; // workers currently holding this batch
    };

    void run_erased(std::size_t task_count, void* fn_object, TaskFn fn);
    void start_workers();
    void stop_workers();
    void worker_main();
    static void drain(Batch& batch);

    unsigned thread_count_ = 1;
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::atomic<std::uint64_t> epoch_{0}; // bumped (under mutex_) for every published batch
    Batch* batch_ = nullptr;              // guarded by mutex_
    bool stop_ = false;                   // guarded by mutex_
    std::atomic<bool> stopping_{false};   // lock-free mirror of stop_ for spinning workers
    std::uint64_t parallel_batches_ = 0;
};

// The engine's shared pool, used by ParticleSystem and the renderer helpers
// unless they are given another one. Starts with one thread (inline) until
// configure() is called (runtime: --jobs / CUERVO_JOBS / project.toml).
JobSystem& default_jobs();
void configure(unsigned thread_count);

// The thread count to use: the first of `cli` (--jobs), the CUERVO_JOBS
// environment variable and `project` (project.toml `jobs`) that is set and
// > 0, else auto_thread_count(). Always 1 without thread support.
unsigned resolve_thread_count(std::optional<unsigned> cli, std::optional<unsigned> project);
// Hardware threads, capped at 8 (beyond that the per-tick batches are too
// small to gain), at least 1.
unsigned auto_thread_count();
// Parses a positive thread count ("4"); nullopt for anything else.
std::optional<unsigned> parse_thread_count(std::string_view text);

} // namespace cuervo::jobs
