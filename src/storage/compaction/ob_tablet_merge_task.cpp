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

#define USING_LOG_PREFIX STORAGE_COMPACTION
#include "ob_tablet_merge_ctx.h"
#include "ob_tablet_merge_task.h"
#include "ob_partition_merger.h"
#include "ob_partition_merge_policy.h"
#include "share/rc/ob_tenant_base.h"
#include "lib/stat/ob_session_stat.h"
#include "storage/blocksstable/ob_index_block_builder.h"
#include "storage/tablet/ob_tablet_common.h"
#include "storage/tx_storage/ob_ls_service.h"
#include "ob_tenant_compaction_progress.h"
#include "ob_compaction_diagnose.h"
#include "ob_compaction_suggestion.h"
#include "ob_partition_merge_progress.h"
#include "ob_tx_table_merge_task.h"
#include "storage/ddl/ob_ddl_merge_task.h"
#include "ob_schedule_dag_func.h"
#include "ob_tenant_tablet_scheduler.h"
#include "share/ob_get_compat_mode.h"

namespace oceanbase
{
using namespace blocksstable;
using namespace storage;
namespace compaction
{

bool is_merge_dag(ObDagType::ObDagTypeEnum dag_type)
{
  return dag_type == ObDagType::DAG_TYPE_MAJOR_MERGE
    || dag_type == ObDagType::DAG_TYPE_MINOR_MERGE
    || dag_type == ObDagType::DAG_TYPE_MINI_MERGE
    || dag_type == ObDagType::DAG_TYPE_TX_TABLE_MERGE;
}

/*
 *  ----------------------------------------------ObMergeParameter--------------------------------------------------
 */
ObMergeParameter::ObMergeParameter()
  : ls_id_(),
    tablet_id_(),
    ls_handle_(),
    tables_handle_(nullptr),
    merge_type_(INVALID_MERGE_TYPE),
    merge_level_(MACRO_BLOCK_MERGE_LEVEL),
    table_schema_(nullptr),
    merge_schema_(nullptr),
    merge_range_(),
    version_range_(),
    log_ts_range_(),
    full_read_info_(nullptr),
    is_full_merge_(false),
    is_sstable_cut_(false)
{
}

bool ObMergeParameter::is_valid() const
{
  return (ls_id_.is_valid() && tablet_id_.is_valid())
         && ls_handle_.is_valid()
         && tables_handle_ != nullptr
         && !tables_handle_->empty()
         && is_schema_valid()
         && merge_type_ > INVALID_MERGE_TYPE
         && merge_type_ < MERGE_TYPE_MAX;
}

bool ObMergeParameter::is_schema_valid() const
{
  bool bret = true;
  if (OB_ISNULL(merge_schema_)) {
    bret = false;
    STORAGE_LOG(WARN, "schema is invalid, merge schema is null");
  } else if (storage::is_multi_version_minor_merge(merge_type_) || storage::is_backfill_tx_merge(merge_type_)) {
    bret = merge_schema_->is_valid();
  } else {
    bret = NULL != table_schema_ && table_schema_->is_valid();
  }
  return bret;
}

void ObMergeParameter::reset()
{
  ls_id_.reset();
  tablet_id_.reset();
  ls_handle_.reset();
  tables_handle_ = nullptr;
  merge_type_ = INVALID_MERGE_TYPE;
  merge_level_ = MACRO_BLOCK_MERGE_LEVEL;
  table_schema_ = nullptr;
  merge_schema_ = nullptr;
  merge_range_.reset();
  version_range_.reset();
  log_ts_range_.reset();
  is_full_merge_ = false;
  is_sstable_cut_ = false;
}

int ObMergeParameter::init(compaction::ObTabletMergeCtx &merge_ctx, const int64_t idx)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(!merge_ctx.is_valid() || idx < 0 || idx >= merge_ctx.get_concurrent_cnt())) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "Invalid argument to assign merge parameter", K(merge_ctx), K(idx), K(ret));
  } else if (OB_FAIL(merge_ctx.get_merge_range(idx, merge_range_))) {
    STORAGE_LOG(WARN, "failed to get merge range from merge context", K(ret));
  } else {
    ls_id_ = merge_ctx.param_.ls_id_;
    tablet_id_ = merge_ctx.param_.tablet_id_;
    ls_handle_ = merge_ctx.ls_handle_;
    tables_handle_ = &merge_ctx.tables_handle_;
    merge_type_ = merge_ctx.param_.merge_type_;
    merge_level_ = merge_ctx.merge_level_;
    table_schema_ = merge_ctx.schema_ctx_.table_schema_;
    merge_schema_ = merge_ctx.get_merge_schema();
    version_range_ = merge_ctx.sstable_version_range_;
    if (is_major_merge()) {
      // major merge should only read data between two major freeze points
      // but there will be some minor sstables which across major freeze points
      version_range_.base_version_ = MAX(merge_ctx.read_base_version_, version_range_.base_version_);
    } else if (is_buf_minor_merge()) {
      // buf minor merge does not keep multi-version
      version_range_.multi_version_start_ = version_range_.snapshot_version_;
    } else if (is_multi_version_minor_merge()) {
      // minor compaction always need to read all the data from input table
      // rewrite version to whole version range
      version_range_.base_version_ = 0;
      version_range_.snapshot_version_ = INT64_MAX - 2;
    }
    log_ts_range_ = merge_ctx.log_ts_range_;
    is_full_merge_ = merge_ctx.is_full_merge_;
    full_read_info_ = &(merge_ctx.tablet_handle_.get_obj()->get_full_read_info());
    is_sstable_cut_ = false;
  }

  return ret;
}

/*
 *  ----------------------------------------------ObTabletMergeDagParam--------------------------------------------------
 */

ObTabletMergeDagParam::ObTabletMergeDagParam()
  :  merge_type_(INVALID_MERGE_TYPE),
     merge_version_(0),
     ls_id_(),
     tablet_id_(),
     report_(nullptr),
     for_diagnose_(false)
{
}

bool ObTabletMergeDagParam::is_valid() const
{
  return ls_id_.is_valid()
         && tablet_id_.is_valid()
         && (merge_type_ > INVALID_MERGE_TYPE && merge_type_ < MERGE_TYPE_MAX)
         && (!is_major_merge() || merge_version_ >= 0);
}

