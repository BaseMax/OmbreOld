// Copyright (c) 2018, The OMBRE Project
// Copyright (c) 2014-2017, The Monero Project
// Copyright (c) 2017, SUMOKOIN
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Parts of this file are originally copyright (c) 2012-2013 The Cryptonote developers

#include <algorithm>
#include <cstdio>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/filesystem.hpp>

#include "include_base_utils.h"
#include "cryptonote_basic_impl.h"
#include "tx_pool.h"
#include "blockchain.h"
#include "blockchain_db/blockchain_db.h"
#include "cryptonote_format_utils.h"
#include "cryptonote_boost_serialization.h"
#include "cryptonote_config.h"
#include "miner.h"
#include "misc_language.h"
#include "profile_tools.h"
#include "file_io_utils.h"
#include "common/int-util.h"
#include "common/boost_serialization_helper.h"
#include "warnings.h"
#include "crypto/hash.h"
#include "cryptonote_core/checkpoints.h"
#include "cryptonote_core/cryptonote_core.h"
#include "ringct/rctSigs.h"
#include "common/perf_timer.h"
#if defined(PER_BLOCK_CHECKPOINT)
#include "blocks/blocks.h"
#endif

//#include "serialization/json_archive.h"

/* TODO:
 *  Clean up code:
 *    Possibly change how outputs are referred to/indexed in blockchain and wallets
 *
 */

using namespace cryptonote;
using epee::string_tools::pod_to_hex;

DISABLE_VS_WARNINGS(4267)

// used to overestimate the block reward when estimating a per kB to use
#define BLOCK_REWARD_OVERESTIMATE   ((uint64_t)(16000000000))
#define MAINNET_HARDFORK_V3_HEIGHT  ((uint64_t)(116520))

static const struct {
  uint8_t version;
  uint64_t height;
  uint8_t threshold;
  time_t time;
} mainnet_hard_forks[] = {
  { 1, 1, 0, 1482806500 },
  { 2, 21301, 0, 1497657600 },
  { 3, 72001, 0, 1524577218 }, // Roughly the 20th of April.
  { 4, 208500, 0, 1531762611 } // Roughly the 23rd of July.
};
static const uint64_t mainnet_hard_fork_version_1_till = (uint64_t)-1;

static const struct {
  uint8_t version;
  uint64_t height;
  uint8_t threshold;
  time_t time;
} testnet_hard_forks[] = {
  { 1, 1, 0, 1482806500 },
  { 2, 6, 0, 1497181713 },
  { 3, 7, 0, 1522597016 },
  { 4, 8, 0, 1522597017 }
};
static const uint64_t testnet_hard_fork_version_1_till = (uint64_t)-1;

//------------------------------------------------------------------
Blockchain::Blockchain(tx_memory_pool& tx_pool) :
  m_db(), m_tx_pool(tx_pool), m_hardfork(NULL), m_timestamps_and_difficulties_height(0), m_current_block_cumul_sz_limit(0), m_is_in_checkpoint_zone(false),
  m_is_blockchain_storing(false), m_enforce_dns_checkpoints(false), m_max_prepare_blocks_threads(4), m_db_blocks_per_sync(1), m_db_sync_mode(db_async), m_fast_sync(true), m_show_time_stats(false), m_sync_counter(0), m_cancel(false)
{
  LOG_PRINT_L3("Blockchain::" << __func__);
}
//------------------------------------------------------------------
bool Blockchain::have_tx(const crypto::hash &id) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);
  return m_db->tx_exists(id);
}
//------------------------------------------------------------------
bool Blockchain::have_tx_keyimg_as_spent(const crypto::key_image &key_im) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);
  return  m_db->has_key_image(key_im);
}
//------------------------------------------------------------------
// This function makes sure that each "input" in an input (mixins) exists
// and collects the public key for each from the transaction it was included in
// via the visitor passed to it.
template <class visitor_t>
bool Blockchain::scan_outputkeys_for_indexes(size_t tx_version, const txin_to_key& tx_in_to_key, visitor_t &vis, const crypto::hash &tx_prefix_hash, uint64_t* pmax_related_block_height) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);

  // ND: Disable locking and make method private.
  //CRITICAL_REGION_LOCAL(m_blockchain_lock);

  // verify that the input has key offsets (that it exists properly, really)
  if(!tx_in_to_key.key_offsets.size())
    return false;

  // cryptonote_format_utils uses relative offsets for indexing to the global
  // outputs list.  that is to say that absolute offset #2 is absolute offset
  // #1 plus relative offset #2.
  // TODO: Investigate if this is necessary / why this is done.
  std::vector<uint64_t> absolute_offsets = relative_output_offsets_to_absolute(tx_in_to_key.key_offsets);
  std::vector<output_data_t> outputs;

  bool found = false;
  auto it = m_scan_table.find(tx_prefix_hash);
  if (it != m_scan_table.end())
  {
    auto its = it->second.find(tx_in_to_key.k_image);
    if (its != it->second.end())
    {
      outputs = its->second;
      found = true;
    }
  }

  if (!found)
  {
    try
    {
      m_db->get_output_key(tx_in_to_key.amount, absolute_offsets, outputs);
    }
    catch (...)
    {
      LOG_PRINT_L0("Output does not exist! amount = " << tx_in_to_key.amount);
      return false;
    }
  }
  else
  {
    // check for partial results and add the rest if needed;
    if (outputs.size() < absolute_offsets.size() && outputs.size() > 0)
    {
      LOG_PRINT_L1("Additional outputs needed: " << absolute_offsets.size() - outputs.size());
      std::vector < uint64_t > add_offsets;
      std::vector<output_data_t> add_outputs;
      for (size_t i = outputs.size(); i < absolute_offsets.size(); i++)
        add_offsets.push_back(absolute_offsets[i]);
      try
      {
        m_db->get_output_key(tx_in_to_key.amount, add_offsets, add_outputs);
      }
      catch (...)
      {
        LOG_PRINT_L0("Output does not exist! amount = " << tx_in_to_key.amount);
        return false;
      }
      outputs.insert(outputs.end(), add_outputs.begin(), add_outputs.end());
    }
  }

  size_t count = 0;
  for (const uint64_t& i : absolute_offsets)
  {
    try
    {
      output_data_t output_index;
      try
      {
        // get tx hash and output index for output
        if (count < outputs.size())
          output_index = outputs.at(count);
        else
          output_index = m_db->get_output_key(tx_in_to_key.amount, i);

        // call to the passed boost visitor to grab the public key for the output
        if (!vis.handle_output(output_index.unlock_time, output_index.pubkey, output_index.commitment))
        {
          LOG_PRINT_L0("Failed to handle_output for output no = " << count << ", with absolute offset " << i);
          return false;
        }
      }
      catch (...)
      {
        LOG_PRINT_L0("Output does not exist! amount = " << tx_in_to_key.amount << ", absolute_offset = " << i);
        return false;
      }

      // if on last output and pmax_related_block_height not null pointer
      if(++count == absolute_offsets.size() && pmax_related_block_height)
      {
        // set *pmax_related_block_height to tx block height for this output
        auto h = output_index.height;
        if(*pmax_related_block_height < h)
        {
          *pmax_related_block_height = h;
        }
      }

    }
    catch (const OUTPUT_DNE& e)
    {
      LOG_PRINT_L0("Output does not exist: " << e.what());
      return false;
    }
    catch (const TX_DNE& e)
    {
      LOG_PRINT_L0("Transaction does not exist: " << e.what());
      return false;
    }

  }

  return true;
}
//------------------------------------------------------------------
uint64_t Blockchain::get_current_blockchain_height() const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);
  return m_db->height();
}
//------------------------------------------------------------------
//FIXME: possibly move this into the constructor, to avoid accidentally
//       dereferencing a null BlockchainDB pointer
bool Blockchain::init(BlockchainDB* db, const bool testnet, const cryptonote::test_options *test_options)
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);

  bool fakechain = test_options != NULL;

  if (db == nullptr)
  {
    LOG_ERROR("Attempted to init Blockchain with null DB");
    return false;
  }
  if (!db->is_open())
  {
    LOG_ERROR("Attempted to init Blockchain with unopened DB");
    return false;
  }

  m_db = db;

  m_testnet = testnet;
  if (m_hardfork == nullptr)
  {
    if (fakechain)
      m_hardfork = new HardFork(*db, 1, 0);
    else if (m_testnet)
      m_hardfork = new HardFork(*db, 1, testnet_hard_fork_version_1_till);
    else
      m_hardfork = new HardFork(*db, 1, mainnet_hard_fork_version_1_till);
  }
  if (fakechain)
  {
    for (size_t n = 0; test_options->hard_forks[n].first; ++n)
      m_hardfork->add_fork(test_options->hard_forks[n].first, test_options->hard_forks[n].second, 0, n + 1);
  }
  else if (m_testnet)
  {
    for (size_t n = 0; n < sizeof(testnet_hard_forks) / sizeof(testnet_hard_forks[0]); ++n)
      m_hardfork->add_fork(testnet_hard_forks[n].version, testnet_hard_forks[n].height, testnet_hard_forks[n].threshold, testnet_hard_forks[n].time);
  }
  else
  {
    for (size_t n = 0; n < sizeof(mainnet_hard_forks) / sizeof(mainnet_hard_forks[0]); ++n)
      m_hardfork->add_fork(mainnet_hard_forks[n].version, mainnet_hard_forks[n].height, mainnet_hard_forks[n].threshold, mainnet_hard_forks[n].time);
  }
  m_hardfork->init();

  m_db->set_hard_fork(m_hardfork);

  // if the blockchain is new, add the genesis block
  // this feels kinda kludgy to do it this way, but can be looked at later.
  // TODO: add function to create and store genesis block,
  //       taking testnet into account
  if(!m_db->height())
  {
    LOG_PRINT_L0("Blockchain not loaded, generating genesis block.");
    block bl = boost::value_initialized<block>();
    block_verification_context bvc = boost::value_initialized<block_verification_context>();
    if (m_testnet)
    {
      generate_genesis_block(bl, config::testnet::GENESIS_TX, config::testnet::GENESIS_NONCE);
    }
    else
    {
      generate_genesis_block(bl, config::GENESIS_TX, config::GENESIS_NONCE);
    }
    add_new_block(bl, bvc);
    CHECK_AND_ASSERT_MES(!bvc.m_verifivation_failed, false, "Failed to add genesis block to blockchain");
  }
  // TODO: if blockchain load successful, verify blockchain against both
  //       hard-coded and runtime-loaded (and enforced) checkpoints.
  else
  {
  }

  if (!fakechain)
  {
    // ensure we fixup anything we found and fix in the future
    m_db->fixup();
  }

  m_db->block_txn_start(true);
  // check how far behind we are
  uint64_t top_block_timestamp = m_db->get_top_block_timestamp();
  uint64_t timestamp_diff = time(NULL) - top_block_timestamp;

  // genesis block has no timestamp, could probably change it to have timestamp of 1341378000...
  if(!top_block_timestamp)
    timestamp_diff = time(NULL) - 1341378000;

  // create general purpose async service queue

  m_async_work_idle = std::unique_ptr < boost::asio::io_service::work > (new boost::asio::io_service::work(m_async_service));
  // we only need 1
  m_async_pool.create_thread(boost::bind(&boost::asio::io_service::run, &m_async_service));

#if defined(PER_BLOCK_CHECKPOINT)
  if (!fakechain)
    load_compiled_in_block_hashes();
#endif

  LOG_PRINT_GREEN("Blockchain initialized. last block: " << m_db->height() - 1 << ", " << epee::misc_utils::get_time_interval_string(timestamp_diff) << " time ago, current difficulty: " << get_difficulty_for_next_block(), LOG_LEVEL_0);
  m_db->block_txn_stop();

  return true;
}
//------------------------------------------------------------------
bool Blockchain::init(BlockchainDB* db, HardFork*& hf, const bool testnet)
{
  if (hf != nullptr)
    m_hardfork = hf;
  bool res = init(db, testnet, NULL);
  if (hf == nullptr)
    hf = m_hardfork;
  return res;
}
//------------------------------------------------------------------
bool Blockchain::store_blockchain()
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  // lock because the rpc_thread command handler also calls this
  CRITICAL_REGION_LOCAL(m_db->m_synchronization_lock);

  TIME_MEASURE_START(save);
  // TODO: make sure sync(if this throws that it is not simply ignored higher
  // up the call stack
  try
  {
    m_db->sync();
  }
  catch (const std::exception& e)
  {
    LOG_PRINT_L0(std::string("Error syncing blockchain db: ") + e.what() + "-- shutting down now to prevent issues!");
    throw;
  }
  catch (...)
  {
    LOG_PRINT_L0("There was an issue storing the blockchain, shutting down now to prevent issues!");
    throw;
  }

  TIME_MEASURE_FINISH(save);
  if(m_show_time_stats)
    LOG_PRINT_L0("Blockchain stored OK, took: " << save << " ms");
  return true;
}
//------------------------------------------------------------------
bool Blockchain::deinit()
{
  LOG_PRINT_L3("Blockchain::" << __func__);

  LOG_PRINT_L1("Stopping blockchain read/write activity");

 // stop async service
  m_async_work_idle.reset();
  m_async_pool.join_all();
  m_async_service.stop();

  // as this should be called if handling a SIGSEGV, need to check
  // if m_db is a NULL pointer (and thus may have caused the illegal
  // memory operation), otherwise we may cause a loop.
  if (m_db == NULL)
  {
    throw new DB_ERROR("The db pointer is null in Blockchain, the blockchain may be corrupt!");
  }

  try
  {
    m_db->close();
    LOG_PRINT_L1("Local blockchain read/write activity stopped successfully");
  }
  catch (const std::exception& e)
  {
    LOG_ERROR(std::string("Error closing blockchain db: ") + e.what());
  }
  catch (...)
  {
    LOG_ERROR("There was an issue closing/storing the blockchain, shutting down now to prevent issues!");
  }

  delete m_hardfork;
  delete m_db;
  return true;
}
//------------------------------------------------------------------
// This function tells BlockchainDB to remove the top block from the
// blockchain and then returns all transactions (except the miner tx, of course)
// from it to the tx_pool
block Blockchain::pop_block_from_blockchain()
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);

  m_timestamps_and_difficulties_height = 0;

  block popped_block;
  std::vector<transaction> popped_txs;

  try
  {
    m_db->pop_block(popped_block, popped_txs);
  }
  // anything that could cause this to throw is likely catastrophic,
  // so we re-throw
  catch (const std::exception& e)
  {
    LOG_ERROR("Error popping block from blockchain: " << e.what());
    throw;
  }
  catch (...)
  {
    LOG_ERROR("Error popping block from blockchain, throwing!");
    throw;
  }

  // return transactions from popped block to the tx_pool
  for (transaction& tx : popped_txs)
  {
    if (!is_coinbase(tx))
    {
      cryptonote::tx_verification_context tvc = AUTO_VAL_INIT(tvc);

      // FIXME: HardFork
      // Besides the below, popping a block should also remove the last entry
      // in hf_versions.
      //
      // FIXME: HardFork
      // This is not quite correct, as we really want to add the txes
      // to the pool based on the version determined after all blocks
      // are popped.
      uint8_t version = get_current_hard_fork_version();

      // We assume that if they were in a block, the transactions are already
      // known to the network as a whole. However, if we had mined that block,
      // that might not be always true. Unlikely though, and always relaying
      // these again might cause a spike of traffic as many nodes re-relay
      // all the transactions in a popped block when a reorg happens.
      bool r = m_tx_pool.add_tx(tx, tvc, true, true, version);
      if (!r)
      {
        LOG_ERROR("Error returning transaction to tx_pool");
      }
    }
  }
  update_next_cumulative_size_limit();
  m_tx_pool.on_blockchain_dec(m_db->height()-1, get_tail_id());

  return popped_block;
}
//------------------------------------------------------------------
bool Blockchain::reset_and_set_genesis_block(const block& b)
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);
  m_alternative_chains.clear();
  m_db->reset();
  m_hardfork->init();

  block_verification_context bvc = boost::value_initialized<block_verification_context>();
  add_new_block(b, bvc);
  return bvc.m_added_to_main_chain && !bvc.m_verifivation_failed;
}
//------------------------------------------------------------------
crypto::hash Blockchain::get_tail_id(uint64_t& height) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);
  height = m_db->height() - 1;
  return get_tail_id();
}
//------------------------------------------------------------------
crypto::hash Blockchain::get_tail_id() const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);
  return m_db->top_block_hash();
}
//------------------------------------------------------------------
/*TODO: this function was...poorly written.  As such, I'm not entirely
 *      certain on what it was supposed to be doing.  Need to look into this,
 *      but it doesn't seem terribly important just yet.
 *
 * puts into list <ids> a list of hashes representing certain blocks
 * from the blockchain in reverse chronological order
 *
 * the blocks chosen, at the time of this writing, are:
 *   the most recent 11
 *   powers of 2 less recent from there, so 13, 17, 25, etc...
 *
 */
