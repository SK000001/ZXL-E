// M8 step 1 — gating measurement.
// Builds the BWT (which internally constructs the suffix array) of an input
// blob on the GPU via libcubwt, times it, reports MB/s. BWT throughput is a
// fair proxy for SA throughput here — SA construction dominates the cost.
//
// Compare result against xz-9e match-finder throughput on the same input.
// Decision threshold: ≥5× xz-9e to greenlight M8 step 2.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <vector>
#include <string>
#include <cuda_runtime.h>
#include "../../third_party/libcubwt/libcubwt.cuh"

static bool read_file(const char *path, std::vector<uint8_t> &out, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t to_read = (cap && (size_t)sz > cap) ? cap : (size_t)sz;
    size_t off = out.size();
    out.resize(off + to_read);
    size_t got = fread(out.data() + off, 1, to_read, f);
    fclose(f);
    out.resize(off + got);
    return got > 0;
}

int main(int argc, char **argv) {
    size_t target_mb = 100;
    if (argc >= 2) target_mb = (size_t)atoll(argv[1]);
    size_t cap = target_mb * 1024 * 1024;

    // Concat silesia files until we hit `target_mb`.
    const char *files[] = {
        "tests/corpus/silesia/mozilla",
        "tests/corpus/silesia/webster",
        "tests/corpus/silesia/nci",
        "tests/corpus/silesia/samba",
        "tests/corpus/silesia/dickens",
    };
    std::vector<uint8_t> input;
    input.reserve(cap);
    for (size_t i = 0; i < sizeof(files)/sizeof(files[0]) && input.size() < cap; i++) {
        size_t remaining = cap - input.size();
        std::vector<uint8_t> tmp;
        if (!read_file(files[i], tmp, remaining)) {
            fprintf(stderr, "warn: could not read %s\n", files[i]);
            continue;
        }
        input.insert(input.end(), tmp.begin(), tmp.end());
        printf("loaded %s: %zu bytes (total %zu)\n", files[i], tmp.size(), input.size());
    }
    if (input.empty()) {
        fprintf(stderr, "no input loaded\n");
        return 1;
    }
    size_t n = input.size();
    printf("input: %zu bytes (%.2f MB)\n", n, n / (1024.0 * 1024.0));

    // Device probe.
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0);
    printf("gpu: %s sm_%d%d vram=%zu MiB\n", prop.name, prop.major, prop.minor,
           prop.totalGlobalMem >> 20);

    // Allocate libcubwt storage (~20.5× input).
    void *storage = nullptr;
    auto t_alloc0 = std::chrono::high_resolution_clock::now();
    int64_t rc = libcubwt_allocate_device_storage(&storage, (int64_t)n);
    auto t_alloc1 = std::chrono::high_resolution_clock::now();
    if (rc != LIBCUBWT_NO_ERROR) {
        fprintf(stderr, "libcubwt_allocate_device_storage failed: %lld\n", (long long)rc);
        return 2;
    }
    double alloc_ms = std::chrono::duration<double, std::milli>(t_alloc1 - t_alloc0).count();
    printf("alloc: %.1f ms (~%.2f GiB VRAM est.)\n", alloc_ms, (n * 20.5) / (1024.0*1024.0*1024.0));

    std::vector<uint8_t> out(n);

    // Warm-up run (untimed) to amortize JIT / first-launch overheads.
    rc = libcubwt_bwt(storage, input.data(), out.data(), (int64_t)n);
    if (rc < 0) {
        fprintf(stderr, "libcubwt_bwt warm failed: %lld\n", (long long)rc);
        return 3;
    }
    cudaDeviceSynchronize();

    // Timed runs.
    const int RUNS = 3;
    double best_ms = 1e18, sum_ms = 0;
    for (int r = 0; r < RUNS; r++) {
        cudaDeviceSynchronize();
        auto t0 = std::chrono::high_resolution_clock::now();
        rc = libcubwt_bwt(storage, input.data(), out.data(), (int64_t)n);
        cudaDeviceSynchronize();
        auto t1 = std::chrono::high_resolution_clock::now();
        if (rc < 0) {
            fprintf(stderr, "libcubwt_bwt run %d failed: %lld\n", r, (long long)rc);
            return 4;
        }
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        sum_ms += ms;
        if (ms < best_ms) best_ms = ms;
        printf("run %d: %.1f ms  %.2f MB/s\n", r, ms, (n / 1.0e6) / (ms / 1000.0));
    }
    double avg_ms = sum_ms / RUNS;
    double mbps_best = (n / 1.0e6) / (best_ms / 1000.0);
    double mbps_avg  = (n / 1.0e6) / (avg_ms  / 1000.0);
    printf("---\n");
    printf("best: %.1f ms  %.1f MB/s  (%.3f GB/s)\n", best_ms, mbps_best, mbps_best / 1000.0);
    printf("avg : %.1f ms  %.1f MB/s  (%.3f GB/s)\n", avg_ms,  mbps_avg,  mbps_avg  / 1000.0);

    libcubwt_free_device_storage(storage);
    return 0;
}
