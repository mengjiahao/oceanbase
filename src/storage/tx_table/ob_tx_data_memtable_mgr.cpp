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

#include "storage/tx_table/ob_tx_data_memtable_mgr.h"
#include "storage/meta_mem/ob_tenant_meta_mem_mgr.h"
#include "storage/tx_storage/ob_ls_service.h"

namespace oceanbase
{
using namespace share;
namespace storage
{
class ObILS;
namespace checkpoint
{
  class ObCheckpointExecutor;
}

int ObTxDataMemtableMgr::init(const common::ObTabletID &tablet_id,
                              const ObLSID &ls_id,
                              ObFreezer *freezer,
                              ObTenantMetaMemMgr *t3m)
{
  int ret = OB_SUCCESS;
  ObLSHandle ls_handle;
  ObTxTable *tx_table = nullptr;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
    STORAGE_LOG(WARN, "ObTxDataMemtableMgr has been initialized.", KR(ret));
  } else if (OB_UNLIKELY(!tablet_id.is_valid()) || OB_ISNULL(freezer) || OB_ISNULL(t3m)) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "invalid arguments", K(ret), K(tablet_id), KP(freezer), KP(t3m));
  } else if (OB_FAIL(MTL(ObLSService*)->get_ls(ls_id, ls_handle, ObLSGetMod::STORAGE_MOD))){
    STORAGE_LOG(WARN, "Get ls from ls service failed.", KR(ret));
  } else if (OB_ISNULL(tx_table = ls_handle.get_ls()->get_tx_table())) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "Get tx table from ls failed.", KR(ret));
  } else {
    reset_tables();
    ls_id_ = ls_id;
    tablet_id_ = tablet_id;
    t3m_ = t3m;
    table_type_ = ObITable::TableType::TX_DATA_MEMTABLE;
    freezer_ = freezer;
    tx_data_table_ = tx_table->get_tx_data_table();
    ls_tablet_svr_ = ls_handle.get_ls()->get_tablet_svr();
    ObLSTxService *ls_tx_svr = nullptr;
    if (OB_ISNULL(ls_tx_svr = freezer_->get_ls_tx_svr())) {
      ret = OB_ERR_UNEXPECTED;
      STORAGE_LOG(WARN, "ls_tx_svr is null", K(ret), KP(freezer_));
    } else if (OB_FAIL(ls_tx_svr->register_common_checkpoint(
                      checkpoint::TX_DATA_MEMTABLE_TYPE, this))) {
      STORAGE_LOG(WARN, "tx_data register_common_checkpoint failed", K(ret), K(ls_id));
    } else if (OB_ISNULL(tx_data_table_) || OB_ISNULL(ls_tablet_svr_)) {
      ret = OB_ERR_NULL_VALUE;
      STORAGE_LOG(WARN, "Init tx data memtable mgr failed.", KR(ret));
    } else {
      is_inited_ = true;
    }
  }

  if (IS_NOT_INIT) {
    destroy();
  }
  return ret;
}

void ObTxDataMemtableMgr::destroy()
{
  SpinWLockGuard guard(lock_);
  reset_tables();
  ls_id_ = 0;
  tablet_id_ = 0;
  tx_data_table_ = nullptr;
  ls_tablet_svr_ = nullptr;
  freezer_ = nullptr;
  is_inited_ = false;
}

int ObTxDataMemtableMgr::release_head_memtable_(memtable::ObIMemtable *imemtable,
                                                const bool force)
{
  int ret = OB_SUCCESS;
  ObTxDataMemtable *memtable = static_cast<ObTxDataMemtable *>(imemtable);
  STORAGE_LOG(INFO, "tx data memtable mgr release head memtable", K(get_memtable_count_()),
              KPC(memtable));

  if (OB_LIKELY(get_memtable_count_() > 0)) {
    const int64_t idx = get_memtable_idx(memtable_head_);
    if (nullptr != tables_[idx] && memtable == tables_[idx]) {
      memtable->set_state(ObTxDataMemtable::State::RELEASED);
      STORAGE_LOG(INFO, "tx data memtable mgr release head memtable", KPC(memtable));
      release_head_memtable();
    } else {
      ret = OB_INVALID_ARGUMENT;
      ObTxDataMemtable *head_memtable = static_cast<ObTxDataMemtable *>(tables_[get_memtable_idx(idx)]);
      STORAGE_LOG(WARN, "trying to release an invalid tx data memtable.", KR(ret), K(idx),
                  K(memtable), KP(memtable), KPC(head_memtable));
    }
  }
  return ret;
}

