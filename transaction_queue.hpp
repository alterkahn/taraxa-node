#ifndef TARAXA_NODE_TRANSACTION_QUEUE_HPP
#define TARAXA_NODE_TRANSACTION_QUEUE_HPP

#include "transaction.hpp"

namespace taraxa {

using std::string;
class DagBlock;
class FullNode;

/**
 */

class TransactionQueue {
 public:
  enum class VerifyMode : uint8_t { normal, skip_verify_sig };
  using listIter = std::list<Transaction>::iterator;
  TransactionQueue() {}

  ~TransactionQueue() { stop(); }

  void start();
  void stop();
  void insert(Transaction const &trx, bool verify);
  Transaction top();
  void pop();
  std::pair<trx_hash_t, listIter> getUnverifiedTransaction();
  void removeTransactionFromBuffer(trx_hash_t const &hash);
  void addTransactionToVerifiedQueue(trx_hash_t const &hash,
                                     std::list<Transaction>::iterator);
  std::unordered_map<trx_hash_t, Transaction> moveVerifiedTrxSnapShot(
      uint16_t max_trx_to_pack = 0);
  std::unordered_map<trx_hash_t, Transaction> getVerifiedTrxSnapShot();
  std::pair<size_t, size_t> getTransactionQueueSize() const;
  std::vector<Transaction> getNewVerifiedTrxSnapShot();
  std::unordered_map<trx_hash_t, Transaction> removeBlockTransactionsFromQueue(
      vec_trx_t const &all_block_trxs);
  unsigned long getVerifiedTrxCount();
  std::shared_ptr<Transaction> getTransaction(trx_hash_t const &hash) const;
  void setFullNode(std::shared_ptr<FullNode> full_node) {
    full_node_ = full_node;
  }

 private:
  using uLock = boost::unique_lock<boost::shared_mutex>;
  using sharedLock = boost::shared_lock<boost::shared_mutex>;
  using upgradableLock = boost::upgrade_lock<boost::shared_mutex>;
  using upgradeLock = boost::upgrade_to_unique_lock<boost::shared_mutex>;
  addr_t getFullNodeAddress() const;
  std::atomic<bool> stopped_ = true;
  bool new_verified_transactions_ = true;
  std::weak_ptr<FullNode> full_node_;

  std::list<Transaction> trx_buffer_;
  std::unordered_map<trx_hash_t, listIter> queued_trxs_;  // all trx
  mutable boost::shared_mutex shared_mutex_for_queued_trxs_;

  std::unordered_map<trx_hash_t, listIter> verified_trxs_;
  mutable boost::shared_mutex shared_mutex_for_verified_qu_;
  std::deque<std::pair<trx_hash_t, listIter>> unverified_hash_qu_;
  mutable boost::shared_mutex shared_mutex_for_unverified_qu_;
  boost::condition_variable_any cond_for_unverified_qu_;

  mutable dev::Logger log_si_{
      dev::createLogger(dev::Verbosity::VerbositySilent, "TRXQU")};
  mutable dev::Logger log_er_{
      dev::createLogger(dev::Verbosity::VerbosityError, "TRXQU")};
  mutable dev::Logger log_wr_{
      dev::createLogger(dev::Verbosity::VerbosityWarning, "TRXQU")};
  mutable dev::Logger log_nf_{
      dev::createLogger(dev::Verbosity::VerbosityInfo, "TRXQU")};
  mutable dev::Logger log_dg_{
      dev::createLogger(dev::Verbosity::VerbosityDebug, "TRXQU")};
};

}  // namespace taraxa

#endif