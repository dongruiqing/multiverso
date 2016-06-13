#include "multiverso/server.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include "multiverso/actor.h"
#include "multiverso/dashboard.h"
#include "multiverso/multiverso.h"
#include "multiverso/table_interface.h"
#include "multiverso/io/io.h"
#include "multiverso/util/configure.h"
#include "multiverso/util/mt_queue.h"
#include "multiverso/zoo.h"

#include "multiverso/controller.h"

namespace multiverso {

MV_DEFINE_bool(sync, false, "sync or async");
MV_DEFINE_int(backup_worker_ratio, 0, "ratio% of backup workers, set 20 means 20%");

Server::Server() : Actor(actor::kServer) {
  RegisterHandler(MsgType::Request_Get, std::bind(
    &Server::ProcessGet, this, std::placeholders::_1));
  RegisterHandler(MsgType::Request_Add, std::bind(
    &Server::ProcessAdd, this, std::placeholders::_1));
}

int Server::RegisterTable(ServerTable* server_table) {
  int id = static_cast<int>(store_.size());
  store_.push_back(server_table);
  return id;
}

void Server::ProcessGet(MessagePtr& msg) {
  MONITOR_BEGIN(SERVER_PROCESS_GET);
  if (msg->data().size() != 0) {
    MessagePtr reply(msg->CreateReplyMessage());
    int table_id = msg->table_id();
    CHECK(table_id >= 0 && table_id < store_.size());
    store_[table_id]->ProcessGet(msg->data(), &reply->data());
    SendTo(actor::kCommunicator, reply);
  }
  MONITOR_END(SERVER_PROCESS_GET);
}

void Server::ProcessAdd(MessagePtr& msg) {
  MONITOR_BEGIN(SERVER_PROCESS_ADD)
  if (msg->data().size() != 0) {
    MessagePtr reply(msg->CreateReplyMessage());
    int table_id = msg->table_id();
    CHECK(table_id >= 0 && table_id < store_.size());
    store_[table_id]->ProcessAdd(msg->data());
    SendTo(actor::kCommunicator, reply);
  }
  MONITOR_END(SERVER_PROCESS_ADD)
}


// The Sync Server implement logic to support Sync SGD training
// The implementation assumes all the workers will call same number
// of Add and/or Get requests
// The server promise all workers i-th Get will get the same parameters
// If worker k has add delta to server j times when its i-th Get 
// then the server will return the parameter after all K 
// workers finished their j-th update

class SyncServer : public Server {
public:
  SyncServer() : Server() {
    RegisterHandler(MsgType::Server_Finish_Train, std::bind(
      &SyncServer::ProcessFinishTrain, this, std::placeholders::_1));
    int num_worker = Zoo::Get()->num_workers();
    worker_get_clocks_.reset(new VectorClock(num_worker));
    worker_add_clocks_.reset(new VectorClock(num_worker));
    num_waited_add_.resize(num_worker, 0);
  }

  // make some modification to suit to the sync server
  // please not use in other place, may different with the general vector clock
  class VectorClock {
  public:
    explicit VectorClock(int n) : 
      local_clock_(n, 0), global_clock_(0), size_(0) {}

    static bool except_max_int_compare(int a, int b) {
      return (b == std::numeric_limits<int>::max() ? false : a < b);
    }

    // Return true when all clock reach a same number
    virtual bool Update(int i) {
      ++local_clock_[i];
      if (global_clock_ < *(std::min_element(std::begin(local_clock_),
        std::end(local_clock_)))) {
        ++global_clock_;
        if (global_clock_ == *(std::max_element(std::begin(local_clock_),
          std::end(local_clock_), except_max_int_compare))) {
          return true;
        }
      }
      return false;
    }

    virtual bool FinishTrain(int i) {
      local_clock_[i] = std::numeric_limits<int>::max();
      if (global_clock_ < *(std::min_element(std::begin(local_clock_),
        std::end(local_clock_)))) {
        ++global_clock_;
        if (global_clock_ == *(std::max_element(std::begin(local_clock_),
          std::end(local_clock_), except_max_int_compare))) {
          return true;
        }
      }
      return false;
    }

    std::string DebugString() {
      std::string os = "global ";
      os += std::to_string(global_clock_) + " local: ";
      for (auto i : local_clock_) os += std::to_string(i) + " ";
      return os;
    }

    int local_clock(int i) const { return local_clock_[i]; }
    int global_clock() const { return global_clock_; }

