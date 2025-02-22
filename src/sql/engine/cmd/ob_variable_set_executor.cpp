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

#define USING_LOG_PREFIX  SQL_ENG

#include "lib/string/ob_sql_string.h"
#include "lib/mysqlclient/ob_mysql_proxy.h"
#include "common/sql_mode/ob_sql_mode_utils.h"
#include "observer/ob_server_struct.h"
#include "observer/ob_sql_client_decorator.h"
#include "share/ob_i_sql_expression.h"
#include "share/ob_schema_status_proxy.h"
#include "share/ob_common_rpc_proxy.h"
#include "share/object/ob_obj_cast.h"
#include "share/inner_table/ob_inner_table_schema.h"
#include "share/schema/ob_schema_utils.h"
#include "sql/engine/cmd/ob_variable_set_executor.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/ob_physical_plan.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/code_generator/ob_expr_generator_impl.h"
#include "sql/code_generator/ob_column_index_provider.h"
#include "sql/ob_sql_trans_control.h"
#include "sql/ob_end_trans_callback.h"
#include "sql/ob_select_stmt_printer.h"
#include "lib/timezone/ob_oracle_format_models.h"
#include "observer/ob_server.h"

using namespace oceanbase::common;
using namespace oceanbase::share;
using namespace oceanbase::share::schema;
namespace oceanbase
{
namespace sql
{

#define DEFINE_CAST_CTX()              \
  ObCollationType cast_coll_type = CS_TYPE_INVALID;                        \
  if (NULL != ctx.get_my_session()) {                                    \
    if (common::OB_SUCCESS != ctx.get_my_session()->                     \
        get_collation_connection(cast_coll_type)) {                        \
      LOG_WARN("fail to get collation_connection");                        \
      cast_coll_type = ObCharset::get_default_collation(ObCharset::get_default_charset());\
    } else {}                                                              \
  } else {                                                                 \
    LOG_WARN("session is null");                                      \
    cast_coll_type = ObCharset::get_system_collation();                    \
  }                                                                        \
  const ObDataTypeCastParams dtc_params                                    \
            = ObBasicSessionInfo::create_dtc_params(ctx.get_my_session()); \
  ObCastCtx cast_ctx(&calc_buf,                                 \
                     &dtc_params,                               \
                     get_cur_time(ctx.get_physical_plan_ctx()),               \
                     CM_NONE,                  \
                     cast_coll_type,                                       \
                     (NULL));

ObVariableSetExecutor::ObVariableSetExecutor()
{
}

ObVariableSetExecutor::~ObVariableSetExecutor()
{
}

int ObVariableSetExecutor::execute(ObExecContext &ctx, ObVariableSetStmt &stmt)
{
  int ret = OB_SUCCESS;
  int ret_ac = OB_SUCCESS;
  ObSQLSessionInfo *session = NULL;
  ObMySQLProxy *sql_proxy = NULL;
  ObPhysicalPlanCtx *plan_ctx = NULL;
  if (OB_ISNULL(session = ctx.get_my_session()) ||
      OB_ISNULL(sql_proxy = ctx.get_sql_proxy()) ||
      OB_ISNULL(plan_ctx = ctx.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("session or sql proxy or physical plan ctx is NULL", K(ret),
             K(session), K(sql_proxy), K(plan_ctx));
  } else {
    HEAP_VAR(ObPhysicalPlan, phy_plan) {
      ObPhysicalPlanCtx phy_plan_ctx(ctx.get_allocator());
      ObExprCtx expr_ctx;
      phy_plan_ctx.set_phy_plan(&phy_plan);
      phy_plan_ctx.set_last_insert_id_session(session->get_local_last_insert_id());
      const int64_t cur_time = plan_ctx->has_cur_time() ?
          plan_ctx->get_cur_time().get_timestamp() : ObTimeUtility::current_time();
      phy_plan_ctx.set_cur_time(cur_time, *session);

      expr_ctx.phy_plan_ctx_ = &phy_plan_ctx;
      expr_ctx.my_session_ = session;
      expr_ctx.exec_ctx_ = &ctx;
      expr_ctx.calc_buf_ = &ctx.get_allocator();
      if (OB_ISNULL(expr_ctx.exec_ctx_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_ERROR("expr_ctx.exec_ctx_ is NULL", K(ret));
      } else {
        expr_ctx.exec_ctx_->set_sql_proxy(sql_proxy);
      }
      ObVariableSetStmt::VariableSetNode tmp_node;//just for init node
      for (int64_t i = 0; OB_SUCC(ret) && i < stmt.get_variables_size(); ++i) {
        ObVariableSetStmt::VariableSetNode &node = tmp_node;
        if (OB_FAIL(stmt.get_variable_node(i, node))) {
          LOG_WARN("fail to get variable node", K(i), K(ret));
        } else {
          ObObj value_obj;
          ObBasicSysVar *sys_var = NULL;
          if (true == node.is_set_default_) {
            if (false == node.is_system_variable_) {
              ret = OB_ERR_UNEXPECTED;
              LOG_ERROR("when reach here, node.is_system_variable_ must be true", K(ret));
            } else {}
          } else if (node.value_expr_->has_flag(CNT_SUB_QUERY)) {
            if (OB_FAIL(calc_subquery_expr_value(ctx, session, node.value_expr_, value_obj))) {
              LOG_WARN("failed to calc subquery result", K(ret));
            }
          } else {
            if (OB_FAIL(calc_var_value_static_engine(node, stmt, ctx, value_obj))) {
              LOG_WARN("calc var value in static engine failed", K(ret));
            }
          }
          if (OB_FAIL(ret)) {
          } else if (false == node.is_system_variable_) {
            if (OB_FAIL(set_user_variable(value_obj, node.variable_name_, expr_ctx))) {
              LOG_WARN("set user variable failed", K(ret));
            }
          } else {
            ObSetVar set_var(node.variable_name_, node.set_scope_, node.is_set_default_,
                             stmt.get_actual_tenant_id(), *expr_ctx.calc_buf_, *sql_proxy);
            ObObj out_obj;
            const bool is_set_stmt = true;
            if (OB_FAIL(session->get_sys_variable_by_name(node.variable_name_, sys_var))) {
              if (OB_ERR_SYS_VARIABLE_UNKNOWN == ret) {
                // session中没有找到这个名字的系统变量，有可能是proxy同步的时候发过来的新版本的数据，
                // 因此先去系统表__all_sys_variable中查一下
                ret = OB_SUCCESS;
                const uint64_t tenant_id = session->get_effective_tenant_id();
                const uint64_t exec_tenant_id = ObSchemaUtils::get_exec_tenant_id(tenant_id);
                ObSQLClientRetryWeak sql_client_retry_weak(sql_proxy,
                                                           exec_tenant_id,
                                                           OB_ALL_SYS_VARIABLE_TID);
                ObObj tmp_val;
                ObSqlString sql;
                SMART_VAR(ObMySQLProxy::MySQLResult, res) {
                  sqlclient::ObMySQLResult *result = NULL;
                  if (OB_FAIL(sql.assign_fmt("select 1 from %s where tenant_id=%lu and name='%.*s';",
                      OB_ALL_SYS_VARIABLE_TNAME, ObSchemaUtils::get_extract_tenant_id(exec_tenant_id, tenant_id),
                      node.variable_name_.length(), node.variable_name_.ptr()))) {
                    LOG_WARN("assign sql string failed", K(ret));
                  } else if (OB_FAIL(sql_client_retry_weak.read(res, exec_tenant_id, sql.ptr()))) {
                    LOG_WARN("execute sql failed", K(sql), K(ret));
                  } else if (OB_ISNULL(result = res.get_result())) {
                    ret = OB_ERR_UNEXPECTED;
                    LOG_WARN("fail to get sql result", K(ret));
                  } else if (OB_FAIL(result->next())) {
                    if (OB_ITER_END == ret) {
                      //内部表中没有发现该系统变量，说明这不是一个系统变量
                      ret = OB_ERR_SYS_VARIABLE_UNKNOWN;
                      LOG_USER_ERROR(OB_ERR_SYS_VARIABLE_UNKNOWN, node.variable_name_.length(), node.variable_name_.ptr());
                    } else {
                      LOG_WARN("get result failed", K(ret));
                    }
                  } else {
                    // 从系统表__all_sys_variable中查到了，说明是由于版本兼容导致的，
                    // 返回值设为OB_SYS_VARS_MAYBE_DIFF_VERSION，后面将其设为OB_SUCCESS
                    ret = OB_SYS_VARS_MAYBE_DIFF_VERSION;
                    LOG_INFO("try to set sys var from new version, ignore it", K(ret), K(node.variable_name_));
                  }
                }
              } else {
                LOG_WARN("fail to get system variable", K(ret), K(node.variable_name_));
              }
            } else if (OB_ISNULL(sys_var)) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("sys_var is NULL", K(ret), K(node.variable_name_));
            } else if (!lib::is_mysql_mode() && sys_var->is_mysql_only()) {
              //ignore set mysql only variables in oracle mode
            } else if (!lib::is_oracle_mode() && sys_var->is_oracle_only()) {
              //ignore set oracle only variables in mysql mode
            } else {
              if (OB_FAIL(check_and_convert_sys_var(
                          ctx, set_var, *sys_var, value_obj, out_obj, is_set_stmt))) {
                LOG_WARN("fail to check", K(ret), K(node), K(*sys_var), K(value_obj));
              } else if (FALSE_IT(value_obj = out_obj)) {
              } else if (OB_FAIL(cast_value(ctx, node, stmt.get_actual_tenant_id(),
                                            *expr_ctx.calc_buf_, *sys_var, value_obj, out_obj))) {
                LOG_WARN("fail to cast value", K(ret), K(node), K(*sys_var), K(value_obj));
              } else if (FALSE_IT(value_obj = out_obj)) {
              } else if (node.variable_name_ == OB_SV_AUTO_INCREMENT_INCREMENT
                         || node.variable_name_ == OB_SV_AUTO_INCREMENT_OFFSET) {
                if (OB_FAIL(process_auto_increment_hook(session->get_sql_mode(), //FIXME 参考mysql源码移到ObBasicSysVar的函数中
                                                        node.variable_name_,
                                                        value_obj))) {
                  LOG_WARN("fail to process auto increment hook", K(ret));
                } else {}
              } else if (node.variable_name_ == OB_SV_LAST_INSERT_ID) {
                if (OB_FAIL(process_last_insert_id_hook(plan_ctx,
                                                        session->get_sql_mode(), //FIXME 参考mysql源码移到ObBasicSysVar的函数中
                                                        node.variable_name_,
                                                        value_obj))) {
                  LOG_WARN("fail to process auto increment hook", K(ret));
                } else {}
              }

              if (OB_FAIL(ret)) {
              } else if (ObSetVar::SET_SCOPE_SESSION == node.set_scope_) {
                // 处理autocommit的特殊情况,必须先于update_sys_variable调用
                // 因为update_sys_variable会改变ac的值
                if (node.variable_name_ == OB_SV_AUTOCOMMIT) {
                  //FIXME 参考mysql源码移到ObBasicSysVar的函数中
                  if (OB_UNLIKELY(OB_SUCCESS != (ret_ac = process_session_autocommit_hook(
                                  ctx, value_obj)))) {
                    LOG_WARN("fail to process session autocommit", K(ret), K(ret_ac));
                    if (OB_ERR_WRONG_VALUE_FOR_VAR == ret_ac) {
                      ret = ret_ac;
                    }
                  } else {}
                } else {}
              }

              if (OB_FAIL(ret)) {
              } else if (set_var.var_name_ == OB_SV_READ_ONLY) {
                if (session->get_in_transaction()) {
                  ret = OB_ERR_LOCK_OR_ACTIVE_TRANSACTION;

                  LOG_WARN("Can't execute the given command because "
                           "you have active locked tables or an active transaction", K(ret));
                } else {}
              } else {}

              if (OB_FAIL(ret)) {
              } else if (set_var.var_name_ == OB_SV_COMPATIBILITY_MODE) {
                if (!(OB_SYS_TENANT_ID == session->get_effective_tenant_id())
                    || !GCONF.in_upgrade_mode()) {
                  ret = OB_OP_NOT_ALLOW;
                  LOG_WARN("Compatibility mode can be changed only under upgrade mode and system tenant",
                           K(ret), K(session->get_effective_tenant_id()));
                  LOG_USER_ERROR(OB_OP_NOT_ALLOW,
                          "Compatibility mode be changed not under upgrade mode and system tenant");
                } else if (ObSetVar::SET_SCOPE_SESSION != set_var.set_scope_) {
                  ret = OB_OP_NOT_ALLOW;
                  LOG_WARN("Compatibility mode can be changed only under session scope",
                           K(ret), K(session->get_effective_tenant_id()));
                  LOG_USER_ERROR(OB_OP_NOT_ALLOW,
                          "Compatibility mode be changed not in session scope");
                }
              } else {}
                
              if (OB_SUCC(ret) && 0 == set_var.var_name_.case_compare(OB_SV_SECURE_FILE_PRIV)) {
                ObAddr addr = OBSERVER.get_self();
                char buf[MAX_IP_ADDR_LENGTH + 1];
                if (OB_NOT_NULL(ctx.get_my_session())) {
                  ObString client_ip = ctx.get_my_session()->get_client_ip();
                  if (!addr.ip_to_string(buf, sizeof(buf))) {
                    ret = OB_ERR_UNEXPECTED;
                    LOG_WARN("format leader ip failed", K(ret), K(addr));
                  } else if (!(0 == ctx.get_my_session()->get_client_ip().compare(buf)
                               || 0 == client_ip.compare("127.0.0.1")
                               || 0 == client_ip.compare("::1"))) {
                    ret = OB_NOT_SUPPORTED;
                    LOG_WARN("modify SECURE_FILE_PRIV by remote client", K(ret), K(client_ip));
                    LOG_USER_ERROR(OB_NOT_SUPPORTED, "modify SECURE_FILE_PRIV by remote client");
                  }
                }
              }

              if (OB_SUCC(ret) && set_var.set_scope_ == ObSetVar::SET_SCOPE_GLOBAL) {
                if (OB_FAIL(update_global_variables(ctx, stmt, set_var, value_obj))) {
                  LOG_WARN("failed to update global variables", K(ret));
                } else { }
              }
              if (OB_SUCC(ret) && set_var.set_scope_ == ObSetVar::SET_SCOPE_SESSION) {
                if (OB_FAIL(sys_var->session_update(ctx, set_var, value_obj))) {
                  LOG_WARN("fail to update", K(ret), K(*sys_var), K(set_var), K(value_obj));
                }
              }
              //某些变量需要立即更新状态
              if (OB_SUCC(ret)) {
                if (OB_FAIL(sys_var->update(ctx, set_var, value_obj))) {
                  LOG_WARN("update sys var state failed", K(ret), K(set_var));
                }
              }
            }
          }
        }
        if (OB_SYS_VARS_MAYBE_DIFF_VERSION == ret) {
          // 版本兼容，ret改为OB_SUCCESS以便让for循环继续
          ret = OB_SUCCESS;
        }
      }
    }
  }
  if (OB_SUCCESS != ret_ac) {
    // 事务超时的时候，不返回赋值错误码，返回事务超时的错误码
    ret = ret_ac;
  }
  return ret;
}

int ObVariableSetExecutor::calc_var_value_static_engine(
      ObVariableSetStmt::VariableSetNode &node,
      ObVariableSetStmt &stmt,
      ObExecContext &exec_ctx,
      ObObj &value_obj)
{
  int ret = OB_SUCCESS;
  const ParamStore &param_store = exec_ctx.get_physical_plan_ctx()->get_param_store();
  if (OB_ISNULL(node.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("node.value_expr_ is NULL", K(ret));
  } else if (OB_FAIL(ObSQLUtils::calc_const_expr(
                                exec_ctx.get_my_session(),
                                *node.value_expr_,
                                value_obj,
                                exec_ctx.get_allocator(),
                                param_store,
                                &exec_ctx))) {
    LOG_WARN("calc const expr failed", K(ret));
  }
  return ret;
}

// for subquery expr, we calculate expr value by executing an inner sql
int ObVariableSetExecutor::calc_subquery_expr_value(ObExecContext &ctx,
                                                    ObSQLSessionInfo *session_info,
                                                    ObRawExpr *expr,
                                                    common::ObObj &value_obj)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(expr) || OB_ISNULL(session_info)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(expr), K(session_info));
  } else if (expr->has_flag(CNT_SUB_QUERY)) {
    HEAP_VAR(char[OB_MAX_DEFAULT_VALUE_LENGTH], expr_str_buf) {
      MEMSET(expr_str_buf, 0, sizeof(expr_str_buf));
      int64_t pos = 0;
      ObArenaAllocator temp_allocator;
      ObString tmp_expr_query_str;
      ObString expr_query_str;
      ObCharsetType client_cs_type = CHARSET_INVALID;
      ObSqlString tmp_expr_subquery;
      ObSqlString expr_subquery;
      ObRawExprPrinter expr_printer(expr_str_buf, OB_MAX_DEFAULT_VALUE_LENGTH,
                                    &pos, session_info->get_timezone_info());
      if (OB_FAIL(expr_printer.do_print(expr, T_NONE_SCOPE, true))) {
        LOG_WARN("print expr definition failed", K(ret));
      } else if (OB_FAIL(tmp_expr_subquery.assign_fmt("select %.*s from dual",
                                                  static_cast<int32_t>(pos), expr_str_buf))) {
        LOG_WARN("failed to assign sql", K(ret));
      } else if (OB_FALSE_IT(tmp_expr_query_str = ObString::make_string(tmp_expr_subquery.ptr()))) {
      } else if (OB_FAIL(session_info->get_character_set_client(client_cs_type))) {
        LOG_WARN("failed to get character type", K(ret));
      } else if (OB_FAIL(ObCharset::charset_convert(temp_allocator,
                                                    tmp_expr_query_str,
                                                    ObCharset::get_default_collation(ObCharset::get_default_charset()),
                                                    ObCharset::get_default_collation(client_cs_type),
                                                    expr_query_str))) {
        LOG_WARN("failed to convert charset", K(ret));
      } else if (OB_FAIL(expr_subquery.append(expr_query_str))) {
        LOG_WARN("failed to append sql string", K(ret));
      } else if (OB_FAIL(execute_subquery_expr(ctx, session_info, expr_subquery, value_obj))) {
        LOG_WARN("failed to execute subquery expr", K(ret));
      }
    }
  }
  return ret;
}

int ObVariableSetExecutor::execute_subquery_expr(ObExecContext &ctx,
                                                 ObSQLSessionInfo *session_info,
                                                 const ObSqlString &subquery_expr,
                                                 common::ObObj &value_obj)
{
  int ret = OB_SUCCESS;
  ObMySQLProxy *sql_proxy = GCTX.sql_proxy_;
  observer::ObInnerSQLConnectionPool *pool = NULL;
  sqlclient::ObISQLConnection *conn = NULL;
  uint64_t tenant_id = 0;
  if (OB_ISNULL(session_info) || OB_ISNULL(sql_proxy) || OB_ISNULL(sql_proxy->get_pool())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(session_info), K(sql_proxy));
  } else if (OB_UNLIKELY(sqlclient::INNER_POOL != sql_proxy->get_pool()->get_type())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("sql proxy must be inner", K(ret), K(sql_proxy->get_pool()->get_type()));
  } else if (OB_FALSE_IT(pool = static_cast<observer::ObInnerSQLConnectionPool *>(sql_proxy->get_pool()))) {
  } else if (OB_FAIL(pool->acquire(session_info, conn))) {
    LOG_WARN("failed to acquire connection", K(ret));
  } else {
    tenant_id = session_info->get_effective_tenant_id();
    int64_t idx = 0;
    ObObj tmp_value;
    SMART_VAR(ObISQLClient::ReadResult, res) {
      common::sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(conn->execute_read(tenant_id, subquery_expr.ptr(), res))) {
        LOG_WARN("failed to execute sql", K(ret), K(subquery_expr));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret));
      } else if (OB_FAIL(result->next())) {
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        } else {
          LOG_WARN("failed to get next result", K(ret));
        }
      } else if (OB_FAIL(result->get_obj(idx, tmp_value))) {
        LOG_WARN("failed to get obj", K(ret), K(idx));
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(conn) && OB_NOT_NULL(sql_proxy) &&
               OB_FAIL(sql_proxy->close(conn, true))) {
      LOG_WARN("failed to close connection", K(ret));
    } else if (tmp_value.need_deep_copy()) {
      char *copy_data = NULL;
      int64_t copy_size = tmp_value.get_deep_copy_size();
      int64_t copy_pos = 0;
      if (OB_ISNULL(copy_data = static_cast<char *>(ctx.get_allocator().alloc(copy_size)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("memory allocate failed", K(ret));
      } else if (OB_FAIL(value_obj.deep_copy(tmp_value, copy_data, copy_size, copy_pos))) {
        LOG_WARN("obj deep copy failed", K(ret));
      }
      LOG_TRACE("succeed to deep copy current value", K(ret));
    } else {
      value_obj = tmp_value;
    }
    LOG_TRACE("succ to calculate value by executing inner sql", K(ret), K(value_obj), K(subquery_expr));
  }
  return ret;
}

