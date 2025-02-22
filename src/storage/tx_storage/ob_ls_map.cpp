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

#define USING_LOG_PREFIX STORAGE

#include "storage/tx_storage/ob_ls_map.h"

#include "lib/ob_errno.h"
#include "storage/ls/ob_ls.h"
#include "storage/tx_storage/ob_ls_handle.h"

using namespace oceanbase::share;
using namespace oceanbase::common;
using namespace oceanbase::common::hash;

namespace oceanbase
{
namespace storage
{
// ------------------- ObLSIterator -------------------- //
ObLSIterator::ObLSIterator()
  : lss_(),
    bucket_pos_(0),
    array_idx_(0),
    ls_map_(NULL),
    mod_(ObLSGetMod::INVALID_MOD)
{
}

ObLSIterator::~ObLSIterator()
{
  reset();
}

void ObLSIterator::reset()
{
  if (OB_NOT_NULL(ls_map_)) {
    for (int64_t i = 0; i < lss_.count(); ++i) {
      ls_map_->revert_ls(lss_.at(i), mod_);
    }
    lss_.reuse();
    bucket_pos_ = 0;
    array_idx_ = 0;
    mod_ = ObLSGetMod::INVALID_MOD;
  }
}

int ObLSIterator::get_next(ObLS *&ls)
{
  int ret = OB_SUCCESS;

  if (OB_ISNULL(ls_map_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("The ls service is NULL, ", K(ret));
  } else {
    while(OB_SUCC(ret)) {
      if (array_idx_ < lss_.count()) {
        ls = lss_.at(array_idx_);
        array_idx_++;
        break;
      } else {
        if (lss_.count() > 0) {
          for (int64_t i = 0; i < lss_.count(); ++i) {
            ls_map_->revert_ls(lss_.at(i), mod_);
          }
          lss_.reuse();
        }
        array_idx_ = 0;

        if (OB_ISNULL(ls_map_->ls_buckets_) || bucket_pos_ >= ls_map_->BUCKETS_CNT) {
          ret = OB_ITER_END;
        } else {
          if (OB_NOT_NULL(ls_map_->ls_buckets_[bucket_pos_])) {
            share::ObQSyncLockReadGuard guard(ls_map_->buckets_lock_[bucket_pos_]);
            ls = ls_map_->ls_buckets_[bucket_pos_];

            while (OB_NOT_NULL(ls) && OB_SUCC(ret)) {
              if (OB_FAIL(ls->get_ref_mgr().inc(mod_))) {
                LOG_WARN("ls inc ref fail", K(ret));
              } else if (OB_FAIL(lss_.push_back(ls))) {
                LOG_WARN("Fail to push ls to array, ", K(ret));
                ls->get_ref_mgr().dec(mod_);
              } else {
                ls = (ObLS *)ls->next_;
              }
            }
          }
          bucket_pos_++;
        }
      }
    }
  }
  return ret;
}

// ------------------- ObLSMap -------------------- //
void ObLSMap::reset()
{
  if (OB_NOT_NULL(ls_buckets_)) {
    ObLS *ls = nullptr;
    ObLS *next_ls = nullptr;
    for (int64_t i = 0; i < BUCKETS_CNT; ++i) {
      ls = ls_buckets_[i];
      while (OB_NOT_NULL(ls)) {
        next_ls = (ObLS *)ls->next_;
        ls->get_ref_mgr().set_delete();
        // here mod muse the same as add_ls
        revert_ls(ls, ObLSGetMod::TXSTORAGE_MOD);
        ls = next_ls;
      }
    }
    ob_free(ls_buckets_);
    ls_buckets_ = NULL;
  }
  if (OB_NOT_NULL(buckets_lock_)) {
    ob_free(buckets_lock_);
    buckets_lock_ = nullptr;
  }
  ls_cnt_ = 0;
  tenant_id_ = OB_INVALID_ID;
  ls_allocator_ = nullptr;
  is_inited_ = false;
}

int ObLSMap::init(const int64_t tenant_id, ObIAllocator *ls_allocator)
{
  int ret = OB_SUCCESS;
  const char *OB_LS_MAP = "LSMap";
  ObMemAttr mem_attr(tenant_id, OB_LS_MAP);
  void *buf = NULL;

  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("ObLSMap init twice", K(ret));
  } else if (OB_ISNULL(ls_allocator)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret));
  } else if (OB_ISNULL(buckets_lock_ = (share::ObQSyncLock*)ob_malloc(sizeof(share::ObQSyncLock) * BUCKETS_CNT, mem_attr))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_ISNULL(buf = ob_malloc(sizeof(ObLS*) * BUCKETS_CNT, mem_attr))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("Fail to allocate memory, ", K(ret), LITERAL_K(BUCKETS_CNT));
  } else {
    for (int64_t i = 0 ; i < BUCKETS_CNT; ++i) {
      new(buckets_lock_ + i) share::ObQSyncLock();
      if (OB_FAIL((buckets_lock_ + i)->init(mem_attr))) {
        LOG_WARN("buckets_lock_ init fail", K(ret), K(tenant_id));
        for (int64_t j = 0 ; j <= i; ++j) {
          (buckets_lock_ + j)->destroy();
        }
        ob_free(buf);
        buf = NULL;
        ob_free(buckets_lock_);
        buckets_lock_ = NULL;
        break;
      }
    }
    if (OB_LIKELY(ret == common::OB_SUCCESS)) {
      MEMSET(buf, 0, sizeof(ObLS*) * BUCKETS_CNT);
      ls_buckets_ = new (buf) ObLS*[BUCKETS_CNT];
      tenant_id_ = tenant_id;
      ls_allocator_ = ls_allocator;
      is_inited_ = true;
    }
  }

