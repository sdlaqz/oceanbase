/**
 * Copyright (c) 2023 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX WR

#include "deps/oblib/src/lib/mysqlclient/ob_mysql_proxy.h"
#include "observer/ob_server_struct.h"
#include "share/wr/ob_wr_collector.h"
#include "share/wr/ob_wr_task.h"
#include "share/ob_define.h"
#include "share/ob_dml_sql_splicer.h"
#include "lib/utility/ob_tracepoint.h"
#include "sql/monitor/ob_sql_stat_record.h"
#include "share/rc/ob_tenant_base.h"
#include "sql/plan_cache/ob_plan_cache.h"

using namespace oceanbase::common::sqlclient;
using namespace oceanbase::sql;

namespace oceanbase
{
namespace share
{

#define WR_INSERT_BATCH_SIZE 5000
#define WR_INSERT_SQL_STAT_BATCH_SIZE 16

ObWrCollector::ObWrCollector(int64_t snap_id, int64_t snapshot_begin_time,
    int64_t snapshot_end_time, int64_t snapshot_timeout_ts)
    : snap_id_(snap_id),
      snapshot_begin_time_(snapshot_begin_time),
      snapshot_end_time_(snapshot_end_time),
      timeout_ts_(snapshot_timeout_ts)
{
  if (OB_UNLIKELY(snapshot_begin_time_ == snapshot_end_time_)) {
    snapshot_begin_time_ = 0;
  }
}

ERRSIM_POINT_DEF(ERRSIM_WR_SNAPSHOT_COLLECTOR_FAILURE);

int ObWrCollector::collect()
{
  int ret = OB_SUCCESS;
  ObASHSetInnerSqlWaitGuard ash_inner_sql_guard(ObInnerSqlWaitTypeId::WR_TAKE_SNAPSHOT);
  if (OB_FAIL(collect_sysstat())) {
    LOG_WARN("failed to collect sysstat", KR(ret));
  } else if (OB_FAIL(collect_system_event())) {
    LOG_WARN("failed to collect system event", KR(ret));
  } else if (OB_FAIL(collect_ash())) {
    LOG_WARN("failed to collect ash", KR(ret));
  } else if (OB_FAIL(collect_statname())) {
    LOG_WARN("failed to collect statname", KR(ret));
  } else if (OB_FAIL(collect_eventname())) {
    LOG_WARN("failed to collect wait event name", KR(ret));
  } else if (OB_FAIL(collect_sqlstat())) {
    LOG_WARN("failed to collect sqlstat", KR(ret));
  } else if (OB_FAIL(update_sqlstat())) {
    LOG_WARN("failed to update sqlstat", KR(ret));
  } else if (OB_FAIL(collect_sqltext())) {
    LOG_WARN("failed to collect sql text", KR(ret));
  }

  if (OB_SUCC(ret) && OB_UNLIKELY(ERRSIM_WR_SNAPSHOT_COLLECTOR_FAILURE)) {
    ret = ERRSIM_WR_SNAPSHOT_COLLECTOR_FAILURE;
    LOG_WARN("errsim for wr snapshot collector", KR(ret), KPC(this));
  }
  return ret;
}

int ObWrCollector::collect_sysstat()
{
  int ret = OB_SUCCESS;
  common::ObMySQLProxy *sql_proxy = GCTX.sql_proxy_;
  const uint64_t tenant_id = MTL_ID();
  int64_t cluster_id = ObServerConfig::get_instance().cluster_id;
  ObDMLSqlSplicer dml_splicer;
  ObSqlString sql;
  int64_t tmp_real_str_len = 0;
  int64_t query_timeout = timeout_ts_ - common::ObTimeUtility::current_time();
  SMART_VAR(ObISQLClient::ReadResult, res)
  {
    ObMySQLResult *result = nullptr;
    if (OB_UNLIKELY(query_timeout <= 0)) {
      ret = OB_TIMEOUT;
      LOG_WARN("wr snapshot timeout", KR(ret), K_(timeout_ts));
    } else if (OB_FAIL(sql.assign_fmt(
                   "SELECT /*+ WORKLOAD_REPOSITORY_SNAPSHOT QUERY_TIMEOUT(%ld) */ svr_ip, svr_port, stat_id, value from "
                   "__all_virtual_sysstat where tenant_id=%ld",
                   query_timeout, tenant_id))) {
      LOG_WARN("failed to assign sysstat query string", KR(ret));
    } else if (OB_FAIL(sql_proxy->read(res, tenant_id, sql.ptr()))) {
      LOG_WARN("failed to fetch sysstat", KR(ret), K(tenant_id), K(sql));
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("fail to get mysql result", KR(ret), K(tenant_id), K(sql));
    } else {
      while (OB_SUCC(ret)) {
        if (OB_FAIL(result->next())) {
          if (OB_ITER_END == ret) {
            ret = OB_SUCCESS;
            break;
          } else {
            LOG_WARN("fail to get next row", KR(ret));
          }
        } else {
          ObWrSysstat sysstat;
          EXTRACT_STRBUF_FIELD_MYSQL(
              *result, "svr_ip", sysstat.svr_ip_, OB_IP_STR_BUFF, tmp_real_str_len);
          EXTRACT_INT_FIELD_MYSQL(*result, "svr_port", sysstat.svr_port_, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "stat_id", sysstat.stat_id_, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "value", sysstat.value_, int64_t);
          if (OB_SUCC(ret)) {
            if (OB_FAIL(dml_splicer.add_pk_column(K(tenant_id)))) {
              LOG_WARN("failed to add tenant_id", KR(ret), K(tenant_id));
            } else if (OB_FAIL(dml_splicer.add_pk_column(K(cluster_id)))) {
              LOG_WARN("failed to add column cluster_id", KR(ret), K(cluster_id));
            } else if (OB_FAIL(dml_splicer.add_pk_column("SNAP_ID", snap_id_))) {
              LOG_WARN("failed to add column SNAP_ID", KR(ret), K(snap_id_));
            } else if (OB_FAIL(dml_splicer.add_pk_column("svr_ip", sysstat.svr_ip_))) {
              LOG_WARN("failed to add column svr_ip", KR(ret), K(sysstat));
            } else if (OB_FAIL(dml_splicer.add_pk_column("svr_port", sysstat.svr_port_))) {
              LOG_WARN("failed to add column svr_port", KR(ret), K(sysstat));
            } else if (OB_FAIL(dml_splicer.add_pk_column("stat_id", sysstat.stat_id_))) {
              LOG_WARN("failed to add column stat_id", KR(ret), K(sysstat));
            } else if (OB_FAIL(dml_splicer.add_column("value", sysstat.value_))) {
              LOG_WARN("failed to add column value", KR(ret), K(sysstat));
            } else if (OB_FAIL(dml_splicer.finish_row())) {
              LOG_WARN("failed to finish row", KR(ret));
            }
          }
        }
        if (OB_SUCC(ret) && dml_splicer.get_row_count() >= WR_INSERT_BATCH_SIZE) {
          if (OB_FAIL(write_to_wr(dml_splicer, OB_WR_SYSSTAT_TNAME, tenant_id))) {
            LOG_WARN("failed to batch write to wr", KR(ret));
          }
        }
      }
      if (OB_SUCC(ret) && dml_splicer.get_row_count() > 0 &&
          OB_FAIL(write_to_wr(dml_splicer, OB_WR_SYSSTAT_TNAME, tenant_id))) {
        LOG_WARN("failed to batch write remaining part to wr", KR(ret));
      }
    }
  }
  return ret;
}