bool Blockchain::get_short_chain_history(std::list<crypto::hash>& ids) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);
  uint64_t i = 0;
  uint64_t current_multiplier = 1;
  uint64_t sz = m_db->height();

  if(!sz)
    return true;

  m_db->block_txn_start(true);
  bool genesis_included = false;
  uint64_t current_back_offset = 1;
  while(current_back_offset < sz)
  {
    ids.push_back(m_db->get_block_hash_from_height(sz - current_back_offset));

    if(sz-current_back_offset == 0)
    {
      genesis_included = true;
    }
    if(i < 10)
    {
      ++current_back_offset;
    }
    else
    {
      current_multiplier *= 2;
      current_back_offset += current_multiplier;
    }
    ++i;
  }

  if (!genesis_included)
  {
    ids.push_back(m_db->get_block_hash_from_height(0));
  }
  m_db->block_txn_stop();

  return true;
}
//------------------------------------------------------------------
crypto::hash Blockchain::get_block_id_by_height(uint64_t height) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);
  try
  {
    return m_db->get_block_hash_from_height(height);
  }
  catch (const BLOCK_DNE& e)
  {
  }
  catch (const std::exception& e)
  {
    LOG_PRINT_L0(std::string("Something went wrong fetching block hash by height: ") + e.what());
    throw;
  }
  catch (...)
  {
    LOG_PRINT_L0(std::string("Something went wrong fetching block hash by height"));
    throw;
  }
  return null_hash;
}
//------------------------------------------------------------------
bool Blockchain::get_block_by_hash(const crypto::hash &h, block &blk) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);

  // try to find block in main chain
  try
  {
    blk = m_db->get_block(h);
    return true;
  }
  // try to find block in alternative chain
  catch (const BLOCK_DNE& e)
  {
    blocks_ext_by_hash::const_iterator it_alt = m_alternative_chains.find(h);
    if (m_alternative_chains.end() != it_alt)
    {
      blk = it_alt->second.bl;
      return true;
    }
  }
  catch (const std::exception& e)
  {
    LOG_PRINT_L0(std::string("Something went wrong fetching block by hash: ") + e.what());
    throw;
  }
  catch (...)
  {
    LOG_PRINT_L0(std::string("Something went wrong fetching block hash by hash"));
    throw;
  }

  return false;
}
//------------------------------------------------------------------
//FIXME: this function does not seem to be called from anywhere, but
// if it ever is, should probably change std::list for std::vector
void Blockchain::get_all_known_block_ids(std::list<crypto::hash> &main, std::list<crypto::hash> &alt, std::list<crypto::hash> &invalid) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);

  for (auto& a : m_db->get_hashes_range(0, m_db->height() - 1))
  {
    main.push_back(a);
  }

  for (const blocks_ext_by_hash::value_type &v: m_alternative_chains)
    alt.push_back(v.first);

  for (const blocks_ext_by_hash::value_type &v: m_invalid_blocks)
    invalid.push_back(v.first);
}
//------------------------------------------------------------------
// This function aggregates the cumulative difficulties and timestamps of the
// last DIFFICULTY_BLOCKS_COUNT blocks and passes them to next_difficulty,
// returning the result of that call.  Ignores the genesis block, and can use
// less blocks than desired if there aren't enough.
difficulty_type Blockchain::get_difficulty_for_next_block()
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);
  std::vector<uint64_t> timestamps;
  std::vector<difficulty_type> difficulties;
  auto height = m_db->height();
  size_t difficult_block_count = DIFFICULTY_BLOCKS_COUNT;

  // ND: Speedup
  // 1. Keep a list of the last 735 (or less) blocks that is used to compute difficulty,
  //    then when the next block difficulty is queried, push the latest height data and
  //    pop the oldest one from the list. This only requires 1x read per height instead
  //    of doing 735 (DIFFICULTY_BLOCKS_COUNT).
  if (m_timestamps_and_difficulties_height != 0 && ((height - m_timestamps_and_difficulties_height) == 1))
  {
    uint64_t index = height - 1;
    m_timestamps.push_back(m_db->get_block_timestamp(index));
    m_difficulties.push_back(m_db->get_block_cumulative_difficulty(index));

    while (m_timestamps.size() > difficult_block_count)
      m_timestamps.erase(m_timestamps.begin());
    while (m_difficulties.size() > difficult_block_count)
      m_difficulties.erase(m_difficulties.begin());

    m_timestamps_and_difficulties_height = height;
    timestamps = m_timestamps;
    difficulties = m_difficulties;
  }
  else
  {
    size_t offset = height - std::min < size_t >(height, static_cast<size_t>(difficult_block_count));
    if (offset == 0)
      ++offset;

    timestamps.clear();
    difficulties.clear();
    for (; offset < height; offset++)
    {
      timestamps.push_back(m_db->get_block_timestamp(offset));
      difficulties.push_back(m_db->get_block_cumulative_difficulty(offset));
    }

    m_timestamps_and_difficulties_height = height;
    m_timestamps = timestamps;
    m_difficulties = difficulties;
  }
  size_t target = DIFFICULTY_TARGET;
  return next_difficulty(timestamps, difficulties, target);
}
//------------------------------------------------------------------
// This function removes blocks from the blockchain until it gets to the
// position where the blockchain switch started and then re-adds the blocks
// that had been removed.
bool Blockchain::rollback_blockchain_switching(std::list<block>& original_chain, uint64_t rollback_height)
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);

  // fail if rollback_height passed is too high
  if (rollback_height > m_db->height())
  {
    return true;
  }

  m_timestamps_and_difficulties_height = 0;

  // remove blocks from blockchain until we get back to where we should be.
  while (m_db->height() != rollback_height)
  {
    pop_block_from_blockchain();
  }

  // make sure the hard fork object updates its current version
  m_hardfork->reorganize_from_chain_height(rollback_height);

  //return back original chain
  for (auto& bl : original_chain)
  {
    block_verification_context bvc = boost::value_initialized<block_verification_context>();
    bool r = handle_block_to_main_chain(bl, bvc);
    CHECK_AND_ASSERT_MES(r && bvc.m_added_to_main_chain, false, "PANIC! failed to add (again) block while chain switching during the rollback!");
  }

  m_hardfork->reorganize_from_chain_height(rollback_height);

  LOG_PRINT_L1("Rollback to height " << rollback_height << " was successful.");
  if (original_chain.size())
  {
    LOG_PRINT_L1("Restoration to previous blockchain successful as well.");
  }
  return true;
}
//------------------------------------------------------------------
// This function attempts to switch to an alternate chain, returning
// boolean based on success therein.
bool Blockchain::switch_to_alternative_blockchain(std::list<blocks_ext_by_hash::iterator>& alt_chain, bool discard_disconnected_chain)
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);

  m_timestamps_and_difficulties_height = 0;

  // if empty alt chain passed (not sure how that could happen), return false
  CHECK_AND_ASSERT_MES(alt_chain.size(), false, "switch_to_alternative_blockchain: empty chain passed");

  // verify that main chain has front of alt chain's parent block
  if (!m_db->block_exists(alt_chain.front()->second.bl.prev_id))
  {
    LOG_ERROR("Attempting to move to an alternate chain, but it doesn't appear to connect to the main chain!");
    return false;
  }

  // pop blocks from the blockchain until the top block is the parent
  // of the front block of the alt chain.
  std::list<block> disconnected_chain;
  while (m_db->top_block_hash() != alt_chain.front()->second.bl.prev_id)
  {
    block b = pop_block_from_blockchain();
    disconnected_chain.push_front(b);
  }

  auto split_height = m_db->height();

  //connecting new alternative chain
  for(auto alt_ch_iter = alt_chain.begin(); alt_ch_iter != alt_chain.end(); alt_ch_iter++)
  {
    auto ch_ent = *alt_ch_iter;
    block_verification_context bvc = boost::value_initialized<block_verification_context>();

    // add block to main chain
    bool r = handle_block_to_main_chain(ch_ent->second.bl, bvc);

    // if adding block to main chain failed, rollback to previous state and
    // return false
    if(!r || !bvc.m_added_to_main_chain)
    {
      LOG_PRINT_L1("Failed to switch to alternative blockchain");

      // rollback_blockchain_switching should be moved to two different
      // functions: rollback and apply_chain, but for now we pretend it is
      // just the latter (because the rollback was done above).
      rollback_blockchain_switching(disconnected_chain, split_height);

      // FIXME: Why do we keep invalid blocks around?  Possibly in case we hear
      // about them again so we can immediately dismiss them, but needs some
      // looking into.
      add_block_as_invalid(ch_ent->second, get_block_hash(ch_ent->second.bl));
      LOG_PRINT_L1("The block was inserted as invalid while connecting new alternative chain, block_id: " << get_block_hash(ch_ent->second.bl));
      m_alternative_chains.erase(*alt_ch_iter++);

      for(auto alt_ch_to_orph_iter = alt_ch_iter; alt_ch_to_orph_iter != alt_chain.end(); )
      {
        add_block_as_invalid((*alt_ch_to_orph_iter)->second, (*alt_ch_to_orph_iter)->first);
        m_alternative_chains.erase(*alt_ch_to_orph_iter++);
      }
      return false;
    }
  }

  // if we're to keep the disconnected blocks, add them as alternates
  if(!discard_disconnected_chain)
  {
    //pushing old chain as alternative chain
    for (auto& old_ch_ent : disconnected_chain)
    {
      block_verification_context bvc = boost::value_initialized<block_verification_context>();
      bool r = handle_alternative_block(old_ch_ent, get_block_hash(old_ch_ent), bvc);
      if(!r)
      {
        LOG_PRINT_L1("Failed to push ex-main chain blocks to alternative chain ");
        // previously this would fail the blockchain switching, but I don't
        // think this is bad enough to warrant that.
      }
    }
  }

  //removing alt_chain entries from alternative chains container
  for (auto ch_ent: alt_chain)
  {
    m_alternative_chains.erase(ch_ent);
  }

  m_hardfork->reorganize_from_chain_height(split_height);

  LOG_PRINT_GREEN("REORGANIZE SUCCESS! on height: " << split_height << ", new blockchain size: " << m_db->height(), LOG_LEVEL_0);
  return true;
}
//------------------------------------------------------------------
// This function calculates the difficulty target for the block being added to
// an alternate chain.
difficulty_type Blockchain::get_next_difficulty_for_alternative_chain(const std::list<blocks_ext_by_hash::iterator>& alt_chain, block_extended_info& bei) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  std::vector<uint64_t> timestamps;
  std::vector<difficulty_type> cumulative_difficulties;
  size_t difficult_block_count = DIFFICULTY_BLOCKS_COUNT;

  // if the alt chain isn't long enough to calculate the difficulty target
  // based on its blocks alone, need to get more blocks from the main chain
  if (alt_chain.size() < difficult_block_count)
  {
    CRITICAL_REGION_LOCAL(m_blockchain_lock);

    // Figure out start and stop offsets for main chain blocks
    size_t main_chain_stop_offset = alt_chain.size() ? alt_chain.front()->second.height : bei.height;
    size_t main_chain_count = difficult_block_count - std::min(static_cast<size_t>(difficult_block_count), alt_chain.size());
    main_chain_count = std::min(main_chain_count, main_chain_stop_offset);
    size_t main_chain_start_offset = main_chain_stop_offset - main_chain_count;

    if (!main_chain_start_offset)
      ++main_chain_start_offset; //skip genesis block

    // get difficulties and timestamps from relevant main chain blocks
    for (; main_chain_start_offset < main_chain_stop_offset; ++main_chain_start_offset)
    {
      timestamps.push_back(m_db->get_block_timestamp(main_chain_start_offset));
      cumulative_difficulties.push_back(m_db->get_block_cumulative_difficulty(main_chain_start_offset));
    }

    // make sure we haven't accidentally grabbed too many blocks...maybe don't need this check?
    CHECK_AND_ASSERT_MES((alt_chain.size() + timestamps.size()) <= difficult_block_count, false, "Internal error, alt_chain.size()[" << alt_chain.size() << "] + vtimestampsec.size()[" << timestamps.size() << "] NOT <= DIFFICULTY_WINDOW[]" << difficult_block_count);

    for (auto it : alt_chain)
    {
      timestamps.push_back(it->second.bl.timestamp);
      cumulative_difficulties.push_back(it->second.cumulative_difficulty);
    }
  }
  // if the alt chain is long enough for the difficulty calc, grab difficulties
  // and timestamps from it alone
  else
  {
    timestamps.resize(static_cast<size_t>(difficult_block_count));
    cumulative_difficulties.resize(static_cast<size_t>(difficult_block_count));
    size_t count = 0;
    size_t max_i = timestamps.size() - 1;
    // get difficulties and timestamps from most recent blocks in alt chain
    BOOST_REVERSE_FOREACH(auto it, alt_chain)
    {
      timestamps[max_i - count] = it->second.bl.timestamp;
      cumulative_difficulties[max_i - count] = it->second.cumulative_difficulty;
      count++;
      if (count >= difficult_block_count)
        break;
    }
  }

  // FIXME: This will fail if fork activation heights are subject to voting
  size_t target = DIFFICULTY_TARGET;

  // calculate the difficulty target for the block and return it
  return next_difficulty(timestamps, cumulative_difficulties, target);
}
//------------------------------------------------------------------
// This function does a sanity check on basic things that all miner
// transactions have in common, such as:
//   one input, of type txin_gen, with height set to the block's height
//   correct miner tx unlock time
//   a non-overflowing tx amount (dubious necessity on this check)
bool Blockchain::prevalidate_miner_transaction(const block& b, uint64_t height)
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CHECK_AND_ASSERT_MES(b.miner_tx.vin.size() == 1, false, "coinbase transaction in the block has no inputs");
  CHECK_AND_ASSERT_MES(b.miner_tx.vin[0].type() == typeid(txin_gen), false, "coinbase transaction in the block has the wrong type");
  if(boost::get<txin_gen>(b.miner_tx.vin[0]).height != height)
  {
    LOG_PRINT_RED_L1("The miner transaction in block has invalid height: " << boost::get<txin_gen>(b.miner_tx.vin[0]).height << ", expected: " << height);
    return false;
  }
  CHECK_AND_ASSERT_MES(b.miner_tx.unlock_time == height + CRYPTONOTE_MINED_MONEY_UNLOCK_WINDOW, false, "coinbase transaction transaction has the wrong unlock time=" << b.miner_tx.unlock_time << ", expected " << height + CRYPTONOTE_MINED_MONEY_UNLOCK_WINDOW);

  //check outs overflow
  //NOTE: not entirely sure this is necessary, given that this function is
  //      designed simply to make sure the total amount for a transaction
  //      does not overflow a uint64_t, and this transaction *is* a uint64_t...
  if(!check_outs_overflow(b.miner_tx))
  {
    LOG_PRINT_RED_L1("miner transaction has money overflow in block " << get_block_hash(b));
    return false;
  }

  return true;
}
//------------------------------------------------------------------
// This function validates the miner transaction reward
bool Blockchain::validate_miner_transaction(const block& b, size_t cumulative_block_size, uint64_t fee, uint64_t& base_reward, uint64_t already_generated_coins, bool &partial_block_reward, uint8_t version)
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  //validate reward
  uint64_t money_in_use = 0;

  // The genesis block does not have the project free.
  if (b.timestamp != 0 && b.miner_tx.vout.size() != 2) {
    LOG_PRINT_L1("miner transaction doesn't contain 2 outs! Size is " << b.miner_tx.vout.size());
    return false;
  }

  account_base project_account;

  address_parse_info address_info;
  if (!get_account_address_from_str(address_info, false, CRYPTONOTE_PROJECT_BLOCK_ADDRESS)) {
    LOG_PRINT_L1("Unable to parse developer wallet address!");
    return false;
  }

  cryptonote::blobdata viewkey_data;
  string project_viewkey = CRYPTONOTE_PROJECT_BLOCK_VIEWKEY;
  if(!epee::string_tools::parse_hexstr_to_binbuff(project_viewkey, viewkey_data)) {
    LOG_PRINT_L1("Unable to parse developer wallet viewkey!");
    return false;
  }

  crypto::secret_key viewkey = *reinterpret_cast<const crypto::secret_key*>(viewkey_data.data());
  project_account.create_from_viewkey(address_info.address, viewkey);

	std::vector<tx_extra_field> tx_extra_fields;
  parse_tx_extra(b.miner_tx.extra, tx_extra_fields);

  tx_extra_pub_key pub_key_field;
  if (!find_tx_extra_field_by_type(tx_extra_fields, pub_key_field, 0)) {
    LOG_PRINT_L1("Public tx key not found in transaction");
    return false;
  }

  const cryptonote::account_keys& keys = project_account.get_keys();
  crypto::key_derivation derivation;
  generate_key_derivation(pub_key_field.pub_key, keys.m_view_secret_key, derivation);

  bool project_out_found = false;
  float project_dev_fee = get_project_block_reward_fee(already_generated_coins);
  float project_dev_fee_amount = 0;

  for (size_t i = 0; i < b.miner_tx.vout.size(); ++i) {
    crypto::public_key pk;
    derive_public_key(derivation, i, keys.m_account_address.m_spend_public_key, pk);
    if (pk == boost::get<txout_to_key>(b.miner_tx.vout[i].target).key) {
      LOG_PRINT_L3("Found project out in miner_tx");
      // Check if the amount is correct.
      project_dev_fee_amount = b.miner_tx.vout[i].amount;
      project_out_found = true;
    }
    money_in_use += b.miner_tx.vout[i].amount;
  }

  partial_block_reward = false;

  std::vector<size_t> last_blocks_sizes;
  get_last_n_blocks_sizes(last_blocks_sizes, CRYPTONOTE_REWARD_BLOCKS_WINDOW);

  if (!get_block_reward(epee::misc_utils::median(last_blocks_sizes), cumulative_block_size, already_generated_coins, base_reward, m_db->height(), version))
  {
    LOG_PRINT_L1("block size " << cumulative_block_size << " is bigger than allowed for this blockchain");
    return false;
  }

  if (project_dev_fee > 0) {
    if (m_db->height() != 0 && (project_dev_fee * base_reward) != project_dev_fee_amount) {
      LOG_PRINT_L1("Project dev fee is incorrect! " << (project_dev_fee * base_reward) << " != " << project_dev_fee_amount);
      return false;
    }
  }

  if(base_reward + fee < money_in_use)
  {
    LOG_PRINT_L1("coinbase transaction spend too much money (" << print_money(money_in_use) << "). Block reward is " << print_money(base_reward + fee) << "(" << print_money(base_reward) << "+" << print_money(fee) << ")");
    return false;
  }

  // from hard fork 2, since a miner can claim less than the full block reward, we update the base_reward
  // to show the amount of coins that were actually generated, the remainder will be pushed back for later
  // emission. This modifies the emission curve very slightly.
  CHECK_AND_ASSERT_MES(money_in_use - fee <= base_reward, false, "base reward calculation bug");
  if(base_reward + fee != money_in_use)
    partial_block_reward = true;
  base_reward = money_in_use - fee;

  return m_db->height() != 0 ? project_out_found : true;
}
//------------------------------------------------------------------
// get the block sizes of the last <count> blocks, and return by reference <sz>.
void Blockchain::get_last_n_blocks_sizes(std::vector<size_t>& sz, size_t count) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);
  auto h = m_db->height();

  // this function is meaningless for an empty blockchain...granted it should never be empty
  if(h == 0)
    return;

  m_db->block_txn_start(true);
  // add size of last <count> blocks to vector <sz> (or less, if blockchain size < count)
  size_t start_offset = h - std::min<size_t>(h, count);
  for(size_t i = start_offset; i < h; i++)
  {
    sz.push_back(m_db->get_block_size(i));
  }
  m_db->block_txn_stop();
}
//------------------------------------------------------------------
uint64_t Blockchain::get_current_cumulative_blocksize_limit() const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  return m_current_block_cumul_sz_limit;
}
//------------------------------------------------------------------
//TODO: This function only needed minor modification to work with BlockchainDB,
//      and *works*.  As such, to reduce the number of things that might break
//      in moving to BlockchainDB, this function will remain otherwise
//      unchanged for the time being.
//
// This function makes a new block for a miner to mine the hash for
//
// FIXME: this codebase references #if defined(DEBUG_CREATE_BLOCK_TEMPLATE)
// in a lot of places.  That flag is not referenced in any of the code
// nor any of the makefiles, howeve.  Need to look into whether or not it's
// necessary at all.
bool Blockchain::create_block_template(block& b, const account_public_address& miner_address, difficulty_type& diffic, uint64_t& height, const blobdata& ex_nonce)
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  size_t median_size;
  uint64_t already_generated_coins;
  uint64_t cal_height;

  CRITICAL_REGION_BEGIN(m_blockchain_lock);
  height = m_db->height();

  b.major_version = m_hardfork->get_current_version();
  b.minor_version = m_hardfork->get_ideal_version();
  b.prev_id = get_tail_id();
  b.timestamp = time(NULL);

  diffic = get_difficulty_for_next_block();
  CHECK_AND_ASSERT_MES(diffic, false, "difficulty overhead.");

  median_size = m_current_block_cumul_sz_limit / 2;

  cal_height = height - height % COIN_EMISSION_HEIGHT_INTERVAL;
  already_generated_coins = cal_height ? m_db->get_block_already_generated_coins(cal_height - 1) : 0;

  CRITICAL_REGION_END();

  size_t txs_size;
  uint64_t fee;
  uint8_t hf_version = m_hardfork->get_current_version();
  if (!m_tx_pool.fill_block_template(b, median_size, already_generated_coins, txs_size, fee, height, hf_version))
  {
    return false;
  }
