// apex_pipeline: builds the order book of EVERY stock in a full-day ITCH file with
// different thread layouts and compares speed. Results must be identical.
//
// Usage: apex_pipeline <file.NASDAQ_ITCH50.gz> [rounds=1]

#include <cstdio>
#include <cstdlib>
#include <string>

#include "apex/pipeline.hpp"

using namespace apex;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <itch_file.gz> [rounds]\n", argv[0]);
        return 2;
    }
    const int rounds = argc > 2 ? std::atoi(argv[2]) : 1;

    struct Layout {
        const char* name;
        PipelineConfig config;
    };
    const Layout layouts[] = {
        {"1 thread  [gunzip+parse+books]", {false, 0}},
        {"2 threads [gunzip] > [parse+books]", {true, 0}},
        {"2 threads [gunzip+parse] > [books]", {false, 1}},
        {"3 threads [gunzip] > [parse] > [books]", {true, 1}},
        {"3 threads [gunzip+parse] > [books x2]", {false, 2}},
        {"4 threads [gunzip] > [parse] > [books x2]", {true, 2}},
        {"5 threads [gunzip] > [parse] > [books x3]", {true, 3}},
    };

    std::printf("%-44s %8s %10s %8s  %9s %8s  %s\n", "layout", "seconds", "M msg/s", "speedup", "rejected",
                "books", "queue handoff p50 / p99 / p99.9 (sampled 1/1024)");
    double baseline = 0;
    std::uint64_t reference_checksum = 0;
    for (int r = 0; r < rounds; ++r) {
        for (const Layout& l : layouts) {
            const PipelineResult res = run_pipeline(argv[1], l.config);
            if (!res.ok) {
                std::fprintf(stderr, "failed to read %s\n", argv[1]);
                return 1;
            }
            if (baseline == 0) {
                baseline = res.seconds;
                reference_checksum = res.checksum;
            }
            std::printf("%-44s %8.2f %10.1f %7.2fx  %9llu %8llu", l.name, res.seconds, res.messages / res.seconds / 1e6,
                        baseline / res.seconds, (unsigned long long)res.rejected, (unsigned long long)res.books);
            if (res.handoff_ns.count() > 0) {
                std::printf("  %llu / %llu / %llu ns", (unsigned long long)res.handoff_ns.percentile(50),
                            (unsigned long long)res.handoff_ns.percentile(99),
                            (unsigned long long)res.handoff_ns.percentile(99.9));
            }
            std::printf("%s\n", res.checksum == reference_checksum ? "" : "   CHECKSUM DIFFERS");
            std::printf("%-44s idle:", "");
            for (const auto& t : res.idle) std::printf("  %s %.1fs", t.name.c_str(), t.idle_seconds);
            std::printf("\n");
            std::fflush(stdout);
        }
    }
    return 0;
}