ObBasicTabletMergeDag::ObBasicTabletMergeDag(
    const ObDagType::ObDagTypeEnum type)
  : ObIDag(type),
    ObMergeDagHash(),
    is_inited_(false),
    compat_mode_(lib::Worker::CompatMode::INVALID),
    ctx_(nullptr),
    param_(),
    allocator_("MergeDag")
{
}

ObBasicTabletMergeDag::~ObBasicTabletMergeDag()
{
  int report_ret = OB_SUCCESS;
  if (OB_NOT_NULL(ctx_)) {
  //    if (ctx_->partition_guard_.get_pg_partition() != NULL) {
  //      ctx_->partition_guard_.get_pg_partition()->set_merge_status(OB_SUCCESS == get_dag_ret());
  //    }
    ctx_->~ObTabletMergeCtx();
    allocator_.free(ctx_);
    ctx_ = nullptr;
  }
}

// create ObTabletMergeCtx when Dag start running
int ObBasicTabletMergeDag::get_tablet_and_compat_mode()
{
  int ret = OB_SUCCESS;
  void *buf = nullptr;
  if (OB_NOT_NULL(ctx_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ctx is not null", K(ret), K(ctx_));
  } else if (NULL == (buf = allocator_.alloc(sizeof(ObTabletMergeCtx)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to allocator memory", K(ret), "alloc_size", sizeof(ObTabletMergeCtx));
  } else {
    ctx_ = new(buf) ObTabletMergeCtx(param_, allocator_);
    ctx_->merge_dag_ = this;
  }
  // can't get tablet_handle now! because this func is called in create dag,
  // the last compaction dag is not finished yet, tablet is in old version
  ObTabletHandle tmp_tablet_handle;
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(MTL(ObLSService *)->get_ls(ls_id_, ctx_->ls_handle_, ObLSGetMod::STORAGE_MOD))) {
    LOG_WARN("failed to get log stream", K(ret), K(ls_id_));
  } else if (OB_FAIL(ctx_->ls_handle_.get_ls()->get_tablet_svr()->get_tablet(
          tablet_id_,
          tmp_tablet_handle,
          0/*timeout_us*/))) {
    LOG_WARN("failed to get tablet", K(ret), K(ls_id_), K(tablet_id_));
  } else {
    compat_mode_ = tmp_tablet_handle.get_obj()->get_tablet_meta().compat_mode_;
  }

  int tmp_ret = OB_SUCCESS;
  if (OB_SUCC(ret)
      && typeid(*this) != typeid(ObTxTableMergeDag)
      && OB_UNLIKELY(OB_SUCCESS != (tmp_ret = ctx_->init_merge_progress(param_.merge_type_ == MAJOR_MERGE)))) {
    LOG_WARN("failed to init merge progress", K(tmp_ret), K_(param));
  }

  return ret;
}

int64_t ObBasicTabletMergeDag::to_string(char* buf, const int64_t buf_len) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObIDag::to_string(buf, buf_len))) {
    LOG_WARN("failed to call to ObIDag::string", K(ret), K(buf_len));
  } else if (OB_FAIL(param_.to_string(buf, buf_len))) {
    LOG_WARN("failed to call to ObTabletMergeDagParam::string", K(ret), K(buf_len));
  } else if (OB_FAIL(common::databuff_printf(buf, buf_len, ", compat_mode_=%d,", compat_mode_))) {
    LOG_WARN("failed to print compat mode", K(ret), K(compat_mode_));
  } else if (OB_NOT_NULL(ctx_)) {
    if (OB_FAIL(ctx_->sstable_version_range_.to_string(buf, buf_len))) {
      LOG_WARN("failed to call to version range", K(ret), K(buf_len));
    } else if (OB_FAIL(ctx_->log_ts_range_.to_string(buf, buf_len))) {
      LOG_WARN("failed to call to log ts range", K(ret), K(buf_len));
    }
  }
  return ret;
}

int ObBasicTabletMergeDag::inner_init(const ObTabletMergeDagParam &param)
{
  int ret = OB_SUCCESS;

  if (is_inited_) {
    ret = OB_INIT_TWICE;
    LOG_WARN("cannot init twice", K(ret), K(param));
  } else if (!param.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", K(ret), K(param));
  } else {
    param_ = param;
    merge_type_ = param.merge_type_;
    ls_id_ = param.ls_id_;
    tablet_id_ = param.tablet_id_;
    if (!param.for_diagnose_ && OB_FAIL(get_tablet_and_compat_mode())) {
      LOG_WARN("failed to get tablet and compat mode", K(ret));
    } else {
      is_inited_ = true;
    }
  }
  return ret;
}

bool ObBasicTabletMergeDag::operator == (const ObIDag &other) const
{
  bool is_same = true;
  if (this == &other) {
    // same
  } else if (get_type() != other.get_type()) {
    is_same = false;
  } else {
    const ObTabletMergeDag &other_merge_dag = static_cast<const ObTabletMergeDag &>(other);
    if (merge_type_ != other_merge_dag.merge_type_
        || ls_id_ != other_merge_dag.ls_id_
        || tablet_id_ != other_merge_dag.tablet_id_) {
      is_same = false;
    }
  }
  return is_same;
}

int64_t ObMergeDagHash::inner_hash() const
{
  int64_t hash_value = 0;
  ObMergeType merge_type = merge_type_;
  if (merge_type_ == MINOR_MERGE || merge_type_ == MINI_MINOR_MERGE) {
    merge_type = MINI_MINOR_MERGE;
  }
  hash_value = common::murmurhash(&merge_type, sizeof(merge_type), hash_value);
  hash_value += ls_id_.hash();
  hash_value += tablet_id_.hash();
  return hash_value;
}

int64_t ObBasicTabletMergeDag::hash() const
{
  return ObMergeDagHash::inner_hash();
}

int ObBasicTabletMergeDag::fill_comment(char *buf, const int64_t buf_len) const
{
  int ret = OB_SUCCESS;
  const char *merge_type = merge_type_to_str(merge_type_);

  if (OB_FAIL(databuff_printf(buf, buf_len, "%s dag: ls_id=%ld tablet_id=%ld",
                              merge_type, ls_id_.id(), tablet_id_.id()))) {
    LOG_WARN("failed to fill comment", K(ret), K(ctx_));
  }

  return ret;
}

