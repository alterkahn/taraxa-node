#include "db_storage.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/split.hpp>

#include "full_node.hpp"
#include "rocksdb/utilities/checkpoint.h"
#include "transaction.hpp"
#include "vote.hpp"

namespace taraxa {
using namespace std;
using namespace dev;
using namespace rocksdb;
namespace fs = std::filesystem;

DbStorage::DbStorage(fs::path const& path, uint32_t db_snapshot_each_n_pbft_block, uint32_t db_max_snapshots,
                     uint32_t db_revert_to_period, addr_t node_addr, bool rebuild)
    : path_(path),
      handles_(Columns::all.size()),
      db_snapshot_each_n_pbft_block_(db_snapshot_each_n_pbft_block),
      db_max_snapshots_(db_max_snapshots),
      node_addr_(node_addr) {
  db_path_ = (path / db_dir);
  state_db_path_ = (path / state_db_dir);

  if (rebuild) {
    const std::string backup_label = "-rebuild-backup-";
    auto timestamp = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    auto backup_db_path = db_path_;
    backup_db_path += (backup_label + timestamp);
    auto backup_state_db_path = state_db_path_;
    backup_state_db_path += (backup_label + timestamp);
    fs::rename(db_path_, backup_db_path);
    fs::rename(state_db_path_, backup_state_db_path);
    db_path_ = backup_db_path;
    state_db_path_ = backup_state_db_path;
  }

  fs::create_directories(db_path_);
  rocksdb::Options options;
  options.create_missing_column_families = true;
  options.create_if_missing = true;
  vector<ColumnFamilyDescriptor> descriptors;
  std::transform(Columns::all.begin(), Columns::all.end(), std::back_inserter(descriptors),
                 [](const Column& col) { return ColumnFamilyDescriptor(col.name, ColumnFamilyOptions()); });
  LOG_OBJECTS_CREATE("DBS");

  // Iterate over the db folders and populate snapshot set
  loadSnapshots();

  // Revert to period if needed
  if (db_revert_to_period) {
    recoverToPeriod(db_revert_to_period);
  }

  checkStatus(DB::Open(options, db_path_.string(), descriptors, &handles_, &db_));
  dag_blocks_count_.store(getStatusField(StatusDbField::DagBlkCount));
  dag_edge_count_.store(getStatusField(StatusDbField::DagEdgeCount));

  auto major_version = getStatusField(StatusDbField::DbMajorVersion);
  auto minor_version = getStatusField(StatusDbField::DbMinorVersion);
  if (major_version == 0 && minor_version == 0) {
    saveStatusField(StatusDbField::DbMajorVersion, FullNode::c_database_major_version);
    saveStatusField(StatusDbField::DbMinorVersion, FullNode::c_database_minor_version);
  } else {
    if (major_version != FullNode::c_database_major_version) {
      throw DbException(string("Database version mismatch. Version on disk ") +
                        getFormattedVersion(major_version, minor_version) + " Node version:" +
                        getFormattedVersion(FullNode::c_database_major_version, FullNode::c_database_minor_version));
    } else if (minor_version != FullNode::c_database_minor_version) {
      minor_version_changed_ = true;
    }
  }
}

void DbStorage::loadSnapshots() {
  // Find all the existing folders containing db and state_db snapshots
  for (fs::directory_iterator itr(path_); itr != fs::directory_iterator(); ++itr) {
    std::string fileName = itr->path().filename().string();
    bool delete_dir = false;
    uint64_t dir_period = 0;

    try {
      // Check for db or state_db prefix
      if (boost::starts_with(fileName, db_dir) && fileName.size() > db_dir.size()) {
        dir_period = stoi(fileName.substr(db_dir.size()));
      } else if (boost::starts_with(fileName, state_db_dir) && fileName.size() > state_db_dir.size()) {
        dir_period = stoi(fileName.substr(state_db_dir.size()));
      } else {
        continue;
      }
    } catch (...) {
      // This should happen if there is a incorrect formatted folder, just skip
      // it
      LOG(log_er_) << "Unexpected file: " << fileName;
      continue;
    }
    LOG(log_dg_) << "Found snapshot: " << fileName;
    snapshots_.insert(dir_period);
  }
}

bool DbStorage::createSnapshot(uint64_t const& period) {
  // Only creates snapshot each db_snapshot_each_n_pbft_block_ periods
  if (db_snapshot_each_n_pbft_block_ > 0 && period % db_snapshot_each_n_pbft_block_ == 0) {
    LOG(log_nf_) << "Creating DB snapshot on period: " << period;

    // Create rocskd checkpoint/snapshot
    rocksdb::Checkpoint* checkpoint;
    auto status = rocksdb::Checkpoint::Create(db_, &checkpoint);
    // Scope is to delete checkpoint object as soon as we don't need it anymore
    {
      unique_ptr<rocksdb::Checkpoint> realPtr = unique_ptr<rocksdb::Checkpoint>(checkpoint);
      checkStatus(status);
      auto snapshot_path = db_path_;
      snapshot_path += std::to_string(period);
      status = checkpoint->CreateCheckpoint(snapshot_path.string());
    }
    checkStatus(status);
    snapshots_.insert(period);

    // Delete any snapshot over db_max_snapshots_
    if (db_max_snapshots_ && snapshots_.size() > db_max_snapshots_) {
      while (snapshots_.size() > db_max_snapshots_) {
        auto snapshot = snapshots_.begin();
        deleteSnapshot(*snapshot);
        snapshots_.erase(snapshot);
      }
    }
    return true;
  }
  return false;
}

void DbStorage::recoverToPeriod(uint64_t const& period) {
  LOG(log_nf_) << "Revet to snapshot from period: " << period;

  // Construct the snapshot folder names
  auto period_path = db_path_;
  auto period_state_path = state_db_path_;
  period_path += to_string(period);
  period_state_path += to_string(period);

  if (fs::exists(period_path)) {
    LOG(log_dg_) << "Deleting current db/state";
    fs::remove_all(db_path_);
    fs::remove_all(state_db_path_);
    LOG(log_dg_) << "Reverting to period: " << period;
    fs::rename(period_path, db_path_);
    fs::rename(period_state_path, state_db_path_);
    // Delete all the period snapshots that are after the snapshot we are
    // reverting to
    LOG(log_dg_) << "Deleting newer periods:";
    for (auto snapshot = snapshots_.begin(); snapshot != snapshots_.end();) {
      if (*snapshot > period) {
        deleteSnapshot(*snapshot);
        snapshots_.erase(snapshot++);
      } else {
        snapshot++;
      }
    }
  } else {
    LOG(log_er_) << "Period snapshot missing";
  }
}

void DbStorage::deleteSnapshot(uint64_t const& period) {
  LOG(log_nf_) << "Deleting " << period << "snapshot";

  // Construct the snapshot folder names
  auto period_path = db_path_;
  auto period_state_path = state_db_path_;
  period_path += to_string(period);
  period_state_path += to_string(period);

  // Delete both db and state_db folder
  fs::remove_all(period_path);
  LOG(log_dg_) << "Deleted folder: " << period_path;
  fs::remove_all(period_state_path);
  LOG(log_dg_) << "Deleted folder: " << period_path;
}

DbStorage::~DbStorage() {
  db_->Close();
  delete db_;
}

void DbStorage::checkStatus(rocksdb::Status const& status) {
  if (status.ok()) return;
  throw DbException(string("Db error. Status code: ") + to_string(status.code()) +
                    " SubCode: " + to_string(status.subcode()) + " Message:" + status.ToString());
}

DbStorage::Batch DbStorage::createWriteBatch() { return Batch(weak_this_.lock()); }

dev::bytes DbStorage::getDagBlockRaw(blk_hash_t const& hash) {
  return asBytes(lookup(hash.asBytes(), Columns::dag_blocks));
}

std::shared_ptr<DagBlock> DbStorage::getDagBlock(blk_hash_t const& hash) {
  auto blk_bytes = getDagBlockRaw(hash);
  if (blk_bytes.size() > 0) {
    return std::make_shared<DagBlock>(blk_bytes);
  }
  return nullptr;
}

std::string DbStorage::getBlocksByLevel(level_t level) { return lookup(level, Columns::dag_blocks_index); }

std::vector<std::shared_ptr<DagBlock>> DbStorage::getDagBlocksAtLevel(level_t level, int number_of_levels) {
  std::vector<std::shared_ptr<DagBlock>> res;
  for (int i = 0; i < number_of_levels; i++) {
    if (level + i == 0) continue;  // Skip genesis
    string entry = getBlocksByLevel(level + i);
    if (entry.empty()) break;
    vector<string> blocks;
    boost::split(blocks, entry, boost::is_any_of(","));
    for (auto const& block : blocks) {
      auto blk = getDagBlock(blk_hash_t(block));
      if (blk) {
        res.push_back(blk);
      }
    }
  }
  return res;
}

std::map<blk_hash_t, bool> DbStorage::getAllDagBlockState() {
  std::map<blk_hash_t, bool> res;
  auto i = u_ptr(db_->NewIterator(read_options_, handle(Columns::dag_blocks_state)));
  for (i->SeekToFirst(); i->Valid(); i->Next()) {
    res[blk_hash_t(asBytes(i->key().ToString()))] = (bool)*(uint8_t*)(i->value().data());
  }
  return res;
}

void DbStorage::saveTransaction(Transaction const& trx) {
  insert(Columns::transactions, trx.getHash().asBytes(), *trx.rlp());
}

void DbStorage::saveTransactionStatus(trx_hash_t const& trx_hash, TransactionStatus const& status) {
  insert(Columns::trx_status, trx_hash.asBytes(), (uint16_t)status);
}

TransactionStatus DbStorage::getTransactionStatus(trx_hash_t const& hash) {
  auto data = lookup(hash.asBytes(), Columns::trx_status);
  if (!data.empty()) {
    return (TransactionStatus) * (uint16_t*)&data[0];
  }
  return TransactionStatus::not_seen;
}

std::map<trx_hash_t, TransactionStatus> DbStorage::getAllTransactionStatus() {
  std::map<trx_hash_t, TransactionStatus> res;
  auto i = u_ptr(db_->NewIterator(read_options_, handle(Columns::trx_status)));
  for (i->SeekToFirst(); i->Valid(); i->Next()) {
    res[trx_hash_t(asBytes(i->key().ToString()))] = (TransactionStatus) * (uint16_t*)(i->value().data());
  }
  return res;
}

dev::bytes DbStorage::getTransactionRaw(trx_hash_t const& hash) {
  return asBytes(lookup(hash.asBytes(), Columns::transactions));
}

std::shared_ptr<Transaction> DbStorage::getTransaction(trx_hash_t const& hash) {
  auto trx_bytes = getTransactionRaw(hash);
  if (trx_bytes.size() > 0) {
    return std::make_shared<Transaction>(trx_bytes);
  }
  return nullptr;
}

std::shared_ptr<std::pair<Transaction, taraxa::bytes>> DbStorage::getTransactionExt(trx_hash_t const& hash) {
  auto trx_bytes = asBytes(lookup(hash.asBytes(), Columns::transactions));
  if (trx_bytes.size() > 0) {
    return std::make_shared<std::pair<Transaction, taraxa::bytes>>(trx_bytes, trx_bytes);
  }
  return nullptr;
}

bool DbStorage::transactionInDb(trx_hash_t const& hash) {
  return !lookup(hash.asBytes(), Columns::transactions).empty();
}

uint64_t DbStorage::getStatusField(StatusDbField const& field) {
  auto status = lookup((uint8_t)field, Columns::status);
  if (!status.empty()) return *(uint64_t*)&status[0];
  return 0;
}

void DbStorage::saveStatusField(StatusDbField const& field, uint64_t const& value) {
  insert(Columns::status, (uint8_t)field, value);
}

// PBFT
uint64_t DbStorage::getPbftMgrField(PbftMrgField const& field) {
  auto status = lookup((uint8_t)field, Columns::pbft_mgr);
  if (!status.empty()) {
    return *(uint64_t*)&status[0];
  }
  return 1;
}

void DbStorage::savePbftMgrField(PbftMrgField const& field, uint64_t const& value) {
  insert(Columns::pbft_mgr, (uint8_t)field, value);
}

std::shared_ptr<PbftBlock> DbStorage::getPbftBlock(blk_hash_t const& hash) {
  auto block = lookup(hash, Columns::pbft_blocks);
  if (!block.empty()) {
    return s_ptr(new PbftBlock(dev::RLP(block)));
  }
  return nullptr;
}

bool DbStorage::pbftBlockInDb(blk_hash_t const& hash) { return !lookup(hash, Columns::pbft_blocks).empty(); }

shared_ptr<blk_hash_t> DbStorage::getPeriodPbftBlock(uint64_t const& period) {
  auto hash = asBytes(lookup(period, Columns::period_pbft_block));
  if (hash.size() > 0) {
    return make_shared<blk_hash_t>(hash);
  }
  return nullptr;
}

string DbStorage::getPbftHead(blk_hash_t const& hash) { return lookup(hash.asBytes(), Columns::pbft_head); }

void DbStorage::savePbftHead(blk_hash_t const& hash, string const& pbft_chain_head_str) {
  insert(Columns::pbft_head, hash.asBytes(), pbft_chain_head_str);
}

bytes DbStorage::getVotes(blk_hash_t const& hash) { return asBytes(lookup(hash, Columns::votes)); }

std::shared_ptr<uint64_t> DbStorage::getDagBlockPeriod(blk_hash_t const& hash) {
  auto period = asBytes(lookup(hash.asBytes(), Columns::dag_block_period));
  if (period.size() > 0) {
    return make_shared<uint64_t>(*(uint64_t*)&period[0]);
  }
  return nullptr;
}

vector<blk_hash_t> DbStorage::getFinalizedDagBlockHashesByAnchor(blk_hash_t const& anchor) {
  auto raw = lookup(anchor, Columns::dag_finalized_blocks);
  if (raw.empty()) {
    return {};
  }
  return RLP(raw).toVector<blk_hash_t>();
}

void DbStorage::forEach(Column const& col, OnEntry const& f) {
  auto i = u_ptr(db_->NewIterator(read_options_, handle(col)));
  for (i->SeekToFirst(); i->Valid(); i->Next()) {
    if (!f(i->key(), i->value())) {
      break;
    }
  }
}

DbStorage::MultiGetQuery::MultiGetQuery(shared_ptr<DbStorage> const& db, uint capacity) : db_(db) {
  if (capacity) {
    cfs_.reserve(capacity);
    keys_.reserve(capacity);
    str_pool_.reserve(capacity);
  }
}

dev::bytesConstRef DbStorage::MultiGetQuery::get_key(uint pos) {
  auto const& slice = keys_[pos];
  return dev::bytesConstRef((uint8_t*)slice.data(), slice.size());
}

uint DbStorage::MultiGetQuery::size() { return keys_.size(); }

vector<string> DbStorage::MultiGetQuery::execute(bool and_reset) {
  auto _size = size();
  if (_size == 0) {
    return {};
  }
  vector<string> ret(_size);
  uint i = 0;
  for (auto const& s : db_->db_->MultiGet(db_->read_options_, cfs_, keys_, &ret)) {
    if (s.IsNotFound()) {
      ret[i] = "";
    } else {
      checkStatus(s);
    }
    ++i;
  }
  if (and_reset) {
    reset();
  }
  return ret;
}

DbStorage::MultiGetQuery& DbStorage::MultiGetQuery::reset() {
  cfs_.clear();
  keys_.clear();
  str_pool_.clear();
  return *this;
}

void DbStorage::Batch::commit() { checkStatus(db_->db_->Write(db_->write_options_, b_.GetWriteBatch())); }

void DbStorage::Batch::addPbftHead(taraxa::blk_hash_t const& head_hash, std::string const& head_str) {
  put(Columns::pbft_head, head_hash.asBytes(), head_str);
}
void DbStorage::Batch::addPbftCertVotes(const taraxa::blk_hash_t& pbft_block_hash,
                                        const std::vector<Vote>& cert_votes) {
  RLPStream s(cert_votes.size());
  for (auto const& v : cert_votes) {
    s.appendRaw(v.rlp());
  }
  put(Columns::votes, pbft_block_hash, s.out());
}

void DbStorage::Batch::addPbftBlockPeriod(uint64_t const& period, taraxa::blk_hash_t const& pbft_block_hash) {
  put(Columns::period_pbft_block, period, pbft_block_hash.asBytes());
}

void DbStorage::Batch::addDagBlockPeriod(blk_hash_t const& hash, uint64_t const& period) {
  put(Columns::dag_block_period, hash.asBytes(), period);
}

void DbStorage::Batch::putFinalizedDagBlockHashesByAnchor(blk_hash_t const& anchor, vector<blk_hash_t> const& hs) {
  put(DbStorage::Columns::dag_finalized_blocks, anchor, RLPStream().appendVector(hs).out());
}

void DbStorage::Batch::addPbftMgrField(PbftMrgField const& field, uint64_t const& value) {
  put(DbStorage::Columns::pbft_mgr, (uint8_t)field, value);
}

void DbStorage::Batch::addTransaction(Transaction const& trx) {
  put(DbStorage::Columns::transactions, trx.getHash().asBytes(), *trx.rlp());
}

void DbStorage::Batch::saveDagBlock(DagBlock const& blk) {
  // Lock is needed since we are editing some fields
  lock_guard u_lock(db_->dag_blocks_mutex_);
  auto block_bytes = blk.rlp(true);
  auto block_hash = blk.getHash();
  put(Columns::dag_blocks, block_hash.asBytes(), block_bytes);
  auto level = blk.getLevel();
  std::string blocks = db_->getBlocksByLevel(level);
  if (blocks == "") {
    blocks = blk.getHash().toString();
  } else {
    blocks = blocks + "," + blk.getHash().toString();
  }
  put(Columns::dag_blocks_index, level, blocks);
  db_->dag_blocks_count_.fetch_add(1);
  put(Columns::status, (uint8_t)StatusDbField::DagBlkCount, db_->dag_blocks_count_.load());
  // Do not count genesis pivot field
  if (blk.getPivot() == blk_hash_t(0)) {
    db_->dag_edge_count_.fetch_add(blk.getTips().size());
  } else {
    db_->dag_edge_count_.fetch_add(blk.getTips().size() + 1);
  }
  put(Columns::status, (uint8_t)StatusDbField::DagEdgeCount, db_->dag_edge_count_.load());
}

void DbStorage::Batch::addDagBlockState(blk_hash_t const& blk_hash, bool finalized) {
  put(Columns::dag_blocks_state, blk_hash.asBytes(), finalized);
}

void DbStorage::Batch::removeDagBlockState(blk_hash_t const& blk_hash) {
  remove(Columns::dag_blocks_state, blk_hash.asBytes());
}

void DbStorage::Batch::addTransactionStatus(trx_hash_t const& trx, TransactionStatus const& status) {
  put(Columns::trx_status, trx.asBytes(), (uint16_t)status);
}

void DbStorage::Batch::addStatusField(StatusDbField const& field, uint64_t const& value) {
  put(DbStorage::Columns::status, (uint8_t)field, value);
}

void DbStorage::Batch::addPbftBlock(const taraxa::PbftBlock& pbft_block) {
  put(Columns::pbft_blocks, pbft_block.getBlockHash(), pbft_block.rlp(true));
}

}  // namespace taraxa
