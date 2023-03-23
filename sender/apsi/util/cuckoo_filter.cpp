// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

// STD
#include <sstream>

// APSI
#include "apsi/util/cuckoo_filter.h"
#include "apsi/util/cuckoo_filter_generated.h"
#include "apsi/version.h"
#include "apsi/util/hash.h"
#include "apsi/util/utils.h"

using namespace std;
using namespace apsi::util;
using namespace apsi::sender::util;

namespace {
    /**
    Hash function for the cuckoo filter.
    The seed is completely arbitrary, doesn't need to be random.
    */
    HashFunc hasher_(/* seed */ 20);
} // namespace


CuckooFilter::CuckooFilter(CuckooFilterTable& table, std::size_t table_num_items, std::size_t overflow_index, std::uint32_t overflow_tag, bool overflow_used){
    table_ = make_unique<CuckooFilterTable>(table);
    num_items_ = table_num_items;
    overflow_ = OverflowCache();
    overflow_.index = overflow_index;
    overflow_.tag = overflow_tag;
    overflow_.used = overflow_used;
}

CuckooFilter::CuckooFilter(size_t key_count_max, size_t bits_per_tag) : num_items_(0), overflow_()
{
    overflow_.used = false;
    table_ = make_unique<CuckooFilterTable>(key_count_max, bits_per_tag);
}

bool CuckooFilter::contains(const felt_t &item) const
{
    size_t idx1, idx2;
    uint32_t tag;

    get_tag_and_index(item, tag, idx1);
    idx2 = get_alt_index(idx1, tag);

    if (overflow_.used && overflow_.tag == tag) {
        if (overflow_.index == idx1 || overflow_.index == idx2)
            return true;
    }

    return table_->find_tag_in_buckets(idx1, idx2, tag);
}

bool CuckooFilter::add(const felt_t &item)
{
    if (overflow_.used)
        return false; // No more space

    uint32_t tag;
    size_t idx;
    get_tag_and_index(item, tag, idx);

    bool result = add_index_tag(idx, tag);
    if (result) {
        num_items_++;
    }

    return result;
}

bool CuckooFilter::add_index_tag(std::size_t idx, std::uint32_t tag)
{
    size_t curr_idx = idx;
    uint32_t curr_tag = tag;
    uint32_t old_tag = 0;

    for (size_t i = 0; i < max_cuckoo_kicks_; i++) {
        bool kickout = i > 0;
        old_tag = 0;

        if (table_->insert_tag(curr_idx, curr_tag, kickout, old_tag)) {
            return true;
        }

        if (kickout) {
            curr_tag = old_tag;
        }

        curr_idx = get_alt_index(curr_idx, curr_tag);
    }

    overflow_.index = curr_idx;
    overflow_.tag = curr_tag;
    overflow_.used = true;

    return true;
}

bool CuckooFilter::remove(const felt_t &item)
{
    size_t idx1, idx2;
    uint32_t tag;

    get_tag_and_index(item, tag, idx1);
    idx2 = get_alt_index(idx1, tag);

    if (table_->delete_tag(idx1, tag)) {
        num_items_--;
        try_eliminate_overflow();
        return true;
    }

    if (table_->delete_tag(idx2, tag)) {
        num_items_--;
        try_eliminate_overflow();
        return true;
    }

    if (overflow_.used && (overflow_.index == idx1 || overflow_.index == idx2) &&
        overflow_.tag == tag) {
        overflow_.used = false;
        num_items_--;
        return true;
    }

    return false;
}

uint32_t CuckooFilter::tag_bit_limit(uint32_t value) const
{
    uint32_t mask = (1 << static_cast<uint32_t>(table_->get_bits_per_tag())) - 1;
    uint32_t tag = value & mask;
    tag += (tag == 0);
    return tag;
}

size_t CuckooFilter::idx_bucket_limit(size_t value) const
{
    size_t mask = table_->get_num_buckets() - 1;
    return value & mask;
}

void CuckooFilter::get_tag_and_index(const felt_t &item, uint32_t &tag, size_t &idx) const
{
    uint64_t hash = static_cast<uint64_t>(hasher_(item));
    idx = idx_bucket_limit(hash >> 32);
    tag = tag_bit_limit(static_cast<uint32_t>(hash));
}

