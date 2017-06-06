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
#ifndef LIBBITCOIN_DATABASE_DATA_BASE_HPP
#define LIBBITCOIN_DATABASE_DATA_BASE_HPP

#include <atomic>
#include <cstddef>
#include <memory>
#include <boost/filesystem.hpp>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/database/define.hpp>
#include <bitcoin/database/databases/block_database.hpp>
#include <bitcoin/database/databases/spend_database.hpp>
#include <bitcoin/database/databases/transaction_database.hpp>
#include <bitcoin/database/databases/history_database.hpp>
#include <bitcoin/database/databases/stealth_database.hpp>
#include <bitcoin/database/define.hpp>
#include <bitcoin/database/settings.hpp>
#include <bitcoin/database/store.hpp>

namespace libbitcoin {
namespace database {

/// This class is thread safe and implements the sequential locking pattern.
class BCD_API data_base
  : public store, noncopyable
{
public:
    typedef store::handle handle;
    typedef handle0 result_handler;
    typedef boost::filesystem::path path;

    data_base(const settings& settings);

    // Open and close.
    // ------------------------------------------------------------------------

    /// Create and open all databases.
    bool create(const chain::block& genesis);

    /// Open all databases.
    bool open() override;

    /// Close all databases.
    bool close() override;

    /// Call close on destruct.
    ~data_base();

    /// Reader interfaces.
    // ------------------------------------------------------------------------
    // These are const to preclude write operations.

    const block_database& blocks() const;

    const transaction_database& transactions() const;

    /// Invalid if indexes not initialized.
    const spend_database& spends() const;

    /// Invalid if indexes not initialized.
    const history_database& history() const;

    /// Invalid if indexes not initialized.
    const stealth_database& stealth() const;

    // Synchronous writers.
    // ------------------------------------------------------------------------

    /// Push unconfirmed tx that was verified with the given forks.
    code push(const chain::transaction& tx, uint32_t forks);

    /// Push next top block of expected height.
    code push(const chain::block& block, size_t height);

    /// Push next top header of expected height.
    code push(const chain::header& header, size_t height);

    /// Pop top block of expected height.
    code pop(chain::block& out_block, size_t height);

    /// Pop top header of expected height.
    code pop(chain::header& out_header, size_t height);

    // Reorganization.
    // ------------------------------------------------------------------------

    void reorganize(const config::checkpoint& fork_point,
        block_const_ptr_list_const_ptr incoming,
        block_const_ptr_list_ptr outgoing, dispatcher& dispatch,
        result_handler handler);

    void reorganize(const config::checkpoint& fork_point,
        header_const_ptr_list_const_ptr incoming,
        header_const_ptr_list_ptr outgoing, dispatcher& dispatch,
        result_handler handler);

protected:
    void start();
    void commit();
    bool flush() const override;

    // Utilities.
    // ------------------------------------------------------------------------

    code verify(const config::checkpoint& fork_point, bool block_index) const;
    code verify_top(size_t height, bool block_index) const;
    code verify_push(const chain::header& header, size_t height) const;
    code verify_push(const chain::block& block, size_t height) const;
    code verify_push(const chain::transaction& tx) const;
    chain::transaction::list to_transactions(const block_result& result) const;

    // Synchronous.
    // ------------------------------------------------------------------------

    bool push_transactions(const chain::block& block, size_t height, size_t bucket=0, size_t buckets=1, transaction_state state = transaction_state::confirmed);
    void push_inputs(const chain::transaction& tx, size_t height);
    void push_outputs(const chain::transaction& tx, size_t height);
    void push_stealth(const chain::transaction& tx, size_t height);

    bool pop_transactions(const chain::block& out_block, size_t bucket=0, size_t buckets=1);
    bool pop_inputs(const chain::transaction& tx);
    bool pop_outputs(const chain::transaction& tx);
    bool pop_stealth(const chain::transaction& tx);

    // Block Reorganization (push is parallel).
    // ------------------------------------------------------------------------

    void pop_above(block_const_ptr_list_ptr blocks,
        const config::checkpoint& fork_point, dispatcher& dispatch,
        result_handler handler);
    void handle_pop(const code& ec,
        block_const_ptr_list_const_ptr blocks, size_t fork_height,
        dispatcher& dispatch, result_handler handler);
    void push_all(block_const_ptr_list_const_ptr blocks, size_t fork_height,
        dispatcher& dispatch, result_handler handler);
    void push_next(const code& ec, block_const_ptr_list_const_ptr blocks,
        size_t index, size_t height, dispatcher& dispatch,
        result_handler handler);
    void handle_push(const code& ec, result_handler handler) const;

    void do_push(block_const_ptr block, size_t height, dispatcher& dispatch,
        result_handler handler);
    void do_push_transactions(block_const_ptr block, size_t height,
        size_t bucket, size_t buckets, result_handler handler);
    void handle_do_push_transactions(const code& ec, block_const_ptr block,
        size_t height, result_handler handler);

    // Header Reorganization (not parallel).
    // ------------------------------------------------------------------------

    bool pop_above(header_const_ptr_list_ptr headers,
        const config::checkpoint& fork_point, dispatcher& dispatch,
        result_handler handler);
    bool push_all(header_const_ptr_list_const_ptr headers,
        const config::checkpoint& fork_point, dispatcher& dispatch,
        result_handler handler);

    std::shared_ptr<block_database> blocks_;
    std::shared_ptr<transaction_database> transactions_;
    std::shared_ptr<spend_database> spends_;
    std::shared_ptr<history_database> history_;
    std::shared_ptr<stealth_database> stealth_;

private:
    typedef chain::input::list inputs;
    typedef chain::output::list outputs;

    std::atomic<bool> closed_;
    const settings& settings_;

    // Used to prevent concurrent unsafe writes.
    mutable shared_mutex write_mutex_;

    // Used to prevent concurrent file remapping.
    std::shared_ptr<shared_mutex> remap_mutex_;
};

} // namespace database
} // namespace libbitcoin

#endif
