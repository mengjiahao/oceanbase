/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#ifndef OCEANBASE_MVCC_OB_MVCC_TRANS_CTX_
#define OCEANBASE_MVCC_OB_MVCC_TRANS_CTX_
#include "lib/utility/ob_macro_utils.h"
#include "lib/utility/utility.h"
#include "common/ob_tablet_id.h"
#include "ob_row_data.h"
#include "ob_mvcc_row.h"
#include "ob_mvcc.h"
#include "storage/memtable/ob_memtable_key.h"
#include "storage/tx/ob_trans_define.h"
#include "storage/memtable/mvcc/ob_tx_callback_list.h"
#include "storage/tablelock/ob_table_lock_common.h"

namespace oceanbase
{
namespace common
{
class ObTabletID;
};
namespace memtable
{
class ObMemtableCtxCbAllocator;
class ObIMemtable;
class ObMemtable;
enum class MutatorType; 

class ObITransCallback;
struct RedoDataNode
{
  void set(const ObMemtableKey *key,
           const ObRowData &old_row,
           const ObRowData &new_row,
           const blocksstable::ObDmlFlag dml_flag,
           const uint32_t modify_count,
           const uint32_t acc_checksum,
           const int64_t version,
           const int32_t flag,
           const int64_t seq_no,
           const common::ObTabletID &tablet_id);
  void set_callback(ObITransCallback *callback) { callback_ = callback; }
  ObMemtableKey key_;
  ObRowData old_row_;
  ObRowData new_row_;
  blocksstable::ObDmlFlag dml_flag_;
  uint32_t modify_count_;
  uint32_t acc_checksum_;
  int64_t version_;
  int32_t flag_;
  int64_t seq_no_;
  ObITransCallback *callback_;
  common::ObTabletID tablet_id_;
};

struct TableLockRedoDataNode
{
  void set(const ObMemtableKey *key,
           const transaction::tablelock::ObTableLockOp &lock_op,
           const common::ObTabletID &tablet_id,
           ObITransCallback *callback);
  ObMemtableKey key_;
  int64_t seq_no_;
  ObITransCallback *callback_;
  common::ObTabletID tablet_id_;

  transaction::tablelock::ObLockID lock_id_;
  transaction::tablelock::ObTableLockOwnerID owner_id_;
  transaction::tablelock::ObTableLockMode lock_mode_;
  transaction::tablelock::ObTableLockOpType lock_op_type_;
  int64_t create_timestamp_;
  int64_t create_schema_version_;
};

class ObMemtableCtx;
class ObTxCallbackList;

class ObITransCallbackIterator
{
public:
  ObITransCallbackIterator(): cur_(nullptr) {}
  ObITransCallbackIterator(ObITransCallback *cur): cur_(cur) {}
  void reset() { cur_ = nullptr; }
  ObITransCallbackIterator& operator=(const ObITransCallbackIterator &that)
  {
    cur_ = that.cur_;
    return *this;
  }
  ObITransCallback* operator*() { return cur_; }
  ObITransCallback* operator*() const { return cur_; }
  bool operator==(const ObITransCallbackIterator &that) { return cur_ == that.cur_; }
  bool operator!=(const ObITransCallbackIterator &that) { return cur_ != that.cur_; }
  ObITransCallbackIterator operator+(int i)
  {
    ObITransCallbackIterator ret(cur_);
    while (i != 0) {
      if (i > 0) {
        ++ret;
        --i;
      } else {
        --ret;
        ++i;
      }
    }
    return ret;
  }
  ObITransCallbackIterator operator-(int i)
  {
    return (*this) + (-i);
  }
  ObITransCallbackIterator& operator++() // ++iter
  {
    cur_ = cur_->get_next();
    return *this;
  }
  ObITransCallbackIterator& operator--() // --iter
  {
    cur_ = cur_->get_prev();
    return *this;
  }
  ObITransCallbackIterator operator++(int) // iter++
  {
    cur_ = cur_->get_next();
    return ObITransCallbackIterator(cur_->get_prev());
  }
  ObITransCallbackIterator operator--(int) // iter--
  {
    cur_ = cur_->get_prev();
    return ObITransCallbackIterator(cur_->get_next());
  }
private:
  ObITransCallback *cur_;
};

// 事务commit/abort的callback不允许出错，也没法返回错误，就算返回错误调用者也没法处理，所以callback都返回void
class ObTransCallbackMgr
{
public:
  class WRLockGuard
  {
  public:
    explicit WRLockGuard(const common::SpinRWLock &rwlock);
    ~WRLockGuard() {}
  private:
    common::ObSimpleTimeGuard time_guard_; // print log and lbt, if the lock is held too much time.
    common::SpinWLockGuard lock_guard_;
  };
  class RDLockGuard
  {
  public:
    explicit RDLockGuard(const common::SpinRWLock &rwlock);
    ~RDLockGuard() {}
  private:
    common::ObSimpleTimeGuard time_guard_; // print log and lbt, if the lock is held too much time.
    common::SpinRLockGuard lock_guard_;
  };