  return ret;
}

int ObLSMap::add_ls(
    ObLS &ls)
{
  int ret = OB_SUCCESS;
  ObLS *prev = NULL;
  ObLS *curr = NULL;
  const ObLSID &ls_id = ls.get_ls_id();
  LOG_INFO("ls map add ls",
           K(ls_id), KP(&ls), "ref", ls.get_ref_mgr().get_total_ref_cnt());

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObLSMap not init", K(ret), K(ls_id));
  } else {
    int64_t pos = ls_id.hash() % BUCKETS_CNT;
    share::ObQSyncLockWriteGuard guard(buckets_lock_[pos]);
    curr = ls_buckets_[pos];
    while (OB_NOT_NULL(curr)) {
      if (curr->get_ls_id() == ls_id) {
        break;
      } else {
        prev = curr;
        curr = static_cast<ObLS *>(curr->next_);
      }
    }

    if (OB_ISNULL(curr)) {
      int64_t cnt = ATOMIC_AAF(&ls_cnt_, 1);
      if (cnt > OB_MAX_LS_NUM_PER_TENANT_PER_SERVER) {
        ATOMIC_DEC(&ls_cnt_);
        ret = OB_TOO_MANY_TENANT_LS;
        LOG_WARN("too many lss of a tenant", K(ret), K(cnt),
                 LITERAL_K(OB_MAX_LS_NUM_PER_TENANT_PER_SERVER));
      } else if (OB_FAIL(ls.get_ref_mgr().inc(ObLSGetMod::TXSTORAGE_MOD))) {
        ATOMIC_DEC(&ls_cnt_);
        LOG_WARN("ls inc ref fail", K(ret), K(ls_id));
      } else {
        ls.next_ = ls_buckets_[pos];
        ls_buckets_[pos] = &ls;
      }
    } else {
      ret = OB_ENTRY_EXIST;
    }
  }

  LOG_INFO("ls map finish add ls",
           K(ls_id), KP(&ls), "ref", ls.get_ref_mgr().get_total_ref_cnt(), K(ret));
  return ret;
}

int ObLSMap::del_ls(const share::ObLSID &ls_id)
{
  int ret = OB_SUCCESS;
  ObLS *prev = NULL;
  ObLS *ls = NULL;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObLSMap not init", K(ret), K(ls_id));
  } else {
    int64_t pos = ls_id.hash() % BUCKETS_CNT;
    //remove ls from map
    share::ObQSyncLockWriteGuard guard(buckets_lock_[pos]);
    ls = ls_buckets_[pos];
    while (OB_NOT_NULL(ls)) {
      if (ls->get_ls_id() == ls_id) {
        break;
      } else {
        prev = ls;
        ls = (ObLS *)ls->next_;
      }
    }

    if (OB_ISNULL(ls)) {
      ret = OB_LS_NOT_EXIST;
    } else {
      LOG_INFO("ls service del ls", K(ls_id),
               KP(ls), "ref", ls->get_ref_mgr().get_total_ref_cnt());
      if (OB_ISNULL(prev)) {
        ls_buckets_[pos] = (ObLS *)ls->next_;
      } else {
        prev->next_ = ls->next_;
      }
      ls->next_ = NULL;
      del_ls_impl(ls);
    }
  }

  return ret;
}

void ObLSMap::del_ls_impl(ObLS *ls)
{
  int ret = OB_SUCCESS;
  if (nullptr != ls) {
    const ObLSID &ls_id = ls->get_ls_id();
    ATOMIC_DEC(&ls_cnt_);
    ls->get_ref_mgr().set_delete();
    // here mod must the same with add_ls
    revert_ls(ls, ObLSGetMod::TXSTORAGE_MOD);
  }
}

