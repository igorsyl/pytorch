#pragma once

#include <c10/util/Optional.h>
#include <torch/csrc/distributed/rpc/message.h>
#include <torch/csrc/distributed/rpc/rpc_agent.h>
#include <torch/csrc/distributed/rpc/rref.h>
#include <torch/csrc/distributed/rpc/types.h>

#include <atomic>

namespace torch {
namespace distributed {
namespace rpc {

// Manages RRef lifetime and keeps track of RRef forks.
class RRefContext {
 public:
  static void initInstance(std::shared_ptr<RpcAgent>);
  static std::unique_ptr<RRefContext>& getInstance();

  static void handleException(const Message& message);

  RRefContext(const RRefContext&) = delete;
  void operator=(const RRefContext&) = delete;

  inline worker_id_t getWorkerId() const {
    return agent_->getWorkerInfo().id_;
  }

  inline const std::string& getWorkerName() const {
    return agent_->getWorkerInfo().name_;
  }

  inline GloballyUniqueId genGloballyUniqueId() {
    return GloballyUniqueId(getWorkerId(), nextLocalId_++);
  }

  inline const std::shared_ptr<RpcAgent>& agent() const {
    return agent_;
  }

  template <typename T>
  std::shared_ptr<UserRRef<T>> createUserRRef(worker_id_t ownerId);

  template <typename T>
  std::shared_ptr<UserRRef<T>> createUserRRef(
      worker_id_t ownerId,
      const RRefId& rrefId,
      const ForkId& forkId);

  template <typename T>
  std::shared_ptr<RRef> getOrCreateRRef(
      worker_id_t ownerId,
      const RRefId& rrefId,
      const ForkId& forkId);

  template <typename T>
  std::shared_ptr<OwnerRRef<T>> getOrCreateOwnerRRef(const RRefId& rrefId);

  RRefForkData forkTo(const std::shared_ptr<RRef>&, worker_id_t forkDst);

  Message acceptUserRRef(const RRefId& rrefId, const ForkId& forkId);
  Message acceptForkRequest(
      const RRefId& rrefId,
      const ForkId& forkId,
      worker_id_t forkDst);
  void finishForkRequest(const ForkId& forkId);
  void finishUserRRef(const RRefId& rrefId, const ForkId& forkId);

  void addForkOfOwner(const RRefId& rrefId, const ForkId& forkId);
  void delForkOfOwner(const RRefId& rrefId, const ForkId& forkId);

  inline const std::vector<std::shared_ptr<RRef>>& getRRefArgs() const {
    return rrefArgs_;
  }

  void addRRefArgs(int64_t messageId);
  void delRRefArgs(int64_t messageId);

 private:
  RRefContext(std::shared_ptr<RpcAgent>);

  static std::unique_ptr<RRefContext> context_;
  static std::atomic<local_id_t> nextLocalId_;

  // RRef arguments involved in a RPC/Remote call. These need to be kept alive
  // until the the cal is acked by the callee. Otherwise, as we don't enforce
  // FIFO message delivery/processing, the RRef might be deleted before the
  // usage.
  static thread_local std::vector<std::shared_ptr<RRef>> rrefArgs_;
  std::unordered_map<int64_t, std::vector<std::shared_ptr<RRef>>>
      pendingRRefArgs_;

  const std::shared_ptr<RpcAgent> agent_;
  mutable std::mutex mutex_;
  // Keep OwnerRRefs alive until there is no living UserRRefs.
  std::unordered_map<RRefId, std::shared_ptr<RRef>, RRefId::Hash> owners_;
  // Tracks known living UserRRefs of an OwnerRRef
  std::unordered_map<
      RRefId,
      std::unordered_set<ForkId, ForkId::Hash>,
      RRefId::Hash>
      forks_;

  // The follow two maps keep UserRRefs alive by holding a shared_ptr to the
  // RRef instances. A UserRRef must be added into this map if any of the
  // following two conditions is ture:
  //
  // (1) A UserRRef has not been accepted by owner yet.
  //
  //     It can be used or shared, but cannot be deleted, and hence in this map.
  //     A message of type RREF_USER_ACCEPT will remove the corresponding RRef
  //     from this map.
  std::unordered_map<ForkId, std::shared_ptr<RRef>, ForkId::Hash> pendingUsers_;

  // (2) A UserRRef has pending fork requests that are not accepted by the owner
  //     yet.
  //
  //     This is case, this UserRRef cannot send out RREF_USER_DELETE message,
  //     because it is not guaranteed communications are FIFO between any pair
  //     of worker (due to thread pool and potentially new RpcAgent
  //     implementations). As a result, RREF_USER_DELETE might be processed
  //     by the owner before previous RREF_FORK_NOTIFY messages, which would
  //     mess up RRef reference counts.
  std::unordered_map<ForkId, std::shared_ptr<RRef>, ForkId::Hash>
      pendingForkRequests_;

  // RREF_USER_ACCEPT message arrives before the UserRRef was created. This may
  // occur as the RREF_USER_ACCEPT is sent from owner to the callee UserRRef,
  // while the UserRRef is created when the message from caller UserRRef arrives
  std::unordered_set<ForkId, ForkId::Hash> pendingAcceptedUsers_;
};

} // namespace rpc
} // namespace distributed
} // namespace torch