int ObWrCollector::collect_ash()
{
  int ret = OB_SUCCESS;
  common::ObMySQLProxy *sql_proxy = GCTX.sql_proxy_;
  const uint64_t tenant_id = MTL_ID();
  int64_t cluster_id = ObServerConfig::get_instance().cluster_id;
  ObDMLSqlSplicer dml_splicer;
  ObSqlString sql;
  int64_t tmp_real_str_len = 0;
  int64_t query_timeout = timeout_ts_ - common::ObTimeUtility::current_time();
  int64_t collected_ash_row_count = 0;
  uint64_t data_version = 0;

  SMART_VAR(ObISQLClient::ReadResult, res)
  {
    ObMySQLResult *result = nullptr;
    const char *ASH_VIEW_SQL = "select /*+ WORKLOAD_REPOSITORY_SNAPSHOT QUERY_TIMEOUT(%ld) */ svr_ip, svr_port, sample_id, session_id, "
                   "time_to_usec(sample_time) as sample_time, "
                   "user_id, session_type, sql_id, trace_id, event_no, time_waited, "
                   "p1, p2, p3, sql_plan_line_id, time_model, module, action, "
                   "client_id, backtrace, plan_id, program, tm_delta_time, tm_delta_cpu_time, tm_delta_db_time from "
                   "__all_virtual_ash where tenant_id=%ld and is_wr_sample=true and "
                   "sample_time between usec_to_time(%ld) and usec_to_time(%ld)";
    const char *ASH_VIEW_SQL_422 = "select /*+ WORKLOAD_REPOSITORY_SNAPSHOT QUERY_TIMEOUT(%ld) */ svr_ip, svr_port, sample_id, session_id, "
                   "time_to_usec(sample_time) as sample_time, "
                   "user_id, session_type, sql_id, top_level_sql_id, trace_id, event_no, event_id, time_waited, "
                   "p1, p2, p3, sql_plan_line_id, time_model, module, action, "
                   "client_id, backtrace, plan_id, program, tm_delta_time, tm_delta_cpu_time, tm_delta_db_time, "
                   "plsql_entry_object_id, plsql_entry_subprogram_id, plsql_entry_subprogram_name, plsql_object_id, "
                   "plsql_subprogram_id, plsql_subprogram_name  from "
                   "__all_virtual_ash where tenant_id=%ld and is_wr_sample=true and "
                   "sample_time between usec_to_time(%ld) and usec_to_time(%ld)";
    if (OB_FAIL(GET_MIN_DATA_VERSION(tenant_id, data_version))) {
      LOG_WARN("get_min_data_version failed", K(ret), K(tenant_id));
    } else if (OB_UNLIKELY(query_timeout <= 0)) {
      ret = OB_TIMEOUT;
      LOG_WARN("wr snapshot timeout", KR(ret), K_(timeout_ts));
    } else if (OB_FAIL(sql.assign_fmt(
                   data_version >= DATA_VERSION_4_2_2_0 ? ASH_VIEW_SQL_422 : ASH_VIEW_SQL,
                   query_timeout, tenant_id, snapshot_begin_time_, snapshot_end_time_))) {
      LOG_WARN("failed to assign ash query string", KR(ret));
    } else if (OB_FAIL(sql_proxy->read(res, tenant_id, sql.ptr()))) {
      LOG_WARN("failed to fetch ash", KR(ret), K(tenant_id), K(sql));
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("fail to get mysql result", KR(ret), K(tenant_id), K(sql));
    } else {
      const bool skip_null_error = true;
      const bool skip_column_error = false;
      const int64_t default_value = -1;
      while (OB_SUCC(ret)) {
        if (OB_FAIL(result->next())) {
          if (OB_ITER_END == ret) {
            ret = OB_SUCCESS;
            break;
          } else {
            LOG_WARN("fail to get next row", KR(ret));
          }
        } else {
          ObWrAsh ash;
          EXTRACT_STRBUF_FIELD_MYSQL(
              *result, "svr_ip", ash.svr_ip_, sizeof(ash.svr_ip_), tmp_real_str_len);
          EXTRACT_INT_FIELD_MYSQL(*result, "svr_port", ash.svr_port_, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "sample_id", ash.sample_id_, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "session_id", ash.session_id_, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "sample_time", ash.sample_time_, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "user_id", ash.user_id_, int64_t);
          EXTRACT_BOOL_FIELD_MYSQL(*result, "session_type", ash.session_type_);
          EXTRACT_STRBUF_FIELD_MYSQL(
              *result, "sql_id", ash.sql_id_, sizeof(ash.sql_id_), tmp_real_str_len);
          EXTRACT_STRBUF_FIELD_MYSQL(
              *result, "trace_id", ash.trace_id_, sizeof(ash.trace_id_), tmp_real_str_len);
          EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "event_no", ash.event_no_, int64_t, skip_null_error, skip_column_error, default_value);
          EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "event_id", ash.event_id_, int64_t, skip_null_error, skip_column_error, default_value);
          EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "time_waited", ash.time_waited_, int64_t, skip_null_error, skip_column_error, default_value);
          EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "p1", ash.p1_, int64_t, skip_null_error, skip_column_error, default_value);
          EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "p2", ash.p2_, int64_t, skip_null_error, skip_column_error, default_value);
          EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "p3", ash.p3_, int64_t, skip_null_error, skip_column_error, default_value);
          EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "sql_plan_line_id", ash.sql_plan_line_id_, int64_t, skip_null_error, skip_column_error, default_value);
          EXTRACT_UINT_FIELD_MYSQL(*result, "time_model", ash.time_model_, uint64_t);
          EXTRACT_STRBUF_FIELD_MYSQL_SKIP_RET(*result, "program", ash.program_, sizeof(ash.program_), tmp_real_str_len);
          EXTRACT_INT_FIELD_MYSQL(*result, "tm_delta_time", ash.tm_delta_time_, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "tm_delta_cpu_time", ash.tm_delta_cpu_time_, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "tm_delta_db_time", ash.tm_delta_db_time_, int64_t);
          EXTRACT_STRBUF_FIELD_MYSQL_SKIP_RET(*result, "module", ash.module_, sizeof(ash.module_), tmp_real_str_len);
          // ash not implement this for now.
          // EXTRACT_STRBUF_FIELD_MYSQL(*result, "action", ash.action_,
          // sizeof(ash.action_), tmp_real_str_len);
          // EXTRACT_STRBUF_FIELD_MYSQL(*result,
          // "client_id", ash.client_id_, sizeof(ash.client_id_), tmp_real_str_len);
          EXTRACT_STRBUF_FIELD_MYSQL_SKIP_RET(
              *result, "backtrace", ash.backtrace_, sizeof(ash.backtrace_), tmp_real_str_len);
          EXTRACT_INT_FIELD_MYSQL(*result, "plan_id", ash.plan_id_, int64_t);
          EXTRACT_STRBUF_FIELD_MYSQL_SKIP_RET(
              *result, "top_level_sql_id", ash.top_level_sql_id_, sizeof(ash.top_level_sql_id_), tmp_real_str_len);
          EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "plsql_entry_object_id", ash.plsql_entry_object_id_,
              int64_t, skip_null_error, skip_column_error, OB_INVALID_ID);
          EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "plsql_entry_subprogram_id", ash.plsql_entry_subprogram_id_,
              int64_t, skip_null_error, skip_column_error, OB_INVALID_ID);
          EXTRACT_STRBUF_FIELD_MYSQL_SKIP_RET(
              *result, "plsql_entry_subprogram_name", ash.plsql_entry_subprogram_name_, sizeof(ash.plsql_entry_subprogram_name_), tmp_real_str_len);
          EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "plsql_object_id", ash.plsql_object_id_,
              int64_t, skip_null_error, skip_column_error, OB_INVALID_ID);
          EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "plsql_subprogram_id", ash.plsql_subprogram_id_,
              int64_t, skip_null_error, skip_column_error, OB_INVALID_ID);
          EXTRACT_STRBUF_FIELD_MYSQL_SKIP_RET(
              *result, "plsql_subprogram_name", ash.plsql_subprogram_name_, sizeof(ash.plsql_subprogram_name_), tmp_real_str_len);
          if (OB_SUCC(ret)) {
            if (OB_FAIL(dml_splicer.add_pk_column(K(tenant_id)))) {
              LOG_WARN("failed to add tenant_id", KR(ret), K(tenant_id));
            } else if (OB_FAIL(dml_splicer.add_pk_column(K(cluster_id)))) {
              LOG_WARN("failed to add column cluster_id", KR(ret), K(cluster_id));
            } else if (OB_FAIL(dml_splicer.add_pk_column("SNAP_ID", snap_id_))) {
              LOG_WARN("failed to add column SNAP_ID", KR(ret), K(snap_id_));
            } else if (OB_FAIL(dml_splicer.add_pk_column("svr_ip", ash.svr_ip_))) {
              LOG_WARN("failed to add column svr_ip", KR(ret), K(ash));
            } else if (OB_FAIL(dml_splicer.add_pk_column("svr_port", ash.svr_port_))) {
              LOG_WARN("failed to add column svr_port", KR(ret), K(ash));
            } else if (OB_FAIL(dml_splicer.add_pk_column("sample_id", ash.sample_id_))) {
              LOG_WARN("failed to add column sample_id", KR(ret), K(ash));
            } else if (OB_FAIL(dml_splicer.add_pk_column("session_id", ash.session_id_))) {
              LOG_WARN("failed to add column session_id", KR(ret), K(ash));
            } else if (OB_FAIL(dml_splicer.add_time_column("sample_time", ash.sample_time_))) {
              LOG_WARN("failed to add column sample_time", KR(ret), K(ash));
            } else if (OB_FAIL(dml_splicer.add_column("user_id", ash.user_id_))) {
              LOG_WARN("failed to add column user_id", KR(ret), K(ash));
            } else if (OB_FAIL(dml_splicer.add_column("session_type", ash.session_type_))) {
              LOG_WARN("failed to add column session_type", KR(ret), K(ash));
            } else if (OB_FAIL(dml_splicer.add_column("sql_id", ash.sql_id_))) {
              LOG_WARN("failed to add column sql_id", KR(ret), K(ash));
            } else if (OB_FAIL(dml_splicer.add_column("trace_id", ash.trace_id_))) {
              LOG_WARN("failed to add column trace_id", KR(ret), K(ash));
            } else if (ash.event_no_ < 0 && OB_FAIL(dml_splicer.add_column(true, "event_no"))) {
              LOG_WARN("failed to add column event_no", KR(ret), K(ash));
            } else if (ash.event_no_ >= 0 && OB_FAIL(dml_splicer.add_column("event_no", ash.event_no_))) {
              LOG_WARN("failed to add column event_no", KR(ret), K(ash));
            } else if (ash.event_no_ < 0 && OB_FAIL(dml_splicer.add_column(true, "event_id"))) {
              LOG_WARN("failed to add column event_id", KR(ret), K(ash));
            } else if (ash.event_no_ >= 0 && OB_FAIL(dml_splicer.add_column("event_id", ash.event_id_))) {
              LOG_WARN("failed to add column event_id", KR(ret), K(ash));
            } else if (ash.time_waited_ < 0 && OB_FAIL(dml_splicer.add_column(true, "time_waited"))) {
              LOG_WARN("failed to add column time_waited", KR(ret), K(ash));
            } else if (ash.time_waited_ >= 0 && OB_FAIL(dml_splicer.add_column("time_waited", ash.time_waited_))) {
              LOG_WARN("failed to add column time_waited", KR(ret), K(ash));
            } else if (ash.p1_ < 0 && OB_FAIL(dml_splicer.add_column(true, "p1"))) {
              LOG_WARN("failed to add column p1", KR(ret), K(ash));
            } else if (ash.p1_ >= 0 && OB_FAIL(dml_splicer.add_column("p1", ash.p1_))) {
              LOG_WARN("failed to add column p1", KR(ret), K(ash));
            } else if (ash.p2_ < 0 && OB_FAIL(dml_splicer.add_column(true, "p2"))) {
              LOG_WARN("failed to add column p2", KR(ret), K(ash));
            } else if (ash.p2_ >= 0 && OB_FAIL(dml_splicer.add_column("p2", ash.p2_))) {
              LOG_WARN("failed to add column p2", KR(ret), K(ash));
            } else if (ash.p2_ < 0 && OB_FAIL(dml_splicer.add_column(true, "p3"))) {
              LOG_WARN("failed to add column p3", KR(ret), K(ash));
            } else if (ash.p3_ >= 0 && OB_FAIL(dml_splicer.add_column("p3", ash.p3_))) {
              LOG_WARN("failed to add column p3", KR(ret), K(ash));
            } else if (ash.sql_plan_line_id_ < 0 &&
                       OB_FAIL(dml_splicer.add_column(true, "sql_plan_line_id"))) {
              LOG_WARN("failed to add column sql_plan_line_id", KR(ret), K(ash));
            } else if (ash.sql_plan_line_id_ >= 0 &&
                       OB_FAIL(dml_splicer.add_column("sql_plan_line_id", ash.sql_plan_line_id_))) {
              LOG_WARN("failed to add column sql_plan_line_id", KR(ret), K(ash));
            } else if (OB_FAIL(dml_splicer.add_column("time_model", ash.time_model_))) {
              LOG_WARN("failed to add column time_model", KR(ret), K(ash));
            } else if (ash.module_[0] == '\0' && OB_FAIL(dml_splicer.add_column(true, "module"))) {
              LOG_WARN("failed to add column module", KR(ret), K(ash));
            } else if (ash.module_[0] != '\0' && OB_FAIL(dml_splicer.add_column("module", ash.module_))) {
              LOG_WARN("failed to add column module", KR(ret), K(ash));
            } else if (OB_FAIL(dml_splicer.add_column(true, "action"))) {
              LOG_WARN("failed to add column action", KR(ret), K(ash));
            } else if (OB_FAIL(dml_splicer.add_column(true, "client_id"))) {
              LOG_WARN("failed to add column client_id", KR(ret), K(ash));
            } else if (ash.backtrace_[0] == '\0' &&
                       OB_FAIL(dml_splicer.add_column(true, "backtrace"))) {
              LOG_WARN("failed to add column backtrace", KR(ret), K(ash));
            } else if (ash.backtrace_[0] != '\0' &&
                       OB_FAIL(dml_splicer.add_column("backtrace", ash.backtrace_))) {
              LOG_WARN("failed to add column backtrace", KR(ret), K(ash));
            } else if (OB_FAIL(dml_splicer.add_column("plan_id", ash.plan_id_))) {
              LOG_WARN("failed to add column plan_id", KR(ret), K(ash));
            } else if (ash.program_[0] == '\0' &&
                       OB_FAIL(dml_splicer.add_column(true, "program"))) {
              LOG_WARN("failed to add null column program", KR(ret), K(ash));
            } else if (ash.program_[0] != '\0' &&
                       OB_FAIL(dml_splicer.add_column("program", ash.program_))) {
              LOG_WARN("failed to add column program", KR(ret), K(ash));
            } else if (OB_FAIL(dml_splicer.add_column("tm_delta_time", ash.tm_delta_time_))) {
              LOG_WARN("failed to add column tm_delta_time", KR(ret), K(ash));
            } else if (OB_FAIL(dml_splicer.add_column("tm_delta_cpu_time", ash.tm_delta_cpu_time_))) {
              LOG_WARN("failed to add column tm_delta_cpu_time", KR(ret), K(ash));
            } else if (OB_FAIL(dml_splicer.add_column("tm_delta_db_time", ash.tm_delta_db_time_))) {
              LOG_WARN("failed to add column tm_delta_db_time", KR(ret), K(ash));
            } else if (ash.top_level_sql_id_[0] == '\0' &&
                       OB_FAIL(dml_splicer.add_column(true, "top_level_sql_id"))) {
              LOG_WARN("failed to add column top_level_sql_id", KR(ret), K(ash));
            } else if (ash.top_level_sql_id_[0] != '\0' &&
                       OB_FAIL(dml_splicer.add_column("top_level_sql_id", ash.top_level_sql_id_))) {
              LOG_WARN("failed to add column sql_id", KR(ret), K(ash));
            } else if (OB_INVALID_ID == ash.plsql_entry_object_id_ &&
                       OB_FAIL(dml_splicer.add_column(true, "plsql_entry_object_id"))) {
              LOG_WARN("failed to add column plsql_entry_object_id", KR(ret), K(ash));
            } else if (OB_INVALID_ID != ash.plsql_entry_object_id_ &&
                        OB_FAIL(dml_splicer.add_column("plsql_entry_object_id", ash.plsql_entry_object_id_))) {
              LOG_WARN("failed to add column plsql_entry_object_id", KR(ret), K(ash));
            } else if (OB_INVALID_ID == ash.plsql_entry_subprogram_id_ &&
                       OB_FAIL(dml_splicer.add_column(true, "plsql_entry_subprogram_id"))) {
              LOG_WARN("failed to add column plsql_entry_subprogram_id", KR(ret), K(ash));
            } else if (OB_INVALID_ID != ash.plsql_entry_subprogram_id_ &&
                        OB_FAIL(dml_splicer.add_column("plsql_entry_subprogram_id", ash.plsql_entry_subprogram_id_))) {
              LOG_WARN("failed to add column plsql_entry_subprogram_id", KR(ret), K(ash));
            } else if (ash.plsql_entry_subprogram_name_[0] == '\0' &&
                       OB_FAIL(dml_splicer.add_column(true, "plsql_entry_subprogram_name"))) {
              LOG_WARN("failed to add column plsql_entry_subprogram_name", KR(ret), K(ash));
            } else if (OB_FAIL(ash.plsql_entry_subprogram_name_[0] != '\0'
                        && dml_splicer.add_column("plsql_entry_subprogram_name", ash.plsql_entry_subprogram_name_))) {
              LOG_WARN("failed to add column plsql_entry_subprogram_name", KR(ret), K(ash));
            } else if (OB_INVALID_ID == ash.plsql_object_id_ &&
                       OB_FAIL(dml_splicer.add_column(true, "plsql_object_id"))) {
              LOG_WARN("failed to add column plsql_object_id", KR(ret), K(ash));
            } else if (OB_INVALID_ID != ash.plsql_object_id_ &&
                        OB_FAIL(dml_splicer.add_column("plsql_object_id", ash.plsql_object_id_))) {
              LOG_WARN("failed to add column plsql_object_id", KR(ret), K(ash));
            } else if (OB_INVALID_ID == ash.plsql_subprogram_id_ &&
                       OB_FAIL(dml_splicer.add_column(true, "plsql_subprogram_id"))) {
              LOG_WARN("failed to add column plsql_subprogram_id", KR(ret), K(ash));
            } else if (OB_INVALID_ID != ash.plsql_subprogram_id_ &&
                        OB_FAIL(dml_splicer.add_column("plsql_subprogram_id", ash.plsql_subprogram_id_))) {
              LOG_WARN("failed to add column plsql_subprogram_id", KR(ret), K(ash));
            } else if (OB_FAIL(ash.plsql_subprogram_name_[0] == '\0'
                        && dml_splicer.add_column(true, "plsql_subprogram_name"))) {
              LOG_WARN("failed to add column plsql_subprogram_name", KR(ret), K(ash));
            } else if (ash.plsql_subprogram_name_[0] != '\0'  &&
                       OB_FAIL(dml_splicer.add_column("plsql_subprogram_name", ash.plsql_subprogram_name_))) {
              LOG_WARN("failed to add column plsql_subprogram_name", KR(ret), K(ash));
            } else if (OB_FAIL(dml_splicer.finish_row())) {
              LOG_WARN("failed to finish row", KR(ret));
            }
          }
        }
        if (OB_SUCC(ret) && dml_splicer.get_row_count() >= WR_INSERT_BATCH_SIZE) {
          collected_ash_row_count += dml_splicer.get_row_count();
          if (OB_FAIL(write_to_wr(dml_splicer, OB_WR_ACTIVE_SESSION_HISTORY_TNAME, tenant_id))) {
            LOG_WARN("failed to batch write to wr", KR(ret));
          }
        }
      }
      if (OB_SUCC(ret) && dml_splicer.get_row_count() > 0) {
        collected_ash_row_count += dml_splicer.get_row_count();
        if (OB_FAIL(write_to_wr(dml_splicer, OB_WR_ACTIVE_SESSION_HISTORY_TNAME, tenant_id))) {
          LOG_WARN("failed to batch write remaining part to wr", KR(ret));
        }
      }
    }
  }
  if (OB_LIKELY(collected_ash_row_count != 0)) {
    EVENT_ADD(WR_COLLECTED_ASH_ROW_COUNT, collected_ash_row_count);
  }
  return ret;
}

