#include "server.h"
#include "frame.h"


namespace janus {

EpaxosServer::EpaxosServer(Frame * frame) {
  /* Your code here. This function can be called from another OS thread. */
  frame_ = frame;
  Log::set_level(Log::DEBUG); // TODO: REMOVE
}

EpaxosServer::~EpaxosServer() {
  /* Your code here for server teardown */
}

void EpaxosServer::Setup() {
  // Future Work: Give unique replica id on restarts (by storing old replica_id persistently and new_replica_id = old_replica_id + N)
  replica_id_ = site_id_;

  // Process requests
  Coroutine::CreateRun([this](){
    while(true) {
      mtx_.lock();
      int size = reqs.size();
      mtx_.unlock();
      // Future Work: Can make more efficient by having a pub-sub kind of thing
      if (size == 0) {
        Coroutine::Sleep(1000);
      } else {
        mtx_.lock();
        EpaxosRequest req = reqs.front();
        reqs.pop_front();
        mtx_.unlock();
        Coroutine::CreateRun([this, &req](){
          StartPreAccept(req.cmd, req.dkey, req.ballot, req.replica_id, req.instance_no, req.leader_dep_instance, false);
        });
      }
    }
  });

  // Send new commands for execution
  Coroutine::CreateRun([this](){
    while(true) {
      mtx_.lock();
      while (pause_execution) {
        mtx_.unlock();
        Coroutine::Sleep(10000);
        mtx_.lock();
      }
      auto received_till_ = this->received_till;
      auto exec_started_till_ = this->exec_started_till;
      mtx_.unlock();
      for (auto itr : received_till_) {
        uint64_t replica_id = itr.first;
        uint64_t received_till_instance_no = itr.second;
        uint64_t exec_started_till_instance_no = exec_started_till_.count(replica_id) ? exec_started_till_[replica_id] : -1;
        for (uint64_t instance_no = exec_started_till_instance_no+1; instance_no <= received_till_instance_no; instance_no++) {
          Coroutine::CreateRun([this, replica_id, instance_no](){
            StartExecution(replica_id, instance_no);
          });
        }
      }
      mtx_.lock();
      this->exec_started_till = received_till_;
      mtx_.unlock();
      Coroutine::Sleep(1000);
    }
  });

  // Process prepare requests
  Coroutine::CreateRun([this](){
    while(true) {
      mtx_.lock();
      int size = prepare_reqs.size();
      mtx_.unlock();
      // Future Work: Can make more efficient by having a pub-sub kind of thing
      if (size == 0) {
        Coroutine::Sleep(10000);
      } else {
        mtx_.lock();
        auto req = prepare_reqs.front();
        prepare_reqs.pop_front();
        mtx_.unlock();
        Coroutine::CreateRun([this, &req](){
          PrepareTillCommitted(req.first, req.second);
        });
      }
    }
  });
}

void EpaxosServer::Start(shared_ptr<Marshallable>& cmd, string dkey, uint64_t *replica_id, uint64_t *instance_no) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  EpaxosRequest req = CreateEpaxosRequest(cmd, dkey);
  *replica_id = req.replica_id;
  *instance_no = req.instance_no;
  Log_debug("Received request in server: %d for dep_key: %s replica: %d instance: %d", site_id_, dkey.c_str(), req.replica_id, req.instance_no);
  reqs.push_back(req);
}

void EpaxosServer::GetState(uint64_t replica_id, 
                uint64_t instance_no, 
                shared_ptr<Marshallable> *cmd, 
                string *dkey,
                uint64_t *seq, 
                unordered_map<uint64_t, uint64_t> *deps, 
                status_t *state) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  *cmd = cmds[replica_id][instance_no].cmd;
  *dkey = cmds[replica_id][instance_no].dkey;
  *deps = cmds[replica_id][instance_no].deps;
  *seq = cmds[replica_id][instance_no].seq;
  *state = cmds[replica_id][instance_no].state;
}

void EpaxosServer::Prepare(uint64_t replica_id, uint64_t instance_no) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  Log_debug("Received prepare request in server: %d for replica: %d instance: %d", site_id_, replica_id, instance_no);
  prepare_reqs.push_back(make_pair(replica_id, instance_no));
}

void EpaxosServer::PrepareAll() {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  unordered_map<uint64_t, uint64_t> last_instance;
  for (auto repl : cmds) {
    for (auto inst : repl.second) {
      uint64_t replica_id = repl.first;
      uint64_t instance_no = inst.first;
      last_instance[replica_id] = max(last_instance[replica_id], instance_no);
    }
  }
  last_instance[replica_id_] = next_instance_no-1;
  for (auto itr : last_instance) {
    uint64_t replica_id = itr.first;
    for (uint64_t instance_no = 0; instance_no < itr.second; instance_no++) {
      prepare_reqs.push_back(make_pair(replica_id, instance_no));
    }
  }
}

void EpaxosServer::PauseExecution(bool pause) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  pause_execution = pause;
}

