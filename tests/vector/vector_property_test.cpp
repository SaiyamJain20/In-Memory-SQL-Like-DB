// Randomised model-based test for Vector. A plain std::vector<Value> is the reference model;
// random sequences of structural operations (Slice, Flatten, Reference, Copy, Reset+refill) are
// applied to the real vector and to the model, and every row is compared after every step.
// Vectors that were Reference()d before a Reset must keep their old contents (shared-buffer
// safety). Run under ASan/UBSan to catch lifetime errors in the string heap.

#include "vector/vector.h"

#include "test_util.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace cdb {

namespace {

using test::BitIdentical;

struct Snapshot {
    std::unique_ptr<Vector> vec;
    std::vector<Value> model;
};

void ExpectMatches(const Vector& v, const std::vector<Value>& model, const std::string& ctx) {
    for (idx_t i = 0; i < model.size(); i++) {
        const Value got = v.GetValue(i);
        ASSERT_TRUE(BitIdentical(got, model[i]))
            << ctx << ": row " << i << " got " << got.ToString() << " expected "
            << model[i].ToString();
    }
}

void Fill(Vector& v, std::vector<Value>& model, test::Rng& rng, idx_t n, double nulls) {
    model.clear();
    for (idx_t i = 0; i < n; i++) {
        model.push_back(test::RandomValue(rng, v.type(), nulls));
        v.SetValue(i, model.back());
    }
}

} // namespace

TEST(VectorProperty, RandomOperationSequencesMatchTheReferenceModel) {
    const idx_t kCapacities[] = {1, 2, 63, 64, 65, 100, 513, kVectorSize};
    const double kNullRates[] = {0.0, 0.2, 0.9, 1.0};
    size_t scenarios = 0;

    for (uint64_t seed = 0; seed < 60; seed++) {
        for (LogicalType type : test::AllTypes()) {
            test::Rng rng(seed * 1000 + static_cast<uint64_t>(type.id()));
            const idx_t cap = kCapacities[test::RandBelow(rng, std::size(kCapacities))];
            const double nulls = kNullRates[test::RandBelow(rng, std::size(kNullRates))];
            const std::string ctx = "seed=" + std::to_string(seed) + " type=" + type.ToString() +
                                    " cap=" + std::to_string(cap);

            Vector v(type, cap);
            std::vector<Value> model;
            idx_t n = test::RandBelow(rng, cap + 1);
            Fill(v, model, rng, n, nulls);
            std::vector<Snapshot> snapshots;

            for (int step = 0; step < 12; step++) {
                switch (test::RandBelow(rng, 6)) {
                case 0: { // Slice with a random selection (reorders, repeats, may be empty)
                    if (n == 0)
                        break;
                    const idx_t m = test::RandBelow(rng, cap + 1);
                    SelectionVector sel(m);
                    std::vector<Value> next;
                    for (idx_t i = 0; i < m; i++) {
                        const auto src = static_cast<sel_t>(test::RandBelow(rng, n));
                        sel.Set(i, src);
                        next.push_back(model[src]);
                    }
                    v.Slice(sel, m);
                    model = std::move(next);
                    n = m;
                    break;
                }
                case 1: // Flatten (no-op for flat vectors)
                    v.Flatten(n);
                    ASSERT_EQ(v.format(), VectorFormat::Flat) << ctx;
                    break;
                case 2: { // keep a Reference whose contents must survive later mutation
                    auto ref = std::make_unique<Vector>(type, cap);
                    ref->Reference(v);
                    snapshots.push_back({std::move(ref), model});
                    break;
                }
                case 3: { // Copy into a pre-populated vector at a random offset
                    const bool use_sel = n > 0 && test::Chance(rng, 0.5);
                    const idx_t m = use_sel ? test::RandBelow(rng, cap + 1) : n;
                    const idx_t offset = test::RandBelow(rng, cap - m + 1);
                    Vector out(type, cap);
                    std::vector<Value> expect;
                    Fill(out, expect, rng, cap, 0.5); // occupied rows, with plenty of NULLs
                    if (use_sel) {
                        SelectionVector sel(m);
                        for (idx_t i = 0; i < m; i++) {
                            const auto src = static_cast<sel_t>(test::RandBelow(rng, n));
                            sel.Set(i, src);
                            expect[offset + i] = model[src];
                        }
                        VectorOps::Copy(v, out, &sel, m, offset);
                    } else {
                        for (idx_t i = 0; i < n; i++)
                            expect[offset + i] = model[i];
                        VectorOps::Copy(v, out, nullptr, n, offset);
                    }
                    ExpectMatches(out, expect, ctx + " copy");
                    out.Verify(cap);
                    break;
                }
                case 4: { // Reset + refill: previously taken references must be unaffected
                    v.Reset();
                    n = test::RandBelow(rng, cap + 1);
                    Fill(v, model, rng, n, nulls);
                    break;
                }
                default: { // SetConstant, then back to a normal vector
                    const Value c = test::RandomValue(rng, type, 0.3);
                    v.SetConstant(c);
                    model.assign(n, c);
                    break;
                }
                }
                v.Verify(n);
                ExpectMatches(v, model, ctx + " step " + std::to_string(step));
                for (const Snapshot& s : snapshots) {
                    ExpectMatches(*s.vec, s.model, ctx + " snapshot");
                }
            }
            scenarios++;
        }
    }
    EXPECT_EQ(scenarios, 360u);
}

} // namespace cdb
