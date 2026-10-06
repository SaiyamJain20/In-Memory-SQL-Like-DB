#include "storage/encoding.h"

#include "kernels/decode.h"
#include "storage/binary_io.h"
#include "storage/bitpacking.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>
#include <unordered_map>

namespace cdb {

namespace {

std::atomic<int>& CompressionState() {
    static std::atomic<int> state{std::getenv("CDB_NO_COMPRESSION") != nullptr ? 0 : 1};
    return state;
}

// Auto accepts an encoding only if it is at most this fraction of the raw size: decoding costs CPU,
// and a raw segment scans with no copying at all, so a marginal saving is not worth it.
constexpr double kMaxEncodedFraction = 0.7;

constexpr int kMaxExponent = 6;
constexpr double kPow10[kMaxExponent + 1] = {1, 10, 100, 1e3, 1e4, 1e5, 1e6};

Value IntValue(LogicalType type, int64_t v) {
    switch (type.id()) {
    case TypeId::Boolean:
        return Value::Boolean(v != 0);
    case TypeId::Integer:
        return Value::Integer(static_cast<int32_t>(v));
    case TypeId::Date:
        return Value::Date(date_t{static_cast<int32_t>(v)});
    case TypeId::BigInt:
        return Value::BigInt(v);
    default:
        CDB_UNREACHABLE("IntValue");
    }
}

bool IsIntLike(LogicalType type) {
    const PhysicalType p = type.physical();
    return p == PhysicalType::Bool || p == PhysicalType::Int32 || p == PhysicalType::Int64;
}

// Every value of an integer-like column as int64. NULL rows repeat the previous valid value (the
// first valid one, before any), so they never widen a range, break a run or a monotone sequence.
std::vector<int64_t> LoadInts(const ColumnSegment& raw) {
    const idx_t n = raw.count();
    std::vector<int64_t> values(n);
    const uint8_t* data = raw.raw_data();
    const ValidityMask& validity = raw.validity();
    const auto load = [&](idx_t i) -> int64_t {
        switch (raw.type().physical()) {
        case PhysicalType::Bool:
            return reinterpret_cast<const bool*>(data)[i] ? 1 : 0;
        case PhysicalType::Int32:
            return reinterpret_cast<const int32_t*>(data)[i];
        default:
            return reinterpret_cast<const int64_t*>(data)[i];
        }
    };
    int64_t carry = 0;
    for (idx_t i = 0; i < n; i++) { // find the first valid value to start the carry with
        if (validity.IsValid(i)) {
            carry = load(i);
            break;
        }
    }
    for (idx_t i = 0; i < n; i++) {
        if (validity.IsValid(i)) {
            carry = load(i);
        }
        values[i] = carry;
    }
    return values;
}

size_t RawDataBytes(const ColumnSegment& raw) {
    size_t bytes = AlignUp(raw.count(), kVectorSize) * raw.type().width();
    if (raw.heap() != nullptr) {
        bytes += raw.heap()->BytesUsed();
    }
    return bytes;
}

// The number of 2048-row vectors of a segment of `count` rows, and the rows in vector `v`.
idx_t VectorsOf(idx_t count) {
    return (count + kVectorSize - 1) / kVectorSize;
}
idx_t RowsIn(idx_t count, idx_t v) {
    return std::min<idx_t>(kVectorSize, count - v * kVectorSize);
}

// Checks that the packed data of vector `v` (`rows` values of `width` bits) lies inside a payload
// of `payload_size` bytes starting at `offset`, the way BitUnpack will read it.
void CheckPacked(const BinaryReader& r, uint32_t offset, uint8_t width, idx_t rows,
                 size_t payload_size) {
    if (width > 64) {
        r.Fail("bit width " + std::to_string(width) + " is out of range");
    }
    if (width > 0 && (offset > payload_size || PackedBytes(rows, width) > payload_size - offset)) {
        r.Fail("a packed vector at offset " + std::to_string(offset) + " does not fit the payload");
    }
}

// Packs a vector's values (already offset to start at 0) and returns where they went.
uint32_t AppendPacked(std::vector<uint8_t>& payload, const uint64_t* values, idx_t n,
                      uint8_t width) {
    const size_t at = payload.size();
    payload.resize(at + PackedBytes(n, width), 0);
    BitPack(values, n, width, payload.data() + at);
    return static_cast<uint32_t>(at);
}

// ---------------------------------------------------------------------------------- bit-packed
// ints

class BitpackedInts final : public EncodedColumn {
  public:
    struct Meta {
        int64_t base;
        uint32_t offset;
        uint8_t width;
        uint8_t delta; // 1: value[i] = value[i-1] + packed[i]; 0: value = base + packed
    };

