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

#ifndef OBDEV_SRC_SQL_DAS_OB_DATA_ACCESS_SERVICE_H_
#define OBDEV_SRC_SQL_DAS_OB_DATA_ACCESS_SERVICE_H_
#include "share/ob_define.h"
#include "sql/das/ob_das_rpc_proxy.h"
#include "sql/das/ob_das_id_cache.h"
#include "sql/das/ob_das_task_result.h"
namespace oceanbase
{
namespace sql
{
class ObDASRef;
class ObDASTaskArg;
class ObDASTaskResp;
class ObPhyTableLocation;
class ObDASExtraData;
class ObDataAccessService
{
public:
  ObDataAccessService()
    : das_rpc_proxy_(),
      ctrl_addr_()
  { }
  ~ObDataAccessService() = default;
  static int mtl_init(ObDataAccessService* &das);
  static void mtl_destroy(ObDataAccessService* &das);
  int init(rpc::frame::ObReqTransport *transport,
           const common::ObAddr &self_addr);
  //开启DAS Task分区相关的事务控制，并执行task对应的op
  int execute_das_task(ObDASRef &das_ref, ObIDASTaskOp &task_op);
  //关闭DAS Task的执行流程，并释放task持有的资源，并结束相关的事务控制
  int end_das_task(ObDASRef &das_ref, ObIDASTaskOp &task_op);
  int get_das_task_id(int64_t &das_id);
  int rescan_das_task(ObDASRef &das_ref, ObDASScanOp &scan_op);
  obrpc::ObDASRpcProxy &get_rpc_proxy() { return das_rpc_proxy_; }
  ObDASTaskResultMgr &get_task_res_mgr() { return task_result_mgr_; }
  static ObDataAccessService &get_instance();
private:
  int execute_dist_das_task(ObDASRef &das_ref, ObIDASTaskOp &task_op);
  int clear_task_exec_env(ObDASRef &das_ref, ObIDASTaskOp &task_op);
  int refresh_partition_location(ObDASRef &das_ref, ObIDASTaskOp &task_op);
  int retry_das_task(ObDASRef &das_ref, ObIDASTaskOp &task_op);
  int do_local_das_task(ObDASRef &das_ref, ObDASTaskArg &task_arg);
  int do_remote_das_task(ObDASRef &das_ref, ObDASTaskArg &das_task);
  int setup_extra_result(ObDASRef &das_ref,
                         ObDASTaskResp &task_resp,
                         ObIDASTaskOp *task_op,
                         ObDASExtraData *&extra_result);
  int collect_das_task_info(ObDASTaskArg &task_arg, ObDASRemoteInfo &remote_info);
private:
  obrpc::ObDASRpcProxy das_rpc_proxy_;
  common::ObAddr ctrl_addr_;
  ObDASIDCache id_cache_;
  ObDASTaskResultMgr task_result_mgr_;
};
}  // namespace sql
}  // namespace oceanbase

#endif /* OBDEV_SRC_SQL_DAS_OB_DATA_ACCESS_SERVICE_H_ */
