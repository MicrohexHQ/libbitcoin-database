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
#include <bitcoin/database/data_base.hpp>

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <boost/filesystem.hpp>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/database/define.hpp>
#include <bitcoin/database/settings.hpp>
#include <bitcoin/database/store.hpp>

namespace libbitcoin {
namespace database {

using namespace std::placeholders;
using namespace boost::filesystem;
using namespace bc::chain;
using namespace bc::config;
using namespace bc::wallet;

#define NAME "data_base"

// TODO: remove spends store, replace with complex query, output gets inpoint:
// (1) transactions_.get(outpoint, require_confirmed)->spender_height.
// (2) blocks_.get(spender_height)->transactions().
// (3) (transactions()->inputs()->previous_output() == outpoint)->inpoint.
// This has the same average cost as 1 output-query + 1/2 block-query.
// This will reduce server indexing by 29% (address/stealth indexing only).
// Could make index optional, redirecting queries if not present.

// A failure after begin_write is returned without calling end_write.
// This leaves the local flush lock enabled, preventing usage after restart.

// Construct.
// ----------------------------------------------------------------------------

data_base::data_base(const settings& settings)
  : closed_(true),
    settings_(settings),
    remap_mutex_(std::make_shared<shared_mutex>()),
    store(settings.directory, settings.index_addresses, settings.flush_writes)
{
    LOG_DEBUG(LOG_DATABASE)
        << "Buckets: "
        << "block [" << settings.block_table_buckets << "], "
        << "transaction [" << settings.transaction_table_buckets << "], "
        << "spend [" << settings.spend_table_buckets << "], "
        << "history [" << settings.history_table_buckets << "]";
}

data_base::~data_base()
{
    close();
}

// Open and close.
// ----------------------------------------------------------------------------

// Throws if there is insufficient disk space, not idempotent.
bool data_base::create(const block& genesis)
{
    ///////////////////////////////////////////////////////////////////////////
    // Lock exclusive file access.
    if (!store::open())
        return false;

    // Create files.
    if (!store::create())
        return false;

    start();

    // These leave the databases open.
    auto created =
        blocks_->create() &&
        transactions_->create();

    if (use_indexes)
        created = created &&
            spends_->create() &&
            history_->create() &&
            stealth_->create();

    if (!created)
        return false;

    // Store and index the first header/block.
    created = push(genesis.header(), 0) == error::success &&
        push(genesis, 0) == error::success;

    closed_ = false;
    return created;
}

// Must be called before performing queries, not idempotent.
// May be called after stop and/or after close in order to reopen.
bool data_base::open()
{
    ///////////////////////////////////////////////////////////////////////////
    // Lock exclusive file access and conditionally the global flush lock.
    if (!store::open())
        return false;

    start();

    auto opened =
        blocks_->open() &&
        transactions_->open();

    if (use_indexes)
        opened = opened &&
            spends_->open() &&
            history_->open() &&
            stealth_->open();

    closed_ = false;
    return opened;
}

// protected
void data_base::start()
{
    blocks_ = std::make_shared<block_database>(block_table, header_index,
        block_index, transaction_index, settings_.block_table_buckets,
        settings_.file_growth_rate, remap_mutex_);

    transactions_ = std::make_shared<transaction_database>(transaction_table,
        settings_.transaction_table_buckets, settings_.file_growth_rate,
        settings_.cache_capacity, remap_mutex_);

    if (use_indexes)
    {
        spends_ = std::make_shared<spend_database>(spend_table,
            settings_.spend_table_buckets, settings_.file_growth_rate,
            remap_mutex_);

        history_ = std::make_shared<history_database>(history_table,
            history_rows, settings_.history_table_buckets,
            settings_.file_growth_rate, remap_mutex_);

        stealth_ = std::make_shared<stealth_database>(stealth_rows,
            settings_.file_growth_rate, remap_mutex_);
    }
}

// protected
void data_base::commit()
{
    if (use_indexes)
    {
        spends_->commit();
        history_->commit();
        stealth_->commit();
    }

    transactions_->commit();
    blocks_->commit();
}

// protected
bool data_base::flush() const
{
    // Avoid a race between flush and close whereby flush is skipped because
    // close is true and therefore the flush lock file is deleted before close
    // fails. This would leave the database corrupted and undetected. The flush
    // call must execute and successfully flush or the lock must remain.
    ////if (closed_)
    ////    return true;

    auto flushed =
        blocks_->flush() &&
        transactions_->flush();

    if (use_indexes)
        flushed = flushed &&
            spends_->flush() &&
            history_->flush() &&
            stealth_->flush();

    LOG_DEBUG(LOG_DATABASE)
        << "Write flushed to disk: "
        << code(flushed ? error::success : error::operation_failed).message();

    return flushed;
}

// Close is idempotent and thread safe.
// Optional as the database will close on destruct.
bool data_base::close()
{
    if (closed_)
        return true;

    closed_ = true;

    auto closed =
        blocks_->close() &&
        transactions_->close();

    if (use_indexes)
        closed = closed &&
        spends_->close() &&
        history_->close() &&
        stealth_->close();

    return closed && store::close();
    // Unlock exclusive file access and conditionally the global flush lock.
    ///////////////////////////////////////////////////////////////////////////
}

// Reader interfaces.
// ----------------------------------------------------------------------------
// public

const block_database& data_base::blocks() const
{
    return *blocks_;
}

const transaction_database& data_base::transactions() const
{
    return *transactions_;
}

// Invalid if indexes not initialized.
const spend_database& data_base::spends() const
{
    return *spends_;
}

// Invalid if indexes not initialized.
const history_database& data_base::history() const
{
    return *history_;
}

// Invalid if indexes not initialized.
const stealth_database& data_base::stealth() const
{
    return *stealth_;
}

// Synchronous writers.
// ----------------------------------------------------------------------------
// public

// TODO: enable promotion from any unconfirmed state to pooled.
// This expects tx is validated, unconfirmed and not yet stored.
code data_base::push(const transaction& tx, uint32_t forks)
{
    static const auto unconfirmed = transaction_result::unconfirmed;

    // Critical Section
    ///////////////////////////////////////////////////////////////////////////
    unique_lock lock(write_mutex_);

    // Returns error::unspent_duplicate if an unspent tx with same hash exists.
    const auto ec = verify_push(tx);

    if (ec)
        return ec;

    //vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
    if (!begin_write())
        return error::operation_failed;

    // When position is unconfirmed, height is used to store validation forks.
    transactions_->store(tx, forks, unconfirmed, transaction_state::pooled);
    transactions_->commit();

    return end_write() ? error::success : error::operation_failed;
    //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    ///////////////////////////////////////////////////////////////////////////
}

// TODO: enable promotion from any unconfirmed state (to confirmed).
// This expects header is validated and not yet stored.
code data_base::push(const header& header, size_t height)
{
    // Critical Section
    ///////////////////////////////////////////////////////////////////////////
    unique_lock lock(write_mutex_);

    const auto ec = verify_push(header, height);

    if (ec)
        return ec;

    //vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
    if (!begin_write())
        return error::operation_failed;

    blocks_->store(header, height);
    blocks_->commit();

    return end_write() ? error::success : error::operation_failed;
    //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    ///////////////////////////////////////////////////////////////////////////
}

// TODO: enable promotion from any unconfirmed state (to indexed).
// This expects block is validated, header is not yet stored.
code data_base::push(const block& block, size_t height)
{
    // Critical Section
    ///////////////////////////////////////////////////////////////////////////
    unique_lock lock(write_mutex_);

    const auto ec = verify_push(block, height);

    if (ec)
        return ec;

    //vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
    if (!begin_write())
        return error::operation_failed;

    // Pushes transactions sequentially as confirmed.
    if (!push_transactions(block, height))
        return error::operation_failed;

    blocks_->store(block, height);
    commit();

    return end_write() ? error::success : error::operation_failed;
    //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    ///////////////////////////////////////////////////////////////////////////
}

// This expects block exists at the top of the block index.
code data_base::pop(chain::block& out_block, size_t height)
{
    // Critical Section
    ///////////////////////////////////////////////////////////////////////////
    unique_lock lock(write_mutex_);

    const auto ec = verify_top(height, true);

    if (ec)
        return ec;

    const auto result = blocks_->get(height, true);

    if (!result)
        return error::operation_failed;

    // Create a block for walking transactions and return.
    out_block = chain::block(result.header(), to_transactions(result));

    //vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
    if (!begin_write())
        return error::operation_failed;

    if (!pop_transactions(out_block))
        return error::operation_failed;

    if (!blocks_->unconfirm(height, true))
        return error::operation_failed;

    // Commit everything that was changed.
    commit();

    BITCOIN_ASSERT(out_block.is_valid());
    return end_write() ? error::success : error::operation_failed;
    //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    ///////////////////////////////////////////////////////////////////////////
}

// This expects header exists at the top of the header index.
code data_base::pop(chain::header& out_header, size_t height)
{
    // Critical Section
    ///////////////////////////////////////////////////////////////////////////
    unique_lock lock(write_mutex_);

    const auto ec = verify_top(height, false);

    if (ec)
        return ec;

    const auto result = blocks_->get(height, false);

    if (!result)
        return error::operation_failed;

    // Create a block for walking transactions.
    const chain::block block(result.header(), to_transactions(result));

    //vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
    if (!begin_write())
        return error::operation_failed;

    if (!pop_transactions(block))
        return error::operation_failed;

    if (!blocks_->unconfirm(height, false))
        return error::operation_failed;

    // Commit everything that was changed.
    commit();

    out_header = block.header();
    BITCOIN_ASSERT(out_header.is_valid());
    return end_write() ? error::success : error::operation_failed;
    //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    ///////////////////////////////////////////////////////////////////////////
}

// Utilities.
// ----------------------------------------------------------------------------
// protected

static size_t get_next_block(const block_database& blocks,
    bool block_index)
{
    size_t current_height;
    const auto empty_chain = !blocks.top(current_height, block_index);
    return empty_chain ? 0 : current_height + 1;
}

static hash_digest get_previous_block(const block_database& blocks,
    size_t height, bool block_index)
{
    return height == 0 ? null_hash : 
        blocks.get(height - 1, block_index).hash();
}

code data_base::verify_top(size_t height, bool block_index) const
{
    size_t actual_height;
    return blocks_->top(actual_height, block_index) &&
        (actual_height == height) ? error::success : error::operation_failed;
}

code data_base::verify(const checkpoint& fork_point, bool block_index) const
{
    const auto result = blocks_->get(fork_point.hash());
    if (!result || fork_point.height() != result.height())
        return error::operation_failed;

    const auto state = result.state();
    if (!is_confirmed(state) && (block_index || !is_indexed(state)))
        return  error::operation_failed;

    return error::success;
}

// This store-level check is a failsafe for blockchain behavior.
code data_base::verify_push(const header& header, size_t height) const
{
    if (get_next_block(blocks(), false) != height)
        return error::store_block_invalid_height;

    if (get_previous_block(blocks(), height, false) !=
        header.previous_block_hash())
        return error::store_block_missing_parent;

    return error::success;
}

// This store-level check is a failsafe for blockchain behavior.
code data_base::verify_push(const block& block, size_t height) const
{
    if (block.transactions().empty())
        return error::empty_block;

    if (get_next_block(blocks(), true) != height)
        return error::store_block_invalid_height;

    if (get_previous_block(blocks(), height, true) !=
        block.header().previous_block_hash())
        return error::store_block_missing_parent;

    return error::success;
}

// This store-level check is a failsafe for blockchain behavior.
code data_base::verify_push(const transaction& tx) const
{
    const auto result = transactions_->get(tx.hash());

    // This is an expensive re-check, but only if a duplicate exists.
    if (result && !result.is_spent())
        return error::unspent_duplicate;

    return error::success;
}

// TODO: could move into block_result but there is tx store reference.
transaction::list data_base::to_transactions(const block_result& result) const
{
    transaction::list txs;
    txs.reserve(result.transaction_count());

    for (const auto offset: result.transaction_offsets())
    {
        // TODO: change tx.get(...) to always populate offset.
        const auto result = transactions_->get(offset);
        BITCOIN_ASSERT(static_cast<bool>(result));
        txs.push_back(result.transaction());
        txs.back().validation.offset = offset;
    }

    return txs;
}

// Synchronous transaction writers.
// ----------------------------------------------------------------------------
// protected

// A false return implies store corruption.
// To push in order call with bucket = 0 and buckets = 1 (defaults).
bool data_base::push_transactions(const block& block, size_t height,
    size_t bucket, size_t buckets, transaction_state state)
{
    BITCOIN_ASSERT(bucket < buckets);
    const auto& txs = block.transactions();
    const auto count = txs.size();

    for (auto position = bucket; position < count;
        position = ceiling_add(position, buckets))
    {
        const auto& tx = txs[position];

        if (!transactions_->store(tx, height, position, state))
            return false;

        if (settings_.index_addresses)
        {
            push_inputs(tx, height);
            push_outputs(tx, height);
            push_stealth(tx, height);
        }
    }

    return true;
}

void data_base::push_inputs(const transaction& tx, size_t height)
{
    if (tx.is_coinbase())
        return;

    const auto hash = tx.hash();
    const auto& inputs = tx.inputs();

    for (uint32_t index = 0; index < inputs.size(); ++index)
    {
        const input_point point{ hash, index };
        const auto& input = inputs[index];
        const auto& prevout = input.previous_output();
        const auto checksum = prevout.checksum();

        // TODO: eliminate spend index table optimization.
        spends_->store(prevout, point);

        // If the prevout can be required here then this is better than input
        // extraction because we get pay_multisig and pay_public_key spends.
        // Could make it optional, using if available.
        ////BITCOIN_ASSERT(prevout.validation);
        ////BITCOIN_ASSERT(prevout.validation.cache.is_valid());
        ////for (const auto& address: prevout.validation.cache.addresses())

        for (const auto& address: input.addresses())
            history_->store(address.hash(),
            {
                height,
                point,
                checksum
            });
    }
}

void data_base::push_outputs(const transaction& tx, size_t height)
{
    const auto hash = tx.hash();
    const auto& outputs = tx.outputs();

    for (uint32_t index = 0; index < outputs.size(); ++index)
    {
        const auto point = output_point{ hash, index };
        const auto& output = outputs[index];
        const auto value = output.value();

        for (const auto& address: output.addresses())
            history_->store(address.hash(),
            {
                height,
                point,
                value
            });
    }
}

void data_base::push_stealth(const transaction& tx, size_t height)
{
    const auto hash = tx.hash();
    const auto& outputs = tx.outputs();

    // Protected loop termination.
    if (outputs.empty())
        return;

    // Stealth outputs are paired by convention.
    for (size_t index = 0; index < (outputs.size() - 1); ++index)
    {
        const auto& ephemeral_script = outputs[index].script();
        const auto& payment_output = outputs[index + 1];

        // Try to extract the payment address from the second output.
        auto address = payment_output.address();
        if (!address)
            continue;

        // Try to extract an unsigned ephemeral key from the first output.
        hash_digest unsigned_ephemeral_key;
        if (!extract_ephemeral_key(unsigned_ephemeral_key, ephemeral_script))
            continue;

        // Try to extract a stealth prefix from the first output.
        uint32_t prefix;
        if (!to_stealth_prefix(prefix, ephemeral_script))
            continue;

        // The payment address versions are arbitrary and unused here.
        stealth_->store(
        {
            height,
            prefix,
            unsigned_ephemeral_key,
            address.hash(),
            hash
        });
    }
}

// A false return implies store corruption.
// To pop in order call with bucket = 0 and buckets = 1 (defaults).
bool data_base::pop_transactions(const block& block, size_t bucket,
    size_t buckets)
{
    BITCOIN_ASSERT(bucket < buckets);
    const auto& txs = block.transactions();
    const auto count = txs.size();

    for (auto position = bucket; position < count;
        position = ceiling_add(position, buckets))
    {
        const auto& tx = txs[position];

        if (!transactions_->pool(tx))
            return false;

        if (settings_.index_addresses)
        {
            if (!pop_inputs(tx) ||
                !pop_outputs(tx) ||
                !pop_stealth(tx))
                return false;
        }
    }

    return true;
}

// A false return implies store corruption.
bool data_base::pop_inputs(const chain::transaction& tx)
{
    if (!settings_.index_addresses || tx.is_coinbase())
        return true;

    const auto& inputs = tx.inputs();

    // TODO: eliminate spend index table optimization.
    for (auto input = inputs.begin(); input != inputs.end(); ++input)
        if (!spends_->unlink(input->previous_output()))
            return false;

    for (auto input = inputs.begin(); input != inputs.end(); ++input)
        for (const auto& address: input->addresses())
            if (!history_->unlink_last_row(address.hash()))
                return false;

    return true;
}

// A false return implies store corruption.
bool data_base::pop_outputs(const chain::transaction& tx)
{
    if (!settings_.index_addresses)
        return true;

    const auto& outputs = tx.outputs();

    for (auto output = outputs.begin(); output != outputs.end(); ++output)
        for (const auto& address: output->addresses())
            if (!history_->unlink_last_row(address.hash()))
                return false;

    return true;
}

// A false return implies store corruption.
bool data_base::pop_stealth(const chain::transaction& tx)
{
    // Stealth unlink is not implemented, there is no way to correlate.
    return true;
}

// Block reorganization.
// ----------------------------------------------------------------------------

void data_base::reorganize(const config::checkpoint& fork_point,
    block_const_ptr_list_const_ptr incoming,
    block_const_ptr_list_ptr outgoing, dispatcher& dispatch,
    result_handler handler)
{
    if (fork_point.height() > max_size_t - incoming->size())
    {
        handler(error::operation_failed);
        return;
    }

    const result_handler pop_handler =
        std::bind(&data_base::handle_pop,
            this, _1, incoming, fork_point.height(), std::ref(dispatch),
                handler);

    // Critical Section.
    ///////////////////////////////////////////////////////////////////////////
    write_mutex_.lock();

    //vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
    if (!begin_write())
    {
        pop_handler(error::operation_failed);
        return;
    }

    pop_above(outgoing, fork_point, dispatch, pop_handler);
}

// TODO: make async.
// This precludes popping the genesis block.
void data_base::pop_above(block_const_ptr_list_ptr blocks,
    const checkpoint& fork_point, dispatcher& dispatch, result_handler handler)
{
    auto ec = verify(fork_point, true);

    if (ec)
    {
        handler(ec);
        return;
    }

    size_t top;
    if (!blocks_->top(top, true))
    {
        handler(error::operation_failed);
        return;
    }

    const auto fork = fork_point.height();
    const auto depth = top - fork;

    if (depth == 0)
    {
        handler(error::success);
        return;
    }

    blocks->clear();
    blocks->reserve(depth);

    for (size_t height = top; height > fork; --height)
    {
        const auto start_time = asio::steady_clock::now();
        const auto next = std::make_shared<message::block>();

        // TODO: parallelize tx pop.
        if ((ec = pop(*next, height)))
        {
            handler(ec);
            return;
        }

        blocks->insert(blocks->begin(), next);
        next->validation.start_pop = start_time;
        next->header().validation.height = height;
    }

    // This is the beginning of the push_all sequence.
    handler(error::success);
}

// TODO: pop_next().

void data_base::handle_pop(const code& ec,
    block_const_ptr_list_const_ptr blocks, size_t fork_height, dispatcher& dispatch,
    result_handler handler)
{
    const result_handler push_handler =
        std::bind(&data_base::handle_push,
            this, _1, handler);

    if (ec)
    {
        push_handler(ec);
        return;
    }

    push_all(blocks, fork_height, dispatch, push_handler);
}

void data_base::push_all(block_const_ptr_list_const_ptr blocks,
    size_t fork_height, dispatcher& dispatch, result_handler handler)
{
    push_next(error::success, blocks, 0, fork_height + 1, dispatch, handler);
}

// This controls the asynchronous block push loop.
void data_base::push_next(const code& ec,
    block_const_ptr_list_const_ptr blocks, size_t index, size_t height,
    dispatcher& dispatch, result_handler handler)
{
    if (ec || index >= blocks->size())
    {
        // This ends the loop.
        handler(ec);
        return;
    }

    const auto block = (*blocks)[index];
    block->validation.start_push = asio::steady_clock::now();

    const result_handler next_handler =
        std::bind(&data_base::push_next,
            this, _1, blocks, index + 1, height + 1, std::ref(dispatch),
                handler);

    // This is the start of the parallel block sub-sequence.
    dispatch.concurrent(&data_base::do_push,
        this, block, height, std::ref(dispatch), next_handler);
}

// We never invoke the caller's handler under the mutex, we never fail to clear
// the mutex, and we always invoke the caller's handler exactly once.
void data_base::handle_push(const code& ec, result_handler handler) const
{
    write_mutex_.unlock();
    // End Critical Section.
    ///////////////////////////////////////////////////////////////////////////

    if (ec)
    {
        handler(ec);
        return;
    }

    handler(end_write() ? error::success : error::operation_failed);
    //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
}

// Block push (parallel by tx).
// ----------------------------------------------------------------------------

void data_base::do_push(block_const_ptr block, size_t height,
    dispatcher& dispatch, result_handler handler)
{
    result_handler block_complete =
        std::bind(&data_base::handle_do_push_transactions,
            this, _1, block, height, handler);

    const auto ec = verify_push(*block, height);

    if (ec)
    {
        block_complete(ec);
        return;
    }

    const auto threads = dispatch.size();
    const auto buckets = std::min(threads, block->transactions().size());
    BITCOIN_ASSERT(buckets != 0);

    const auto join_handler = bc::synchronize(std::move(block_complete),
        buckets, NAME "_do_push");

    for (size_t bucket = 0; bucket < buckets; ++bucket)
        dispatch.concurrent(&data_base::do_push_transactions,
            this, block, height, bucket, buckets, join_handler);
}

void data_base::do_push_transactions(block_const_ptr block, size_t height,
    size_t bucket, size_t buckets, result_handler handler)
{
    const auto result = push_transactions(*block, height, bucket, buckets);
    handler(result ? error::success : error::operation_failed);
}

void data_base::handle_do_push_transactions(const code& ec,
    block_const_ptr block, size_t height, result_handler handler)
{
    if (ec)
    {
        handler(ec);
        return;
    }

    blocks_->store(*block, height);
    commit();

    block->validation.end_push = asio::steady_clock::now();

    // This is the end of the parallel block sub-sequence.
    handler(error::success);
}

// Header reorganization.
// ----------------------------------------------------------------------------

// TODO: make async.
// A false return implies store corruption.
void data_base::reorganize(const config::checkpoint& fork_point,
    header_const_ptr_list_const_ptr incoming,
    header_const_ptr_list_ptr outgoing, dispatcher& dispatch,
    result_handler handler)
{
    if (fork_point.height() > max_size_t - incoming->size())
    {
        handler(error::operation_failed);
        return;
    }

    const auto result =
        pop_above(outgoing, fork_point, dispatch, handler) &&
        push_all(incoming, fork_point, dispatch, handler);

    handler(result ? error::success : error::operation_failed);
}

// TODO: make async.
// A false return implies store corruption.
bool data_base::pop_above(header_const_ptr_list_ptr headers,
    const checkpoint& fork_point, dispatcher& dispatch, result_handler handler)
{
    auto ec = verify(fork_point, false);

    if (ec)
        return false;

    size_t top;
    if (!blocks_->top(top, false))
        return false;

    const auto fork = fork_point.height();
    const auto depth = top - fork;

    if (depth == 0)
        return true;

    headers->clear();
    headers->reserve(depth);

    for (size_t height = top; height > fork; --height)
    {
        const auto next = std::make_shared<message::header>();

        // TODO: parallelize tx pop.
        if ((ec = pop(*next, height)))
            return false;

        headers->insert(headers->begin(), next);
        next->validation.height = height;
    }

    return true;
}

// TODO: make async.
// A false return implies store corruption.
bool data_base::push_all(header_const_ptr_list_const_ptr headers,
    const checkpoint& fork_point, dispatcher& dispatch, result_handler handler)
{
    code ec;
    const auto first_height = fork_point.height() + 1;

    for (size_t index = 0; index < headers->size(); ++index)
    {
        const auto next = (*headers)[index];

        // TODO: parallelize tx push.
        if ((ec = push(*next, first_height + index)))
            return false;
    }

    return true;
}

} // namespace database
} // namespace libbitcoin
