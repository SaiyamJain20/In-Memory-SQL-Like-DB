#pragma once

#include <cstddef>
#include <cstdint>

namespace cdb {

// CRC-32C (Castagnoli, the polynomial iSCSI, ext4 and RocksDB use), the checksum of every block of
// a checkpoint file and every frame of the write-ahead log. Crc32c(b, nb, Crc32c(a, na)) equals the
// checksum of a followed by b, so a block can be checksummed in pieces. The check value of the
// ASCII string "123456789" is 0xE3069283.
//
// Uses the SSE4.2 CRC32 instruction when the CPU has it (and SIMD is enabled, see kernels/cpu.h),
// a table-driven version otherwise; both give identical results (tested).
uint32_t Crc32c(const void* data, size_t size, uint32_t seed = 0) noexcept;

// The portable implementation, for tests.
uint32_t Crc32cPortable(const void* data, size_t size, uint32_t seed = 0) noexcept;

} // namespace cdb
