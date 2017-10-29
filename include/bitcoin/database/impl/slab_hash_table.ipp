/**
 * Copyright (c) 2011-2017 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef LIBBITCOIN_DATABASE_SLAB_HASH_TABLE_IPP
#define LIBBITCOIN_DATABASE_SLAB_HASH_TABLE_IPP

#include <bitcoin/bitcoin.hpp>
#include <bitcoin/database/memory/memory.hpp>
#include "../impl/remainder.ipp"
#include "../impl/slab_row.ipp"

namespace libbitcoin {
namespace database {

template <typename KeyType>
const file_offset slab_hash_table<KeyType>::not_found = 0;

template <typename KeyType>
slab_hash_table<KeyType>::slab_hash_table(slab_hash_table_header& header,
    slab_manager& manager)
  : header_(header), manager_(manager)
{
}

// This is not limited to storing unique key values. If duplicate keyed values
// are store then retrieval and unlinking will fail as these multiples cannot
// be differentiated except in the order written (used by bip30).
template <typename KeyType>
file_offset slab_hash_table<KeyType>::store(const KeyType& key,
    write_function write, size_t value_size)
{
    // Allocate and populate new unlinked slab.
    slab_row<KeyType> slab(manager_);
    const auto position = slab.create(key, write, value_size);

    // Critical Section.
    ///////////////////////////////////////////////////////////////////////////
    mutex_.lock();

    // Link new slab.next to current first slab.
    slab.link(read_bucket_value(key));

    // Link header to new slab as the new first.
    link(key, position);

    mutex_.unlock();
    ///////////////////////////////////////////////////////////////////////////

    // Return the file offset of the slab data segment.
    return position + slab_row<KeyType>::prefix_size;
}

// Execute a writer against a key's buffer if the key is found.
// Return the file offset of the found value (or zero).
template <typename KeyType>
file_offset slab_hash_table<KeyType>::update(const KeyType& key,
    write_function write)
{
    // Find start item...
    auto current = read_bucket_value(key);

    // Iterate through list...
    while (current != header_.empty)
    {
        const slab_row<KeyType> item(manager_, current);

        // Found.
        if (item.compare(key))
        {
            const auto data = REMAP_ADDRESS(item.data());
            auto serial = make_unsafe_serializer(data);
            write(serial);
            return item.offset();
        }

        const auto previous = current;
        current = item.next_position();
        BITCOIN_ASSERT(previous != current);
    }

    return not_found;
}

// This is limited to returning the first of multiple matching key values.
template <typename KeyType>
memory_ptr slab_hash_table<KeyType>::find(const KeyType& key) const
{
    // Find start item...
    auto current = read_bucket_value(key);

    // Iterate through list...
    while (current != header_.empty)
    {
        const slab_row<KeyType> item(manager_, current);

        // Found.
        if (item.compare(key))
            return item.data();

        const auto previous = current;
        current = item.next_position();
        BITCOIN_ASSERT(previous != current);
    }

    return nullptr;
}

// Unlink is not safe for concurrent write.
// This is limited to unlinking the first of multiple matching key values.
template <typename KeyType>
bool slab_hash_table<KeyType>::unlink(const KeyType& key)
{
    // Find start item...
    const auto begin = read_bucket_value(key);
    const slab_row<KeyType> begin_item(manager_, begin);

    // If start item has the key then unlink from buckets.
    if (begin_item.compare(key))
    {
        link(key, begin_item.next_position());
        return true;
    }

    // Continue on...
    auto previous = begin;
    auto current = begin_item.next_position();

    // Iterate through list...
    while (current != header_.empty)
    {
        const slab_row<KeyType> item(manager_, current);

        // Found, unlink current item from previous.
        if (item.compare(key))
        {
            unlink(item, previous);
            return true;
        }

        previous = current;
        current = item.next_position();
        BITCOIN_ASSERT(previous != current);
    }

    return false;
}

template <typename KeyType>
array_index slab_hash_table<KeyType>::bucket_index(const KeyType& key) const
{
    const auto bucket = remainder(key, header_.size());
    BITCOIN_ASSERT(bucket < header_.size());
    return bucket;
}

template <typename KeyType>
file_offset slab_hash_table<KeyType>::read_bucket_value(
    const KeyType& key) const
{
    const auto value = header_.read(bucket_index(key));
    static_assert(sizeof(value) == sizeof(file_offset), "Invalid size");
    return value;
}

template <typename KeyType>
void slab_hash_table<KeyType>::link(const KeyType& key, file_offset begin)
{
    header_.write(bucket_index(key), begin);
}

template <typename KeyType>
template <typename ListItem>
void slab_hash_table<KeyType>::unlink(const ListItem& item,
    file_offset previous)
{
    ListItem previous_item(manager_, previous);
    previous_item.write_next_position(item.next_position());
}

} // namespace database
} // namespace libbitcoin

#endif