int ObVariableSetExecutor::set_user_variable(const ObObj &val,
                                             const ObString &variable_name,
                                             const ObExprCtx &expr_ctx)
{
  int ret = OB_SUCCESS;
  // user defined tmp variable
  ObSQLSessionInfo *session = expr_ctx.my_session_;
  ObExecContext *ctx = expr_ctx.exec_ctx_;
  ObSessionVariable sess_var;
  if (OB_ISNULL(session) || OB_ISNULL(ctx)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("session is null", K(ret));
  } else if (OB_FAIL(switch_to_session_variable(expr_ctx, val, sess_var))) {
    LOG_WARN("fail to stiwch to session variablee", K(ret), K(val));
  } else if (OB_FAIL(session->replace_user_variable(*ctx, variable_name, sess_var))) {
    LOG_WARN("set variable to session plan failed", K(ret), K(variable_name));
  } else {
  }
  return ret;
}

int ObVariableSetExecutor::set_user_variable(const ObObj &val,
                                             const ObString &variable_name,
                                             ObSQLSessionInfo *session)
{
  int ret = OB_SUCCESS;
  // user defined tmp variable
  ObSessionVariable sess_var;
  if (OB_ISNULL(session)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("session is null", K(ret));
  } else if (OB_FAIL(switch_to_session_variable(val, sess_var))) {
    LOG_WARN("fail to stiwch to session variablee", K(ret), K(val));
  } else if (OB_FAIL(session->replace_user_variable(variable_name, sess_var))) {
    LOG_WARN("set variable to session plan failed", K(ret), K(variable_name));
  } else {
  }
  return ret;
}

