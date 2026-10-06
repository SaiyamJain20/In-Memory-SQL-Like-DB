#include "execution/key_index.h"

#include "execution/hashing.h"
#include "execution/type_dispatch.h"

#include <algorithm>

namespace cdb {

KeyComparator::KeyComparator(const std::vector<const Vector*>& columns, bool nulls_equal)
    : input_(columns.size()), nulls_equal_(nulls_equal) {
    types_.reserve(columns.size());
    for (size_t c = 0; c < columns.size(); c++) {
        columns[c]->ToUnified(input_[c]);
        types_.push_back(columns[c]->type().physical());
    }
}

bool KeyComparator::StoredEqualsInput(const ChunkStore& store, idx_t stored_row,
                                      idx_t input_row) const {
    const DataChunk& chunk = store.chunk(stored_row >> ChunkStore::kShift);
    const idx_t off = stored_row & (kVectorSize - 1);
    for (size_t c = 0; c < input_.size(); c++) {
        const Vector& sv = chunk.column(c);
        const bool s_valid = sv.Validity().IsValid(off);
        const bool i_valid = input_[c].IsValid(input_row);
        if (!s_valid || !i_valid) {
            if (s_valid != i_valid || !nulls_equal_) {
                return false;
            }
            continue;
        }
        const bool eq = DispatchPhysical(types_[c], [&](auto tag) {
            using T = decltype(tag);
            return KeyEquals<T>(sv.FlatData<T>()[off],
                                input_[c].Data<T>()[input_[c].sel[input_row]]);
        });
        if (!eq) {
            return false;
        }
    }
    return true;
}

bool KeyComparator::InputEqualsInput(idx_t a, idx_t b) const {
    for (size_t c = 0; c < input_.size(); c++) {
        const bool a_valid = input_[c].IsValid(a), b_valid = input_[c].IsValid(b);
        if (!a_valid || !b_valid) {
            if (a_valid != b_valid || !nulls_equal_) {
                return false;
            }
            continue;
        }
        const bool eq = DispatchPhysical(types_[c], [&](auto tag) {
            using T = decltype(tag);
            return KeyEquals<T>(input_[c].Data<T>()[input_[c].sel[a]],
                                input_[c].Data<T>()[input_[c].sel[b]]);
        });
        if (!eq) {
            return false;
        }
    }
    return true;
}

KeyIndex::KeyIndex(std::vector<LogicalType> key_types) : keys_(std::move(key_types)) {}

void KeyIndex::EnsureCapacity(idx_t extra) {
    idx_t wanted = 1024;
    while (wanted < 2 * (Count() + extra)) {
        wanted *= 2;
    }
    if (wanted <= slots_.size()) {
        return;
    }
    slots_.assign(wanted, 0);
    mask_ = wanted - 1;
    for (idx_t id = 0; id < hashes_.size(); id++) {
        uint64_t pos = hashes_[id] & mask_;
        while (slots_[pos] != 0) {
            pos = (pos + 1) & mask_;
        }
        slots_[pos] = static_cast<uint32_t>(id + 1);
    }
}

idx_t KeyIndex::FindOrInsert(const DataChunk& keys, idx_t count, uint32_t* ids,
                             std::vector<sel_t>* new_rows) {
    if (count == 0) {
        return 0;
    }
    const idx_t columns = keys_.ColumnCount();
    CDB_CHECK(keys.ColumnCount() == columns);
    if (columns == 0) {
        // One implicit key: everything is the same group.
        const idx_t created = hashes_.empty() ? 1 : 0;
        if (created) {
            hashes_.push_back(0);
            if (new_rows) {
                new_rows->push_back(0);
            }
        }
        std::fill(ids, ids + count, 0U);
        return created;
    }

    std::vector<const Vector*> cols(columns);
    for (idx_t c = 0; c < columns; c++) {
        cols[c] = &keys.column(c);
    }
    std::vector<uint64_t> hashes(count);
    HashColumns(cols.data(), columns, count, hashes.data());
    const KeyComparator cmp(cols, /*nulls_equal=*/true);

    EnsureCapacity(count);
    const idx_t stored = hashes_.size();
    std::vector<sel_t> pending; // input rows that created the new ids stored, stored + 1, ...
    for (idx_t i = 0; i < count; i++) {
        const uint64_t h = hashes[i];
        uint64_t pos = h & mask_;
        for (;;) {
            const uint32_t slot = slots_[pos];
            if (slot == 0) {
                const idx_t id = hashes_.size();
                hashes_.push_back(h);
                slots_[pos] = static_cast<uint32_t>(id + 1);
                pending.push_back(static_cast<sel_t>(i));
                ids[i] = static_cast<uint32_t>(id);
                break;
            }
            const idx_t id = slot - 1;
            if (hashes_[id] == h && (id < stored ? cmp.StoredEqualsInput(keys_, id, i)
                                                 : cmp.InputEqualsInput(pending[id - stored], i))) {
                ids[i] = static_cast<uint32_t>(id);
                break;
            }
            pos = (pos + 1) & mask_;
        }
    }
    if (!pending.empty()) {
        SelectionVector sel(pending.size());
        std::copy(pending.begin(), pending.end(), sel.MutableData());
        keys_.Append(keys, &sel, pending.size());
        if (new_rows) {
            new_rows->insert(new_rows->end(), pending.begin(), pending.end());
        }
    }
    return pending.size();
}

} // namespace cdb