int ObWrCollector::collect_statname()
{
  int ret = OB_SUCCESS;
  int64_t start = ObTimeUtility::current_time();

  ObSqlString sql;
  int64_t expected_rows = 0;
  const uint64_t tenant_id = MTL_ID();
  int64_t cluster_id = ObServerConfig::get_instance().cluster_id;

  if (OB_FAIL(sql.assign_fmt("INSERT /*+ WORKLOAD_REPOSITORY */ IGNORE INTO %s "
                             "(TENANT_ID, CLUSTER_ID, STAT_ID, STAT_NAME) VALUES",
          OB_WR_STATNAME_TNAME))) {
    LOG_WARN("sql assign failed", K(ret));
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < ObStatEventIds::STAT_EVENT_SET_END; ++i) {
    if (i == ObStatEventIds::STAT_EVENT_ADD_END) {
      // the end of ADD stat event, do nothing
      continue;
    } else {
      int64_t stat_id = OB_STAT_EVENTS[i].stat_id_;
      ObString stat_name = ObString::make_string(OB_STAT_EVENTS[i].name_);
      expected_rows++;
      if (OB_FAIL(sql.append_fmt("%s('%lu', '%lu', '%ld', '%.*s')", (i == 0) ? " " : ", ",
              tenant_id, cluster_id, stat_id, stat_name.length(), stat_name.ptr()))) {
        LOG_WARN("sql append failed", K(ret));
      }
    }
  }  // end for

  if (OB_SUCC(ret)) {
    if (expected_rows > 0) {
      int64_t affected_rows = 0;
      uint64_t exec_tenant_id = gen_meta_tenant_id(tenant_id);
      if (OB_FAIL(GCTX.sql_proxy_->write(exec_tenant_id, sql.ptr(), affected_rows))) {
        LOG_WARN("execute sql failed", KR(ret), K(tenant_id), K(exec_tenant_id), K(sql));
      } else if (OB_UNLIKELY(affected_rows > 0)) {
        LOG_TRACE("the stat name has changed, there are new statname.", K(affected_rows));
      } else if (OB_UNLIKELY(affected_rows < 0)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected affected_rows", KR(ret), K(expected_rows), K(affected_rows));
      }
    }
  }
  LOG_TRACE(
      "init __wr_statname ", K(ret), K(tenant_id), "cost", ObTimeUtility::current_time() - start);
  return ret;
}


int ObWrCollector::collect_eventname()
{
  int ret = OB_SUCCESS;
  int64_t start = ObTimeUtility::current_time();

  ObSqlString sql;
  int64_t expected_rows = 0;
  const uint64_t tenant_id = MTL_ID();
  int64_t cluster_id = ObServerConfig::get_instance().cluster_id;

  if (OB_FAIL(sql.assign_fmt("INSERT /*+ WORKLOAD_REPOSITORY */ IGNORE INTO %s "
                             "(TENANT_ID, CLUSTER_ID, EVENT_ID, EVENT_NAME, PARAMETER1, PARAMETER2, PARAMETER3, WAIT_CLASS_ID, WAIT_CLASS) VALUES",
          OB_WR_EVENT_NAME_TNAME))) {
    LOG_WARN("sql assign failed", K(ret));
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < WAIT_EVENTS_TOTAL; ++i) {
    const int64_t event_id = OB_WAIT_EVENTS[i].event_id_;
    const int64_t wait_class_id = OB_WAIT_CLASSES[OB_WAIT_EVENTS[i].wait_class_].wait_class_id_;
    const ObString event_name = ObString::make_string(OB_WAIT_EVENTS[i].event_name_);
    const ObString parameter1 = ObString::make_string(OB_WAIT_EVENTS[i].param1_);
    const ObString parameter2 = ObString::make_string(OB_WAIT_EVENTS[i].param2_);
    const ObString parameter3 = ObString::make_string(OB_WAIT_EVENTS[i].param3_);
    const ObString wait_class =
        ObString::make_string(OB_WAIT_CLASSES[OB_WAIT_EVENTS[i].wait_class_].wait_class_);
    expected_rows++;
    if (OB_FAIL(
            sql.append_fmt("%s('%lu', '%lu', '%ld', '%.*s', '%.*s', '%.*s', '%.*s', '%lu', '%.*s')",
                (i == 0) ? " " : ", ", tenant_id, cluster_id, event_id, event_name.length(),
                event_name.ptr(), parameter1.length(), parameter1.ptr(), parameter2.length(),
                parameter2.ptr(), parameter3.length(), parameter3.ptr(), wait_class_id,
                wait_class.length(), wait_class.ptr()))) {
      LOG_WARN("sql append failed", K(ret));
    }
  }  // end for

  if (OB_SUCC(ret)) {
    if (expected_rows > 0) {
      int64_t affected_rows = 0;
      uint64_t exec_tenant_id = gen_meta_tenant_id(tenant_id);
      if (OB_FAIL(GCTX.sql_proxy_->write(exec_tenant_id, sql.ptr(), affected_rows))) {
        LOG_WARN("execute sql failed", KR(ret), K(tenant_id), K(exec_tenant_id), K(sql));
      } else if (OB_UNLIKELY(affected_rows > 0)) {
        LOG_TRACE("the event name has changed, there are new event name.", K(affected_rows));
      } else if (OB_UNLIKELY(affected_rows < 0)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected affected_rows", KR(ret), K(expected_rows), K(affected_rows));
      }
    }
  }
  LOG_TRACE(
      "init __wr_event_name", K(ret), K(tenant_id), "cost", ObTimeUtility::current_time() - start);
  return ret;
}