int ObLSMap::get_ls(const share::ObLSID &ls_id,
                    ObLSHandle &handle,
                    ObLSGetMod mod) const
{
  int ret = OB_SUCCESS;
  ObLS *ls = NULL;
  int64_t pos = 0;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObLSMap not init", K(ret), K(ls_id));
  } else {
    pos = ls_id.hash() % BUCKETS_CNT;
    share::ObQSyncLockReadGuard bucket_guard(buckets_lock_[pos]);
    ls = ls_buckets_[pos];
    while (OB_NOT_NULL(ls)) {
      if (ls->get_ls_id() == ls_id) {
        break;
      } else {
        ls = static_cast<ObLS *>(ls->next_);
      }
    }

    if (OB_ISNULL(ls)) {
      ret = OB_LS_NOT_EXIST;
    } else if (OB_FAIL(handle.set_ls(*this, *ls, mod))) {
      LOG_WARN("get_ls fail", K(ret), K(ls_id));
    }
  }
  return ret;
}

int ObLSMap::remove_duplicate_ls()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObLSMap has not been inited", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < BUCKETS_CNT; ++i) {
      share::ObQSyncLockWriteGuard bucket_guard(buckets_lock_[i]);
      if (nullptr != ls_buckets_[i]) {
        if (OB_FAIL(remove_duplicate_ls_in_linklist(ls_buckets_[i]))) {
          LOG_WARN("fail to remove same ls in linklist", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObLSMap::choose_preserve_ls(ObLS *left_ls,
                                       ObLS *right_ls,
                                       ObLS *&result_ls)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(nullptr == left_ls || nullptr == right_ls)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), KP(left_ls), KP(right_ls));
  } else {
    const ObReplicaType left_replica_type = left_ls->get_replica_type();
    const ObReplicaType right_replica_type = right_ls->get_replica_type();
    if (ObReplicaTypeCheck::is_writable_replica(left_replica_type)) {
      result_ls = left_ls;
    } else if (ObReplicaTypeCheck::is_writable_replica(right_replica_type)) {
      result_ls = right_ls;
    } else if (ObReplicaTypeCheck::is_readonly_replica(left_replica_type)) {
      result_ls = left_ls;
    } else if (ObReplicaTypeCheck::is_readonly_replica(right_replica_type)) {
      result_ls = right_ls;
    } else {
      result_ls = left_ls;
    }
  }
  return ret;
}

int ObLSMap::remove_duplicate_ls_in_linklist(ObLS *&head)
{
  int ret = OB_SUCCESS;
  common::hash::ObHashMap<ObLSID, ObLS *> effective_ls_map;
  const int64_t MAX_LS_CNT_IN_BUCKET = 10L;
  lib::ObLabel label("LS_TMP_MAP");
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObLSMap has not been inited", K(ret));
  } else if (OB_ISNULL(head)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), KP(head));
  } else if (OB_FAIL(effective_ls_map.create(MAX_LS_CNT_IN_BUCKET, label))) {
    LOG_WARN("fail to create effetive ls hash map", K(ret));
  } else {
    ObLS *curr = head;
    ObLS *next = nullptr;
    bool has_same_ls = false;
    while (OB_SUCC(ret) && curr != nullptr) {
      ObLS *ls = nullptr;
      next = static_cast<ObLS *>(curr->next_);
      bool need_set = false;
      if (OB_FAIL(effective_ls_map.get_refactored(curr->get_ls_id(), ls))) {
        if (OB_HASH_NOT_EXIST == ret) {
          if (OB_FAIL(effective_ls_map.set_refactored(curr->get_ls_id(), curr))) {
            LOG_WARN("fail to set to effective ls map", K(ret));
          }
        }
      } else {
        ObLS *choose_ls = nullptr;
        has_same_ls = true;
        if (OB_FAIL(choose_preserve_ls(curr, ls, choose_ls))) {
          LOG_WARN("fail to choose preserve ls", K(ret));
        } else {
          if (choose_ls == curr) {
            if (OB_FAIL(effective_ls_map.set_refactored(curr->get_ls_id(), choose_ls, true/*overwrite*/))) {
              LOG_WARN("fail to set to effective ls map", K(ret));
            } else {
              del_ls_impl(ls);
            }
          } else {
            del_ls_impl(curr);
          }
        }
      }
      curr = next;
    }
    if (OB_SUCC(ret) && has_same_ls) {
      ObLS *prev = nullptr;
      for (ObHashMap<ObLSID, ObLS *>::iterator iter = effective_ls_map.begin(); iter != effective_ls_map.end(); ++iter) {
        if (nullptr == prev) {
          head = iter->second;
          prev = head;
        } else {
          prev->next_ = iter->second;
          prev = iter->second;
        }
      }
      if (OB_SUCC(ret)) {
        prev->next_ = nullptr;
      }
    }
    effective_ls_map.destroy();
  }
  return ret;
}

}; // end namespace storage
}; // end namespace oceanbase