#if defined(DEBUG_CREATE_BLOCK_TEMPLATE)
  size_t real_txs_size = 0;
  uint64_t real_fee = 0;
  CRITICAL_REGION_BEGIN(m_tx_pool.m_transactions_lock);
  for(crypto::hash &cur_hash: b.tx_hashes)
  {
    auto cur_res = m_tx_pool.m_transactions.find(cur_hash);
    if (cur_res == m_tx_pool.m_transactions.end())
    {
      LOG_ERROR("Creating block template: error: transaction not found");
      continue;
    }
    tx_memory_pool::tx_details &cur_tx = cur_res->second;
    real_txs_size += cur_tx.blob_size;
    real_fee += cur_tx.fee;
    if (cur_tx.blob_size != get_object_blobsize(cur_tx.tx))
    {
      LOG_ERROR("Creating block template: error: invalid transaction size");
    }
    if (cur_tx.tx.version == 1)
    {
      uint64_t inputs_amount;
      if (!get_inputs_money_amount(cur_tx.tx, inputs_amount))
      {
        LOG_ERROR("Creating block template: error: cannot get inputs amount");
      }
      else if (cur_tx.fee != inputs_amount - get_outs_money_amount(cur_tx.tx))
      {
        LOG_ERROR("Creating block template: error: invalid fee");
      }
    }
    else
    {
      if (cur_tx.fee != cur_tx.tx.rct_signatures.txnFee)
      {
        LOG_ERROR("Creating block template: error: invalid fee");
      }
    }
  }
  if (txs_size != real_txs_size)
  {
    LOG_ERROR("Creating block template: error: wrongly calculated transaction size");
  }
  if (fee != real_fee)
  {
    LOG_ERROR("Creating block template: error: wrongly calculated fee");
  }
  CRITICAL_REGION_END();
  LOG_PRINT_L1("Creating block template: height " << height <<
      ", median size " << median_size <<
      ", already generated coins " << already_generated_coins <<
      ", transaction size " << txs_size <<
      ", fee " << fee);
#endif

  /*
   two-phase miner transaction generation: we don't know exact block size until we prepare block, but we don't know reward until we know
   block size, so first miner transaction generated with fake amount of money, and with phase we know think we know expected block size
   */
  //make blocks coin-base tx looks close to real coinbase tx to get truthful blob size
  size_t max_outs = 1;
  bool r = construct_miner_tx(height, median_size, already_generated_coins, txs_size, fee, miner_address, b.miner_tx, ex_nonce, max_outs, hf_version);
  CHECK_AND_ASSERT_MES(r, false, "Failed to construct miner tx, first chance");
  size_t cumulative_size = txs_size + get_object_blobsize(b.miner_tx);
#if defined(DEBUG_CREATE_BLOCK_TEMPLATE)
  LOG_PRINT_L1("Creating block template: miner tx size " << get_object_blobsize(b.miner_tx) <<
      ", cumulative size " << cumulative_size);
#endif
  for (size_t try_count = 0; try_count != 10; ++try_count)
  {
    r = construct_miner_tx(height, median_size, already_generated_coins, cumulative_size, fee, miner_address, b.miner_tx, ex_nonce, max_outs, hf_version);

    CHECK_AND_ASSERT_MES(r, false, "Failed to construct miner tx, second chance");
    size_t coinbase_blob_size = get_object_blobsize(b.miner_tx);
    if (coinbase_blob_size > cumulative_size - txs_size)
    {
      cumulative_size = txs_size + coinbase_blob_size;
#if defined(DEBUG_CREATE_BLOCK_TEMPLATE)
      LOG_PRINT_L1("Creating block template: miner tx size " << coinbase_blob_size <<
          ", cumulative size " << cumulative_size << " is greater then before");
#endif
      continue;
    }

    if (coinbase_blob_size < cumulative_size - txs_size)
    {
      size_t delta = cumulative_size - txs_size - coinbase_blob_size;
#if defined(DEBUG_CREATE_BLOCK_TEMPLATE)
      LOG_PRINT_L1("Creating block template: miner tx size " << coinbase_blob_size <<
          ", cumulative size " << txs_size + coinbase_blob_size <<
          " is less then before, adding " << delta << " zero bytes");
#endif
      b.miner_tx.extra.insert(b.miner_tx.extra.end(), delta, 0);
      //here  could be 1 byte difference, because of extra field counter is varint, and it can become from 1-byte len to 2-bytes len.
      if (cumulative_size != txs_size + get_object_blobsize(b.miner_tx))
      {
        CHECK_AND_ASSERT_MES(cumulative_size + 1 == txs_size + get_object_blobsize(b.miner_tx), false, "unexpected case: cumulative_size=" << cumulative_size << " + 1 is not equal txs_cumulative_size=" << txs_size << " + get_object_blobsize(b.miner_tx)=" << get_object_blobsize(b.miner_tx));
        b.miner_tx.extra.resize(b.miner_tx.extra.size() - 1);
        if (cumulative_size != txs_size + get_object_blobsize(b.miner_tx))
        {
          //fuck, not lucky, -1 makes varint-counter size smaller, in that case we continue to grow with cumulative_size
          LOG_PRINT_RED("Miner tx creation has no luck with delta_extra size = " << delta << " and " << delta - 1 , LOG_LEVEL_2);
          cumulative_size += delta - 1;
          continue;
        }
        LOG_PRINT_GREEN("Setting extra for block: " << b.miner_tx.extra.size() << ", try_count=" << try_count, LOG_LEVEL_1);
      }
    }
    CHECK_AND_ASSERT_MES(cumulative_size == txs_size + get_object_blobsize(b.miner_tx), false, "unexpected case: cumulative_size=" << cumulative_size << " is not equal txs_cumulative_size=" << txs_size << " + get_object_blobsize(b.miner_tx)=" << get_object_blobsize(b.miner_tx));
#if defined(DEBUG_CREATE_BLOCK_TEMPLATE)
    LOG_PRINT_L1("Creating block template: miner tx size " << coinbase_blob_size <<
        ", cumulative size " << cumulative_size << " is now good");
#endif
    return true;
  }
  LOG_ERROR("Failed to create_block_template with " << 10 << " tries");
  return false;
}
//------------------------------------------------------------------
// for an alternate chain, get the timestamps from the main chain to complete
// the needed number of timestamps for the BLOCKCHAIN_TIMESTAMP_CHECK_WINDOW.
bool Blockchain::complete_timestamps_vector(uint64_t start_top_height, std::vector<uint64_t>& timestamps)
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  size_t blockchain_timestamp_check_window = BLOCKCHAIN_TIMESTAMP_CHECK_WINDOW;
  if (timestamps.size() >= blockchain_timestamp_check_window)
    return true;

  CRITICAL_REGION_LOCAL(m_blockchain_lock);
  size_t need_elements = blockchain_timestamp_check_window - timestamps.size();
  CHECK_AND_ASSERT_MES(start_top_height < m_db->height(), false, "internal error: passed start_height not < " << " m_db->height() -- " << start_top_height << " >= " << m_db->height());
  size_t stop_offset = start_top_height > need_elements ? start_top_height - need_elements : 0;
  while (start_top_height != stop_offset)
  {
    timestamps.push_back(m_db->get_block_timestamp(start_top_height));
    --start_top_height;
  }
  return true;
}
//------------------------------------------------------------------
// If a block is to be added and its parent block is not the current
// main chain top block, then we need to see if we know about its parent block.
// If its parent block is part of a known forked chain, then we need to see
// if that chain is long enough to become the main chain and re-org accordingly
// if so.  If not, we need to hang on to the block in case it becomes part of
// a long forked chain eventually.
bool Blockchain::handle_alternative_block(const block& b, const crypto::hash& id, block_verification_context& bvc)
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);
  m_timestamps_and_difficulties_height = 0;
  uint64_t block_height = get_block_height(b);
  if(0 == block_height)
  {
    LOG_PRINT_L1("Block with id: " << epee::string_tools::pod_to_hex(id) << " (as alternative), but miner tx says height is 0.");
    bvc.m_verifivation_failed = true;
    return false;
  }
  // this basically says if the blockchain is smaller than the first
  // checkpoint then alternate blocks are allowed.  Alternatively, if the
  // last checkpoint *before* the end of the current chain is also before
  // the block to be added, then this is fine.
  if (!m_checkpoints.is_alternative_block_allowed(get_current_blockchain_height(), block_height))
  {
    LOG_PRINT_RED_L1("Block with id: " << id << std::endl << " can't be accepted for alternative chain, block height: " << block_height << std::endl << " blockchain height: " << get_current_blockchain_height());
    bvc.m_verifivation_failed = true;
    return false;
  }

  // this is a cheap test
  if (!m_hardfork->check_for_height(b, block_height))
  {
    LOG_PRINT_L1("Block with id: " << id << std::endl << "has old version for height " << block_height);
    bvc.m_verifivation_failed = true;
    return false;
  }

  //block is not related with head of main chain
  //first of all - look in alternative chains container
  auto it_prev = m_alternative_chains.find(b.prev_id);
  bool parent_in_main = m_db->block_exists(b.prev_id);
  if(it_prev != m_alternative_chains.end() || parent_in_main)
  {
    //we have new block in alternative chain

    //build alternative subchain, front -> mainchain, back -> alternative head
    blocks_ext_by_hash::iterator alt_it = it_prev; //m_alternative_chains.find()
    std::list<blocks_ext_by_hash::iterator> alt_chain;
    std::vector<uint64_t> timestamps;
    while(alt_it != m_alternative_chains.end())
    {
      alt_chain.push_front(alt_it);
      timestamps.push_back(alt_it->second.bl.timestamp);
      alt_it = m_alternative_chains.find(alt_it->second.bl.prev_id);
    }

    // if block to be added connects to known blocks that aren't part of the
    // main chain -- that is, if we're adding on to an alternate chain
    if(alt_chain.size())
    {
      // make sure alt chain doesn't somehow start past the end of the main chain
      CHECK_AND_ASSERT_MES(m_db->height() > alt_chain.front()->second.height, false, "main blockchain wrong height");

      // make sure that the blockchain contains the block that should connect
      // this alternate chain with it.
      if (!m_db->block_exists(alt_chain.front()->second.bl.prev_id))
      {
        LOG_PRINT_L1("alternate chain does not appear to connect to main chain...");
        return false;
      }

      // make sure block connects correctly to the main chain
      auto h = m_db->get_block_hash_from_height(alt_chain.front()->second.height - 1);
      CHECK_AND_ASSERT_MES(h == alt_chain.front()->second.bl.prev_id, false, "alternative chain has wrong connection to main chain");
      complete_timestamps_vector(m_db->get_block_height(alt_chain.front()->second.bl.prev_id), timestamps);
    }
    // if block not associated with known alternate chain
    else
    {
      // if block parent is not part of main chain or an alternate chain,
      // we ignore it
      CHECK_AND_ASSERT_MES(parent_in_main, false, "internal error: broken imperative condition: parent_in_main");

      complete_timestamps_vector(m_db->get_block_height(b.prev_id), timestamps);
    }

    // verify that the block's timestamp is within the acceptable range
    // (not earlier than the median of the last X blocks)
    if(!check_block_timestamp(timestamps, b))
    {
      LOG_PRINT_RED_L1("Block with id: " << id << std::endl << " for alternative chain, has invalid timestamp: " << b.timestamp);
      bvc.m_verifivation_failed = true;
      return false;
    }

    // FIXME: consider moving away from block_extended_info at some point
    block_extended_info bei = boost::value_initialized<block_extended_info>();
    bei.bl = b;
    bei.height = alt_chain.size() ? it_prev->second.height + 1 : m_db->get_block_height(b.prev_id) + 1;

    bool is_a_checkpoint;
    if(!m_checkpoints.check_block(bei.height, id, is_a_checkpoint))
    {
      LOG_ERROR("CHECKPOINT VALIDATION FAILED");
      bvc.m_verifivation_failed = true;
      return false;
    }

    // Check the block's hash against the difficulty target for its alt chain
    difficulty_type current_diff = get_next_difficulty_for_alternative_chain(alt_chain, bei);
    CHECK_AND_ASSERT_MES(current_diff, false, "!!!!!!! DIFFICULTY OVERHEAD !!!!!!!");
    crypto::hash proof_of_work = null_hash;
    get_block_longhash(bei.bl, m_pow_ctx, proof_of_work);

    if(!check_hash(proof_of_work, current_diff))
    {
      LOG_PRINT_RED_L1("Block with id: " << id << std::endl << " for alternative chain, does not have enough proof of work: " << proof_of_work << std::endl << " expected difficulty: " << current_diff);
      bvc.m_verifivation_failed = true;
      return false;
    }

    if(!prevalidate_miner_transaction(b, bei.height))
    {
      LOG_PRINT_RED_L1("Block with id: " << epee::string_tools::pod_to_hex(id) << " (as alternative) has incorrect miner transaction.");
      bvc.m_verifivation_failed = true;
      return false;
    }

    // FIXME:
    // this brings up an interesting point: consider allowing to get block
    // difficulty both by height OR by hash, not just height.
    difficulty_type main_chain_cumulative_difficulty = m_db->get_block_cumulative_difficulty(m_db->height() - 1);
    if (alt_chain.size())
    {
      bei.cumulative_difficulty = it_prev->second.cumulative_difficulty;
    }
    else
    {
      // passed-in block's previous block's cumulative difficulty, found on the main chain
      bei.cumulative_difficulty = m_db->get_block_cumulative_difficulty(m_db->get_block_height(b.prev_id));
    }
    bei.cumulative_difficulty += current_diff;

    // add block to alternate blocks storage,
    // as well as the current "alt chain" container
    auto i_res = m_alternative_chains.insert(blocks_ext_by_hash::value_type(id, bei));
    CHECK_AND_ASSERT_MES(i_res.second, false, "insertion of new alternative block returned as it already exist");
    alt_chain.push_back(i_res.first);

    // FIXME: is it even possible for a checkpoint to show up not on the main chain?
    if(is_a_checkpoint)
    {
      //do reorganize!
      LOG_PRINT_GREEN("###### REORGANIZE on height: " << alt_chain.front()->second.height << " of " << m_db->height() - 1 << ", checkpoint is found in alternative chain on height " << bei.height, LOG_LEVEL_0);

      bool r = switch_to_alternative_blockchain(alt_chain, true);

      if(r) bvc.m_added_to_main_chain = true;
      else bvc.m_verifivation_failed = true;

      return r;
    } else if(main_chain_cumulative_difficulty < bei.cumulative_difficulty) //check if difficulty bigger then in main chain
    {
      //do reorganize!
      LOG_PRINT_GREEN("###### REORGANIZE on height: " << alt_chain.front()->second.height << " of " << m_db->height() - 1 << " with cum_difficulty " << m_db->get_block_cumulative_difficulty(m_db->height() - 1) << std::endl << " alternative blockchain size: " << alt_chain.size() << " with cum_difficulty " << bei.cumulative_difficulty, LOG_LEVEL_0);

      bool r = switch_to_alternative_blockchain(alt_chain, false);
      if (r)
        bvc.m_added_to_main_chain = true;
      else
        bvc.m_verifivation_failed = true;
      return r;
    } else {
      LOG_PRINT_BLUE("----- BLOCK ADDED AS ALTERNATIVE ON HEIGHT " << bei.height << std::endl << "id:\t" << id << std::endl << "PoW:\t" << proof_of_work << std::endl << "difficulty:\t" << current_diff, LOG_LEVEL_0);
      return true;
    }
  }
  else
  {
    //block orphaned
    bvc.m_marked_as_orphaned = true;
    LOG_PRINT_RED_L1("Block recognized as orphaned and rejected, id = " << id);
  }

  return true;
}
//------------------------------------------------------------------
bool Blockchain::get_blocks(uint64_t start_offset, size_t count, std::list<block>& blocks, std::list<transaction>& txs) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);
  if(start_offset > m_db->height())
    return false;

  if (!get_blocks(start_offset, count, blocks))
  {
    return false;
  }

  for(const block& blk : blocks)
  {
    std::list<crypto::hash> missed_ids;
    get_transactions(blk.tx_hashes, txs, missed_ids);
    CHECK_AND_ASSERT_MES(!missed_ids.size(), false, "has missed transactions in own block in main blockchain");
  }

  return true;
}
//------------------------------------------------------------------
bool Blockchain::get_blocks(uint64_t start_offset, size_t count, std::list<block>& blocks) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);
  if(start_offset > m_db->height())
    return false;

  for(size_t i = start_offset; i < start_offset + count && i < m_db->height();i++)
  {
    blocks.push_back(m_db->get_block_from_height(i));
  }
  return true;
}
//------------------------------------------------------------------
//TODO: This function *looks* like it won't need to be rewritten
//      to use BlockchainDB, as it calls other functions that were,
//      but it warrants some looking into later.
//
//FIXME: This function appears to want to return false if any transactions
//       that belong with blocks are missing, but not if blocks themselves
//       are missing.
bool Blockchain::handle_get_objects(NOTIFY_REQUEST_GET_OBJECTS::request& arg, NOTIFY_RESPONSE_GET_OBJECTS::request& rsp)
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);
  m_db->block_txn_start(true);
  rsp.current_blockchain_height = get_current_blockchain_height();
  std::list<block> blocks;
  get_blocks(arg.blocks, blocks, rsp.missed_ids);

  for (const auto& bl: blocks)
  {
    std::list<crypto::hash> missed_tx_ids;
    std::list<transaction> txs;

    // FIXME: s/rsp.missed_ids/missed_tx_id/ ?  Seems like rsp.missed_ids
    //        is for missed blocks, not missed transactions as well.
    get_transactions(bl.tx_hashes, txs, missed_tx_ids);

    if (missed_tx_ids.size() != 0)
    {
      LOG_ERROR("Error retrieving blocks, missed " << missed_tx_ids.size()
          << " transactions for block with hash: " << get_block_hash(bl)
          << std::endl
      );

      // append missed transaction hashes to response missed_ids field,
      // as done below if any standalone transactions were requested
      // and missed.
      rsp.missed_ids.splice(rsp.missed_ids.end(), missed_tx_ids);
	  m_db->block_txn_stop();
      return false;
    }

    rsp.blocks.push_back(block_complete_entry());
    block_complete_entry& e = rsp.blocks.back();
    //pack block
    e.block = t_serializable_object_to_blob(bl);
    //pack transactions
    for (transaction& tx: txs)
      e.txs.push_back(t_serializable_object_to_blob(tx));
  }
  //get another transactions, if need
  std::list<transaction> txs;
  get_transactions(arg.txs, txs, rsp.missed_ids);
  //pack aside transactions
  for (const auto& tx: txs)
    rsp.txs.push_back(t_serializable_object_to_blob(tx));

  m_db->block_txn_stop();
  return true;
}
//------------------------------------------------------------------
bool Blockchain::get_alternative_blocks(std::list<block>& blocks) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);

  for (const auto& alt_bl: m_alternative_chains)
  {
    blocks.push_back(alt_bl.second.bl);
  }
  return true;
}
//------------------------------------------------------------------
size_t Blockchain::get_alternative_blocks_count() const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);
  return m_alternative_chains.size();
}
//------------------------------------------------------------------
// This function adds the output specified by <amount, i> to the result_outs container
// unlocked and other such checks should be done by here.
void Blockchain::add_out_to_get_random_outs(COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount& result_outs, uint64_t amount, size_t i) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);

  COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::out_entry& oen = *result_outs.outs.insert(result_outs.outs.end(), COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::out_entry());
  oen.global_amount_index = i;
  output_data_t data = m_db->get_output_key(amount, i);
  oen.out_key = data.pubkey;
}
//------------------------------------------------------------------
// This function takes an RPC request for mixins and creates an RPC response
// with the requested mixins.
// TODO: figure out why this returns boolean / if we should be returning false
// in some cases
bool Blockchain::get_random_outs_for_amounts(const COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::request& req, COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::response& res) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);

  // for each amount that we need to get mixins for, get <n> random outputs
  // from BlockchainDB where <n> is req.outs_count (number of mixins).
  for (uint64_t amount : req.amounts)
  {
    auto num_outs = m_db->get_num_outputs(amount);
    // ensure we don't include outputs that aren't yet eligible to be used
    // outpouts are sorted by height
    while (num_outs > 0)
    {
      const tx_out_index toi = m_db->get_output_tx_and_index(amount, num_outs - 1);
      const uint64_t height = m_db->get_tx_block_height(toi.first);
      if (height + CRYPTONOTE_DEFAULT_TX_SPENDABLE_AGE <= m_db->height())
        break;
      --num_outs;
    }

    // create outs_for_amount struct and populate amount field
    COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount& result_outs = *res.outs.insert(res.outs.end(), COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount());
    result_outs.amount = amount;

    std::unordered_set<uint64_t> seen_indices;

    // if there aren't enough outputs to mix with (or just enough),
    // use all of them.  Eventually this should become impossible.
    if (num_outs <= req.outs_count)
    {
      for (uint64_t i = 0; i < num_outs; i++)
      {
        // get tx_hash, tx_out_index from DB
        tx_out_index toi = m_db->get_output_tx_and_index(amount, i);

        // if tx is unlocked, add output to result_outs
        if (is_tx_spendtime_unlocked(m_db->get_tx_unlock_time(toi.first)))
        {
          add_out_to_get_random_outs(result_outs, amount, i);
        }

      }
    }
    else
    {
      // while we still need more mixins
      while (result_outs.outs.size() < req.outs_count)
      {
        // if we've gone through every possible output, we've gotten all we can
        if (seen_indices.size() == num_outs)
        {
          break;
        }

        // get a random output index from the DB.  If we've already seen it,
        // return to the top of the loop and try again, otherwise add it to the
        // list of output indices we've seen.

        // triangular distribution over [a,b) with a=0, mode c=b=up_index_limit
        uint64_t r = crypto::rand<uint64_t>() % ((uint64_t)1 << 53);
        double frac = std::sqrt((double)r / ((uint64_t)1 << 53));
        uint64_t i = (uint64_t)(frac*num_outs);
        // just in case rounding up to 1 occurs after sqrt
        if (i == num_outs)
          --i;

        if (seen_indices.count(i))
        {
          continue;
        }
        seen_indices.emplace(i);

        // get tx_hash, tx_out_index from DB
        tx_out_index toi = m_db->get_output_tx_and_index(amount, i);

        // if the output's transaction is unlocked, add the output's index to
        // our list.
        if (is_tx_spendtime_unlocked(m_db->get_tx_unlock_time(toi.first)))
        {
          add_out_to_get_random_outs(result_outs, amount, i);
        }
      }
    }
  }
  return true;
}
//------------------------------------------------------------------
// This function adds the ringct output at index i to the list
// unlocked and other such checks should be done by here.
void Blockchain::add_out_to_get_rct_random_outs(std::list<COMMAND_RPC_GET_RANDOM_RCT_OUTPUTS::out_entry>& outs, uint64_t amount, size_t i) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);

  COMMAND_RPC_GET_RANDOM_RCT_OUTPUTS::out_entry& oen = *outs.insert(outs.end(), COMMAND_RPC_GET_RANDOM_RCT_OUTPUTS::out_entry());
  oen.amount = amount;
  oen.global_amount_index = i;
  output_data_t data = m_db->get_output_key(amount, i);
  oen.out_key = data.pubkey;
  oen.commitment = data.commitment;
}
//------------------------------------------------------------------
// This function takes an RPC request for mixins and creates an RPC response
// with the requested mixins.
// TODO: figure out why this returns boolean / if we should be returning false
// in some cases
bool Blockchain::get_random_rct_outs(const COMMAND_RPC_GET_RANDOM_RCT_OUTPUTS::request& req, COMMAND_RPC_GET_RANDOM_RCT_OUTPUTS::response& res) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);

  // for each amount that we need to get mixins for, get <n> random outputs
  // from BlockchainDB where <n> is req.outs_count (number of mixins).
  auto num_outs = m_db->get_num_outputs(0);
  // ensure we don't include outputs that aren't yet eligible to be used
  // outpouts are sorted by height
  while (num_outs > 0)
  {
    const tx_out_index toi = m_db->get_output_tx_and_index(0, num_outs - 1);
    const uint64_t height = m_db->get_tx_block_height(toi.first);
    if (height + CRYPTONOTE_DEFAULT_TX_SPENDABLE_AGE <= m_db->height())
      break;
    --num_outs;
  }

  std::unordered_set<uint64_t> seen_indices;

  // if there aren't enough outputs to mix with (or just enough),
  // use all of them.  Eventually this should become impossible.
  if (num_outs <= req.outs_count)
  {
    for (uint64_t i = 0; i < num_outs; i++)
    {
      // get tx_hash, tx_out_index from DB
      tx_out_index toi = m_db->get_output_tx_and_index(0, i);

      // if tx is unlocked, add output to result_outs
      if (is_tx_spendtime_unlocked(m_db->get_tx_unlock_time(toi.first)))
      {
        add_out_to_get_rct_random_outs(res.outs, 0, i);
      }
    }
  }
  else
  {
    // while we still need more mixins
    while (res.outs.size() < req.outs_count)
    {
      // if we've gone through every possible output, we've gotten all we can
      if (seen_indices.size() == num_outs)
      {
        break;
      }

      // get a random output index from the DB.  If we've already seen it,
      // return to the top of the loop and try again, otherwise add it to the
      // list of output indices we've seen.

      // triangular distribution over [a,b) with a=0, mode c=b=up_index_limit
      uint64_t r = crypto::rand<uint64_t>() % ((uint64_t)1 << 53);
      double frac = std::sqrt((double)r / ((uint64_t)1 << 53));
      uint64_t i = (uint64_t)(frac*num_outs);
      // just in case rounding up to 1 occurs after sqrt
      if (i == num_outs)
        --i;

      if (seen_indices.count(i))
      {
        continue;
      }
      seen_indices.emplace(i);

      // get tx_hash, tx_out_index from DB
      tx_out_index toi = m_db->get_output_tx_and_index(0, i);

      // if the output's transaction is unlocked, add the output's index to
      // our list.
      if (is_tx_spendtime_unlocked(m_db->get_tx_unlock_time(toi.first)))
      {
        add_out_to_get_rct_random_outs(res.outs, 0, i);
      }
    }
  }

  if (res.outs.size() < req.outs_count)
    return false;