int ObWrCollector::collect_system_event()
{
  int ret = OB_SUCCESS;
  common::ObMySQLProxy *sql_proxy = GCTX.sql_proxy_;
  const uint64_t tenant_id = MTL_ID();
  int64_t cluster_id = ObServerConfig::get_instance().cluster_id;
  ObDMLSqlSplicer dml_splicer;
  ObSqlString sql;
  int64_t tmp_real_str_len = 0;
  int64_t query_timeout = timeout_ts_ - common::ObTimeUtility::current_time();
  SMART_VAR(ObISQLClient::ReadResult, res)
  {
    ObMySQLResult *result = nullptr;
    if (OB_UNLIKELY(query_timeout <= 0)) {
      ret = OB_TIMEOUT;
      LOG_WARN("wr snapshot timeout", KR(ret), K_(timeout_ts));
    } else if (OB_FAIL(sql.assign_fmt(
                   "SELECT /*+ WORKLOAD_REPOSITORY_SNAPSHOT QUERY_TIMEOUT(%ld) */ svr_ip, svr_port, event_id, total_waits, total_timeouts, time_waited_micro from "
                   "__all_virtual_system_event where tenant_id=%ld",
                   query_timeout, tenant_id))) {
      LOG_WARN("failed to assign sysstat query string", KR(ret));
    } else if (OB_FAIL(sql_proxy->read(res, tenant_id, sql.ptr()))) {
      LOG_WARN("failed to fetch sysstat", KR(ret), K(tenant_id), K(sql));
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("fail to get mysql result", KR(ret), K(tenant_id), K(sql));
    } else {
      while (OB_SUCC(ret)) {
        if (OB_FAIL(result->next())) {
          if (OB_ITER_END == ret) {
            ret = OB_SUCCESS;
            break;
          } else {
            LOG_WARN("fail to get next row", KR(ret));
          }
        } else {
          ObWrSystemEvent sysevent;
          EXTRACT_STRBUF_FIELD_MYSQL(
              *result, "svr_ip", sysevent.svr_ip_, OB_IP_STR_BUFF, tmp_real_str_len);
          EXTRACT_INT_FIELD_MYSQL(*result, "svr_port", sysevent.svr_port_, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "event_id", sysevent.event_id_, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "total_waits", sysevent.total_waits_, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "total_timeouts", sysevent.total_timeouts_, int64_t);
          EXTRACT_INT_FIELD_MYSQL(*result, "time_waited_micro", sysevent.time_waited_micro_, int64_t);
          if (OB_FAIL(ret)) {
          } else if (OB_UNLIKELY(sysevent.total_waits_ <= 0)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("wr system event handle invalid rows", KR(ret), K(sysevent));
          }
          if (OB_SUCC(ret)) {
            if (OB_FAIL(dml_splicer.add_pk_column(K(tenant_id)))) {
              LOG_WARN("failed to add tenant_id", KR(ret), K(tenant_id));
            } else if (OB_FAIL(dml_splicer.add_pk_column(K(cluster_id)))) {
              LOG_WARN("failed to add column cluster_id", KR(ret), K(cluster_id));
            } else if (OB_FAIL(dml_splicer.add_pk_column("SNAP_ID", snap_id_))) {
              LOG_WARN("failed to add column SNAP_ID", KR(ret), K(snap_id_));
            } else if (OB_FAIL(dml_splicer.add_pk_column("svr_ip", sysevent.svr_ip_))) {
              LOG_WARN("failed to add column svr_ip", KR(ret), K(sysevent));
            } else if (OB_FAIL(dml_splicer.add_pk_column("svr_port", sysevent.svr_port_))) {
              LOG_WARN("failed to add column svr_port", KR(ret), K(sysevent));
            } else if (OB_FAIL(dml_splicer.add_pk_column("event_id", sysevent.event_id_))) {
              LOG_WARN("failed to add column event_id", KR(ret), K(sysevent));
            } else if (OB_FAIL(dml_splicer.add_column("total_waits", sysevent.total_waits_))) {
              LOG_WARN("failed to add column total_waits", KR(ret), K(sysevent));
            } else if (OB_FAIL(dml_splicer.add_column("total_timeouts", sysevent.total_timeouts_))) {
              LOG_WARN("failed to add column total_timeouts", KR(ret), K(sysevent));
            } else if (OB_FAIL(dml_splicer.add_column("time_waited_micro", sysevent.time_waited_micro_))) {
              LOG_WARN("failed to add column time_waited_micro", KR(ret), K(sysevent));
            } else if (OB_FAIL(dml_splicer.finish_row())) {
              LOG_WARN("failed to finish row", KR(ret));
            }
          }
        }
        if (OB_SUCC(ret) && dml_splicer.get_row_count() >= WR_INSERT_BATCH_SIZE) {
          if (OB_FAIL(write_to_wr(dml_splicer, OB_WR_SYSTEM_EVENT_TNAME, tenant_id))) {
            LOG_WARN("failed to batch write to wr", KR(ret));
          }
        }
      }
      if (OB_SUCC(ret) && dml_splicer.get_row_count() > 0 &&
          OB_FAIL(write_to_wr(dml_splicer, OB_WR_SYSTEM_EVENT_TNAME, tenant_id))) {
        LOG_WARN("failed to batch write remaining part to wr", KR(ret));
      }
    }
  }
  return ret;
}

