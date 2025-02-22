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
#include "storage/ls/ob_ls_sync_tablet_seq_handler.h"
#include "storage/ls/ob_ls.h"
#include "storage/ob_sync_tablet_seq_clog.h"
#include "logservice/ob_log_base_header.h"
#include "lib/oblog/ob_log_module.h"

namespace oceanbase
{

namespace storage
{

int ObLSSyncTabletSeqHandler::init(ObLS *ls)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("ObLSSyncTabletSeqHandler init twice", K(ret));
  } else if (OB_ISNULL(ls)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret));
  } else {
    ls_ = ls;
    is_inited_ = true;
  }
  return ret;
}

void ObLSSyncTabletSeqHandler::reset()
{
  is_inited_ = false;
  ls_ = nullptr;
}

int ObLSSyncTabletSeqHandler::replay(const void *buffer,
                                     const int64_t nbytes,
                                     const palf::LSN &lsn,
                                     const int64_t ts_ns)
{
  int ret = OB_SUCCESS;
  logservice::ObLogBaseHeader base_header;
  ObSyncTabletSeqLog log;
  int64_t tmp_pos = 0;
  const char *log_buf = static_cast<const char *>(buffer);
  ObTabletHandle tablet_handle;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObLSSyncTabletSeqHandler not inited", K(ret));
  } else if (OB_FAIL(base_header.deserialize(log_buf, nbytes, tmp_pos))) {
    LOG_WARN("log base header deserialize error", K(ret));
  } else if (OB_FAIL(log.deserialize(log_buf, nbytes, tmp_pos))) {
    LOG_WARN("ObSyncTabletSeqLog deserialize error", K(ret));
  } else if (OB_FAIL(ls_->replay_get_tablet(log.get_tablet_id(),
                                            ts_ns,
                                            tablet_handle))) {
    if (OB_TABLET_NOT_EXIST == ret) {
      LOG_INFO("tablet may be deleted, skip this log", K(ret), "tablet_id", log.get_tablet_id(), K(ts_ns));
      ret = OB_SUCCESS;
    } else if (OB_EAGAIN == ret) {
      // retry replay again
    } else {
      LOG_WARN("fail to replay get tablet, retry again", K(ret), K(log), K(ts_ns));
      ret = OB_EAGAIN;
    }
  } else if (OB_FAIL(tablet_handle.get_obj()->update_tablet_autoinc_seq(log.get_autoinc_seq(),
                                                                        ts_ns))) {
    LOG_WARN("failed to update tablet auto inc seq", K(ret), K(log));
  }
  return ret;
}

void ObLSSyncTabletSeqHandler::switch_to_follower_forcedly()
{
  // TODO
}

int ObLSSyncTabletSeqHandler::switch_to_leader()
{
  int ret = OB_SUCCESS;

  //TODO

  return ret;
}

int ObLSSyncTabletSeqHandler::switch_to_follower_gracefully()
{
  int ret = OB_SUCCESS;

  //TODO

  return ret;
}

int ObLSSyncTabletSeqHandler::resume_leader()
{
  int ret = OB_SUCCESS;

  //TODO

  return ret;
}

int ObLSSyncTabletSeqHandler::flush(int64_t rec_log_ts)
{
  // TODO
  UNUSED(rec_log_ts);
  return OB_SUCCESS;
}

int64_t ObLSSyncTabletSeqHandler::get_rec_log_ts()
{
  // TODO
  return INT64_MAX;
}

}
}