EpaxosRequest EpaxosServer::CreateEpaxosRequest(shared_ptr<Marshallable>& cmd, string dkey) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  uint64_t instance_no = next_instance_no;
  next_instance_no = next_instance_no + 1;
  int64_t leader_dep_instance = -1;
  if (dkey_deps[dkey].count(replica_id_)) {
    leader_dep_instance = dkey_deps[dkey][replica_id_];
  }
  dkey_deps[dkey][replica_id_] = instance_no; // Important - otherwise next command may not have dependency on this command
  EpaxosBallot ballot = EpaxosBallot(curr_epoch, 0, replica_id_);
  return EpaxosRequest(cmd, dkey, ballot, replica_id_, instance_no, leader_dep_instance);
}

bool EpaxosServer::StartPreAccept(shared_ptr<Marshallable>& cmd_, 
                                  string dkey, 
                                  EpaxosBallot ballot, 
                                  uint64_t replica_id,
                                  uint64_t instance_no,
                                  int64_t leader_dep_instance,
                                  bool recovery) {
  mtx_.lock();
  Log_debug("Started pre-accept for request for replica: %d instance: %d dep_key: %s with leader_dep_instance: %d ballot: %d leader: %d by replica: %d", 
            replica_id, instance_no, dkey.c_str(), leader_dep_instance, ballot.ballot_no, ballot.replica_id, replica_id_);
  // Initialise attributes
  uint64_t seq = dkey_seq[dkey] + 1;
  unordered_map<uint64_t, uint64_t> deps = dkey_deps[dkey];
  if(leader_dep_instance >= 0) {
    deps[replica_id] = leader_dep_instance;
  } else {
    deps.erase(replica_id);
  }
  // Pre-accept command
  cmds[replica_id][instance_no].cmd = cmd_;
  cmds[replica_id][instance_no].dkey = dkey;
  cmds[replica_id][instance_no].seq = seq;
  cmds[replica_id][instance_no].deps = deps;
  cmds[replica_id][instance_no].highest_seen = ballot;
  cmds[replica_id][instance_no].state = EpaxosCommandState::PRE_ACCEPTED;
  // Update internal atributes
  if (cmd_->kind_ != MarshallDeputy::CMD_NOOP) {
    dkey_seq[dkey] = seq;
    dkey_deps[dkey][replica_id] = max(dkey_deps[dkey][replica_id], instance_no);
  }
  received_till[replica_id] = max(received_till[replica_id], instance_no);
  mtx_.unlock();

  auto ev = commo()->SendPreAccept(site_id_, 
                                   partition_id_, 
                                   recovery,
                                   ballot.epoch, 
                                   ballot.ballot_no, 
                                   ballot.replica_id, 
                                   replica_id,
                                   instance_no, 
                                   cmd_, 
                                   dkey, 
                                   seq, 
                                   deps);
  ev->Wait(1000000);

  Log_debug("Started pre-accept reply processing for replica: %d instance: %d dep_key: %s with leader_dep_instance: %d ballot: %d leader: %d by replica: %d", 
            replica_id, instance_no, dkey.c_str(), leader_dep_instance, ballot.ballot_no, ballot.replica_id, replica_id_);
  // Fail if timeout/no-majority
  bool status = false;
  if (ev->status_ == Event::TIMEOUT || ev->No()) {
    Log_debug("Pre-accept failed for replica: %d instance: %d by replica: %d", replica_id, instance_no, replica_id_);
    UpdateHighestSeenBallot(ev->replies, replica_id, instance_no);
  }
  // Success via fast path quorum
  else if (ev->FastPath()) {
    status = StartCommit(replica_id, instance_no);
  }
  // Success via slow path quorum
  else if (ev->SlowPath()) {
    UpdateAttributes(ev->replies, replica_id, instance_no);
    status = StartAccept(replica_id, instance_no);
  }
  else {
    verify(0);
  }
  return status;
}