int ObWrCollector::collect_sqlstat()
{
  int ret = OB_SUCCESS;
  common::ObMySQLProxy *sql_proxy = GCTX.sql_proxy_;
  const uint64_t tenant_id = MTL_ID();
  int64_t cluster_id = ObServerConfig::get_instance().cluster_id;
  ObDMLSqlSplicer dml_splicer;
  ObSqlString sql;
  int64_t tmp_real_str_len = 0;
  int64_t collected_sqlstat_row_count = 0;
  int64_t topnsql = -1;
  {
    // read wr control`s top N SQL
    SMART_VAR(ObISQLClient::ReadResult, res)
    {
      ObMySQLResult *result = nullptr;
      int64_t query_timeout = timeout_ts_ - common::ObTimeUtility::current_time();
      if (OB_UNLIKELY(query_timeout <= 0)) {
        ret = OB_TIMEOUT;
        LOG_WARN("wr snapshot timeout", KR(ret), K_(timeout_ts));
      } else if (OB_FAIL(sql.assign_fmt(
                  "select /*+ workload_repository_snapshot query_timeout(%ld) */ topnsql "
                  "from oceanbase.DBA_WR_CONTROL", query_timeout))) {
        LOG_WARN("failed to assign ash query string", KR(ret));
      } else if (OB_FAIL(sql_proxy->read(res, OB_SYS_TENANT_ID, sql.ptr()))) {
        LOG_WARN("failed to fetch ash", KR(ret), K(OB_SYS_TENANT_ID), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get mysql result", KR(ret), K(OB_SYS_TENANT_ID), K(sql));
      } else {
        while (OB_SUCC(ret)) {
          if (OB_FAIL(result->next())) {
            if (OB_ITER_END == ret) {
              ret = OB_SUCCESS;
              break;
            } else {
              LOG_WARN("fail to get next row", KR(ret));
            }
          } else {
            EXTRACT_INT_FIELD_MYSQL(*result, "topnsql", topnsql , int64_t);
          }
        } // end of while
      }
    }
  }

  if (OB_SUCC(ret) && topnsql != -1) {
    SMART_VAR(ObISQLClient::ReadResult, res)
    {
      ObMySQLResult *result = nullptr;
      int64_t query_timeout = timeout_ts_ - common::ObTimeUtility::current_time();
      if (OB_UNLIKELY(query_timeout <= 0)) {
        ret = OB_TIMEOUT;
        LOG_WARN("wr snapshot timeout", KR(ret), K_(timeout_ts));
      } else if (OB_FAIL(sql.assign_fmt(
                  " select /*+ workload_repository_snapshot query_timeout(%ld) */ svr_ip,svr_port,tenant_id,     "
                  "    sql_id, plan_hash, plan_type, module, action, parsing_db_id,      "
                  "    parsing_db_name, parsing_user_id, cast(sum(executions_total) as SIGNED INTEGER ) as executions_total, "
                  "    cast(sum(executions_delta) as SIGNED INTEGER ) as executions_delta, cast(sum(disk_reads_total) as SIGNED INTEGER ) as disk_reads_total,     "
                  "    cast(sum(disk_reads_delta) as SIGNED INTEGER ) as disk_reads_delta, cast(sum(buffer_gets_total) as SIGNED INTEGER ) as buffer_gets_total, "
                  "    cast(sum(buffer_gets_delta) as SIGNED INTEGER ) as buffer_gets_delta, cast(sum(elapsed_time_total) as SIGNED INTEGER ) as elapsed_time_total, "
                  "    cast(sum(elapsed_time_delta) as SIGNED INTEGER ) as elapsed_time_delta, cast(sum(cpu_time_total) as SIGNED INTEGER ) as cpu_time_total, "
                  "    cast(sum(cpu_time_delta) as SIGNED INTEGER ) as cpu_time_delta, cast(sum(ccwait_total) as SIGNED INTEGER ) as ccwait_total, "
                  "    cast(sum(ccwait_delta) as SIGNED INTEGER ) as ccwait_delta, "
                  "    cast(sum(userio_wait_total) as SIGNED INTEGER ) as userio_wait_total, cast(sum(userio_wait_delta) as SIGNED INTEGER ) as userio_wait_delta, "
                  "    cast(sum(apwait_total) as SIGNED INTEGER ) as apwait_total, cast(sum(apwait_delta) as SIGNED INTEGER ) as apwait_delta, "
                  "    cast(sum(physical_read_requests_total) as SIGNED INTEGER ) as physical_read_requests_total, "
                  "    cast(sum(physical_read_requests_delta) as SIGNED INTEGER ) as physical_read_requests_delta, cast(sum(physical_read_bytes_total) as SIGNED INTEGER ) as physical_read_bytes_total, "
                  "    cast(sum(physical_read_bytes_delta) as SIGNED INTEGER ) as physical_read_bytes_delta, "
                  "    cast(sum(write_throttle_total) as SIGNED INTEGER ) as write_throttle_total, cast(sum(write_throttle_delta) as SIGNED INTEGER ) as write_throttle_delta, "
                  "    cast(sum(rows_processed_total) as SIGNED INTEGER ) as rows_processed_total, cast(sum(rows_processed_delta) as SIGNED INTEGER ) as rows_processed_delta, "
                  "    cast(sum(memstore_read_rows_total) as SIGNED INTEGER ) as memstore_read_rows_total, cast(sum(memstore_read_rows_delta) as SIGNED INTEGER ) as memstore_read_rows_delta, "
                  "    cast(sum(minor_ssstore_read_rows_total) as SIGNED INTEGER ) as minor_ssstore_read_rows_total, "
                  "    cast(sum(minor_ssstore_read_rows_delta) as SIGNED INTEGER ) as minor_ssstore_read_rows_delta, "
                  "    cast(sum(major_ssstore_read_rows_total) as SIGNED INTEGER ) as major_ssstore_read_rows_total, "
                  "    cast(sum(major_ssstore_read_rows_delta) as SIGNED INTEGER ) as major_ssstore_read_rows_delta, "
                  "    cast(sum(rpc_total) as SIGNED INTEGER ) as rpc_total, cast(sum(rpc_delta) as SIGNED INTEGER ) as rpc_delta, "
                  "    cast(sum(fetches_total) as SIGNED INTEGER ) as fetches_total, cast(sum(fetches_delta) as SIGNED INTEGER ) as fetches_delta, "
                  "    cast(sum(retry_total) as SIGNED INTEGER ) as retry_total, cast(sum(retry_delta) as SIGNED INTEGER ) as retry_delta, "
                  "    cast(sum(partition_total) as SIGNED INTEGER ) as partition_total, "
                  "    cast(sum(partition_delta) as SIGNED INTEGER ) as partition_delta, cast(sum(nested_sql_total) as SIGNED INTEGER ) as nested_sql_total, "
                  "    cast(sum(nested_sql_delta) as SIGNED INTEGER ) as nested_sql_delta ,source_ip, source_port , cast(sum(route_miss_total) as SIGNED INTEGER ) as route_miss_total, "
                  "    cast(sum(route_miss_delta) as SIGNED INTEGER ) as route_miss_delta "
                  "from oceanbase.__all_virtual_sqlstat   "
                  "where tenant_id = %ld and (sql_id,plan_hash) in  (   "
                  "select sql_id, plan_hash   "
                  "from     "
                  "    (  select  sql_id ,plan_hash,     "
                  "        row_number() over( order by sum(elapsed_time_delta) desc ) elapsed_time_delta_rank,     "
                  "        row_number() over( order by sum(cpu_time_delta) desc) cpu_time_delta_rank,     "
                  "        row_number() over( order by sum(userio_wait_delta) desc) userio_wait_delta_rank,     "
                  "        row_number() over( order by sum(physical_read_requests_delta) desc) physical_read_requests_delta_rank,     "
                  "        row_number() over( order by sum(executions_delta) desc) executions_delta_rank     "
                  "    from oceanbase.__all_virtual_sqlstat where tenant_id = %ld group by sql_id, plan_hash   "
                  "    )     "
                  "where     "
                  "    elapsed_time_delta_rank <= %ld     "
                  "    or cpu_time_delta_rank <= %ld     "
                  "    or userio_wait_delta_rank <= %ld       "
                  "    or physical_read_requests_delta_rank <= %ld       "
                  "    or executions_delta_rank <= %ld  ) "
                  "group by tenant_id, svr_ip, svr_port, sql_id, plan_hash, source_ip, source_port",
                  query_timeout, tenant_id, tenant_id, topnsql, topnsql, topnsql, topnsql, topnsql ))) {
        LOG_WARN("failed to assign ash query string", KR(ret));
      } else if (OB_FAIL(sql_proxy->read(res, tenant_id, sql.ptr()))) {
        LOG_WARN("failed to fetch sql stat", KR(ret), K(tenant_id), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get mysql result", KR(ret), K(tenant_id), K(sql));
      } else {
        const bool null_error = false;
        const bool skip_column_error = false;
        const int64_t default_value = -1;
        while (OB_SUCC(ret)) {
          if (OB_FAIL(result->next())) {
            if (OB_ITER_END == ret) {
              ret = OB_SUCCESS;
              break;
            } else {
              LOG_WARN("fail to get next row", KR(ret));
            }
          } else {
            ObWrSqlStat sqlstat;
            EXTRACT_STRBUF_FIELD_MYSQL(
                *result, "svr_ip", sqlstat.svr_ip_, sizeof(sqlstat.svr_ip_), tmp_real_str_len);
            EXTRACT_INT_FIELD_MYSQL(*result, "svr_port", sqlstat.svr_port_, int64_t);
            EXTRACT_STRBUF_FIELD_MYSQL(
                *result, "sql_id", sqlstat.sql_id_, sizeof(sqlstat.sql_id_), tmp_real_str_len);
            EXTRACT_UINT_FIELD_MYSQL(*result, "plan_hash", sqlstat.plan_hash_, uint64_t);
            EXTRACT_INT_FIELD_MYSQL(*result, "plan_type", sqlstat.plan_type_, int64_t);
            EXTRACT_STRBUF_FIELD_MYSQL_SKIP_RET(*result, "module", sqlstat.module_, sizeof(sqlstat.module_), tmp_real_str_len);
            EXTRACT_STRBUF_FIELD_MYSQL_SKIP_RET(*result, "action", sqlstat.action_, sizeof(sqlstat.action_), tmp_real_str_len);
            EXTRACT_INT_FIELD_MYSQL(*result, "parsing_db_id", sqlstat.parsing_db_id_, int64_t);
            EXTRACT_STRBUF_FIELD_MYSQL_SKIP_RET(*result, "parsing_db_name", sqlstat.parsing_db_name_, sizeof(sqlstat.parsing_db_name_), tmp_real_str_len);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "parsing_user_id", sqlstat.parsing_user_id_, int64_t, null_error, skip_column_error, default_value);

            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "executions_total", sqlstat.executions_total_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "executions_delta", sqlstat.executions_delta_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "disk_reads_total", sqlstat.disk_reads_total_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "disk_reads_delta", sqlstat.disk_reads_delta_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "buffer_gets_total", sqlstat.buffer_gets_total_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "buffer_gets_delta", sqlstat.buffer_gets_delta_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "elapsed_time_total", sqlstat.elapsed_time_total_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "elapsed_time_delta", sqlstat.elapsed_time_delta_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "cpu_time_total", sqlstat.cpu_time_total_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "cpu_time_delta", sqlstat.cpu_time_delta_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "ccwait_total", sqlstat.ccwait_total_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "ccwait_delta", sqlstat.ccwait_delta_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "userio_wait_total", sqlstat.userio_wait_total_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "userio_wait_delta", sqlstat.userio_wait_delta_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "apwait_total", sqlstat.apwait_total_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "apwait_delta", sqlstat.apwait_delta_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "physical_read_requests_total", sqlstat.physical_read_requests_total_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "physical_read_requests_delta", sqlstat.physical_read_requests_delta_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "physical_read_bytes_total", sqlstat.physical_read_bytes_total_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "physical_read_bytes_delta", sqlstat.physical_read_bytes_delta_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "write_throttle_total", sqlstat.write_throttle_total_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "write_throttle_delta", sqlstat.write_throttle_delta_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "rows_processed_total", sqlstat.rows_processed_total_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "rows_processed_delta", sqlstat.rows_processed_delta_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "memstore_read_rows_total", sqlstat.memstore_read_rows_total_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "memstore_read_rows_delta", sqlstat.memstore_read_rows_delta_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "minor_ssstore_read_rows_total", sqlstat.minor_ssstore_read_rows_total_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "minor_ssstore_read_rows_delta", sqlstat.minor_ssstore_read_rows_delta_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "major_ssstore_read_rows_total", sqlstat.major_ssstore_read_rows_total_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "major_ssstore_read_rows_delta", sqlstat.major_ssstore_read_rows_delta_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "rpc_total", sqlstat.rpc_total_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "rpc_delta", sqlstat.rpc_delta_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "fetches_total", sqlstat.fetches_total_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "fetches_delta", sqlstat.fetches_delta_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "retry_total", sqlstat.retry_total_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "retry_delta", sqlstat.retry_delta_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "partition_total", sqlstat.partition_total_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "partition_delta", sqlstat.partition_delta_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "nested_sql_total", sqlstat.nested_sql_total_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "nested_sql_delta", sqlstat.nested_sql_delta_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_STRBUF_FIELD_MYSQL_SKIP_RET(*result, "source_ip", sqlstat.source_ip_, sizeof(sqlstat.source_ip_), tmp_real_str_len);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "source_port", sqlstat.source_port_, int64_t, true/*skip_null_error*/, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "route_miss_total", sqlstat.route_miss_total_, int64_t, null_error, skip_column_error, default_value);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "route_miss_delta", sqlstat.route_miss_delta_, int64_t, null_error, skip_column_error, default_value);

            if (OB_SUCC(ret)) {
              if (OB_FAIL(dml_splicer.add_pk_column(K(tenant_id)))) {
                LOG_WARN("failed to add tenant_id", KR(ret), K(tenant_id));
              } else if (OB_FAIL(dml_splicer.add_pk_column(K(cluster_id)))) {
                LOG_WARN("failed to add column cluster_id", KR(ret), K(cluster_id));
              } else if (OB_FAIL(dml_splicer.add_pk_column("SNAP_ID", snap_id_))) {
                LOG_WARN("failed to add column SNAP_ID", KR(ret), K(snap_id_));
              } else if (OB_FAIL(dml_splicer.add_pk_column("svr_ip", sqlstat.svr_ip_))) {
                LOG_WARN("failed to add column svr_ip", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_pk_column("svr_port", sqlstat.svr_port_))) {
                LOG_WARN("failed to add column svr_port", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_pk_column("sql_id", sqlstat.sql_id_))) {
                LOG_WARN("failed to add column sql_id", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_pk_column("source_ip", sqlstat.source_ip_))) {
                LOG_WARN("failed to add column source_ip", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_pk_column("source_port", sqlstat.source_port_))) {
                LOG_WARN("failed to add column source_port", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_uint64_pk_column("plan_hash", sqlstat.plan_hash_))) {
                LOG_WARN("failed to add column plan_hash", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("plan_type", sqlstat.plan_type_))) {
                LOG_WARN("failed to add column plan_type", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column(true, "module"))) { // 没有设置为null
                LOG_WARN("failed to add column module", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column(true, "action"))) {
                LOG_WARN("failed to add column action", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("parsing_db_id", sqlstat.parsing_db_id_))) {
                LOG_WARN("failed to add column parsing_db_id", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("parsing_db_name", sqlstat.parsing_db_name_))) {
                LOG_WARN("failed to add column parsing_db_name", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("parsing_user_id", sqlstat.parsing_user_id_))) {
                LOG_WARN("failed to add column parsing_user_id", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("executions_total", sqlstat.executions_total_))) {
                LOG_WARN("failed to add column executions_total", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("executions_delta", sqlstat.executions_delta_))) {
                LOG_WARN("failed to add column executions_delta", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("disk_reads_total", sqlstat.disk_reads_total_))) {
                LOG_WARN("failed to add column disk_reads_total", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("disk_reads_delta", sqlstat.disk_reads_delta_))) {
                LOG_WARN("failed to add column disk_reads_delta", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("buffer_gets_total", sqlstat.buffer_gets_total_))) {
                LOG_WARN("failed to add column buffer_gets_total", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("buffer_gets_delta", sqlstat.buffer_gets_delta_))) {
                LOG_WARN("failed to add column buffer_gets_delta", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("elapsed_time_total", sqlstat.elapsed_time_total_))) {
                LOG_WARN("failed to add column elapsed_time_total", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("elapsed_time_delta", sqlstat.elapsed_time_delta_))) {
                LOG_WARN("failed to add column elapsed_time_delta", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("cpu_time_total", sqlstat.cpu_time_total_))) {
                LOG_WARN("failed to add column cpu_time_total", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("cpu_time_delta", sqlstat.cpu_time_delta_))) {
                LOG_WARN("failed to add column cpu_time_delta", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("ccwait_total", sqlstat.ccwait_total_))) {
                LOG_WARN("failed to add column ccwait_total", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("ccwait_delta", sqlstat.ccwait_delta_))) {
                LOG_WARN("failed to add column ccwait_delta", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("userio_wait_total", sqlstat.userio_wait_total_))) {
                LOG_WARN("failed to add column userio_wait_total", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("userio_wait_delta", sqlstat.userio_wait_delta_))) {
                LOG_WARN("failed to add column userio_wait_delta", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("apwait_total", sqlstat.apwait_total_))) {
                LOG_WARN("failed to add column apwait_total", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("apwait_delta", sqlstat.apwait_delta_))) {
                LOG_WARN("failed to add column apwait_delta", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("physical_read_requests_total", sqlstat.physical_read_requests_total_))) {
                LOG_WARN("failed to add column physical_read_requests_total", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("physical_read_requests_delta", sqlstat.physical_read_requests_delta_))) {
                LOG_WARN("failed to add column physical_read_requests_delta", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("physical_read_bytes_total", sqlstat.physical_read_bytes_total_))) {
                LOG_WARN("failed to add column physical_read_bytes_total", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("physical_read_bytes_delta", sqlstat.physical_read_bytes_delta_))) {
                LOG_WARN("failed to add column physical_read_bytes_delta", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("write_throttle_total", sqlstat.write_throttle_total_))) {
                LOG_WARN("failed to add column write_throttle_total", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("write_throttle_delta", sqlstat.write_throttle_delta_))) {
                LOG_WARN("failed to add column write_throttle_delta", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("rows_processed_total", sqlstat.rows_processed_total_))) {
                LOG_WARN("failed to add column rows_processed_total", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("rows_processed_delta", sqlstat.rows_processed_delta_))) {
                LOG_WARN("failed to add column rows_processed_delta", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("memstore_read_rows_total", sqlstat.memstore_read_rows_total_))) {
                LOG_WARN("failed to add column memstore_read_rows_total", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("memstore_read_rows_delta", sqlstat.memstore_read_rows_delta_))) {
                LOG_WARN("failed to add column memstore_read_rows_delta", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("minor_ssstore_read_rows_total", sqlstat.minor_ssstore_read_rows_total_))) {
                LOG_WARN("failed to add column minor_ssstore_read_rows_total", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("minor_ssstore_read_rows_delta", sqlstat.minor_ssstore_read_rows_delta_))) {
                LOG_WARN("failed to add column minor_ssstore_read_rows_delta", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("major_ssstore_read_rows_total", sqlstat.major_ssstore_read_rows_total_))) {
                LOG_WARN("failed to add column major_ssstore_read_rows_total", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("major_ssstore_read_rows_delta", sqlstat.major_ssstore_read_rows_delta_))) {
                LOG_WARN("failed to add column major_ssstore_read_rows_delta", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("rpc_total", sqlstat.rpc_total_))) {
                LOG_WARN("failed to add column rpc_total", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("rpc_delta", sqlstat.rpc_delta_))) {
                LOG_WARN("failed to add column rpc_delta", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("fetches_total", sqlstat.fetches_total_))) {
                LOG_WARN("failed to add column fetches_total", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("fetches_delta", sqlstat.fetches_delta_))) {
                LOG_WARN("failed to add column fetches_delta", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("retry_total", sqlstat.retry_total_))) {
                LOG_WARN("failed to add column retry_total", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("retry_delta", sqlstat.retry_delta_))) {
                LOG_WARN("failed to add column retry_delta", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("partition_total", sqlstat.partition_total_))) {
                LOG_WARN("failed to add column partition_total", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("partition_delta", sqlstat.partition_delta_))) {
                LOG_WARN("failed to add column partition_delta", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("nested_sql_total", sqlstat.nested_sql_total_))) {
                LOG_WARN("failed to add column nested_sql_total", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("nested_sql_delta", sqlstat.nested_sql_delta_))) {
                LOG_WARN("failed to add column nested_sql_delta", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("route_miss_total", sqlstat.route_miss_total_))) {
                LOG_WARN("failed to add column route_miss_total", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.add_column("route_miss_delta", sqlstat.route_miss_delta_))) {
                LOG_WARN("failed to add column route_miss_delta", KR(ret), K(sqlstat));
              } else if (OB_FAIL(dml_splicer.finish_row())) {
                LOG_WARN("failed to finish row", KR(ret));
              }

              if (OB_SUCC(ret) && dml_splicer.get_row_count() >= WR_INSERT_SQL_STAT_BATCH_SIZE) {
                collected_sqlstat_row_count += dml_splicer.get_row_count();
                if (OB_FAIL(write_to_wr(dml_splicer, OB_WR_SQLSTAT_TNAME, tenant_id))) {
                  LOG_WARN("failed to batch write to wr", KR(ret));
                }
              }
            }
          }
        } // end while
        if (OB_SUCC(ret) && dml_splicer.get_row_count() > 0) {
          collected_sqlstat_row_count += dml_splicer.get_row_count();
          if (OB_FAIL(write_to_wr(dml_splicer, OB_WR_SQLSTAT_TNAME, tenant_id))) {
            LOG_WARN("failed to batch write remaining part to wr", KR(ret));
          }
        }
      }
    } //end smart_var
  }
  return ret;
}


