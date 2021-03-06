// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "service/impala-server.h"

#include <algorithm>
#include <boost/algorithm/string/join.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/unordered_set.hpp>
#include <jni.h>
#include <thrift/protocol/TDebugProtocol.h>
#include <gtest/gtest.h>
#include <boost/foreach.hpp>
#include <boost/bind.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <google/heap-profiler.h>
#include <google/malloc_extension.h>

#include "codegen/llvm-codegen.h"
#include "common/logging.h"
#include "common/version.h"
#include "exec/ddl-executor.h"
#include "exec/exec-node.h"
#include "exec/hdfs-table-sink.h"
#include "exec/scan-node.h"
#include "exprs/expr.h"
#include "runtime/data-stream-mgr.h"
#include "runtime/client-cache.h"
#include "runtime/descriptors.h"
#include "runtime/data-stream-sender.h"
#include "runtime/row-batch.h"
#include "runtime/plan-fragment-executor.h"
#include "runtime/hdfs-fs-cache.h"
#include "runtime/exec-env.h"
#include "runtime/timestamp-value.h"
#include "statestore/simple-scheduler.h"
#include "util/bit-util.h"
#include "util/container-util.h"
#include "util/debug-util.h"
#include "util/impalad-metrics.h"
#include "util/jni-util.h"
#include "util/network-util.h"
#include "util/parse-util.h"
#include "util/string-parser.h"
#include "util/thrift-util.h"
#include "util/thrift-server.h"
#include "util/webserver.h"
#include "gen-cpp/Types_types.h"
#include "gen-cpp/ImpalaService.h"
#include "gen-cpp/DataSinks_types.h"
#include "gen-cpp/Types_types.h"
#include "gen-cpp/ImpalaService.h"
#include "gen-cpp/ImpalaService_types.h"
#include "gen-cpp/ImpalaInternalService.h"
#include "gen-cpp/ImpalaPlanService.h"
#include "gen-cpp/ImpalaPlanService_types.h"
#include "gen-cpp/Frontend_types.h"

using namespace std;
using namespace boost;
using namespace boost::algorithm;
using namespace apache::thrift;
using namespace apache::thrift::protocol;
using namespace apache::thrift::transport;
using namespace apache::thrift::server;
using namespace apache::thrift::concurrency;
using namespace apache::hive::service::cli::thrift;
using namespace beeswax;

DEFINE_bool(use_planservice, false, "Use external planservice if true");
DECLARE_string(planservice_host);
DECLARE_int32(planservice_port);
DECLARE_int32(be_port);
DECLARE_string(nn);
DECLARE_int32(nn_port);

DEFINE_int32(beeswax_port, 21000, "port on which Beeswax client requests are served");
DEFINE_int32(hs2_port, 21050, "port on which HiveServer2 client requests are served");

DEFINE_int32(fe_service_threads, 64,
    "number of threads available to serve client requests");
DEFINE_int32(be_service_threads, 64,
    "(Advanced) number of threads available to serve backend execution requests");
DEFINE_bool(load_catalog_at_startup, false, "if true, load all catalog data at startup");
DEFINE_string(default_query_options, "", "key=value pair of default query options for"
    " impalad, separated by ','");
DEFINE_int32(query_log_size, 25, "Number of queries to retain in the query log. If -1, "
                                 "the query log has unbounded size.");
DEFINE_bool(log_query_to_file, true, "if true, logs completed query profiles to file.");

// TODO: this logging should go into a per query log.
DEFINE_int32(log_mem_usage_interval, 0, "If non-zero, impalad will output memory usage "
    "every log_mem_usage_interval'th fragment completion.");
DEFINE_string(heap_profile_dir, "", "if non-empty, enable heap profiling and output "
    " to specified directory.");

DEFINE_bool(abort_on_config_error, true, "Abort Impala if there are improper configs.");

namespace impala {

ThreadManager* fe_tm;
ThreadManager* be_tm;

Status ImpalaServer::QueryExecState::Exec(TExecRequest* exec_request) {
  exec_request_ = *exec_request;
  profile_.set_name("Query (id=" + PrintId(exec_request->request_id) + ")");
  query_id_ = exec_request->request_id;

  if (exec_request->stmt_type == TStmtType::QUERY ||
      exec_request->stmt_type == TStmtType::DML) {
    DCHECK(exec_request_.__isset.query_exec_request);
    TQueryExecRequest& query_exec_request = exec_request_.query_exec_request;

    // we always need at least one plan fragment
    DCHECK_GT(query_exec_request.fragments.size(), 0);

    // If desc_tbl is not set, query has SELECT with no FROM. In that
    // case, the query can only have a single fragment, and that fragment needs to be
    // executed by the coordinator. This check confirms that.
    // If desc_tbl is set, the query may or may not have a coordinator fragment.
    bool has_coordinator_fragment =
        query_exec_request.fragments[0].partition.type == TPartitionType::UNPARTITIONED;
    DCHECK(has_coordinator_fragment || query_exec_request.__isset.desc_tbl);
    bool has_from_clause = query_exec_request.__isset.desc_tbl;

    if (!has_from_clause) {
      // query without a FROM clause: only one fragment, and it doesn't have a plan
      DCHECK(!query_exec_request.fragments[0].__isset.plan);
      DCHECK_EQ(query_exec_request.fragments.size(), 1);
      // TODO: This does not handle INSERT INTO ... SELECT 1 because there is no
      // coordinator fragment actually executing to drive the sink.
      // Workaround: INSERT INTO ... SELECT 1 FROM tbl LIMIT 1
      local_runtime_state_.reset(new RuntimeState(
          exec_request->request_id, exec_request->query_options,
          query_exec_request.query_globals.now_string,
          NULL /* = we don't expect to be executing anything here */));
      RETURN_IF_ERROR(PrepareSelectListExprs(local_runtime_state_.get(),
          query_exec_request.fragments[0].output_exprs, RowDescriptor()));
    } else {
      // If the first fragment has a "limit 0", simply set EOS to true and return.
      // TODO: Remove this check if this is an INSERT. To be compatible with
      // Hive, OVERWRITE inserts must clear out target tables / static
      // partitions even if no rows are written.
      DCHECK(query_exec_request.fragments[0].__isset.plan);
      if (query_exec_request.fragments[0].plan.nodes[0].limit == 0) {
        eos_ = true;
        return Status::OK;
      }

      coord_.reset(new Coordinator(exec_env_));
      RETURN_IF_ERROR(coord_->Exec(
          exec_request->request_id, &query_exec_request, exec_request->query_options));

      if (has_coordinator_fragment) {
        RETURN_IF_ERROR(PrepareSelectListExprs(coord_->runtime_state(),
            query_exec_request.fragments[0].output_exprs, coord_->row_desc()));
      }

      profile_.AddChild(&summary_info_);
      summary_info_.AddInfoString("Sql Statement", exec_request->sql_stmt);
      summary_info_.AddInfoString("Start Time", start_time().DebugString());
      summary_info_.AddInfoString("Query Type", PrintTStmtType(stmt_type()));
      summary_info_.AddInfoString("Query State", PrintQueryState(query_state_));
      summary_info_.AddInfoString("Impala Version", GetVersionString());
      summary_info_.AddInfoString("User", user());
      summary_info_.AddInfoString("Default Db", default_db());
      if (query_exec_request.__isset.query_plan) {
        stringstream plan_ss;
        // Add some delimiters to make it clearer where the plan
        // begins and the profile ends
        plan_ss << "\n----------------\n"
                << query_exec_request.query_plan
                << "----------------";
        summary_info_.AddInfoString("Plan", plan_ss.str());
      }
      profile_.AddChild(coord_->query_profile());
    }
  } else {
    ddl_executor_.reset(new DdlExecutor(impala_server_));
    RETURN_IF_ERROR(ddl_executor_->Exec(&exec_request_.ddl_exec_request));
  }

  return Status::OK;
}

void ImpalaServer::QueryExecState::Done() {
  end_time_ = TimestampValue::local_time();
  summary_info_.AddInfoString("End Time", end_time().DebugString());
  summary_info_.AddInfoString("Query State", PrintQueryState(query_state_));
}

Status ImpalaServer::QueryExecState::Exec(const TMetadataOpRequest& exec_request) {
  ddl_executor_.reset(new DdlExecutor(impala_server_));
  RETURN_IF_ERROR(ddl_executor_->Exec(exec_request));
  query_id_ = ddl_executor_->request_id();
  VLOG_QUERY << "query_id:" << query_id_.hi << ":" << query_id_.lo;
  result_metadata_ = ddl_executor_->result_set_metadata();
  return Status::OK;
}

Status ImpalaServer::QueryExecState::Wait() {
  if (coord_.get() != NULL) {
    RETURN_IF_ERROR(coord_->Wait());
    RETURN_IF_ERROR(UpdateMetastore());
  }

  return Status::OK;
}

Status ImpalaServer::QueryExecState::FetchRows(const int32_t max_rows,
    QueryResultSet* fetched_rows) {
  query_status_ = FetchRowsInternal(max_rows, fetched_rows);
  if (!query_status_.ok()) {
    query_state_ = QueryState::EXCEPTION;
  }
  return query_status_;
}

void ImpalaServer::QueryExecState::UpdateQueryState(QueryState::type query_state) {
  lock_guard<mutex> l(lock_);
  if (query_state_ < query_state) query_state_ = query_state;
}

void ImpalaServer::QueryExecState::SetErrorStatus(const Status& status) {
  DCHECK(!status.ok());
  lock_guard<mutex> l(lock_);
  query_state_ = QueryState::EXCEPTION;
  query_status_ = status;
}

Status ImpalaServer::QueryExecState::FetchRowsInternal(const int32_t max_rows,
    QueryResultSet* fetched_rows) {
  DCHECK(query_state_ != QueryState::EXCEPTION);

  if (eos_) return Status::OK;

  // List of expr values to hold evaluated rows from the query
  vector<void*> result_row;
  result_row.resize(output_exprs_.size());

  if (coord_ == NULL && ddl_executor_ == NULL) {
    query_state_ = QueryState::FINISHED;  // results will be ready after this call
    // query without FROM clause: we return exactly one row
    eos_ = true;
    RETURN_IF_ERROR(GetRowValue(NULL, &result_row));
    return fetched_rows->AddOneRow(result_row);
  } else {
    if (coord_ != NULL) {
      RETURN_IF_ERROR(coord_->Wait());
    }
    query_state_ = QueryState::FINISHED;  // results will be ready after this call
    if (coord_ != NULL) {
      // Fetch the next batch if we've returned the current batch entirely
      if (current_batch_ == NULL || current_batch_row_ >= current_batch_->num_rows()) {
        RETURN_IF_ERROR(FetchNextBatch());
      }
      if (current_batch_ == NULL) return Status::OK;

      // Convert the available rows, limited by max_rows
      int available = current_batch_->num_rows() - current_batch_row_;
      int fetched_count = available;
      // max_rows <= 0 means no limit
      if (max_rows > 0 && max_rows < available) fetched_count = max_rows;
      for (int i = 0; i < fetched_count; ++i) {
        TupleRow* row = current_batch_->GetRow(current_batch_row_);
        RETURN_IF_ERROR(GetRowValue(row, &result_row));
        RETURN_IF_ERROR(fetched_rows->AddOneRow(result_row));
        ++num_rows_fetched_;
        ++current_batch_row_;
      }
    } else {
      DCHECK(ddl_executor_.get());
      int num_rows = 0;
      const vector<TResultRow>& all_rows = ddl_executor_->result_set();
      // If max_rows < 0, there's no maximum on the number of rows to return
      while ((num_rows < max_rows || max_rows < 0)
          && num_rows_fetched_ < all_rows.size()) {
        fetched_rows->AddOneRow(all_rows[num_rows_fetched_]);
        ++num_rows_fetched_;
        ++num_rows;
      }
      eos_ = (num_rows_fetched_ == all_rows.size());
      return Status::OK;
    }
  }
  return Status::OK;
}

Status ImpalaServer::QueryExecState::GetRowValue(TupleRow* row, vector<void*>* result) {
  DCHECK(result->size() >= output_exprs_.size());
  for (int i = 0; i < output_exprs_.size(); ++i) {
    (*result)[i] = output_exprs_[i]->GetValue(row);
  }
  return Status::OK;
}

void ImpalaServer::QueryExecState::Cancel() {
  // If the query is completed, no need to cancel.
  if (eos_) return;
  // we don't want multiple concurrent cancel calls to end up executing
  // Coordinator::Cancel() multiple times
  if (query_state_ == QueryState::EXCEPTION) return;
  query_state_ = QueryState::EXCEPTION;
  if (coord_.get() != NULL) coord_->Cancel();
}

Status ImpalaServer::QueryExecState::PrepareSelectListExprs(
    RuntimeState* runtime_state,
    const vector<TExpr>& exprs, const RowDescriptor& row_desc) {
  RETURN_IF_ERROR(
      Expr::CreateExprTrees(runtime_state->obj_pool(), exprs, &output_exprs_));
  for (int i = 0; i < output_exprs_.size(); ++i) {
    // Don't codegen these, they are unused anyway.
    // TODO: codegen this and the write values path
    RETURN_IF_ERROR(Expr::Prepare(output_exprs_[i], runtime_state, row_desc, true));
  }
  return Status::OK;
}

Status ImpalaServer::QueryExecState::UpdateMetastore() {
  if (stmt_type() != TStmtType::DML) return Status::OK;


  DCHECK(exec_request().__isset.query_exec_request);
  TQueryExecRequest query_exec_request = exec_request().query_exec_request;
  if (!query_exec_request.__isset.finalize_params) return Status::OK;

  TFinalizeParams& finalize_params = query_exec_request.finalize_params;
  // TODO: Doesn't handle INSERT with no FROM
  if (coord() == NULL) {
    stringstream ss;
    ss << "INSERT with no FROM clause not fully supported, not updating metastore"
       << " (query_id: " << query_id() << ")";
    VLOG_QUERY << ss.str();
    return Status(ss.str());
  }

  TCatalogUpdate catalog_update;
  if (!coord()->PrepareCatalogUpdate(&catalog_update)) {
    VLOG_QUERY << "No partitions altered, not updating metastore (query id: "
               << query_id() << ")";
  } else {
    // TODO: We track partitions written to, not created, which means
    // that we do more work than is necessary, because written-to
    // partitions don't always require a metastore change.
    VLOG_QUERY << "Updating metastore with " << catalog_update.created_partitions.size()
               << " altered partitions ("
               << join (catalog_update.created_partitions, ", ") << ")";

    catalog_update.target_table = finalize_params.table_name;
    catalog_update.db_name = finalize_params.table_db;
    RETURN_IF_ERROR(impala_server_->UpdateMetastore(catalog_update));
  }

  // TODO: Reset only the updated table
  return impala_server_->ResetCatalogInternal();
}

Status ImpalaServer::QueryExecState::FetchNextBatch() {
  DCHECK(!eos_);
  DCHECK(coord_.get() != NULL);

  RETURN_IF_ERROR(coord_->GetNext(&current_batch_, coord_->runtime_state()));
  current_batch_row_ = 0;
  eos_ = current_batch_ == NULL;
  return Status::OK;
}

// Execution state of a single plan fragment.
class ImpalaServer::FragmentExecState {
 public:
  FragmentExecState(const TUniqueId& query_id, int backend_num,
                    const TUniqueId& fragment_instance_id, ExecEnv* exec_env,
                    const TNetworkAddress& coord_hostport)
    : query_id_(query_id),
      backend_num_(backend_num),
      fragment_instance_id_(fragment_instance_id),
      executor_(exec_env,
          bind<void>(mem_fn(&ImpalaServer::FragmentExecState::ReportStatusCb),
                     this, _1, _2, _3)),
      client_cache_(exec_env->client_cache()),
      coord_hostport_(coord_hostport) {
  }

