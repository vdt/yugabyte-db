// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/client/client.h"
#include "yb/client/ql-dml-test-base.h"
#include "yb/client/table_handle.h"

#include "yb/consensus/raft_consensus.h"
#include "yb/consensus/retryable_requests.h"

#include "yb/tablet/tablet_peer.h"

#include "yb/tserver/mini_tablet_server.h"

#include "yb/yql/cql/ql/util/statement_result.h"

DECLARE_double(respond_write_failed_probability);
DECLARE_bool(detect_duplicates_for_retryable_requests);

namespace yb {
namespace client {

namespace {

const std::string kValueColumn = "v";

}

class QLStressTest : public QLDmlTestBase {
 public:
  QLStressTest() {
  }

  void SetUp() override {
    QLDmlTestBase::SetUp();

    YBSchemaBuilder b;
    b.AddColumn("h")->Type(INT32)->HashPrimaryKey()->NotNull();
    b.AddColumn(kValueColumn)->Type(STRING);

    ASSERT_OK(table_.Create(kTableName, CalcNumTablets(3), client_.get(), &b));
  }

  YBqlWriteOpPtr InsertRow(const YBSessionPtr& session, int32_t key, const std::string& value) {
    auto op = table_.NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT);
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, key);
    table_.AddStringColumnValue(req, kValueColumn, value);
    EXPECT_OK(session->Apply(op));
    return op;
  }

  CHECKED_STATUS WriteRow(const YBSessionPtr& session, int32_t key, const std::string& value) {
    auto op = InsertRow(session, key, value);
    RETURN_NOT_OK(session->Flush());
    if (op->response().status() != QLResponsePB::YQL_STATUS_OK) {
      return STATUS_FORMAT(
          RemoteError, "Write failed: $0", QLResponsePB::QLStatus_Name(op->response().status()));
    }

    return Status::OK();
  }

  YBqlReadOpPtr SelectRow(const YBSessionPtr& session, int32_t key) {
    auto op = table_.NewReadOp();
    auto* const req = op->mutable_request();
    QLAddInt32HashValue(req, key);
    table_.AddColumns({kValueColumn}, req);
    EXPECT_OK(session->Apply(op));
    return op;
  }

  Result<std::string> ReadRow(const YBSessionPtr& session, int32_t key) {
    auto op = SelectRow(session, key);
    RETURN_NOT_OK(session->Flush());
    if (op->response().status() != QLResponsePB::YQL_STATUS_OK) {
      return STATUS_FORMAT(
          RemoteError, "Read failed: $0", QLResponsePB::QLStatus_Name(op->response().status()));
    }
    auto rowblock = ql::RowsResult(op.get()).GetRowBlock();
    if (rowblock->row_count() != 1) {
      return STATUS_FORMAT(NotFound, "Bad count for $0, count: $1", key, rowblock->row_count());
    }
    const auto& row = rowblock->row(0);
    return row.column(0).string_value();
  }

  void TestRetryWrites(bool restarts);

  bool CheckRetryableRequestsCounts(size_t* total_entries);

  TableHandle table_;
};

bool QLStressTest::CheckRetryableRequestsCounts(size_t* total_entries) {
  *total_entries = 0;
  bool result = true;
  size_t replicated_limit = FLAGS_detect_duplicates_for_retryable_requests ? 1 : 0;
  for (int i = 0; i != cluster_->num_tablet_servers(); ++i) {
    auto peers = cluster_->GetTabletPeers(i);
    for (const auto& peer : peers) {
      auto leader = peer->LeaderStatus() != consensus::LeaderStatus::NOT_LEADER;
      size_t tablet_entries = peer->tablet()->TEST_CountRocksDBRecords();
      auto raft_consensus = down_cast<consensus::RaftConsensus*>(peer->consensus());
      auto request_counts = raft_consensus->TEST_CountRetryableRequests();
      LOG(INFO) << "T " << peer->tablet()->tablet_id() << " P " << peer->permanent_uuid()
                << ", entries: " << tablet_entries
                << ", running: " << request_counts.running
                << ", replicated: " << request_counts.replicated;
      if (leader) {
        *total_entries += tablet_entries;
      }
      if (request_counts.running != 0 || request_counts.replicated > replicated_limit) {
        result = false;
      }
    }
  }

  return result;
}

void QLStressTest::TestRetryWrites(bool restarts) {
  const size_t kConcurrentWrites = 5;

  SetAtomicFlag(0.25, &FLAGS_respond_write_failed_probability);

  std::vector<std::thread> write_threads;
  std::atomic<int32_t> key_source(0);
  std::atomic<bool> stop_requested(false);
  while (write_threads.size() < kConcurrentWrites) {
    write_threads.emplace_back([this, &key_source, &stop_requested] {
      auto session = NewSession();
      while (!stop_requested.load(std::memory_order_acquire)) {
        int32_t key = key_source.fetch_add(1, std::memory_order_acq_rel);

        auto op = InsertRow(session, key, Format("value_$0", key));
        auto flush_status = session->Flush();
        if (flush_status.ok()) {
          ASSERT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
          continue;
        }
        ASSERT_TRUE(flush_status.IsIOError()) << "Status: " << flush_status;
        ASSERT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_RUNTIME_ERROR);
        ASSERT_EQ(op->response().error_message(), "Duplicate write");
      }
    });
  }

  std::thread restart_thread;
  if (restarts) {
    restart_thread = std::thread([this, &stop_requested] {
      int it = 0;
      while (!stop_requested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(5s);
        ASSERT_OK(cluster_->mini_tablet_server(++it % cluster_->num_tablet_servers())->Restart());
      }
    });
  }

  std::this_thread::sleep_for(restarts ? 60s : 15s);

  stop_requested.store(true, std::memory_order_release);

  for (auto& thread : write_threads) {
    thread.join();
  }

  if (restart_thread.joinable()) {
    restart_thread.join();
  }

  int written_keys = key_source.load(std::memory_order_acquire);
  auto session = NewSession();
  for (int key = 0; key != written_keys; ++key) {
    auto value = ASSERT_RESULT(ReadRow(session, key));
    ASSERT_EQ(value, Format("value_$0", key));
  }

  size_t total_entries = 0;
  ASSERT_OK(WaitFor(std::bind(&QLStressTest::CheckRetryableRequestsCounts, this, &total_entries),
                    15s, "Retryable requests cleanup"));

  // We have 2 entries per row.
  if (FLAGS_detect_duplicates_for_retryable_requests) {
    ASSERT_EQ(total_entries, written_keys * 2);
  } else {
    // If duplicate request tracking is disabled, then total_entries should be greater than
    // written keys, otherwise test does not work.
    ASSERT_GT(total_entries, written_keys * 2);
  }

  ASSERT_GE(written_keys, RegularBuildVsSanitizers(100, 40));
}

TEST_F(QLStressTest, RetryWrites) {
  FLAGS_detect_duplicates_for_retryable_requests = true;
  TestRetryWrites(false /* restarts */);
}

TEST_F(QLStressTest, RetryWritesWithRestarts) {
  FLAGS_detect_duplicates_for_retryable_requests = true;
  TestRetryWrites(true /* restarts */);
}

TEST_F(QLStressTest, RetryWritesDisabled) {
  FLAGS_detect_duplicates_for_retryable_requests = false;
  TestRetryWrites(false /* restarts */);
}

} // namespace client
} // namespace yb