int ObTxDataMemtableMgr::create_memtable(const int64_t clog_checkpoint_ts,
                                         const int64_t schema_version,
                                         const bool for_replay)
{
  UNUSED(for_replay);
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    STORAGE_LOG(WARN, "ObTxDataMemtableMgr has not initialized", K(ret), K_(is_inited));
  } else if (OB_UNLIKELY(schema_version < 0)) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "invalid argument", K(ret), K(schema_version));
  } else if (OB_ISNULL(slice_allocator_)) {
    ret = OB_ERR_NULL_VALUE;
    STORAGE_LOG(WARN, "slice_allocator_ has not been set.");
  } else {
    SpinWLockGuard lock_guard(lock_);
    if (OB_FAIL(create_memtable_(clog_checkpoint_ts, schema_version))) {
      STORAGE_LOG(WARN, "create memtable fail.", KR(ret));
    } else {
      // create memtable success
    }
  }

  return ret;
}

int ObTxDataMemtableMgr::create_memtable_(const int64_t clog_checkpoint_ts,
                                          const int64_t schema_version)
{
  UNUSED(schema_version);
  int ret = OB_SUCCESS;
  ObTableHandleV2 handle;
  ObITable::TableKey table_key;
  table_key.table_type_ = ObITable::TX_DATA_MEMTABLE;
  table_key.tablet_id_ = ObTabletID(ObTabletID::LS_TX_DATA_TABLET_ID);
  table_key.log_ts_range_.start_log_ts_ = clog_checkpoint_ts;
  table_key.log_ts_range_.end_log_ts_ = INT64_MAX;
  ObITable *table = nullptr;
  ObTxDataMemtable *tx_data_memtable = nullptr;

  if (OB_FAIL(t3m_->acquire_tx_data_memtable(handle))) {
    STORAGE_LOG(WARN, "failed to create memtable", KR(ret), KP(t3m_));
  } else if (OB_ISNULL(table = handle.get_table())) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(ERROR, "table is nullptr", KR(ret), K(handle));
  } else if (FALSE_IT(tx_data_memtable = dynamic_cast<ObTxDataMemtable *>(table))) {
  } else if (OB_ISNULL(tx_data_memtable)) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(ERROR, "dynamic cast failed", KR(ret), KPC(this));
  } else if (OB_FAIL(tx_data_memtable->init(table_key, slice_allocator_, this))) {
    STORAGE_LOG(WARN, "memtable init fail.", KR(ret), KPC(tx_data_memtable));
  } else if (OB_FAIL(add_memtable_(handle))) {
    STORAGE_LOG(WARN, "add memtable fail.", KR(ret));
  } else if (OB_FAIL(tx_data_memtable->set_freezer(freezer_))) {
    STORAGE_LOG(WARN, "tx_data_memtable set freezer failed", KR(ret), KPC(tx_data_memtable));
  } else {
    // create memtable success
    STORAGE_LOG(INFO, "create tx data memtable done", KR(ret), KPC(tx_data_memtable));
  }

  return ret;
}

int ObTxDataMemtableMgr::freeze()
{
  int ret = OB_SUCCESS;
  STORAGE_LOG(INFO, "start freeze tx data memtable", K(ls_id_));

  if (IS_NOT_INIT) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "tx data memtable container is not inited.", KR(ret));
  } else if (get_memtable_count_() <= 0) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(ERROR, "there is no tx data memtable.", KR(ret), K(get_memtable_count_()));
  } else if (OB_ISNULL(slice_allocator_)) {
    ret = OB_ERR_NULL_VALUE;
    STORAGE_LOG(WARN, "slice_allocator_ has not been set.", KR(ret), KP(slice_allocator_));
  } else {
    SpinWLockGuard lock_guard(lock_);
    if (OB_FAIL(freeze_())) {
      STORAGE_LOG(WARN, "freeze tx data memtable fail.", KR(ret));
    } else {
      // freeze success
    }
  }

  return ret;
}

