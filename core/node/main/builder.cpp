/**
 * Copyright Soramitsu Co., Ltd. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "builder.hpp"

#include <boost/di/extension/scopes/shared.hpp>
#include <libp2p/injector/host_injector.hpp>

#include <libp2p/protocol/gossip/gossip.hpp>
#include <libp2p/protocol/identify/identify.hpp>
#include <libp2p/protocol/identify/identify_delta.hpp>
#include <libp2p/protocol/identify/identify_push.hpp>

#include "api/make.hpp"
#include "blockchain/block_validator/impl/block_validator_impl.hpp"
#include "blockchain/impl/weight_calculator_impl.hpp"
#include "clock/impl/chain_epoch_clock_impl.hpp"
#include "clock/impl/utc_clock_impl.hpp"
#include "crypto/bls/impl/bls_provider_impl.hpp"
#include "crypto/secp256k1/impl/secp256k1_provider_impl.hpp"
#include "drand/impl/beaconizer.hpp"
#include "node/blocksync_client.hpp"
#include "node/blocksync_server.hpp"
#include "node/chain_db.hpp"
#include "node/chain_store_impl.hpp"
#include "node/identify.hpp"
#include "node/index_db_backend.hpp"
#include "node/receive_hello.hpp"
#include "node/say_hello.hpp"
#include "node/syncer.hpp"
#include "node/tipset_loader.hpp"
#include "power/impl/power_table_impl.hpp"
#include "storage/car/car.hpp"
#include "storage/chain/msg_waiter.hpp"
#include "storage/in_memory/in_memory_storage.hpp"
#include "storage/ipfs/graphsync/impl/graphsync_impl.hpp"
#include "storage/ipfs/impl/datastore_leveldb.hpp"
#include "storage/ipfs/impl/in_memory_datastore.hpp"
#include "storage/keystore/impl/in_memory/in_memory_keystore.hpp"
#include "storage/leveldb/leveldb.hpp"
#include "storage/mpool/mpool.hpp"
#include "vm/actor/builtin/v0/init/init_actor.hpp"
#include "vm/interpreter/impl/interpreter_impl.hpp"
#include "vm/runtime/impl/tipset_randomness.hpp"
#include "vm/state/impl/state_tree_impl.hpp"

namespace fc::node {

  namespace {
    auto log() {
      static common::Logger logger = common::createLogger("node");
      return logger.get();
    }

    gsl::span<const uint8_t> toSpanU8(const std::string &s) {
      return gsl::span<const uint8_t>((const uint8_t *)s.data(), s.size());
    }

    std::vector<std::string> toStrings(const std::vector<CID> &cids) {
      std::vector<std::string> v;
      v.reserve(cids.size());
      for (const auto &cid : cids) {
        v.push_back(cid.toString().value());
      }
      return v;
    }

    outcome::result<void> loadCar(storage::ipfs::IpfsDatastore &storage,
                                  Config &config) {
      std::ifstream file{config.car_file_name,
                         std::ios::binary | std::ios::ate};
      if (!file.good()) {
        log()->error("cannot open file {}", config.car_file_name);
        return Error::CAR_FILE_OPEN_ERROR;
      }

      static const size_t kMaxSize = 64 * 1024 * 1024;
      auto size = static_cast<size_t>(file.tellg());
      if (size > kMaxSize) {
        log()->error("car file size above expected, file:{}, size:{}, limit:{}",
                     config.car_file_name,
                     size,
                     kMaxSize);
        return Error::CAR_FILE_SIZE_ABOVE_LIMIT;
      }

      std::string buffer;
      buffer.resize(size);
      file.seekg(0, std::ios::beg);
      file.read(buffer.data(), buffer.size());

      auto result = fc::storage::car::loadCar(storage, toSpanU8(buffer));
      if (!result) {
        log()->error("cannot load car file {}: {}",
                     config.car_file_name,
                     result.error().message());
        return result.error();
      }

      const auto &roots = result.value();
      if (roots.empty()) {
        return Error::NO_GENESIS_BLOCK;
      }

      if (config.genesis_cid) {
        if (config.genesis_cid.value() != roots[0]) {
          log()->error("Genesis mismatch: got cids:{}, expected:{}",
                       fmt::join(toStrings(roots), " "),
                       config.genesis_cid.value().toString().value());
          return Error::GENESIS_MISMATCH;
        }
      } else {
        config.genesis_cid = roots[0];
        log()->debug("Genesis found in {}: {}",
                     config.car_file_name,
                     config.genesis_cid.value().toString().value());
      }

      return fc::outcome::success();
    }

    outcome::result<void> initNetworkName(
        const primitives::tipset::Tipset &genesis_tipset,
        const std::shared_ptr<storage::ipfs::IpfsDatastore> &ipld,
        Config &config) {
      OUTCOME_TRY(init_state,
                  vm::state::StateTreeImpl(
                      ipld, genesis_tipset.blks[0].parent_state_root)
                      .state<vm::actor::builtin::v0::init::InitActorState>(
                          vm::actor::kInitAddress));
      config.network_name = init_state.network_name;
      return outcome::success();
    }

  }  // namespace

  outcome::result<NodeObjects> createNodeObjects(Config &config) {
    NodeObjects o;

    std::shared_ptr<sync::IndexDbBackend> index_db_backend;

    log()->debug("Creating storage...");

    bool creating_new_db = false;

    if (config.storage_path == "memory") {
      OUTCOME_TRYA(index_db_backend, sync::IndexDbBackend::create(":memory:"));
      o.ipld = std::make_shared<storage::ipfs::InMemoryDatastore>();
      o.kv_store = std::make_shared<storage::InMemoryStorage>();
      creating_new_db = true;
    } else {
      leveldb::Options options;
      if (!config.car_file_name.empty()) {
        options.create_if_missing = true;
        options.error_if_exists = true;
        creating_new_db = true;
      }
      auto leveldb_res =
          storage::LevelDB::create(config.storage_path, std::move(options));
      if (!leveldb_res) {
        return Error::STORAGE_INIT_ERROR;
      }
      o.ipld = std::make_shared<storage::ipfs::LeveldbDatastore>(
          leveldb_res.value());
      o.kv_store = std::move(leveldb_res.value());

      OUTCOME_TRYA(
          index_db_backend,
          sync::IndexDbBackend::create(config.storage_path + "/index.db"));
    }

    if (creating_new_db) {
      log()->debug("Loading initial car file...");
      OUTCOME_TRY(loadCar(*o.ipld, config));
    }

    log()->debug("Creating chain DB...");

    o.index_db = std::make_shared<sync::IndexDb>(std::move(index_db_backend));
    o.chain_db = std::make_shared<sync::ChainDb>();
    OUTCOME_TRY(o.chain_db->init(
        o.ipld, o.index_db, config.genesis_cid, creating_new_db));

    if (!config.genesis_cid) {
      config.genesis_cid = o.chain_db->genesisCID();
    }

    OUTCOME_TRY(initNetworkName(o.chain_db->genesisTipset(), o.ipld, config));
    log()->info("Network name: {}", config.network_name);

    auto genesis_timestamp = o.chain_db->genesisTipset().blks[0].timestamp;

    // TODO (artem): ts in nanosec ? resolve
    genesis_timestamp /= 1000000000;

    log()->info("Genesis: {}, timestamp {}",
                config.genesis_cid.value().toString().value(),
                genesis_timestamp);

    o.utc_clock = std::make_shared<clock::UTCClockImpl>();

    o.chain_epoch_clock = std::make_shared<clock::ChainEpochClockImpl>(
        clock::UnixTime(genesis_timestamp));

    log()->debug("Creating host...");

    // TODO useKeypair
    auto injector = libp2p::injector::makeHostInjector<
        boost::di::extension::shared_config>(
        boost::di::bind<clock::UTCClock>.template to<clock::UTCClockImpl>());

    o.io_context = injector.create<std::shared_ptr<boost::asio::io_context>>();

    o.scheduler =
        injector.create<std::shared_ptr<libp2p::protocol::Scheduler>>();

    o.host = injector.create<std::shared_ptr<libp2p::Host>>();

    OUTCOME_TRY(o.host->listen(config.listen_address));

    log()->debug("Creating protocols...");

    auto identify_protocol =
        injector.create<std::shared_ptr<libp2p::protocol::Identify>>();
    auto identify_push_protocol =
        injector.create<std::shared_ptr<libp2p::protocol::IdentifyPush>>();
    auto identify_delta_protocol =
        injector.create<std::shared_ptr<libp2p::protocol::IdentifyDelta>>();

    o.identify =
        std::make_shared<sync::Identify>(o.host,
                                         std::move(identify_protocol),
                                         std::move(identify_push_protocol),
                                         std::move(identify_delta_protocol));

    o.say_hello =
        std::make_shared<sync::SayHello>(o.host, o.scheduler, o.utc_clock);

    o.receive_hello = std::make_shared<sync::ReceiveHello>(o.host, o.utc_clock);

    o.gossip = libp2p::protocol::gossip::create(
        o.scheduler, o.host, config.gossip_config);

    using libp2p::protocol::gossip::ByteArray;
    o.gossip->setMessageIdFn(
        [](const ByteArray &from, const ByteArray &seq, const ByteArray &data) {
          auto h = crypto::blake2b::blake2b_256(data);
          return ByteArray(h.data(), h.data() + h.size());
        });

    // o.graphsync =
    // TODO (artem) default service handler for GS

    log()->debug("Creating chain loaders...");

    o.blocksync_client =
        std::make_shared<fc::sync::blocksync::BlocksyncClient>(o.host, o.ipld);

    o.blocksync_server =
        std::make_shared<fc::sync::blocksync::BlocksyncServer>(o.host, o.ipld);

    o.tipset_loader = std::make_shared<fc::sync::TipsetLoader>(
        o.blocksync_client, o.chain_db);

    o.vm_interpreter = std::make_shared<vm::interpreter::CachedInterpreter>(
        std::make_shared<vm::interpreter::InterpreterImpl>(
            std::make_shared<vm::runtime::TipsetRandomness>(o.ipld)),
        o.kv_store);

    o.syncer = std::make_shared<sync::Syncer>(o.scheduler,
                                              o.tipset_loader,
                                              o.chain_db,
                                              o.kv_store,
                                              o.vm_interpreter,
                                              o.ipld);

    log()->debug("Creating chain store...");

    auto weight_calculator =
        std::make_shared<blockchain::weight::WeightCalculatorImpl>(o.ipld);

    auto power_table = std::make_shared<power::PowerTableImpl>();

    auto bls_provider = std::make_shared<crypto::bls::BlsProviderImpl>();

    auto secp_provider =
        std::make_shared<crypto::secp256k1::Secp256k1ProviderImpl>();

    auto block_validator =
        std::make_shared<blockchain::block_validator::BlockValidatorImpl>(
            o.ipld,
            o.utc_clock,
            o.chain_epoch_clock,
            weight_calculator,
            power_table,
            bls_provider,
            secp_provider,
            o.vm_interpreter);

    o.chain_store =
        std::make_shared<sync::ChainStoreImpl>(o.chain_db,
                                               o.ipld,
                                               o.kv_store,
                                               weight_calculator,
                                               std::move(block_validator));

    /* TODO(artem): miner api methods in node, keystore, etc (??? resolve)

       auto mpool =
           storage::mpool::Mpool::create(o.ipld, o.vm_interpreter,
       o.chain_store);

       auto msg_waiter =
           storage::blockchain::MsgWaiter::create(o.ipld, o.chain_store);

       auto key_store = std::make_shared<storage::keystore::InMemoryKeyStore>(
           bls_provider, secp_provider);

       OUTCOME_TRY(keypair, bls_provider->generateKeyPair());

       drand::ChainInfo info {
           .key{keypair.public_key},
           .genesis{genesis_timestamp},
           .period{clock::kEpochDuration}
       };

       std::vector<std::string> drand_servers;

       auto beaconizer = std::make_shared<drand::BeaconizerImpl>(
           o.io_context, o.utc_clock, o.scheduler, info, drand_servers, 111);

       auto drand_schedule = std::make_shared<drand::DrandScheduleImpl>(
           // TODO (???)
       );

       o.api = std::make_shared<api::Api>(o.chain_store,
                                          weight_calculator,
                                          o.ipld,
                                          mpool,
                                          o.vm_interpreter,
                                          msg_waiter,
                                          beaconizer,
                                          drand_schedule,
                                          o.pubsub_gate,
                                          key_store);
   */
    return o;
  }

}  // namespace fc::node

OUTCOME_CPP_DEFINE_CATEGORY(fc::node, Error, e) {
  using E = fc::node::Error;

  switch (e) {
    case E::STORAGE_INIT_ERROR:
      return "cannot initialize storage";
    case E::CAR_FILE_OPEN_ERROR:
      return "cannot open initial car file";
    case E::CAR_FILE_SIZE_ABOVE_LIMIT:
      return "car file size above limit";
    case E::NO_GENESIS_BLOCK:
      return "no genesis block";
    case E::GENESIS_MISMATCH:
      return "genesis mismatch";
    default:
      break;
  }
  return "node::Error: unknown error";
}