    BitpackedInts(LogicalType type, std::vector<Meta> meta, std::vector<uint8_t> payload)
        : type_(type), meta_(std::move(meta)), payload_(std::move(payload)) {
        constant_ = std::all_of(meta_.begin(), meta_.end(),
                                [](const Meta& m) { return m.width == 0 && !m.delta; });
    }

    EncodingKind kind() const noexcept override {
        return constant_ ? EncodingKind::Constant : EncodingKind::Bitpacked;
    }
    void Serialize(BinaryWriter& w) const override {
        w.U32(static_cast<uint32_t>(meta_.size()));
        for (const Meta& m : meta_) {
            w.I64(m.base);
            w.U32(m.offset);
            w.U8(m.width);
            w.U8(m.delta);
        }
        w.U32(static_cast<uint32_t>(payload_.size()));
        w.Bytes(payload_.data(), payload_.size());
    }
    static std::shared_ptr<BitpackedInts> Read(BinaryReader& r, LogicalType type, idx_t count) {
        if (!IsIntLike(type)) {
            r.Fail("bit-packed integers for a " + type.ToString() + " column");
        }
        const uint32_t n = r.Count(14);
        if (n != VectorsOf(count)) {
            r.Fail("bit-packed metadata for " + std::to_string(n) + " vectors, the column has " +
                   std::to_string(VectorsOf(count)));
        }
        std::vector<Meta> meta(n);
        for (Meta& m : meta) {
            m.base = r.I64();
            m.offset = r.U32();
            m.width = r.U8();
            m.delta = r.U8();
        }
        const uint32_t payload_size = r.U32();
        const uint8_t* payload = r.Bytes(payload_size);
        for (idx_t v = 0; v < n; v++) {
            Meta& m = meta[v];
            if (m.delta > 1) {
                r.Fail("a delta flag of " + std::to_string(m.delta));
            }
            CheckPacked(r, m.offset, m.width, RowsIn(count, v), payload_size);
            if (m.width == 0) {
                m.offset = 0; // nothing is read: keep a stray offset out of pointer arithmetic
            }
        }
        return std::make_shared<BitpackedInts>(
            type, std::move(meta), std::vector<uint8_t>(payload, payload + payload_size));
    }
    size_t MemoryUsage() const noexcept override {
        return payload_.size() + meta_.size() * sizeof(Meta);
    }

    bool DecodeVector(idx_t v, idx_t n, const ValidityMask& validity, Vector& out) const override {
        const Meta& m = meta_[v];
        if (m.width == 0 && !m.delta && validity.CountValid(n) == n) {
            out.SetConstant(IntValue(type_, m.base));
            return true;
        }
        switch (type_.physical()) {
        case PhysicalType::Bool:
            Decode<bool>(m, n, out);
            break;
        case PhysicalType::Int32:
            Decode<int32_t>(m, n, out);
            break;
        default:
            Decode<int64_t>(m, n, out);
            break;
        }
        return false;
    }

  private:
    template <class T> void Decode(const Meta& m, idx_t n, Vector& out) const {
        T* o = out.FlatDataForOverwrite<T>();
        uint64_t tmp[kVectorSize];
        BitUnpack(payload_.data() + m.offset, n, m.width, tmp);
        const auto base = static_cast<uint64_t>(m.base);
        if (m.delta) {
            uint64_t cur = base;
            for (idx_t i = 0; i < n; i++) {
                cur += tmp[i]; // tmp[0] is stored as 0
                o[i] = static_cast<T>(static_cast<int64_t>(cur));
            }
        } else if constexpr (std::is_same_v<T, int32_t>) {
            kernels::OffsetsToInt32(tmp, n, base, o);
        } else if constexpr (std::is_same_v<T, int64_t>) {
            kernels::OffsetsToInt64(tmp, n, base, o);
        } else {
            for (idx_t i = 0; i < n; i++) {
                o[i] = static_cast<T>(static_cast<int64_t>(base + tmp[i]));
            }
        }
    }