int ObTxDataMemtableMgr::freeze_()
{
  int ret = OB_SUCCESS;

  ObTxDataMemtable *freeze_memtable = static_cast<ObTxDataMemtable *>(tables_[get_memtable_idx(memtable_tail_ - 1)]);
  int64_t pre_memtable_tail = memtable_tail_;
  // FIXME @gengli: set clog_checkpoint_ts and schema_version
  int64_t clog_checkpoint_ts = 1;
  int64_t schema_version = 1;

  // FIXME : @gengli remove this condition after upper_trans_version is not needed
  if (get_memtable_count_() >= MAX_TX_DATA_MEMTABLE_CNT) {
    ret = OB_EAGAIN;
    STORAGE_LOG(INFO, "There is a freezed memetable existed. Try freeze after flushing it.", KR(ret), K(get_memtable_count_()));
  } else if (get_memtable_count_() >= MAX_MEMSTORE_CNT) {
    ret = OB_SIZE_OVERFLOW;
    STORAGE_LOG(WARN, "tx data memtable size is overflow.", KR(ret), K(get_memtable_count_()));
  } else if (OB_ISNULL(freeze_memtable)) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "freeze memtable is nullptr", KR(ret), KP(freeze_memtable));
  } else if (ObTxDataMemtable::State::ACTIVE != freeze_memtable->get_state()) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "trying to freeze an inactive tx data memtable.", KR(ret),
                KPC(freeze_memtable));
  } else if (0 == freeze_memtable->get_tx_data_count()) {
    ret = OB_STATE_NOT_MATCH;
    STORAGE_LOG(WARN, "tx data memtable is empty. do not need freeze.", KR(ret), KPC(freeze_memtable));
  } else if (OB_FAIL(create_memtable_(clog_checkpoint_ts, schema_version))) {
    STORAGE_LOG(WARN, "create memtable fail.", KR(ret), K(clog_checkpoint_ts), K(schema_version));
  } else {
    ObTxDataMemtable *new_memtable = static_cast<ObTxDataMemtable *>(tables_[get_memtable_idx(memtable_tail_ - 1)]);
    if (OB_ISNULL(new_memtable) && OB_UNLIKELY(new_memtable->is_tx_data_memtable())) {
      ret = OB_ERR_UNEXPECTED;
      STORAGE_LOG(WARN, "get tx data memtable from handle fail.", KR(ret), KPC(new_memtable));
    } else {
      int64_t start_ts = ObTimeUtil::fast_current_time();
      while (freeze_memtable->get_write_ref() > 0) {
        // waiting for all write operation done.
        if (TC_REACH_TIME_INTERVAL(TX_DATA_MEMTABLE_MAX_FREEZE_WAIT_TIME)) {
          int64_t freeze_wait_time_ms = (ObTimeUtil::fast_current_time() - start_ts) / 1000;
          STORAGE_LOG(WARN, "freeze tx data memtable cost too much time. has wait for(ms) : ",
                      K(freeze_wait_time_ms), KPC(freeze_memtable));
        }
        PAUSE();
      }
      freeze_memtable->set_end_log_ts();
      freeze_memtable->set_state(ObTxDataMemtable::State::FREEZING);
      new_memtable->set_start_log_ts(freeze_memtable->get_end_log_ts());
      new_memtable->set_state(ObTxDataMemtable::State::ACTIVE);
      STORAGE_LOG(INFO, "tx data memtable freeze success.", K(get_memtable_count_()),
                  KPC(freeze_memtable), KPC(new_memtable));
    }
  }

  if (OB_FAIL(ret)) {
    if (memtable_tail_ != pre_memtable_tail) {
      STORAGE_LOG(ERROR, "unexpected error happened.", KR(ret), K(pre_memtable_tail),
                  K(memtable_tail_), KPC(freeze_memtable));
      memtable_tail_ = pre_memtable_tail;
    }
  }

  return ret;
}

