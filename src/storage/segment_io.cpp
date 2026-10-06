#include "storage/segment_io.h"

#include "storage/encoding.h"

#include <algorithm>
#include <bit>

namespace cdb {

namespace {

bool ValidTypeId(uint8_t id) {
    return id <= static_cast<uint8_t>(TypeId::Varchar);
}

// Calls f(tag) with a value of the C++ element type of `type`.
template <class F> void WithElement(LogicalType type, F&& f) {
    switch (type.physical()) {
    case PhysicalType::Bool:
        f(bool{});
        break;
    case PhysicalType::Int32:
        f(int32_t{});
        break;
    case PhysicalType::Int64:
        f(int64_t{});
        break;
    case PhysicalType::Double:
        f(double{});
        break;
    case PhysicalType::String:
        f(string_t{});
        break;
    }
}

} // namespace

void WriteValue(BinaryWriter& w, const Value& value) {
    CDB_CHECK(!value.IsNull());
    switch (value.type().id()) {
    case TypeId::Boolean:
        w.U8(value.GetBoolean() ? 1 : 0);
        break;
    case TypeId::Integer:
        w.U32(static_cast<uint32_t>(value.GetInteger()));
        break;
    case TypeId::Date:
        w.U32(static_cast<uint32_t>(value.GetDate().days));
        break;
    case TypeId::BigInt:
        w.I64(value.GetBigInt());
        break;
    case TypeId::Double:
        w.F64(value.GetDouble());
        break;
    case TypeId::Varchar:
        w.String(value.GetVarchar());
        break;
    }
}

Value ReadValue(BinaryReader& r, LogicalType type) {
    switch (type.id()) {
    case TypeId::Boolean: {
        const uint8_t b = r.U8();
        if (b > 1) {
            r.Fail("a BOOLEAN of " + std::to_string(b));
        }
        return Value::Boolean(b != 0);
    }
    case TypeId::Integer:
        return Value::Integer(static_cast<int32_t>(r.U32()));
    case TypeId::Date:
        return Value::Date(date_t{static_cast<int32_t>(r.U32())});
    case TypeId::BigInt:
        return Value::BigInt(r.I64());
    case TypeId::Double:
        return Value::Double(r.F64());
    case TypeId::Varchar:
        return Value::Varchar(std::string(r.String()));
    }
    r.Fail("an unknown type");
}

// ---------------------------------------------------------------------------------- validity

void WriteValidity(BinaryWriter& w, const ValidityMask& validity, idx_t n) {
    bool any_null = false;
    if (!validity.AllValid()) {
        any_null = validity.CountValid(n) != n;
    }
    w.U8(any_null ? 1 : 0);
    if (!any_null) {
        return;
    }
    const uint64_t* words = validity.Words();
    const idx_t bytes = (n + 7) / 8;
    for (idx_t j = 0; j < bytes; j++) {
        auto b = static_cast<uint8_t>((words[j / 8] >> (8 * (j % 8))) & 0xFF);
        if (j == bytes - 1 && n % 8 != 0) {
            b &= static_cast<uint8_t>((1u << (n % 8)) - 1); // bits past the last row mean nothing
        }
        w.U8(b);
    }
}

idx_t ReadValidity(BinaryReader& r, idx_t n, ValidityMask& validity) {
    const uint8_t has_nulls = r.U8();
    if (has_nulls > 1) {
        r.Fail("a NULL flag of " + std::to_string(has_nulls));
    }
    if (has_nulls == 0) {
        return 0;
    }
    const idx_t bytes = (n + 7) / 8;
    const uint8_t* bitmap = r.Bytes(bytes);
    idx_t nulls = 0;
    for (idx_t i = 0; i < n; i++) {
        if (((bitmap[i / 8] >> (i % 8)) & 1) == 0) {
            validity.SetInvalid(i);
            nulls++;
        }
    }
    return nulls;
}

// ---------------------------------------------------------------------------------- column data

void WriteColumnData(BinaryWriter& w, LogicalType type, const uint8_t* data, const sel_t* sel,
                     const ValidityMask& validity, idx_t n) {
    // validity as seen through `sel`
    bool any_null = false;
    for (idx_t i = 0; i < n && !any_null; i++) {
        any_null = !validity.IsValid(sel != nullptr ? sel[i] : i);
    }
    w.U8(any_null ? 1 : 0);
    if (any_null) {
        const idx_t bytes = (n + 7) / 8;
        for (idx_t j = 0; j < bytes; j++) {
            uint8_t b = 0;
            for (idx_t bit = 0; bit < 8 && j * 8 + bit < n; bit++) {
                const idx_t i = j * 8 + bit;
                if (validity.IsValid(sel != nullptr ? sel[i] : i)) {
                    b |= static_cast<uint8_t>(1u << bit);
                }
            }
            w.U8(b);
        }
    }
    WithElement(type, [&](auto tag) {
        using T = decltype(tag);
        const T* values = reinterpret_cast<const T*>(data);
        for (idx_t i = 0; i < n; i++) {
            const idx_t row = sel != nullptr ? sel[i] : i;
            if (!validity.IsValid(row)) {
                continue;
            }
            if constexpr (std::is_same_v<T, bool>) {
                w.U8(values[row] ? 1 : 0);
            } else if constexpr (std::is_same_v<T, int32_t>) {
                w.U32(static_cast<uint32_t>(values[row]));
            } else if constexpr (std::is_same_v<T, int64_t>) {
                w.I64(values[row]);
            } else if constexpr (std::is_same_v<T, double>) {
                w.F64(values[row]);
            } else {
                w.String(values[row].view());
            }
        }
    });
}

idx_t ReadColumnData(BinaryReader& r, LogicalType type, idx_t n, uint8_t* data,
                     ValidityMask& validity, StringHeap* heap) {
    const idx_t nulls = ReadValidity(r, n, validity);
    WithElement(type, [&](auto tag) {
        using T = decltype(tag);
        T* values = reinterpret_cast<T*>(data);
        for (idx_t i = 0; i < n; i++) {
            if (!validity.IsValid(i)) {
                continue;
            }
            if constexpr (std::is_same_v<T, bool>) {
                const uint8_t b = r.U8();
                if (b > 1) {
                    r.Fail("a BOOLEAN of " + std::to_string(b));
                }
                values[i] = b != 0;
            } else if constexpr (std::is_same_v<T, int32_t>) {
                values[i] = static_cast<int32_t>(r.U32());
            } else if constexpr (std::is_same_v<T, int64_t>) {
                values[i] = r.I64();
            } else if constexpr (std::is_same_v<T, double>) {
                values[i] = r.F64();
            } else {
                CDB_CHECK(heap != nullptr);
                values[i] = heap->Add(r.String());
            }
        }
    });
    return nulls;
}

// ---------------------------------------------------------------------------------- chunks

void WriteChunk(BinaryWriter& w, const DataChunk& chunk) {
    w.U32(static_cast<uint32_t>(chunk.size()));
    for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
        UnifiedFormat u;
        chunk.column(c).ToUnified(u);
        WriteColumnData(w, chunk.column(c).type(), u.data, u.sel, *u.validity, chunk.size());
    }
}