    LogicalType type_;
    std::vector<Meta> meta_;
    std::vector<uint8_t> payload_;
    bool constant_ = false;
};

std::shared_ptr<EncodedColumn> EncodeBitpacked(const ColumnSegment& raw) {
    const std::vector<int64_t> values = LoadInts(raw);
    std::vector<BitpackedInts::Meta> meta;
    std::vector<uint8_t> payload;
    std::vector<uint64_t> packed(kVectorSize);
    for (idx_t start = 0; start < values.size(); start += kVectorSize) {
        const idx_t n = std::min<idx_t>(kVectorSize, values.size() - start);
        const int64_t* x = values.data() + start;
        int64_t lo = x[0], hi = x[0];
        bool monotone = true;
        uint64_t max_delta = 0;
        for (idx_t i = 1; i < n; i++) {
            lo = std::min(lo, x[i]);
            hi = std::max(hi, x[i]);
            if (x[i] < x[i - 1]) {
                monotone = false;
            } else {
                max_delta = std::max(max_delta,
                                     static_cast<uint64_t>(x[i]) - static_cast<uint64_t>(x[i - 1]));
            }
        }
        const uint8_t for_width = BitWidth(static_cast<uint64_t>(hi) - static_cast<uint64_t>(lo));
        const uint8_t delta_width = BitWidth(max_delta);
        BitpackedInts::Meta m{};
        if (monotone && n > 1 && delta_width + 2 <= for_width) {
            m.base = x[0];
            m.width = delta_width;
            m.delta = 1;
            packed[0] = 0;
            for (idx_t i = 1; i < n; i++) {
                packed[i] = static_cast<uint64_t>(x[i]) - static_cast<uint64_t>(x[i - 1]);
            }
        } else {
            m.base = lo;
            m.width = for_width;
            m.delta = 0;
            for (idx_t i = 0; i < n; i++) {
                packed[i] = static_cast<uint64_t>(x[i]) - static_cast<uint64_t>(lo);
            }
        }
        m.offset = m.width == 0 ? 0 : AppendPacked(payload, packed.data(), n, m.width);
        meta.push_back(m);
    }
    return std::make_shared<BitpackedInts>(raw.type(), std::move(meta), std::move(payload));
}

// ---------------------------------------------------------------------------------- run-length
// ints

class RleInts final : public EncodedColumn {
  public:
    RleInts(LogicalType type, std::vector<int64_t> values, std::vector<uint32_t> ends,
            std::vector<uint32_t> first_run)
        : type_(type), values_(std::move(values)), ends_(std::move(ends)),
          first_run_(std::move(first_run)) {}

    EncodingKind kind() const noexcept override { return EncodingKind::Rle; }
    void Serialize(BinaryWriter& w) const override {
        w.U32(static_cast<uint32_t>(values_.size()));
        for (const int64_t v : values_) {
            w.I64(v);
        }
        for (const uint32_t e : ends_) {
            w.U32(e);
        }
        w.U32(static_cast<uint32_t>(first_run_.size()));
        for (const uint32_t f : first_run_) {
            w.U32(f);
        }
    }
    static std::shared_ptr<RleInts> Read(BinaryReader& r, LogicalType type, idx_t count) {
        if (!IsIntLike(type)) {
            r.Fail("run-length integers for a " + type.ToString() + " column");
        }
        const uint32_t runs = r.Count(12);
        if (runs == 0 || runs > count) {
            r.Fail(std::to_string(runs) + " runs for " + std::to_string(count) + " rows");
        }
        std::vector<int64_t> values(runs);
        for (int64_t& v : values) {
            v = r.I64();
        }
        std::vector<uint32_t> ends(runs);
        uint64_t previous = 0;
        for (uint32_t& e : ends) {
            e = r.U32();
            if (e <= previous || e > count) {
                r.Fail("run ends are not increasing within the column");
            }
            previous = e;
        }
        if (previous != count) {
            r.Fail("the runs cover " + std::to_string(previous) + " of " + std::to_string(count) +
                   " rows");
        }
        const uint32_t n = r.Count(4);
        if (n != VectorsOf(count)) {
            r.Fail("run starts for " + std::to_string(n) + " vectors, the column has " +
                   std::to_string(VectorsOf(count)));
        }
        std::vector<uint32_t> first_run(n);
        uint32_t run = 0;
        for (idx_t v = 0; v < n; v++) {
            first_run[v] = r.U32();
            while (ends[run] <= v * kVectorSize) { // the run holding the vector's first row
                run++;
            }
            if (first_run[v] != run) {
                r.Fail("vector " + std::to_string(v) + " claims to start in run " +
                       std::to_string(first_run[v]) + ", it is run " + std::to_string(run));
            }
        }
        return std::make_shared<RleInts>(type, std::move(values), std::move(ends),
                                         std::move(first_run));
    }
    size_t MemoryUsage() const noexcept override {
        return values_.size() * sizeof(int64_t) + ends_.size() * sizeof(uint32_t) +
               first_run_.size() * sizeof(uint32_t);
    }