  // Calling the d'tor releases all memory and closes all data streams
  // held by executor_.
  ~FragmentExecState() {
  }

  // Returns current execution status, if there was an error. Otherwise cancels
  // the fragment and returns OK.
  Status Cancel();

  // Call Prepare() and create and initialize data sink.
  Status Prepare(const TExecPlanFragmentParams& exec_params);

  // Main loop of plan fragment execution. Blocks until execution finishes.
  void Exec();

  const TUniqueId& query_id() const { return query_id_; }
  const TUniqueId& fragment_instance_id() const { return fragment_instance_id_; }

  void set_exec_thread(thread* exec_thread) { exec_thread_.reset(exec_thread); }

 private:
  TUniqueId query_id_;
  int backend_num_;
  TUniqueId fragment_instance_id_;
  PlanFragmentExecutor executor_;
  ImpalaInternalServiceClientCache* client_cache_;
  TExecPlanFragmentParams exec_params_;

  // initiating coordinator to which we occasionally need to report back
  // (it's exported ImpalaInternalService)
  const TNetworkAddress coord_hostport_;

  // the thread executing this plan fragment
  scoped_ptr<thread> exec_thread_;

  // protects exec_status_
  mutex status_lock_;

  // set in ReportStatusCb();
  // if set to anything other than OK, execution has terminated w/ an error
  Status exec_status_;

  // Callback for executor; updates exec_status_ if 'status' indicates an error
  // or if there was a thrift error.
  void ReportStatusCb(const Status& status, RuntimeProfile* profile, bool done);