int ObBasicTabletMergeDag::fill_dag_key(char *buf, const int64_t buf_len) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(databuff_printf(buf, buf_len, "ls_id=%ld tablet_id=%ld", ls_id_.id(), tablet_id_.id()))) {
    LOG_WARN("failed to fill dag key", K(ret), K(ctx_));
  }
  return ret;
}

/*
 *  ----------------------------------------------ObTabletMergeDag--------------------------------------------------
 */

ObTabletMergeDag::ObTabletMergeDag(const ObDagType::ObDagTypeEnum type)
  : ObBasicTabletMergeDag(type)
{
}

int ObTabletMergeDag::create_first_task()
{
  int ret = OB_SUCCESS;
  ObTabletMergePrepareTask *prepare_task = NULL;
  if (OB_FAIL(alloc_task(prepare_task))) {
    STORAGE_LOG(WARN, "fail to alloc task", K(ret));
  } else if (OB_FAIL(prepare_task->init())) {
    STORAGE_LOG(WARN, "failed to init prepare_task", K(ret));
  } else if (OB_FAIL(add_task(*prepare_task))) {
    STORAGE_LOG(WARN, "fail to add task", K(ret), K_(ls_id), K_(tablet_id), K_(ctx));
  }
  return ret;
}

int ObTabletMergeDag::gene_compaction_info(compaction::ObTabletCompactionProgress &input_progress)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_NOT_NULL(ctx_) && ObIDag::DAG_STATUS_NODE_RUNNING == get_dag_status()) {
    input_progress.tenant_id_ = MTL_ID();
    input_progress.merge_type_ = merge_type_;
    input_progress.merge_version_ = ctx_->param_.merge_version_;
    input_progress.status_ = get_dag_status();
    input_progress.ls_id_ = ctx_->param_.ls_id_.id();
    input_progress.tablet_id_ = ctx_->param_.tablet_id_.id();
    input_progress.dag_id_ = get_dag_id();
    input_progress.create_time_ = add_time_;
    input_progress.start_time_ = start_time_;
    input_progress.progressive_merge_round_ = ctx_->progressive_merge_round_;
    input_progress.estimated_finish_time_ = ObTimeUtility::fast_current_time() + ObCompactionProgress::EXTRA_TIME;

    int64_t tmp_ret = OB_SUCCESS;
    if (OB_NOT_NULL(ctx_->merge_progress_)
        && OB_UNLIKELY(OB_SUCCESS != (tmp_ret = ctx_->merge_progress_->get_progress_info(input_progress)))) {
      LOG_WARN("failed to get progress info", K(tmp_ret), K(ctx_));
    } else {
      LOG_INFO("success to get progress info", K(tmp_ret), K(ctx_), K(input_progress));
    }

    if (DAG_STATUS_FINISH == input_progress.status_) { // fix merge_progress
      input_progress.unfinished_data_size_ = 0;
      input_progress.estimated_finish_time_ = ObTimeUtility::fast_current_time();
    }
  } else {
    ret = OB_EAGAIN;
  }
  return ret;
}

int ObTabletMergeDag::diagnose_compaction_info(compaction::ObDiagnoseTabletCompProgress &input_progress)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (DAG_STATUS_NODE_RUNNING == get_dag_status()) { // only diagnose running dag
    input_progress.tenant_id_ = MTL_ID();
    input_progress.merge_type_ = merge_type_;
    input_progress.status_ = get_dag_status();
    input_progress.dag_id_ = get_dag_id();
    input_progress.create_time_ = add_time_;
    input_progress.start_time_ = start_time_;

    if (OB_NOT_NULL(ctx_)) { // ctx_ is not created yet
      input_progress.snapshot_version_ = ctx_->sstable_version_range_.snapshot_version_;
      input_progress.base_version_ = ctx_->sstable_version_range_.base_version_;

      if (OB_NOT_NULL(ctx_->merge_progress_)) {
        int64_t tmp_ret = OB_SUCCESS;
        if (OB_UNLIKELY(OB_SUCCESS != (tmp_ret = ctx_->merge_progress_->get_progress_info(input_progress)))) {
          LOG_WARN("failed to get progress info", K(tmp_ret), K(ctx_));
        } else if (OB_UNLIKELY(OB_SUCCESS != (tmp_ret = ctx_->merge_progress_->diagnose_progress(input_progress)))) {
          LOG_INFO("success to diagnose progress", K(tmp_ret), K(ctx_), K(input_progress));
        }
      }
    }
  }
  return ret;
}

/*
 *  ----------------------------------------------ObTabletMajorMergeDag--------------------------------------------------
 */

ObTabletMajorMergeDag::ObTabletMajorMergeDag()
  : ObTabletMergeDag(ObDagType::DAG_TYPE_MAJOR_MERGE)
{
}

ObTabletMajorMergeDag::~ObTabletMajorMergeDag()
{
  // TODO dead lock, fix later
  // if (0 == MTL(ObTenantDagScheduler*)->get_dag_count(ObDagType::DAG_TYPE_MAJOR_MERGE)) {
  //   MTL(ObTenantTabletScheduler *)->merge_all();
  // }
}

int ObTabletMajorMergeDag::init_by_param(const ObIDagInitParam *param)
{
  int ret = OB_SUCCESS;
  const ObTabletMergeDagParam *merge_param = nullptr;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
    LOG_WARN("cannot init twice", K(ret), K(param));
  } else if (OB_ISNULL(param)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("input param is null", K(ret), K(param));
  } else if (FALSE_IT(merge_param = static_cast<const ObTabletMergeDagParam *>(param))) {
  } else if (OB_UNLIKELY(!merge_param->is_major_merge())) {
    ret = OB_ERR_SYS;
    LOG_ERROR("param is invalid or is major merge param not match", K(ret), K(param));
  } else if (OB_FAIL(ObBasicTabletMergeDag::inner_init(*merge_param))) {
    LOG_WARN("failed to init ObTabletMergeDag", K(ret));
  }
  return ret;
}

/*
 *  ----------------------------------------------ObTabletMiniMergeDag--------------------------------------------------
 */