    bool DecodeVector(idx_t v, idx_t n, const ValidityMask& validity, Vector& out) const override {
        const idx_t start = v * kVectorSize, end = start + n;
        uint32_t run = first_run_[v];
        if (ends_[run] >= end && validity.CountValid(n) == n) { // one run covers the vector
            out.SetConstant(IntValue(type_, values_[run]));
            return true;
        }
        switch (type_.physical()) {
        case PhysicalType::Bool:
            Fill<bool>(start, end, run, out);
            break;
        case PhysicalType::Int32:
            Fill<int32_t>(start, end, run, out);
            break;
        default:
            Fill<int64_t>(start, end, run, out);
            break;
        }
        return false;
    }

  private:
    template <class T> void Fill(idx_t start, idx_t end, uint32_t run, Vector& out) const {
        T* o = out.FlatDataForOverwrite<T>();
        idx_t pos = start;
        while (pos < end) {
            const idx_t stop = std::min<idx_t>(ends_[run], end);
            const T value = static_cast<T>(values_[run]);
            for (idx_t i = pos; i < stop; i++) {
                o[i - start] = value;
            }
            pos = stop;
            run++;
        }
    }

    LogicalType type_;
    std::vector<int64_t> values_;     // value of each run
    std::vector<uint32_t> ends_;      // row after the run's last row
    std::vector<uint32_t> first_run_; // per vector: the run holding its first row
};

std::shared_ptr<EncodedColumn> EncodeRle(const ColumnSegment& raw, bool force) {
    const std::vector<int64_t> values = LoadInts(raw);
    const idx_t n = values.size();
    idx_t runs = 1;
    for (idx_t i = 1; i < n; i++) {
        runs += values[i] != values[i - 1];
    }
    if (!force && runs > n / 8) {
        return nullptr; // average run shorter than 8: not worth building
    }
    std::vector<int64_t> run_values;
    std::vector<uint32_t> ends;
    std::vector<uint32_t> first_run;
    for (idx_t i = 0; i < n; i++) {
        if (i == 0 || values[i] != values[i - 1]) {
            if (i > 0) {
                ends.push_back(static_cast<uint32_t>(i));
            }
            run_values.push_back(values[i]);
        }
        if (i % kVectorSize == 0) {
            first_run.push_back(
                static_cast<uint32_t>(run_values.size() - 1)); // the run holding row i
        }
    }
    ends.push_back(static_cast<uint32_t>(n));
    return std::make_shared<RleInts>(raw.type(), std::move(run_values), std::move(ends),
                                     std::move(first_run));
}

// ---------------------------------------------------------------------------------- scaled doubles

class ScaledDoubles final : public EncodedColumn {
  public:
    static constexpr uint8_t kRaw = 0xFE;
    static constexpr uint8_t kConst = 0xFF;
    struct Meta {
        int64_t base; // kConst: the double's bit pattern
        uint32_t offset;
        uint8_t width;
        uint8_t exponent; // 0..kMaxExponent, kRaw or kConst
    };

    ScaledDoubles(std::vector<Meta> meta, std::vector<uint8_t> payload)
        : meta_(std::move(meta)), payload_(std::move(payload)) {}

    EncodingKind kind() const noexcept override { return EncodingKind::ScaledDouble; }
    void Serialize(BinaryWriter& w) const override {
        w.U32(static_cast<uint32_t>(meta_.size()));
        for (const Meta& m : meta_) {
            w.I64(m.base);
            w.U32(m.offset);
            w.U8(m.width);
            w.U8(m.exponent);
        }
        w.U32(static_cast<uint32_t>(payload_.size()));
        w.Bytes(payload_.data(), payload_.size());
    }
    static std::shared_ptr<ScaledDoubles> Read(BinaryReader& r, LogicalType type, idx_t count) {
        if (type.physical() != PhysicalType::Double) {
            r.Fail("scaled doubles for a " + type.ToString() + " column");
        }
        const uint32_t n = r.Count(14);
        if (n != VectorsOf(count)) {
            r.Fail("scaled-double metadata for " + std::to_string(n) + " vectors, the column has " +
                   std::to_string(VectorsOf(count)));
        }
        std::vector<Meta> meta(n);
        for (Meta& m : meta) {
            m.base = r.I64();
            m.offset = r.U32();
            m.width = r.U8();
            m.exponent = r.U8();
        }
        const uint32_t payload_size = r.U32();
        const uint8_t* payload = r.Bytes(payload_size);
        for (idx_t v = 0; v < n; v++) {
            Meta& m = meta[v];
            const idx_t rows = RowsIn(count, v);
            if (m.exponent == kConst) {
                m.offset = 0;
                m.width = 0;
            } else if (m.exponent == kRaw) {
                if (m.offset > payload_size || rows * sizeof(double) > payload_size - m.offset) {
                    r.Fail("raw doubles at offset " + std::to_string(m.offset) +
                           " do not fit the payload");
                }
            } else if (m.exponent <= kMaxExponent) {
                CheckPacked(r, m.offset, m.width, rows, payload_size);
                if (m.width == 0) {
                    m.offset = 0;
                }
            } else {
                r.Fail("a decimal exponent of " + std::to_string(m.exponent));
            }
        }
        return std::make_shared<ScaledDoubles>(
            std::move(meta), std::vector<uint8_t>(payload, payload + payload_size));
    }
    bool all_const() const noexcept {
        return std::all_of(meta_.begin(), meta_.end(),
                           [](const Meta& m) { return m.exponent == kConst; });
    }
    size_t MemoryUsage() const noexcept override {
        return payload_.size() + meta_.size() * sizeof(Meta);
    }