#if 0
  // if we do not have enough RCT inputs, we can pick from the non RCT ones
  // which will have a zero mask
  if (res.outs.size() < req.outs_count)
  {
    LOG_PRINT_L0("Out of RCT inputs (" << res.outs.size() << "/" << req.outs_count << "), using regular ones");

    // TODO: arbitrary selection, needs better
    COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::request req2 = AUTO_VAL_INIT(req2);
    COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::response res2 = AUTO_VAL_INIT(res2);
    req2.outs_count = req.outs_count - res.outs.size();
    static const uint64_t amounts[] = {1, 10, 20, 50, 100, 200, 500, 1000, 10000};
    for (uint64_t a: amounts)
      req2.amounts.push_back(a);
    if (!get_random_outs_for_amounts(req2, res2))
      return false;

    // pick random ones from there
    while (res.outs.size() < req.outs_count)
    {
      int list_idx = rand() % (sizeof(amounts)/sizeof(amounts[0]));
      if (!res2.outs[list_idx].outs.empty())
      {
        const COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::out_entry oe = res2.outs[list_idx].outs.back();
        res2.outs[list_idx].outs.pop_back();
        add_out_to_get_rct_random_outs(res.outs, res2.outs[list_idx].amount, oe.global_amount_index);
      }
    }
  }