ObTabletMiniMergeDag::ObTabletMiniMergeDag()
  : ObTabletMergeDag(ObDagType::DAG_TYPE_MINI_MERGE)
{
}

ObTabletMiniMergeDag::~ObTabletMiniMergeDag()
{
}

int ObTabletMiniMergeDag::init_by_param(const share::ObIDagInitParam *param)
{
  int ret = OB_SUCCESS;
  const ObTabletMergeDagParam *merge_param = nullptr;
  if (OB_ISNULL(param)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("input param is null", K(ret), K(param));
  } else if (FALSE_IT(merge_param = static_cast<const ObTabletMergeDagParam *>(param))) {
  } else if (OB_UNLIKELY(!merge_param->is_mini_merge())) {
    ret = OB_ERR_SYS;
    LOG_ERROR("is mini merge param not match", K(ret), K(param));
  } else if (OB_FAIL(ObBasicTabletMergeDag::inner_init(*merge_param))) {
    LOG_WARN("failed to init ObTabletMergeDag", K(ret));
  }
  return ret;
}

/*
 *  ----------------------------------------------ObTabletMinorMergeDag--------------------------------------------------
 */

ObTabletMinorMergeDag::ObTabletMinorMergeDag()
  : ObTabletMergeDag(ObDagType::DAG_TYPE_MINOR_MERGE)
{
}

ObTabletMinorMergeDag::~ObTabletMinorMergeDag()
{
}

int ObTabletMinorMergeDag::init_by_param(const share::ObIDagInitParam *param)
{
  int ret = OB_SUCCESS;
  const ObTabletMergeDagParam *merge_param = nullptr;
  if (OB_ISNULL(param)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Invalid argument to init sstable minor merge dag", K(ret), K(param));
  } else if (FALSE_IT(merge_param = static_cast<const ObTabletMergeDagParam *>(param))) {
  } else if (OB_UNLIKELY(!merge_param->is_multi_version_minor_merge() && !merge_param->is_buf_minor_merge())) {
    ret = OB_ERR_SYS;
    LOG_ERROR("Unexpected merge type to init minor merge dag", K(ret), KPC(merge_param));
  } else if (OB_FAIL(ObTabletMergeDag::inner_init(*merge_param))) {
    LOG_WARN("failed to init ObTabletMergeDag", K(ret));
  }

  return ret;
}

bool ObTabletMinorMergeDag::operator == (const ObIDag &other) const
{
  bool is_same = true;
  if (this == &other) {
    // same
  } else if (get_type() != other.get_type()) {
    is_same = false;
  } else {
    const ObTabletMergeDag &other_merge_dag = static_cast<const ObTabletMergeDag &>(other);
    if (!is_mini_minor_merge(merge_type_)
        || !is_mini_minor_merge(other_merge_dag.merge_type_)
        || ls_id_ != other_merge_dag.ls_id_
        || tablet_id_ != other_merge_dag.tablet_id_) {
      is_same = false;
    }
  }
  return is_same;
}

/*
 *  ----------------------------------------------ObTabletMergePrepareTask--------------------------------------------------
 */

ObTabletMergePrepareTask::ObTabletMergePrepareTask()
  : ObITask(ObITask::TASK_TYPE_SSTABLE_MERGE_PREPARE),
    is_inited_(false),
    merge_dag_(NULL)
{
}

ObTabletMergePrepareTask::~ObTabletMergePrepareTask()
{
}

int ObTabletMergePrepareTask::init()
{
  int ret = OB_SUCCESS;

  if (is_inited_) {
    ret = OB_INIT_TWICE;
    LOG_WARN("cannot init twice", K(ret));
  } else if (OB_ISNULL(dag_)) {
    ret = OB_ERR_SYS;
    LOG_WARN("dag must not null", K(ret));
  } else if (!is_merge_dag(dag_->get_type())) {
    ret = OB_ERR_SYS;
    LOG_ERROR("dag type not match", K(ret), KPC(dag_));
  } else {
    merge_dag_ = static_cast<ObBasicTabletMergeDag *>(dag_);
    if (OB_UNLIKELY(!merge_dag_->get_param().is_valid())) {
      ret = OB_ERR_SYS;
      LOG_WARN("param_ is not valid", K(ret), K(merge_dag_->get_param()));
    } else {
      is_inited_ = true;
    }
  }

  return ret;
}

int ObTabletMergePrepareTask::process()
{
  int ret = OB_SUCCESS;
  ObTenantStatEstGuard stat_est_guard(MTL_ID());
  ObTabletMergeCtx *ctx = NULL;
  ObTaskController::get().switch_task(share::ObTaskType::DATA_MAINTAIN);
  bool skip_rest_operation = false;

  DEBUG_SYNC(MERGE_PARTITION_TASK);

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret));
  } else if (OB_ISNULL(ctx = &merge_dag_->get_ctx())) {
  } else if (OB_UNLIKELY(ctx->param_.is_major_merge()
                         && !MTL(ObTenantTabletScheduler *)->could_major_merge_start())) {
    ret = OB_CANCELED;
    LOG_INFO("Merge has been paused", K(ret), K(ctx));
  } else if (FALSE_IT(ctx->time_guard_.click(ObCompactionTimeGuard::DAG_WAIT_TO_SCHEDULE))) {
  } else if (OB_FAIL(ctx->ls_handle_.get_ls()->get_tablet(ctx->param_.tablet_id_,
                                                          ctx->tablet_handle_,
                                                          ObTabletCommon::NO_CHECK_GET_TABLET_TIMEOUT_US))) {
    LOG_WARN("failed to get tablet", K(ret), "ls_id", ctx->param_.ls_id_,
        "tablet_id", ctx->param_.tablet_id_);
  } else if (OB_FAIL(build_merge_ctx(skip_rest_operation))) {
    if (OB_NO_NEED_MERGE != ret) {
      LOG_WARN("failed to build merge ctx", K(ret), K(ctx->param_));
    }
  } else if (!skip_rest_operation && ctx->param_.is_multi_version_minor_merge()) {
    if (ctx->log_ts_range_.is_empty()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_ERROR("Unexcepted empty log ts range in minor merge", K(ret), K(ctx->log_ts_range_));
    } else {
      ctx->merge_log_ts_ = ctx->log_ts_range_.end_log_ts_;
    }
  }

  if (OB_FAIL(ret) || skip_rest_operation) {
  } else {
    if (OB_NOT_NULL(ctx->compaction_filter_)) {
      ctx->is_full_merge_ = (ctx->is_full_merge_ || ctx->compaction_filter_->is_full_merge_);
    }
    if (OB_FAIL(generate_merge_task())) {
      LOG_WARN("Failed to generate_merge_sstable_task", K(ret));
    } else {
      int tmp_ret = OB_SUCCESS;
      if (!ctx->tablet_handle_.is_valid()) {
        STORAGE_LOG(WARN, "Unexcepted invalid tablet handle", K(ret), KPC(ctx));
      } else if (OB_NOT_NULL(ctx->merge_progress_)) {
        const ObTableReadInfo &read_info = ctx->tablet_handle_.get_obj()->get_full_read_info();
        if (OB_SUCCESS != (tmp_ret = ctx->merge_progress_->init(ctx, read_info))) {
          ctx->merge_progress_->reset();
          LOG_WARN("failed to init merge progress", K(tmp_ret));
        } else {
          LOG_DEBUG("succeed to init merge progress", K(tmp_ret), KPC(ctx->merge_progress_));
        }
      }
      LOG_DEBUG("succeed to init merge ctx", "task", *this);
    }
  }
  if (OB_FAIL(ret)) {
    FLOG_WARN("sstable merge finish", K(ret), K(ctx), "task", *(static_cast<ObITask *>(this)));
  }

  return ret;
}