  protected:
    std::vector<int> local_clock_;
    int global_clock_;
    int size_;
  };
protected:
  void Main() override {
    is_working_ = true;
    MessagePtr msg;
    while (ctrl::console_running()) {
      if (!mailbox_->TryPop(msg)) continue;
      if (handlers_.find(msg->type()) != handlers_.end()) {
        handlers_[msg->type()](msg);
      }
      else if (handlers_.find(MsgType::Default) != handlers_.end()) {
        handlers_[MsgType::Default](msg);
      }
      else {
        Log::Fatal("Unexpected msg type\n");
      }
    }
    if (!ctrl::console_running()) {
      Log::Info("Server %d get clock: %s\n", MV_ServerId(), worker_get_clocks_->DebugString().c_str());
      Log::Info("Server %d add clock: %s\n", MV_ServerId(), worker_add_clocks_->DebugString().c_str());
      Log::Info("Server %d get cache size %d\n", msg_get_cache_.Size());
      MessagePtr msg;
      while (!msg_get_cache_.Empty()) {
        CHECK(msg_get_cache_.Pop(msg));
        Log::Info("Server %d get cache msg from %d\n", MV_ServerId(), Zoo::Get()->rank_to_worker_id(msg->src()));
      }
      while (!msg_add_cache_.Empty()) {
        CHECK(msg_add_cache_.Pop(msg));
        Log::Info("Server %d add cache msg from %d\n", MV_ServerId(), Zoo::Get()->rank_to_worker_id(msg->src()));
      }
      exit(1);
    }
  }
  void ProcessAdd(MessagePtr& msg) override {
    // 1. Before add: cache faster worker
    int worker = Zoo::Get()->rank_to_worker_id(msg->src());
    if (worker_get_clocks_->local_clock(worker) >
        worker_get_clocks_->global_clock()) {
      msg_add_cache_.Push(msg);
      ++num_waited_add_[worker];
      // Log::Info("Server %d: ProcessAdd, cache ADD from worker %d\n", MV_ServerId(), worker);
      return;
    }
    // 2. Process Add
    Server::ProcessAdd(msg);
    // Log::Info("Server %d: ProcessAdd, process ADD from worker %d\n", MV_ServerId(), worker);
    // 3. After add: process cached process get if necessary
    if (worker_add_clocks_->Update(worker)) {
      CHECK(msg_add_cache_.Empty());
      while (!msg_get_cache_.Empty()) {
        MessagePtr get_msg;
        CHECK(msg_get_cache_.TryPop(get_msg));
        int get_worker = Zoo::Get()->rank_to_worker_id(get_msg->src());
        Server::ProcessGet(get_msg);
        // Log::Info("Server %d: ProcessAdd, process cache GET from worker %d\n", MV_ServerId(), get_worker);
        CHECK(!worker_get_clocks_->Update(get_worker));
      }
    }
  }

  void ProcessGet(MessagePtr& msg) override {
    // 1. Before get: cache faster worker
    int worker = Zoo::Get()->rank_to_worker_id(msg->src());
    if (worker_add_clocks_->local_clock(worker) > worker_add_clocks_->global_clock() || 
        // worker_get_clocks_->local_clock(worker) > 
        // worker_get_clocks_->global_clock()) {
        num_waited_add_[worker] > 0) {
      // Will wait for other worker finished Add
      msg_get_cache_.Push(msg);
      // Log::Info("Server %d: ProcessGet, cache GET from worker %d\n", MV_ServerId(), worker);
      return;
    }
    // 2. Process Get
    Server::ProcessGet(msg);
    // Log::Info("Server %d: ProcessGet, process GET from worker %d\n", MV_ServerId(), worker);
    // 3. After get: process cached process add if necessary
    if (worker_get_clocks_->Update(worker)) {
      while (!msg_add_cache_.Empty()) {
        MessagePtr add_msg;
        CHECK(msg_add_cache_.TryPop(add_msg));
        int add_worker = Zoo::Get()->rank_to_worker_id(add_msg->src());
        Server::ProcessAdd(add_msg);
        // Log::Info("Server %d: ProcessGet, process cache ADD from worker %d\n", MV_ServerId(), add_worker);
        CHECK(!worker_add_clocks_->Update(add_worker));
        --num_waited_add_[add_worker];
      }
    }
  }

  void ProcessFinishTrain(MessagePtr& msg) {
    int worker = Zoo::Get()->rank_to_worker_id(msg->src());
    Log::Debug("[ProcessFinishTrain] Server %d, worker %d has finished training.\n", 
               Zoo::Get()->server_rank(), worker);
    if (worker_get_clocks_->FinishTrain(worker)) {
      CHECK(msg_get_cache_.Empty());
      while (!msg_add_cache_.Empty()) {
        MessagePtr add_msg;
        CHECK(msg_add_cache_.TryPop(add_msg));
        int add_worker = Zoo::Get()->rank_to_worker_id(add_msg->src());
        Server::ProcessAdd(add_msg);
        worker_add_clocks_->Update(add_worker);
      }
    }
    if (worker_add_clocks_->FinishTrain(worker)) {
      CHECK(msg_add_cache_.Empty());
      while (!msg_get_cache_.Empty()) {
        MessagePtr get_msg;
        CHECK(msg_get_cache_.TryPop(get_msg));
        int get_worker = Zoo::Get()->rank_to_worker_id(get_msg->src());
        Server::ProcessGet(get_msg);
        worker_get_clocks_->Update(get_worker);
      }
    }
  }

private:
  std::unique_ptr<VectorClock> worker_get_clocks_;
  std::unique_ptr<VectorClock> worker_add_clocks_;
  std::vector<int> num_waited_add_;

  MtQueue<MessagePtr> msg_add_cache_;
  MtQueue<MessagePtr> msg_get_cache_;
};

Server* Server::GetServer() {
  if (!MV_CONFIG_sync) {
    Log::Info("Create a async server\n");
    return new Server();
  }
  // if (MV_CONFIG_backup_worker_ratio > 0.0) {
  Log::Info("Create a sync server\n");
  return new SyncServer();
  // }
}

}  // namespace multiverso