int ObWrCollector::update_sqlstat()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(GCTX.omt_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("omt is null", K(ret));
  } else {
    observer::ObReqTimeGuard req_timeinfo_guard;
    sql::ObPlanCache *plan_cache = NULL;
    plan_cache = MTL(ObPlanCache*);
    if (nullptr != plan_cache){
      ObUpdateSqlStatOp op;
      if (OB_FAIL(plan_cache->foreach_cache_obj(op))) {
        LOG_WARN("fail to traverse tmp sql stat map", K(ret));
      }
    } else {
      LOG_WARN("failed to get library cache", K(ret));
    }
  }
  return ret;
}

int ObWrCollector::collect_sqltext()
{
  int ret = OB_SUCCESS;
  common::ObMySQLProxy *sql_proxy = GCTX.sql_proxy_;
  const uint64_t tenant_id = MTL_ID();
  int64_t cluster_id = ObServerConfig::get_instance().cluster_id;
  ObDMLSqlSplicer dml_splicer;
  ObSqlString sql;
  int64_t tmp_real_str_len = 0;

  SMART_VAR(ObISQLClient::ReadResult, res)
    {
      ObMySQLResult *result = nullptr;
      int64_t query_timeout = timeout_ts_ - common::ObTimeUtility::current_time();
      if (OB_UNLIKELY(query_timeout <= 0)) {
        ret = OB_TIMEOUT;
        LOG_WARN("wr snapshot timeout", KR(ret), K_(timeout_ts));
      } else if (OB_FAIL(sql.assign_fmt(
                  "select /*+ workload_repository_snapshot query_timeout(%ld) */ "
                    " sql_id, query_sql , "
                    " sql_type from oceanbase.__all_virtual_sqlstat "
                    "where tenant_id = %ld and sql_id in (select distinct sql_id from oceanbase.__all_virtual_wr_sqlstat where "
                    "tenant_id = %ld and snap_id = %ld and query_sql <> '')  group by sql_id ",
                  query_timeout, tenant_id, tenant_id, snap_id_))) {
        LOG_WARN("failed to assign ash query string", KR(ret));
      } else if (OB_FAIL(sql_proxy->read(res, tenant_id, sql.ptr()))) {
        LOG_WARN("failed to fetch sql stat", KR(ret), K(tenant_id), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get mysql result", KR(ret), K(tenant_id), K(sql));
      } else {
        const bool skip_null_error = true;
        const bool skip_column_error = false;
        const int64_t default_value = -1;
        while (OB_SUCC(ret)) {
          if (OB_FAIL(result->next())) {
            if (OB_ITER_END == ret) {
              ret = OB_SUCCESS;
              break;
            } else {
              LOG_WARN("fail to get next row", KR(ret));
            }
          } else {
            ObWrSqlText sqltext;
            const bool skip_null_error = true;
            const bool skip_column_error = false;
            const int64_t default_value = 0;
            EXTRACT_STRBUF_FIELD_MYSQL(
                *result, "sql_id", sqltext.sql_id_, sizeof(sqltext.sql_id_), tmp_real_str_len);
            EXTRACT_STRBUF_FIELD_MYSQL_SKIP_RET_AND_TRUNCATION(
                *result, "query_sql", sqltext.query_sql_, sizeof(sqltext.query_sql_), tmp_real_str_len);
            EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "sql_type", sqltext.sql_type_, int64_t, skip_null_error, skip_column_error, default_value);


            if (OB_SUCC(ret)) {
              int tmp_ret = OB_SUCCESS;
              ObSqlString insert_sql;
              int64_t affected_rows = 0;
              uint64_t exec_tenant_id = gen_meta_tenant_id(tenant_id);
              query_timeout = timeout_ts_ - common::ObTimeUtility::current_time();
              ObHexEscapeSqlStr query_sql = ObHexEscapeSqlStr(ObString::make_string(sqltext.query_sql_));
              if (OB_UNLIKELY(query_timeout <= 0)) {
                ret = OB_TIMEOUT;
                LOG_WARN("wr snapshot timeout", KR(tmp_ret), K_(timeout_ts));
              } else if (OB_TMP_FAIL(insert_sql.assign_fmt(
                  "insert /*+ workload_repository_snapshot query_timeout(%ld) */ into %s "
                  "(TENANT_ID, CLUSTER_ID,SNAP_ID, SQL_ID, QUERY_SQL, SQL_TYPE) values "
                  "(%ld, %ld, %ld, '%s',\"%.*s\" , %ld)",
                  query_timeout, OB_WR_SQLTEXT_TNAME,
                  tenant_id, cluster_id, snap_id_, sqltext.sql_id_,
                  query_sql.str().length(), query_sql.str().ptr(), sqltext.sql_type_))) {
                LOG_WARN("failed to assign insert query string", KR(tmp_ret));
              } else if (OB_TMP_FAIL(GCTX.sql_proxy_->write(exec_tenant_id, insert_sql.ptr(), affected_rows))) {
                LOG_WARN("execute sql failed", KR(tmp_ret), K(tenant_id), K(exec_tenant_id), K(insert_sql));
              } else if (OB_UNLIKELY(affected_rows != 1)) {
                // ret = OB_ERR_UNEXPECTED;
                LOG_WARN("unexpected affected_rows", KR(tmp_ret), K(affected_rows), K(insert_sql));
              }
            }


          }
        } // end while
      }
    } //end smart_var
  return ret;
}