EpaxosPreAcceptReply EpaxosServer::OnPreAcceptRequest(shared_ptr<Marshallable>& cmd_, 
                                                      string dkey, 
                                                      EpaxosBallot ballot, 
                                                      uint64_t seq_, 
                                                      unordered_map<uint64_t, uint64_t> deps_, 
                                                      uint64_t replica_id, 
                                                      uint64_t instance_no) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  Log_debug("Received pre-accept request for replica: %d instance: %d dep_key: %s with ballot: %d leader: %d by replica: %d", 
            replica_id, instance_no, dkey.c_str(), ballot.ballot_no, ballot.replica_id, replica_id_);
  EpaxosPreAcceptStatus status = EpaxosPreAcceptStatus::IDENTICAL;
  // Reject older ballots
  if (!ballot.isGreater(cmds[replica_id][instance_no].highest_seen)) {
    status = EpaxosPreAcceptStatus::FAILED;
    EpaxosBallot &highest_seen = cmds[replica_id][instance_no].highest_seen;
    EpaxosPreAcceptReply reply(status, highest_seen.epoch, highest_seen.ballot_no, highest_seen.replica_id);
    return reply;
  }
  /* 
     - If it was pre-accepted by majority identically then it will never come to pre-accept phase again and instead go to accept phase.
     - If it was accepted by majority of replica then it will never come to pre-accept phase again and instead go to accept phase.
     - If it was committed in some other replica then it will never come to pre-accept phase again and instead go to accept phase.
     - If already committed or executed in this replica then it is just an delayed message so fail it (same as ignore). 
     - If already accepted in this replica then it means it was pre-accepted by some majority non-identically and accepted by the 
       leader. So prepare should have tried to accept this message again. So reject it so that prepare will try again.
       If new cmd is NOOP, then it will overwrite the accept when commit request comes. */
  if (cmds[replica_id][instance_no].state == EpaxosCommandState::ACCEPTED 
      || cmds[replica_id][instance_no].state == EpaxosCommandState::COMMITTED
      || cmds[replica_id][instance_no].state == EpaxosCommandState::EXECUTED) {
    status = EpaxosPreAcceptStatus::FAILED;
    cmds[replica_id][instance_no].highest_seen = ballot; // Verify: needed?
    EpaxosPreAcceptReply reply(status, ballot.epoch, ballot.ballot_no, ballot.replica_id);
    return reply;
  }
  // Initialise attributes
  uint64_t seq = dkey_seq[dkey] + 1;
  seq = max(seq, seq_);
  if (seq != seq_) {
    status = EpaxosPreAcceptStatus::NON_IDENTICAL;
  }
  auto deps = deps_;
  for (auto itr : dkey_deps[dkey]) {
    uint64_t dreplica_id = itr.first;
    uint64_t dinstance_no = itr.second;
    if (dreplica_id == replica_id) continue;
    if (deps.count(dreplica_id) == 0 || dinstance_no > deps[dreplica_id]) {
      deps[dreplica_id] = dinstance_no;
      status = EpaxosPreAcceptStatus::NON_IDENTICAL;
    }
  }
  // Pre-accept command
  cmds[replica_id][instance_no].cmd = cmd_;
  cmds[replica_id][instance_no].dkey = dkey;
  cmds[replica_id][instance_no].seq = seq;
  cmds[replica_id][instance_no].deps = deps;
  cmds[replica_id][instance_no].highest_seen = ballot;
  cmds[replica_id][instance_no].state = EpaxosCommandState::PRE_ACCEPTED;
  // Update internal attributes
  if (cmd_->kind_ != MarshallDeputy::CMD_NOOP) {
    dkey_seq[dkey] = cmds[replica_id][instance_no].seq;
    int64_t leader_dep_instance = max(dkey_deps[dkey][replica_id], instance_no);
    dkey_deps[dkey] = cmds[replica_id][instance_no].deps;
    dkey_deps[dkey][replica_id] = leader_dep_instance;
    received_till[replica_id] = max(received_till[replica_id], dkey_deps[dkey][replica_id]);
  }
  received_till[replica_id] = max(received_till[replica_id], instance_no);
  // Reply
  EpaxosPreAcceptReply reply(status, ballot.epoch, ballot.ballot_no, ballot.replica_id, seq, deps);
  return reply;
}

bool EpaxosServer::StartAccept(uint64_t replica_id, uint64_t instance_no) {
  mtx_.lock();
  // Accept command
  Log_debug("Started accept request for replica: %d instance: %d by replica: %d", replica_id, instance_no, replica_id_);
  cmds[replica_id][instance_no].state = EpaxosCommandState::ACCEPTED;
  EpaxosCommand cmd = cmds[replica_id][instance_no];
  mtx_.unlock();
  
  auto ev = commo()->SendAccept(site_id_, 
                                partition_id_, 
                                cmd.highest_seen.epoch, 
                                cmd.highest_seen.ballot_no, 
                                cmd.highest_seen.replica_id, 
                                replica_id,
                                instance_no, 
                                cmd.cmd, 
                                cmd.dkey, 
                                cmd.seq, 
                                cmd.deps);
  ev->Wait(1000000);

  Log_debug("Started accept reply processing for replica: %d instance: %d by replica: %d", replica_id, instance_no, replica_id_);
  // Fail if timeout/no-majority
  if (ev->status_ == Event::TIMEOUT || ev->No()) {
    Log_debug("Accept failed for replica: %d instance: %d by replica: %d", replica_id, instance_no, replica_id_);
    UpdateHighestSeenBallot(ev->replies, replica_id, instance_no);
    return false;
  }
  // Success
  if (ev->Yes()) {
    return StartCommit(replica_id, instance_no);
  }
  verify(0);
  return false;
}

