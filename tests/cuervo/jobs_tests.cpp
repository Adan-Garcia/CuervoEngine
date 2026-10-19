#include <doctest.h>

#include <atomic>
#include <cstdint>
#include <numeric>
#include <vector>

#include "cuervo/jobs/job_system.hpp"
#include "cuervo/platform/platform.hpp"

using namespace cuervo;

TEST_CASE("JobSystem runs every task exactly once, for any thread count") {
    for (unsigned threads : {1u, 2u, 3u, 8u}) {
        jobs::JobSystem js(threads);
        CHECK(js.thread_count() == threads);
        for (std::size_t tasks : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{1000}}) {
            std::vector<std::uint32_t> hits(tasks, 0);
            js.run(tasks, [&](std::size_t t) { hits[t] += 1; }); // disjoint writes
            for (std::uint32_t h : hits) CHECK(h == 1);
        }
    }
}

TEST_CASE("JobSystem::parallel_for covers the range with disjoint chunks") {
    jobs::JobSystem js(4);
    for (std::size_t count : {std::size_t{1}, std::size_t{100}, std::size_t{4097}, std::size_t{100000}}) {
        std::vector<std::uint8_t> seen(count, 0);
        std::atomic<std::size_t> calls{0};
        js.parallel_for(count, 256, [&](std::size_t begin, std::size_t end) {
            CHECK(begin < end);
            ++calls;
            for (std::size_t i = begin; i < end; ++i) seen[i] += 1;
        });
        for (std::uint8_t s : seen) REQUIRE(s == 1);
        CHECK(calls.load() == js.chunk_count(count, 256));
    }
}

TEST_CASE("JobSystem: many back-to-back batches, nested batches run inline") {
    jobs::JobSystem js(8);
    std::vector<std::uint64_t> sums(64, 0);
    for (int round = 0; round < 500; ++round) {
        js.run(sums.size(), [&](std::size_t t) {
            // A nested batch from inside a task must not deadlock.
            std::uint64_t local = 0;
            js.run(4, [&](std::size_t k) { local += k; });
            sums[t] += local + t;
        });
    }
    for (std::size_t t = 0; t < sums.size(); ++t) CHECK(sums[t] == 500u * (6u + t));
    CHECK(js.parallel_batches() > 0);
    js.set_thread_count(2);
    CHECK(js.thread_count() == 2);
    std::vector<int> v(10, 0);
    js.run(v.size(), [&](std::size_t t) { v[t] = static_cast<int>(t); });
    CHECK(std::accumulate(v.begin(), v.end(), 0) == 45);
}

TEST_CASE("Job thread count configuration") {
    CHECK(jobs::parse_thread_count("4") == 4u);
    CHECK_FALSE(jobs::parse_thread_count("0"));
    CHECK_FALSE(jobs::parse_thread_count("-2"));
    CHECK_FALSE(jobs::parse_thread_count("4x"));
    CHECK_FALSE(jobs::parse_thread_count(""));
    CHECK(jobs::auto_thread_count() >= 1u);
    CHECK(jobs::auto_thread_count() <= 8u);
    // --jobs wins over everything; project.toml over auto.
    CHECK(jobs::resolve_thread_count(3u, 5u) == 3u);
    if (!platform::env("CUERVO_JOBS")) {
        CHECK(jobs::resolve_thread_count(std::nullopt, 5u) == 5u);
        CHECK(jobs::resolve_thread_count(std::nullopt, std::nullopt) == jobs::auto_thread_count());
    }
}