    bool DecodeVector(idx_t v, idx_t n, const ValidityMask& validity, Vector& out) const override {
        const Meta& m = meta_[v];
        if (m.exponent == kConst) {
            double d;
            std::memcpy(&d, &m.base, sizeof(d));
            if (validity.CountValid(n) == n) {
                out.SetConstant(Value::Double(d));
                return true;
            }
            std::fill_n(out.FlatDataForOverwrite<double>(), n, d);
            return false;
        }
        double* o = out.FlatDataForOverwrite<double>();
        if (m.exponent == kRaw) {
            std::memcpy(o, payload_.data() + m.offset, n * sizeof(double));
            return false;
        }
        uint64_t tmp[kVectorSize];
        BitUnpack(payload_.data() + m.offset, n, m.width, tmp);
        const double scale = kPow10[m.exponent];
        if (m.width <= 52) {
            kernels::OffsetsToScaledDouble(tmp, n, m.base, scale, o);
        } else { // offsets this wide cannot go through the exact-conversion trick
            const auto base = static_cast<uint64_t>(m.base);
            for (idx_t i = 0; i < n; i++) {
                o[i] = static_cast<double>(static_cast<int64_t>(base + tmp[i])) / scale;
            }
        }
        return false;
    }

  private:
    std::vector<Meta> meta_;
    std::vector<uint8_t> payload_;
};

// Tries v == n / 10^e for every valid value: the encoding is lossless only if the division gives
// back exactly the original bits, so that is what is checked (not a tolerance).
bool TryScale(const double* v, const ValidityMask& validity, idx_t start, idx_t n, int e,
              std::vector<int64_t>& ints) {
    const double scale = kPow10[e];
    for (idx_t i = 0; i < n; i++) {
        if (!validity.IsValid(start + i)) {
            ints[i] = INT64_MIN; // filled in afterwards
            continue;
        }
        const double scaled = v[i] * scale;
        if (!(std::fabs(scaled) < 9.0e15)) { // also rejects NaN and infinities
            return false;
        }
        const auto rounded = static_cast<int64_t>(std::llround(scaled));
        const double back = static_cast<double>(rounded) / scale;
        if (std::memcmp(&back, &v[i], sizeof(double)) != 0) { // -0.0 fails here too
            return false;
        }
        ints[i] = rounded;
    }
    return true;
}

std::shared_ptr<EncodedColumn> EncodeScaledDoubles(const ColumnSegment& raw) {
    const auto* data = reinterpret_cast<const double*>(raw.raw_data());
    const ValidityMask& validity = raw.validity();
    std::vector<ScaledDoubles::Meta> meta;
    std::vector<uint8_t> payload;
    std::vector<int64_t> ints(kVectorSize);
    std::vector<uint64_t> packed(kVectorSize);
    for (idx_t start = 0; start < raw.count(); start += kVectorSize) {
        const idx_t n = std::min<idx_t>(kVectorSize, raw.count() - start);
        const double* v = data + start;
        ScaledDoubles::Meta m{};
        // all valid values bit-identical (or none valid)?
        idx_t first = n;
        bool same = true;
        for (idx_t i = 0; i < n; i++) {
            if (!validity.IsValid(start + i)) {
                continue;
            }
            if (first == n) {
                first = i;
            } else if (std::memcmp(&v[i], &v[first], sizeof(double)) != 0) {
                same = false;
                break;
            }
        }
        if (same) {
            const double d = first == n ? 0.0 : v[first];
            std::memcpy(&m.base, &d, sizeof(d));
            m.exponent = ScaledDoubles::kConst;
            meta.push_back(m);
            continue;
        }
        int exponent = -1;
        for (int e = 0; e <= kMaxExponent && exponent < 0; e++) {
            if (TryScale(v, validity, start, n, e, ints)) {
                exponent = e;
            }
        }
        if (exponent < 0) {
            m.exponent = ScaledDoubles::kRaw;
            m.width = 64;
            m.offset = static_cast<uint32_t>(payload.size());
            payload.resize(payload.size() + n * sizeof(double));
            std::memcpy(payload.data() + m.offset, v, n * sizeof(double));
            meta.push_back(m);
            continue;
        }
        int64_t lo = INT64_MAX, hi = INT64_MIN;
        for (idx_t i = 0; i < n; i++) {
            if (ints[i] != INT64_MIN) {
                lo = std::min(lo, ints[i]);
                hi = std::max(hi, ints[i]);
            }
        }
        for (idx_t i = 0; i < n; i++) {
            if (ints[i] == INT64_MIN) {
                ints[i] = lo; // NULL rows: any in-range value
            }
        }
        m.exponent = static_cast<uint8_t>(exponent);
        m.base = lo;
        m.width = BitWidth(static_cast<uint64_t>(hi) - static_cast<uint64_t>(lo));
        for (idx_t i = 0; i < n; i++) {
            packed[i] = static_cast<uint64_t>(ints[i]) - static_cast<uint64_t>(lo);
        }
        m.offset = m.width == 0 ? 0 : AppendPacked(payload, packed.data(), n, m.width);
        meta.push_back(m);
    }
    return std::make_shared<ScaledDoubles>(std::move(meta), std::move(payload));
}

// ---------------------------------------------------------------------------------- dictionary
// strings

class DictionaryStrings final : public EncodedColumn {
  public:
    DictionaryStrings(std::shared_ptr<Vector> dictionary, uint32_t entries, bool has_null,
                      uint8_t width, std::vector<uint32_t> offsets, std::vector<uint8_t> payload,
                      size_t dictionary_bytes)
        : dictionary_(std::move(dictionary)), entries_(entries), has_null_(has_null), width_(width),
          offsets_(std::move(offsets)), payload_(std::move(payload)),
          dictionary_bytes_(dictionary_bytes) {}