EpaxosAcceptReply EpaxosServer::OnAcceptRequest(shared_ptr<Marshallable>& cmd_, 
                                                string dkey, 
                                                EpaxosBallot ballot, 
                                                uint64_t seq,
                                                unordered_map<uint64_t, uint64_t> deps, 
                                                uint64_t replica_id, 
                                                uint64_t instance_no) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  Log_debug("Received accept request for replica: %d instance: %d by replica: %d", replica_id, instance_no, replica_id_);
  // Reject older ballots
  if (!ballot.isGreaterOrEqual(cmds[replica_id][instance_no].highest_seen)) {
    EpaxosBallot &highest_seen = cmds[replica_id][instance_no].highest_seen;
    EpaxosAcceptReply reply(false, highest_seen.epoch, highest_seen.ballot_no, highest_seen.replica_id);
    return reply;
  }
  // Accept command
  /* 
     - If already committed or executed in this replica then it is just an delayed message so fail it (same as ignore). 
     - If already accepted in this replica then it can be still overwritten because majority haven't agreed to it identically. */
  if (cmds[replica_id][instance_no].state == EpaxosCommandState::COMMITTED 
      || cmds[replica_id][instance_no].state == EpaxosCommandState::EXECUTED) {
    cmds[replica_id][instance_no].highest_seen = ballot;
    EpaxosAcceptReply reply(false, ballot.epoch, ballot.ballot_no, ballot.replica_id);
    return reply;
  }
  cmds[replica_id][instance_no].cmd = cmd_;
  cmds[replica_id][instance_no].dkey = dkey;
  cmds[replica_id][instance_no].seq = seq;
  cmds[replica_id][instance_no].deps = deps;
  cmds[replica_id][instance_no].highest_seen = ballot;
  cmds[replica_id][instance_no].state = EpaxosCommandState::ACCEPTED;
  // Update internal attributes
  UpdateInternalAttributes(cmd_, dkey, replica_id, instance_no, seq, deps);
  // Reply
  EpaxosAcceptReply reply(true, ballot.epoch, ballot.ballot_no, ballot.replica_id);
  return reply;
}

bool EpaxosServer::StartCommit(uint64_t replica_id, uint64_t instance_no) {
  mtx_.lock();
  Log_debug("Started commit request for replica: %d instance: %d by replica: %d", replica_id, instance_no, replica_id_);
  // Commit command if not committed
  if (cmds[replica_id][instance_no].state != EpaxosCommandState::EXECUTED) {
    cmds[replica_id][instance_no].state = EpaxosCommandState::COMMITTED;
  }
  EpaxosCommand cmd = cmds[replica_id][instance_no];
  mtx_.unlock();
  // TODO: reply to client
  auto ev = commo()->SendCommit(site_id_, 
                                partition_id_, 
                                cmd.highest_seen.epoch, 
                                cmd.highest_seen.ballot_no, 
                                cmd.highest_seen.replica_id,
                                replica_id,
                                instance_no, 
                                cmd.cmd, 
                                cmd.dkey, 
                                cmd.seq, 
                                cmd.deps);
  ev->Wait(1000000);

  return true;
}

void EpaxosServer::OnCommitRequest(shared_ptr<Marshallable>& cmd_, 
                                   string dkey, 
                                   EpaxosBallot ballot, 
                                   uint64_t seq,
                                   unordered_map<uint64_t, uint64_t> deps, 
                                   uint64_t replica_id, 
                                   uint64_t instance_no) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  Log_debug("Received commit request for replica: %d instance: %d by replica: %d", replica_id, instance_no, replica_id_);
  // Use the latest ballot
  if (!ballot.isGreater(cmds[replica_id][instance_no].highest_seen)) {
    ballot = cmds[replica_id][instance_no].highest_seen;
  }
  // Commit command if not committed
  if (cmds[replica_id][instance_no].state == EpaxosCommandState::COMMITTED
      || cmds[replica_id][instance_no].state == EpaxosCommandState::EXECUTED) {
    return;
  }
  cmds[replica_id][instance_no].cmd = cmd_;
  cmds[replica_id][instance_no].dkey = dkey;
  cmds[replica_id][instance_no].seq = seq;
  cmds[replica_id][instance_no].deps = deps;
  cmds[replica_id][instance_no].highest_seen = ballot;
  cmds[replica_id][instance_no].state = EpaxosCommandState::COMMITTED;
  // Update internal attributes
  UpdateInternalAttributes(cmd_, dkey, replica_id, instance_no, seq, deps);
}

