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

#ifndef OBDEV_SRC_SQL_DAS_OB_DAS_REF_H_
#define OBDEV_SRC_SQL_DAS_OB_DAS_REF_H_
#include "sql/das/ob_das_task.h"
#include "sql/das/ob_das_define.h"
#include "sql/das/ob_das_factory.h"
#include "sql/das/ob_das_def_reg.h"
#include "storage/tx/ob_trans_service.h"
namespace oceanbase
{
namespace sql
{
class ObDASScanOp;
class ObDASInsertOp;
class ObDASRef
{
public:
  explicit ObDASRef(ObEvalCtx &eval_ctx, ObExecContext &exec_ctx);
  ~ObDASRef() { reset(); }

  DASOpResultIter begin_result_iter();
  DASTaskIter begin_task_iter() { return batched_tasks_.begin(); }
  ObDASTaskFactory &get_das_factory() { return das_factory_; }
  void set_mem_attr(const common::ObMemAttr &memattr) { das_alloc_.set_attr(memattr); }
  ObExecContext &get_exec_ctx() { return exec_ctx_; }
  template <typename DASOp>
  bool has_das_op(const ObDASTabletLoc *tablet_loc, DASOp *&das_op);
  ObIDASTaskOp* find_das_task(const ObDASTabletLoc *tablet_loc, ObDASOpType op_type);
  int add_batched_task(ObIDASTaskOp *das_task) { return batched_tasks_.store_obj(das_task); }
  //创建一个DAS Task，并由das_ref持有
  template <typename DASOp>
  int prepare_das_task(const ObDASTabletLoc *tablet_loc, DASOp *&task_op);
  int create_das_task(const ObDASTabletLoc *tablet_loc,
                      ObDASOpType op_type,
                      ObIDASTaskOp *&task_op);
  bool has_task() const { return !batched_tasks_.empty(); }
  int32_t get_das_task_cnt() const { return batched_tasks_.get_size(); }
  int execute_all_task();
  int close_all_task();
  bool is_all_local_task() const;
  void set_execute_directly(bool v) { execute_directly_ = v; }
  bool is_execute_directly() const { return execute_directly_; }
  common::ObIAllocator &get_das_alloc() { return das_alloc_; }

  int pick_del_task_to_first();

  void print_all_das_task();

  void set_frozen_node();
  const ObExprFrameInfo *get_expr_frame_info() const { return expr_frame_info_; }
  void set_expr_frame_info(const ObExprFrameInfo *info) { expr_frame_info_ = info; }

  ObEvalCtx &get_eval_ctx() { return eval_ctx_; };
  void reset();
  void reuse();
  void set_lookup_iter(DASOpResultIter *lookup_iter) { wild_datum_info_.lookup_iter_ = lookup_iter; }
private:
  DISABLE_COPY_ASSIGN(ObDASRef);
private:
  typedef common::ObObjNode<ObIDASTaskOp*> DasOpNode;
  //declare das allocator
  common::ObWrapperAllocatorWithAttr das_alloc_;
  common::ObArenaAllocator *reuse_alloc_;
  union {
    common::ObArenaAllocator reuse_alloc_buf_;
  };
  /////
  ObDASTaskFactory das_factory_;
  //一个SQL Operator可以同时产生多个das task，并由DAS批量执行
  DasTaskList batched_tasks_;
  ObExecContext &exec_ctx_;
  ObEvalCtx &eval_ctx_;
  DasOpNode *frozen_op_node_; // 初始为链表的head节点，冻结一次之后为链表的最后一个节点
  const ObExprFrameInfo *expr_frame_info_;
  DASOpResultIter::WildDatumPtrInfo wild_datum_info_;
public:
  //all flags
  union {
    uint64_t flags_;
    struct {
      uint64_t execute_directly_                : 1;
      uint64_t reserved_                        : 63;
    };
  };
};

template <typename DASOp>
OB_INLINE bool ObDASRef::has_das_op(const ObDASTabletLoc *tablet_loc, DASOp *&das_op)
{
  ObDASOpType type = static_cast<ObDASOpType>(das_reg::ObDASOpTraits<DASOp>::type_);
  bool bret = false;
  ObIDASTaskOp *das_task = find_das_task(tablet_loc, type);
  if (das_task != nullptr) {
    bret = true;
    OB_ASSERT(typeid(*das_task) == typeid(DASOp));
    das_op = static_cast<DASOp*>(das_task);
  }
  return bret;
}

template <typename DASOp>
OB_INLINE int ObDASRef::prepare_das_task(const ObDASTabletLoc *tablet_loc, DASOp *&task_op)
{
  ObDASOpType type = static_cast<ObDASOpType>(das_reg::ObDASOpTraits<DASOp>::type_);
  ObIDASTaskOp *tmp_op = nullptr;
  int ret = create_das_task(tablet_loc, type, tmp_op);
  if (OB_SUCC(ret)) {
    task_op = static_cast<DASOp*>(tmp_op);
  }
  return ret;
}
}  // namespace sql
}  // namespace oceanbase
#endif /* OBDEV_SRC_SQL_DAS_OB_DAS_REF_H_ */
