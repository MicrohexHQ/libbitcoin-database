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
#ifndef LIBBITCOIN_DATABASE_RECORD_ROW_IPP
#define LIBBITCOIN_DATABASE_RECORD_ROW_IPP

#include <bitcoin/database/define.hpp>
#include <bitcoin/database/memory/memory.hpp>
#include <bitcoin/database/primitives/record_manager.hpp>

namespace libbitcoin {
namespace database {

template <typename KeyType>
record_row<KeyType>::record_row(record_manager& manager, array_index index)
  : manager_(manager), index_(index)
{
}

template <typename KeyType>
array_index record_row<KeyType>::create(const KeyType& key,
    write_function write)
{
    BITCOIN_ASSERT(index_ == 0);

    // Create new record and populate its key and data.
    //   [ KeyType  ] <==
    //   [ next:4   ]
    //   [ value... ] <==
    index_ = manager_.new_records(1);

    const auto memory = raw_data(key_start);
    const auto record = REMAP_ADDRESS(memory);
    auto serial = make_unsafe_serializer(record);
    serial.write_forward(key);
    serial.skip(index_size);
    serial.write_delegated(write);

    return index_;
}

template <typename KeyType>
void record_row<KeyType>::link(array_index next)
{
    // Populate next pointer value.
    //   [ KeyType  ]
    //   [ next:4   ] <==
    //   [ value... ]

    // Write record.
    const auto memory = raw_data(key_size);
    auto serial = make_unsafe_serializer(REMAP_ADDRESS(memory));

    //*************************************************************************
    serial.template write_little_endian<array_index>(next);
    //*************************************************************************
}

template <typename KeyType>
bool record_row<KeyType>::compare(const KeyType& key) const
{
    // Key data is at the start.
    const auto memory = raw_data(key_start);
    return std::equal(key.begin(), key.end(), REMAP_ADDRESS(memory));
}

template <typename KeyType>
memory_ptr record_row<KeyType>::data() const
{
    // Get value pointer.
    //   [ KeyType  ]
    //   [ next:4   ]
    //   [ value... ] ==>

    // Value data is at the end.
    return raw_data(prefix_size);
}

template <typename KeyType>
file_offset record_row<KeyType>::offset() const
{
    // Value data is at the end.
    return index_ + prefix_size;
}

template <typename KeyType>
array_index record_row<KeyType>::next_index() const
{
    const auto memory = raw_data(key_size);
    const auto next_address = REMAP_ADDRESS(memory);

    // Critical Section
    ///////////////////////////////////////////////////////////////////////////
    shared_lock lock(mutex_);
    return from_little_endian_unsafe<array_index>(next_address);
    ///////////////////////////////////////////////////////////////////////////
}

template <typename KeyType>
void record_row<KeyType>::write_next_index(array_index next)
{
    const auto memory = raw_data(key_size);
    auto serial = make_unsafe_serializer(REMAP_ADDRESS(memory));

    // Critical Section
    ///////////////////////////////////////////////////////////////////////////
    unique_lock lock(mutex_);
    serial.template write_little_endian<array_index>(next);
    ///////////////////////////////////////////////////////////////////////////
}

template <typename KeyType>
memory_ptr record_row<KeyType>::raw_data(file_offset offset) const
{
    auto memory = manager_.get(index_);
    REMAP_INCREMENT(memory, offset);
    return memory;
}

} // namespace database
} // namespace libbitcoin

#endif