bool EpaxosServer::StartPrepare(uint64_t replica_id, uint64_t instance_no) {
  mtx_.lock();
  if (cmds[replica_id][instance_no].state == EpaxosCommandState::COMMITTED
      || cmds[replica_id][instance_no].state == EpaxosCommandState::EXECUTED) {
    mtx_.unlock();
    return true;
  }
  // Get ballot = highest seen ballot
  EpaxosBallot ballot = EpaxosBallot(curr_epoch, 0, replica_id_);
  // Create prepare reply from self
  shared_ptr<Marshallable> NOOP_CMD = dynamic_pointer_cast<Marshallable>(make_shared<TpcNoopCommand>());
  EpaxosPrepareReply self_reply(true, NOOP_CMD, NOOP_DKEY, 0, unordered_map<uint64_t, uint64_t>(), EpaxosCommandState::NOT_STARTED, replica_id_, 0, -1, 0);
  if (cmds[replica_id][instance_no].state != EpaxosCommandState::NOT_STARTED) {
    self_reply = EpaxosPrepareReply(true, 
                                    cmds[replica_id][instance_no].cmd, 
                                    cmds[replica_id][instance_no].dkey, 
                                    cmds[replica_id][instance_no].seq,
                                    cmds[replica_id][instance_no].deps,
                                    cmds[replica_id][instance_no].state,
                                    replica_id_,
                                    cmds[replica_id][instance_no].highest_seen.epoch,
                                    cmds[replica_id][instance_no].highest_seen.ballot_no,
                                    cmds[replica_id][instance_no].highest_seen.replica_id);
  }
  ballot.ballot_no = max(ballot.ballot_no, cmds[replica_id][instance_no].highest_seen.ballot_no) + 1;
  cmds[replica_id][instance_no].highest_seen = ballot;
  Log_debug("Started prepare for replica: %d instance: %d by replica: %d for ballot: %d", replica_id, instance_no, replica_id_, ballot.ballot_no);
  mtx_.unlock();
  
  auto ev = commo()->SendPrepare(site_id_, 
                                partition_id_, 
                                ballot.epoch, 
                                ballot.ballot_no, 
                                ballot.replica_id, 
                                replica_id,
                                instance_no);
  ev->Wait(1000000);

  Log_debug("Started prepare reply processing for replica: %d instance: %d by replica: %d", replica_id, instance_no, replica_id_);
  if (ev->status_ == Event::TIMEOUT || ev->No()) {
    Log_debug("Prepare failed for replica: %d instance: %d by replica: %d", replica_id, instance_no, replica_id_);
    UpdateHighestSeenBallot(ev->replies, replica_id, instance_no);
    return false;
  }
  // Add self reply
  ev->replies.push_back(self_reply);
  // Get set of replies with highest ballot
  vector<EpaxosPrepareReply> highest_ballot_replies;
  EpaxosBallot highest_ballot = EpaxosBallot();
  for (auto reply : ev->replies) {
    if (reply.status && reply.cmd_state != EpaxosCommandState::NOT_STARTED) {
      EpaxosBallot reply_ballot = EpaxosBallot(reply.epoch, reply.ballot_no, reply.replica_id);
      if (reply_ballot.isGreater(highest_ballot)) { // VERIFY: If 2 can have diff replica ids and cause issue
        highest_ballot = reply_ballot;
        highest_ballot_replies = vector<EpaxosPrepareReply>();
        highest_ballot_replies.push_back(reply);
      } else if (reply_ballot == highest_ballot) {
        highest_ballot_replies.push_back(reply);
      }
    }
  }
  // Check if the highest ballot seen is same as default ballot
  bool is_default_ballot = highest_ballot.isDefault(); 
  // Get all unique commands and their counts
  vector<EpaxosRecoveryCommand> unique_cmds;
  EpaxosPrepareReply any_preaccepted_reply;
  EpaxosPrepareReply any_accepted_reply;
  for (auto reply : highest_ballot_replies) {
    // Atleast one commited reply
    if (reply.cmd_state == EpaxosCommandState::COMMITTED || reply.cmd_state == EpaxosCommandState::EXECUTED) {
      Log_debug("Prepare - committed cmd found for replica: %d instance: %d by replica: %d from acceptor: %d", replica_id, instance_no, replica_id_, reply.acceptor_replica_id);
      UpdateInternalAttributes(reply.cmd, reply.dkey, replica_id, instance_no, reply.seq, reply.deps);
      mtx_.lock();
      cmds[replica_id][instance_no].cmd = reply.cmd;
      cmds[replica_id][instance_no].dkey = reply.dkey;
      cmds[replica_id][instance_no].seq = reply.seq;
      cmds[replica_id][instance_no].deps = reply.deps;
      cmds[replica_id][instance_no].highest_seen = ballot;
      mtx_.unlock();
      return StartCommit(replica_id, instance_no);
    }
    // Atleast one accept reply
    if (reply.cmd_state == EpaxosCommandState::ACCEPTED) {
      Log_debug("Prepare - accepted cmd found for replica: %d instance: %d by replica: %d from acceptor: %d", replica_id, instance_no, replica_id_, reply.acceptor_replica_id);
      UpdateInternalAttributes(reply.cmd, reply.dkey, replica_id, instance_no, reply.seq, reply.deps);
      any_accepted_reply = reply;
    }
    // Atleast one pre-accept reply
    if (reply.cmd_state == EpaxosCommandState::PRE_ACCEPTED && any_accepted_reply.cmd_state != EpaxosCommandState::ACCEPTED) {
      any_preaccepted_reply = reply;
      Log_debug("Prepare - pre-accepted cmd found for replica: %d instance: %d by replica: %d from acceptor: %d", replica_id, instance_no, replica_id_, reply.acceptor_replica_id);
      UpdateInternalAttributes(reply.cmd, reply.dkey, replica_id, instance_no, reply.seq, reply.deps);
      // Checking for N/2 identical replies for default ballot
      if (is_default_ballot && reply.acceptor_replica_id != replica_id) {
        bool found = false;
        for (auto &cmd : unique_cmds) {
          if (cmd.cmd->kind_ == reply.cmd->kind_ && cmd.seq == reply.seq && cmd.deps == reply.deps) {
            cmd.count++;
            Log_debug("Prepare - identical %d pre-accepted cmd found for replica: %d instance: %d by replica: %d", cmd.count, replica_id, instance_no, replica_id_);
            found = true;
            break;
          }
        }
        if (!found) {
          EpaxosRecoveryCommand rec_cmd(reply.cmd, reply.dkey, reply.seq, reply.deps, 1);
          unique_cmds.push_back(rec_cmd);
        }
      }
    }
  }
  // Atleast one accepted reply - start phase accept
  if (any_accepted_reply.cmd_state == EpaxosCommandState::ACCEPTED) {
    mtx_.lock();
    cmds[replica_id][instance_no] = EpaxosCommand(any_accepted_reply.cmd, any_accepted_reply.dkey, any_accepted_reply.seq, any_accepted_reply.deps, ballot, EpaxosCommandState::ACCEPTED);
    mtx_.unlock();
    return StartAccept(replica_id, instance_no);
  }
  // N/2 identical pre-accepted replies for default ballot
  for (auto rec_cmd : unique_cmds) {
    int n_total = commo()->rpc_par_proxies_[partition_id_].size();
    if (rec_cmd.count >= n_total/2) {
      mtx_.lock();
      Log_debug("Prepare - identical pre-accepted cmd found for replica: %d instance: %d by replica: %d", replica_id, instance_no, replica_id_);
      cmds[replica_id][instance_no] = EpaxosCommand(rec_cmd.cmd, rec_cmd.dkey, rec_cmd.seq, rec_cmd.deps, ballot, EpaxosCommandState::PRE_ACCEPTED);
      mtx_.unlock();
      return StartAccept(replica_id, instance_no);
    }
  }
  // Atleast one pre-accepted reply - start phase pre-accept
  Log_debug("Prepare - Atleast one pre-accepted cmd found for replica: %d instance: %d by replica: %d", replica_id, instance_no, replica_id_);
  if (any_preaccepted_reply.cmd_state == EpaxosCommandState::PRE_ACCEPTED) {
    uint64_t leader_deps_instance = any_preaccepted_reply.deps.count(replica_id) ? any_preaccepted_reply.deps[replica_id] : -1;
    return StartPreAccept(any_preaccepted_reply.cmd, any_preaccepted_reply.dkey, ballot, replica_id, instance_no, leader_deps_instance, true);
  }
  // No pre-accepted replies - start phase pre-accept with NO_OP
  Log_debug("Prepare - No pre-accepted cmd found for replica: %d instance: %d by replica: %d", replica_id, instance_no, replica_id_);
  return StartPreAccept(NOOP_CMD, NOOP_DKEY, ballot, replica_id, instance_no, -1, true);
}