int ObVariableSetExecutor::update_global_variables(ObExecContext &ctx,
                                                   ObDDLStmt &stmt,
                                                   const ObSetVar &set_var,
                                                   const ObObj &val)
{
  int ret = OB_SUCCESS;
  obrpc::ObRpcOpts rpc_opt;
  ObSQLSessionInfo *session = NULL;
  ObTaskExecutorCtx *task_exec_ctx = NULL;
  obrpc::ObCommonRpcProxy *common_rpc_proxy = NULL;
  obrpc::ObModifySysVarArg &arg = static_cast<obrpc::ObModifySysVarArg &>(stmt.get_ddl_arg());
  ObString extra_var_name;
  ObString extra_var_value;
  ObString extra_val;
  ObString val_str;
  ObCollationType extra_coll_type = CS_TYPE_INVALID;
  char extra_var_value_buf[32] = {'\0'};
  int64_t pos = 0;
  bool should_update_extra_var = false;
  if (OB_ISNULL(session = ctx.get_my_session())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("session is NULL", K(ret));
  } else {
    arg.tenant_id_ = set_var.actual_tenant_id_;
    arg.exec_tenant_id_ = set_var.actual_tenant_id_;
    ObString first_stmt;
    if (OB_FAIL(stmt.get_first_stmt(first_stmt))) {
      LOG_WARN("fail to get first stmt" , K(ret));
    } else {
      arg.ddl_stmt_str_ = first_stmt;
    }
    if (OB_FAIL(ret)) {
    } else if (set_var.var_name_ == OB_SV_COLLATION_SERVER
        || set_var.var_name_ == OB_SV_COLLATION_DATABASE
        || set_var.var_name_ == OB_SV_COLLATION_CONNECTION) {
      ObString coll_str;
      int64_t coll_int64 = OB_INVALID_INDEX;
      if (OB_FAIL(val.get_int(coll_int64))) {
        LOG_WARN("get int from val failed", K(ret));
      } else if (OB_UNLIKELY(!ObCharset::is_valid_collation(coll_int64))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_ERROR("invalid collation", K(ret), K(coll_int64), K(val));
      } else if (FALSE_IT(coll_str = ObString::make_string(ObCharset::collation_name(static_cast<ObCollationType>(coll_int64))))) {
        //do nothing
      } else if (OB_FAIL(ObBasicSysVar::get_charset_var_and_val_by_collation(
                  set_var.var_name_, coll_str, extra_var_name, extra_val, extra_coll_type))) {
        LOG_ERROR("fail to get charset variable and value by collation",
                  K(ret), K(set_var.var_name_), K(val), K(coll_str));
      } else if (OB_FAIL(databuff_printf(extra_var_value_buf, sizeof(extra_var_value_buf), pos, "%d", static_cast<int32_t>(extra_coll_type)))) {
        LOG_WARN("databuff printf failed", K(extra_coll_type), K(ret));
      } else {
        extra_var_value.assign(extra_var_value_buf, pos);
        should_update_extra_var = true;
      }
    } else if (set_var.var_name_ == OB_SV_CHARACTER_SET_SERVER ||
        set_var.var_name_ == OB_SV_CHARACTER_SET_DATABASE ||
        set_var.var_name_ == OB_SV_CHARACTER_SET_CONNECTION) {
      ObString cs_str;
      int64_t coll_int64 = OB_INVALID_INDEX;
      if (OB_FAIL(val.get_int(coll_int64))) {
        LOG_WARN("get int from value failed", K(ret));
      } else if (OB_UNLIKELY(!ObCharset::is_valid_collation(coll_int64))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid collation", K(ret), K(coll_int64));
      } else if (FALSE_IT(cs_str = ObString::make_string(ObCharset::charset_name(ObCharset::charset_type_by_coll(
            static_cast<ObCollationType>(coll_int64)))))) {
        //do nothing
      } else if (OB_FAIL(ObBasicSysVar::get_collation_var_and_val_by_charset(
          set_var.var_name_, cs_str, extra_var_name, extra_val, extra_coll_type))) {
        LOG_ERROR("fail to get collation variable and value by charset", K(ret), K(set_var.var_name_), K(val), K(cs_str));
      } else if (OB_FAIL(databuff_printf(extra_var_value_buf, sizeof(extra_var_value_buf), pos, "%d", static_cast<int32_t>(extra_coll_type)))) {
        LOG_WARN("databuff printf failed", K(extra_coll_type), K(ret));
      } else {
        extra_var_value.assign(extra_var_value_buf, pos);
        should_update_extra_var = true;
      }
    } else if (set_var.var_name_ == OB_SV_NLS_DATE_FORMAT
               || set_var.var_name_ == OB_SV_NLS_TIMESTAMP_FORMAT
               || set_var.var_name_ == OB_SV_NLS_TIMESTAMP_TZ_FORMAT) {
      ObString format;
      if (OB_UNLIKELY(val.is_null_oracle())) {
        ret = OB_INVALID_DATE_FORMAT;
        LOG_WARN("date format not recognized", K(ret), K(set_var.var_name_), K(val));
      } else if (OB_FAIL(val.get_varchar(format))) {
        LOG_WARN("fail get varchar", K(val), K(ret));
      } else {
        int64_t nls_enum = ObNLSFormatEnum::NLS_DATE;
        ObDTMode mode = DT_TYPE_DATETIME;
        if (set_var.var_name_ == OB_SV_NLS_TIMESTAMP_FORMAT) {
          mode |= DT_TYPE_ORACLE;
          nls_enum = ObNLSFormatEnum::NLS_TIMESTAMP;
        } else if (set_var.var_name_ == OB_SV_NLS_TIMESTAMP_TZ_FORMAT) {
          mode |= DT_TYPE_ORACLE;
          mode |= DT_TYPE_TIMEZONE;
          nls_enum = ObNLSFormatEnum::NLS_TIMESTAMP_TZ;
        }
        ObSEArray<ObDFMElem, ObDFMUtil::COMMON_ELEMENT_NUMBER> dfm_elems;
        ObFixedBitSet<OB_DEFAULT_BITSET_SIZE_FOR_DFM> elem_flags;
        //1. parse and check semantic of format string
        // TODO: support double-quotes in system variable when ob-client support.
        if (OB_FAIL(ObDFMUtil::parse_datetime_format_string(format, dfm_elems,
                                                            false /* support double-quotes */))) {
          LOG_WARN("fail to parse oracle datetime format string", K(ret), K(format));
        } else if (OB_FAIL(ObDFMUtil::check_semantic(dfm_elems, elem_flags, mode))) {
          LOG_WARN("check semantic of format string failed", K(ret), K(format));
        }
      }
    } else if (set_var.var_name_ == OB_SV_LOG_LEVEL) {
      ObString log_level;
      if (OB_FAIL(val.get_varchar(log_level))) {
        LOG_WARN("fail get varchar", K(val), K(ret));
      } else if (0 == log_level.case_compare("disabled")) {
        //allowed for variables
      } else if (OB_FAIL(OB_LOGGER.parse_check(log_level.ptr(), log_level.length()))) {
        LOG_WARN("Log level parse check error", K(log_level), K(ret));
      }
    } else if (set_var.var_name_ == OB_SV_TRANSACTION_ISOLATION) {
      extra_var_name = ObString::make_string(OB_SV_TX_ISOLATION);
      should_update_extra_var = true;
      if (OB_FAIL(val.get_varchar(extra_var_value))) {
        LOG_WARN("fail get varchar", K(val), K(ret));
      }
    } else if (set_var.var_name_ == OB_SV_TX_ISOLATION) {
      extra_var_name = ObString::make_string(OB_SV_TRANSACTION_ISOLATION);
      should_update_extra_var = true;
      if (OB_FAIL(val.get_varchar(extra_var_value))) {
        LOG_WARN("fail get varchar", K(val), K(ret));
      }
    } else if (set_var.var_name_ == OB_SV_TX_READ_ONLY) {
      int64_t extra_var_values = -1;
      extra_var_name = ObString::make_string(OB_SV_TRANSACTION_READ_ONLY);
      if (OB_FAIL(val.get_int(extra_var_values))) {
        LOG_WARN("fail get int", K(val), K(ret));
      } else if (OB_FAIL(databuff_printf(extra_var_value_buf, sizeof(extra_var_value_buf), pos, "%d", static_cast<int32_t>(extra_var_values)))) {
        LOG_WARN("databuff printf failed", K(extra_var_values), K(ret));
      } else {
        extra_var_value.assign(extra_var_value_buf, pos);
        should_update_extra_var = true;
      }
    } else if (set_var.var_name_ == OB_SV_TRANSACTION_READ_ONLY) {
      extra_var_name = ObString::make_string(OB_SV_TX_READ_ONLY);
      int64_t extra_var_values = -1;
      if (OB_FAIL(val.get_int(extra_var_values))) {
        LOG_WARN("fail get int", K(val), K(ret));
      } else if (OB_FAIL(databuff_printf(extra_var_value_buf, sizeof(extra_var_value_buf), pos, "%d", static_cast<int32_t>(extra_var_values)))) {
        LOG_WARN("databuff printf failed", K(extra_var_values), K(ret));
      } else {
        extra_var_value.assign(extra_var_value_buf, pos);
        should_update_extra_var = true;
      }
    }

    if (OB_SUCC(ret) && should_update_extra_var) {
      ObSysVarSchema sysvar_schema;
      if (OB_FAIL(sysvar_schema.set_name(extra_var_name))) {
        LOG_WARN("set sysvar schema name failed", K(ret));
      } else if (OB_FAIL(sysvar_schema.set_value(extra_var_value))) {
        LOG_WARN("set sysvar schema value failed", K(ret));
      } else {
        sysvar_schema.set_tenant_id(arg.tenant_id_);
        if (OB_FAIL(arg.sys_var_list_.push_back(sysvar_schema))) {
          LOG_WARN("store sys var to array failed", K(ret));
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    ObExprCtx expr_ctx;
    expr_ctx.exec_ctx_ = &ctx;
    expr_ctx.calc_buf_ = &set_var.calc_buf_;
    expr_ctx.my_session_ = ctx.get_my_session();
    EXPR_DEFINE_CAST_CTX(expr_ctx, CM_NONE);
    EXPR_GET_VARCHAR_V2(val, val_str);
    ObSysVarSchema sysvar_schema;
    if (OB_UNLIKELY(val_str.length() > OB_MAX_SYS_VAR_VAL_LENGTH)) {
      ret = OB_SIZE_OVERFLOW;
      LOG_WARN("set sysvar value is overflow", "max length", OB_MAX_SYS_VAR_VAL_LENGTH,
               "value length", val_str.length(), "name", set_var.var_name_, "value", val_str);
    } else if (OB_FAIL(sysvar_schema.set_name(set_var.var_name_))) {
      LOG_WARN("set sysvar schema name failed", K(ret));
    } else if (OB_FAIL(sysvar_schema.set_value(val_str))) {
      LOG_WARN("set sysvar schema value failed", K(ret));
    } else {
      sysvar_schema.set_tenant_id(arg.tenant_id_);
      if (OB_FAIL(arg.sys_var_list_.push_back(sysvar_schema))) {
        LOG_WARN("store sys var to array failed", K(ret));
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_ISNULL(task_exec_ctx = GET_TASK_EXECUTOR_CTX(ctx)) ||
        OB_ISNULL(common_rpc_proxy = task_exec_ctx->get_common_rpc())) {
      ret = OB_NOT_INIT;
      LOG_WARN("task exec ctx or common rpc proxy is NULL", K(ret), K(task_exec_ctx), K(common_rpc_proxy));
    } else if (OB_FAIL(common_rpc_proxy->modify_system_variable(arg))) {
      LOG_WARN("rpc proxy alter system variable failed", K(ret));
    } else {}
  }
  return ret;
}

int ObVariableSetExecutor::check_and_convert_sys_var(ObExecContext &ctx,
                                                     const ObSetVar &set_var,
                                                     ObBasicSysVar &sys_var,
                                                     const ObObj &in_val,
                                                     ObObj &out_val,
                                                     bool is_set_stmt)
{
  int ret = OB_SUCCESS;
  //OB_ASSERT(true == var_node.is_system_variable_);

  // collation_connection的取值有限制，不能设置成utf16
  if (OB_SUCC(ret)) {
    if ((0 == set_var.var_name_.case_compare(OB_SV_CHARACTER_SET_CLIENT)
        || 0 == set_var.var_name_.case_compare(OB_SV_CHARACTER_SET_CONNECTION)
        || 0 == set_var.var_name_.case_compare(OB_SV_CHARACTER_SET_RESULTS)
        || 0 == set_var.var_name_.case_compare(OB_SV_COLLATION_CONNECTION))
        && (in_val.get_string().prefix_match_ci("utf16"))) {
      ret = OB_ERR_WRONG_VALUE_FOR_VAR;
      LOG_USER_ERROR(OB_ERR_WRONG_VALUE_FOR_VAR,
                     set_var.var_name_.length(),
                     set_var.var_name_.ptr(),
                     in_val.get_string().length(),
                     in_val.get_string().ptr());
    }
  }

  //check readonly
  if (is_set_stmt && sys_var.is_readonly()) {
    if (sys_var.is_with_upgrade() && GCONF.in_upgrade_mode()) {
      // do nothing ...
    } else {
      ret = OB_ERR_INCORRECT_GLOBAL_LOCAL_VAR;
      LOG_USER_ERROR(OB_ERR_INCORRECT_GLOBAL_LOCAL_VAR, set_var.var_name_.length(), set_var.var_name_.ptr(),
                     (int)strlen("read only"), "read only");
    }
  }

  //check scope
  if (OB_FAIL(ret)) {
  } else if (ObSetVar::SET_SCOPE_GLOBAL == set_var.set_scope_
             && !sys_var.is_global_scope()) {
    ret = OB_ERR_LOCAL_VARIABLE;
    LOG_USER_ERROR(OB_ERR_LOCAL_VARIABLE, set_var.var_name_.length(), set_var.var_name_.ptr());
  } else if (ObSetVar::SET_SCOPE_SESSION == set_var.set_scope_
      && !sys_var.is_session_scope()) {
    ret = OB_ERR_GLOBAL_VARIABLE;
    LOG_USER_ERROR(OB_ERR_GLOBAL_VARIABLE, set_var.var_name_.length(), set_var.var_name_.ptr());
  }

  //check update type and value
  if (OB_FAIL(ret)) {
  } else if (OB_SUCCESS != (ret = sys_var.check_update_type(set_var, in_val))) {
    if (OB_ERR_WRONG_TYPE_FOR_VAR == ret) {
      LOG_USER_ERROR(OB_ERR_WRONG_TYPE_FOR_VAR, set_var.var_name_.length(), set_var.var_name_.ptr());
    } else {
      LOG_WARN("fail to check update type", K(ret));
    }
  } else if (OB_FAIL(sys_var.check_and_convert(ctx, set_var, in_val, out_val))) {
    if (OB_ERR_WRONG_TYPE_FOR_VAR == ret) {
      LOG_USER_ERROR(OB_ERR_WRONG_TYPE_FOR_VAR, set_var.var_name_.length(), set_var.var_name_.ptr());
    } else {
      LOG_WARN("fail to check value", K(ret));
    }
  }

  //do not support modify now
  if (OB_FAIL(ret)) {
  } else if (lib::is_oracle_mode() && set_var.var_name_.prefix_match("nls_")) {
    static const common::ObString DEFAULT_VALUE_LANGUAGE("AMERICAN");
    static const common::ObString DEFAULT_VALUE_TERRITORY("AMERICA");
    static const common::ObString DEFAULT_VALUE_SORT("BINARY");
    static const common::ObString DEFAULT_VALUE_COMP("BINARY");
    static const common::ObString DEFAULT_VALUE_NCHAR_CHARACTERSET("AL16UTF16");
    static const common::ObString DEFAULT_VALUE_DATE_LANGUAGE("AMERICAN");
    static const common::ObString DEFAULT_VALUE_NCHAR_CONV_EXCP("FALSE");
    static const common::ObString DEFAULT_VALUE_CALENDAR("GREGORIAN");
    static const common::ObString DEFAULT_VALUE_NUMERIC_CHARACTERS(".,");

    const ObString new_value = out_val.get_string();
    if ((set_var.var_name_ == OB_SV_NLS_LANGUAGE && new_value.case_compare(DEFAULT_VALUE_LANGUAGE) != 0)
        || (set_var.var_name_ == OB_SV_NLS_TERRITORY && new_value.case_compare(DEFAULT_VALUE_TERRITORY) != 0)
        || (set_var.var_name_ == OB_SV_NLS_SORT && new_value.case_compare(DEFAULT_VALUE_SORT) != 0)
        || (set_var.var_name_ == OB_SV_NLS_COMP && new_value.case_compare(DEFAULT_VALUE_COMP) != 0)
        || (set_var.var_name_ == OB_SV_NLS_CHARACTERSET) //不允许修改charset
        || (set_var.var_name_ == OB_SV_NLS_NCHAR_CHARACTERSET && new_value.case_compare(DEFAULT_VALUE_NCHAR_CHARACTERSET) != 0)
        || (set_var.var_name_ == OB_SV_NLS_DATE_LANGUAGE && new_value.case_compare(DEFAULT_VALUE_DATE_LANGUAGE) != 0)
        || (set_var.var_name_ == OB_SV_NLS_NCHAR_CONV_EXCP && new_value.case_compare(DEFAULT_VALUE_NCHAR_CONV_EXCP) != 0)
        || (set_var.var_name_ == OB_SV_NLS_CALENDAR && new_value.case_compare(DEFAULT_VALUE_CALENDAR) != 0)
        || (set_var.var_name_ == OB_SV_NLS_NUMERIC_CHARACTERS && new_value.case_compare(DEFAULT_VALUE_NUMERIC_CHARACTERS) != 0)) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("not support modify this variables now", K(set_var), K(new_value), K(ret));
      char buf[128];
      databuff_printf(buf, sizeof(buf), "modify NLS data %.*s",
          set_var.var_name_.length(), set_var.var_name_.ptr());
      LOG_USER_ERROR(OB_NOT_SUPPORTED, buf);
    }
  }

  if (OB_FAIL(ret)) {
  } else if (set_var.var_name_ == OB_SV_DEFAULT_STORAGE_ENGINE) {
    static const common::ObString DEFAULT_VALUE_STORAGE_ENGINE("OceanBase");
    const ObString new_value = out_val.get_string();
    if (new_value.case_compare(DEFAULT_VALUE_STORAGE_ENGINE) != 0) {
      ret = OB_ERR_PARAM_VALUE_INVALID;
      LOG_USER_ERROR(OB_ERR_PARAM_VALUE_INVALID);
    }
  }

  return ret;
}

int ObVariableSetExecutor::cast_value(ObExecContext &ctx,
                                      const ObVariableSetStmt::VariableSetNode &var_node,
                                      uint64_t actual_tenant_id,
                                      ObIAllocator &calc_buf,
                                      const ObBasicSysVar &sys_var,
                                      const ObObj &in_val,
                                      ObObj &out_val)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("schema_service_ is null");
  } else if (OB_ISNULL(ctx.get_my_session())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("my session is null");
  } else if (var_node.is_set_default_) {
    uint64_t tenant_id = actual_tenant_id;
    if (ObSetVar::SET_SCOPE_SESSION == var_node.set_scope_) {
      ObSchemaGetterGuard schema_guard;
      const ObSysVarSchema *var_schema = NULL;
      const ObDataTypeCastParams dtc_params =
            ObBasicSessionInfo::create_dtc_params(ctx.get_my_session());
      if (OB_FAIL(GCTX.schema_service_->get_tenant_schema_guard(
                  tenant_id,
                  schema_guard))) {
        LOG_WARN("get schema guard failed", K(ret));
      } else if (OB_FAIL(schema_guard.get_tenant_system_variable(tenant_id, var_node.variable_name_, var_schema))) {
        LOG_WARN("get tenant system variable failed", K(ret), K(tenant_id), K(var_node.variable_name_));
      } else if (OB_FAIL(var_schema->get_value(&calc_buf, dtc_params, out_val))) {
        LOG_WARN("get value from sysvar schema failed", K(ret));
      }
    } else if (ObSetVar::SET_SCOPE_GLOBAL == var_node.set_scope_) {
      const ObObj &def_val = sys_var.get_global_default_value();
      DEFINE_CAST_CTX();
      if (OB_FAIL(ObObjCaster::to_type(sys_var.get_data_type(), cast_ctx, def_val, out_val))) {
        LOG_ERROR("failed to cast object", K(ret), K(var_node.variable_name_),
                  K(def_val), K(sys_var.get_data_type()));
      }
    } else {
      ret = OB_ERR_UNEXPECTED;
      LOG_ERROR("invalid set scope", K(ret), K(var_node.set_scope_));
    }
  } else if (ObNullType == in_val.get_type()) {
    out_val = in_val;
  } else {
    DEFINE_CAST_CTX();
    if (OB_FAIL(ObObjCaster::to_type(sys_var.get_data_type(), cast_ctx, in_val, out_val))) {
      LOG_WARN("failed to cast object", K(ret), K(var_node.variable_name_),
               K(in_val), K(sys_var.get_data_type()));
    } else {}
  }
  return ret;
}

// 当执行set autocommit=1的时候, 可能会触发implicit commit。
// 事务控制语句：BEGIN、START TRANSACTION、SET AUTOCOMMIT=1（如果当前状态是
// AC=0时）会触发implicit commit,
// 这保证了事务不会嵌套。
int ObVariableSetExecutor::process_session_autocommit_hook(ObExecContext &exec_ctx,
                                                           const ObObj &val)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *my_session = GET_MY_SESSION(exec_ctx);
  if (OB_ISNULL(my_session)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("session is NULL", K(ret));
  } else {
    int64_t autocommit = 0;
    bool in_trans = my_session->get_in_transaction();
    bool ac = true;

    if (OB_FAIL(my_session->get_autocommit(ac))) {
      LOG_WARN("fail to get autocommit", K(ret));
    } else if (OB_FAIL(val.get_int(autocommit))) {
      LOG_WARN("fail get commit val", K(val), K(ret));
    } else if (0 != autocommit && 1 != autocommit) {
      const char *autocommit_str = to_cstring(autocommit);
      ret = OB_ERR_WRONG_VALUE_FOR_VAR;
      LOG_USER_ERROR(OB_ERR_WRONG_VALUE_FOR_VAR, (int)strlen(OB_SV_AUTOCOMMIT), OB_SV_AUTOCOMMIT,
                     (int)strlen(autocommit_str), autocommit_str);
    } else {
      if (false == ac &&  true == in_trans && 1 == autocommit) {
        if (OB_FAIL(ObSqlTransControl::implicit_end_trans(exec_ctx, false))) {
          LOG_WARN("fail implicit commit trans", K(ret));
        }
      } else {
        // 其它只影响AC标志位，但无需做commit操作
      }
    }
  }
  return ret;
}

