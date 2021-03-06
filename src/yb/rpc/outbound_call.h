// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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
#ifndef YB_RPC_OUTBOUND_CALL_H_
#define YB_RPC_OUTBOUND_CALL_H_

#include <deque>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/macros.h"
#include "yb/rpc/rpc_fwd.h"
#include "yb/rpc/constants.h"
#include "yb/rpc/remote_method.h"
#include "yb/rpc/response_callback.h"
#include "yb/rpc/rpc_call.h"
#include "yb/rpc/rpc_header.pb.h"
#include "yb/rpc/service_if.h"

#include "yb/util/locks.h"
#include "yb/util/monotime.h"
#include "yb/util/net/sockaddr.h"
#include "yb/util/object_pool.h"
#include "yb/util/ref_cnt_buffer.h"
#include "yb/util/slice.h"
#include "yb/util/status.h"
#include "yb/util/trace.h"

namespace google {
namespace protobuf {
class Message;
}  // namespace protobuf
}  // namespace google

namespace yb {
namespace rpc {

class CallResponse;
class DumpRunningRpcsRequestPB;
class YBInboundTransfer;
class RpcCallInProgressPB;
class RpcCallDetailsPB;
class RpcController;

// Used to key on Connection information.
// For use as a key in an unordered STL collection, use ConnectionIdHash and ConnectionIdEqual.
// This class is copyable for STL compatibility, but not assignable (use CopyFrom() for that).
class ConnectionId {
 public:
  ConnectionId() {}

  // Convenience constructor.
  ConnectionId(const Endpoint& remote, size_t idx, const Protocol* protocol)
      : remote_(remote), idx_(idx), protocol_(protocol) {}

  // The remote address.
  const Endpoint& remote() const { return remote_; }
  uint8_t idx() const { return idx_; }
  const Protocol* protocol() const { return protocol_; }

  // Returns a string representation of the object, not including the password field.
  std::string ToString() const;

  size_t HashCode() const;

 private:
  // Remember to update HashCode() and Equals() when new fields are added.
  Endpoint remote_;
  uint8_t idx_ = 0;  // Connection index, used to support multiple connections to the same server.
  const Protocol* protocol_ = nullptr;
};

class ConnectionIdHash {
 public:
  std::size_t operator() (const ConnectionId& conn_id) const;
};

inline bool operator==(const ConnectionId& lhs, const ConnectionId& rhs) {
  return lhs.remote() == rhs.remote() && lhs.idx() == rhs.idx() && lhs.protocol() == rhs.protocol();
}

// Container for OutboundCall metrics
struct OutboundCallMetrics {
  explicit OutboundCallMetrics(const scoped_refptr<MetricEntity>& metric_entity);

  scoped_refptr<Histogram> queue_time;
  scoped_refptr<Histogram> send_time;
  scoped_refptr<Histogram> time_to_response;
};

// A response to a call, on the client side.
// Upon receiving a response, this is allocated in the reactor thread and filled
// into the OutboundCall instance via OutboundCall::SetResponse.
//
// This may either be a success or error response.
//
// This class takes care of separating out the distinct payload slices sent
// over.
class CallResponse {
 public:
  // Maximum number of separate payloads in one response i.e. max number of separate results that
  // return rows (not just status) for the ops grouped together in one tserver RPC call.
  static constexpr size_t kMaxSidecarSlices = 16;

  CallResponse();

  CallResponse(CallResponse&& rhs);
  void operator=(CallResponse&& rhs);

  // Parse the response received from a call. This must be called before any
  // other methods on this object. Takes ownership of data content.
  CHECKED_STATUS ParseFrom(std::vector<char>* data);

  // Return true if the call succeeded.
  bool is_success() const {
    DCHECK(parsed_);
    return !header_.is_error();
  }

  // Return the call ID that this response is related to.
  int32_t call_id() const {
    DCHECK(parsed_);
    return header_.call_id();
  }

  // Return the serialized response data. This is just the response "body" --
  // either a serialized ErrorStatusPB, or the serialized user response protobuf.
  const Slice &serialized_response() const {
    DCHECK(parsed_);
    return serialized_response_;
  }

  // See RpcController::GetSidecar()
  CHECKED_STATUS GetSidecar(int idx, Slice* sidecar) const;

 private:
  // True once ParseFrom() is called.
  bool parsed_;

  // The parsed header.
  ResponseHeader header_;

  // The slice of data for the encoded protobuf response.
  // This slice refers to memory allocated by transfer_
  Slice serialized_response_;

  // Slices of data for rpc sidecars. They point into memory owned by transfer_.
  // Number of sidecars chould be obtained from header_.
  std::array<Slice, kMaxSidecarSlices> sidecar_slices_;

  // The incoming transfer data - retained because serialized_response_
  // and sidecar_slices_ refer into its data.
  std::vector<char> response_data_;