int ObTabletMergePrepareTask::prepare_index_tree()
{
  int ret = OB_SUCCESS;
  ObDataStoreDesc desc;
  ObTabletMergeCtx &ctx = merge_dag_->get_ctx();
  if (OB_UNLIKELY(!ctx.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid merge ctx", K(ret), K(ctx));
  } else if (OB_FAIL(desc.init(*ctx.get_merge_schema(),
                               ctx.param_.ls_id_,
                               ctx.param_.tablet_id_,
                               ctx.param_.merge_type_,
                               ctx.sstable_version_range_.snapshot_version_))) {
    LOG_WARN("failed to init index store desc", K(ret), K(ctx));
  } else {
    // TODO(zhuixin.gsy) modify index_desc.init to avoid reset col_desc_array_
    const ObMergeSchema *merge_schema = ctx.get_merge_schema();
    desc.row_column_count_ = desc.rowkey_column_count_ + 1;
    desc.col_desc_array_.reset();
    desc.need_prebuild_bloomfilter_ = false;
    if (OB_FAIL(desc.col_desc_array_.init(desc.row_column_count_))) {
      LOG_WARN("failed to reserve column desc array", K(ret));
    } else if (OB_FAIL(merge_schema->get_rowkey_column_ids(desc.col_desc_array_))) {
      LOG_WARN("failed to get rowkey column ids", K(ret));
    } else if (OB_FAIL(ObMultiVersionRowkeyHelpper::add_extra_rowkey_cols(desc.col_desc_array_))) {
      LOG_WARN("failed to get extra rowkey column ids", K(ret));
    } else {
      ObObjMeta meta;
      meta.set_varchar();
      meta.set_collation_type(CS_TYPE_BINARY);
      share::schema::ObColDesc col;
      col.col_id_ = static_cast<uint64_t>(desc.row_column_count_ + OB_APP_MIN_COLUMN_ID);
      col.col_type_ = meta;
      col.col_order_ = DESC;

      if (OB_FAIL(desc.col_desc_array_.push_back(col))) {
        LOG_WARN("failed to push back last col for index", K(ret), K(col));
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(ctx.merge_info_.prepare_index_builder(desc))) {
      LOG_WARN("failed to prepare index builder", K(ret), K(desc));
    }
  }
  return ret;
}

int ObTabletMergePrepareTask::generate_merge_task()
{
  int ret = OB_SUCCESS;
  ObTabletMergeCtx &ctx = merge_dag_->get_ctx();
  ObTabletMergeTask *merge_task = NULL;
  ObTabletMergeFinishTask *finish_task = NULL;

  // add macro merge task
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret));
  } else if (OB_FAIL(prepare_index_tree())) {
    LOG_WARN("fail to prepare sstable index tree", K(ret), K(ctx));
  } else if (OB_FAIL(merge_dag_->alloc_task(merge_task))) {
    LOG_WARN("fail to alloc task", K(ret));
  } else if (OB_ISNULL(merge_task)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("Unexpecte null macro merge task", K(ret), K(ctx));
  } else if (OB_FAIL(merge_task->init(0/*task_idx*/, ctx))) {
    LOG_WARN("fail to init macro merge task", K(ret), K(ctx));
  } else if (OB_FAIL(add_child(*merge_task))) {
    LOG_WARN("fail to add child", K(ret), K(ctx));
  } else if (OB_FAIL(merge_dag_->add_task(*merge_task))) {
    LOG_WARN("fail to add task", K(ret), K(ctx));
  }

  // add finish task
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(merge_dag_->alloc_task(finish_task))) {
    LOG_WARN("fail to alloc task", K(ret), K(ctx));
  } else if (OB_FAIL(finish_task->init())) {
    LOG_WARN("fail to init main table finish task", K(ret), K(ctx));
  } else if (OB_NOT_NULL(merge_task) && OB_FAIL(merge_task->add_child(*finish_task))) {
    LOG_WARN("fail to add child", K(ret), K(ctx));
  } else if (OB_ISNULL(merge_task) && OB_FAIL(add_child(*finish_task))) {
    LOG_WARN("fail to add child", K(ret), K(ctx));
  } else if (OB_FAIL(merge_dag_->add_task(*finish_task))) {
    LOG_WARN("fail to add task", K(ret), K(ctx));
  }
  if (OB_FAIL(ret)) {
    if (OB_NOT_NULL(merge_task)) {
      merge_dag_->remove_task(*merge_task);
      merge_task = nullptr;
    }
    if (OB_NOT_NULL(finish_task)) {
      merge_dag_->remove_task(*finish_task);
      finish_task = nullptr;
    }
  }
  return ret;
}