void ReadChunk(BinaryReader& r, const std::vector<LogicalType>& types, DataChunk& out) {
    const uint32_t n = r.U32();
    if (n > kVectorSize) {
        r.Fail("a chunk of " + std::to_string(n) + " rows");
    }
    out.Initialize(types, kVectorSize);
    for (idx_t c = 0; c < types.size(); c++) {
        Vector& v = out.column(c);
        StringHeap* heap = types[c].id() == TypeId::Varchar ? &v.Heap() : nullptr;
        ReadColumnData(r, types[c], n, v.FlatBytes(), v.Validity(), heap);
    }
    out.SetCardinality(n);
}

// ---------------------------------------------------------------------------------- segments

namespace {

// A distinct-value sketch, in whichever form is smaller: [u8 0] (none), [u8 1][u16 count]
// ([u16 register][u8 value])... for a sketch with few non-empty registers (a low-cardinality
// column), or [u8 2] and every register for a dense one.
constexpr uint8_t kSketchNone = 0, kSketchSparse = 1, kSketchDense = 2;

void WriteSketch(BinaryWriter& w, const HyperLogLog* sketch) {
    if (sketch == nullptr) {
        w.U8(kSketchNone);
        return;
    }
    const size_t used = sketch->NonZeroRegisters();
    const std::vector<uint8_t>& registers = sketch->registers();
    if (used * 3 + 2 < HyperLogLog::kRegisters) {
        w.U8(kSketchSparse);
        w.U8(static_cast<uint8_t>(used & 0xff));
        w.U8(static_cast<uint8_t>(used >> 8));
        for (size_t i = 0; i < registers.size(); i++) {
            if (registers[i] != 0) {
                w.U8(static_cast<uint8_t>(i & 0xff));
                w.U8(static_cast<uint8_t>(i >> 8));
                w.U8(registers[i]);
            }
        }
        return;
    }
    w.U8(kSketchDense);
    w.Bytes(registers.data(), registers.size());
}

std::shared_ptr<const HyperLogLog> ReadSketch(BinaryReader& r) {
    const uint8_t form = r.U8();
    std::vector<uint8_t> registers;
    switch (form) {
    case kSketchNone:
        return nullptr;
    case kSketchSparse: {
        const size_t used = r.U8() | (static_cast<size_t>(r.U8()) << 8);
        if (used * 3 > r.remaining()) {
            r.Fail("a distinct-value sketch of " + std::to_string(used) + " registers in " +
                   std::to_string(r.remaining()) + " bytes");
        }
        registers.assign(HyperLogLog::kRegisters, 0);
        long previous = -1;
        for (size_t k = 0; k < used; k++) {
            const size_t index = r.U8() | (static_cast<size_t>(r.U8()) << 8);
            const uint8_t value = r.U8();
            if (index >= HyperLogLog::kRegisters || static_cast<long>(index) <= previous ||
                value == 0) {
                r.Fail("a malformed sparse distinct-value sketch");
            }
            previous = static_cast<long>(index);
            registers[index] = value;
        }
        break;
    }
    case kSketchDense: {
        const uint8_t* bytes = r.Bytes(HyperLogLog::kRegisters);
        registers.assign(bytes, bytes + HyperLogLog::kRegisters);
        break;
    }
    default:
        r.Fail("a distinct-value sketch form of " + std::to_string(form));
    }
    auto sketch = std::make_shared<HyperLogLog>();
    if (!HyperLogLog::FromRegisters(std::move(registers), *sketch)) {
        r.Fail("a distinct-value sketch with an out-of-range register");
    }
    return sketch;
}

} // namespace