int ObWrCollector::collect_sql_plan()
{
  return OB_SUCCESS;
  // int ret = OB_SUCCESS;
  // common::ObMySQLProxy *sql_proxy = GCTX.sql_proxy_;
  // const uint64_t tenant_id = MTL_ID();
  // int64_t cluster_id = ObServerConfig::get_instance().cluster_id;
  // ObDMLSqlSplicer dml_splicer;
  // ObSqlString sql;
  // int64_t tmp_real_str_len = 0;
  // int64_t query_timeout = timeout_ts_ - common::ObTimeUtility::current_time();
  // SMART_VAR(ObISQLClient::ReadResult, res)
  // {
  //   ObMySQLResult *result = nullptr;
  //   if (OB_UNLIKELY(query_timeout <= 0)) {
  //     ret = OB_TIMEOUT;
  //     LOG_WARN("wr snapshot timeout", KR(ret), K_(timeout_ts));
  //   } else if (OB_FAIL(sql.assign_fmt(
  //                  "SELECT /*+ WORKLOAD_REPOSITORY_SNAPSHOT QUERY_TIMEOUT(%ld) */ svr_ip, svr_port, "
  //                  "sql_id, plan_hash, plan_id, id, operator, options, object_id, object_owner, "
  //                  "object_name, object_alias, object_type, parent_id, position, depth, cost ,cardinality, other_xml as other"
  //                  "__all_virtual_sql_plan where tenant_id=%ld",
  //                  query_timeout, tenant_id))) {
  //     LOG_WARN("failed to assign sysstat query string", KR(ret));
  //   } else if (OB_FAIL(sql_proxy->read(res, tenant_id, sql.ptr()))) {
  //     LOG_WARN("failed to fetch sysstat", KR(ret), K(tenant_id), K(sql));
  //   } else if (OB_ISNULL(result = res.get_result())) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("fail to get mysql result", KR(ret), K(tenant_id), K(sql));
  //   } else {
  //     const bool skip_null_error = true;
  //     const bool skip_column_error = false;
  //     const int64_t default_value = -1;
  //     while (OB_SUCC(ret)) {
  //       if (OB_FAIL(result->next())) {
  //         if (OB_ITER_END == ret) {
  //           ret = OB_SUCCESS;
  //           break;
  //         } else {
  //           LOG_WARN("fail to get next row", KR(ret));
  //         }
  //       } else {
  //         ObWrSqlPlan sqlplan;
  //         EXTRACT_STRBUF_FIELD_MYSQL(
  //             *result, "svr_ip", sqlplan.svr_ip_, OB_IP_STR_BUFF, tmp_real_str_len);
  //         EXTRACT_INT_FIELD_MYSQL(*result, "svr_port", sqlplan.svr_port_, int64_t);
  //         EXTRACT_STRBUF_FIELD_MYSQL_SKIP_RET(*result, "sql_id", sqlplan.sql_id_, sizeof(sqlplan.sql_id_), tmp_real_str_len);
  //         EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "plan_hash", sqlplan.plan_hash_, int64_t, skip_null_error, skip_column_error, default_value);
  //         EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "plan_id", sqlplan.plan_id_, int64_t, skip_null_error, skip_column_error, default_value);
  //         EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "id", sqlplan.id_, int64_t, skip_null_error, skip_column_error, default_value);
  //         EXTRACT_STRBUF_FIELD_MYSQL_SKIP_RET(*result, "operator", sqlplan.operator_, sizeof(sqlplan.operator_), tmp_real_str_len);
  //         EXTRACT_STRBUF_FIELD_MYSQL_SKIP_RET(*result, "option", sqlplan.option_, sizeof(sqlplan.option_), tmp_real_str_len);
  //         EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "object_id", sqlplan.object_id_, int64_t, skip_null_error, skip_column_error, default_value);
  //         EXTRACT_STRBUF_FIELD_MYSQL_SKIP_RET(*result, "object_owner", sqlplan.object_owner_, sizeof(sqlplan.object_owner_), tmp_real_str_len);
  //         EXTRACT_STRBUF_FIELD_MYSQL_SKIP_RET(*result, "object_name", sqlplan.object_name_, sizeof(sqlplan.object_name_), tmp_real_str_len);
  //         EXTRACT_STRBUF_FIELD_MYSQL_SKIP_RET(*result, "object_alias", sqlplan.object_alias_, sizeof(sqlplan.object_alias_), tmp_real_str_len);
  //         EXTRACT_STRBUF_FIELD_MYSQL_SKIP_RET(*result, "object_type", sqlplan.object_type_, sizeof(sqlplan.object_type_), tmp_real_str_len);
  //         EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "parent_id", sqlplan.parent_id_, int64_t, skip_null_error, skip_column_error, default_value);
  //         EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "position", sqlplan.position_, int64_t, skip_null_error, skip_column_error, default_value);
  //         EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "depth", sqlplan.depth_, int64_t, skip_null_error, skip_column_error, default_value);
  //         EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "cost", sqlplan.cost_, int64_t, skip_null_error, skip_column_error, default_value);
  //         EXTRACT_INT_FIELD_MYSQL_WITH_DEFAULT_VALUE(*result, "cardinality", sqlplan.cardinality_, int64_t, skip_null_error, skip_column_error, default_value);
  //         EXTRACT_STRBUF_FIELD_MYSQL_SKIP_RET(*result, "other", sqlplan.other_, sizeof(sqlplan.other_), tmp_real_str_len);

  //         if (OB_SUCC(ret)) {
  //           if (OB_FAIL(dml_splicer.add_pk_column(K(tenant_id)))) {
  //             LOG_WARN("failed to add tenant_id", KR(ret), K(tenant_id));
  //           } else if (OB_FAIL(dml_splicer.add_pk_column(K(cluster_id)))) {
  //             LOG_WARN("failed to add column cluster_id", KR(ret), K(cluster_id));
  //           } else if (OB_FAIL(dml_splicer.add_pk_column("SNAP_ID", snap_id_))) {
  //             LOG_WARN("failed to add column SNAP_ID", KR(ret), K(snap_id_));
  //           } else if (OB_FAIL(dml_splicer.add_pk_column("svr_ip", sqlplan.svr_ip_))) {
  //             LOG_WARN("failed to add column svr_ip", KR(ret), K(sqlplan));
  //           } else if (OB_FAIL(dml_splicer.add_pk_column("svr_port", sqlplan.svr_port_))) {
  //             LOG_WARN("failed to add column svr_port", KR(ret), K(sqlplan));
  //           } else if (OB_FAIL(dml_splicer.add_pk_column("plan_hash", sqlplan.plan_hash_))) {
  //             LOG_WARN("failed to add column plan_hash", KR(ret), K(sqlplan));
  //           } else if (OB_FAIL(dml_splicer.add_column("plan_id", sqlplan.plan_id_))) {
  //             LOG_WARN("failed to add column plan_id", KR(ret), K(sqlplan));
  //           } else if (OB_FAIL(dml_splicer.add_column("id", sqlplan.id_))) {
  //             LOG_WARN("failed to add column id", KR(ret), K(sqlplan));
  //           } else if (OB_FAIL(dml_splicer.add_column("operator", sqlplan.operator_))) {
  //             LOG_WARN("failed to add column operator", KR(ret), K(sqlplan));
  //           } else if (OB_FAIL(dml_splicer.add_column("option", sqlplan.option_))) {
  //             LOG_WARN("failed to add column option", KR(ret), K(sqlplan));
  //           } else if (OB_FAIL(dml_splicer.add_column("object_id", sqlplan.object_id_))) {
  //             LOG_WARN("failed to add column object_id", KR(ret), K(sqlplan));
  //           } else if (OB_FAIL(dml_splicer.add_column("object_owner", sqlplan.object_owner_))) {
  //             LOG_WARN("failed to add column object_owner", KR(ret), K(sqlplan));
  //           } else if (OB_FAIL(dml_splicer.add_column("object_name", sqlplan.object_name_))) {
  //             LOG_WARN("failed to add column object_name", KR(ret), K(sqlplan));
  //           } else if (OB_FAIL(dml_splicer.add_column("object_alias", sqlplan.object_alias_))) {
  //             LOG_WARN("failed to add column object_alias", KR(ret), K(sqlplan));
  //           } else if (OB_FAIL(dml_splicer.add_column("object_type", sqlplan.object_type_))) {
  //             LOG_WARN("failed to add column object_type", KR(ret), K(sqlplan));
  //           } else if (OB_FAIL(dml_splicer.add_column("parent_id", sqlplan.parent_id_))) {
  //             LOG_WARN("failed to add column parent_id", KR(ret), K(sqlplan));
  //           } else if (OB_FAIL(dml_splicer.add_column("position", sqlplan.position_))) {
  //             LOG_WARN("failed to add column position", KR(ret), K(sqlplan));
  //           } else if (OB_FAIL(dml_splicer.add_column("depth", sqlplan.depth_))) {
  //             LOG_WARN("failed to add column depth_", KR(ret), K(sqlplan));
  //           } else if (OB_FAIL(dml_splicer.add_column("cost", sqlplan.cost_))) {
  //             LOG_WARN("failed to add column cost", KR(ret), K(sqlplan));
  //           } else if (OB_FAIL(dml_splicer.add_column("cardinality", sqlplan.cardinality_))) {
  //             LOG_WARN("failed to add column cardinality", KR(ret), K(sqlplan));
  //           } else if (OB_FAIL(dml_splicer.add_column("other", ObHexEscapeSqlStr(ObString::make_string(sqlplan.other_))))) {
  //             LOG_WARN("failed to add column other", KR(ret), K(sqlplan));
  //           } else if (OB_FAIL(dml_splicer.finish_row())) {
  //             LOG_WARN("failed to finish row", KR(ret));
  //           }
  //         }
  //       }
  //       if (OB_SUCC(ret) && dml_splicer.get_row_count() >= WR_INSERT_BATCH_SIZE) {
  //         if (OB_FAIL(write_to_wr(dml_splicer, OB_WR_SQL_PLAN, tenant_id))) {
  //           LOG_WARN("failed to batch write to wr", KR(ret));
  //         }
  //       }
  //     }
  //     if (OB_SUCC(ret) && dml_splicer.get_row_count() > 0 &&
  //         OB_FAIL(write_to_wr(dml_splicer, OB_WR_SQL_PLAN, tenant_id))) {
  //       LOG_WARN("failed to batch write remaining part to wr", KR(ret));
  //     }
  //   }
  // }
  // return ret;
}