#endif

  return true;
}
//------------------------------------------------------------------
bool Blockchain::get_outs(const COMMAND_RPC_GET_OUTPUTS_BIN::request& req, COMMAND_RPC_GET_OUTPUTS_BIN::response& res) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);

  res.outs.clear();
  res.outs.reserve(req.outputs.size());
  for (const auto &i: req.outputs)
  {
    // get tx_hash, tx_out_index from DB
    const output_data_t od = m_db->get_output_key(i.amount, i.index);
    tx_out_index toi = m_db->get_output_tx_and_index(i.amount, i.index);
    bool unlocked = is_tx_spendtime_unlocked(m_db->get_tx_unlock_time(toi.first));

    res.outs.push_back({od.pubkey, od.commitment, unlocked});
  }
  return true;
}
//------------------------------------------------------------------
// This function takes a list of block hashes from another node
// on the network to find where the split point is between us and them.
// This is used to see what to send another node that needs to sync.
bool Blockchain::find_blockchain_supplement(const std::list<crypto::hash>& qblock_ids, uint64_t& starter_offset) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);

  // make sure the request includes at least the genesis block, otherwise
  // how can we expect to sync from the client that the block list came from?
  if(!qblock_ids.size() /*|| !req.m_total_height*/)
  {
    LOG_PRINT_L1("Client sent wrong NOTIFY_REQUEST_CHAIN: m_block_ids.size()=" << qblock_ids.size() << /*", m_height=" << req.m_total_height <<*/ ", dropping connection");
    return false;
  }

  m_db->block_txn_start(true);
  // make sure that the last block in the request's block list matches
  // the genesis block
  auto gen_hash = m_db->get_block_hash_from_height(0);
  if(qblock_ids.back() != gen_hash)
  {
    LOG_PRINT_L1("Client sent wrong NOTIFY_REQUEST_CHAIN: genesis block mismatch: " << std::endl << "id: " << qblock_ids.back() << ", " << std::endl << "expected: " << gen_hash << "," << std::endl << " dropping connection");
	m_db->block_txn_abort();
    return false;
  }

  // Find the first block the foreign chain has that we also have.
  // Assume qblock_ids is in reverse-chronological order.
  auto bl_it = qblock_ids.begin();
  uint64_t split_height = 0;
  for(; bl_it != qblock_ids.end(); bl_it++)
  {
    try
    {
      if (m_db->block_exists(*bl_it, &split_height))
        break;
    }
    catch (const std::exception& e)
    {
      LOG_PRINT_L1("Non-critical error trying to find block by hash in BlockchainDB, hash: " << *bl_it);
	  m_db->block_txn_abort();
      return false;
    }
  }
  m_db->block_txn_stop();

  // this should be impossible, as we checked that we share the genesis block,
  // but just in case...
  if(bl_it == qblock_ids.end())
  {
    LOG_PRINT_L1("Internal error handling connection, can't find split point");
    return false;
  }

  //we start to put block ids INCLUDING last known id, just to make other side be sure
  starter_offset = split_height;
  return true;
}
//------------------------------------------------------------------
uint64_t Blockchain::block_difficulty(uint64_t i) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);
  try
  {
    return m_db->get_block_difficulty(i);
  }
  catch (const BLOCK_DNE& e)
  {
    LOG_PRINT_L0("Attempted to get block difficulty for height above blockchain height");
  }
  return 0;
}
//------------------------------------------------------------------
//TODO: return type should be void, throw on exception
//       alternatively, return true only if no blocks missed
template<class t_ids_container, class t_blocks_container, class t_missed_container>
bool Blockchain::get_blocks(const t_ids_container& block_ids, t_blocks_container& blocks, t_missed_container& missed_bs) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);

  for (const auto& block_hash : block_ids)
  {
    try
    {
      blocks.push_back(m_db->get_block(block_hash));
    }
    catch (const BLOCK_DNE& e)
    {
      missed_bs.push_back(block_hash);
    }
    catch (const std::exception& e)
    {
      return false;
    }
  }
  return true;
}
//------------------------------------------------------------------
//TODO: return type should be void, throw on exception
//       alternatively, return true only if no transactions missed
template<class t_ids_container, class t_tx_container, class t_missed_container>
bool Blockchain::get_transactions(const t_ids_container& txs_ids, t_tx_container& txs, t_missed_container& missed_txs) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);

  for (const auto& tx_hash : txs_ids)
  {
    try
    {
      txs.push_back(m_db->get_tx(tx_hash));
    }
    catch (const TX_DNE& e)
    {
      missed_txs.push_back(tx_hash);
    }
    catch (const std::exception& e)
    {
      return false;
    }
  }
  return true;
}
//------------------------------------------------------------------
void Blockchain::print_blockchain(uint64_t start_index, uint64_t end_index) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  std::stringstream ss;
  CRITICAL_REGION_LOCAL(m_blockchain_lock);
  auto h = m_db->height();
  if(start_index > h)
  {
    LOG_PRINT_L1("Wrong starter index set: " << start_index << ", expected max index " << h);
    return;
  }

  for(size_t i = start_index; i <= h && i != end_index; i++)
  {
    ss << "height " << i << ", timestamp " << m_db->get_block_timestamp(i) << ", cumul_dif " << m_db->get_block_cumulative_difficulty(i) << ", size " << m_db->get_block_size(i) << "\nid\t\t" << m_db->get_block_hash_from_height(i) << "\ndifficulty\t\t" << m_db->get_block_difficulty(i) << ", nonce " << m_db->get_block_from_height(i).nonce << ", tx_count " << m_db->get_block_from_height(i).tx_hashes.size() << std::endl;
  }
  LOG_PRINT_L1("Current blockchain:" << std::endl << ss.str());
  LOG_PRINT_L0("Blockchain printed with log level 1");
}
//------------------------------------------------------------------
void Blockchain::print_blockchain_index() const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  std::stringstream ss;
  CRITICAL_REGION_LOCAL(m_blockchain_lock);
  auto height = m_db->height();
  if (height != 0)
  {
    for(uint64_t i = 0; i <= height; i++)
    {
      ss << "height: " << i << ", hash: " << m_db->get_block_hash_from_height(i);
    }
  }

  LOG_PRINT_L0("Current blockchain index:" << std::endl << ss.str());
}
//------------------------------------------------------------------
//TODO: remove this function and references to it
void Blockchain::print_blockchain_outs(const std::string& file) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  return;
}
//------------------------------------------------------------------
// Find the split point between us and foreign blockchain and return
// (by reference) the most recent common block hash along with up to
// BLOCKS_IDS_SYNCHRONIZING_DEFAULT_COUNT additional (more recent) hashes.
bool Blockchain::find_blockchain_supplement(const std::list<crypto::hash>& qblock_ids, NOTIFY_RESPONSE_CHAIN_ENTRY::request& resp) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);

  // if we can't find the split point, return false
  if(!find_blockchain_supplement(qblock_ids, resp.start_height))
  {
    return false;
  }

  resp.total_height = get_current_blockchain_height();
  size_t count = 0;
  for(size_t i = resp.start_height; i < resp.total_height && count < BLOCKS_IDS_SYNCHRONIZING_DEFAULT_COUNT; i++, count++)
  {
    resp.m_block_ids.push_back(m_db->get_block_hash_from_height(i));
  }
  return true;
}
//------------------------------------------------------------------
//FIXME: change argument to std::vector, low priority
// find split point between ours and foreign blockchain (or start at
// blockchain height <req_start_block>), and return up to max_count FULL
// blocks by reference.
bool Blockchain::find_blockchain_supplement(const uint64_t req_start_block, const std::list<crypto::hash>& qblock_ids, std::list<std::pair<block, std::list<transaction> > >& blocks, uint64_t& total_height, uint64_t& start_height, size_t max_count) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);

  // if a specific start height has been requested
  if(req_start_block > 0)
  {
    // if requested height is higher than our chain, return false -- we can't help
    if (req_start_block >= m_db->height())
    {
      return false;
    }
    start_height = req_start_block;
  }
  else
  {
    if(!find_blockchain_supplement(qblock_ids, start_height))
    {
      return false;
    }
  }

  total_height = get_current_blockchain_height();
  size_t count = 0;
  for(size_t i = start_height; i < total_height && count < max_count; i++, count++)
  {
    blocks.resize(blocks.size()+1);
    blocks.back().first = m_db->get_block_from_height(i);
    std::list<crypto::hash> mis;
    get_transactions(blocks.back().first.tx_hashes, blocks.back().second, mis);
    CHECK_AND_ASSERT_MES(!mis.size(), false, "internal error, transaction from block not found");
  }
  return true;
}
//------------------------------------------------------------------
bool Blockchain::add_block_as_invalid(const block& bl, const crypto::hash& h)
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  block_extended_info bei = AUTO_VAL_INIT(bei);
  bei.bl = bl;
  return add_block_as_invalid(bei, h);
}
//------------------------------------------------------------------
bool Blockchain::add_block_as_invalid(const block_extended_info& bei, const crypto::hash& h)
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);
  auto i_res = m_invalid_blocks.insert(std::map<crypto::hash, block_extended_info>::value_type(h, bei));
  CHECK_AND_ASSERT_MES(i_res.second, false, "at insertion invalid by tx returned status existed");
  LOG_PRINT_L1("BLOCK ADDED AS INVALID: " << h << std::endl << ", prev_id=" << bei.bl.prev_id << ", m_invalid_blocks count=" << m_invalid_blocks.size());
  return true;
}
//------------------------------------------------------------------
bool Blockchain::have_block(const crypto::hash& id) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);

  if(m_db->block_exists(id))
  {
    LOG_PRINT_L3("block exists in main chain");
    return true;
  }

  if(m_alternative_chains.count(id))
  {
    LOG_PRINT_L3("block found in m_alternative_chains");
    return true;
  }

  if(m_invalid_blocks.count(id))
  {
    LOG_PRINT_L3("block found in m_invalid_blocks");
    return true;
  }

  return false;
}
//------------------------------------------------------------------
bool Blockchain::handle_block_to_main_chain(const block& bl, block_verification_context& bvc)
{
    LOG_PRINT_L3("Blockchain::" << __func__);
    crypto::hash id = get_block_hash(bl);
    return handle_block_to_main_chain(bl, id, bvc);
}
//------------------------------------------------------------------
size_t Blockchain::get_total_transactions() const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);
  return m_db->get_tx_count();
}
//------------------------------------------------------------------
// This function checks each input in the transaction <tx> to make sure it
// has not been used already, and adds its key to the container <keys_this_block>.
//
// This container should be managed by the code that validates blocks so we don't
// have to store the used keys in a given block in the permanent storage only to
// remove them later if the block fails validation.
bool Blockchain::check_for_double_spend(const transaction& tx, key_images_container& keys_this_block) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);
  struct add_transaction_input_visitor: public boost::static_visitor<bool>
  {
    key_images_container& m_spent_keys;
    BlockchainDB* m_db;
    add_transaction_input_visitor(key_images_container& spent_keys, BlockchainDB* db) :
      m_spent_keys(spent_keys), m_db(db)
    {
    }
    bool operator()(const txin_to_key& in) const
    {
      const crypto::key_image& ki = in.k_image;

      // attempt to insert the newly-spent key into the container of
      // keys spent this block.  If this fails, the key was spent already
      // in this block, return false to flag that a double spend was detected.
      //
      // if the insert into the block-wide spent keys container succeeds,
      // check the blockchain-wide spent keys container and make sure the
      // key wasn't used in another block already.
      auto r = m_spent_keys.insert(ki);
      if(!r.second || m_db->has_key_image(ki))
      {
        //double spend detected
        return false;
      }

      // if no double-spend detected, return true
      return true;
    }

    bool operator()(const txin_gen& tx) const
    {
      return true;
    }
    bool operator()(const txin_to_script& tx) const
    {
      return false;
    }
    bool operator()(const txin_to_scripthash& tx) const
    {
      return false;
    }
  };

  for (const txin_v& in : tx.vin)
  {
    if(!boost::apply_visitor(add_transaction_input_visitor(keys_this_block, m_db), in))
    {
      LOG_ERROR("Double spend detected!");
      return false;
    }
  }

  return true;
}
//------------------------------------------------------------------
bool Blockchain::get_tx_outputs_gindexs(const crypto::hash& tx_id, std::vector<uint64_t>& indexs) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);
  uint64_t tx_index;
  if (!m_db->tx_exists(tx_id, tx_index))
  {
    LOG_PRINT_RED_L1("warning: get_tx_outputs_gindexs failed to find transaction with id = " << tx_id);
    return false;
  }

  // get amount output indexes, currently referred to in parts as "output global indices", but they are actually specific to amounts
  indexs = m_db->get_tx_amount_output_indices(tx_index);
  if (indexs.empty())
  {
    // empty indexs is only valid if the vout is empty, which is legal but rare
    cryptonote::transaction tx = m_db->get_tx(tx_id);
    CHECK_AND_ASSERT_MES(tx.vout.empty(), false, "internal error: global indexes for transaction " << tx_id << " is empty, and tx vout is not");
  }

  return true;
}
//------------------------------------------------------------------
//FIXME: it seems this function is meant to be merely a wrapper around
//       another function of the same name, this one adding one bit of
//       functionality.  Should probably move anything more than that
//       (getting the hash of the block at height max_used_block_id)
//       to the other function to keep everything in one place.
// This function overloads its sister function with
// an extra value (hash of highest block that holds an output used as input)
// as a return-by-reference.
bool Blockchain::check_tx_inputs(transaction& tx, uint64_t& max_used_block_height, crypto::hash& max_used_block_id, tx_verification_context &tvc, bool kept_by_block)
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);

#if defined(PER_BLOCK_CHECKPOINT)
  // check if we're doing per-block checkpointing
  // FIXME: investigate why this block returns
  if (m_db->height() < m_blocks_hash_check.size() && kept_by_block)
  {
    TIME_MEASURE_START(a);
    m_blocks_txs_check.push_back(get_transaction_hash(tx));
    max_used_block_id = null_hash;
    max_used_block_height = 0;
    TIME_MEASURE_FINISH(a);
    if(m_show_time_stats)
      LOG_PRINT_L0("HASH: " << "-" << " VIN/VOUT: " << tx.vin.size() << "/" << tx.vout.size() << " H: " << 0 << " chcktx: " << a);
    return true;
  }
#endif

  TIME_MEASURE_START(a);
  bool res = check_tx_inputs(tx, tvc, &max_used_block_height);
  TIME_MEASURE_FINISH(a);
  if(m_show_time_stats)
    LOG_PRINT_L0("HASH: " << "+" << " VIN/VOUT: " << tx.vin.size() << "/" << tx.vout.size() << " H: " << max_used_block_height << " chcktx: " << a + m_fake_scan_time);

  if (!res)
    return false;

  CHECK_AND_ASSERT_MES(max_used_block_height < m_db->height(), false,  "internal error: max used block index=" << max_used_block_height << " is not less then blockchain size = " << m_db->height());
  max_used_block_id = m_db->get_block_hash_from_height(max_used_block_height);
  return true;
}
//------------------------------------------------------------------
bool Blockchain::check_tx_outputs(const transaction& tx, tx_verification_context &tvc)
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);

  // in a v2 tx, all outputs must have 0 amount
  for (auto &o: tx.vout) {
    if (o.amount != 0) {
      tvc.m_invalid_output = true;
      return false;
    }
  }

  // from v4, forbid invalid pubkeys
  for (const auto &o: tx.vout) {
    if (o.target.type() == typeid(txout_to_key)) {
      const txout_to_key& out_to_key = boost::get<txout_to_key>(o.target);
      if (!crypto::check_key(out_to_key.key)) {
        tvc.m_invalid_output = true;
        return false;
      }
    }
  }

  return true;
}
//------------------------------------------------------------------
bool Blockchain::have_tx_keyimges_as_spent(const transaction &tx) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  for (const txin_v& in: tx.vin)
  {
    CHECKED_GET_SPECIFIC_VARIANT(in, const txin_to_key, in_to_key, true);
    if(have_tx_keyimg_as_spent(in_to_key.k_image))
      return true;
  }
  return false;
}
bool Blockchain::expand_transaction_2(transaction &tx, const crypto::hash &tx_prefix_hash, const std::vector<std::vector<rct::ctkey>> &pubkeys)
{
  PERF_TIMER(expand_transaction_2);
  CHECK_AND_ASSERT_MES(tx.version == 2, false, "Transaction version is not 2");

  rct::rctSig &rv = tx.rct_signatures;

  // message - hash of the transaction prefix
  rv.message = rct::hash2rct(tx_prefix_hash);

  // mixRing - full and simple store it in opposite ways
  if (rv.type == rct::RCTTypeFull)
  {
    rv.mixRing.resize(pubkeys[0].size());
    for (size_t m = 0; m < pubkeys[0].size(); ++m)
      rv.mixRing[m].clear();
    for (size_t n = 0; n < pubkeys.size(); ++n)
    {
      CHECK_AND_ASSERT_MES(pubkeys[n].size() <= pubkeys[0].size(), false, "More inputs that first ring");
      for (size_t m = 0; m < pubkeys[n].size(); ++m)
      {
        rv.mixRing[m].push_back(pubkeys[n][m]);
      }
    }
  }
  else if (rv.type == rct::RCTTypeSimple)
  {
    rv.mixRing.resize(pubkeys.size());
    for (size_t n = 0; n < pubkeys.size(); ++n)
    {
      rv.mixRing[n].clear();
      for (size_t m = 0; m < pubkeys[n].size(); ++m)
      {
        rv.mixRing[n].push_back(pubkeys[n][m]);
      }
    }
  }
  else
  {
    CHECK_AND_ASSERT_MES(false, false, "Unsupported rct tx type: " + boost::lexical_cast<std::string>(rv.type));
  }

  // II
  if (rv.type == rct::RCTTypeFull)
  {
    rv.p.MGs.resize(1);
    rv.p.MGs[0].II.resize(tx.vin.size());
    for (size_t n = 0; n < tx.vin.size(); ++n)
      rv.p.MGs[0].II[n] = rct::ki2rct(boost::get<txin_to_key>(tx.vin[n]).k_image);
  }
  else if (rv.type == rct::RCTTypeSimple)
  {
    CHECK_AND_ASSERT_MES(rv.p.MGs.size() == tx.vin.size(), false, "Bad MGs size");
    for (size_t n = 0; n < tx.vin.size(); ++n)
    {
      rv.p.MGs[n].II.resize(1);
      rv.p.MGs[n].II[0] = rct::ki2rct(boost::get<txin_to_key>(tx.vin[n]).k_image);
    }
  }
  else
  {
    CHECK_AND_ASSERT_MES(false, false, "Unsupported rct tx type: " + boost::lexical_cast<std::string>(rv.type));
  }

  // outPk
  CHECK_AND_ASSERT_MES(rv.outPk.size() == tx.vout.size(), false, "Bad outPk size");
  for (size_t n = 0; n < tx.rct_signatures.outPk.size(); ++n)
    rv.outPk[n].dest = rct::pk2rct(boost::get<txout_to_key>(tx.vout[n].target).key);

  return true;
}
//------------------------------------------------------------------
// This function validates transaction inputs and their keys.
// FIXME: consider moving functionality specific to one input into
//        check_tx_input() rather than here, and use this function simply
//        to iterate the inputs as necessary (splitting the task
//        using threads, etc.)
bool Blockchain::check_tx_inputs(transaction& tx, tx_verification_context &tvc, uint64_t* pmax_used_block_height)
{
  PERF_TIMER(check_tx_inputs);
  LOG_PRINT_L3("Blockchain::" << __func__);
  size_t sig_index = 0;
  if(pmax_used_block_height)
    *pmax_used_block_height = 0;

  crypto::hash tx_prefix_hash = get_transaction_prefix_hash(tx);

  // from hard fork 2, we require mixin at least 2 unless one output cannot mix with 2 others
  // if one output cannot mix with 2 others, we accept at most 1 output that can mix
  size_t n_unmixable = 0, n_mixable = 0;
  size_t mixin = std::numeric_limits<size_t>::max();
  //const size_t min_mixin = hf_version >= 5 ? 4 : 2;
  const size_t min_mixin = DEFAULT_MIXIN;
  const size_t max_mixin = MAX_MIXIN;
  for (const auto& txin : tx.vin)
  {
    // non txin_to_key inputs will be rejected below
    if (txin.type() == typeid(txin_to_key))
    {
      const txin_to_key& in_to_key = boost::get<txin_to_key>(txin);
      if (in_to_key.amount == 0)
      {
        // always consider rct inputs mixable. Even if there's not enough rct
        // inputs on the chain to mix with, this is going to be the case for
        // just a few blocks right after the fork at most
        ++n_mixable;
      }
      else
      {
        uint64_t n_outputs = m_db->get_num_outputs(in_to_key.amount);
        LOG_PRINT_L2("output size " << print_money(in_to_key.amount) << ": " << n_outputs << " available");
        // n_outputs includes the output we're considering
        if (n_outputs <= min_mixin)
          ++n_unmixable;
        else
          ++n_mixable;
      }
      if (in_to_key.key_offsets.size() - 1 < mixin)
        mixin = in_to_key.key_offsets.size() - 1;
    }
  }

  if (mixin < min_mixin)
  {
    if (n_unmixable == 0)
    {
      LOG_PRINT_L1("Tx " << get_transaction_hash(tx) << " has too low mixin (" << mixin << "), and no unmixable inputs");
      tvc.m_low_mixin = true;
      return false;
    }
    if (n_mixable > 1)
    {
      LOG_PRINT_L1("Tx " << get_transaction_hash(tx) << " has too low mixin (" << mixin << "), and more than one mixable input with unmixable inputs");
      tvc.m_low_mixin = true;
      return false;
    }
  }

  if (mixin > max_mixin)
  {
    LOG_PRINT_L1("Tx " << get_transaction_hash(tx) << " has too high mixin (" << mixin << "), max mixin = " << max_mixin);
    tvc.m_high_mixin = true;
    return false;
  }


  // min/max tx version based on HF, and we accept v1 txes if having a non mixable
  const size_t max_tx_version = CURRENT_TRANSACTION_VERSION;
  if (tx.version > max_tx_version)
  {
    LOG_PRINT_L1("transaction version " << (unsigned)tx.version << " is higher than max accepted version " << max_tx_version);
    tvc.m_verifivation_failed = true;
    return false;
  }
  const size_t min_tx_version = MIN_TRANSACTION_VERSION;
  if (tx.version < min_tx_version)
  {
    LOG_PRINT_L1("transaction version " << (unsigned)tx.version << " is lower than min accepted version " << min_tx_version);
    tvc.m_verifivation_failed = true;
    return false;
  }


  auto it = m_check_txin_table.find(tx_prefix_hash);
  if(it == m_check_txin_table.end())
  {
    m_check_txin_table.emplace(tx_prefix_hash, std::unordered_map<crypto::key_image, bool>());
    it = m_check_txin_table.find(tx_prefix_hash);
    assert(it != m_check_txin_table.end());
  }

  uint64_t t_t1 = 0;
  std::vector<std::vector<rct::ctkey>> pubkeys(tx.vin.size());
  std::vector < uint64_t > results;
  results.resize(tx.vin.size(), 0);

  int threads = tools::get_max_concurrency();

  boost::asio::io_service ioservice;
  boost::thread_group threadpool;
  bool ioservice_active = false;

  std::unique_ptr < boost::asio::io_service::work > work(new boost::asio::io_service::work(ioservice));
  if(threads > 1)
  {
    for (int i = 0; i < threads; i++)
    {
      threadpool.create_thread(boost::bind(&boost::asio::io_service::run, &ioservice));
    }
    ioservice_active = true;
  }

#define KILL_IOSERVICE()  \
        if(ioservice_active) \
        { \
            work.reset(); \
            while (!ioservice.stopped()) ioservice.poll(); \
            threadpool.join_all(); \
            ioservice.stop(); \
            ioservice_active = false; \
        }

  epee::misc_utils::auto_scope_leave_caller ioservice_killer = epee::misc_utils::create_scope_leave_handler([&]() { KILL_IOSERVICE(); });

  for (const auto& txin : tx.vin)
  {
    // make sure output being spent is of type txin_to_key, rather than
    // e.g. txin_gen, which is only used for miner transactions
    CHECK_AND_ASSERT_MES(txin.type() == typeid(txin_to_key), false, "wrong type id in tx input at Blockchain::check_tx_inputs");
    const txin_to_key& in_to_key = boost::get<txin_to_key>(txin);

    // make sure tx output has key offset(s) (is signed to be used)
    CHECK_AND_ASSERT_MES(in_to_key.key_offsets.size(), false, "empty in_to_key.key_offsets in transaction with id " << get_transaction_hash(tx));

    if(have_tx_keyimg_as_spent(in_to_key.k_image))
    {
      LOG_PRINT_L1("Key image already spent in blockchain: " << epee::string_tools::pod_to_hex(in_to_key.k_image));
      tvc.m_double_spend = true;
      return false;
    }

    if (tx.version == 1)
    {
      // basically, make sure number of inputs == number of signatures
      CHECK_AND_ASSERT_MES(sig_index < tx.signatures.size(), false, "wrong transaction: not signature entry for input with index= " << sig_index);

#if defined(CACHE_VIN_RESULTS)
      auto itk = it->second.find(in_to_key.k_image);
      if(itk != it->second.end())
      {
        if(!itk->second)
        {
          LOG_PRINT_L1("Failed ring signature for tx " << get_transaction_hash(tx) << "  vin key with k_image: " << in_to_key.k_image << "  sig_index: " << sig_index);
          return false;
        }

        // txin has been verified already, skip
        sig_index++;
        continue;
      }
#endif
    }

    // make sure that output being spent matches up correctly with the
    // signature spending it.
    if (!check_tx_input(tx.version, in_to_key, tx_prefix_hash, std::vector<crypto::signature>(), tx.rct_signatures, pubkeys[sig_index], pmax_used_block_height))
    {
      it->second[in_to_key.k_image] = false;
      LOG_PRINT_L1("Failed to check ring signature for tx " << get_transaction_hash(tx) << "  vin key with k_image: " << in_to_key.k_image << "  sig_index: " << sig_index);
      if (pmax_used_block_height) // a default value of NULL is used when called from Blockchain::handle_block_to_main_chain()
      {
        LOG_PRINT_L1("  *pmax_used_block_height: " << *pmax_used_block_height);
      }

      return false;
    }

    sig_index++;
  }

  KILL_IOSERVICE();

  if (!expand_transaction_2(tx, tx_prefix_hash, pubkeys))
  {
    LOG_PRINT_L1("Failed to expand rct signatures!");
    return false;
  }

  // from version 2, check ringct signatures
  // obviously, the original and simple rct APIs use a mixRing that's indexes
  // in opposite orders, because it'd be too simple otherwise...
  const rct::rctSig &rv = tx.rct_signatures;
  switch (rv.type)
  {
  case rct::RCTTypeNull: {
    // we only accept no signatures for coinbase txes
    LOG_PRINT_L1("Null rct signature on non-coinbase tx");
    return false;
  }
  case rct::RCTTypeSimple: {
    // check all this, either recontructed (so should really pass), or not
    {
      if (pubkeys.size() != rv.mixRing.size())
      {
        LOG_PRINT_L1("Failed to check ringct signatures: mismatched pubkeys/mixRing size");
        return false;
      }
      for (size_t i = 0; i < pubkeys.size(); ++i)
      {
        if (pubkeys[i].size() != rv.mixRing[i].size())
        {
          LOG_PRINT_L1("Failed to check ringct signatures: mismatched pubkeys/mixRing size");
          return false;
        }
      }

      for (size_t n = 0; n < pubkeys.size(); ++n)
      {
        for (size_t m = 0; m < pubkeys[n].size(); ++m)
        {
          if (pubkeys[n][m].dest != rct::rct2pk(rv.mixRing[n][m].dest))
          {
            LOG_PRINT_L1("Failed to check ringct signatures: mismatched pubkey at vin " << n << ", index " << m);
            return false;
          }
          if (pubkeys[n][m].mask != rct::rct2pk(rv.mixRing[n][m].mask))
          {
            LOG_PRINT_L1("Failed to check ringct signatures: mismatched commitment at vin " << n << ", index " << m);
            return false;
          }
        }
      }
    }

    if (rv.p.MGs.size() != tx.vin.size())
    {
      LOG_PRINT_L1("Failed to check ringct signatures: mismatched MGs/vin sizes");
      return false;
    }
    for (size_t n = 0; n < tx.vin.size(); ++n)
    {
      if (memcmp(&boost::get<txin_to_key>(tx.vin[n]).k_image, &rv.p.MGs[n].II[0], 32))
      {
        LOG_PRINT_L1("Failed to check ringct signatures: mismatched key image");
        return false;
      }
    }

    if (!rct::verRctSimple(rv))
    {
      LOG_PRINT_L1("Failed to check ringct signatures!");
      return false;
    }
    break;
  }
  case rct::RCTTypeFull: {
    // check all this, either recontructed (so should really pass), or not
    {
      bool size_matches = true;
      for (size_t i = 0; i < pubkeys.size(); ++i)
        size_matches &= pubkeys[i].size() == rv.mixRing.size();
      for (size_t i = 0; i < rv.mixRing.size(); ++i)
        size_matches &= pubkeys.size() == rv.mixRing[i].size();
      if (!size_matches)
      {
        LOG_PRINT_L1("Failed to check ringct signatures: mismatched pubkeys/mixRing size");
        return false;
      }

      for (size_t n = 0; n < pubkeys.size(); ++n)
      {
        for (size_t m = 0; m < pubkeys[n].size(); ++m)
        {
          if (pubkeys[n][m].dest != rct::rct2pk(rv.mixRing[m][n].dest))
          {
            LOG_PRINT_L1("Failed to check ringct signatures: mismatched pubkey at vin " << n << ", index " << m);
            return false;
          }
          if (pubkeys[n][m].mask != rct::rct2pk(rv.mixRing[m][n].mask))
          {
            LOG_PRINT_L1("Failed to check ringct signatures: mismatched commitment at vin " << n << ", index " << m);
            return false;
          }
        }
      }
    }

    if (rv.p.MGs.size() != 1)
    {
      LOG_PRINT_L1("Failed to check ringct signatures: Bad MGs size");
      return false;
    }
    if (rv.p.MGs[0].II.size() != tx.vin.size())
    {
      LOG_PRINT_L1("Failed to check ringct signatures: mismatched II/vin sizes");
      return false;
    }
    for (size_t n = 0; n < tx.vin.size(); ++n)
    {
      if (memcmp(&boost::get<txin_to_key>(tx.vin[n]).k_image, &rv.p.MGs[0].II[n], 32))
      {
        LOG_PRINT_L1("Failed to check ringct signatures: mismatched II/vin sizes");
        return false;
      }
    }

    if (!rct::verRct(rv))
    {
      LOG_PRINT_L1("Failed to check ringct signatures!");
      return false;
    }
    break;
  }
  default:
    LOG_PRINT_L1("Unsupported rct type: " << rv.type);
    return false;
  }

  return true;
}