int ObVariableSetExecutor::process_auto_increment_hook(const ObSQLMode sql_mode,
                                                       const ObString var_name,
                                                       ObObj &val)
{
  int ret = OB_SUCCESS;
  uint64_t auto_increment = 0;
  if (OB_FAIL(val.get_uint64(auto_increment))) {
    LOG_WARN("fail get auto_increment value", K(ret), K(val));
  } else {
    if (SMO_STRICT_ALL_TABLES & sql_mode) {
      if (auto_increment <= 0 || auto_increment > UINT16_MAX) {
        char auto_increment_str[OB_CAST_TO_VARCHAR_MAX_LENGTH];
        int length = snprintf(auto_increment_str, OB_CAST_TO_VARCHAR_MAX_LENGTH,
                                  "%lu", auto_increment);
        if (length < 0 || length >= OB_CAST_TO_VARCHAR_MAX_LENGTH) {
          length = OB_CAST_TO_VARCHAR_MAX_LENGTH - 1;
          auto_increment_str[length] = '\0';
        }
        ret = OB_ERR_WRONG_VALUE_FOR_VAR;
        LOG_USER_ERROR(OB_ERR_WRONG_VALUE_FOR_VAR, var_name.length(), var_name.ptr(), length, auto_increment_str);
      }
    } else {
      if (auto_increment <= 0) {
        auto_increment = 1;
        // should generate warning message
        //SQL_ENG_LOG(WARN, "Truncated incorrect value");
      } else if (auto_increment > UINT16_MAX) {
        auto_increment = UINT16_MAX;
        // should generate warning message
        //SQL_ENG_LOG(WARN, "Truncated incorrect value");
      }
      val.set_uint64(auto_increment);
    }
  }
  return ret;
}