  // Update exec_status_ w/ status, if the former isn't already an error.
  // Returns current exec_status_.
  Status UpdateStatus(const Status& status);
};

Status ImpalaServer::FragmentExecState::UpdateStatus(const Status& status) {
  lock_guard<mutex> l(status_lock_);
  if (!status.ok() && exec_status_.ok()) exec_status_ = status;
  return exec_status_;
}

Status ImpalaServer::FragmentExecState::Cancel() {
  lock_guard<mutex> l(status_lock_);
  RETURN_IF_ERROR(exec_status_);
  executor_.Cancel();
  return Status::OK;
}

Status ImpalaServer::FragmentExecState::Prepare(
    const TExecPlanFragmentParams& exec_params) {
  exec_params_ = exec_params;
  RETURN_IF_ERROR(executor_.Prepare(exec_params));
  return Status::OK;
}

void ImpalaServer::FragmentExecState::Exec() {
  // Open() does the full execution, because all plan fragments have sinks
  executor_.Open();
}

// There can only be one of these callbacks in-flight at any moment, because
// it is only invoked from the executor's reporting thread.
// Also, the reported status will always reflect the most recent execution status,
// including the final status when execution finishes.
void ImpalaServer::FragmentExecState::ReportStatusCb(
    const Status& status, RuntimeProfile* profile, bool done) {
  DCHECK(status.ok() || done);  // if !status.ok() => done
  Status exec_status = UpdateStatus(status);

  Status coord_status;
  ImpalaInternalServiceConnection coord(client_cache_, coord_hostport_, &coord_status);
  if (!coord_status.ok()) {
    stringstream s;
    s << "couldn't get a client for " << coord_hostport_;
    UpdateStatus(Status(TStatusCode::INTERNAL_ERROR, s.str()));
    return;
  }

  TReportExecStatusParams params;
  params.protocol_version = ImpalaInternalServiceVersion::V1;
  params.__set_query_id(query_id_);
  params.__set_backend_num(backend_num_);
  params.__set_fragment_instance_id(fragment_instance_id_);
  exec_status.SetTStatus(&params);
  params.__set_done(done);
  profile->ToThrift(&params.profile);
  params.__isset.profile = true;

  RuntimeState* runtime_state = executor_.runtime_state();
  DCHECK(runtime_state != NULL);
  // Only send updates to insert status if fragment is finished, the coordinator
  // waits until query execution is done to use them anyhow.
  if (done) {
    TInsertExecStatus insert_status;

    if (runtime_state->hdfs_files_to_move()->size() > 0) {
      insert_status.__set_files_to_move(*runtime_state->hdfs_files_to_move());
    }
    if (executor_.runtime_state()->num_appended_rows()->size() > 0) {
      insert_status.__set_num_appended_rows(
          *executor_.runtime_state()->num_appended_rows());
    }

    params.__set_insert_exec_status(insert_status);
  }

  // Send new errors to coordinator
  runtime_state->GetUnreportedErrors(&(params.error_log));
  params.__isset.error_log = (params.error_log.size() > 0);

  TReportExecStatusResult res;
  Status rpc_status;
  try {
    try {
      coord->ReportExecStatus(res, params);
    } catch (TTransportException& e) {
      VLOG_RPC << "Retrying ReportExecStatus: " << e.what();
      rpc_status = coord.Reopen();
      if (!rpc_status.ok()) {
        // we need to cancel the execution of this fragment
        UpdateStatus(rpc_status);
        executor_.Cancel();
        return;
      }
      coord->ReportExecStatus(res, params);
    }
    rpc_status = Status(res.status);
  } catch (TException& e) {
    stringstream msg;
    msg << "ReportExecStatus() to " << coord_hostport_ << " failed:\n" << e.what();
    VLOG_QUERY << msg.str();
    rpc_status = Status(TStatusCode::INTERNAL_ERROR, msg.str());
  }

  if (!rpc_status.ok()) {
    // we need to cancel the execution of this fragment
    UpdateStatus(rpc_status);
    executor_.Cancel();
  }
}

const char* ImpalaServer::SQLSTATE_SYNTAX_ERROR_OR_ACCESS_VIOLATION = "42000";
const char* ImpalaServer::SQLSTATE_GENERAL_ERROR = "HY000";
const char* ImpalaServer::SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED = "HYC00";
const int ImpalaServer::ASCII_PRECISION = 16; // print 16 digits for double/float

ImpalaServer::ImpalaServer(ExecEnv* exec_env)
    : exec_env_(exec_env) {
  // Initialize default config
  InitializeConfigVariables();

#ifndef ADDRESS_SANITIZER
  // tcmalloc and address sanitizer can not be used together
  if (!FLAGS_heap_profile_dir.empty()) {
    HeapProfilerStart(FLAGS_heap_profile_dir.c_str());
  }
#endif

  if (!FLAGS_use_planservice) {
    JNIEnv* jni_env = getJNIEnv();
    // create instance of java class JniFrontend
    jclass fe_class = jni_env->FindClass("com/cloudera/impala/service/JniFrontend");
    jmethodID fe_ctor = jni_env->GetMethodID(fe_class, "<init>", "(Z)V");
    EXIT_IF_EXC(jni_env);
    create_exec_request_id_ =
        jni_env->GetMethodID(fe_class, "createExecRequest", "([B)[B");
    EXIT_IF_EXC(jni_env);
    get_explain_plan_id_ =
        jni_env->GetMethodID(fe_class, "getExplainPlan", "([B)Ljava/lang/String;");
    EXIT_IF_EXC(jni_env);
    reset_catalog_id_ = jni_env->GetMethodID(fe_class, "resetCatalog", "()V");
    EXIT_IF_EXC(jni_env);
    get_hadoop_config_id_ =
        jni_env->GetMethodID(fe_class, "getHadoopConfigAsHtml", "()Ljava/lang/String;");
    EXIT_IF_EXC(jni_env);
    get_hadoop_config_value_id_ =
        jni_env->GetMethodID(fe_class, "getHadoopConfigValue",
          "(Ljava/lang/String;)Ljava/lang/String;");
    EXIT_IF_EXC(jni_env);
    check_hadoop_config_id_ = jni_env->GetMethodID(fe_class, "checkHadoopConfig",
       "()Ljava/lang/String;");
    EXIT_IF_EXC(jni_env);
    update_metastore_id_ = jni_env->GetMethodID(fe_class, "updateMetastore", "([B)V");
    EXIT_IF_EXC(jni_env);
    get_table_names_id_ = jni_env->GetMethodID(fe_class, "getTableNames", "([B)[B");
    EXIT_IF_EXC(jni_env);
    describe_table_id_ = jni_env->GetMethodID(fe_class, "describeTable", "([B)[B");
    EXIT_IF_EXC(jni_env);
    get_db_names_id_ = jni_env->GetMethodID(fe_class, "getDbNames", "([B)[B");
    EXIT_IF_EXC(jni_env);
    exec_hs2_metadata_op_id_ =
        jni_env->GetMethodID(fe_class, "execHiveServer2MetadataOp", "([B)[B");
    EXIT_IF_EXC(jni_env);
    alter_table_id_ = jni_env->GetMethodID(fe_class, "alterTable", "([B)V");
    EXIT_IF_EXC(jni_env);
    create_table_id_ = jni_env->GetMethodID(fe_class, "createTable", "([B)V");
    EXIT_IF_EXC(jni_env);
    create_table_like_id_ = jni_env->GetMethodID(fe_class, "createTableLike", "([B)V");
    EXIT_IF_EXC(jni_env);
    create_database_id_ = jni_env->GetMethodID(fe_class, "createDatabase", "([B)V");
    EXIT_IF_EXC(jni_env);
    drop_table_id_ = jni_env->GetMethodID(fe_class, "dropTable", "([B)V");
    EXIT_IF_EXC(jni_env);
    drop_database_id_ = jni_env->GetMethodID(fe_class, "dropDatabase", "([B)V");
    EXIT_IF_EXC(jni_env);

    jboolean lazy = (FLAGS_load_catalog_at_startup ? false : true);
    jobject fe = jni_env->NewObject(fe_class, fe_ctor, lazy);
    EXIT_IF_EXC(jni_env);
    EXIT_IF_ERROR(JniUtil::LocalToGlobalRef(jni_env, fe, &fe_));

    Status status = ValidateSettings();
    if (!status.ok()) {
      LOG(ERROR) << status.GetErrorMsg();
      if (FLAGS_abort_on_config_error) {
        LOG(ERROR) << "Impala is aborted due to improper configurations.";
        exit(1);
      }
    }
  } else {
    planservice_socket_.reset(new TSocket(FLAGS_planservice_host,
        FLAGS_planservice_port));
    planservice_transport_.reset(new TBufferedTransport(planservice_socket_));
    planservice_protocol_.reset(new TBinaryProtocol(planservice_transport_));
    planservice_client_.reset(new ImpalaPlanServiceClient(planservice_protocol_));
    planservice_transport_->open();
  }

  Webserver::PathHandlerCallback varz_callback =
      bind<void>(mem_fn(&ImpalaServer::RenderHadoopConfigs), this, _1, _2);
  exec_env->webserver()->RegisterPathHandler("/varz", varz_callback);

  Webserver::PathHandlerCallback query_callback =
      bind<void>(mem_fn(&ImpalaServer::QueryStatePathHandler), this, _1, _2);
  exec_env->webserver()->RegisterPathHandler("/queries", query_callback);

  Webserver::PathHandlerCallback sessions_callback =
      bind<void>(mem_fn(&ImpalaServer::SessionPathHandler), this, _1, _2);
  exec_env->webserver()->RegisterPathHandler("/sessions", sessions_callback);

  Webserver::PathHandlerCallback catalog_callback =
      bind<void>(mem_fn(&ImpalaServer::CatalogPathHandler), this, _1, _2);
  exec_env->webserver()->RegisterPathHandler("/catalog", catalog_callback);

  Webserver::PathHandlerCallback backends_callback =
      bind<void>(mem_fn(&ImpalaServer::BackendsPathHandler), this, _1, _2);
  exec_env->webserver()->RegisterPathHandler("/backends", backends_callback);

  Webserver::PathHandlerCallback profile_callback =
      bind<void>(mem_fn(&ImpalaServer::QueryProfilePathHandler), this, _1, _2);
  exec_env->webserver()->
      RegisterPathHandler("/query_profile", profile_callback, true, false);

  Webserver::PathHandlerCallback profile_encoded_callback =
      bind<void>(mem_fn(&ImpalaServer::QueryProfileEncodedPathHandler), this, _1, _2);
  exec_env->webserver()->RegisterPathHandler("/query_profile_encoded",
      profile_encoded_callback, false, false);
  
  Webserver::PathHandlerCallback inflight_query_ids_callback =
      bind<void>(mem_fn(&ImpalaServer::InflightQueryIdsPathHandler), this, _1, _2);
  exec_env->webserver()->RegisterPathHandler("/inflight_query_ids",
      inflight_query_ids_callback, false, false);

  // Initialize impalad metrics
  ImpaladMetrics::CreateMetrics(exec_env->metrics());
  ImpaladMetrics::IMPALA_SERVER_START_TIME->Update(
      TimestampValue::local_time().DebugString());

  // Register the membership callback if required
  if (exec_env->subscriber() != NULL) {
    StateStoreSubscriber::UpdateCallback cb =
        bind<void>(mem_fn(&ImpalaServer::MembershipCallback), this, _1, _2);
    exec_env->subscriber()->AddTopic(SimpleScheduler::IMPALA_MEMBERSHIP_TOPIC, true, cb);
  }
}

void ImpalaServer::RenderHadoopConfigs(const Webserver::ArgumentMap& args,
    stringstream* output) {
  (*output) << "<h2>Hadoop Configuration</h2>";
  if (FLAGS_use_planservice) {
    (*output) << "Using external PlanService, no Hadoop configs available";
    return;
  }
  JNIEnv* jni_env = getJNIEnv();
  jstring java_html_string =
      static_cast<jstring>(jni_env->CallObjectMethod(fe_, get_hadoop_config_id_));
  RETURN_IF_EXC(jni_env);
  jboolean is_copy;
  const char *str = jni_env->GetStringUTFChars(java_html_string, &is_copy);
  RETURN_IF_EXC(jni_env);
  (*output) << str;
  jni_env->ReleaseStringUTFChars(java_html_string, str);
  RETURN_IF_EXC(jni_env);
}

Status ImpalaServer::GetHadoopConfigValue(const string& key, string* output) {
  if (FLAGS_use_planservice) {
    return Status("Using external PlanService, no Hadoop configs available");
  }
  JNIEnv* jni_env = getJNIEnv();
  jstring value_arg = jni_env->NewStringUTF(key.c_str());
  jstring java_config_value = static_cast<jstring>(
      jni_env->CallObjectMethod(fe_, get_hadoop_config_value_id_, value_arg));
  RETURN_ERROR_IF_EXC(jni_env, JniUtil::throwable_to_string_id());
  const char *str = jni_env->GetStringUTFChars(java_config_value, NULL);
  RETURN_ERROR_IF_EXC(jni_env, JniUtil::throwable_to_string_id());
  *output = str;
  jni_env->ReleaseStringUTFChars(java_config_value, str);
  RETURN_ERROR_IF_EXC(jni_env, JniUtil::throwable_to_string_id());

  return Status::OK;
}

// We expect the query id to be passed as one parameter, 'query_id'.
// Returns true if the query id was present and valid; false otherwise.
static bool ParseQueryId(const Webserver::ArgumentMap& args, TUniqueId* id) {
  Webserver::ArgumentMap::const_iterator it = args.find("query_id");
  if (it == args.end()) {
    return false;
  } else {
    return ParseId(it->second, id);
  }
}

void ImpalaServer::QueryProfilePathHandler(const Webserver::ArgumentMap& args,
    stringstream* output) {
  TUniqueId unique_id;
  if (!ParseQueryId(args, &unique_id)) {
    (*output) << "Invalid query id";
    return;
  }

  (*output) << "<pre>";
  Status status = GetRuntimeProfileStr(unique_id, false, output);
  if (!status.ok()) {
    (*output) << status.GetErrorMsg();
  }
  (*output) << "</pre>";
}

void ImpalaServer::QueryProfileEncodedPathHandler(const Webserver::ArgumentMap& args,
    stringstream* output) {
  TUniqueId unique_id;
  if (!ParseQueryId(args, &unique_id)) {
    (*output) << "Invalid query id";
    return;
  }

  Status status = GetRuntimeProfileStr(unique_id, true, output);
  if (!status.ok()) {
    (*output) << status.GetErrorMsg();
  }
}

void ImpalaServer::InflightQueryIdsPathHandler(const Webserver::ArgumentMap& args,
    stringstream* output) {
  lock_guard<mutex> l(query_exec_state_map_lock_);
  BOOST_FOREACH(const QueryExecStateMap::value_type& exec_state, query_exec_state_map_) {
    *output << exec_state.second->query_id() << "\n";
  }
}

Status ImpalaServer::GetRuntimeProfileStr(const TUniqueId& query_id,
    bool base64_encoded, stringstream* output) {
  DCHECK(output != NULL);
  // Search for the query id in the active query map
  {
    lock_guard<mutex> l(query_exec_state_map_lock_);
    QueryExecStateMap::const_iterator exec_state = query_exec_state_map_.find(query_id);
    if (exec_state != query_exec_state_map_.end()) {
      if (base64_encoded) {
        exec_state->second->profile().SerializeToArchiveString(output);
      } else {
        exec_state->second->profile().PrettyPrint(output);
      }
      return Status::OK;
    }
  }

  // The query was not found the active query map, search the query log.
  {
    lock_guard<mutex> l(query_log_lock_);
    QueryLogIndex::const_iterator query_record = query_log_index_.find(query_id);
    if (query_record == query_log_index_.end()) {
      stringstream ss;
      ss << "Query id " << PrintId(query_id) << " not found.";
      return Status(ss.str());
    }
    if (base64_encoded) {
      (*output) << query_record->second->encoded_profile_str;
    } else {
      (*output) << query_record->second->profile_str;
    }
  }
  return Status::OK;
}

void ImpalaServer::RenderSingleQueryTableRow(const ImpalaServer::QueryStateRecord& record,
    bool render_end_time, stringstream* output) {
  (*output) << "<tr>"
            << "<td>" << record.user << "</td>"
            << "<td>" << record.default_db << "</td>"
            << "<td>" << record.stmt << "</td>"
            << "<td>"
            << _TStmtType_VALUES_TO_NAMES.find(record.stmt_type)->second
            << "</td>";

  // Output start/end times
  (*output) << "<td>" << record.start_time.DebugString() << "</td>";
  if (render_end_time) {
    (*output) << "<td>" << record.end_time.DebugString() << "</td>";
  }

  // Output progress
  (*output) << "<td>";
  if (record.has_coord == false) {
    (*output) << "N/A";
  } else {
    (*output) << record.num_complete_fragments << " / " << record.total_fragments
              << " (" << setw(4);
    if (record.total_fragments == 0) {
      (*output) << " (0%)";
    } else {
      (*output) <<
          (100.0 * record.num_complete_fragments / (1.f * record.total_fragments))
                << "%)";
    }
  }

  // Output state and rows fetched
  (*output) << "</td>"
            << "<td>" << _QueryState_VALUES_TO_NAMES.find(record.query_state)->second
            << "</td><td>" << record.num_rows_fetched << "</td>";

  // Output profile
  (*output) << "<td><a href='/query_profile?query_id=" << record.id
            << "'>Profile</a></td>";
  (*output) << "</tr>" << endl;
}

void ImpalaServer::QueryStatePathHandler(const Webserver::ArgumentMap& args,
    stringstream* output) {
  (*output) << "<h2>Queries</h2>";
  lock_guard<mutex> l(query_exec_state_map_lock_);
  (*output) << "This page lists all registered queries, i.e., those that are not closed "
    " nor cancelled.<br/>" << endl;
  (*output) << query_exec_state_map_.size() << " queries in flight" << endl;
  (*output) << "<table class='table table-hover table-border'><tr>"
            << "<th>User</th>" << endl
            << "<th>Default Db</th>" << endl
            << "<th>Statement</th>" << endl
            << "<th>Query Type</th>" << endl
            << "<th>Start Time</th>" << endl
            << "<th>Backend Progress</th>" << endl
            << "<th>State</th>" << endl
            << "<th># rows fetched</th>" << endl
            << "<th>Profile</th>" << endl
            << "</tr>";
  BOOST_FOREACH(const QueryExecStateMap::value_type& exec_state, query_exec_state_map_) {
    QueryStateRecord record(*exec_state.second);
    RenderSingleQueryTableRow(record, false, output);
  }

  (*output) << "</table>";

  // Print the query location counts.
  (*output) << "<h2>Query Locations</h2>";
  (*output) << "<table class='table table-hover table-bordered'>";
  (*output) << "<tr><th>Location</th><th>Number of Fragments</th></tr>" << endl;
  {
    lock_guard<mutex> l(query_locations_lock_);
    BOOST_FOREACH(const QueryLocations::value_type& location, query_locations_) {
      (*output) << "<tr><td>" << location.first << "<td><b>" << location.second.size()
		<< "</b></td></tr>";
    }
  }
  (*output) << "</table>";

  // Print the query log
  (*output) << "<h2>Finished Queries</h2>";
  (*output) << "<table class='table table-hover table-border'><tr>"
            << "<th>User</th>" << endl
            << "<th>Default Db</th>" << endl
            << "<th>Statement</th>" << endl
            << "<th>Query Type</th>" << endl
            << "<th>Start Time</th>" << endl
            << "<th>End Time</th>" << endl
            << "<th>Backend Progress</th>" << endl
            << "<th>State</th>" << endl
            << "<th># rows fetched</th>" << endl
            << "<th>Profile</th>" << endl
            << "</tr>";

  {
    lock_guard<mutex> l(query_log_lock_);
    BOOST_FOREACH(const QueryStateRecord& log_entry, query_log_) {
      RenderSingleQueryTableRow(log_entry, true, output);
    }
  }

  (*output) << "</table>";
}

void ImpalaServer::SessionPathHandler(const Webserver::ArgumentMap& args,
    stringstream* output) {
  (*output) << "<h2>Sessions</h2>" << endl;
  lock_guard<mutex> l(session_state_map_lock_);
  (*output) << "There are " << session_state_map_.size() << " active sessions." << endl
            << "<table class='table table-bordered table-hover'>"
            << "<tr><th>Session Type</th>"
            << "<th>User</th>"
            << "<th>Session Key</th>"
            << "<th>Default Database</th>"
            << "<th>Start Time</th></tr>"
            << endl;
  BOOST_FOREACH(const SessionStateMap::value_type& session, session_state_map_) {
    string session_type;
    string session_key;
    if (session.second->session_type == BEESWAX) {
      session_type = "Beeswax";
      session_key = session.first;
    } else {
      session_type = "HiveServer2";
      // Print HiveServer2 session key as TUniqueId
      stringstream result;
      TUniqueId tmp_key;
      memcpy(&(tmp_key.hi), session.first.c_str(), 8);
      memcpy(&(tmp_key.lo), session.first.c_str() + 8, 8);
      result << tmp_key.hi << ":" << tmp_key.lo;
      session_key = result.str();
    }
    (*output) << "<tr>"
              << "<td>" << session_type << "</td>"
              << "<td>" << session.second->user << "</td>"
              << "<td>" << session_key << "</td>"
              << "<td>" << session.second->database << "</td>"
              << "<td>" << session.second->start_time.DebugString() << "</td>"
              << "</tr>";
  }
  (*output) << "</table>";
}

void ImpalaServer::CatalogPathHandler(const Webserver::ArgumentMap& args,
    stringstream* output) {
  (*output) << "<h2>Catalog</h2>" << endl;
  TGetDbsResult get_dbs_result;
  Status status = GetDbNames(NULL, &get_dbs_result);
  vector<string>& db_names = get_dbs_result.dbs;

  if (!status.ok()) {
    (*output) << "Error: " << status.GetErrorMsg();
    return;
  }

  // Build a navigation string like [ default | tpch | ... ]
  vector<string> links;
  BOOST_FOREACH(const string& db, db_names) {
    stringstream ss;
    ss << "<a href='#" << db << "'>" << db << "</a>";
    links.push_back(ss.str());
  }
  (*output) << "[ " <<  join(links, " | ") << " ] ";

  BOOST_FOREACH(const string& db, db_names) {
    (*output) << "<a id='" << db << "'><h3>" << db << "</h3></a>";
    TGetTablesResult get_table_results;
    Status status = GetTableNames(&db, NULL, &get_table_results);
    vector<string>& table_names = get_table_results.tables;
    if (!status.ok()) {
      (*output) << "Error: " << status.GetErrorMsg();
      continue;
    }

    (*output) << "<p>" << db << " contains <b>" << table_names.size()
              << "</b> tables</p>";

    (*output) << "<ul>" << endl;
    BOOST_FOREACH(const string& table, table_names) {
      (*output) << "<li>" << table << "</li>" << endl;
    }
    (*output) << "</ul>" << endl;
  }
}

void ImpalaServer::BackendsPathHandler(const Webserver::ArgumentMap& args,
    stringstream* output) {
  Scheduler::HostList backends;
  exec_env_->scheduler()->GetAllKnownHosts(&backends);
  sort(backends.begin(), backends.end(), TNetworkAddressComparator);
  (*output) << "<h2>Known Backends "
            << "(" << backends.size() << ")"
            << "</h2>";

  (*output) << "<pre>";
  BOOST_FOREACH(const Scheduler::HostList::value_type& host, backends) {
    (*output) << host << endl;
  }
  (*output) << "</pre>";
}

void ImpalaServer::ArchiveQuery(const QueryExecState& query) {
  string encoded_profile_str;

  if (FLAGS_log_query_to_file) {
    // GLOG chops logs off past the max log len.  Output the profile to the log
    // in parts.
    // TODO: is there a better way?
    int max_output_len = google::LogMessage::kMaxLogMessageLen - 1000;
    DCHECK_GT(max_output_len, 1000);
    encoded_profile_str = query.profile().SerializeToArchiveString();
    int num_lines = BitUtil::Ceil(encoded_profile_str.size(), max_output_len);
    int encoded_str_len = encoded_profile_str.size();
    for (int i = 0; i < num_lines; ++i) {
      int len = ::min(max_output_len, encoded_str_len - i * max_output_len);
      LOG(INFO) << "Query " << query.query_id() << " finished ("
                << (i + 1) << "/" << num_lines << ") "
                << string(encoded_profile_str.c_str() + i * max_output_len, len);
    }
  }

  if (FLAGS_query_log_size == 0) return;
  QueryStateRecord record(query, true, encoded_profile_str);
  {
    lock_guard<mutex> l(query_log_lock_);
    // Add record to the beginning of the log, and to the lookup index.
    query_log_index_[query.query_id()] = query_log_.insert(query_log_.begin(), record);

    if (FLAGS_query_log_size > -1 && FLAGS_query_log_size < query_log_.size()) {
      DCHECK_EQ(query_log_.size() - FLAGS_query_log_size, 1);
      query_log_index_.erase(query_log_.back().id);
      query_log_.pop_back();
    }
  }
}

ImpalaServer::~ImpalaServer() {}

Status ImpalaServer::Execute(const TClientRequest& request,
    shared_ptr<SessionState> session_state,
    const TSessionState& query_session_state,
    shared_ptr<QueryExecState>* exec_state) {
  bool registered_exec_state;
  ImpaladMetrics::IMPALA_SERVER_NUM_QUERIES->Increment(1L);
  Status status = ExecuteInternal(request, session_state, query_session_state,
      &registered_exec_state, exec_state);
  if (!status.ok() && registered_exec_state) {
    UnregisterQuery((*exec_state)->query_id());
  }
  return status;
}

Status ImpalaServer::ExecuteInternal(
    const TClientRequest& request,
    shared_ptr<SessionState> session_state,
    const TSessionState& query_session_state,
    bool* registered_exec_state,
    shared_ptr<QueryExecState>* exec_state) {
  DCHECK(session_state != NULL);

  exec_state->reset(
      new QueryExecState(exec_env_, this, session_state, query_session_state));
  *registered_exec_state = false;

  TExecRequest result;
  {
    SCOPED_TIMER((*exec_state)->planner_timer());
    RETURN_IF_ERROR(GetExecRequest(request, &result));
  }

  if (result.stmt_type == TStmtType::DDL &&
      result.ddl_exec_request.ddl_type == TDdlType::USE) {
    {
      lock_guard<mutex> l(session_state->lock);
      session_state->database = result.ddl_exec_request.use_db_params.db;
    }
    exec_state->reset();
    return Status::OK;
  }

  if (result.__isset.result_set_metadata) {
    (*exec_state)->set_result_metadata(result.result_set_metadata);
  }

  // register exec state before starting execution in order to handle incoming
  // status reports
  RETURN_IF_ERROR(RegisterQuery(session_state, result.request_id, *exec_state));
  *registered_exec_state = true;

  // start execution of query; also starts fragment status reports
  RETURN_IF_ERROR((*exec_state)->Exec(&result));

  if ((*exec_state)->coord() != NULL) {
    const unordered_set<TNetworkAddress>& unique_hosts =
        (*exec_state)->coord()->unique_hosts();
    if (!unique_hosts.empty()) {
      lock_guard<mutex> l(query_locations_lock_);
      BOOST_FOREACH(const TNetworkAddress& port, unique_hosts) {
        query_locations_[port].insert((*exec_state)->query_id());
      }
    }
  }

  return Status::OK;
}

Status ImpalaServer::RegisterQuery(shared_ptr<SessionState> session_state,
    const TUniqueId& query_id, const shared_ptr<QueryExecState>& exec_state) {
  lock_guard<mutex> l2(session_state->lock);
  if (session_state->closed) {
    return Status("Session has been closed, ignoring query.");
  }

  lock_guard<mutex> l(query_exec_state_map_lock_);

  QueryExecStateMap::iterator entry = query_exec_state_map_.find(query_id);
  if (entry != query_exec_state_map_.end()) {
    // There shouldn't be an active query with that same id.
    // (query_id is globally unique)
    stringstream ss;
    ss << "query id " << PrintId(query_id) << " already exists";
    return Status(TStatusCode::INTERNAL_ERROR, ss.str());
  }

  session_state->inflight_queries.insert(query_id);
  query_exec_state_map_.insert(make_pair(query_id, exec_state));
  return Status::OK;
}

bool ImpalaServer::UnregisterQuery(const TUniqueId& query_id) {
  VLOG_QUERY << "UnregisterQuery(): query_id=" << query_id;

  // Cancel the query if it's still running
  CancelInternal(query_id);

  shared_ptr<QueryExecState> exec_state;
  {
    lock_guard<mutex> l(query_exec_state_map_lock_);
    QueryExecStateMap::iterator entry = query_exec_state_map_.find(query_id);
    if (entry == query_exec_state_map_.end()) {
      VLOG_QUERY << "unknown query id: " << PrintId(query_id);
      return false;
    } else {
      exec_state = entry->second;
    }
    query_exec_state_map_.erase(entry);
  }

  exec_state->Done();

  {
    lock_guard<mutex> l(exec_state->parent_session()->lock);
    exec_state->parent_session()->inflight_queries.erase(query_id);
  }

  ArchiveQuery(*exec_state);

  if (exec_state->coord() != NULL) {
    const unordered_set<TNetworkAddress>& unique_hosts =
        exec_state->coord()->unique_hosts();
    if (!unique_hosts.empty()) {
      lock_guard<mutex> l(query_locations_lock_);
      BOOST_FOREACH(const TNetworkAddress& hostport, unique_hosts) {
        // Query may have been removed already by cancellation path. In
        // particular, if node to fail was last sender to an exchange, the
        // coordinator will realise and fail the query at the same time the
        // failure detection path does the same thing. They will harmlessly race
        // to remove the query from this map.
        QueryLocations::iterator it = query_locations_.find(hostport);
        if (it != query_locations_.end()) {
          it->second.erase(exec_state->query_id());
        }
      }
    }
  }

  return true;
}

void ImpalaServer::Wait(boost::shared_ptr<QueryExecState> exec_state) {
  // block until results are ready
  Status status = exec_state->Wait();
  if (status.ok()) {
    exec_state->UpdateQueryState(QueryState::FINISHED);
  } else {
    exec_state->SetErrorStatus(status);
  }
}

Status ImpalaServer::UpdateMetastore(const TCatalogUpdate& catalog_update) {
  VLOG_QUERY << "UpdateMetastore()";
  if (!FLAGS_use_planservice) {
    JNIEnv* jni_env = getJNIEnv();
    jbyteArray request_bytes;
    RETURN_IF_ERROR(SerializeThriftMsg(jni_env, &catalog_update, &request_bytes));
    jni_env->CallObjectMethod(fe_, update_metastore_id_, request_bytes);
    RETURN_ERROR_IF_EXC(jni_env, JniUtil::throwable_to_string_id());
  } else {
    try {
      planservice_client_->UpdateMetastore(catalog_update);
    } catch (TException& e) {
      return Status(e.what());
    }
  }

  return Status::OK;
}

Status ImpalaServer::AlterTable(const TAlterTableParams& params) {
  if (FLAGS_use_planservice) {
    return Status("AlterTable not supported with external planservice");
  }
  JNIEnv* jni_env = getJNIEnv();
  jbyteArray request_bytes;
  RETURN_IF_ERROR(SerializeThriftMsg(jni_env, &params, &request_bytes));
  jni_env->CallObjectMethod(fe_, alter_table_id_, request_bytes);
  RETURN_ERROR_IF_EXC(jni_env, JniUtil::throwable_to_string_id());
  return Status::OK;
}

Status ImpalaServer::CreateDatabase(const TCreateDbParams& params) {
  if (FLAGS_use_planservice) {
   return Status("CreateDatabase not supported with external planservice");
  }
  JNIEnv* jni_env = getJNIEnv();
  jbyteArray request_bytes;
  RETURN_IF_ERROR(SerializeThriftMsg(jni_env, &params, &request_bytes));
  jni_env->CallObjectMethod(fe_, create_database_id_, request_bytes);
  RETURN_ERROR_IF_EXC(jni_env, JniUtil::throwable_to_string_id());
  return Status::OK;
}

Status ImpalaServer::CreateTableLike(const TCreateTableLikeParams& params) {
  if (FLAGS_use_planservice) {
    return Status("CreateTableLike not supported with external planservice");
  }
  JNIEnv* jni_env = getJNIEnv();
  jbyteArray request_bytes;
  RETURN_IF_ERROR(SerializeThriftMsg(jni_env, &params, &request_bytes));
  jni_env->CallObjectMethod(fe_, create_table_like_id_, request_bytes);
  RETURN_ERROR_IF_EXC(jni_env, JniUtil::throwable_to_string_id());
  return Status::OK;
}

Status ImpalaServer::CreateTable(const TCreateTableParams& params) {
  if (FLAGS_use_planservice) {
    return Status("CreateTable not supported with external planservice");
  }
  JNIEnv* jni_env = getJNIEnv();
  jbyteArray request_bytes;
  RETURN_IF_ERROR(SerializeThriftMsg(jni_env, &params, &request_bytes));
  jni_env->CallObjectMethod(fe_, create_table_id_, request_bytes);
  RETURN_ERROR_IF_EXC(jni_env, JniUtil::throwable_to_string_id());
  return Status::OK;
}

Status ImpalaServer::DropDatabase(const TDropDbParams& params) {
  if (FLAGS_use_planservice) {
    return Status("DropDatabase not supported with external planservice");
  }
  JNIEnv* jni_env = getJNIEnv();
  jbyteArray request_bytes;
  RETURN_IF_ERROR(SerializeThriftMsg(jni_env, &params, &request_bytes));
  jni_env->CallObjectMethod(fe_, drop_database_id_, request_bytes);
  RETURN_ERROR_IF_EXC(jni_env, JniUtil::throwable_to_string_id());
  return Status::OK;
}

Status ImpalaServer::DropTable(const TDropTableParams& params) {
  if (FLAGS_use_planservice) {
    return Status("DropTable not supported with external planservice");
  }
  JNIEnv* jni_env = getJNIEnv();
  jbyteArray request_bytes;
  RETURN_IF_ERROR(SerializeThriftMsg(jni_env, &params, &request_bytes));
  jni_env->CallObjectMethod(fe_, drop_table_id_, request_bytes);
  RETURN_ERROR_IF_EXC(jni_env, JniUtil::throwable_to_string_id());
  return Status::OK;
}

Status ImpalaServer::DescribeTable(const string& db, const string& table,
    TDescribeTableResult* columns) {
 if (FLAGS_use_planservice) {
   return Status("DescribeTable not supported with external planservice");
  }
  JNIEnv* jni_env = getJNIEnv();
  jbyteArray request_bytes;
  TDescribeTableParams params;
  params.__set_db(db);
  params.__set_table_name(table);

  RETURN_IF_ERROR(SerializeThriftMsg(jni_env, &params, &request_bytes));
  jbyteArray result_bytes = static_cast<jbyteArray>(
      jni_env->CallObjectMethod(fe_, describe_table_id_, request_bytes));

  RETURN_ERROR_IF_EXC(jni_env, JniUtil::throwable_to_string_id());

  TDescribeTableResult result;
  RETURN_IF_ERROR(DeserializeThriftMsg(jni_env, result_bytes, columns));
  return Status::OK;
}

Status ImpalaServer::GetTableNames(const string* db, const string* pattern,
    TGetTablesResult* table_names) {
  if (FLAGS_use_planservice) {
    return Status("GetTableNames not supported with external planservice");
  }
  JNIEnv* jni_env = getJNIEnv();
  jbyteArray request_bytes;
  TGetTablesParams params;
  if (db != NULL) {
    params.__set_db(*db);
  }
  if (pattern != NULL) {
    params.__set_pattern(*pattern);
  }

  RETURN_IF_ERROR(SerializeThriftMsg(jni_env, &params, &request_bytes));
  jbyteArray result_bytes = static_cast<jbyteArray>(
      jni_env->CallObjectMethod(fe_, get_table_names_id_, request_bytes));

  RETURN_ERROR_IF_EXC(jni_env, JniUtil::throwable_to_string_id());

  RETURN_IF_ERROR(DeserializeThriftMsg(jni_env, result_bytes, table_names));
  return Status::OK;
}

Status ImpalaServer::GetDbNames(const string* pattern, TGetDbsResult* db_names) {
  if (FLAGS_use_planservice) {
    return Status("GetDbNames not supported with external planservice");
  }
  JNIEnv* jni_env = getJNIEnv();
  jbyteArray request_bytes;
  TGetDbsParams params;
  if (pattern != NULL) {
    params.__set_pattern(*pattern);
  }

  RETURN_IF_ERROR(SerializeThriftMsg(jni_env, &params, &request_bytes));
  jbyteArray result_bytes = static_cast<jbyteArray>(
      jni_env->CallObjectMethod(fe_, get_db_names_id_, request_bytes));

  RETURN_ERROR_IF_EXC(jni_env, JniUtil::throwable_to_string_id());

  RETURN_IF_ERROR(DeserializeThriftMsg(jni_env, result_bytes, db_names));
  return Status::OK;
}

Status ImpalaServer::GetExecRequest(
    const TClientRequest& request, TExecRequest* result) {
  if (!FLAGS_use_planservice) {
    // TODO: figure out if repeated calls to
    // JNI_GetCreatedJavaVMs()/AttachCurrentThread() are too expensive
    JNIEnv* jni_env = getJNIEnv();
    jbyteArray request_bytes;
    RETURN_IF_ERROR(SerializeThriftMsg(jni_env, &request, &request_bytes));
    jbyteArray result_bytes = static_cast<jbyteArray>(
        jni_env->CallObjectMethod(fe_, create_exec_request_id_, request_bytes));
    RETURN_ERROR_IF_EXC(jni_env, JniUtil::throwable_to_string_id());
    RETURN_IF_ERROR(DeserializeThriftMsg(jni_env, result_bytes, result));
    // TODO: dealloc result_bytes?
    // TODO: figure out if we should detach here
    //RETURN_IF_JNIERROR(jvm_->DetachCurrentThread());
    return Status::OK;
  } else {
    planservice_client_->CreateExecRequest(*result, request);
    return Status::OK;
  }
}

Status ImpalaServer::GetExplainPlan(
    const TClientRequest& query_request, string* explain_string) {
  if (!FLAGS_use_planservice) {
    // TODO: figure out if repeated calls to
    // JNI_GetCreatedJavaVMs()/AttachCurrentThread() are too expensive
    JNIEnv* jni_env = getJNIEnv();
    jbyteArray query_request_bytes;
    RETURN_IF_ERROR(SerializeThriftMsg(jni_env, &query_request, &query_request_bytes));
    jstring java_explain_string = static_cast<jstring>(
        jni_env->CallObjectMethod(fe_, get_explain_plan_id_, query_request_bytes));
    RETURN_ERROR_IF_EXC(jni_env, JniUtil::throwable_to_string_id());
    jboolean is_copy;
    const char *str = jni_env->GetStringUTFChars(java_explain_string, &is_copy);
    RETURN_ERROR_IF_EXC(jni_env, JniUtil::throwable_to_string_id());
    *explain_string = str;
    jni_env->ReleaseStringUTFChars(java_explain_string, str);
    RETURN_ERROR_IF_EXC(jni_env, JniUtil::throwable_to_string_id());
    return Status::OK;
  } else {
    try {
      planservice_client_->GetExplainString(*explain_string, query_request);
    } catch (TException& e) {
      return Status(e.what());
    }
    return Status::OK;
  }
}

Status ImpalaServer::ResetCatalogInternal() {
  LOG(INFO) << "Refreshing catalog";
  if (!FLAGS_use_planservice) {
    JNIEnv* jni_env = getJNIEnv();
    jni_env->CallObjectMethod(fe_, reset_catalog_id_);
    RETURN_ERROR_IF_EXC(jni_env, JniUtil::throwable_to_string_id());
  } else {
    try {
      planservice_client_->RefreshMetadata();
    } catch (TTransportException& e) {
      stringstream msg;
      msg << "RefreshMetadata rpc failed: " << e.what();
      LOG(ERROR) << msg.str();
      // TODO: different error code here?
      return Status(msg.str());
    }
  }
  
  ImpaladMetrics::IMPALA_SERVER_LAST_REFRESH_TIME->Update(
      TimestampValue::local_time().DebugString());

  return Status::OK;
}

void ImpalaServer::ResetCatalog(impala::TStatus& status) {
  ResetCatalogInternal().ToThrift(&status);
}

Status ImpalaServer::CancelInternal(const TUniqueId& query_id) {
  VLOG_QUERY << "Cancel(): query_id=" << PrintId(query_id);
  shared_ptr<QueryExecState> exec_state = GetQueryExecState(query_id, true);
  if (exec_state == NULL) return Status("Invalid or unknown query handle");

  lock_guard<mutex> l(*exec_state->lock(), adopt_lock_t());
  // TODO: can we call Coordinator::Cancel() here while holding lock?
  exec_state->Cancel();
  return Status::OK;
}

Status ImpalaServer::CloseSessionInternal(const ThriftServer::SessionKey& session_key) {
  // Find the session_state and remove it from the map.
  shared_ptr<SessionState> session_state;
  {
    lock_guard<mutex> l(session_state_map_lock_);
    SessionStateMap::iterator entry = session_state_map_.find(session_key);
    if (entry == session_state_map_.end()) {
      return Status("Invalid session key");
    }
    session_state = entry->second;
    session_state_map_.erase(session_key);
  }
  DCHECK(session_state != NULL);

  unordered_set<TUniqueId> inflight_queries;
  {
    lock_guard<mutex> l(session_state->lock);
    DCHECK(!session_state->closed);
    session_state->closed = true;
    // Once closed is set to true, no more queries can be started. The inflight list
    // will not grow.
    inflight_queries.insert(session_state->inflight_queries.begin(),
        session_state->inflight_queries.end());
  }

  // Unregister all open queries from this session.
  BOOST_FOREACH(const TUniqueId& query_id, inflight_queries) {
    UnregisterQuery(query_id);
  }

  return Status::OK;
}


Status ImpalaServer::ParseQueryOptions(const string& options,
    TQueryOptions* query_options) {
  if (options.length() == 0) return Status::OK;
  vector<string> kv_pairs;
  split(kv_pairs, options, is_any_of(","), token_compress_on);
  BOOST_FOREACH(string& kv_string, kv_pairs) {
    trim(kv_string);
    if (kv_string.length() == 0) continue;
    vector<string> key_value;
    split(key_value, kv_string, is_any_of("="), token_compress_on);
    if (key_value.size() != 2) {
      stringstream ss;
      ss << "Ignoring invalid configuration option " << kv_string
         << ": bad format (expected key=value)";
      return Status(ss.str());
    }

    RETURN_IF_ERROR(SetQueryOptions(key_value[0], key_value[1], query_options));
  }
  return Status::OK;
}

Status ImpalaServer::SetQueryOptions(const string& key, const string& value,
    TQueryOptions* query_options) {
  int option = GetQueryOption(key);
  if (option < 0) {
    stringstream ss;
    ss << "Ignoring invalid configuration option: " << key;
    return Status(ss.str());
  } else {
    switch (option) {
      case TImpalaQueryOptions::ABORT_ON_ERROR:
        query_options->__set_abort_on_error(
            iequals(value, "true") || iequals(value, "1"));
        break;
      case TImpalaQueryOptions::MAX_ERRORS:
        query_options->__set_max_errors(atoi(value.c_str()));
        break;
      case TImpalaQueryOptions::DISABLE_CODEGEN:
        query_options->__set_disable_codegen(
            iequals(value, "true") || iequals(value, "1"));
        break;
      case TImpalaQueryOptions::BATCH_SIZE:
        query_options->__set_batch_size(atoi(value.c_str()));
        break;
      case TImpalaQueryOptions::MEM_LIMIT: {
        // Parse the mem limit spec and validate it.
        bool is_percent;
        int64_t bytes_limit = ParseUtil::ParseMemSpec(value, &is_percent);
        if (bytes_limit < 0) {
          return Status("Failed to parse mem limit from '" + value + "'.");
        }
        if (is_percent) {
          return Status("Invalid query memory limit with percent '" + value + "'.");
        }
        query_options->__set_mem_limit(bytes_limit);
        break;
      }
      case TImpalaQueryOptions::NUM_NODES:
        query_options->__set_num_nodes(atoi(value.c_str()));
        break;
      case TImpalaQueryOptions::MAX_SCAN_RANGE_LENGTH:
        query_options->__set_max_scan_range_length(atol(value.c_str()));
        break;
      case TImpalaQueryOptions::MAX_IO_BUFFERS:
        query_options->__set_max_io_buffers(atoi(value.c_str()));
        break;
      case TImpalaQueryOptions::NUM_SCANNER_THREADS:
        query_options->__set_num_scanner_threads(atoi(value.c_str()));
        break;
      case TImpalaQueryOptions::ALLOW_UNSUPPORTED_FORMATS:
        query_options->__set_allow_unsupported_formats(
            iequals(value, "true") || iequals(value, "1"));
        break;
      case TImpalaQueryOptions::DEFAULT_ORDER_BY_LIMIT:
        query_options->__set_default_order_by_limit(atoi(value.c_str()));
        break;
      case TImpalaQueryOptions::DEBUG_ACTION:
        query_options->__set_debug_action(value.c_str());
        break;
      case TImpalaQueryOptions::ABORT_ON_DEFAULT_LIMIT_EXCEEDED:
        query_options->__set_abort_on_default_limit_exceeded(
            iequals(value, "true") || iequals(value, "1"));
        break;
      default:
        // We hit this DCHECK(false) if we forgot to add the corresponding entry here
        // when we add a new query option.
        LOG(ERROR) << "Missing exec option implementation: " << key;
        DCHECK(false);
        break;
    }
  }
  return Status::OK;
}

inline shared_ptr<ImpalaServer::FragmentExecState> ImpalaServer::GetFragmentExecState(
    const TUniqueId& fragment_instance_id) {
  lock_guard<mutex> l(fragment_exec_state_map_lock_);
  FragmentExecStateMap::iterator i = fragment_exec_state_map_.find(fragment_instance_id);
  if (i == fragment_exec_state_map_.end()) {
    return shared_ptr<FragmentExecState>();
  } else {
    return i->second;
  }
}

void ImpalaServer::ExecPlanFragment(
    TExecPlanFragmentResult& return_val, const TExecPlanFragmentParams& params) {
  VLOG_QUERY << "ExecPlanFragment() instance_id=" << params.params.fragment_instance_id
             << " coord=" << params.coord << " backend#=" << params.backend_num;
  StartPlanFragmentExecution(params).SetTStatus(&return_val);
}

void ImpalaServer::ReportExecStatus(
    TReportExecStatusResult& return_val, const TReportExecStatusParams& params) {
  VLOG_FILE << "ReportExecStatus() query_id=" << params.query_id
            << " backend#=" << params.backend_num
            << " instance_id=" << params.fragment_instance_id
            << " done=" << (params.done ? "true" : "false");
  // TODO: implement something more efficient here, we're currently
  // acquiring/releasing the map lock and doing a map lookup for
  // every report (assign each query a local int32_t id and use that to index into a
  // vector of QueryExecStates, w/o lookup or locking?)
  shared_ptr<QueryExecState> exec_state = GetQueryExecState(params.query_id, false);
  if (exec_state.get() == NULL) {
    return_val.status.__set_status_code(TStatusCode::INTERNAL_ERROR);
    stringstream str;
    str << "unknown query id: " << params.query_id;
    return_val.status.error_msgs.push_back(str.str());
    LOG(ERROR) << str.str();
    return;
  }
  exec_state->coord()->UpdateFragmentExecStatus(params).SetTStatus(&return_val);
}

void ImpalaServer::CancelPlanFragment(
    TCancelPlanFragmentResult& return_val, const TCancelPlanFragmentParams& params) {
  VLOG_QUERY << "CancelPlanFragment(): instance_id=" << params.fragment_instance_id;
  shared_ptr<FragmentExecState> exec_state =
      GetFragmentExecState(params.fragment_instance_id);
  if (exec_state.get() == NULL) {
    stringstream str;
    str << "unknown fragment id: " << params.fragment_instance_id;
    Status status(TStatusCode::INTERNAL_ERROR, str.str());
    status.SetTStatus(&return_val);
    return;
  }
  // we only initiate cancellation here, the map entry as well as the exec state
  // are removed when fragment execution terminates (which is at present still
  // running in exec_state->exec_thread_)
  exec_state->Cancel().SetTStatus(&return_val);
}

void ImpalaServer::TransmitData(
    TTransmitDataResult& return_val, const TTransmitDataParams& params) {
  VLOG_ROW << "TransmitData(): instance_id=" << params.dest_fragment_instance_id
           << " node_id=" << params.dest_node_id
           << " #rows=" << params.row_batch.num_rows
           << " eos=" << (params.eos ? "true" : "false");
  // TODO: fix Thrift so we can simply take ownership of thrift_batch instead
  // of having to copy its data
  if (params.row_batch.num_rows > 0) {
    Status status = exec_env_->stream_mgr()->AddData(
        params.dest_fragment_instance_id, params.dest_node_id, params.row_batch);
    status.SetTStatus(&return_val);
    if (!status.ok()) {
      // should we close the channel here as well?
      return;
    }
  }

  if (params.eos) {
    exec_env_->stream_mgr()->CloseSender(
        params.dest_fragment_instance_id, params.dest_node_id).SetTStatus(&return_val);
  }
}

Status ImpalaServer::StartPlanFragmentExecution(
    const TExecPlanFragmentParams& exec_params) {
  if (!exec_params.fragment.__isset.output_sink) {
    return Status("missing sink in plan fragment");
  }

  const TPlanFragmentExecParams& params = exec_params.params;
  shared_ptr<FragmentExecState> exec_state(
      new FragmentExecState(
        params.query_id, exec_params.backend_num, params.fragment_instance_id,
        exec_env_, exec_params.coord));
  // Call Prepare() now, before registering the exec state, to avoid calling
  // exec_state->Cancel().
  // We might get an async cancellation, and the executor requires that Cancel() not
  // be called before Prepare() returns.
  RETURN_IF_ERROR(exec_state->Prepare(exec_params));

  {
    lock_guard<mutex> l(fragment_exec_state_map_lock_);
    // register exec_state before starting exec thread
    fragment_exec_state_map_.insert(make_pair(params.fragment_instance_id, exec_state));
  }

  // execute plan fragment in new thread
  // TODO: manage threads via global thread pool
  exec_state->set_exec_thread(
      new thread(&ImpalaServer::RunExecPlanFragment, this, exec_state.get()));
  return Status::OK;
}

void ImpalaServer::RunExecPlanFragment(FragmentExecState* exec_state) {
  ImpaladMetrics::IMPALA_SERVER_NUM_FRAGMENTS->Increment(1L);
  exec_state->Exec();

  // we're done with this plan fragment
  {
    lock_guard<mutex> l(fragment_exec_state_map_lock_);
    FragmentExecStateMap::iterator i =
        fragment_exec_state_map_.find(exec_state->fragment_instance_id());
    if (i != fragment_exec_state_map_.end()) {
      // ends up calling the d'tor, if there are no async cancellations
      fragment_exec_state_map_.erase(i);
    } else {
      LOG(ERROR) << "missing entry in fragment exec state map: instance_id="
                 << exec_state->fragment_instance_id();
    }
  }
#ifndef ADDRESS_SANITIZER
  // tcmalloc and address sanitizer can not be used together
  if (FLAGS_log_mem_usage_interval > 0) {
    uint64_t num_complete = ImpaladMetrics::IMPALA_SERVER_NUM_FRAGMENTS->value();
    if (num_complete % FLAGS_log_mem_usage_interval == 0) {
      char buf[2048];
      // This outputs how much memory is currently being used by this impalad
      MallocExtension::instance()->GetStats(buf, 2048);
      LOG(INFO) << buf;
    }
  }
#endif
}

int ImpalaServer::GetQueryOption(const string& key) {
  map<int, const char*>::const_iterator itr =
      _TImpalaQueryOptions_VALUES_TO_NAMES.begin();
  for (; itr != _TImpalaQueryOptions_VALUES_TO_NAMES.end(); ++itr) {
    if (iequals(key, (*itr).second)) {
      return itr->first;
    }
  }
  return -1;
}

void ImpalaServer::InitializeConfigVariables() {
  Status status = ParseQueryOptions(FLAGS_default_query_options, &default_query_options_);
  if (!status.ok()) {
    // Log error and exit if the default query options are invalid.
    LOG(ERROR) << "Invalid default query options. Please check -default_query_options.\n"
               << status.GetErrorMsg();
    exit(1);
  }
  LOG(INFO) << "Default query options:" << ThriftDebugString(default_query_options_);

  map<string, string> string_map;
  TQueryOptionsToMap(default_query_options_, &string_map);
  map<string, string>::const_iterator itr = string_map.begin();
  for (; itr != string_map.end(); ++itr) {
    ConfigVariable option;
    option.__set_key(itr->first);
    option.__set_value(itr->second);
    default_configs_.push_back(option);
  }
  ConfigVariable support_start_over;
  support_start_over.__set_key("support_start_over");
  support_start_over.__set_value("false");
  default_configs_.push_back(support_start_over);
}

Status ImpalaServer::ValidateSettings() {
  // Use FE to check Hadoop config setting
  // TODO: check OS setting
  stringstream ss;
  JNIEnv* jni_env = getJNIEnv();
  jstring error_string =
      static_cast<jstring>(jni_env->CallObjectMethod(fe_, check_hadoop_config_id_));
  RETURN_ERROR_IF_EXC(jni_env, JniUtil::throwable_to_string_id());
  jboolean is_copy;
  const char *str = jni_env->GetStringUTFChars(error_string, &is_copy);
  RETURN_ERROR_IF_EXC(jni_env, JniUtil::throwable_to_string_id());
  ss << str;
  jni_env->ReleaseStringUTFChars(error_string, str);
  RETURN_ERROR_IF_EXC(jni_env, JniUtil::throwable_to_string_id());

  if (ss.str().size() > 0) {
    return Status(ss.str());
  }
  return Status::OK;
}


void ImpalaServer::TQueryOptionsToMap(const TQueryOptions& query_option,
    map<string, string>* configuration) {
  map<int, const char*>::const_iterator itr =
      _TImpalaQueryOptions_VALUES_TO_NAMES.begin();
  for (; itr != _TImpalaQueryOptions_VALUES_TO_NAMES.end(); ++itr) {
    stringstream val;
    switch (itr->first) {
      case TImpalaQueryOptions::ABORT_ON_ERROR:
        val << query_option.abort_on_error;
        break;
      case TImpalaQueryOptions::MAX_ERRORS:
        val << query_option.max_errors;
        break;
      case TImpalaQueryOptions::DISABLE_CODEGEN:
        val << query_option.disable_codegen;
        break;
      case TImpalaQueryOptions::BATCH_SIZE:
        val << query_option.batch_size;
        break;
      case TImpalaQueryOptions::MEM_LIMIT:
        val << query_option.mem_limit;
        break;
      case TImpalaQueryOptions::NUM_NODES:
        val << query_option.num_nodes;
        break;
      case TImpalaQueryOptions::MAX_SCAN_RANGE_LENGTH:
        val << query_option.max_scan_range_length;
        break;
      case TImpalaQueryOptions::MAX_IO_BUFFERS:
        val << query_option.max_io_buffers;
        break;
      case TImpalaQueryOptions::NUM_SCANNER_THREADS:
        val << query_option.num_scanner_threads;
        break;
      case TImpalaQueryOptions::ALLOW_UNSUPPORTED_FORMATS:
        val << query_option.allow_unsupported_formats;
        break;
      case TImpalaQueryOptions::DEFAULT_ORDER_BY_LIMIT:
        val << query_option.default_order_by_limit;
        break;
      case TImpalaQueryOptions::DEBUG_ACTION:
        val << query_option.debug_action;
        break;
      case TImpalaQueryOptions::ABORT_ON_DEFAULT_LIMIT_EXCEEDED:
        val << query_option.abort_on_default_limit_exceeded;
        break;
      default:
        // We hit this DCHECK(false) if we forgot to add the corresponding entry here
        // when we add a new query option.
        LOG(ERROR) << "Missing exec option implementation: " << itr->second;
        DCHECK(false);
    }
    (*configuration)[itr->second] = val.str();
  }
}

void ImpalaServer::SessionState::ToThrift(TSessionState* state) {
  lock_guard<mutex> l(lock);
  state->database = database;
}

void ImpalaServer::MembershipCallback(
    const StateStoreSubscriber::TopicDeltaMap& topic_deltas,
    vector<TTopicUpdate>* topic_updates) {
  // TODO: Consider rate-limiting this. In the short term, best to have
  // state-store heartbeat less frequently.
  StateStoreSubscriber::TopicDeltaMap::const_iterator topic =
      topic_deltas.find(SimpleScheduler::IMPALA_MEMBERSHIP_TOPIC);

  if (topic != topic_deltas.end()) {
    const TTopicDelta& delta = topic->second;
    // Although the protocol allows for it, we don't accept true
    // deltas from the state-store, but expect each topic to contains
    // its entire contents.
    if (delta.is_delta) {
      VLOG(1) << "Unexpected topic delta from state-store, ignoring.";
      return;
    }
    vector<TNetworkAddress> current_membership(delta.topic_entries.size());

    BOOST_FOREACH(const TTopicItem& item, delta.topic_entries) {
      uint32_t len = item.value.size();
      TNetworkAddress backend_address;
      Status status = DeserializeThriftMsg(reinterpret_cast<const uint8_t*>(
          item.value.data()), &len, false, &backend_address);
      if (!status.ok()) {
        VLOG(2) << "Error deserializing topic item with key: " << item.key;
        continue;
      }
      current_membership.push_back(backend_address);
    }

    vector<TNetworkAddress> difference;
    sort(current_membership.begin(), current_membership.end());
    set_difference(last_membership_.begin(), last_membership_.end(),
                   current_membership.begin(), current_membership.end(),
                   std::inserter(difference, difference.begin()));
    vector<TUniqueId> to_cancel;
    {
      lock_guard<mutex> l(query_locations_lock_);
      // Build a list of hosts that have currently executing queries but aren't
      // in the membership list. Cancel them in a separate loop to avoid holding
      // on to the location map lock too long.
      BOOST_FOREACH(const TNetworkAddress& hostport, difference) {
        QueryLocations::iterator it = query_locations_.find(hostport);
        if (it != query_locations_.end()) {
          to_cancel.insert(to_cancel.begin(), it->second.begin(), it->second.end());
        }
        // We can remove the location wholesale once we know it's failed.
        query_locations_.erase(hostport);
        exec_env_->client_cache()->CloseConnections(hostport);
      }
    }

    BOOST_FOREACH(const TUniqueId& query_id, to_cancel) {
      CancelInternal(query_id);
    }
    last_membership_.swap(current_membership);
  }
}

ImpalaServer::QueryStateRecord::QueryStateRecord(const QueryExecState& exec_state,
    bool copy_profile, const string& encoded_profile) {
  id = exec_state.query_id();
  const TExecRequest& request = exec_state.exec_request();

  stmt = request.__isset.sql_stmt ? request.sql_stmt : "N/A";
  stmt_type = request.stmt_type;
  user = exec_state.user();
  default_db = exec_state.default_db();
  start_time = exec_state.start_time();
  end_time = exec_state.end_time();
  has_coord = false;

  Coordinator* coord = exec_state.coord();
  if (coord != NULL) {
    num_complete_fragments = coord->progress().num_complete();
    total_fragments = coord->progress().total();
    has_coord = true;
  }
  query_state = exec_state.query_state();
  num_rows_fetched = exec_state.num_rows_fetched();

  if (copy_profile) {
    stringstream ss;
    exec_state.profile().PrettyPrint(&ss);
    profile_str = ss.str();
    if (encoded_profile.empty()) {
      encoded_profile_str = exec_state.profile().SerializeToArchiveString();
    } else {
      encoded_profile_str = encoded_profile;
    }
  }
}

Status CreateImpalaServer(ExecEnv* exec_env, int beeswax_port, int hs2_port,
    int be_port, ThriftServer** beeswax_server, ThriftServer** hs2_server,
    ThriftServer** be_server, ImpalaServer** impala_server) {
  DCHECK((beeswax_port == 0) == (beeswax_server == NULL));
  DCHECK((hs2_port == 0) == (hs2_server == NULL));
  DCHECK((be_port == 0) == (be_server == NULL));

  shared_ptr<ImpalaServer> handler(new ImpalaServer(exec_env));

  // If the user hasn't deliberately specified a namenode URI, read it
  // from the frontend and parse it into FLAGS_nn{_port}.

  // This must be done *after* ImpalaServer's constructor which
  // creates a JNI environment but before any queries are run (which
  // cause FLAGS_nn to be read)
  if (FLAGS_nn.empty()) {
    // Read the namenode name and port from the Hadoop config.
    string default_fs;
    RETURN_IF_ERROR(handler->GetHadoopConfigValue("fs.defaultFS", &default_fs));
    if (default_fs.empty()) {
      RETURN_IF_ERROR(handler->GetHadoopConfigValue("fs.default.name", &default_fs));
      if (!default_fs.empty()) {
        LOG(INFO) << "fs.defaultFS not found. Falling back to fs.default.name from Hadoop"
                  << " config: " << default_fs;
      }
    } else {
      LOG(INFO) << "Read fs.defaultFS from Hadoop config: " << default_fs;
    }

    if (!default_fs.empty()) {
      size_t double_slash_pos = default_fs.find("//");
      if (double_slash_pos != string::npos) {
        default_fs.erase(0, double_slash_pos + 2);
      }
      vector<string> strs;
      split(strs, default_fs, is_any_of(":"));
      FLAGS_nn = strs[0];
      DCHECK(!strs[0].empty());
      LOG(INFO) << "Setting default name (-nn): " << strs[0];
      if (strs.size() > 1) {
        LOG(INFO) << "Setting default port (-nn_port): " << strs[1];
        try {
          FLAGS_nn_port = lexical_cast<int>(strs[1]);
        } catch (bad_lexical_cast) {
          LOG(ERROR) << "Could not set -nn_port from Hadoop configuration. Port was: "
                     << strs[1];
        }
      }
    } else {
      return Status("Could not find valid namenode URI. Set fs.defaultFS in Impala's "
                    "Hadoop configuration files");
    }
  }

  // TODO: do we want a BoostThreadFactory?
  // TODO: we want separate thread factories here, so that fe requests can't starve
  // be requests
  shared_ptr<ThreadFactory> thread_factory(new PosixThreadFactory());

  if (beeswax_port != 0 && beeswax_server != NULL) {
    // Beeswax FE must be a TThreadPoolServer because ODBC and Hue only support
    // TThreadPoolServer.
    shared_ptr<TProcessor> beeswax_processor(new ImpalaServiceProcessor(handler));
    *beeswax_server = new ThriftServer("beeswax-frontend", beeswax_processor,
        beeswax_port, exec_env->metrics(), FLAGS_fe_service_threads,
        ThriftServer::ThreadPool);

    (*beeswax_server)->SetSessionHandler(handler.get());

    LOG(INFO) << "Impala Beeswax Service listening on " << beeswax_port;
  }

  if (hs2_port != 0 && hs2_server != NULL) {
    // HiveServer2 JDBC driver does not support non-blocking server.
    shared_ptr<TProcessor> hs2_fe_processor(
        new ImpalaHiveServer2ServiceProcessor(handler));
    *hs2_server = new ThriftServer("hiveServer2-frontend",
        hs2_fe_processor, hs2_port, exec_env->metrics(), FLAGS_fe_service_threads,
        ThriftServer::ThreadPool);

    LOG(INFO) << "Impala HiveServer2 Service listening on " << hs2_port;
  }

  if (be_port != 0 && be_server != NULL) {
    shared_ptr<TProcessor> be_processor(new ImpalaInternalServiceProcessor(handler));
    *be_server = new ThriftServer("backend", be_processor, be_port, exec_env->metrics(),
        FLAGS_be_service_threads);

    LOG(INFO) << "ImpalaInternalService listening on " << be_port;
  }
  if (impala_server != NULL) *impala_server = handler.get();

  return Status::OK;
}

}