int ObTxDataMemtableMgr::get_active_memtable(ObTableHandleV2 &handle) const
{
  int ret = OB_SUCCESS;
  SpinRLockGuard lock_guard(lock_);
  if (0 == memtable_tail_) {
    ret = OB_EAGAIN;
    STORAGE_LOG(INFO, "tx data memtable is not created yet. try agagin.", K(ret), K(memtable_tail_));
  } else if (0 == get_memtable_count_()) {
    ret = OB_ENTRY_NOT_EXIST;
    STORAGE_LOG(WARN, "the tx data memtable manager is empty. may be offline", KR(ret), K(get_memtable_count_()));
  } else if (OB_FAIL(get_ith_memtable(memtable_tail_ - 1, handle))) {
    STORAGE_LOG(WARN, "fail to get ith memtable", K(ret), K(memtable_tail_));
  } else {
    ObTxDataMemtable *tx_data_memtable = nullptr;
    if (OB_FAIL(handle.get_tx_data_memtable(tx_data_memtable))) {
      STORAGE_LOG(ERROR, "get tx data memtable from handle failed.", KR(ret), K(handle));
    } else if (ObTxDataMemtable::State::ACTIVE != tx_data_memtable->get_state()) {
      ret = OB_ERR_UNEXPECTED;
      STORAGE_LOG(ERROR, "the last tx data memtable in manager is not an active memtable", KR(ret), KPC(tx_data_memtable));
    }
  }
  return ret;
}

int ObTxDataMemtableMgr::get_all_memtables_(ObTableHdlArray &handles)
{
  int ret = OB_SUCCESS;
  for (int64_t i = memtable_head_; OB_SUCC(ret) && i < memtable_tail_; ++i) {
    ObTableHandleV2 handle;
    if (OB_FAIL(get_ith_memtable(i, handle))) {
      STORAGE_LOG(WARN, "fail to get ith memtable", K(ret), K(i));
    } else if (OB_FAIL(handles.push_back(handle))) {
      STORAGE_LOG(WARN, "push back into handles failed.", K(ret));
    }
  }
  return ret;
}

int ObTxDataMemtableMgr::get_all_memtables(ObTableHdlArray &handles)
{
  int ret = OB_SUCCESS;
  SpinRLockGuard lock_guard(lock_);
  if (OB_FAIL(get_all_memtables_(handles))) {
    handles.reset();
    STORAGE_LOG(WARN, "get all memtables failed.", KR(ret));
  }
  return ret;
}

int ObTxDataMemtableMgr::get_all_memtables_with_range(ObTableHdlArray &handles, int64_t &memtable_head, int64_t &memtable_tail)
{
  int ret = OB_SUCCESS;
  SpinRLockGuard lock_guard(lock_);
  if (OB_FAIL(get_all_memtables_(handles))) {
    handles.reset();
    STORAGE_LOG(WARN, "get all memtables failed.", KR(ret));
  } else {
    memtable_head = memtable_head_;
    memtable_tail = memtable_tail_;
  }
  return ret;
}

int ObTxDataMemtableMgr::get_all_memtables_for_write(ObTxDataMemtableWriteGuard &write_guard)
{
  int ret = OB_SUCCESS;
  SpinRLockGuard lock_guard(lock_);
  for (int64_t i = memtable_head_; OB_SUCC(ret) && i < memtable_tail_; ++i) {
    int64_t real_idx = get_memtable_idx(i);
    write_guard.handles_[i - memtable_head_].reset();
    ObTableHandleV2 &table_handle = write_guard.handles_[i - memtable_head_];
    ObTxDataMemtable *tx_data_memtable = nullptr;
    if (OB_FAIL(get_ith_memtable(i, table_handle))) {
      STORAGE_LOG(WARN, "fail to get ith memtable", K(ret), K(i));
    } else if (OB_FAIL(table_handle.get_tx_data_memtable(tx_data_memtable))) {
      STORAGE_LOG(ERROR, "get tx data memtable from memtable handle failed", KR(ret), K(table_handle));
    } else if (OB_ISNULL(tx_data_memtable)) {
      ret = OB_ERR_UNEXPECTED;
      STORAGE_LOG(ERROR, "tx data memtable is unexpected nullptr", K(ret), KPC(tx_data_memtable));
    } else {
      write_guard.size_++;
      tx_data_memtable->inc_write_ref();
    }
  }
  return ret;
}