int ObWrCollector::write_to_wr(
    ObDMLSqlSplicer &dml_splicer, const char *table_name, int64_t tenant_id)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  int64_t cur_row = dml_splicer.get_row_count();
  if (OB_FAIL(dml_splicer.splice_batch_insert_sql(table_name, sql))) {
    LOG_WARN("failed to generate sql", KR(ret), K(tenant_id));
  } else if (OB_FAIL(
                 GCTX.sql_proxy_->write(gen_meta_tenant_id(tenant_id), sql.ptr(), affected_rows))) {
    LOG_WARN("failed to write wr data", KR(ret), K(sql), K(gen_meta_tenant_id(tenant_id)));
  } else if (affected_rows != cur_row) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid affected rows", KR(ret), K(affected_rows), K(cur_row), K(sql));
  } else {
    LOG_TRACE("execute wr batch insertion sql success", K(sql), K(affected_rows));
    dml_splicer.reset();
  }
  return ret;
}

int ObWrDeleter::do_delete()
{
  int ret = OB_SUCCESS;
  ObASHSetInnerSqlWaitGuard ash_inner_sql_guard(ObInnerSqlWaitTypeId::WR_DEL_SNAPSHOT);
  uint64_t tenant_id = MTL_ID();
  int64_t cluster_id = ObServerConfig::get_instance().cluster_id;
  int64_t query_timeout = 0;

  for (int64_t i = 0; OB_SUCC(ret) && i < purge_arg_.get_to_delete_snap_ids().count(); ++i) {
    int64_t snap_id = purge_arg_.get_to_delete_snap_ids().at(i);
    query_timeout = purge_arg_.get_timeout_ts() - ObTimeUtility::current_time();
    if (OB_UNLIKELY(query_timeout <= 0)) {
      ret = OB_TIMEOUT;
      LOG_WARN("delete data task timed out, stop task", K(ret), K(tenant_id), K(cluster_id),
          K(snap_id), K(purge_arg_.get_timeout_ts()));
      break;
    } else if (OB_FAIL(modify_snapshot_status(tenant_id, cluster_id, snap_id, query_timeout, ObWrSnapshotStatus::DELETED))) {
      LOG_WARN("failed to modify wr snapshot status to deleted", K(ret), K(tenant_id), K(cluster_id), K(snap_id));
    } else if (OB_FAIL(delete_expired_data_from_wr_table(
                   OB_WR_SYSSTAT_TNAME, tenant_id, cluster_id, snap_id, query_timeout))) {
      LOG_WARN(
          "failed to delete __wr_sysstat data ", K(ret), K(tenant_id), K(cluster_id), K(snap_id));
    } else if (OB_FAIL(delete_expired_data_from_wr_table(OB_WR_ACTIVE_SESSION_HISTORY_TNAME,
                   tenant_id, cluster_id, snap_id, query_timeout))) {
      LOG_WARN("failed to delete __wr_acitve_session_history data ", K(ret), K(tenant_id),
          K(cluster_id), K(snap_id));
    } else if (OB_FAIL(delete_expired_data_from_wr_table(
                   OB_WR_SNAPSHOT_TNAME, tenant_id, cluster_id, snap_id, query_timeout))) {
      LOG_WARN(
          "failed to delete __wr_snapshot data ", K(ret), K(tenant_id), K(cluster_id), K(snap_id));
    } else if (OB_FAIL(delete_expired_data_from_wr_table(
                   OB_WR_SYSTEM_EVENT_TNAME, tenant_id, cluster_id, snap_id, query_timeout))) {
      LOG_WARN(
          "failed to delete __wr_system_event data ", K(ret), K(tenant_id), K(cluster_id), K(snap_id));
    }
    if (OB_FAIL(ret)) {
      int tmp_ret = OB_SUCCESS;
      if (OB_TMP_FAIL(modify_snapshot_status(tenant_id, cluster_id, snap_id, query_timeout, ObWrSnapshotStatus::FAILED))) {
        LOG_WARN("failed to delete snapshot", KR(tmp_ret), K(tenant_id), K(cluster_id), K(snap_id));
      }
    }
  }  // end for
  return ret;
}

int ObWrDeleter::delete_expired_data_from_wr_table(const char *const table_name,
    const uint64_t tenant_id, const int64_t cluster_id, const int64_t snap_id,
    const int64_t query_timeout)
{
  int ret = OB_SUCCESS;
  int64_t start = ObTimeUtility::current_time();

  ObSqlString sql;
  int64_t affected_rows = 0;
  uint64_t meta_tenant_id = gen_meta_tenant_id(tenant_id);
  if (OB_UNLIKELY(query_timeout <= 0)) {
    ret = OB_TIMEOUT;
    LOG_WARN("timeout on wr data deletion", KR(ret), K(query_timeout));
  } else if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("sql_proxy_ nullptr", K(ret));
  } else if (OB_FAIL(sql.assign_fmt(
                 "DELETE /*+ WORKLOAD_REPOSITORY_PURGE QUERY_TIMEOUT(%ld) */ FROM %s WHERE ", query_timeout, table_name))) {
    LOG_WARN("sql assign failed", K(ret));
  } else if (OB_FAIL(sql.append_fmt("tenant_id='%lu' and cluster_id='%lu' and snap_id='%ld'",
                 tenant_id, cluster_id, snap_id))) {
    LOG_WARN("sql append failed", K(ret));
  } else if (OB_FAIL(GCTX.sql_proxy_->write(meta_tenant_id, sql.ptr(), affected_rows))) {
    LOG_WARN("execute sql failed", KR(ret), K(tenant_id), K(meta_tenant_id), K(sql));
  } else if (OB_UNLIKELY(affected_rows == 0)) {
    LOG_TRACE("the snapshot does not have any data.", K(snap_id), K(tenant_id), K(affected_rows),
        K(table_name));
  } else if (OB_UNLIKELY(affected_rows < 0)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected affected_rows", KR(ret), K(snap_id), K(tenant_id), K(affected_rows));
  }

  LOG_TRACE("delete wr table ", K(ret), K(table_name), K(tenant_id), K(snap_id), "time_consumed",
      ObTimeUtility::current_time() - start);
  return ret;
}

int ObWrDeleter::modify_snapshot_status(const uint64_t tenant_id, const int64_t cluster_id,
    const int64_t snap_id, const int64_t query_timeout, const ObWrSnapshotStatus status)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  uint64_t meta_tenant_id = gen_meta_tenant_id(tenant_id);
  if (OB_UNLIKELY(query_timeout <= 0)) {
    ret = OB_TIMEOUT;
    LOG_WARN("timeout on wr data deletion", KR(ret), K(query_timeout));
  } else if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("sql_proxy_ nullptr", K(ret));
  } else if (OB_FAIL(
                 sql.assign_fmt("UPDATE /*+ WORKLOAD_REPOSITORY_PURGE QUERY_TIMEOUT(%ld) */ %s set "
                                "status=%ld where tenant_id=%ld and cluster_id=%ld and snap_id=%ld",
                     query_timeout, OB_WR_SNAPSHOT_TNAME, status, tenant_id,
                     cluster_id, snap_id))) {
    LOG_WARN("sql assign failed", K(ret));
  } else if (OB_FAIL(GCTX.sql_proxy_->write(meta_tenant_id, sql.ptr(), affected_rows))) {
    LOG_WARN("execute sql failed", KR(ret), K(tenant_id), K(meta_tenant_id), K(sql));
  } else if (OB_UNLIKELY(affected_rows == 0)) {
    LOG_TRACE("the snapshot is already marked as deleted, execute delete process anyway", K(snap_id), K(tenant_id), K(affected_rows));
  } else if (OB_UNLIKELY(affected_rows < 0)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected affected_rows", KR(ret), K(snap_id), K(tenant_id), K(affected_rows));
  }
  return ret;
}


int ObUpdateSqlStatOp::operator()(common::hash::HashMapPair<sql::ObCacheObjID, sql::ObILibCacheObject *> &entry)
{
  int ret = common::OB_SUCCESS;
  if (OB_ISNULL(entry.second)) {
    ret = common::OB_ERR_UNEXPECTED;
    LOG_WARN("invalid argument", K(ret));
  } else if (entry.second->get_ns() == sql::ObLibCacheNameSpace::NS_SQLSTAT) {
    sql::ObSqlStatRecordObj *obj = nullptr;
    if (OB_ISNULL(entry.second)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected null obj", K(ret));
    } else if (OB_ISNULL(obj = static_cast<sql::ObSqlStatRecordObj *>(entry.second))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected null obj", K(ret));
    } else if (!obj->get_record_value()->get_key().is_valid()) {
      // do nothing
    } else if (OB_FAIL(obj->get_record_value()->update_last_snap_record_value())) {
      LOG_WARN("failed to update last snap reocrd value", K(ret));
    }
  } else if (entry.second->get_ns() == sql::ObLibCacheNameSpace::NS_CRSR) {
    ObPhysicalPlan *plan = nullptr;
    if (OB_ISNULL(entry.second)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected null obj", K(ret));
    } else if (OB_ISNULL(plan = static_cast<sql::ObPhysicalPlan *>(entry.second))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected null obj", K(ret));
    } else if (!plan->sql_stat_record_value_.get_key().is_valid()) {
      // do nothing
    } else if (OB_FAIL(plan->sql_stat_record_value_.update_last_snap_record_value())) {
      LOG_WARN("failed to update last snap reocrd value", K(ret));
    }
  }
  return ret;
}
}  // end of namespace share
}  // end of namespace oceanbase