//------------------------------------------------------------------
void Blockchain::check_ring_signature(const crypto::hash &tx_prefix_hash, const crypto::key_image &key_image, const std::vector<rct::ctkey> &pubkeys, const std::vector<crypto::signature>& sig, uint64_t &result)
{
  if (m_is_in_checkpoint_zone)
  {
    result = true;
    return;
  }

  std::vector<const crypto::public_key *> p_output_keys;
  for (auto &key : pubkeys)
  {
    // rct::key and crypto::public_key have the same structure, avoid object ctor/memcpy
    p_output_keys.push_back(&(const crypto::public_key&)key.dest);
  }

  result = crypto::check_ring_signature(tx_prefix_hash, key_image, p_output_keys, sig.data()) ? 1 : 0;
}

//------------------------------------------------------------------
uint64_t Blockchain::get_dynamic_per_kb_fee(uint64_t block_reward, size_t median_block_size)
{
  size_t block_growth_zone;
  if (get_current_hard_fork_version() > 2) {
    block_growth_zone = BLOCK_SIZE_GROWTH_FAVORED_ZONE_V2;
  } else {
    block_growth_zone = BLOCK_SIZE_GROWTH_FAVORED_ZONE;
  }
  if (median_block_size < block_growth_zone)
    median_block_size = block_growth_zone;

  // this to avoid full block fee getting too low when block reward decline, i.e. easier for "block filler" attack
  if (block_reward < DYNAMIC_FEE_PER_KB_BASE_BLOCK_REWARD)
    block_reward = DYNAMIC_FEE_PER_KB_BASE_BLOCK_REWARD;

  uint64_t unscaled_fee_per_kb = (DYNAMIC_FEE_PER_KB_BASE_FEE * block_growth_zone / median_block_size);
  uint64_t hi, lo = mul128(unscaled_fee_per_kb, block_reward, &hi);
  static_assert(DYNAMIC_FEE_PER_KB_BASE_BLOCK_REWARD % 1000000 == 0, "DYNAMIC_FEE_PER_KB_BASE_BLOCK_REWARD must be divisible by 1000000");
  static_assert(DYNAMIC_FEE_PER_KB_BASE_BLOCK_REWARD / 1000000 <= std::numeric_limits<uint32_t>::max(), "DYNAMIC_FEE_PER_KB_BASE_BLOCK_REWARD is too large");
  // divide in two steps, since the divisor must be 32 bits, but DYNAMIC_FEE_PER_KB_BASE_BLOCK_REWARD isn't
  div128_32(hi, lo, DYNAMIC_FEE_PER_KB_BASE_BLOCK_REWARD / 1000000, &hi, &lo);
  div128_32(hi, lo, 1000000, &hi, &lo);
  assert(hi == 0);

  return lo;
}

//------------------------------------------------------------------
bool Blockchain::check_fee(size_t blob_size, uint64_t fee)
{
  uint64_t fee_per_kb;
  uint64_t median = m_current_block_cumul_sz_limit / 2;
  uint64_t height = m_db->height();
  uint64_t cal_height = height - height % COIN_EMISSION_HEIGHT_INTERVAL;
  uint64_t cal_generated_coins = cal_height ? m_db->get_block_already_generated_coins(cal_height - 1) : 0;
  uint64_t base_reward;
  if (!get_block_reward(median, 1, cal_generated_coins, base_reward, height, get_current_hard_fork_version()))
    return false;
  fee_per_kb = get_dynamic_per_kb_fee(base_reward, median);

  LOG_PRINT_L2("Using " << print_money(fee) << "/kB fee");

  float kB = (blob_size - CRYPTONOTE_COINBASE_BLOB_RESERVED_SIZE) * 1.0f / 1024;
  uint64_t needed_fee = ((uint64_t)(kB * fee_per_kb)) / 100 * 100;

  if (fee < needed_fee)
  {
    LOG_PRINT_L1("transaction fee is not enough: " << print_money(fee) << ", minimum fee: " << print_money(needed_fee));
    return false;
  }
  return true;
}

//------------------------------------------------------------------
uint64_t Blockchain::get_dynamic_per_kb_fee_estimate(uint64_t grace_blocks)
{
  if (grace_blocks >= CRYPTONOTE_REWARD_BLOCKS_WINDOW)
    grace_blocks = CRYPTONOTE_REWARD_BLOCKS_WINDOW - 1;

  std::vector<size_t> sz;
  get_last_n_blocks_sizes(sz, CRYPTONOTE_REWARD_BLOCKS_WINDOW - grace_blocks);
  for (size_t i = 0; i < grace_blocks; ++i)
    sz.push_back(CRYPTONOTE_BLOCK_GRANTED_FULL_REWARD_ZONE);

  uint64_t median = epee::misc_utils::median(sz);
  if(median <= CRYPTONOTE_BLOCK_GRANTED_FULL_REWARD_ZONE)
    median = CRYPTONOTE_BLOCK_GRANTED_FULL_REWARD_ZONE;

  //uint64_t already_generated_coins = m_db->height() ? m_db->get_block_already_generated_coins(m_db->height() - 1) : 0;
  uint64_t height = m_db->height();
  uint64_t cal_height = height - height % COIN_EMISSION_HEIGHT_INTERVAL;
  uint64_t cal_generated_coins = cal_height ? m_db->get_block_already_generated_coins(cal_height - 1) : 0;
  uint64_t base_reward;
  if (!get_block_reward(median, 1, cal_generated_coins, base_reward, height, get_current_hard_fork_version()))
  {
    LOG_PRINT_L1("Failed to determine block reward, using placeholder " << print_money(BLOCK_REWARD_OVERESTIMATE) << " as a high bound");
    base_reward = BLOCK_REWARD_OVERESTIMATE;
  }

  uint64_t fee = get_dynamic_per_kb_fee(base_reward, median);
  LOG_PRINT_L2("Estimating " << grace_blocks << "-block fee at " << print_money(fee) << "/kB");
  return fee;
}

//------------------------------------------------------------------
// This function checks to see if a tx is unlocked.  unlock_time is either
// a block index or a unix time.
bool Blockchain::is_tx_spendtime_unlocked(uint64_t unlock_time) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  if(unlock_time < CRYPTONOTE_MAX_BLOCK_NUMBER)
  {
    // ND: Instead of calling get_current_blockchain_height(), call m_db->height()
    //    directly as get_current_blockchain_height() locks the recursive mutex.
    if(m_db->height()-1 + CRYPTONOTE_LOCKED_TX_ALLOWED_DELTA_BLOCKS >= unlock_time)
      return true;
    else
      return false;
  }
  else
  {
    //interpret as time
    uint64_t current_time = static_cast<uint64_t>(time(NULL));
    if(current_time + (CRYPTONOTE_LOCKED_TX_ALLOWED_DELTA_SECONDS) >= unlock_time)
      return true;
    else
      return false;
  }
  return false;
}
//------------------------------------------------------------------
// This function locates all outputs associated with a given input (mixins)
// and validates that they exist and are usable.  It also checks the ring
// signature for each input.
bool Blockchain::check_tx_input(size_t tx_version, const txin_to_key& txin, const crypto::hash& tx_prefix_hash, const std::vector<crypto::signature>& sig, const rct::rctSig &rct_signatures, std::vector<rct::ctkey> &output_keys, uint64_t* pmax_related_block_height)
{
  LOG_PRINT_L3("Blockchain::" << __func__);

  // ND:
  // 1. Disable locking and make method private.
  //CRITICAL_REGION_LOCAL(m_blockchain_lock);

  struct outputs_visitor
  {
    std::vector<rct::ctkey >& m_output_keys;
    const Blockchain& m_bch;
    outputs_visitor(std::vector<rct::ctkey>& output_keys, const Blockchain& bch) :
      m_output_keys(output_keys), m_bch(bch)
    {
    }
    bool handle_output(uint64_t unlock_time, const crypto::public_key &pubkey, const rct::key &commitment)
    {
      //check tx unlock time
      if (!m_bch.is_tx_spendtime_unlocked(unlock_time))
      {
        LOG_PRINT_L1("One of outputs for one of inputs has wrong tx.unlock_time = " << unlock_time);
        return false;
      }

      // The original code includes a check for the output corresponding to this input
      // to be a txout_to_key. This is removed, as the database does not store this info,
      // but only txout_to_key outputs are stored in the DB in the first place, done in
      // Blockchain*::add_output

      m_output_keys.push_back(rct::ctkey({rct::pk2rct(pubkey), commitment}));
      return true;
    }
  };

  output_keys.clear();

  // collect output keys
  outputs_visitor vi(output_keys, *this);
  if (!scan_outputkeys_for_indexes(tx_version, txin, vi, tx_prefix_hash, pmax_related_block_height))
  {
    LOG_PRINT_L1("Failed to get output keys for tx with amount = " << print_money(txin.amount) << " and count indexes " << txin.key_offsets.size());
    return false;
  }

  if(txin.key_offsets.size() != output_keys.size())
  {
    LOG_PRINT_L1("Output keys for tx with amount = " << txin.amount << " and count indexes " << txin.key_offsets.size() << " returned wrong keys count " << output_keys.size());
    return false;
  }
  if (tx_version == 1) {
    CHECK_AND_ASSERT_MES(sig.size() == output_keys.size(), false, "internal error: tx signatures count=" << sig.size() << " mismatch with outputs keys count for inputs=" << output_keys.size());
  }
  // rct_signatures will be expanded after this
  return true;
}
//------------------------------------------------------------------
//TODO: Is this intended to do something else?  Need to look into the todo there.
uint64_t Blockchain::get_adjusted_time() const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  //TODO: add collecting median time
  return time(NULL);
}
//------------------------------------------------------------------
//TODO: revisit, has changed a bit on upstream
bool Blockchain::check_block_timestamp(std::vector<uint64_t>& timestamps, const block& b) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  uint64_t median_ts = epee::misc_utils::median(timestamps);
  size_t blockchain_timestamp_check_window = BLOCKCHAIN_TIMESTAMP_CHECK_WINDOW;
  if(b.timestamp < median_ts)
  {
    LOG_PRINT_L1("Timestamp of block with id: " << get_block_hash(b) << ", " << b.timestamp << ", less than median of last " << blockchain_timestamp_check_window << " blocks, " << median_ts);
    return false;
  }

  return true;
}
//------------------------------------------------------------------
// This function grabs the timestamps from the most recent <n> blocks,
// where n = BLOCKCHAIN_TIMESTAMP_CHECK_WINDOW.  If there are not those many
// blocks in the blockchain, the timestap is assumed to be valid.  If there
// are, this function returns:
//   true if the block's timestamp is not less than the timestamp of the
//       median of the selected blocks
//   false otherwise
bool Blockchain::check_block_timestamp(const block& b) const
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  uint64_t block_future_time_limit = get_current_hard_fork_version() < 2 ?
                                          CRYPTONOTE_BLOCK_FUTURE_TIME_LIMIT :
                                          CRYPTONOTE_BLOCK_FUTURE_TIME_LIMIT_V2;
  size_t blockchain_timestamp_check_window = get_current_hard_fork_version() < 2 ?
                                                  BLOCKCHAIN_TIMESTAMP_CHECK_WINDOW :
                                                  BLOCKCHAIN_TIMESTAMP_CHECK_WINDOW_V2;
  if (b.timestamp > get_adjusted_time() + block_future_time_limit)
  {
    LOG_PRINT_L1("Timestamp of block with id: " << get_block_hash(b) << ", " << b.timestamp <<
      ", bigger than adjusted time + " << (get_current_hard_fork_version() < 2 ? "2 hours" : "30 minutes"));
    return false;
  }

  // if not enough blocks, no proper median yet, return true
  if (m_db->height() < blockchain_timestamp_check_window)
  {
    return true;
  }

  std::vector<uint64_t> timestamps;
  auto h = m_db->height();

  // need most recent 60 blocks, get index of first of those
  size_t offset = h - blockchain_timestamp_check_window;
  for(;offset < h; ++offset)
  {
    timestamps.push_back(m_db->get_block_timestamp(offset));
  }

  return check_block_timestamp(timestamps, b);
}
//------------------------------------------------------------------
void Blockchain::return_tx_to_pool(const std::vector<transaction> &txs)
{
  uint8_t version = get_current_hard_fork_version();
  for (auto& tx : txs)
  {
    cryptonote::tx_verification_context tvc = AUTO_VAL_INIT(tvc);
    // We assume that if they were in a block, the transactions are already
    // known to the network as a whole. However, if we had mined that block,
    // that might not be always true. Unlikely though, and always relaying
    // these again might cause a spike of traffic as many nodes re-relay
    // all the transactions in a popped block when a reorg happens.
    if (!m_tx_pool.add_tx(tx, tvc, true, true, version))
    {
      LOG_PRINT_L0("Failed to return taken transaction with hash: " << get_transaction_hash(tx) << " to tx_pool");
    }
  }
}
//------------------------------------------------------------------
bool Blockchain::flush_txes_from_pool(const std::list<crypto::hash> &txids)
{
  CRITICAL_REGION_LOCAL(m_tx_pool);

  bool res = true;
  for (const auto &txid: txids)
  {
    cryptonote::transaction tx;
    size_t blob_size;
    uint64_t fee;
    bool relayed;
    LOG_PRINT_L1("Removing txid " << txid << " from the pool");
    if(m_tx_pool.have_tx(txid) && !m_tx_pool.take_tx(txid, tx, blob_size, fee, relayed))
    {
      LOG_PRINT_L0("Failed to remove txid " << txid << " from the pool");
      res = false;
    }
  }
  return res;
}
//------------------------------------------------------------------
//      Needs to validate the block and acquire each transaction from the
//      transaction mem_pool, then pass the block and transactions to
//      m_db->add_block()
bool Blockchain::handle_block_to_main_chain(const block& bl, const crypto::hash& id, block_verification_context& bvc)
{
  LOG_PRINT_L3("Blockchain::" << __func__);

  TIME_MEASURE_START(block_processing_time);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);
  TIME_MEASURE_START(t1);

  m_db->block_txn_start(true);
  if(bl.prev_id != get_tail_id())
  {
    LOG_PRINT_L1("Block with id: " << id << std::endl << "has wrong prev_id: " << bl.prev_id << std::endl << "expected: " << get_tail_id());
leave:
    m_db->block_txn_stop();
    return false;
  }

  // this is a cheap test
  if (!m_hardfork->check(bl))
  {
    LOG_PRINT_L1("Block with id: " << id << std::endl << "has old version: " << (unsigned)bl.major_version << std::endl << "current: " << (unsigned)m_hardfork->get_current_version());
    bvc.m_verifivation_failed = true;
    goto leave;
  }

  TIME_MEASURE_FINISH(t1);
  TIME_MEASURE_START(t2);

  // make sure block timestamp is not less than the median timestamp
  // of a set number of the most recent blocks.
  if(!check_block_timestamp(bl))
  {
    LOG_PRINT_L1("Block with id: " << id << std::endl << "has invalid timestamp: " << bl.timestamp);
    bvc.m_verifivation_failed = true;
    goto leave;
  }

  TIME_MEASURE_FINISH(t2);
  //check proof of work
  TIME_MEASURE_START(target_calculating_time);

  // get the target difficulty for the block.
  // the calculation can overflow, among other failure cases,
  // so we need to check the return type.
  // FIXME: get_difficulty_for_next_block can also assert, look into
  // changing this to throwing exceptions instead so we can clean up.
  difficulty_type current_diffic = get_difficulty_for_next_block();
  CHECK_AND_ASSERT_MES(current_diffic, false, "!!!!!!!!! difficulty overhead !!!!!!!!!");

  TIME_MEASURE_FINISH(target_calculating_time);

  TIME_MEASURE_START(longhash_calculating_time);

  crypto::hash proof_of_work = null_hash;

  // Formerly the code below contained an if loop with the following condition
  // !m_checkpoints.is_in_checkpoint_zone(get_current_blockchain_height())
  // however, this caused the daemon to not bother checking PoW for blocks
  // before checkpoints, which is very dangerous behaviour. We moved the PoW
  // validation out of the next chunk of code to make sure that we correctly
  // check PoW now.
  // FIXME: height parameter is not used...should it be used or should it not
  // be a parameter?
  // validate proof_of_work versus difficulty target
  bool precomputed = false;
  bool fast_check = false;