int64_t ObTxDataMemtableMgr::get_rec_log_ts()
{
  int ret = OB_SUCCESS;
  int64_t rec_log_ts = INT64_MAX;
  ObTxDataMemtable *oldest_memtable = nullptr;
  ObSEArray<ObTableHandleV2, 2> memtable_handles;
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(get_all_memtables(memtable_handles))) {
    STORAGE_LOG(WARN, "get all memtables failed", KR(ret), KP(this));
  } else if (memtable_handles.count() == 0) {
  } else {
    ObTableHandleV2 &oldest_memtable_handle = memtable_handles.at(0);
    if (OB_FAIL(oldest_memtable_handle.get_tx_data_memtable(oldest_memtable))) {
      STORAGE_LOG(WARN, "get tx data memtable from table handle fail.", KR(ret),
                  K(oldest_memtable_handle));
    } else {
      rec_log_ts = oldest_memtable->get_rec_log_ts();
    }
  }

  return rec_log_ts;
}

int ObTxDataMemtableMgr::flush_all_frozen_memtables_(ObTableHdlArray &memtable_handles)
{
  int ret = OB_SUCCESS;

  // flush all frozen memtable
  for (int i = 0; OB_SUCC(ret) && i < memtable_handles.count() - 1; i++) {
    ObTableHandleV2 &memtable_handle = memtable_handles.at(i);
    ObTxDataMemtable *memtable = nullptr;
    if (OB_FAIL(memtable_handle.get_tx_data_memtable(memtable))) {
      STORAGE_LOG(WARN, "get tx data memtable from table handle fail.", KR(ret), K(memtable));
    } else if (memtable->get_state() != ObTxDataMemtable::State::FROZEN
               && !memtable->ready_for_flush()) {
      // on need return error
      STORAGE_LOG(INFO, "the tx data memtable is not frozen", KPC(memtable));
    } else if (OB_FAIL(memtable->flush())) {
      STORAGE_LOG(WARN, "the tx data memtable flush failed", KR(ret), KPC(memtable));
    }
  }
  return ret;
}

int ObTxDataMemtableMgr::flush(int64_t recycle_log_ts, bool need_freeze)
{
  int ret = OB_SUCCESS;

  // do freeze if needed
  // recycle_log_ts == INT64_MAX && need_freeze == true means this flush is called by tx data table
  // self freeze task
  if (need_freeze) {
    TxDataMemtableMgrFreezeGuard freeze_guard;
    int64_t rec_log_ts = get_rec_log_ts();
    if (rec_log_ts >= recycle_log_ts) {
      STORAGE_LOG(INFO, "no need freeze", K(recycle_log_ts), K(rec_log_ts));
    } else if (OB_FAIL(freeze_guard.init(this))) {
      STORAGE_LOG(WARN, "init tx data memtable mgr freeze guard failed", KR(ret), K(recycle_log_ts),
                  K(rec_log_ts));
    } else if (!freeze_guard.can_freeze()) {
      STORAGE_LOG(INFO, "there is a freeze task is running. skip once.", K(recycle_log_ts),
                  K(rec_log_ts));
    } else if(OB_FAIL(freeze())) {
      STORAGE_LOG(WARN, "freeze failed", KR(ret), KP(this));
    }
  }

  ObSEArray<ObTableHandleV2, 2> memtable_handles;
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(get_all_memtables(memtable_handles))) {
    STORAGE_LOG(WARN, "get all memtables failed", KR(ret), KP(this));
  } else if (memtable_handles.count() == 0) {
    STORAGE_LOG(INFO, "memtable handles is empty. skip flush once.");
  } else if (OB_FAIL(flush_all_frozen_memtables_(memtable_handles))) {
    STORAGE_LOG(WARN, "flush all frozen memtables failed", KR(ret), KP(this));
  } else if (OB_NOT_NULL(tx_data_table_) && OB_FAIL(tx_data_table_->update_memtables_cache())) {
    STORAGE_LOG(WARN, "update memtables cache failed.", KR(ret), KP(this));
  }

  return ret;
}

ObTabletID ObTxDataMemtableMgr::get_tablet_id() const
{
  return LS_TX_DATA_TABLET;
}

bool ObTxDataMemtableMgr::is_flushing() const
{
  return memtable_tail_ - 1 != memtable_head_;
}

int ObTxDataMemtableMgr::get_memtable_range(int64_t &memtable_head, int64_t &memtable_tail)
{
  int ret = OB_SUCCESS;
  SpinRLockGuard lock_guard(lock_);
  memtable_head = memtable_head_;
  memtable_tail = memtable_tail_;
  return ret;
}

}  // namespace storage

}  // namespace oceanbase
