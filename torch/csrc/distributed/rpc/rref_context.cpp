#include <torch/csrc/distributed/rpc/rref_context.h>
#include <torch/csrc/distributed/rpc/rref_proto.h>

namespace torch {
namespace distributed {
namespace rpc {

std::unique_ptr<RRefContext> RRefContext::context_ = nullptr;
thread_local std::vector<std::shared_ptr<RRef>> RRefContext::rrefArgs_ = {};

void RRefContext::initInstance(std::shared_ptr<RpcAgent> agent) {
  TORCH_CHECK(!RRefContext::context_, "Can only initialize RRefContext once.");
  TORCH_CHECK(agent, "RRefContext requires a non-null RpcAgent shared_ptr.");

  RRefContext::context_ =
      std::unique_ptr<RRefContext>(new RRefContext(std::move(agent)));
}

std::unique_ptr<RRefContext>& RRefContext::getInstance() {
  TORCH_CHECK(
      RRefContext::context_, "Have to initialize RRefContext before use.");
  return RRefContext::context_;
}

void RRefContext::handleException(const Message& message) {
  if (message.type() == MessageType::EXCEPTION) {
    // TODO: allow users to register an error handler and call it here.
    std::string err(message.payload().begin(), message.payload().end());
    VLOG(1) << "Got exception: " << err << std::endl << std::flush;
    throw std::runtime_error(err);
  }
}

RRefContext::RRefContext(std::shared_ptr<RpcAgent> agent)
    : agent_(std::move(agent)) {}

template <typename T>
std::shared_ptr<UserRRef<T>> RRefContext::createUserRRef(worker_id_t ownerId) {
  TORCH_CHECK(ownerId != getWorkerId(), "Cannot create UserRRef on owner.");
  return createUserRRef<T>(
      ownerId, genGloballyUniqueId(), genGloballyUniqueId());
}

template std::shared_ptr<UserRRef<IValue>> RRefContext::createUserRRef<IValue>(
    worker_id_t ownerId);

template std::shared_ptr<UserRRef<py::object>> RRefContext::createUserRRef<
    py::object>(worker_id_t ownerId);

template <typename T>
std::shared_ptr<UserRRef<T>> RRefContext::createUserRRef(
    worker_id_t ownerId,
    const RRefId& rrefId,
    const ForkId& forkId) {
  TORCH_CHECK(ownerId != getWorkerId(), "RRef owner cannot create user RRef.");
  // RRefContext does not track user RRefs, it will be destructed when there
  // is no shared_ptrs pointing to it. NB: cannot use make_shared here as the
  // constructor of UserRRef is private
  auto userRRef =
      std::shared_ptr<UserRRef<T>>(new UserRRef<T>(ownerId, rrefId, forkId));

  {
    std::lock_guard<std::mutex> lock(mutex_);
    TORCH_CHECK(
        pendingUsers_.find(forkId) == pendingUsers_.end(),
        "Inconsistent state, attempt to create the same UserRRef twice.")

    auto iter = pendingAcceptedUsers_.find(forkId);
    if (iter == pendingAcceptedUsers_.end()) {
      // UserRRef created before receiving RREF_USER_ACCEPT message
      pendingUsers_[forkId] = userRRef;
    } else {
      // RREF_USER_ACCEPT arrives before UserRRef is created, remove it
      pendingAcceptedUsers_.erase(iter);
    }
  }
  return userRRef;
}

template std::shared_ptr<UserRRef<IValue>> RRefContext::createUserRRef<IValue>(
    worker_id_t ownerId,
    const RRefId& rrefId,
    const ForkId& forkId);

template std::shared_ptr<UserRRef<py::object>> RRefContext::createUserRRef<
    py::object>(
    worker_id_t ownerId,
    const RRefId& rrefId,
    const ForkId& forkId);

template <typename T>
std::shared_ptr<RRef> RRefContext::getOrCreateRRef(
    worker_id_t ownerId,
    const RRefId& rrefId,
    const ForkId& forkId) {
  if (ownerId == getWorkerId()) {
    return getOrCreateOwnerRRef<T>(rrefId);
  } else {
    return createUserRRef<T>(ownerId, rrefId, forkId);
  }
}

template std::shared_ptr<RRef> RRefContext::getOrCreateRRef<IValue>(
    worker_id_t ownerId,
    const RRefId& rrefId,
    const ForkId& forkId);

template std::shared_ptr<RRef> RRefContext::getOrCreateRRef<py::object>(
    worker_id_t ownerId,
    const RRefId& rrefId,
    const ForkId& forkId);

template <typename T>
std::shared_ptr<OwnerRRef<T>> RRefContext::getOrCreateOwnerRRef(
    const RRefId& rrefId) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto iter = owners_.find(rrefId);
  if (iter == owners_.end()) {
    // Scenario (1) the first time this owner knows about this RRef
    // Scenario (2) This owner is also the creator.
    //
    // NB: cannot use make_shared here as the constructor of OwnerRRef is
    // private.
    auto rref =
        std::shared_ptr<OwnerRRef<T>>(new OwnerRRef<T>(getWorkerId(), rrefId));
    owners_[rref->rrefId()] = rref;
    return rref;

  } else {
    // Scenario (3) retrieving an existing RRef
    return std::dynamic_pointer_cast<OwnerRRef<T>>(iter->second);
  }
}

template std::shared_ptr<OwnerRRef<IValue>> RRefContext::getOrCreateOwnerRRef<
    IValue>(const RRefId& rrefId);

template std::shared_ptr<OwnerRRef<py::object>> RRefContext::
    getOrCreateOwnerRRef<py::object>(const RRefId& rrefId);

RRefForkData RRefContext::forkTo(
    const std::shared_ptr<RRef>& rref,
    worker_id_t forkDst) {

  // keep rref argments alive
  // TODO: only do this for requests
  rrefArgs_.push_back(rref);

  auto forkRequest = rref->fork();
  if (rref->owner() != forkDst) {
    // if fork destination if not owner, the forked UserRRef needs to be tracked
    // properly
    if (rref->isOwner()) {
      // fork from owner
      auto fm = agent_->send(
          agent_->getWorkerInfo(forkDst),
          acceptUserRRef(forkRequest.rrefId_, forkRequest.forkId_));

      fm->addCallback([forkRequest, this](const Message& message) {
        handleException(message);
        this->delForkOfOwner(forkRequest.rrefId_, forkRequest.forkId_);
      });
    } else {
      // fork from user, rref cannot be destructed until the fork request is
      // accepted by the owner
      {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingForkRequests_[forkRequest.forkId_] = rref;
      }
      // notify owner
      auto fm = agent_->send(
          agent_->getWorkerInfo(rref->owner()),
          RRefForkNotify(forkRequest.rrefId_, forkRequest.forkId_, forkDst)
              .toMessage());

      fm->addCallback([this](const Message& message) {
        handleException(message);
        auto rfa = RRefForkAccept::fromMessage(message);
        this->finishForkRequest(rfa.forkId());
      });
    }
  }
  return forkRequest;
}

Message RRefContext::acceptUserRRef(
    const RRefId& rrefId,
    const ForkId& forkId) {
  addForkOfOwner(rrefId, forkId);
  return RRefUserAccept(rrefId, forkId).toMessage();
}

Message RRefContext::acceptForkRequest(
    const RRefId& rrefId,
    const ForkId& forkId,
    worker_id_t forkDst) {
  // TODO: add exception handling
  auto fm = agent_->send(
      agent_->getWorkerInfo(forkDst), acceptUserRRef(rrefId, forkId));

  fm->addCallback([rrefId, forkId, this](const Message& message) {
    handleException(message);
    this->delForkOfOwner(rrefId, forkId);
  });
  // notify fork caller UserRRef
  return RRefForkAccept(forkId).toMessage();
}

void RRefContext::finishForkRequest(const ForkId& forkId) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto iter = pendingForkRequests_.find(forkId);
  TORCH_INTERNAL_ASSERT(
      iter != pendingForkRequests_.end(),
      "Cannot finish a non-exist fork request.");
  pendingForkRequests_.erase(iter);
}

void RRefContext::finishUserRRef(const RRefId& rrefId, const ForkId& forkId) {
  std::lock_guard<std::mutex> lock(mutex_);
  TORCH_INTERNAL_ASSERT(
      pendingAcceptedUsers_.find(forkId) == pendingAcceptedUsers_.end(),
      "Inconsistent state, attempt to accept the same UserRRef twice.")

  auto iter = pendingUsers_.find(forkId);
  if (iter != pendingUsers_.end()) {
    TORCH_INTERNAL_ASSERT(
        iter->second->rrefId() == rrefId,
        "Attempt to accept a fork with incorrect RRefId.");
    // UserRRef created before receiving RREF_USER_ACCEPT message
    pendingUsers_.erase(iter);
  } else {
    // RREF_USER_ACCEPT arrives before UserRRef is created, remove it
    pendingAcceptedUsers_.insert(forkId);
  }
}

void RRefContext::addForkOfOwner(const RRefId& rrefId, const ForkId& forkId) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& rrefForks = forks_[rrefId];
  TORCH_INTERNAL_ASSERT(
      rrefForks.find(forkId) == rrefForks.end(),
      "Got fork notification twice on the same RRef ",
      forkId);
  rrefForks.insert(forkId);
}

