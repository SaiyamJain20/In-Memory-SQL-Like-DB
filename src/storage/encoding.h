#pragma once

#include "storage/column_segment.h"

#include <memory>

namespace cdb {

class BinaryWriter;
class BinaryReader;

// Lightweight column encodings for sealed (immutable) segments. Each is chosen per segment at seal
// time from the data itself, and only when it is clearly smaller than the raw layout; otherwise the
// segment stays raw and keeps its zero-copy scans.
//
//   Constant      every value equal (one value per vector, decoded to a CONSTANT vector)
//   Rle           runs of equal values, for integers/dates/booleans
//   Bitpacked     per 2048-row vector: frame of reference (value = base + packed offset) or delta
//                 (value = previous + packed delta, for non-decreasing vectors), for integers/dates
//   ScaledDouble  doubles that are exactly integer / 10^e (money, rates): the integers are
//                 frame-of-reference packed. Verified bit for bit when encoding, so lossless; a
//                 vector with any value that does not round-trip is stored raw
//   Dictionary    low-cardinality strings: a shared dictionary plus bit-packed codes. A scan hands
//                 back a DICTIONARY-format vector (the dictionary + a selection vector of codes),
//                 so no string is copied or decoded
enum class EncodingKind : uint8_t {
    Uncompressed,
    Constant,
    Rle,
    Bitpacked,
    ScaledDouble,
    Dictionary
};

const char* EncodingName(EncodingKind kind) noexcept;

// An encoded column: knows how to produce vector `v` (rows [v * 2048, v * 2048 + n)) of a segment.
class EncodedColumn {
  public:
    virtual ~EncodedColumn() = default;
    virtual EncodingKind kind() const noexcept = 0;
    virtual size_t MemoryUsage() const noexcept = 0;

    // `out` has just been Reset() (an empty Flat vector). `validity` is the segment's validity for
    // this vector. Either writes values into `out` as a Flat vector and returns false (the caller
    // then attaches `validity`), or turns `out` into a complete CONSTANT / DICTIONARY vector,
    // NULLs included, and returns true.
    virtual bool DecodeVector(idx_t v, idx_t n, const ValidityMask& validity,
                              Vector& out) const = 0;

    // Writes the encoding's own data (not its kind): see SerializeEncodedColumn.
    virtual void Serialize(BinaryWriter& w) const = 0;
};

// The encoding as a self-describing byte string: its kind, then its payload.
void SerializeEncodedColumn(const EncodedColumn& column, BinaryWriter& w);

// Reads one back for a column of `count` rows of `type`. The bytes are untrusted: every offset,
// width, run end and dictionary code is checked against the sizes it will be used with, so that
// decoding any vector of the result cannot read outside its own buffers. Throws
// Error(ErrorCode::Corruption) otherwise.
std::shared_ptr<EncodedColumn> DeserializeEncodedColumn(BinaryReader& r, LogicalType type,
                                                        idx_t count);

// What to try. Auto picks the smallest encoding that saves at least 30%; the others force one
// (ignoring the size) and exist so tests can exercise every encoding on any data.
enum class EncodingChoice : uint8_t { Auto, Constant, Rle, Bitpacked, Dictionary };

// Encodes the (raw) segment, or returns null if the choice does not apply to its type or (for Auto)
// does not pay off.
std::shared_ptr<EncodedColumn> EncodeSegment(const ColumnSegment& raw, EncodingChoice choice);

// Compression is on by default; turning it off makes every segment raw (to measure the difference).
bool CompressionEnabled() noexcept;
void SetCompressionEnabled(bool enabled) noexcept;

// The segment compressed with the best encoding, or `raw` itself if compression is off or does not
// pay.
std::shared_ptr<ColumnSegment> CompressSegment(std::shared_ptr<ColumnSegment> raw);

} // namespace cdb