  DISALLOW_COPY_AND_ASSIGN(CallResponse);
};

typedef ThreadSafeObjectPool<RemoteMethodPB> RemoteMethodPool;

// Tracks the status of a call on the client side.
//
// This is an internal-facing class -- clients interact with the
// RpcController class.
//
// This is allocated by the Proxy when a call is first created,
// then passed to the reactor thread to send on the wire. It's typically
// kept using a shared_ptr because a call may terminate in any number
// of different threads, making it tricky to enforce single ownership.
class OutboundCall : public RpcCall {
 public:
  OutboundCall(const RemoteMethod* remote_method,
               const std::shared_ptr<OutboundCallMetrics>& outbound_call_metrics,
               google::protobuf::Message* response_storage,
               RpcController* controller, ResponseCallback callback);
  virtual ~OutboundCall();

  // Serialize the given request PB into this call's internal storage.
  //
  // Because the data is fully serialized by this call, 'req' may be
  // subsequently mutated with no ill effects.
  virtual CHECKED_STATUS SetRequestParam(const google::protobuf::Message& req);

  // Serialize the call for the wire. Requires that SetRequestParam()
  // is called first. This is called from the Reactor thread.
  void Serialize(boost::container::small_vector_base<RefCntBuffer>* output) const override;

  // Callback after the call has been put on the outbound connection queue.
  void SetQueued();

  // Update the call state to show that the request has been sent.
  void SetSent();

  // Update the call state to show that the call has finished.
  void SetFinished();

  // Mark the call as failed. This also triggers the callback to notify
  // the caller. If the call failed due to a remote error, then err_pb
  // should be set to the error returned by the remote server. Takes
  // ownership of 'err_pb'.
  void SetFailed(const Status& status,
                 ErrorStatusPB* err_pb = NULL);

  // Mark the call as timed out. This also triggers the callback to notify
  // the caller.
  void SetTimedOut();
  bool IsTimedOut() const;

  // Is the call finished?
  bool IsFinished() const override;

  // Fill in the call response.
  void SetResponse(CallResponse&& resp);

  std::string ToString() const override;

  bool DumpPB(const DumpRunningRpcsRequestPB& req, RpcCallInProgressPB* resp) override;

  std::string LogPrefix() const override;

  void SetConnectionId(const ConnectionId& value) {
    conn_id_ = value;
  }

  ////////////////////////////////////////////////////////////
  // Getters
  ////////////////////////////////////////////////////////////

  const ConnectionId& conn_id() const { return conn_id_; }
  const RemoteMethod& remote_method() const { return *remote_method_; }
  const ResponseCallback &callback() const { return callback_; }
  RpcController* controller() { return controller_; }
  const RpcController* controller() const { return controller_; }
  google::protobuf::Message* response() const { return response_; }

  int32_t call_id() const {
    return call_id_;
  }

  Trace* trace() {
    return trace_.get();
  }

 protected:
  friend class RpcController;

  virtual CHECKED_STATUS GetSidecar(int idx, Slice* sidecar) const;

  ConnectionId conn_id_;
  MonoTime start_;
  RpcController* const controller_;
  // Pointer for the protobuf where the response should be written.
  google::protobuf::Message* response_;

 private:
  friend class RpcController;

  // Various states the call propagates through.
  // NB: if adding another state, be sure to update OutboundCall::IsFinished()
  // and OutboundCall::StateName(State state) as well.
  enum State {
    READY = 0,
    ON_OUTBOUND_QUEUE = 1,
    SENT = 2,
    TIMED_OUT = 3,
    FINISHED_ERROR = 4,
    FINISHED_SUCCESS = 5
  };

  static std::string StateName(State state);

  void NotifyTransferred(const Status& status, Connection* conn) override;

  void set_state(State new_state);
  State state() const;

  // Same as set_state, but requires that the caller already holds
  // lock_
  void set_state_unlocked(State new_state);

  // return current status
  CHECKED_STATUS status() const;

  // Return the error protobuf, if a remote error occurred.
  // This will only be non-NULL if status().IsRemoteError().
  const ErrorStatusPB* error_pb() const;

  void InitHeader(RequestHeader* header);

  // Lock for state_ status_, error_pb_ fields, since they
  // may be mutated by the reactor thread while the client thread
  // reads them.
  mutable simple_spinlock lock_;
  std::atomic<State> state_ = {READY};
  Status status_;
  gscoped_ptr<ErrorStatusPB> error_pb_;

  // Call the user-provided callback.
  void CallCallback();

  int32_t call_id_;

  // The remote method being called.
  const RemoteMethod* remote_method_;

  ResponseCallback callback_;

  // Buffers for storing segments of the wire-format request.
  RefCntBuffer buffer_;

  // Once a response has been received for this call, contains that response.
  CallResponse call_response_;

  // The trace buffer.
  scoped_refptr<Trace> trace_;

  std::shared_ptr<OutboundCallMetrics> outbound_call_metrics_;

  RemoteMethodPool* remote_method_pool_;

  DISALLOW_COPY_AND_ASSIGN(OutboundCall);
};

}  // namespace rpc
}  // namespace yb

#endif  // YB_RPC_OUTBOUND_CALL_H_