void EpaxosServer::PrepareTillCommitted(uint64_t replica_id, uint64_t instance_no) {
  Log_debug("Prepare till committed replica: %d instance: %d in replica: %d", replica_id, instance_no, replica_id_);
  // Wait till timeout or concurrent prepare succeeds
  mtx_.lock();
  bool prepare_ready = chrono::system_clock::now() - cmds[replica_id][instance_no].received_time > chrono::milliseconds{10}
                       && !cmds[replica_id][instance_no].preparing;
  mtx_.unlock();
  while (!prepare_ready) {
    Log_debug("Waiting for replica: %d instance: %d in replica: %d", replica_id, instance_no, replica_id_);
    Coroutine::Sleep(100000);
    mtx_.lock();
    prepare_ready = chrono::system_clock::now() - cmds[replica_id][instance_no].received_time > chrono::milliseconds{10}
                    && !cmds[replica_id][instance_no].preparing;
    mtx_.unlock();
  }
  mtx_.lock();
  if (cmds[replica_id][instance_no].state == EpaxosCommandState::COMMITTED
        || cmds[replica_id][instance_no].state == EpaxosCommandState::EXECUTED) {
    mtx_.unlock();
    return;
  }
  cmds[replica_id][instance_no].preparing = true;
  mtx_.unlock();
  // Wait till prepare succeeds
  bool committed = StartPrepare(replica_id, instance_no);
  while (!committed) {
    Coroutine::Sleep(100000 + (rand() % 50000));
    committed = StartPrepare(replica_id, instance_no);
  }
  mtx_.lock();
  cmds[replica_id][instance_no].preparing = false;
  mtx_.unlock();
}

