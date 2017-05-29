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
#include <bitcoin/database/databases/history_database.hpp>

#include <cstdint>
#include <cstddef>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/database/memory/memory.hpp>
#include <bitcoin/database/primitives/record_multimap_iterable.hpp>
#include <bitcoin/database/primitives/record_multimap_iterator.hpp>

// Record format (v4) [47 bytes]:
// ----------------------------------------------------------------------------
// [ height:4      - const] (may short-circuit sequential read after height)
// [ kind:1        - const]
// [ point-hash:32 - const]
// [ point-index:2 - const]
// [ data:8        - const]

// Record format (v3) [47 bytes]:
// ----------------------------------------------------------------------------
// [ kind:1        - const]
// [ point-hash:32 - const]
// [ point-index:2 - const]
// [ height:4      - const]
// [ data:8        - const]

namespace libbitcoin {
namespace database {

using namespace bc::chain;

static constexpr auto rows_header_size = 0u;

static constexpr auto flag_size = sizeof(uint8_t);
static constexpr auto point_size = std::tuple_size<point>::value;
static constexpr auto height_position = flag_size + point_size;
static constexpr auto height_size = sizeof(uint32_t);
static constexpr auto checksum_size = sizeof(uint64_t);
static constexpr auto value_size = flag_size + point_size + height_size +
    checksum_size;

static BC_CONSTEXPR auto record_size =
    hash_table_multimap_record_size<short_hash>();
static BC_CONSTEXPR auto row_record_size =
    hash_table_record_size<hash_digest>(value_size);

enum class point_kind : uint8_t
{
    output = 0,
    input = 1
};

// History uses a hash table index, O(1).
history_database::history_database(const path& lookup_filename,
    const path& rows_filename, size_t buckets, size_t expansion,
    mutex_ptr mutex)
  : initial_map_file_size_(record_hash_table_header_size(buckets) +
        minimum_records_size),

    lookup_file_(lookup_filename, mutex, expansion),
    lookup_header_(lookup_file_, buckets),
    lookup_manager_(lookup_file_, record_hash_table_header_size(buckets),
        record_size),
    lookup_map_(lookup_header_, lookup_manager_),

    rows_file_(rows_filename, mutex, expansion),
    rows_manager_(rows_file_, rows_header_size, row_record_size),
    rows_list_(rows_manager_),
    rows_multimap_(lookup_map_, rows_list_)
{
}

history_database::~history_database()
{
    close();
}

// Startup and shutdown.
// ----------------------------------------------------------------------------

bool history_database::create()
{
    // Resize and create require an opened file.
    if (!lookup_file_.open() ||
        !rows_file_.open())
        return false;

    // These will throw if insufficient disk space.
    lookup_file_.resize(initial_map_file_size_);
    rows_file_.resize(minimum_records_size);

    if (!lookup_header_.create() ||
        !lookup_manager_.create() ||
        !rows_manager_.create())
        return false;

    // Should not call start after create, already started.
    return
        lookup_header_.start() &&
        lookup_manager_.start() &&
        rows_manager_.start();
}

bool history_database::open()
{
    return
        lookup_file_.open() &&
        rows_file_.open() &&
        lookup_header_.start() &&
        lookup_manager_.start() &&
        rows_manager_.start();
}

void history_database::commit()
{
    lookup_manager_.sync();
    rows_manager_.sync();
}

bool history_database::flush() const
{
    return
        lookup_file_.flush() &&
        rows_file_.flush();
}

bool history_database::close()
{
    return
        lookup_file_.close() &&
        rows_file_.close();
}

// Queries.
// ----------------------------------------------------------------------------

history_database::list history_database::get(const short_hash& key,
    size_t limit, size_t from_height) const
{
    list result;
    payment_record payment;
    const auto start = rows_multimap_.lookup(key);
    const auto records = record_multimap_iterable(rows_list_, start);

    for (const auto index: records)
    {
        if (limit > 0 && result.size() >= limit)
            break;

        const auto record = rows_list_.get(index);
        auto deserial = make_unsafe_deserializer(REMAP_ADDRESS(record));

        // Failed reads are conflated with skipped returns.
        if (payment.from_data(deserial, from_height))
            result.push_back(payment);
    }

    return result;
}

history_statinfo history_database::statinfo() const
{
    return
    {
        lookup_header_.size(),
        lookup_manager_.count(),
        rows_manager_.count()
    };
}

// Store.
// ----------------------------------------------------------------------------

void history_database::store(const short_hash& key,
    const payment_record& payment)
{
    const auto write = [&](byte_serializer& serial)
    {
        payment.to_data(serial, false);
    };

    rows_multimap_.add_row(key, write);
}

// Update.
// ----------------------------------------------------------------------------

bool history_database::unlink_last_row(const short_hash& key)
{
    return rows_multimap_.delete_last_row(key);
}

} // namespace database
} // namespace libbitcoin