    EncodingKind kind() const noexcept override { return EncodingKind::Dictionary; }
    void Serialize(BinaryWriter& w) const override {
        w.U32(entries_);
        w.U8(has_null_ ? 1 : 0);
        const auto* strings = dictionary_->FlatData<string_t>();
        for (uint32_t i = 0; i < entries_; i++) {
            w.String(strings[i].view());
        }
        w.U8(width_);
        w.U32(static_cast<uint32_t>(offsets_.size()));
        for (const uint32_t o : offsets_) {
            w.U32(o);
        }
        w.U32(static_cast<uint32_t>(payload_.size()));
        w.Bytes(payload_.data(), payload_.size());
    }
    static std::shared_ptr<DictionaryStrings> Read(BinaryReader& r, LogicalType type, idx_t count) {
        if (type.physical() != PhysicalType::String) {
            r.Fail("a string dictionary for a " + type.ToString() + " column");
        }
        const uint32_t entries = r.Count(4);
        if (entries > kVectorSize - 1) {
            r.Fail("a dictionary of " + std::to_string(entries) + " strings");
        }
        const uint8_t has_null = r.U8();
        if (has_null > 1) {
            r.Fail("a dictionary NULL flag of " + std::to_string(has_null));
        }
        const uint32_t dictionary_size = entries + has_null;
        auto dictionary = std::make_shared<Vector>(LogicalType::Varchar(), kVectorSize);
        string_t* d = dictionary->FlatData<string_t>();
        size_t dictionary_bytes = dictionary_size * sizeof(string_t);
        for (uint32_t i = 0; i < entries; i++) {
            const std::string_view s = r.String();
            d[i] = dictionary->AddString(s);
            dictionary_bytes += s.size() > string_t::kInlineCapacity ? s.size() : 0;
        }
        if (has_null != 0) {
            dictionary->Validity().SetInvalid(entries);
        }
        const uint8_t width = r.U8();
        if (width > 12) {
            r.Fail("a dictionary code width of " + std::to_string(width));
        }
        const uint32_t n = r.Count(4);
        if (n != VectorsOf(count)) {
            r.Fail("dictionary codes for " + std::to_string(n) + " vectors, the column has " +
                   std::to_string(VectorsOf(count)));
        }
        std::vector<uint32_t> offsets(n);
        for (uint32_t& o : offsets) {
            o = r.U32();
        }
        const uint32_t payload_size = r.U32();
        const uint8_t* payload = r.Bytes(payload_size);
        // every code of every vector must name an entry: BitUnpack of an unchecked stream could
        // index the dictionary out of range
        uint64_t codes[kVectorSize];
        for (idx_t v = 0; v < n; v++) {
            const idx_t rows = RowsIn(count, v);
            if (width == 0) {
                offsets[v] = 0;
                if (dictionary_size == 0) {
                    r.Fail("rows with codes but an empty dictionary");
                }
                continue;
            }
            CheckPacked(r, offsets[v], width, rows, payload_size);
            BitUnpack(payload + offsets[v], rows, width, codes);
            for (idx_t i = 0; i < rows; i++) {
                if (codes[i] >= dictionary_size) {
                    r.Fail("a dictionary code of " + std::to_string(codes[i]) +
                           " in a dictionary of " + std::to_string(dictionary_size));
                }
            }
        }
        return std::make_shared<DictionaryStrings>(
            std::move(dictionary), entries, has_null != 0, width, std::move(offsets),
            std::vector<uint8_t>(payload, payload + payload_size), dictionary_bytes);
    }
    size_t MemoryUsage() const noexcept override {
        return payload_.size() + offsets_.size() * sizeof(uint32_t) + dictionary_bytes_;
    }