EpaxosPrepareReply EpaxosServer::OnPrepareRequest(EpaxosBallot ballot, uint64_t replica_id, uint64_t instance_no) {
  Log_debug("Received prepare request for replica: %d instance: %d in replica: %d for ballot: %d from new leader: %d", replica_id, instance_no, replica_id_, ballot.ballot_no, ballot.replica_id);
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  shared_ptr<Marshallable> NOOP_CMD = dynamic_pointer_cast<Marshallable>(make_shared<TpcNoopCommand>());
  EpaxosPrepareReply reply(true, NOOP_CMD, NOOP_DKEY, 0, unordered_map<uint64_t, uint64_t>(), EpaxosCommandState::NOT_STARTED, replica_id_, 0, -1, 0);
  // Reject older ballots
  if (!ballot.isGreater(cmds[replica_id][instance_no].highest_seen)) {
    reply.status = false;
    reply.epoch = cmds[replica_id][instance_no].highest_seen.epoch;
    reply.ballot_no = cmds[replica_id][instance_no].highest_seen.ballot_no;
    reply.replica_id = cmds[replica_id][instance_no].highest_seen.replica_id;
    return reply;
  }
  if (cmds[replica_id][instance_no].state != EpaxosCommandState::NOT_STARTED) {
    reply = EpaxosPrepareReply(true, 
                               cmds[replica_id][instance_no].cmd, 
                               cmds[replica_id][instance_no].dkey, 
                               cmds[replica_id][instance_no].seq,
                               cmds[replica_id][instance_no].deps,
                               cmds[replica_id][instance_no].state,
                               replica_id_,
                               cmds[replica_id][instance_no].highest_seen.epoch,
                               cmds[replica_id][instance_no].highest_seen.ballot_no,
                               cmds[replica_id][instance_no].highest_seen.replica_id);
  }
  return reply;
}

int EpaxosServer::CreateEpaxosGraph(uint64_t replica_id, uint64_t instance_no, EpaxosGraph *graph) {
  mtx_.lock();
  Log_debug("Adding to graph replica: %d instance: %d by replica: %d", replica_id, instance_no, replica_id_);
  if (cmds[replica_id][instance_no].state != EpaxosCommandState::COMMITTED
      && cmds[replica_id][instance_no].state != EpaxosCommandState::EXECUTED) {
    mtx_.unlock();
    PrepareTillCommitted(replica_id, instance_no);
    mtx_.lock();
  }
  if (cmds[replica_id][instance_no].state == EpaxosCommandState::EXECUTED) {
    mtx_.unlock();
    return 1;
  }
  if (cmds[replica_id][instance_no].cmd->kind_ == MarshallDeputy::CMD_NOOP) {
    mtx_.unlock();
    return 0;
  }
  shared_ptr<EpaxosVertex> parent = make_shared<EpaxosVertex>(&cmds[replica_id][instance_no], replica_id, instance_no);
  bool exists = graph->FindOrCreateVertex(parent);
  if (exists) {
    mtx_.unlock();
    return 2;
  }
  for (auto itr : parent->cmd->deps) {
    uint64_t dreplica_id = itr.first;
    uint64_t dinstance_no = itr.second;
    mtx_.unlock();
    int status = CreateEpaxosGraph(dreplica_id, dinstance_no, graph);
    mtx_.lock();
    if (status == 0) {
      int64_t prev_instance_no = dinstance_no - 1;
      while (prev_instance_no >= 0) {
        if (cmds[dreplica_id][prev_instance_no].state == EpaxosCommandState::NOT_STARTED) {
          mtx_.unlock();
          PrepareTillCommitted(dreplica_id, prev_instance_no);
          mtx_.lock();
        }
        if (cmds[dreplica_id][prev_instance_no].dkey == parent->cmd->dkey) {
          break;
        }
        prev_instance_no--;
      }
      if (prev_instance_no < 0) continue;
      mtx_.unlock();
      status = CreateEpaxosGraph(dreplica_id, prev_instance_no, graph);
      mtx_.lock();
      dinstance_no = prev_instance_no;
    }
    if (status == 1) continue;
    shared_ptr<EpaxosVertex> child = make_shared<EpaxosVertex>(&cmds[dreplica_id][dinstance_no], dreplica_id, dinstance_no);
    graph->FindOrCreateParentEdge(child, parent);
  }
  mtx_.unlock();
  return 2;
}

void EpaxosServer::StartExecution(uint64_t replica_id, uint64_t instance_no) {
  mtx_.lock();
  Log_debug("Received execution request for replica: %d instance: %d by replica: %d", replica_id, instance_no, replica_id_);
  bool should_wait = cmds[replica_id][instance_no].state != EpaxosCommandState::NOT_STARTED
                     && in_process_dkeys.count(cmds[replica_id][instance_no].dkey) > 0;
  mtx_.unlock();
  while (should_wait) {
    Coroutine::Sleep(10000);
    mtx_.lock();
    should_wait = cmds[replica_id][instance_no].state != EpaxosCommandState::NOT_STARTED
                  && in_process_dkeys.count(cmds[replica_id][instance_no].dkey) > 0;
    mtx_.unlock();
  }
  mtx_.lock();
  in_process_dkeys.insert(cmds[replica_id][instance_no].dkey);
  mtx_.unlock();

  EpaxosGraph graph = EpaxosGraph();
  CreateEpaxosGraph(replica_id, instance_no, &graph);
  auto sorted_vertices = graph.GetSortedVertices();
  Log_debug("Added to graph replica: %d instance: %d by replica: %d", replica_id, instance_no, replica_id_);
  std::lock_guard<std::recursive_mutex> lock(mtx_); // can move lock inside if required
  for (auto vertex : sorted_vertices) {
    if (vertex->cmd->state != EpaxosCommandState::EXECUTED) {
      vertex->cmd->state = EpaxosCommandState::EXECUTED;
      app_next_(*(vertex->cmd->cmd));
      Log_debug("Executed replica: %d instance: %d in replica: %d", replica_id, instance_no, replica_id_);
    }
  }
  Log_debug("Completed replica: %d instance: %d by replica: %d", replica_id, instance_no, replica_id_);
  in_process_dkeys.erase(cmds[replica_id][instance_no].dkey);
}