  friend class ObITransCallbackIterator;
  enum { MAX_CALLBACK_LIST_COUNT = OB_MAX_CPU_NUM };
  enum {
    PARALLEL_STMT = -1
  };
public:
  ObTransCallbackMgr(ObIMvccCtx &host, ObMemtableCtxCbAllocator &cb_allocator)
    : host_(host),
      callback_list_(*this),
      callback_lists_(NULL),
      rwlock_(ObLatchIds::MEMTABLE_CALLBACK_LIST_MGR_LOCK),
      parallel_stat_(0),
      for_replay_(false),
      leader_changed_(false),
      callback_main_list_append_count_(0),
      callback_slave_list_append_count_(0),
      callback_slave_list_merge_count_(0),
      callback_remove_for_trans_end_count_(0),
      callback_remove_for_remove_memtable_count_(0),
      callback_remove_for_fast_commit_count_(0),
      callback_remove_for_rollback_to_count_(0),
      pending_log_size_(0),
      flushed_log_size_(0),
      cb_allocator_(cb_allocator)
  {
  }
  ~ObTransCallbackMgr() {}
  void reset();
  ObIMvccCtx &get_ctx() { return host_; }
  int append(ObITransCallback *node);
  int before_append(ObITransCallback *node);
  int after_append(ObITransCallback *node, const int ret_code);
  void trans_start();
  void calc_checksum_all();
  void print_callbacks();
  void elr_trans_preparing();
  int trans_end(const bool commit);
  int replay_fail(const int64_t log_timestamp);
  int replay_succ(const int64_t log_timestamp);
  int rollback_to(const int64_t seq_no,
                  const int64_t from_seq_no);
  void set_for_replay(const bool for_replay);
  bool is_for_replay() const { return ATOMIC_LOAD(&for_replay_); }
  int remove_callbacks_for_fast_commit(bool &has_remove);
  int remove_callback_for_uncommited_txn(memtable::ObIMemtable *memtable);
  int get_memtable_key_arr(transaction::ObMemtableKeyArray &memtable_key_arr);
  void acquire_callback_list();
  void revert_callback_list();
  // TODO by fengshuo.fs: fix this implement
  ObITransCallbackIterator begin() { return ObITransCallbackIterator(get_guard_()); }
  ObITransCallbackIterator end() { return ObITransCallbackIterator(get_guard_()); }
  common::SpinRWLock& get_rwlock() { return rwlock_; }
private:
  void wakeup_waiting_txns_();
public:
  int calc_checksum_before_log_ts(const int64_t log_ts,
                                  uint64_t &checksum,
                                  int64_t &checksum_log_ts);
  void update_checksum(const uint64_t checksum,
                       const int64_t checksum_log_ts);
  int clean_unlog_callbacks(int64_t &removed_cnt);
  // when not inc, return -1
  int64_t inc_pending_log_size(const int64_t size);
  void try_merge_multi_callback_lists(const int64_t new_size, const int64_t size, const bool is_logging_blocked);
  void inc_flushed_log_size(const int64_t size) { UNUSED(ATOMIC_FAA(&flushed_log_size_, size)); }
  void clear_pending_log_size() { ATOMIC_STORE(&pending_log_size_, 0); }
  int64_t get_pending_log_size() { return ATOMIC_LOAD(&pending_log_size_); }
  int64_t get_flushed_log_size() { return ATOMIC_LOAD(&flushed_log_size_); }
  bool is_all_redo_submitted(ObITransCallback *generate_cursor)
  {
    return (ObITransCallback *)callback_list_.get_tail() == generate_cursor;
  }
  void merge_multi_callback_lists();
  void reset_pdml_stat();
  uint64_t get_main_list_length() const
  { return callback_list_.get_length(); }
  int64_t get_callback_main_list_append_count() const
  { return callback_main_list_append_count_; }
  int64_t get_callback_slave_list_append_count() const
  { return callback_slave_list_append_count_; }
  int64_t get_callback_slave_list_merge_count() const
  { return callback_slave_list_merge_count_; }
  int64_t get_callback_remove_for_trans_end_count() const
  { return callback_remove_for_trans_end_count_; }
  int64_t get_callback_remove_for_remove_memtable_count() const
  { return callback_remove_for_remove_memtable_count_; }
  int64_t get_callback_remove_for_fast_commit_count() const
  { return callback_remove_for_fast_commit_count_; }
  int64_t get_callback_remove_for_rollback_to_count() const
  { return callback_remove_for_rollback_to_count_; }
  void add_main_list_append_cnt(int64_t cnt = 1)
  { ATOMIC_AAF(&callback_main_list_append_count_, cnt); }
  void add_slave_list_append_cnt(int64_t cnt = 1)
  { ATOMIC_AAF(&callback_slave_list_append_count_, cnt); }
  void add_slave_list_merge_cnt(int64_t cnt = 1)
  { ATOMIC_AAF(&callback_slave_list_merge_count_, cnt); }
  void add_tx_end_callback_remove_cnt(int64_t cnt = 1)
  { ATOMIC_AAF(&callback_remove_for_trans_end_count_, cnt); }
  void add_release_memtable_callback_remove_cnt(int64_t cnt = 1)
  { ATOMIC_AAF(&callback_remove_for_remove_memtable_count_, cnt); }
  void add_fast_commit_callback_remove_cnt(int64_t cnt= 1)
  { ATOMIC_AAF(&callback_remove_for_fast_commit_count_, cnt); }
  void add_rollback_to_callback_remove_cnt(int64_t cnt = 1)
  { ATOMIC_AAF(&callback_remove_for_rollback_to_count_, cnt); }
  int64_t get_checksum() const { return callback_list_.get_checksum(); }
  int64_t get_tmp_checksum() const { return callback_list_.get_tmp_checksum(); }
  int64_t get_checksum_log_ts() const { return callback_list_.get_checksum_log_ts(); }
  transaction::ObPartTransCtx *get_trans_ctx() const;
private:
  bool is_all_redo_submitted(ObMvccRowCallback *generate_cursor)
  {
    return (ObMvccRowCallback *)callback_list_.get_tail() == generate_cursor;
  }
  void force_merge_multi_callback_lists();
private:
  ObITransCallback *get_guard_() { return callback_list_.get_guard(); }
private:
  ObIMvccCtx &host_;
  ObTxCallbackList callback_list_;
  ObTxCallbackList *callback_lists_;
  common::SpinRWLock rwlock_;
  union {
    struct {
      int32_t ref_cnt_;
      int32_t tid_;
    };
    int64_t parallel_stat_;
  };
  bool for_replay_;
  bool leader_changed_;
  // statistics for callback remove
  int64_t callback_main_list_append_count_;
  int64_t callback_slave_list_append_count_;
  int64_t callback_slave_list_merge_count_;
  int64_t callback_remove_for_trans_end_count_;
  int64_t callback_remove_for_remove_memtable_count_;
  int64_t callback_remove_for_fast_commit_count_;
  int64_t callback_remove_for_rollback_to_count_;
  // current log size in leader participant
  int64_t pending_log_size_;
  // current flushed log size in leader participant
  int64_t flushed_log_size_;
  ObMemtableCtxCbAllocator &cb_allocator_;
};

//class ObIMvccCtx;
class ObMvccRowCallback final : public ObITransCallback
{
public:
  ObMvccRowCallback(ObIMvccCtx &ctx, ObMvccRow& value, ObMemtable *memtable) :
      ObITransCallback(),
      ctx_(ctx),
      value_(value),
      tnode_(NULL),
      data_size_(-1),
      memtable_(memtable),
      is_link_(false),
      not_calc_checksum_(false),
      seq_no_(0)
  {}
  ObMvccRowCallback(ObMvccRowCallback &cb, ObMemtable *memtable) :
      ObITransCallback(cb.need_fill_redo_, cb.need_submit_log_),
      ctx_(cb.ctx_),
      value_(cb.value_),
      tnode_(cb.tnode_),
      data_size_(cb.data_size_),
      memtable_(memtable),
      is_link_(cb.is_link_),
      not_calc_checksum_(cb.not_calc_checksum_),
      seq_no_(cb.seq_no_)
  {
    (void)key_.encode(cb.key_.get_rowkey());
  }
  virtual ~ObMvccRowCallback() {}
  int link_trans_node();
  void unlink_trans_node();
  void set_is_link() { is_link_ = true; }
  void unset_is_link() { is_link_ = false; }
  void set(const ObMemtableKey *key,
            ObMvccTransNode *node,
            const int64_t data_size,
            const ObRowData *old_row,
            const bool is_replay,
            const int64_t seq_no)
  {
    UNUSED(is_replay);

    if (NULL != key) {
      key_.encode(*key);
    }

    tnode_ = node;
    data_size_ = data_size;
    if (NULL != old_row) {
      old_row_ = *old_row;
      if (old_row_.size_ == 0 && old_row_.data_ != NULL) {
        ob_abort();
      }
    } else {
      old_row_.reset();
    }
    seq_no_ = seq_no;
    if (tnode_) {
      tnode_->set_seq_no(seq_no_);
    }
  }
  bool on_memtable(const ObIMemtable * const memtable) override;
  ObIMemtable *get_memtable() const override;
  virtual MutatorType get_mutator_type() const override;
  int get_redo(RedoDataNode &node);
  ObIMvccCtx &get_ctx() const { return ctx_; }
  const ObRowData &get_old_row() const { return old_row_; }
  const ObMvccRow &get_mvcc_row() const { return value_; }
  ObMvccTransNode *get_trans_node() { return tnode_; }
  const ObMvccTransNode *get_trans_node() const { return tnode_; }
  const ObMemtableKey *get_key() { return &key_; }
  int get_memtable_key(uint64_t &table_id, common::ObStoreRowkey &rowkey) const;
  bool is_logging_blocked() const override;
  int64_t get_seq_no() const { return seq_no_; }
  int get_trans_id(transaction::ObTransID &trans_id) const;
  int get_cluster_version(uint64_t &cluster_version) const override;
  transaction::ObTransCtx *get_trans_ctx() const;
  int64_t to_string(char *buf, const int64_t buf_len) const;
  bool log_synced() const override { return INT64_MAX != log_ts_; }
  virtual int before_append(const bool is_replay) override;
  virtual int after_append(const bool is_replay, const int ret_code) override;
  virtual int log_submitted() override;
  virtual int undo_log_submitted() override;
  int64_t get_data_size()
  {
    return data_size_;
  }
  virtual int clean();
  virtual int del();
  virtual int checkpoint_callback();
  virtual int log_sync(const int64_t log_ts) override;
  virtual int log_sync_fail() override;
  virtual int print_callback() override;
  virtual blocksstable::ObDmlFlag get_dml_flag() const override;
  virtual void set_not_calc_checksum(const bool not_calc_checksum) override
  {
    not_calc_checksum_ = not_calc_checksum;
  }
  const common::ObTabletID &get_tablet_id() const;
  int merge_memtable_key(transaction::ObMemtableKeyArray &memtable_key_arr);
private:
  virtual int trans_commit() override;
  virtual int trans_abort() override;
  virtual int rollback_callback() override;
  virtual int calc_checksum(const int64_t checksum_log_ts,
                            ObBatchChecksum *checksumer) override;
  virtual int elr_trans_preparing() override;
private:
  int link_and_get_next_node(ObMvccTransNode *&next);
  int row_delete();
  int merge_memtable_key(transaction::ObMemtableKeyArray &memtable_key_arr,
      ObMemtableKey &memtable_key, const common::ObTabletID &tablet_id);
  int clean_unlog_cb();
  void inc_unsubmitted_cnt_();
  void inc_unsynced_cnt_();
  int dec_unsubmitted_cnt_();
  int dec_unsynced_cnt_();
  int wakeup_row_waiter_if_need_();
private:
  ObIMvccCtx &ctx_;
  ObMemtableKey key_;
  ObMvccRow &value_;
  ObMvccTransNode *tnode_;
  int64_t data_size_;
  ObRowData old_row_;
  ObMemtable *memtable_;
  struct {
    bool is_link_ : 1;
    bool not_calc_checksum_ : 1;
  };
  int64_t seq_no_;
};

}; // end namespace memtable
}; // end namespace oceanbase

#endif /* OCEANBASE_MVCC_OB_MVCC_TRANS_CTX_ */