#if defined(PER_BLOCK_CHECKPOINT)
  if (m_db->height() < m_blocks_hash_check.size())
  {
    auto hash = get_block_hash(bl);
    if (memcmp(&hash, &m_blocks_hash_check[m_db->height()], sizeof(hash)) != 0)
    {
      LOG_PRINT_L1("Block with id is INVALID: " << id);
      bvc.m_verifivation_failed = true;
      goto leave;
    }
    fast_check = true;
  }
  else
#endif
  {
    auto it = m_blocks_longhash_table.find(id);
    if (it != m_blocks_longhash_table.end())
    {
      precomputed = true;
      proof_of_work = it->second;
    }
    else
	{
		get_block_longhash(bl, m_pow_ctx, proof_of_work);
	}

    // validate proof_of_work versus difficulty target
    if(!check_hash(proof_of_work, current_diffic))
    {
      LOG_PRINT_L1("Block with id: " << id << std::endl << "does not have enough proof of work: " << proof_of_work << std::endl << "unexpected difficulty: " << current_diffic);
      bvc.m_verifivation_failed = true;
      goto leave;
    }
  }

  // If we're at a checkpoint, ensure that our hardcoded checkpoint hash
  // is correct.
  if(m_checkpoints.is_in_checkpoint_zone(get_current_blockchain_height()))
  {
    if(!m_checkpoints.check_block(get_current_blockchain_height(), id))
    {
      LOG_ERROR("CHECKPOINT VALIDATION FAILED");
      bvc.m_verifivation_failed = true;
      goto leave;
    }
  }

  TIME_MEASURE_FINISH(longhash_calculating_time);
  if (precomputed)
    longhash_calculating_time += m_fake_pow_calc_time;

  TIME_MEASURE_START(t3);

  // sanity check basic miner tx properties;
  if(!prevalidate_miner_transaction(bl, m_db->height()))
  {
    LOG_PRINT_L1("Block with id: " << id << " failed to pass prevalidation");
    bvc.m_verifivation_failed = true;
    goto leave;
  }

  size_t coinbase_blob_size = get_object_blobsize(bl.miner_tx);
  size_t cumulative_block_size = coinbase_blob_size;

  std::vector<transaction> txs;
  key_images_container keys;

  uint64_t fee_summary = 0;
  uint64_t t_checktx = 0;
  uint64_t t_exists = 0;
  uint64_t t_pool = 0;
  uint64_t t_dblspnd = 0;
  TIME_MEASURE_FINISH(t3);

// XXX old code adds miner tx here

  int tx_index = 0;
  // Iterate over the block's transaction hashes, grabbing each
  // from the tx_pool and validating them.  Each is then added
  // to txs.  Keys spent in each are added to <keys> by the double spend check.
  for (const crypto::hash& tx_id : bl.tx_hashes)
  {
    transaction tx;
    size_t blob_size = 0;
    uint64_t fee = 0;
    bool relayed = false;
    TIME_MEASURE_START(aa);

// XXX old code does not check whether tx exists
    if (m_db->tx_exists(tx_id))
    {
      LOG_PRINT_L1("Block with id: " << id << " attempting to add transaction already in blockchain with id: " << tx_id);
      bvc.m_verifivation_failed = true;
      return_tx_to_pool(txs);
      goto leave;
    }

    TIME_MEASURE_FINISH(aa);
    t_exists += aa;
    TIME_MEASURE_START(bb);

    // get transaction with hash <tx_id> from tx_pool
    if(!m_tx_pool.take_tx(tx_id, tx, blob_size, fee, relayed))
    {
      LOG_PRINT_L1("Block with id: " << id  << " has at least one unknown transaction with id: " << tx_id);
      bvc.m_verifivation_failed = true;
      return_tx_to_pool(txs);
      goto leave;
    }

    TIME_MEASURE_FINISH(bb);
    t_pool += bb;
    // add the transaction to the temp list of transactions, so we can either
    // store the list of transactions all at once or return the ones we've
    // taken from the tx_pool back to it if the block fails verification.
    txs.push_back(tx);
    TIME_MEASURE_START(dd);

    // FIXME: the storage should not be responsible for validation.
    //        If it does any, it is merely a sanity check.
    //        Validation is the purview of the Blockchain class
    //        - TW
    //
    // ND: this is not needed, db->add_block() checks for duplicate k_images and fails accordingly.
    // if (!check_for_double_spend(tx, keys))
    // {
    //     LOG_PRINT_L0("Double spend detected in transaction (id: " << tx_id);
    //     bvc.m_verifivation_failed = true;
    //     break;
    // }

    TIME_MEASURE_FINISH(dd);
    t_dblspnd += dd;
    TIME_MEASURE_START(cc);

#if defined(PER_BLOCK_CHECKPOINT)
    if (!fast_check)
#endif
    {
      // validate that transaction inputs and the keys spending them are correct.
      tx_verification_context tvc;
      if(!check_tx_inputs(tx, tvc))
      {
        LOG_PRINT_L1("Block with id: " << id  << " has at least one transaction (id: " << tx_id << ") with wrong inputs.");

        //TODO: why is this done?  make sure that keeping invalid blocks makes sense.
        add_block_as_invalid(bl, id);
        LOG_PRINT_L1("Block with id " << id << " added as invalid because of wrong inputs in transactions");
        bvc.m_verifivation_failed = true;
        return_tx_to_pool(txs);
        goto leave;
      }
    }
#if defined(PER_BLOCK_CHECKPOINT)
    else
    {
      // ND: if fast_check is enabled for blocks, there is no need to check
      // the transaction inputs, but do some sanity checks anyway.
      if (memcmp(&m_blocks_txs_check[tx_index++], &tx_id, sizeof(tx_id)) != 0)
      {
        LOG_PRINT_L1("Block with id: " << id << " has at least one transaction (id: " << tx_id << ") with wrong inputs.");
        //TODO: why is this done?  make sure that keeping invalid blocks makes sense.
        add_block_as_invalid(bl, id);
        LOG_PRINT_L1("Block with id " << id << " added as invalid because of wrong inputs in transactions");
        bvc.m_verifivation_failed = true;
        return_tx_to_pool(txs);
        goto leave;
      }
    }
#endif
    TIME_MEASURE_FINISH(cc);
    t_checktx += cc;
    fee_summary += fee;
    cumulative_block_size += blob_size;
  }

  m_blocks_txs_check.clear();

  TIME_MEASURE_START(vmt);
  uint64_t base_reward = 0;
  uint64_t height = m_db->height();
  uint64_t cal_height = height - height % COIN_EMISSION_HEIGHT_INTERVAL;
  uint64_t cal_generated_coins = cal_height ? m_db->get_block_already_generated_coins(cal_height - 1) : 0;
  if (!validate_miner_transaction(bl, cumulative_block_size, fee_summary, base_reward, cal_generated_coins, bvc.m_partial_block_reward, m_hardfork->get_current_version()))
  {
    LOG_PRINT_L1("Block with id: " << id << " has incorrect miner transaction");
    bvc.m_verifivation_failed = true;
    return_tx_to_pool(txs);
    goto leave;
  }

  TIME_MEASURE_FINISH(vmt);
  size_t block_size;
  difficulty_type cumulative_difficulty;

  // populate various metadata about the block to be stored alongside it.
  block_size = cumulative_block_size;
  cumulative_difficulty = current_diffic;
  // In the "tail" state when the minimum subsidy (implemented in get_block_reward) is in effect, the number of
  // coins will eventually exceed MONEY_SUPPLY and overflow a uint64. To prevent overflow, cap already_generated_coins
  // at MONEY_SUPPLY. already_generated_coins is only used to compute the block subsidy and MONEY_SUPPLY yields a
  // subsidy of 0 under the base formula and therefore the minimum subsidy >0 in the tail state.
  uint64_t already_generated_coins = height ? m_db->get_block_already_generated_coins(height - 1) : 0;

  if (m_hardfork->get_current_version() > 3) {
    already_generated_coins = base_reward < (MONEY_SUPPLY-already_generated_coins) ? already_generated_coins + base_reward : MONEY_SUPPLY_V4;
  } else {
    already_generated_coins = base_reward < (MONEY_SUPPLY-already_generated_coins) ? already_generated_coins + base_reward : MONEY_SUPPLY;
  }
  if (height)
    cumulative_difficulty += m_db->get_block_cumulative_difficulty(height - 1);

  TIME_MEASURE_FINISH(block_processing_time);
  if(precomputed)
    block_processing_time += m_fake_pow_calc_time;

  m_db->block_txn_stop();
  TIME_MEASURE_START(addblock);
  uint64_t new_height = 0;
  if (!bvc.m_verifivation_failed)
  {
    try
    {
      new_height = m_db->add_block(bl, block_size, cumulative_difficulty, already_generated_coins, txs);
    }
    catch (const KEY_IMAGE_EXISTS& e)
    {
      LOG_ERROR("Error adding block with hash: " << id << " to blockchain, what = " << e.what());
      bvc.m_verifivation_failed = true;
      return_tx_to_pool(txs);
      return false;
    }
    catch (const std::exception& e)
    {
      //TODO: figure out the best way to deal with this failure
      LOG_ERROR("Error adding block with hash: " << id << " to blockchain, what = " << e.what());
      return_tx_to_pool(txs);
      return false;
    }
  }
  else
  {
    LOG_ERROR("Blocks that failed verification should not reach here");
  }

  TIME_MEASURE_FINISH(addblock);

  // do this after updating the hard fork state since the size limit may change due to fork
  update_next_cumulative_size_limit();

  LOG_PRINT_L1("+++++ BLOCK SUCCESSFULLY ADDED" << std::endl << "id:\t" << id << std::endl << "PoW:\t" << proof_of_work << std::endl << "HEIGHT " << new_height-1 << ", difficulty:\t" << current_diffic << std::endl << "block reward: " << print_money(fee_summary + base_reward) << "(" << print_money(base_reward) << " + " << print_money(fee_summary) << "), coinbase_blob_size: " << coinbase_blob_size << ", cumulative size: " << cumulative_block_size << ", " << block_processing_time << "(" << target_calculating_time << "/" << longhash_calculating_time << ")ms");
  if(m_show_time_stats)
  {
    LOG_PRINT_L0("Height: " << new_height << " blob: " << coinbase_blob_size << " cumm: "
        << cumulative_block_size << " p/t: " << block_processing_time << " ("
        << target_calculating_time << "/" << longhash_calculating_time << "/"
        << t1 << "/" << t2 << "/" << t3 << "/" << t_exists << "/" << t_pool
        << "/" << t_checktx << "/" << t_dblspnd << "/" << vmt << "/" << addblock << ")ms");
  }

  bvc.m_added_to_main_chain = true;
  ++m_sync_counter;

  // appears to be a NOP *and* is called elsewhere.  wat?
  m_tx_pool.on_blockchain_inc(new_height, id);

  return true;
}
//------------------------------------------------------------------
bool Blockchain::update_next_cumulative_size_limit()
{
  uint64_t full_reward_zone = CRYPTONOTE_BLOCK_GRANTED_FULL_REWARD_ZONE;

  LOG_PRINT_L3("Blockchain::" << __func__);
  std::vector<size_t> sz;
  get_last_n_blocks_sizes(sz, CRYPTONOTE_REWARD_BLOCKS_WINDOW);

  uint64_t median = epee::misc_utils::median(sz);
  if(median <= full_reward_zone)
    median = full_reward_zone;

  m_current_block_cumul_sz_limit = median*2;
  return true;
}
//------------------------------------------------------------------
bool Blockchain::add_new_block(const block& bl_, block_verification_context& bvc)
{
  LOG_PRINT_L3("Blockchain::" << __func__);
  //copy block here to let modify block.target
  block bl = bl_;
  crypto::hash id = get_block_hash(bl);
  CRITICAL_REGION_LOCAL(m_tx_pool);//to avoid deadlock lets lock tx_pool for whole add/reorganize process
  CRITICAL_REGION_LOCAL1(m_blockchain_lock);
  m_db->block_txn_start(true);
  if(have_block(id))
  {
    LOG_PRINT_L3("block with id = " << id << " already exists");
    bvc.m_already_exists = true;
    m_db->block_txn_stop();
    return false;
  }

  //check that block refers to chain tail
  if(!(bl.prev_id == get_tail_id()))
  {
    //chain switching or wrong block
    bvc.m_added_to_main_chain = false;
    m_db->block_txn_stop();
    return handle_alternative_block(bl, id, bvc);
    //never relay alternative blocks
  }

  m_db->block_txn_stop();
  return handle_block_to_main_chain(bl, id, bvc);
}
//------------------------------------------------------------------
//TODO: Refactor, consider returning a failure height and letting
//      caller decide course of action.
void Blockchain::check_against_checkpoints(const checkpoints& points, bool enforce)
{
  const auto& pts = points.get_points();

  CRITICAL_REGION_LOCAL(m_blockchain_lock);
  m_db->batch_start();
  for (const auto& pt : pts)
  {
    // if the checkpoint is for a block we don't have yet, move on
    if (pt.first >= m_db->height())
    {
      continue;
    }

    if (!points.check_block(pt.first, m_db->get_block_hash_from_height(pt.first)))
    {
      // if asked to enforce checkpoints, roll back to a couple of blocks before the checkpoint
      if (enforce)
      {
        LOG_ERROR("Local blockchain failed to pass a checkpoint, rolling back!");
        std::list<block> empty;
        rollback_blockchain_switching(empty, pt.first - 2);
      }
      else
      {
        LOG_ERROR("WARNING: local blockchain failed to pass a SumoPulse checkpoint, and you could be on a fork. You should either sync up from scratch, OR download a fresh blockchain bootstrap, OR enable checkpoint enforcing with the --enforce-dns-checkpointing command-line option");
      }
    }
  }
  m_db->batch_stop();
}
//------------------------------------------------------------------
// returns false if any of the checkpoints loading returns false.
// That should happen only if a checkpoint is added that conflicts
// with an existing checkpoint.
bool Blockchain::update_checkpoints(const std::string& file_path, bool check_dns)
{
  if (!m_checkpoints.load_checkpoints_from_json(file_path))
  {
      return false;
  }

  // if we're checking both dns and json, load checkpoints from dns.
  // if we're not hard-enforcing dns checkpoints, handle accordingly
  if (m_enforce_dns_checkpoints && check_dns)
  {
    if (!m_checkpoints.load_checkpoints_from_dns())
    {
      return false;
    }
  }
  else if (check_dns)
  {
    checkpoints dns_points;
    dns_points.load_checkpoints_from_dns();
    if (m_checkpoints.check_for_conflicts(dns_points))
    {
      check_against_checkpoints(dns_points, false);
    }
    else
    {
      LOG_PRINT_L0("One or more checkpoints fetched from DNS conflicted with existing checkpoints!");
    }
  }

  check_against_checkpoints(m_checkpoints, true);

  return true;
}
//------------------------------------------------------------------
void Blockchain::set_enforce_dns_checkpoints(bool enforce_checkpoints)
{
  m_enforce_dns_checkpoints = enforce_checkpoints;
}