template<class ClassT>
void EpaxosServer::UpdateHighestSeenBallot(vector<ClassT>& replies, uint64_t replica_id, uint64_t instance_no) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  EpaxosBallot highest_seen_ballot = cmds[replica_id][instance_no].highest_seen;
  for (auto reply : replies) {
    EpaxosBallot ballot(reply.epoch, reply.ballot_no, reply.replica_id);
    if (ballot.isGreater(highest_seen_ballot)) {
      highest_seen_ballot = ballot;
    }
  }
  cmds[replica_id][instance_no].highest_seen = highest_seen_ballot;
}

void EpaxosServer::UpdateAttributes(vector<EpaxosPreAcceptReply>& replies, uint64_t replica_id, uint64_t instance_no) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  EpaxosCommand &cmd = cmds[replica_id][instance_no];
  if (cmd.cmd->kind_ == MarshallDeputy::CMD_NOOP) {
    return;
  }
  for (auto reply : replies) {
    cmd.seq = max(cmd.seq, reply.seq);
    dkey_seq[cmd.dkey] = max(dkey_seq[cmd.dkey], reply.seq);
    for (auto itr : reply.deps) {
      uint64_t dreplica_id = itr.first;
      uint64_t dinstance_no = itr.second;
      cmd.deps[dreplica_id] = max(cmd.deps[dreplica_id], dinstance_no);
      dkey_deps[cmd.dkey][dreplica_id] = max(dkey_deps[cmd.dkey][dreplica_id], dinstance_no);
      received_till[dreplica_id] = max(received_till[dreplica_id], dinstance_no);
    }
  }
}

void EpaxosServer::UpdateInternalAttributes(shared_ptr<Marshallable> &cmd,
                                            string dkey, 
                                            uint64_t replica_id, 
                                            uint64_t instance_no, 
                                            uint64_t seq, 
                                            unordered_map<uint64_t, uint64_t> deps) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  received_till[replica_id] = max(received_till[replica_id], instance_no);
  if (cmd->kind_ == MarshallDeputy::CMD_NOOP) {
    return;
  }                                           
  dkey_seq[dkey] = max(dkey_seq[dkey], seq);
  for (auto itr : deps) {
    uint64_t dreplica_id = itr.first;
    uint64_t dinstance_no = itr.second;
    dkey_deps[dkey][dreplica_id] = max(dkey_deps[dkey][dreplica_id], dinstance_no);
    received_till[dreplica_id] = max(received_till[dreplica_id], dinstance_no);
  }
  dkey_deps[dkey][replica_id] = max(dkey_deps[dkey][replica_id], instance_no);
}

/* Do not modify any code below here */

void EpaxosServer::Disconnect(const bool disconnect) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  verify(disconnected_ != disconnect);
  // global map of rpc_par_proxies_ values accessed by partition then by site
  static map<parid_t, map<siteid_t, map<siteid_t, vector<SiteProxyPair>>>> _proxies{};
  if (_proxies.find(partition_id_) == _proxies.end()) {
    _proxies[partition_id_] = {};
  }
  EpaxosCommo *c = (EpaxosCommo*) commo();
  if (disconnect) {
    verify(_proxies[partition_id_][loc_id_].size() == 0);
    verify(c->rpc_par_proxies_.size() > 0);
    auto sz = c->rpc_par_proxies_.size();
    _proxies[partition_id_][loc_id_].insert(c->rpc_par_proxies_.begin(), c->rpc_par_proxies_.end());
    c->rpc_par_proxies_ = {};
    verify(_proxies[partition_id_][loc_id_].size() == sz);
    verify(c->rpc_par_proxies_.size() == 0);
  } else {
    verify(_proxies[partition_id_][loc_id_].size() > 0);
    auto sz = _proxies[partition_id_][loc_id_].size();
    c->rpc_par_proxies_ = {};
    c->rpc_par_proxies_.insert(_proxies[partition_id_][loc_id_].begin(), _proxies[partition_id_][loc_id_].end());
    _proxies[partition_id_][loc_id_] = {};
    verify(_proxies[partition_id_][loc_id_].size() == 0);
    verify(c->rpc_par_proxies_.size() == sz);
  }
  disconnected_ = disconnect;
}

bool EpaxosServer::IsDisconnected() {
  return disconnected_;
}

} // namespace janus
