#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace cdb {

// Bump-pointer allocator. Allocations are never freed individually, never move (pointers stay
// valid until Reset() or destruction), and are not thread-safe. Used for string heaps and, later,
// hash-table payloads.
class Arena {
  public:
    static constexpr size_t kDefaultBlockSize = 256 * 1024;
    static constexpr size_t kMaxAlignment = 64;

    explicit Arena(size_t block_size = kDefaultBlockSize);
    ~Arena();
    Arena(Arena&&) noexcept;
    Arena& operator=(Arena&&) noexcept;
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    // Returns `size` bytes aligned to `alignment` (a power of two <= kMaxAlignment). Never
    // returns nullptr, even for size 0. Memory is NOT zeroed.
    void* Allocate(size_t size, size_t alignment = 8);

    // Releases everything; invalidates all pointers handed out. Keeps one block for reuse.
    void Reset();

    size_t BytesUsed() const noexcept { return used_; }
    size_t BytesReserved() const noexcept { return reserved_; }
    size_t BlockCount() const noexcept { return blocks_.size(); }

  private:
    struct Block {
        uint8_t* data;
        size_t size;
    };
    struct BlockDeleter {
        static void Free(uint8_t* p) noexcept;
    };

    Block NewBlock(size_t size);
    void FreeBlocks() noexcept;

    size_t block_size_;
    std::vector<Block> blocks_;
    // The block small allocations are bumped from (index into blocks_), or npos if none yet.
    size_t active_ = static_cast<size_t>(-1);
    size_t offset_ = 0; // bump offset within the active block
    size_t used_ = 0;
    size_t reserved_ = 0;
};

} // namespace cdb