//------------------------------------------------------------------
void Blockchain::block_longhash_worker(cn_pow_hash_v2& hash_ctx, const std::vector<block> &blocks, std::unordered_map<crypto::hash, crypto::hash> &map)
{
  TIME_MEASURE_START(t);

  //FIXME: height should be changing here, as get_block_longhash expects
  //       the height of the block passed to it
  for (const auto & block : blocks)
  {
    if (m_cancel)
       return;
    crypto::hash id = get_block_hash(block);
    crypto::hash pow;
    get_block_longhash(block, hash_ctx, pow);
    map.emplace(id, pow);
  }

  TIME_MEASURE_FINISH(t);
}

//------------------------------------------------------------------
bool Blockchain::cleanup_handle_incoming_blocks(bool force_sync)
{
  LOG_PRINT_YELLOW("Blockchain::" << __func__, LOG_LEVEL_3);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);
  TIME_MEASURE_START(t1);

  if (m_sync_counter > 0)
  {
    if (force_sync)
    {
      if(m_db_sync_mode != db_nosync)
        store_blockchain();
      m_sync_counter = 0;
    }
    else if (m_db_blocks_per_sync && m_sync_counter >= m_db_blocks_per_sync)
    {
      if(m_db_sync_mode == db_async)
      {
        m_sync_counter = 0;
        m_async_service.dispatch(boost::bind(&Blockchain::store_blockchain, this));
      }
      else if(m_db_sync_mode == db_sync)
      {
        store_blockchain();
      }
      else // db_nosync
      {
        // DO NOTHING, not required to call sync.
      }
    }
  }

  TIME_MEASURE_FINISH(t1);
  m_blocks_longhash_table.clear();
  m_scan_table.clear();
  m_blocks_txs_check.clear();
  m_check_txin_table.clear();

  return true;
}

//------------------------------------------------------------------
//FIXME: unused parameter txs
void Blockchain::output_scan_worker(const uint64_t amount, const std::vector<uint64_t> &offsets, std::vector<output_data_t> &outputs, std::unordered_map<crypto::hash, cryptonote::transaction> &txs) const
{
  try
  {
    m_db->get_output_key(amount, offsets, outputs);
  }
  catch (const std::exception& e)
  {
    LOG_PRINT_L1("EXCEPTION: " << e.what());
  }
  catch (...)
  {

  }
}

//------------------------------------------------------------------
// ND: Speedups:
// 1. Thread long_hash computations if possible (m_max_prepare_blocks_threads = nthreads, default = 4)
// 2. Group all amounts (from txs) and related absolute offsets and form a table of tx_prefix_hash
//    vs [k_image, output_keys] (m_scan_table). This is faster because it takes advantage of bulk queries
//    and is threaded if possible. The table (m_scan_table) will be used later when querying output
//    keys.
bool Blockchain::prepare_handle_incoming_blocks(const std::list<block_complete_entry> &blocks_entry)
{
  LOG_PRINT_YELLOW("Blockchain::" << __func__, LOG_LEVEL_3);
  TIME_MEASURE_START(prepare);
  CRITICAL_REGION_LOCAL(m_blockchain_lock);

  if(blocks_entry.size() == 0)
    return false;

  if ((m_db->height() + blocks_entry.size()) < m_blocks_hash_check.size())
    return true;

  bool blocks_exist = false;
  uint64_t threads = tools::get_max_concurrency();

  if (blocks_entry.size() > 1 && threads > 1 && m_max_prepare_blocks_threads > 1)
  {
    // limit threads, default limit = 4
    if(threads > m_max_prepare_blocks_threads)
      threads = m_max_prepare_blocks_threads;

    uint64_t height = m_db->height();
    std::vector<boost::thread *> thread_list;
    int batches = blocks_entry.size() / threads;
    int extra = blocks_entry.size() % threads;
    LOG_PRINT_L1("block_batches: " << batches);
    std::vector<std::unordered_map<crypto::hash, crypto::hash>> maps(threads);
    std::vector < std::vector < block >> blocks(threads);
    auto it = blocks_entry.begin();

    for (uint64_t i = 0; i < threads; i++)
    {
      for (int j = 0; j < batches; j++)
      {
        block block;

        if (!parse_and_validate_block_from_blob(it->block, block))
        {
          std::advance(it, 1);
          continue;
        }

        // check first block and skip all blocks if its not chained properly
        if (i == 0 && j == 0)
        {
          crypto::hash tophash = m_db->top_block_hash();
          if (block.prev_id != tophash)
          {
            LOG_PRINT_L1("Skipping prepare blocks. New blocks don't belong to chain.");
            return true;
          }
        }
        if (have_block(get_block_hash(block)))
        {
          blocks_exist = true;
          break;
        }

        blocks[i].push_back(block);
        std::advance(it, 1);
      }
    }

    for (int i = 0; i < extra && !blocks_exist; i++)
    {
      block block;

      if (!parse_and_validate_block_from_blob(it->block, block))
      {
        std::advance(it, 1);
        continue;
      }

      if (have_block(get_block_hash(block)))
      {
        blocks_exist = true;
        break;
      }

      blocks[i].push_back(block);
      std::advance(it, 1);
    }

    if (!blocks_exist)
    {
      m_blocks_longhash_table.clear();

	  if(m_hash_ctxes_multi.size() < threads)
		m_hash_ctxes_multi.resize(threads);

      for (uint64_t i = 0; i < threads; i++)
      {
        thread_list.push_back(new boost::thread(&Blockchain::block_longhash_worker, this, std::ref(m_hash_ctxes_multi[i]), std::cref(blocks[i]), std::ref(maps[i])));
      }

      for (size_t j = 0; j < thread_list.size(); j++)
      {
        thread_list[j]->join();
        delete thread_list[j];
      }

      thread_list.clear();

      if (m_cancel)
         return false;

      for (const auto & map : maps)
      {
        m_blocks_longhash_table.insert(map.begin(), map.end());
      }
    }
  }

  if (m_cancel)
    return false;

  if (blocks_exist)
  {
    LOG_PRINT_L0("Skipping prepare blocks. Blocks exist.");
    return true;
  }

  m_fake_scan_time = 0;
  m_fake_pow_calc_time = 0;

  m_scan_table.clear();
  m_check_txin_table.clear();

  TIME_MEASURE_FINISH(prepare);
  m_fake_pow_calc_time = prepare / blocks_entry.size();

  if (blocks_entry.size() > 1 && threads > 1 && m_show_time_stats)
    LOG_PRINT_L0("Prepare blocks took: " << prepare << " ms");

  TIME_MEASURE_START(scantable);

  // [input] stores all unique amounts found
  std::vector < uint64_t > amounts;
  // [input] stores all absolute_offsets for each amount
  std::map<uint64_t, std::vector<uint64_t>> offset_map;
  // [output] stores all output_data_t for each absolute_offset
  std::map<uint64_t, std::vector<output_data_t>> tx_map;

#define SCAN_TABLE_QUIT(m) \
        do { \
            LOG_PRINT_L0(m) ;\
            m_scan_table.clear(); \
            return false; \
        } while(0); \

  // generate sorted tables for all amounts and absolute offsets
  for (const auto &entry : blocks_entry)
  {
    if (m_cancel)
      return false;

    for (const auto &tx_blob : entry.txs)
    {
      crypto::hash tx_hash = null_hash;
      crypto::hash tx_prefix_hash = null_hash;
      transaction tx;

      if (!parse_and_validate_tx_from_blob(tx_blob, tx, tx_hash, tx_prefix_hash))
        SCAN_TABLE_QUIT("Could not parse tx from incoming blocks.");

      auto its = m_scan_table.find(tx_prefix_hash);
      if (its != m_scan_table.end())
        SCAN_TABLE_QUIT("Duplicate tx found from incoming blocks.");

      m_scan_table.emplace(tx_prefix_hash, std::unordered_map<crypto::key_image, std::vector<output_data_t>>());
      its = m_scan_table.find(tx_prefix_hash);
      assert(its != m_scan_table.end());

      // get all amounts from tx.vin(s)
      for (const auto &txin : tx.vin)
      {
        const txin_to_key &in_to_key = boost::get < txin_to_key > (txin);

        // check for duplicate
        auto it = its->second.find(in_to_key.k_image);
        if (it != its->second.end())
          SCAN_TABLE_QUIT("Duplicate key_image found from incoming blocks.");

        amounts.push_back(in_to_key.amount);
      }

      // sort and remove duplicate amounts from amounts list
      std::sort(amounts.begin(), amounts.end());
      auto last = std::unique(amounts.begin(), amounts.end());
      amounts.erase(last, amounts.end());

      // add amount to the offset_map and tx_map
      for (const uint64_t &amount : amounts)
      {
        if (offset_map.find(amount) == offset_map.end())
          offset_map.emplace(amount, std::vector<uint64_t>());

        if (tx_map.find(amount) == tx_map.end())
          tx_map.emplace(amount, std::vector<output_data_t>());
      }

      // add new absolute_offsets to offset_map
      for (const auto &txin : tx.vin)
      {
        const txin_to_key &in_to_key = boost::get < txin_to_key > (txin);
        // no need to check for duplicate here.
        auto absolute_offsets = relative_output_offsets_to_absolute(in_to_key.key_offsets);
        for (const auto & offset : absolute_offsets)
          offset_map[in_to_key.amount].push_back(offset);

      }

      // sort and remove duplicate absolute_offsets in offset_map
      for (auto &offsets : offset_map)
      {
        std::sort(offsets.second.begin(), offsets.second.end());
        auto last = std::unique(offsets.second.begin(), offsets.second.end());
        offsets.second.erase(last, offsets.second.end());
      }
    }
  }

  // [output] stores all transactions for each tx_out_index::hash found
  std::vector<std::unordered_map<crypto::hash, cryptonote::transaction>> transactions(amounts.size());

  threads = tools::get_max_concurrency();
  if (!m_db->can_thread_bulk_indices())
    threads = 1;

  if (threads > 1)
  {
    boost::asio::io_service ioservice;
    boost::thread_group threadpool;
    std::unique_ptr < boost::asio::io_service::work > work(new boost::asio::io_service::work(ioservice));

    for (uint64_t i = 0; i < threads; i++)
    {
      threadpool.create_thread(boost::bind(&boost::asio::io_service::run, &ioservice));
    }

    for (size_t i = 0; i < amounts.size(); i++)
    {
      uint64_t amount = amounts[i];
      ioservice.dispatch(boost::bind(&Blockchain::output_scan_worker, this, amount, std::cref(offset_map[amount]), std::ref(tx_map[amount]), std::ref(transactions[i])));
    }

    work.reset();
    threadpool.join_all();
    ioservice.stop();
  }
  else
  {
    for (size_t i = 0; i < amounts.size(); i++)
    {
      uint64_t amount = amounts[i];
      output_scan_worker(amount, offset_map[amount], tx_map[amount], transactions[i]);
    }
  }

  int total_txs = 0;

  // now generate a table for each tx_prefix and k_image hashes
  for (const auto &entry : blocks_entry)
  {
    if (m_cancel)
      return false;

    for (const auto &tx_blob : entry.txs)
    {
      crypto::hash tx_hash = null_hash;
      crypto::hash tx_prefix_hash = null_hash;
      transaction tx;

      if (!parse_and_validate_tx_from_blob(tx_blob, tx, tx_hash, tx_prefix_hash))
        SCAN_TABLE_QUIT("Could not parse tx from incoming blocks.");

      ++total_txs;
      auto its = m_scan_table.find(tx_prefix_hash);
      if (its == m_scan_table.end())
        SCAN_TABLE_QUIT("Tx not found on scan table from incoming blocks.");

      for (const auto &txin : tx.vin)
      {
        const txin_to_key &in_to_key = boost::get < txin_to_key > (txin);
        auto needed_offsets = relative_output_offsets_to_absolute(in_to_key.key_offsets);

        std::vector<output_data_t> outputs;
        for (const uint64_t & offset_needed : needed_offsets)
        {
          size_t pos = 0;
          bool found = false;

          for (const uint64_t &offset_found : offset_map[in_to_key.amount])
          {
            if (offset_needed == offset_found)
            {
              found = true;
              break;
            }

            ++pos;
          }

          if (found && pos < tx_map[in_to_key.amount].size())
            outputs.push_back(tx_map[in_to_key.amount].at(pos));
          else
            break;
        }

        its->second.emplace(in_to_key.k_image, outputs);
      }
    }
  }

  TIME_MEASURE_FINISH(scantable);
  if (total_txs > 0)
  {
    m_fake_scan_time = scantable / total_txs;
    if(m_show_time_stats)
      LOG_PRINT_L0("Prepare scantable took: " << scantable << " ms");
  }

  return true;
}

void Blockchain::set_user_options(uint64_t maxthreads, uint64_t blocks_per_sync, blockchain_db_sync_mode sync_mode, bool fast_sync)
{
  m_db_sync_mode = sync_mode;
  m_fast_sync = fast_sync;
  m_db_blocks_per_sync = blocks_per_sync;
  m_max_prepare_blocks_threads = maxthreads;
}

HardFork::State Blockchain::get_hard_fork_state() const
{
  return m_hardfork->get_state();
}

bool Blockchain::get_hard_fork_voting_info(uint8_t version, uint32_t &window, uint32_t &votes, uint32_t &threshold, uint64_t &earliest_height, uint8_t &voting) const
{
  return m_hardfork->get_voting_info(version, window, votes, threshold, earliest_height, voting);
}

std::map<uint64_t, std::tuple<uint64_t, uint64_t, uint64_t>> Blockchain:: get_output_histogram(const std::vector<uint64_t> &amounts, bool unlocked, uint64_t recent_cutoff) const
{
  return m_db->get_output_histogram(amounts, unlocked, recent_cutoff);
}

void Blockchain::cancel()
{
  m_cancel = true;
}

#if defined(PER_BLOCK_CHECKPOINT)
void Blockchain::load_compiled_in_block_hashes()
{
  if (m_fast_sync && get_blocks_dat_start(m_testnet) != nullptr)
  {
    if (get_blocks_dat_size(m_testnet) > 4)
    {
      const unsigned char *p = get_blocks_dat_start(m_testnet);
      const uint32_t nblocks = *p | ((*(p+1))<<8) | ((*(p+2))<<16) | ((*(p+3))<<24);
      const size_t size_needed = 4 + nblocks * sizeof(crypto::hash);
      if(nblocks > 0 && nblocks > m_db->height() && get_blocks_dat_size(m_testnet) >= size_needed)
      {
        LOG_PRINT_L0("Loading precomputed blocks: " << nblocks);
        p += sizeof(uint32_t);
        for (uint32_t i = 0; i < nblocks; i++)
        {
          crypto::hash hash;
          memcpy(hash.data, p, sizeof(hash.data));
          p += sizeof(hash.data);
          m_blocks_hash_check.push_back(hash);
        }

        // FIXME: clear tx_pool because the process might have been
        // terminated and caused it to store txs kept by blocks.
        // The core will not call check_tx_inputs(..) for these
        // transactions in this case. Consequently, the sanity check
        // for tx hashes will fail in handle_block_to_main_chain(..)
        std::list<transaction> txs;
        m_tx_pool.get_transactions(txs);

        size_t blob_size;
        uint64_t fee;
        bool relayed;
        transaction pool_tx;
        for(const transaction &tx : txs)
        {
          crypto::hash tx_hash = get_transaction_hash(tx);
          m_tx_pool.take_tx(tx_hash, pool_tx, blob_size, fee, relayed);
        }
      }
    }
  }
}
#endif

bool Blockchain::for_all_key_images(std::function<bool(const crypto::key_image&)> f) const
{
  return m_db->for_all_key_images(f);
}

bool Blockchain::for_all_blocks(std::function<bool(uint64_t, const crypto::hash&, const block&)> f) const
{
  return m_db->for_all_blocks(f);
}

bool Blockchain::for_all_transactions(std::function<bool(const crypto::hash&, const cryptonote::transaction&)> f) const
{
  return m_db->for_all_transactions(f);
}

bool Blockchain::for_all_outputs(std::function<bool(uint64_t amount, const crypto::hash &tx_hash, size_t tx_idx)> f) const
{
  return m_db->for_all_outputs(f);;
}