int ObTabletMergePrepareTask::build_merge_ctx(bool &skip_rest_operation)
{
  int ret = OB_SUCCESS;
  skip_rest_operation = false;
  ObTabletMergeCtx &ctx = merge_dag_->get_ctx();
  const common::ObTabletID &tablet_id = ctx.param_.tablet_id_;

  // only ctx.param_ is inited, fill other fields here
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("The tablet has not been initialized", K(ret), K(tablet_id));
  } else if (!ctx.param_.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(ctx));
  } else if (ctx.param_.tablet_id_ != tablet_id) {
    ret = OB_ERR_SYS;
    LOG_WARN("tablet id is not match", K(ret), K(tablet_id), K(ctx.param_));
  } else if (FALSE_IT(ctx.rebuild_seq_ = ctx.ls_handle_.get_ls()->get_rebuild_seq())) {
  } else if (ctx.param_.is_major_merge()) {
    if (!ctx.tablet_handle_.get_obj()->get_tablet_meta().ha_status_.is_data_status_complete()) {
      ret = OB_STATE_NOT_MATCH;
      LOG_WARN("ha status is not allowed major", K(ret), K(tablet_id));
    } else if (OB_FAIL(ctx.inner_init_for_major())) {
      if (OB_NO_NEED_MERGE != ret) {
        LOG_WARN("fail to inner init ctx", K(ret), K(tablet_id), K(ctx));
      }
    }
  } else if (OB_FAIL(ctx.inner_init_for_minor(skip_rest_operation))) {
    if (OB_NO_NEED_MERGE != ret) {
      LOG_WARN("fail to inner init ctx", K(ret), K(tablet_id), K(ctx));
    }
  }

  if (OB_FAIL(ret) || skip_rest_operation) {
  } else if (OB_FAIL(ctx.init_merge_info())) {
    LOG_WARN("fail to init merge info", K(ret), K(tablet_id), K(ctx));
  } else {
    FLOG_INFO("succeed to build merge ctx", K(tablet_id), K(ctx), K(skip_rest_operation));
  }

  return ret;
}

/*
 *  ----------------------------------------------ObTabletMergeFinishTask--------------------------------------------------
 */

ObTabletMergeFinishTask::ObTabletMergeFinishTask()
  : ObITask(ObITask::TASK_TYPE_SSTABLE_MERGE_FINISH),
    is_inited_(false),
    merge_dag_(NULL)
{
}

ObTabletMergeFinishTask::~ObTabletMergeFinishTask()
{
}

int ObTabletMergeFinishTask::init()
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
    LOG_WARN("cannot init twice", K(ret));
  } else if (OB_ISNULL(dag_)) {
    ret = OB_ERR_SYS;
    LOG_WARN("dag must not null", K(ret));
  } else if (!is_merge_dag(dag_->get_type())) {
    ret = OB_ERR_SYS;
    LOG_ERROR("dag type not match", K(ret), KPC(dag_));
  } else {
    merge_dag_ = static_cast<ObTabletMergeDag *>(dag_);
    if (OB_UNLIKELY(!merge_dag_->get_ctx().is_valid())) {
      ret = OB_ERR_SYS;
      LOG_WARN("ctx not valid", K(ret), K(merge_dag_->get_ctx()));
    } else {
      is_inited_ = true;
    }
  }

  return ret;
}

int ObTabletMergeFinishTask::create_sstable_after_merge(ObSSTable *&sstable)
{
  int ret = OB_SUCCESS;
  ObTabletMergeCtx &ctx = merge_dag_->get_ctx();
  if (ctx.merged_table_handle_.is_valid()) {
    if (OB_UNLIKELY(!ctx.param_.is_major_merge())) {
      ret = OB_ERR_SYS;
      LOG_ERROR("Unxpected valid merged table handle with other merge", K(ret), K(ctx));
    } else if (OB_FAIL(ctx.merged_table_handle_.get_sstable(sstable))) {
      LOG_WARN("failed to get sstable", K(ret), KP(sstable));
    } else if (OB_ISNULL(sstable)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("sstable should not be NULL", K(ret), KP(sstable));
    }
  } else if (OB_FAIL(get_merged_sstable(ctx, sstable))) {
    LOG_WARN("failed to finish_merge_sstable", K(ret));
  }
  return ret;
}

int ObTabletMergeFinishTask::process()
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  ObSSTable *sstable = NULL;
  ObTaskController::get().switch_task(share::ObTaskType::DATA_MAINTAIN);

  DEBUG_SYNC(MERGE_PARTITION_FINISH_TASK);

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited yet", K(ret));
  } else {
    ObTabletMergeCtx &ctx = merge_dag_->get_ctx();
    ObTabletID &tablet_id = ctx.param_.tablet_id_;

    ctx.time_guard_.click(ObCompactionTimeGuard::EXECUTE);
    if (OB_FAIL(create_sstable_after_merge(sstable))) {
      LOG_WARN("failed to create sstable after merge", K(ret), K(tablet_id));
    } else if (FALSE_IT(ctx.time_guard_.click(ObCompactionTimeGuard::CREATE_SSTABLE))) {
    } else if (OB_FAIL(add_sstable_for_merge(ctx))) {
      LOG_WARN("failed to add sstable for merge", K(ret));
    }
    if (OB_SUCC(ret) && ctx.param_.is_major_merge() && NULL != ctx.param_.report_) {
      int tmp_ret = OB_SUCCESS;
      if (OB_TMP_FAIL(ctx.param_.report_->submit_tablet_checksums_task(MTL_ID(), ctx.param_.ls_id_, tablet_id))) {
        LOG_WARN("failed to submit tablet checksums task to report", K(tmp_ret), K(MTL_ID()), K(ctx.param_.ls_id_), K(tablet_id));
      } else if (OB_TMP_FAIL(ctx.param_.report_->submit_tablet_update_task(MTL_ID(), ctx.param_.ls_id_, tablet_id))) {
        LOG_WARN("failed to submit tablet update task to report", K(tmp_ret), K(MTL_ID()), K(ctx.param_.ls_id_), K(tablet_id));
      } else if (OB_TMP_FAIL(ctx.ls_handle_.get_ls()->get_tablet_svr()->update_tablet_report_status(tablet_id))) {
        LOG_WARN("failed to update tablet report status", K(tmp_ret), K(MTL_ID()), K(tablet_id));
      }
    }

    if (OB_SUCC(ret) && OB_NOT_NULL(ctx.merge_progress_)) {
      int tmp_ret = OB_SUCCESS;
      if (OB_TMP_FAIL(compaction::ObCompactionSuggestionMgr::get_instance().analyze_merge_info(
              ctx.merge_info_,
              *ctx.merge_progress_))) {
        STORAGE_LOG(WARN, "fail to analyze merge info", K(tmp_ret));
      }

      if (OB_TMP_FAIL(ctx.merge_progress_->finish_merge_progress(
          sstable->get_meta().get_basic_meta().get_total_macro_block_count()))) {
        STORAGE_LOG(WARN, "fail to update final merge progress", K(tmp_ret));
      }
    }
  }


  if (NULL != merge_dag_) {
    if (OB_FAIL(ret)) {
      ObTabletMergeCtx &ctx = merge_dag_->get_ctx();
      FLOG_WARN("sstable merge finish", K(ret), K(ctx), "task", *(static_cast<ObITask *>(this)));
    } else {
      merge_dag_->get_ctx().time_guard_.click(ObCompactionTimeGuard::DAG_FINISH);
      (void)merge_dag_->get_ctx().collect_running_info();
      // ATTENTION! Critical diagnostic log, DO NOT CHANGE!!!
      FLOG_INFO("sstable merge finish", K(ret), "merge_info", merge_dag_->get_ctx().get_merge_info(),
          KPC(sstable), "compat_mode", merge_dag_->get_compat_mode(), K(merge_dag_->get_ctx().time_guard_));
    }
  }

  return ret;
}