size_t CuckooFilter::get_alt_index(size_t idx, uint32_t tag) const
{
    uint64_t hash = static_cast<uint64_t>(hasher_(tag));
    size_t idx_hash = idx_bucket_limit(hash);
    return idx ^ idx_hash;
}

void CuckooFilter::try_eliminate_overflow()
{
    // Try to insert the overflow item into the table.
    if (overflow_.used) {
        overflow_.used = false;
        add_index_tag(overflow_.index, overflow_.tag);
    }
}

std::size_t CuckooFilter::save(std::ostream &out) const
{
    flatbuffers::FlatBufferBuilder fbs_builder(1024);

    // Get the raw table data of Cuckoo Filter Table and create the flatbuffer vector
    auto cuckoo_filter_table_data = fbs_builder.CreateVector(table_->get_raw_table_data());

    // Create the Cuckoo Filter Table flatbuffer object
    fbs::CuckooFilterTableBuilder cuckoo_filter_table_builder(fbs_builder);
    cuckoo_filter_table_builder.add_bits_per_tag(table_->get_bits_per_tag());
    cuckoo_filter_table_builder.add_num_buckets(table_->get_num_buckets());
    cuckoo_filter_table_builder.add_table(cuckoo_filter_table_data);
    auto cuckoo_filter_table = cuckoo_filter_table_builder.Finish();

    // Create the Cuckoo Filter Overflow Cache flatbuffer object
    fbs::CuckooFilterOverflowCacheBuilder cuckoo_filter_overflow_cache_builder(fbs_builder);
    cuckoo_filter_overflow_cache_builder.add_index(overflow_.index);
    cuckoo_filter_overflow_cache_builder.add_tag(overflow_.tag);
    cuckoo_filter_overflow_cache_builder.add_used(overflow_.used);
    auto cuckoo_filter_overflow_cache = cuckoo_filter_overflow_cache_builder.Finish();

    // Create the Cuckoo Filter flatbuffer object
    fbs::CuckooFilterBuilder cuckoo_filter_builder(fbs_builder);
    cuckoo_filter_builder.add_table(cuckoo_filter_table);
    cuckoo_filter_builder.add_num_items(num_items_);
    cuckoo_filter_builder.add_overflow(cuckoo_filter_overflow_cache);

    auto cuckoo_filter = cuckoo_filter_builder.Finish();
    fbs_builder.FinishSizePrefixed(cuckoo_filter);
    out.write(
        reinterpret_cast<const char *>(fbs_builder.GetBufferPointer()),
        fbs_builder.GetSize());

    return fbs_builder.GetSize();
}

CuckooFilter CuckooFilter::Load(std::istream& in, size_t& bytes_read)
{
    vector<unsigned char> in_data(read_from_stream(in));

    auto verifier =
        flatbuffers::Verifier(reinterpret_cast<const uint8_t *>(in_data.data()), in_data.size());
    bool safe = fbs::VerifySizePrefixedCuckooFilterBuffer(verifier);
    if (!safe) {
        throw runtime_error("failed to load parameters: invalid buffer");
    }

    auto cuckoo_filter_fbs = fbs::GetSizePrefixedCuckooFilter(in_data.data());
    auto cuckoo_filter_table_fbs = cuckoo_filter_fbs->table();
    auto cuckoo_filter_table_data_fbs = cuckoo_filter_table_fbs->table();

    std::vector<std::uint64_t> cuckoo_filter_table_data;
    copy(
        cuckoo_filter_table_data_fbs->cbegin(),
        cuckoo_filter_table_data_fbs->cend(),
        inserter(cuckoo_filter_table_data, cuckoo_filter_table_data.end()));

    auto cuckoo_filter_table = CuckooFilterTable(
        cuckoo_filter_table_data,
        cuckoo_filter_table_fbs->num_buckets(),
        cuckoo_filter_table_fbs->bits_per_tag());

    bytes_read = in_data.size();
    return CuckooFilter(
        cuckoo_filter_table,
        cuckoo_filter_fbs->num_items(),
        cuckoo_filter_fbs->overflow()->index(),
        cuckoo_filter_fbs->overflow()->tag(),
        cuckoo_filter_fbs->overflow()->used());

    // return std::make_pair(cuckoo_filter, in_data.size());
    // return { cuckoo_filter, in_data.size() };
}