void WriteSegment(BinaryWriter& w, const ColumnSegment& segment) {
    const idx_t n = segment.count();
    w.U8(static_cast<uint8_t>(segment.type().id()));
    w.U8(segment.encoded() ? 1 : 0);
    w.U32(static_cast<uint32_t>(n));
    const ColumnStats& stats = segment.stats();
    w.U32(static_cast<uint32_t>(stats.null_count));
    const bool bounds = stats.min.has_value() && stats.max.has_value();
    w.U8(bounds ? 1 : 0);
    if (bounds) {
        WriteValue(w, *stats.min);
        WriteValue(w, *stats.max);
    }
    WriteSketch(w, stats.distinct.get());
    if (segment.encoded()) {
        WriteValidity(w, segment.validity(), n);
        SerializeEncodedColumn(*segment.encoding(), w);
    } else {
        WriteColumnData(w, segment.type(), segment.raw_data(), nullptr, segment.validity(), n);
    }
}

std::shared_ptr<ColumnSegment> ReadSegment(BinaryReader& r, LogicalType type, idx_t count) {
    const uint8_t type_id = r.U8();
    if (!ValidTypeId(type_id) || static_cast<TypeId>(type_id) != type.id()) {
        r.Fail("a column of type " + std::to_string(type_id) + ", expected " + type.ToString());
    }
    const uint8_t encoded = r.U8();
    if (encoded > 1) {
        r.Fail("an encoding flag of " + std::to_string(encoded));
    }
    if (r.U32() != count) {
        r.Fail("a segment whose row count differs from its row group's");
    }
    if (count == 0 || count > kRowGroupSize * 64) {
        r.Fail("a segment of " + std::to_string(count) + " rows");
    }
    ColumnStats stats;
    stats.count = count;
    stats.null_count = r.U32();
    if (stats.null_count > count) {
        r.Fail("more NULLs than rows");
    }
    const uint8_t bounds = r.U8();
    if (bounds > 1) {
        r.Fail("a bounds flag of " + std::to_string(bounds));
    }
    if (bounds == 1) {
        stats.min = ReadValue(r, type);
        stats.max = ReadValue(r, type);
        if (Value::Compare(*stats.min, *stats.max) > 0) {
            r.Fail("a minimum above the maximum");
        }
        if (stats.AllNull()) {
            r.Fail("bounds for a column with no values");
        }
        if (type.id() == TypeId::Varchar &&
            (stats.min->GetVarchar().size() > ColumnStats::kMaxBoundStringLength ||
             stats.max->GetVarchar().size() > ColumnStats::kMaxBoundStringLength)) {
            r.Fail("a string bound longer than the zone-map limit");
        }
    }
    stats.distinct = ReadSketch(r);
    if (stats.distinct != nullptr && stats.distinct->Empty() != stats.AllNull()) {
        r.Fail("a distinct-value sketch that disagrees with the NULL count");
    }
    const idx_t rounded = AlignUp(count, kVectorSize);
    ValidityMask validity(rounded);
    if (encoded == 0 && r.remaining() < (count + 7) / 8) {
        // a raw column of n rows needs at least a bit per row (its NULL bitmap) in the file; say so
        // before allocating n elements for a header that merely claims them
        r.Fail("a raw column of " + std::to_string(count) + " rows in " +
               std::to_string(r.remaining()) + " bytes");
    }
    if (encoded == 1) {
        const idx_t nulls = ReadValidity(r, count, validity);
        if (nulls != stats.null_count) {
            r.Fail("the validity bits hold " + std::to_string(nulls) +
                   " NULLs, the statistics say " + std::to_string(stats.null_count));
        }
        std::shared_ptr<EncodedColumn> column = DeserializeEncodedColumn(r, type, count);
        return std::make_shared<ColumnSegment>(type, count, std::move(validity), std::move(stats),
                                               std::move(column));
    }
    auto data = Buffer::Allocate(rounded * type.width());
    std::shared_ptr<StringHeap> heap =
        type.id() == TypeId::Varchar ? std::make_shared<StringHeap>() : nullptr;
    const idx_t nulls = ReadColumnData(r, type, count, data->data(), validity, heap.get());
    if (nulls != stats.null_count) {
        r.Fail("the validity bits hold " + std::to_string(nulls) + " NULLs, the statistics say " +
               std::to_string(stats.null_count));
    }
    if (heap != nullptr) {
        heap->Seal();
    }
    return std::make_shared<ColumnSegment>(type, count, std::move(data), std::move(validity),
                                           std::move(heap), std::move(stats));
}

} // namespace cdb