void RRefContext::delForkOfOwner(const RRefId& rrefId, const ForkId& forkId) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto iter = forks_.find(rrefId);
  TORCH_INTERNAL_ASSERT(
      iter != forks_.end(),
      "Inconsistent states, deleting a fork before the owner knows it.");
  auto& rrefForks = iter->second;
  TORCH_INTERNAL_ASSERT(
      rrefForks.find(forkId) != rrefForks.end(),
      "Attempt to delete a non-exist fork ",
      forkId);
  rrefForks.erase(rrefId);

  if (rrefForks.empty()) {
    owners_.erase(rrefId);
    forks_.erase(rrefId);
  }
}

void RRefContext::addRRefArgs(int64_t messageId) {
  std::lock_guard<std::mutex> lock(mutex_);
  TORCH_INTERNAL_ASSERT(
      pendingRRefArgs_.find(messageId) == pendingRRefArgs_.end(),
      "Cannot set RRef args on the same message twice.");

  pendingRRefArgs_[messageId] = std::move(rrefArgs_);
  rrefArgs_.clear();
}

void RRefContext::delRRefArgs(int64_t messageId) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto iter = pendingRRefArgs_.find(messageId);
  TORCH_INTERNAL_ASSERT(iter != pendingRRefArgs_.end(),
      "Attempt to delete RRef args for non-exist message.");

  pendingRRefArgs_.erase(iter);
}

} // namespace rpc
} // namespace distributed
} // namespace torch