int ObVariableSetExecutor::process_last_insert_id_hook(ObPhysicalPlanCtx *plan_ctx,
                                                       const ObSQLMode sql_mode,
                                                       const ObString var_name,
                                                       ObObj &val)
{
  int ret = OB_SUCCESS;
  int64_t value = 0;
  uint64_t unsigned_value = 0;
  if (OB_ISNULL(plan_ctx)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("plan ctx is NULL", K(ret));
  } else if (OB_FAIL(val.get_int(value))) {
    if (OB_FAIL(val.get_uint64(unsigned_value))) {
      ret = OB_ERR_WRONG_TYPE_FOR_VAR;
      LOG_WARN("failed to get value", K(val), K(ret));
    }
  } else {
    if (SMO_STRICT_ALL_TABLES & sql_mode) {
      if (value < 0) {
        char auto_increment_str[OB_CAST_TO_VARCHAR_MAX_LENGTH];
        int32_t length = snprintf(auto_increment_str, OB_CAST_TO_VARCHAR_MAX_LENGTH, "%ld", value);
        if (length < 0 || length >= OB_CAST_TO_VARCHAR_MAX_LENGTH) {
          length = OB_CAST_TO_VARCHAR_MAX_LENGTH - 1;
          auto_increment_str[length] = '\0';
        }
        ret = OB_ERR_WRONG_VALUE_FOR_VAR;
        LOG_USER_ERROR(OB_ERR_WRONG_VALUE_FOR_VAR, var_name.length(), var_name.ptr(), length, auto_increment_str);
      }
    } else {
      if (value < 0) {
        value = 0;
        // should generate warning message
        //SQL_ENG_LOG(WARN, "Truncated incorrect value");
      }
      val.set_int(value);
    }
  }
  if (OB_SUCC(ret)) {
    if (0 != unsigned_value) {
      plan_ctx->set_last_insert_id_session(unsigned_value);
    } else {
      plan_ctx->set_last_insert_id_session(static_cast<uint64_t>(value));
    }
  }
  return ret;
}

