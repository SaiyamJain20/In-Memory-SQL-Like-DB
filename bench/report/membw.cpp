// membw: how fast can this machine read memory? A STREAM-style read (sum of a 1 GiB array of
// doubles, 4 independent accumulators so the loop is not latency bound) and copy, best of 7, for
// 1, 2, 4, 8 and 16 threads. The numbers turn the scan-bound micro-benchmarks into a fraction of
// what the hardware allows (docs/REPORT.md, "Operators").
//   g++ -O3 -pthread -o build/membw bench/report/membw.cpp && build/membw
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

static double Seconds(std::chrono::steady_clock::duration d) {
    return std::chrono::duration<double>(d).count();
}

static double ReadSum(const double* a, size_t n) {
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    for (size_t i = 0; i + 4 <= n; i += 4) {
        s0 += a[i];
        s1 += a[i + 1];
        s2 += a[i + 2];
        s3 += a[i + 3];
    }
    return s0 + s1 + s2 + s3;
}

int main() {
    const size_t n = (1ull << 30) / sizeof(double); // 1 GiB per array
    std::vector<double> a(n, 1.0), b(n, 2.0);
    volatile double sink = 0;
    std::printf("threads  read GB/s  copy GB/s\n");
    for (const unsigned t : {1u, 2u, 4u, 8u, 16u}) {
        double best_read = 0, best_copy = 0;
        for (int rep = 0; rep < 7; rep++) {
            std::vector<std::thread> pool;
            std::vector<double> part(t, 0);
            auto start = std::chrono::steady_clock::now();
            for (unsigned k = 0; k < t; k++) {
                pool.emplace_back([&, k] {
                    const size_t lo = n / t * k, hi = k + 1 == t ? n : n / t * (k + 1);
                    part[k] = ReadSum(a.data() + lo, hi - lo);
                });
            }
            for (auto& th : pool)
                th.join();
            double s = Seconds(std::chrono::steady_clock::now() - start);
            for (double p : part)
                sink = sink + p;
            best_read = std::max(best_read, static_cast<double>(n * sizeof(double)) / s / 1e9);
            pool.clear();
            start = std::chrono::steady_clock::now();
            for (unsigned k = 0; k < t; k++) {
                pool.emplace_back([&, k] {
                    const size_t lo = n / t * k, hi = k + 1 == t ? n : n / t * (k + 1);
                    for (size_t i = lo; i < hi; i++)
                        b[i] = a[i];
                });
            }
            for (auto& th : pool)
                th.join();
            s = Seconds(std::chrono::steady_clock::now() - start);
            best_copy = std::max(best_copy, static_cast<double>(2 * n * sizeof(double)) / s / 1e9);
        }
        std::printf("%7u  %9.1f  %9.1f\n", t, best_read, best_copy);
    }
    return static_cast<int>(sink) == 12345;
}