    bool DecodeVector(idx_t v, idx_t n, const ValidityMask&, Vector& out) const override {
        uint64_t codes[kVectorSize];
        BitUnpack(payload_.data() + offsets_[v], n, width_, codes);
        SelectionVector sel = SelectionVector::Uninitialized(kVectorSize);
        sel_t* s = sel.MutableData();
        for (idx_t i = 0; i < n; i++) {
            s[i] = static_cast<sel_t>(codes[i]);
        }
        for (idx_t i = n; i < kVectorSize; i++) {
            s[i] =
                0; // rows past n are unspecified, but every index stays a valid dictionary position
        }
        // The dictionary is shared, not copied. NULL rows map to its NULL entry.
        out.SetDictionary(dictionary_, std::move(sel));
        return true;
    }

  private:
    std::shared_ptr<Vector> dictionary_;
    uint32_t entries_;
    bool has_null_;
    uint8_t width_;
    std::vector<uint32_t> offsets_;
    std::vector<uint8_t> payload_;
    size_t dictionary_bytes_;
};

std::shared_ptr<EncodedColumn> EncodeDictionary(const ColumnSegment& raw) {
    const auto* strings = reinterpret_cast<const string_t*>(raw.raw_data());
    const ValidityMask& validity = raw.validity();
    const idx_t count = raw.count();
    std::unordered_map<std::string_view, uint32_t> codes;
    std::vector<std::string_view> entries;
    std::vector<uint32_t> row_code(count);
    constexpr uint32_t kNoCode = ~uint32_t{0};
    bool has_nulls = false;
    for (idx_t i = 0; i < count; i++) {
        if (!validity.IsValid(i)) {
            row_code[i] = kNoCode;
            has_nulls = true;
            continue;
        }
        const std::string_view sv = strings[i].view();
        auto [it, inserted] = codes.try_emplace(sv, static_cast<uint32_t>(entries.size()));
        if (inserted) {
            if (entries.size() == kVectorSize - 1) {
                return nullptr; // too many distinct strings for a dictionary vector (with its NULL
                                // entry)
            }
            entries.push_back(sv);
        }
        row_code[i] = it->second;
    }
    const auto null_code = static_cast<uint32_t>(entries.size());
    const size_t dictionary_size = entries.size() + (has_nulls ? 1 : 0);
    auto dictionary = std::make_shared<Vector>(LogicalType::Varchar(), kVectorSize);
    string_t* d = dictionary->FlatData<string_t>();
    size_t dictionary_bytes = dictionary_size * sizeof(string_t);
    for (size_t c = 0; c < entries.size(); c++) {
        d[c] = dictionary->AddString(entries[c]);
        dictionary_bytes += entries[c].size() > string_t::kInlineCapacity ? entries[c].size() : 0;
    }
    if (has_nulls) {
        dictionary->Validity().SetInvalid(null_code);
    }
    const uint8_t width = BitWidth(dictionary_size == 0 ? 0 : dictionary_size - 1);
    std::vector<uint32_t> offsets;
    std::vector<uint8_t> payload;
    std::vector<uint64_t> packed(kVectorSize);
    for (idx_t start = 0; start < count; start += kVectorSize) {
        const idx_t n = std::min<idx_t>(kVectorSize, count - start);
        for (idx_t i = 0; i < n; i++) {
            packed[i] = row_code[start + i] == kNoCode ? null_code : row_code[start + i];
        }
        offsets.push_back(width == 0 ? 0 : AppendPacked(payload, packed.data(), n, width));
    }
    return std::make_shared<DictionaryStrings>(
        std::move(dictionary), static_cast<uint32_t>(entries.size()), has_nulls, width,
        std::move(offsets), std::move(payload), dictionary_bytes);
}

} // namespace

const char* EncodingName(EncodingKind kind) noexcept {
    switch (kind) {
    case EncodingKind::Uncompressed:
        return "UNCOMPRESSED";
    case EncodingKind::Constant:
        return "CONSTANT";
    case EncodingKind::Rle:
        return "RLE";
    case EncodingKind::Bitpacked:
        return "BITPACKED";
    case EncodingKind::ScaledDouble:
        return "SCALED_DOUBLE";
    case EncodingKind::Dictionary:
        return "DICTIONARY";
    }
    return "?";
}

bool CompressionEnabled() noexcept {
    return CompressionState().load(std::memory_order_relaxed) != 0;
}

void SetCompressionEnabled(bool enabled) noexcept {
    CompressionState().store(enabled ? 1 : 0, std::memory_order_relaxed);
}

std::shared_ptr<EncodedColumn> EncodeSegment(const ColumnSegment& raw, EncodingChoice choice) {
    if (raw.encoded() || raw.count() == 0) {
        return nullptr;
    }
    const bool force = choice != EncodingChoice::Auto;
    const size_t raw_bytes = RawDataBytes(raw);
    const auto good_enough = [&](const std::shared_ptr<EncodedColumn>& e) {
        return e != nullptr && (force || static_cast<double>(e->MemoryUsage()) <=
                                             kMaxEncodedFraction * static_cast<double>(raw_bytes));
    };
    const LogicalType type = raw.type();
    std::shared_ptr<EncodedColumn> best;
    const auto consider = [&](std::shared_ptr<EncodedColumn> candidate) {
        if (good_enough(candidate) &&
            (best == nullptr || candidate->MemoryUsage() < best->MemoryUsage())) {
            best = std::move(candidate);
        }
    };
    if (IsIntLike(type)) {
        if (choice == EncodingChoice::Auto || choice == EncodingChoice::Bitpacked) {
            consider(EncodeBitpacked(raw));
        }
        if (choice == EncodingChoice::Constant) {
            auto e = EncodeBitpacked(raw);
            if (e->kind() == EncodingKind::Constant) {
                best = std::move(e);
            }
        }
        if (choice == EncodingChoice::Auto || choice == EncodingChoice::Rle) {
            consider(EncodeRle(raw, force));
        }
    } else if (type.physical() == PhysicalType::Double) {
        if (choice == EncodingChoice::Auto || choice == EncodingChoice::Bitpacked) {
            consider(EncodeScaledDoubles(raw));
        }
        if (choice == EncodingChoice::Constant) {
            auto e = EncodeScaledDoubles(raw);
            if (static_cast<const ScaledDoubles&>(*e).all_const()) {
                best = std::move(e);
            }
        }
    } else if (type.physical() == PhysicalType::String) {
        if (choice == EncodingChoice::Auto || choice == EncodingChoice::Dictionary) {
            consider(EncodeDictionary(raw));
        }
    }
    return best;
}

void SerializeEncodedColumn(const EncodedColumn& column, BinaryWriter& w) {
    w.U8(static_cast<uint8_t>(column.kind()));
    column.Serialize(w);
}

std::shared_ptr<EncodedColumn> DeserializeEncodedColumn(BinaryReader& r, LogicalType type,
                                                        idx_t count) {
    if (count == 0) {
        r.Fail("an encoded column with no rows");
    }
    const uint8_t kind = r.U8();
    switch (static_cast<EncodingKind>(kind)) {
    case EncodingKind::Constant:
    case EncodingKind::Bitpacked:
        return BitpackedInts::Read(r, type, count);
    case EncodingKind::Rle:
        return RleInts::Read(r, type, count);
    case EncodingKind::ScaledDouble:
        return ScaledDoubles::Read(r, type, count);
    case EncodingKind::Dictionary:
        return DictionaryStrings::Read(r, type, count);
    case EncodingKind::Uncompressed:
        break;
    }
    r.Fail("an unknown encoding kind " + std::to_string(kind));
}

std::shared_ptr<ColumnSegment> CompressSegment(std::shared_ptr<ColumnSegment> raw) {
    if (!CompressionEnabled() || raw->encoded() || raw->count() == 0) {
        return raw;
    }
    std::shared_ptr<EncodedColumn> encoded = EncodeSegment(*raw, EncodingChoice::Auto);
    if (encoded == nullptr) {
        return raw;
    }
    return std::make_shared<ColumnSegment>(raw->type(), raw->count(), raw->validity(), raw->stats(),
                                           std::move(encoded));
}

} // namespace cdb