int ObTabletMergeFinishTask::get_merged_sstable(ObTabletMergeCtx &ctx, ObSSTable *&sstable)
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(!ctx.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected invalid argument to get merged sstable", K(ret), K(ctx));
  } else {
    LOG_INFO("create new merged sstable", K(ctx.param_.tablet_id_),
             "snapshot_version", ctx.sstable_version_range_.snapshot_version_,
             K(ctx.param_.merge_type_), K(ctx.create_snapshot_version_),
             "table_mode_flag", ctx.get_merge_schema()->get_table_mode_flag());

    if (OB_FAIL(ctx.merge_info_.create_sstable(ctx))) {
      LOG_WARN("fail to create sstable", K(ret), K(ctx));
    } else if (OB_FAIL(ctx.merged_table_handle_.get_sstable(sstable))) {
      LOG_WARN("failed to get sstable after merge", K(ret));
    }
  }
  return ret;
}

int ObTabletMergeFinishTask::add_sstable_for_merge(ObTabletMergeCtx &ctx)
{
  int ret = OB_SUCCESS;
  const ObStorageSchema *update_storage_schema = ctx.schema_ctx_.storage_schema_;
  ObStorageSchema tmp_storage_schema;
  if (OB_UNLIKELY(!ctx.is_valid())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected error of merge ctx", K(ctx));
  } else if (ctx.param_.is_major_merge()
      && ctx.get_merge_schema()->get_schema_version() > ctx.schema_ctx_.storage_schema_->get_schema_version()) {
    if (OB_FAIL(tmp_storage_schema.init(
        ctx.allocator_,
        *static_cast<const ObTableSchema *>(ctx.get_merge_schema()),
        ctx.schema_ctx_.storage_schema_->get_compat_mode()))) {
      LOG_WARN("failed to init storage schema", K(ret), KPC(ctx.get_merge_schema()));
    } else {
      update_storage_schema = &tmp_storage_schema;
    }
  }

  if (OB_SUCC(ret)) {
    int64_t clog_checkpoint_ts = ctx.param_.is_mini_merge() ? ctx.merged_table_handle_.get_table()->get_end_log_ts() : 0;
    ObUpdateTableStoreParam param(ctx.merged_table_handle_,
                                  ctx.sstable_version_range_.snapshot_version_,
                                  ctx.sstable_version_range_.multi_version_start_,
                                  update_storage_schema,
                                  ctx.rebuild_seq_,
                                  ctx.param_.is_major_merge(),
                                  clog_checkpoint_ts,
                                  ctx.param_.is_mini_minor_merge());
    ObTablet *old_tablet = ctx.tablet_handle_.get_obj();
    ObTabletHandle new_tablet_handle;
    if (ctx.param_.tablet_id_.is_special_merge_tablet()) {
      param.multi_version_start_ = 1;
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ctx.ls_handle_.get_ls()->update_tablet_table_store(
        ctx.param_.tablet_id_, param, new_tablet_handle))) {
      LOG_WARN("failed to update tablet table store", K(ret), K(param));
    } else if (FALSE_IT(ctx.time_guard_.click(ObCompactionTimeGuard::UPDATE_TABLET))) {
    } else if (ctx.param_.is_mini_merge()) {
      if (OB_FAIL(new_tablet_handle.get_obj()->release_memtables(ctx.log_ts_range_.end_log_ts_))) {
        LOG_WARN("failed to release memtable", K(ret), "end_log_ts", ctx.log_ts_range_.end_log_ts_);
      } else {
        ctx.time_guard_.click(ObCompactionTimeGuard::RELEASE_MEMTABLE);
      }
    }
    // try schedule minor or major merge after mini
    if (OB_SUCC(ret) && ctx.param_.is_mini_merge() && new_tablet_handle.is_valid()) {
      int tmp_ret = OB_SUCCESS;
      if (!ctx.param_.tablet_id_.is_special_merge_tablet()) {
        if (OB_TMP_FAIL(try_schedule_compaction_after_mini(ctx, new_tablet_handle))) {
          LOG_WARN("failed to schedule compaction after mini", K(tmp_ret),
              "ls_id", ctx.param_.ls_id_, "tablet_id", ctx.param_.tablet_id_);
        }
      } else if (OB_TMP_FAIL(ObTenantTabletScheduler::schedule_tx_table_merge(
              ctx.param_.ls_id_,
              *new_tablet_handle.get_obj()))) {
        if (OB_SIZE_OVERFLOW != tmp_ret) {
          LOG_WARN("failed to schedule special tablet minor merge", K(tmp_ret),
              "ls_id", ctx.param_.ls_id_, "tablet_id", ctx.param_.tablet_id_);
        }
      }
      ctx.time_guard_.click(ObCompactionTimeGuard::SCHEDULE_OTHER_COMPACTION);
    }
  }
  return ret;
}

