| Query | Largest function of cdb | Second | Third |
| :--- | :--- | :--- | :--- |
| micro: top-10 of 1M rows | 77% `cdb::SortBuffer::CompareRows` | 5% `std::__merge_sort_with_buffer` | 5% `cdb::LogicalType::physical` |
| micro: sort 1M rows | 81% `cdb::SortBuffer::CompareRows` | 5% `std::__merge_sort_with_buffer` | 5% `cdb::LogicalType::physical` |
| micro: join, 1,000-row build | 45% `cdb::PhysicalHashJoin::Execute` | 25% `cdb::KeyComparator::StoredEqualsInput` | 13% `cdb::ChunkStore::Gather` |
| micro: join, 1M-row build | 32% `cdb::PhysicalHashJoin::Execute` | 17% `cdb::KeyComparator::StoredEqualsInput` | 9% `cdb::ChunkStore::Gather` |
| micro: 100,000 groups | 40% `cdb::KeyComparator::StoredEqualsInput` | 21% `cdb::KeyIndex::FindOrInsert` | 15% `cdb::SumDoubleState::Update` |
| micro: 1M groups | 20% `cdb::KeyIndex::FindOrInsert` | 13% `cdb::KeyComparator::StoredEqualsInput` | 12% `cdb::SumDoubleState::Update` |
| micro: count(*) | 34% `cdb::CountStarState::Update` | 34% `cdb::BitpackedInts::Decode` | 28% `cdb::UnpackWidth` |
| micro: sum of a column | 43% `cdb::UnpackWidth` | 25% `cdb::kernels::SumDoubleAvx2` | 19% `cdb::kernels::OffsetsToScaledDoubleAvx2` |
| micro: filter 90%, sum | 53% `cdb::SumDoubleState::UpdateUngrouped` | 19% `cdb::UnpackWidth` | 6% `cdb::kernels::SelectInt32Avx2` |
| TPC-H Q1, SF0.1 | 28% `cdb::KeyComparator::StoredEqualsInput` | 21% `cdb::SumDoubleState::Update` | 12% `cdb::AvgState` |
| TPC-H Q6 | 25% `__memset_chk_avx2_unaligned_erms` | 10% `cdb::CompareSelectT` | 6% `cdb::kernels::OffsetsToScaledDoubleAvx2` |
| TPC-H Q9 | 34% `cdb::PhysicalHashJoin::Execute` | 18% `cdb::KeyComparator::StoredEqualsInput` | 12% `cdb::ChunkStore::Gather` |
| TPC-H Q17 | 27% `cdb::KeyComparator::StoredEqualsInput` | 23% `cdb::PhysicalHashJoin::Execute` | 13% `cdb::KeyIndex::FindOrInsert` |
| TPC-H Q20 | 15% `cdb::PhysicalHashJoin::Execute` | 11% `cdb::KeyComparator::StoredEqualsInput` | 9% `__memset_chk_avx2_unaligned_erms` |
