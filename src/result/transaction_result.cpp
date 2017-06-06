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
#include <bitcoin/database/result/transaction_result.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/database/databases/transaction_database.hpp>
#include <bitcoin/database/memory/memory.hpp>

namespace libbitcoin {
namespace database {

using namespace bc::chain;
using namespace bc::machine;

static constexpr size_t indexed_size = sizeof(uint8_t);
static constexpr size_t height_size = sizeof(uint32_t);
static constexpr size_t value_size = sizeof(uint64_t);
static constexpr size_t position_size = sizeof(uint16_t);
static constexpr size_t state_size = sizeof(uint8_t);
static constexpr size_t metadata_size = height_size + position_size +
    state_size;

const uint16_t transaction_result::unconfirmed = max_uint16;
const uint32_t transaction_result::unverified = rule_fork::unverified;

transaction_result::transaction_result()
  : transaction_result(nullptr)
{
}

transaction_result::transaction_result(memory_ptr slab)
  : slab_(nullptr),
    height_(unverified),
    position_(unconfirmed),
    hash_(null_hash),
    state_(transaction_state::missing)
{
}

transaction_result::transaction_result(memory_ptr slab, hash_digest&& hash,
    uint32_t height, uint16_t position, transaction_state state)
  : slab_(slab),
    height_(height),
    position_(position),
    hash_(std::move(hash)),
    state_(state)
{
}

transaction_result::transaction_result(memory_ptr slab,
    const hash_digest& hash, uint32_t height, uint16_t position,
    transaction_state state)
  : slab_(slab),
    height_(height),
    position_(position),
    hash_(hash),
    state_(state)
{
}

transaction_result::operator bool() const
{
    return slab_ != nullptr;
}

void transaction_result::reset()
{
    slab_.reset();
}

code transaction_result::error() const
{
    // Height stores error code if the tx is invalid.
    return state_ == transaction_state::invalid ?
        static_cast<error::error_code_t>(height_) : error::success;
}

transaction_state transaction_result::state() const
{
    return state_;
}

// Position is unconfirmed unless if block-associated.
size_t transaction_result::position() const
{
    return position_;
}

// Height is overloaded (holds forks) unless confirmed.
size_t transaction_result::height() const
{
    return height_;
}

const hash_digest& transaction_result::hash() const
{
    return hash_;
}

// Spentness is unguarded and will be inconsistent during write.
bool transaction_result::is_spent(size_t fork_height) const
{
    const auto allow_indexed = (fork_height != max_size_t);
    const auto confirmed =
        (state_ == transaction_state::indexed && allow_indexed) ||
        (state_ == transaction_state::confirmed && height_ <= fork_height);

    // Cannot be spent unless confirmed.
    if (!confirmed)
        return false;

    BITCOIN_ASSERT(slab_);
    const auto tx_start = REMAP_ADDRESS(slab_) + metadata_size;
    auto deserial = make_unsafe_deserializer(tx_start);
    const auto outputs = deserial.read_size_little_endian();

    // Search all outputs for an unspent indication.
    for (uint32_t out = 0; out < outputs; ++out)
    {
        // TODO: This reads the full output, which is simple but not optimial.
        const auto output = output::factory(deserial, false);

        if (!output.validation.spent(fork_height, allow_indexed))
            return false;
    }

    return true;
}

// Spentness is unguarded and will be inconsistent during write.
// If index is out of range returns default/invalid output (.value not_found).
chain::output transaction_result::output(uint32_t index) const
{
    BITCOIN_ASSERT(slab_);
    const auto tx_start = REMAP_ADDRESS(slab_) + metadata_size;
    auto deserial = make_unsafe_deserializer(tx_start);
    const auto outputs = deserial.read_size_little_endian();

    if (index >= outputs)
        return{};

    // Skip outputs until the target output.
    for (uint32_t out = 0; out < index; ++out)
    {
        deserial.skip(indexed_size + height_size + value_size);
        deserial.skip(deserial.read_size_little_endian());
    }

    // Read and return the target output (including spender height).
    return chain::output::factory(deserial, false);
}

// Spentness is unguarded and will be inconsistent during write.
chain::transaction transaction_result::transaction() const
{
    BITCOIN_ASSERT(slab_);
    const auto tx_start = REMAP_ADDRESS(slab_) + metadata_size;
    auto deserial = make_unsafe_deserializer(tx_start);
    return transaction::factory(deserial, hash_);
}

} // namespace database
} // namespace libbitcoin