int ObTabletMergeFinishTask::try_schedule_compaction_after_mini(
    ObTabletMergeCtx &ctx,
    ObTabletHandle &tablet_handle)
{
  int ret = OB_SUCCESS;
  const ObTabletID &tablet_id = ctx.param_.tablet_id_;
  ObLSID ls_id = ctx.param_.ls_id_;
  // schedule minor merge
  if (OB_FAIL(ObTenantTabletScheduler::schedule_tablet_minor_merge(ls_id, *tablet_handle.get_obj()))) {
    if (OB_SIZE_OVERFLOW != ret) {
      LOG_WARN("failed to schedule minor merge", K(ret), K(ls_id), K(tablet_id));
    }
  }
  // schedule major merge
  int64_t schedule_version = MTL(ObTenantTabletScheduler*)->get_frozen_version();
  if (ctx.schedule_major_ && MTL(ObTenantTabletScheduler*)->could_major_merge_start()) {
    bool unused_tablet_merge_finish = false;
    ObTenantTabletScheduler::ObScheduleStatistics unused_schedule_stats;
    // fix issue 44407360: disable tablet force freeze in this call.
    if (OB_FAIL(ObTenantTabletScheduler::schedule_tablet_major_merge(
                schedule_version,
                *ctx.ls_handle_.get_ls(),
                *tablet_handle.get_obj(),
                unused_tablet_merge_finish,
                unused_schedule_stats,
                false /*enable_force_freeze*/))) {
      if (OB_SIZE_OVERFLOW != ret) {
        LOG_WARN("failed to schedule tablet major merge", K(ret), K(schedule_version), K(ls_id), K(tablet_id));
      }
    }
  }

  return ret;
}

/*
 *  ----------------------------------------------ObTabletMergeTask--------------------------------------------------
 */

ObTabletMergeTask::ObTabletMergeTask()
  : ObITask(ObITask::TASK_TYPE_MACROMERGE),
    allocator_(),
    idx_(0),
    ctx_(nullptr),
    merger_(nullptr),
    is_inited_(false)
{
}

ObTabletMergeTask::~ObTabletMergeTask()
{
  if (OB_NOT_NULL(merger_)) {
    merger_->~ObPartitionMerger();
    merger_ = nullptr;
  }
}

int ObTabletMergeTask::init(const int64_t idx, ObTabletMergeCtx &ctx)
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", K(ret));
  } else if (idx < 0 || !ctx.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("argument is invalid", K(ret), K(idx), K(ctx));
  } else {
    void *buf = nullptr;
    if (ctx.param_.is_major_merge() || ctx.param_.is_buf_minor_merge()) {
      if (OB_ISNULL(buf = allocator_.alloc(sizeof(ObPartitionMajorMerger)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        STORAGE_LOG(WARN, "failed to alloc memory for major merger", K(ret));
      } else {
        merger_ = new (buf) ObPartitionMajorMerger();
      }
    } else {
      if (OB_ISNULL(buf = allocator_.alloc(sizeof(ObPartitionMinorMerger)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        STORAGE_LOG(WARN, "failed to alloc memory for minor merger", K(ret));
      } else {
        merger_ = new (buf) ObPartitionMinorMerger();
      }
    }
    if (OB_SUCC(ret)) {
      idx_ = idx;
      ctx_ = &ctx;
      is_inited_ = true;
    }
  }
  return ret;
}

int ObTabletMergeTask::generate_next_task(ObITask *&next_task)
{
  int ret = OB_SUCCESS;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (idx_ + 1 == ctx_->get_concurrent_cnt()) {
    ret = OB_ITER_END;
  } else if (!is_merge_dag(dag_->get_type())) {
    ret = OB_ERR_SYS;
    LOG_ERROR("dag type not match", K(ret), KPC(dag_));
  }  else {
    ObTabletMergeTask *merge_task = NULL;
    ObTabletMergeDag *merge_dag = static_cast<ObTabletMergeDag *>(dag_);

    if (OB_FAIL(merge_dag->alloc_task(merge_task))) {
      LOG_WARN("fail to alloc task", K(ret));
    } else if (OB_FAIL(merge_task->init(idx_ + 1, *ctx_))) {
      LOG_WARN("fail to init task", K(ret));
    } else {
      next_task = merge_task;
    }
  }
  return ret;
}

int ObTabletMergeTask::process()
{
  int ret = OB_SUCCESS;
  ObTenantStatEstGuard stat_est_guard(MTL_ID());
  ObTaskController::get().switch_task(share::ObTaskType::DATA_MAINTAIN);

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    STORAGE_LOG(WARN, "ObTabletMergeTask is not inited", K(ret));
  } else if (OB_ISNULL(ctx_)) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "Unexpected null merge ctx", K(ret));
  } else if (OB_UNLIKELY(ctx_->param_.is_major_merge()
                         && !MTL(ObTenantTabletScheduler *)->could_major_merge_start())) {
    ret = OB_CANCELED;
    LOG_INFO("Merge has been paused", K(ret));
  } else if (OB_ISNULL(merger_)) {
    ret = OB_ERR_SYS;
    STORAGE_LOG(WARN, "Unexpected null partition merger", K(ret));
  } else {
    if (OB_FAIL(merger_->merge_partition(*ctx_, idx_))) {
      STORAGE_LOG(WARN, "failed to merge partition", K(ret));
    } else {
      FLOG_INFO("merge macro blocks ok", K(idx_), "task", *this);
    }
    merger_->reset();
  }

  if (OB_FAIL(ret)) {
    if (NULL != ctx_) {
      if (OB_CANCELED == ret) {
        STORAGE_LOG(INFO, "merge is canceled", K(ret), K(ctx_->param_), K(idx_));
      } else {
        STORAGE_LOG(WARN, "failed to merge", K(ret), K(ctx_->param_), K(idx_));
      }
    }
  }

  return ret;
}

} // namespace compaction
} // namespace oceanbase