int ObVariableSetExecutor::switch_to_session_variable(const ObExprCtx &expr_ctx,
                                                      const ObObj &value,
                                                      ObSessionVariable &sess_var)
{
  int ret = OB_SUCCESS;
  if (ob_is_temporal_type(value.get_type())) {//switch the meta type and value type
    EXPR_DEFINE_CAST_CTX(expr_ctx, CM_NONE);
    ObObj obj_tmp;
    const ObObj *res_obj_ptr = NULL;
    if (OB_FAIL(ObObjCaster::to_type(ObVarcharType, cast_ctx, value, obj_tmp, res_obj_ptr))) {
      LOG_WARN("failed to cast object to ObVarcharType ", K(ret), K(value));
    } else if (OB_ISNULL(res_obj_ptr)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("res_obj_ptr is NULL", K(ret));
    } else {
      sess_var.value_.set_varchar(res_obj_ptr->get_varchar());
      sess_var.meta_.set_collation_level(CS_LEVEL_IMPLICIT);
      sess_var.meta_.set_collation_type(ObCharset::get_default_collation(
              ObCharset::get_default_charset()));
      sess_var.meta_.set_varchar();
    }
  } else if (ObNullType == value.get_type()) {// switch the meta type only
    sess_var.value_.set_null();
    sess_var.meta_.set_collation_level(CS_LEVEL_IMPLICIT);
    sess_var.meta_.set_collation_type(CS_TYPE_BINARY);
  } else { // won't switch
    sess_var.value_ = value;
    sess_var.meta_.set_type(value.get_type());
    sess_var.meta_.set_scale(value.get_scale());
    sess_var.meta_.set_collation_level(CS_LEVEL_IMPLICIT);
    sess_var.meta_.set_collation_type(value.get_collation_type());
  }
  return ret;
}

int ObVariableSetExecutor::switch_to_session_variable(const ObObj &value,
                                                      ObSessionVariable &sess_var)
{
  int ret = OB_SUCCESS;
  if (ob_is_temporal_type(value.get_type())) {
   ret = OB_ERR_UNEXPECTED;
   LOG_WARN("unexpected type", K(ret), K(value));
  } else if (ObNullType == value.get_type()) {// switch the meta type only
    sess_var.value_.set_null();
    sess_var.meta_.set_collation_level(CS_LEVEL_IMPLICIT);
    sess_var.meta_.set_collation_type(CS_TYPE_BINARY);
  } else { // won't switch
    sess_var.value_ = value;
    sess_var.meta_.set_type(value.get_type());
    sess_var.meta_.set_scale(value.get_scale());
    sess_var.meta_.set_collation_level(CS_LEVEL_IMPLICIT);
    sess_var.meta_.set_collation_type(value.get_collation_type());
  }
  return ret;
}

#undef DEFINE_CAST_CTX

}/* ns sql*/
}/* ns oceanbase */
