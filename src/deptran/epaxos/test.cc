#include "test.h"
#include "test_graph.cc"

namespace janus {

#ifdef EPAXOS_TEST_CORO

int EpaxosLabTest::Run(void) {
  Print("START UNIT TESTS");
  TestGraph test_graph;
  test_graph.Run();
  Print("ALL TESTS PASSED\n");

  Print("START WHOLISTIC TESTS");
  config_->SetLearnerAction();
  uint64_t start_rpc = config_->RpcTotal();
  if (testBasicAgree()
      || testFastPathIndependentAgree()
      || testFastPathDependentAgree()
      || testSlowPathIndependentAgree()
      || testSlowPathDependentAgree()
      || testFailNoQuorum()
      || testNonIdenticalAttrsAgree()
      || testPrepareCommittedCommandAgree()
      || testPrepareAcceptedCommandAgree()
      || testPreparePreAcceptedCommandAgree()
      || testPrepareNoopCommandAgree()
      || testConcurrentAgree()
      || testConcurrentUnreliableAgree1()
      // || testConcurrentUnreliableAgree2()
      // || testExecutionOrder()
    ) {
    Print("TESTS FAILED");
    return 1;
  }
  Print("ALL TESTS PASSED");
  Print("Total RPC count: %ld", config_->RpcTotal() - start_rpc);
  return 0;
}

void EpaxosLabTest::Cleanup(void) {
  config_->Shutdown();
}

#define Init2(test_id, description) { \
        Init(test_id, description); \
        cmd = ((cmd / 100) + 1) * 100; \
        verify(config_->NDisconnected() == 0 && !config_->IsUnreliable() && !config_->AnySlow()); \
      }

#define InitSub2(sub_test_id, description) { \
        InitSub(sub_test_id, description); \
        verify(config_->NDisconnected() == 0 && !config_->IsUnreliable() && !config_->AnySlow()); \
      }

#define Passed2() { \
        Passed(); \
        return 0; \
      }

#define Assert(expr) if (!(expr)) { \
        return 1; \
      }

#define Assert2(expr, msg, ...) if (!(expr)) { \
        Failed(msg, ##__VA_ARGS__); \
        return 1; \
      }

#define AssertNoneExecuted(cmd) { \
        auto ne = config_->NExecuted(cmd, NSERVERS); \
        Assert2(ne == 0, "%d servers unexpectedly executed command %d", ne, cmd); \
      }

#define AssertNExecuted(cmd, expected) { \
        auto ne = config_->NExecuted(cmd, expected); \
        Assert2(ne == expected, "%d servers executed command %d (%d expected)", ne, cmd, expected) \
      }

#define AssertExecutedOrder(expected_exec_order) { \
        auto r = config_->ExecutedInOrder(expected_exec_order); \
        Assert2(r, "unexpected execution order of commands"); \
      }

#define AssertValidCommitStatus(replica_id, instance_no, r) { \
          Assert2(r != -1, "failed to reach agreement for replica: %d instance: %d, committed different commands", replica_id, instance_no); \
          Assert2(r != -2, "failed to reach agreement for replica: %d instance: %d, committed different dkey", replica_id, instance_no); \
          Assert2(r != -3, "failed to reach agreement for replica: %d instance: %d, committed different seq", replica_id, instance_no); \
          Assert2(r != -4, "failed to reach agreement for replica: %d instance: %d, committed different deps", replica_id, instance_no); \
        }

#define AssertNCommitted(replica_id, instance_no, n) { \
        auto r = config_->NCommitted(replica_id, instance_no, n); \
        Assert2(r >= n, "failed to reach agreement for replica: %d instance: %d among %d servers", replica_id, instance_no, n); \
        AssertValidCommitStatus(replica_id, instance_no, r); \
      }

#define AssertNCommittedAndVerifyNoop(replica_id, instance_no, n, noop) { \
        bool cnoop; \
        string cdkey; \
        uint64_t cseq; \
        unordered_map<uint64_t, uint64_t> cdeps; \
        auto r = config_->NCommitted(replica_id, instance_no, n, &cnoop, &cdkey, &cseq, &cdeps); \
        Assert2(r >= n, "failed to reach agreement for replica: %d instance: %d among %d servers", replica_id, instance_no, n); \
        AssertValidCommitStatus(replica_id, instance_no, r); \
        Assert2(cnoop == noop || !noop, "failed to reach agreement for replica: %d instance: %d, expected noop, got command", replica_id, instance_no); \
        Assert2(cnoop == noop || noop, "failed to reach agreement for replica: %d instance: %d, expected command, got noop", replica_id, instance_no); \
      }
      
#define AssertNCommittedAndVerifyAttrs(replica_id, instance_no, n, noop, exp_dkey, exp_seq, exp_deps) { \
        bool cnoop; \
        string cdkey; \
        uint64_t cseq; \
        unordered_map<uint64_t, uint64_t> cdeps; \
        auto r = config_->NCommitted(replica_id, instance_no, n, &cnoop, &cdkey, &cseq, &cdeps); \
        Assert2(r >= n, "failed to reach agreement for replica: %d instance: %d among %d servers", replica_id, instance_no, n); \
        AssertValidCommitStatus(replica_id, instance_no, r); \
        Assert2(cnoop == noop || !noop, "failed to reach agreement for replica: %d instance: %d, expected noop, got command", replica_id, instance_no); \
        Assert2(cnoop == noop || noop, "failed to reach agreement for replica: %d instance: %d, expected command, got noop", replica_id, instance_no); \
        Assert2(cdkey == exp_dkey, "failed to reach agreement for replica: %d instance: %d, expected dkey %s, got dkey %s", replica_id, instance_no, exp_dkey.c_str(), cdkey.c_str()); \
        Assert2(cseq == exp_seq, "failed to reach agreement for replica: %d instance: %d, expected seq %d, got seq %d", replica_id, instance_no, exp_seq, cseq); \
        Assert2(cdeps == exp_deps, "failed to reach agreement for replica: %d instance: %d, expected deps %s different from committed deps %s", replica_id, instance_no, map_to_string(exp_deps).c_str(), map_to_string(cdeps).c_str()); \
      }

#define DoAgreeAndAssertNCommittedAndVerifyAttrs(cmd, dkey, n, noop, exp_dkey, exp_seq, exp_deps) { \
        bool cnoop; \
        string cdkey; \
        uint64_t cseq; \
        unordered_map<uint64_t, uint64_t> cdeps; \
        auto r = config_->DoAgreement(cmd, dkey, n, false, &cnoop, &cdkey, &cseq, &cdeps); \
        Assert2(r >= 0, "failed to reach agreement for command %d among %d servers", cmd, n); \
        Assert2(r != -1, "failed to reach agreement for command %d, committed different commands", cmd); \
        Assert2(r != -2, "failed to reach agreement for command %d, committed different dkey", cmd); \
        Assert2(r != -3, "failed to reach agreement for command %d, committed different seq", cmd); \
        Assert2(r != -4, "failed to reach agreement for command %d, committed different deps", cmd); \
        Assert2(cnoop == noop || !noop, "failed to reach agreement for command %d, expected noop, got command", cmd); \
        Assert2(cnoop == noop || noop, "failed to reach agreement for command %d, expected command, got noop", cmd); \
        Assert2(cdkey == exp_dkey, "failed to reach agreement for command %d, expected dkey %s, got dkey %s", cmd, exp_dkey.c_str(), cdkey.c_str()); \
        Assert2(cseq == exp_seq, "failed to reach agreement for command %d, expected seq %d, got seq %d", cmd, exp_seq, cseq); \
        Assert2(cdeps == exp_deps, "failed to reach agreement for command %d, expected deps %s different from committed deps %s", cmd, map_to_string(exp_deps).c_str(), map_to_string(cdeps).c_str()); \
      }
      
#define DoAgreeAndAssertNoneCommitted(cmd, dkey) { \
        bool cnoop; \
        string cdkey; \
        uint64_t cseq; \
        unordered_map<uint64_t, uint64_t> cdeps; \
        auto r = config_->DoAgreement(cmd, dkey, 1, false, &cnoop, &cdkey, &cseq, &cdeps); \
        Assert2(r == 0, "committed command %d without majority", cmd); \
      }

int EpaxosLabTest::testBasicAgree(void) {
  Init2(1, "Basic agreement");
  for (int i = 1; i <= 3; i++) {
    // complete agreement and make sure its attributes are as expected
    cmd++;
    string dkey = to_string(cmd);
    unordered_map<uint64_t, uint64_t> deps;
    DoAgreeAndAssertNCommittedAndVerifyAttrs(cmd, dkey, NSERVERS, false, dkey, 1, deps);
    AssertNExecuted(cmd, NSERVERS);
  }
  Passed2();
}

int EpaxosLabTest::testFastPathIndependentAgree(void) {
  Init2(2, "Fast path agreement of independent commands");
  config_->Disconnect(0);
  unordered_map<uint64_t, uint64_t> deps;
  for (int i = 1; i <= 3; i++) {
    // complete agreement and make sure its attributes are as expected
    cmd++;
    string dkey = to_string(cmd);
    DoAgreeAndAssertNCommittedAndVerifyAttrs(cmd, dkey, FAST_PATH_QUORUM, false, dkey, 1, deps);
    AssertNExecuted(cmd, FAST_PATH_QUORUM);
  }
  // Reconnect all
  config_->Reconnect(0);
  Passed2();
}

int EpaxosLabTest::testFastPathDependentAgree(void) {
  Init2(3, "Fast path agreement of dependent commands");
  config_->Disconnect(0);
  // complete 1st agreement and make sure its attributes are as expected
  uint64_t cmd1 = ++cmd;
  string dkey = "1";
  uint64_t seq = 1;
  unordered_map<uint64_t, uint64_t> deps;
  DoAgreeAndAssertNCommittedAndVerifyAttrs(cmd, dkey, FAST_PATH_QUORUM, false, dkey, seq, deps);
  AssertNExecuted(cmd, FAST_PATH_QUORUM);
  // complete 2nd agreement and make sure its attributes are as expected
  uint64_t cmd2 = ++cmd;
  seq++;
  deps[1] = 3;
  DoAgreeAndAssertNCommittedAndVerifyAttrs(cmd, dkey, FAST_PATH_QUORUM, false, dkey, seq, deps);
  AssertNExecuted(cmd, FAST_PATH_QUORUM);
  // complete 3rd agreement and make sure its attributes are as expected
  uint64_t cmd3 = ++cmd;
  seq++;
  deps[1] = 4;
  DoAgreeAndAssertNCommittedAndVerifyAttrs(cmd, dkey, FAST_PATH_QUORUM, false, dkey, seq, deps);
  AssertNExecuted(cmd, FAST_PATH_QUORUM);
  // Verify order of execution
  vector<pair<uint64_t, uint64_t>> exp_exec_order = {{cmd1, cmd2}, {cmd2, cmd3}};
  AssertExecutedOrder(exp_exec_order)
  // Reconnect all
  config_->Reconnect(0);
  Passed2();
}

int EpaxosLabTest::testSlowPathIndependentAgree(void) {
  Init2(4, "Slow path agreement of independent commands");
  config_->Disconnect(0);
  config_->Disconnect(1);
  for (int i = 1; i <= 3; i++) {
    // complete agreement and make sure its attributes are as expected
    cmd++;
    string dkey = to_string(cmd);
    unordered_map<uint64_t, uint64_t> deps;
    DoAgreeAndAssertNCommittedAndVerifyAttrs(cmd, dkey, SLOW_PATH_QUORUM, false, dkey, 1, deps);
    AssertNExecuted(cmd, SLOW_PATH_QUORUM);
  }
  // Reconnect all
  config_->Reconnect(0);
  config_->Reconnect(1);
  Passed2();
}

int EpaxosLabTest::testSlowPathDependentAgree(void) {
  Init2(5, "Slow path agreement of dependent commands");
  config_->Disconnect(0);
  config_->Disconnect(1);
  // complete 1st agreement and make sure its attributes are as expected
  uint64_t cmd1 = ++cmd;
  string dkey = "2";
  uint64_t seq = 1;
  unordered_map<uint64_t, uint64_t> deps;
  DoAgreeAndAssertNCommittedAndVerifyAttrs(cmd, dkey, SLOW_PATH_QUORUM, false, dkey, seq, deps);
  AssertNExecuted(cmd, SLOW_PATH_QUORUM);
  // complete 2nd agreement and make sure its attributes are as expected
  uint64_t cmd2 = ++cmd;
  seq++;
  deps[2] = 3;
  DoAgreeAndAssertNCommittedAndVerifyAttrs(cmd, dkey, SLOW_PATH_QUORUM, false, dkey, seq, deps);
  AssertNExecuted(cmd, SLOW_PATH_QUORUM);
  // complete 3rd agreement and make sure its attributes are as expected
  uint64_t cmd3 = ++cmd;
  seq++;
  deps[2] = 4;
  DoAgreeAndAssertNCommittedAndVerifyAttrs(cmd, dkey, SLOW_PATH_QUORUM, false, dkey, seq, deps);
  AssertNExecuted(cmd, SLOW_PATH_QUORUM);
  // Verify order of execution
  vector<pair<uint64_t, uint64_t>> exp_exec_order = {{cmd1, cmd2}, {cmd1, cmd3}};
  AssertExecutedOrder(exp_exec_order)
  // Reconnect all
  config_->Reconnect(0);
  config_->Reconnect(1);
  Passed2();
}

int EpaxosLabTest::testFailNoQuorum(void) {
  Init2(6, "No agreement if too many servers disconnect");
  config_->Disconnect(0);
  config_->Disconnect(1);
  config_->Disconnect(2);
  for (int i = 1; i <= 3; i++) {
    // complete 1 agreement and make sure its not committed
    cmd++;
    string dkey = to_string(cmd);
    DoAgreeAndAssertNoneCommitted(cmd, dkey);
    AssertNoneExecuted(cmd);
  }
  // Reconnect all
  config_->Reconnect(0);
  config_->Reconnect(1);
  config_->Reconnect(2);
  Passed2();
}

int EpaxosLabTest::testNonIdenticalAttrsAgree(void) {
  Init2(7, "Leader and replicas have different dependencies");
  config_->PauseExecution(true);
  /*********** Sub Test 1 ***********/
  InitSub2(1, "Leader have more dependencies than replicas");
  cmd++;
  string dkey = "3";
  uint64_t replica_id, instance_no;
  unordered_map<uint64_t, uint64_t> init_deps;
  // Pre-accept different commands in each server
  for (int i=0; i<NSERVERS; i++) {
    config_->Disconnect(i);
    config_->Start(i, cmd, dkey, &replica_id, &instance_no);
    auto np = config_->NPreAccepted(replica_id, instance_no, NSERVERS);
    Assert2(np == 1, "unexpected number of pre-accepted servers");
    auto na = config_->NAccepted(replica_id, instance_no, NSERVERS);
    Assert2(na == 0, "unexpected number of accepted servers");
    auto nc = config_->NCommitted(replica_id, instance_no, NSERVERS);
    Assert2(nc == 0, "unexpected number of accepted servers");
    init_deps[replica_id] = instance_no;
    cmd++;
    config_->Reconnect(i);
  }
  int CMD_LEADER = 4;
  // Commit in majority
  int seq = 2;
  unordered_map<uint64_t, uint64_t> deps;
  config_->Disconnect((CMD_LEADER + 1) % NSERVERS);
  config_->Disconnect((CMD_LEADER + 2) % NSERVERS);
  deps[CMD_LEADER] = init_deps[CMD_LEADER];
  deps[(CMD_LEADER + 3) % NSERVERS] = init_deps[(CMD_LEADER + 3) % NSERVERS];
  deps[(CMD_LEADER + 4) % NSERVERS] = init_deps[(CMD_LEADER + 4) % NSERVERS];
  config_->Start(CMD_LEADER, cmd, dkey, &replica_id, &instance_no);
  AssertNCommittedAndVerifyAttrs(replica_id, instance_no, SLOW_PATH_QUORUM, false, dkey, seq, deps);
  config_->Reconnect((CMD_LEADER + 1) % NSERVERS);
  config_->Reconnect((CMD_LEADER + 2) % NSERVERS);
  // Commit in another majority
  cmd++;
  seq = 3;
  config_->Disconnect((CMD_LEADER + 3) % NSERVERS);
  config_->Disconnect((CMD_LEADER + 4) % NSERVERS);
  deps[CMD_LEADER] = instance_no;
  deps[(CMD_LEADER + 1) % NSERVERS] = init_deps[(CMD_LEADER + 1) % NSERVERS];
  deps[(CMD_LEADER + 2) % NSERVERS] = init_deps[(CMD_LEADER + 2) % NSERVERS];
  config_->Start(CMD_LEADER % NSERVERS, cmd, dkey, &replica_id, &instance_no);
  AssertNCommittedAndVerifyAttrs(replica_id, instance_no, SLOW_PATH_QUORUM, false, dkey, seq, deps)
  config_->Reconnect((CMD_LEADER + 3) % NSERVERS);
  config_->Reconnect((CMD_LEADER + 4) % NSERVERS);
  // Commit in all
  cmd++;
  seq = 4;
  deps[CMD_LEADER] = instance_no;
  config_->Start(CMD_LEADER, cmd, dkey, &replica_id, &instance_no);
  AssertNCommittedAndVerifyAttrs(replica_id, instance_no, NSERVERS, false, dkey, seq, deps);


  /*********** Sub Test 2 ***********/
  InitSub2(2, "Replicas have more dependencies than leader");
  cmd++;
  // Pre-accept different commands in each server
  for (int i=0; i<NSERVERS; i++) {
    config_->Disconnect(i);
    config_->Start(i, cmd, dkey, &replica_id, &instance_no);
    auto np = config_->NPreAccepted(replica_id, instance_no, NSERVERS);
    Assert2(np == 1, "unexpected number of pre-accepted servers");
    auto na = config_->NAccepted(replica_id, instance_no, NSERVERS);
    Assert2(na == 0, "unexpected number of accepted servers");
    auto nc = config_->NCommitted(replica_id, instance_no, NSERVERS);
    Assert2(nc == 0, "unexpected number of accepted servers");
    init_deps[replica_id] = instance_no;
    cmd++;
    config_->Reconnect(i);
  }
  // Commit in majority
  seq = 6;
  config_->Disconnect((CMD_LEADER + 1) % NSERVERS);
  config_->Disconnect((CMD_LEADER + 2) % NSERVERS);
  deps[CMD_LEADER] = init_deps[CMD_LEADER];
  deps[(CMD_LEADER + 3) % NSERVERS] = init_deps[(CMD_LEADER + 3) % NSERVERS];
  deps[(CMD_LEADER + 4) % NSERVERS] = init_deps[(CMD_LEADER + 4) % NSERVERS];
  config_->Start(CMD_LEADER, cmd, dkey, &replica_id, &instance_no);
  AssertNCommittedAndVerifyAttrs(replica_id, instance_no, SLOW_PATH_QUORUM, false, dkey, seq, deps);
  config_->Reconnect((CMD_LEADER + 1) % NSERVERS);
  config_->Reconnect((CMD_LEADER + 2) % NSERVERS);
  // Commit in another majority
  cmd++;
  seq = 7;
  config_->Disconnect((CMD_LEADER + 3) % NSERVERS);
  config_->Disconnect((CMD_LEADER + 4) % NSERVERS);
  deps[CMD_LEADER] = instance_no;
  deps[(CMD_LEADER + 1) % NSERVERS] = init_deps[(CMD_LEADER + 1) % NSERVERS];
  deps[(CMD_LEADER + 2) % NSERVERS] = init_deps[(CMD_LEADER + 2) % NSERVERS];
  config_->Start((CMD_LEADER + 1) % NSERVERS, cmd, dkey, &replica_id, &instance_no);
  AssertNCommittedAndVerifyAttrs(replica_id, instance_no, SLOW_PATH_QUORUM, false, dkey, seq, deps)
  config_->Reconnect((CMD_LEADER + 3) % NSERVERS);
  config_->Reconnect((CMD_LEADER + 4) % NSERVERS);
  // Commit in all
  cmd++;
  seq = 8;
  deps[(CMD_LEADER + 1) % NSERVERS] = instance_no;
  config_->Start((CMD_LEADER + 2) % NSERVERS, cmd, dkey, &replica_id, &instance_no);
  AssertNCommittedAndVerifyAttrs(replica_id, instance_no, NSERVERS, false, dkey, seq, deps);
  Passed2();
}

int EpaxosLabTest::testPrepareCommittedCommandAgree(void) {
  Init2(8, "Commit through prepare - committed command (takes a few minutes)");
  config_->PauseExecution(true);
  /*********** Sub Test 1 ***********/
  InitSub2(1, "Committed (via fast path) in 2 servers (leader and one replica). Prepare returns 1 committed reply.");
  cmd++;
  string dkey = "4";
  uint64_t replica_id, instance_no;
  int time_to_sleep = 1100, diff = 100;
  int CMD_LEADER = 0;
  // Keep only fast-path quorum of servers alive
  config_->Disconnect((CMD_LEADER + 1) % NSERVERS);
  // Repeat till only 2 servers (leader and one replica) have committed the command
  while (true) {
    config_->Start(CMD_LEADER, cmd, dkey, &replica_id, &instance_no);
    Coroutine::Sleep(time_to_sleep);
    config_->Disconnect((CMD_LEADER + 4) % NSERVERS);
    config_->Disconnect((CMD_LEADER + 3) % NSERVERS);
    config_->Disconnect((CMD_LEADER + 2) % NSERVERS);
    config_->Disconnect(CMD_LEADER);
    auto nc = config_->NCommitted(replica_id, instance_no, NSERVERS);
    Assert2(nc <= FAST_PATH_QUORUM, "unexpected number of accepted servers");
    AssertValidCommitStatus(replica_id, instance_no, nc);
    if (nc == 2) break;
    // Retry if more than 2 servers committed
    time_to_sleep = (nc < 2) ? (time_to_sleep + diff) : (time_to_sleep - diff);
    cmd++;
    config_->Reconnect(CMD_LEADER);
    config_->Reconnect((CMD_LEADER + 2) % NSERVERS);
    config_->Reconnect((CMD_LEADER + 3) % NSERVERS);
    config_->Reconnect((CMD_LEADER + 4) % NSERVERS);
  }
  // Reconnect all except command leader and commit in all via prepare
  config_->Reconnect((CMD_LEADER + 1) % NSERVERS);
  config_->Reconnect((CMD_LEADER + 2) % NSERVERS);
  config_->Reconnect((CMD_LEADER + 3) % NSERVERS);
  config_->Reconnect((CMD_LEADER + 4) % NSERVERS);
  config_->Prepare((CMD_LEADER + 1) % NSERVERS, replica_id, instance_no);
  AssertNCommittedAndVerifyNoop(replica_id, instance_no, NSERVERS, false);
  // Reconnect leader
  config_->Reconnect(CMD_LEADER);

  /*********** Sub Test 2 ***********/
  InitSub2(2, "Committed (via slow path) in 1 server (leader). Prepare returns 1 accepted reply.");
  cmd++;
  time_to_sleep = 1800;
  // Keep only slow-path quorum of servers alive
  config_->Disconnect((CMD_LEADER + 1) % NSERVERS);
  config_->Disconnect((CMD_LEADER + 2) % NSERVERS);
  // Repeat till only 1 server (leader) have committed the command
  while (true) {
    config_->Start(CMD_LEADER, cmd, dkey, &replica_id, &instance_no);
    Coroutine::Sleep(time_to_sleep);
    config_->Disconnect((CMD_LEADER + 4) % NSERVERS);
    config_->Disconnect((CMD_LEADER + 3) % NSERVERS);
    config_->Disconnect(CMD_LEADER);
    auto nc = config_->NCommitted(replica_id, instance_no, NSERVERS);
    Assert2(nc <= SLOW_PATH_QUORUM, "unexpected number of accepted servers");
    AssertValidCommitStatus(replica_id, instance_no, nc);
    if (nc == 1) break;
    // Retry if more than 1 server committed
    time_to_sleep = (nc < 1) ? (time_to_sleep + diff) : (time_to_sleep - diff);
    cmd++;
    config_->Reconnect(CMD_LEADER);
    config_->Reconnect((CMD_LEADER + 3) % NSERVERS);
    config_->Reconnect((CMD_LEADER + 4) % NSERVERS);
  }
  // Reconnect all disconnected servers and one accepted server and commit in those via prepare
  config_->Reconnect((CMD_LEADER + 1) % NSERVERS);
  config_->Reconnect((CMD_LEADER + 2) % NSERVERS);
  config_->Reconnect((CMD_LEADER + 3) % NSERVERS);
  config_->Prepare((CMD_LEADER + 1) % NSERVERS, replica_id, instance_no);
  AssertNCommittedAndVerifyNoop(replica_id, instance_no, NSERVERS-1, false);
  // Reconnect other accepted server and commit in all via prepare
  config_->Reconnect((CMD_LEADER + 4) % NSERVERS);
  config_->Prepare((CMD_LEADER + 4) % NSERVERS, replica_id, instance_no);
  AssertNCommittedAndVerifyNoop(replica_id, instance_no, NSERVERS, false);
  // Reconnect leader
  config_->Reconnect(CMD_LEADER);

  /*********** Sub Test 3 ***********/
  InitSub2(3, "Committed (via fast path) in 1 server (leader). Prepare returns N/2 identical pre-accepted replies.");
  cmd++;
  time_to_sleep = 1200;
  // Keep only fast-path quorum of servers alive
  config_->Disconnect((CMD_LEADER + 1) % NSERVERS);
  // Repeat till only 1 server (leader) have committed the command
  while (true) {
    config_->Start(CMD_LEADER, cmd, dkey, &replica_id, &instance_no);
    Coroutine::Sleep(time_to_sleep);
    config_->Disconnect((CMD_LEADER + 4) % NSERVERS);
    config_->Disconnect((CMD_LEADER + 3) % NSERVERS);
    config_->Disconnect((CMD_LEADER + 2) % NSERVERS);
    config_->Disconnect(CMD_LEADER);
    auto nc = config_->NCommitted(replica_id, instance_no, NSERVERS);
    Assert2(nc <= FAST_PATH_QUORUM, "unexpected number of accepted servers");
    AssertValidCommitStatus(replica_id, instance_no, nc);
    if (nc == 1) break;
    // Retry if more than 1 server committed
    time_to_sleep = (nc < 1) ? (time_to_sleep + diff) : (time_to_sleep - diff);
    cmd++;
    config_->Reconnect(CMD_LEADER);
    config_->Reconnect((CMD_LEADER + 2) % NSERVERS);
    config_->Reconnect((CMD_LEADER + 3) % NSERVERS);
    config_->Reconnect((CMD_LEADER + 4) % NSERVERS);
  }
  // Reconnect all except command leader and commit in all via prepare
  config_->Reconnect((CMD_LEADER + 1) % NSERVERS);
  config_->Reconnect((CMD_LEADER + 2) % NSERVERS);
  config_->Reconnect((CMD_LEADER + 3) % NSERVERS);
  config_->Reconnect((CMD_LEADER + 4) % NSERVERS);
  config_->Prepare((CMD_LEADER + 1) % NSERVERS, replica_id, instance_no);
  AssertNCommittedAndVerifyNoop(replica_id, instance_no, NSERVERS, false);
  // Reconnect leader
  config_->Reconnect(CMD_LEADER);

  /*********** Sub Test 4 ***********/
  InitSub2(4, "Committed (via slow path) in 2 servers (leader and one replica). Prepare returns 1 committed reply.");
  cmd++;
  time_to_sleep = 1900;
  // Keep only slow-path quorum of servers alive
  config_->Disconnect((CMD_LEADER + 1) % NSERVERS);
  config_->Disconnect((CMD_LEADER + 2) % NSERVERS);
  // Repeat till only 2 servers (leader and one replica) have committed the command
  while (true) {
    config_->Start(CMD_LEADER, cmd, dkey, &replica_id, &instance_no);
    Coroutine::Sleep(time_to_sleep);
    config_->Disconnect((CMD_LEADER + 4) % NSERVERS);
    config_->Disconnect((CMD_LEADER + 3) % NSERVERS);
    config_->Disconnect(CMD_LEADER);
    auto nc = config_->NCommitted(replica_id, instance_no, NSERVERS);
    Assert2(nc <= SLOW_PATH_QUORUM, "unexpected number of accepted servers");
    AssertValidCommitStatus(replica_id, instance_no, nc);
    if (nc == 2) break;
    // Retry if more than 2 servers committed
    time_to_sleep = (nc < 2) ? (time_to_sleep + diff) : (time_to_sleep - diff);
    cmd++;
    config_->Reconnect(CMD_LEADER);
    config_->Reconnect((CMD_LEADER + 3) % NSERVERS);
    config_->Reconnect((CMD_LEADER + 4) % NSERVERS);
  }
  // Reconnect one disconnected server and commit in those via prepare
  config_->Reconnect((CMD_LEADER + 1) % NSERVERS);
  config_->Reconnect((CMD_LEADER + 3) % NSERVERS);
  config_->Reconnect((CMD_LEADER + 4) % NSERVERS);
  config_->Prepare((CMD_LEADER + 1) % NSERVERS, replica_id, instance_no);
  AssertNCommitted(replica_id, instance_no, NSERVERS-1);
  // Reconnect other disconnected server and prepare again in it
  config_->Reconnect((CMD_LEADER + 2) % NSERVERS);
  config_->Prepare((CMD_LEADER + 2) % NSERVERS, replica_id, instance_no);
  AssertNCommittedAndVerifyNoop(replica_id, instance_no, NSERVERS, false);
  // Reconnect leader
  config_->Reconnect(CMD_LEADER);
  Passed2();
}

int EpaxosLabTest::testPrepareAcceptedCommandAgree(void) {
  Init2(9, "Commit through prepare - accepted but not committed command (takes a few minutes)");
  config_->PauseExecution(true);
  /*********** Sub Test 1 ***********/
  InitSub2(1, "Accepted in 1 server (leader). Prepare returns 1 pre-accepted reply.");
  cmd++;
  string dkey = "5";
  uint64_t replica_id, instance_no;
  int time_to_sleep = 1000, diff = 100;
  int CMD_LEADER = 1;
  // Keep only slow-path quorum of servers alive
  config_->Disconnect((CMD_LEADER + 1) % NSERVERS);
  config_->Disconnect((CMD_LEADER + 2) % NSERVERS);
  // Repeat till only 1 server (leader) have accepted the command
  while (true) {
    config_->Start(CMD_LEADER, cmd, dkey, &replica_id, &instance_no);
    Coroutine::Sleep(time_to_sleep);
    config_->Disconnect((CMD_LEADER + 3) % NSERVERS);
    config_->Disconnect((CMD_LEADER + 4) % NSERVERS);
    config_->Disconnect(CMD_LEADER);
    auto na = config_->NAccepted(replica_id, instance_no, NSERVERS);
    Assert2(na <= SLOW_PATH_QUORUM, "unexpected number of accepted servers");
    auto nc = config_->NCommitted(replica_id, instance_no, NSERVERS);
    if (na == 1 && nc == 0) break;
    // Retry if more than 1 server accepted
    time_to_sleep = (na < 1 && nc == 0) ? (time_to_sleep + diff) : (time_to_sleep - diff);
    cmd++;
    config_->Reconnect(CMD_LEADER);
    config_->Reconnect((CMD_LEADER + 4) % NSERVERS);
    config_->Reconnect((CMD_LEADER + 3) % NSERVERS);
  }
  auto nc = config_->NCommitted(replica_id, instance_no, NSERVERS);
  Assert2(nc == 0, "unexpected number of accepted servers");
  // Reconnect all disconnected servers and one pre-accepted server and commit in those via prepare
  config_->Reconnect((CMD_LEADER + 1) % NSERVERS);
  config_->Reconnect((CMD_LEADER + 2) % NSERVERS);
  config_->Reconnect((CMD_LEADER + 3) % NSERVERS);
  config_->Prepare((CMD_LEADER + 1) % NSERVERS, replica_id, instance_no);
  AssertNCommittedAndVerifyNoop(replica_id, instance_no, NSERVERS-2, false);
  // Reconnect leader and other pre-accepted server and commit in those via prepare
  config_->Reconnect(CMD_LEADER);
  config_->Reconnect((CMD_LEADER + 4) % NSERVERS);
  config_->Prepare((CMD_LEADER + 4) % NSERVERS, replica_id, instance_no);
  AssertNCommittedAndVerifyNoop(replica_id, instance_no, NSERVERS, false);

  /*********** Sub Test 2 ***********/
  InitSub2(2, "Accepted in 2 servers (leader and one replica). Prepare returns 1 accepted reply.");
  cmd++;
  time_to_sleep = 1300;
  config_->Disconnect((CMD_LEADER + 1) % NSERVERS);
  config_->Disconnect((CMD_LEADER + 2) % NSERVERS);
  // Repeat till only 2 servers (leader and one replica) have accepted the command
  while (true) {
    config_->Start(CMD_LEADER, cmd, dkey, &replica_id, &instance_no);
    Coroutine::Sleep(time_to_sleep);
    config_->Disconnect((CMD_LEADER + 3) % NSERVERS);
    config_->Disconnect((CMD_LEADER + 4) % NSERVERS);
    config_->Disconnect(CMD_LEADER);
    auto na = config_->NAccepted(replica_id, instance_no, NSERVERS);
    Assert2(na <= SLOW_PATH_QUORUM, "unexpected number of accepted servers");
    auto nc = config_->NCommitted(replica_id, instance_no, NSERVERS);
    if (na == 2 && nc == 0) break;
    // Retry if more than 2 server accepted
    time_to_sleep = (na < 2 && nc == 0) ? (time_to_sleep + diff) : (time_to_sleep - diff);
    cmd++;
    config_->Reconnect((CMD_LEADER + 3) % NSERVERS);
    config_->Reconnect((CMD_LEADER + 4) % NSERVERS);
    config_->Reconnect(CMD_LEADER);
  }
  nc = config_->NCommitted(replica_id, instance_no, NSERVERS);
  Assert2(nc == 0, "unexpected number of accepted servers");
  // Reconnect all disconnected servers and commit via prepare
  config_->Reconnect((CMD_LEADER + 1) % NSERVERS);
  config_->Reconnect((CMD_LEADER + 2) % NSERVERS);
  config_->Reconnect((CMD_LEADER + 3) % NSERVERS);
  config_->Reconnect((CMD_LEADER + 4) % NSERVERS);
  config_->Prepare((CMD_LEADER + 1) % NSERVERS, replica_id, instance_no);
  AssertNCommittedAndVerifyNoop(replica_id, instance_no, NSERVERS-1, false);
  // Reconnect leader and commit in it via prepare
  config_->Reconnect(CMD_LEADER);
  config_->Prepare(CMD_LEADER, replica_id, instance_no);
  AssertNCommittedAndVerifyNoop(replica_id, instance_no, NSERVERS, false);
  Passed2();
}

int EpaxosLabTest::testPreparePreAcceptedCommandAgree(void) {
  Init2(10, "Commit through prepare - pre-accepted but not in majority");
  config_->PauseExecution(true);
  /*********** Sub Test 1 ***********/
  InitSub2(1, "Pre-accepted in 1 server (leader). Prepare return 1 pre-accepted reply from leader (avoid fast-path).");
  cmd++;
  string dkey = "6";
  int CMD_LEADER = 2;
  uint64_t replica_id, instance_no;
  // Disconnect leader
  config_->Disconnect(CMD_LEADER);
  // Start agreement in leader - will not replicate as leader is disconnected
  config_->Start(CMD_LEADER, cmd, dkey, &replica_id, &instance_no);
  auto np = config_->NPreAccepted(replica_id, instance_no, NSERVERS);
  Assert2(np == 1, "unexpected number of pre-accepted servers");
  auto na = config_->NAccepted(replica_id, instance_no, NSERVERS);
  Assert2(na == 0, "unexpected number of accepted servers");
  auto nc = config_->NCommitted(replica_id, instance_no, NSERVERS);
  Assert2(nc == 0, "unexpected number of accepted servers");
  // Reconnect leader and commit via prepare
  config_->Reconnect(CMD_LEADER);
  // Set non-leaders slow to prevent from committing noop
  config_->SetSlow((CMD_LEADER + 1) % NSERVERS, true);
  config_->SetSlow((CMD_LEADER + 2) % NSERVERS, true);
  config_->SetSlow((CMD_LEADER + 3) % NSERVERS, true);
  config_->SetSlow((CMD_LEADER + 4) % NSERVERS, true);
  config_->Prepare((CMD_LEADER + 1) % NSERVERS, replica_id, instance_no);
  AssertNCommittedAndVerifyNoop(replica_id, instance_no, NSERVERS, false);
  config_->SetSlow((CMD_LEADER + 1) % NSERVERS, false);
  config_->SetSlow((CMD_LEADER + 2) % NSERVERS, false);
  config_->SetSlow((CMD_LEADER + 3) % NSERVERS, false);
  config_->SetSlow((CMD_LEADER + 4) % NSERVERS, false);
  
  /*********** Sub Test 2 ***********/
  InitSub2(2, "Pre-accepted in 2 server (leader and replica). Prepare returns 1 pre-accepted reply from another replica (slow-path).");
  cmd++;
  // Disconnect all servers except leader and 1 replica
  config_->Disconnect((CMD_LEADER + 1) % NSERVERS);
  config_->Disconnect((CMD_LEADER + 2) % NSERVERS);
  config_->Disconnect((CMD_LEADER + 3) % NSERVERS);
  // Start agreement in leader - will replicate to only 1 replica as others are disconnected
  config_->Start(CMD_LEADER, cmd, dkey, &replica_id, &instance_no);
  np = config_->NPreAccepted(replica_id, instance_no, NSERVERS);
  Assert2(np == 2, "unexpected number of pre-accepted servers");
  na = config_->NAccepted(replica_id, instance_no, NSERVERS);
  Assert2(na == 0, "unexpected number of accepted servers");
  nc = config_->NCommitted(replica_id, instance_no, NSERVERS);
  Assert2(nc == 0, "unexpected number of accepted servers");
  // Disconnect leader and reconnect majority-1 non pre-accepted servers to commit pre-accepted command via prepare
  config_->Disconnect(CMD_LEADER);
  config_->Reconnect((CMD_LEADER + 1) % NSERVERS);
  config_->Reconnect((CMD_LEADER + 2) % NSERVERS);
  config_->Prepare((CMD_LEADER + 1) % NSERVERS, replica_id, instance_no);
  AssertNCommittedAndVerifyNoop(replica_id, instance_no, NSERVERS-2, false);
  config_->Reconnect(CMD_LEADER);
  config_->Reconnect((CMD_LEADER + 3) % NSERVERS);
  config_->Prepare(CMD_LEADER, replica_id, instance_no);
  AssertNCommittedAndVerifyNoop(replica_id, instance_no, NSERVERS, false);
  Passed2();
}

int EpaxosLabTest::testPrepareNoopCommandAgree(void) {
  Init2(11, "Commit through prepare - commit noop");
  config_->PauseExecution(true);
  /*********** Sub Test 1 ***********/
  InitSub2(1, "Pre-accepted in 1 server (leader). Prepare returns no replies (avoid fast-path).");
  cmd++;
  string dkey = "7";
  int CMD_LEADER = 3;
  uint64_t replica_id, instance_no;
  auto deps = unordered_map<uint64_t, uint64_t>();
  int seq = 1;
  // Disconnect leader
  config_->Disconnect(CMD_LEADER);
  // Start agreement in leader - will not replicate as leader is disconnected
  config_->Start(CMD_LEADER, cmd, dkey, &replica_id, &instance_no);
  auto np = config_->NPreAccepted(replica_id, instance_no, NSERVERS);
  Assert2(np == 1, "unexpected number of pre-accepted servers");
  auto na = config_->NAccepted(replica_id, instance_no, NSERVERS);
  Assert2(na == 0, "unexpected number of accepted servers");
  auto nc = config_->NCommitted(replica_id, instance_no, NSERVERS);
  Assert2(nc == 0, "unexpected number of accepted servers");
  // Commit noop in others via prepare
  config_->Prepare((CMD_LEADER + 1) % NSERVERS, replica_id, instance_no);
  AssertNCommittedAndVerifyAttrs(replica_id, instance_no, NSERVERS-1, true, NOOP_DKEY, seq, deps);
  // Reconnect leader and see if noop is committed in all via prepare
  config_->Reconnect(CMD_LEADER);
  config_->Prepare(CMD_LEADER, replica_id, instance_no);
  AssertNCommittedAndVerifyAttrs(replica_id, instance_no, NSERVERS, true, NOOP_DKEY, seq, deps);
  
  /*********** Sub Test 2 ***********/
  InitSub2(2, "Pre-accepted in 2 server (leader and replica). Prepare returns no replies (slow path).");
  cmd++;
  // Disconnect all servers except leader and 1 replica
  config_->Disconnect((CMD_LEADER + 1) % NSERVERS);
  config_->Disconnect((CMD_LEADER + 2) % NSERVERS);
  config_->Disconnect((CMD_LEADER + 3) % NSERVERS);
  // Start agreement in leader - will replicate to only 1 replica as others are disconnected
  config_->Start(CMD_LEADER, cmd, dkey, &replica_id, &instance_no);
  np = config_->NPreAccepted(replica_id, instance_no, NSERVERS);
  Assert2(np == 2, "unexpected number of pre-accepted servers");
  na = config_->NAccepted(replica_id, instance_no, NSERVERS);
  Assert2(na == 0, "unexpected number of accepted servers");
  nc = config_->NCommitted(replica_id, instance_no, NSERVERS);
  Assert2(nc == 0, "unexpected number of accepted servers");
  // Disconnect pre-accepted servers and commit noop in others via prepare
  config_->Disconnect(CMD_LEADER);
  config_->Disconnect((CMD_LEADER + 4) % NSERVERS);
  config_->Reconnect((CMD_LEADER + 1) % NSERVERS);
  config_->Reconnect((CMD_LEADER + 2) % NSERVERS);
  config_->Reconnect((CMD_LEADER + 3) % NSERVERS);
  config_->Prepare((CMD_LEADER + 1) % NSERVERS, replica_id, instance_no);
  AssertNCommittedAndVerifyAttrs(replica_id, instance_no, NSERVERS-2, true, NOOP_DKEY, seq, deps);
  // Reconnect leader and other server and see if noop is committed in all
  config_->Reconnect(CMD_LEADER);
  config_->Reconnect((CMD_LEADER + 4) % NSERVERS);
  config_->Prepare(CMD_LEADER, replica_id, instance_no);
  AssertNCommittedAndVerifyAttrs(replica_id, instance_no, NSERVERS, true, NOOP_DKEY, seq, deps);
  Passed2();
}

class CAArgs {
 public:
  int cmd;
  int svr;
  string dkey;
  std::mutex *mtx;
  std::vector<std::pair<uint64_t, uint64_t>> *retvals;
  EpaxosTestConfig *config;
};

static void *doConcurrentAgreement(void *args) {
  CAArgs *caargs = (CAArgs *)args;
  uint64_t replica_id, instance_no;
  caargs->config->Start(caargs->svr, caargs->cmd, caargs->dkey, &replica_id, &instance_no);
  std::lock_guard<std::mutex> lock(*(caargs->mtx));
  caargs->retvals->push_back(make_pair(replica_id, instance_no));
  return nullptr;
}

int EpaxosLabTest::testConcurrentAgree(void) {
  Init2(12, "Concurrent agreements");
  config_->PauseExecution(true);
  std::vector<pthread_t> threads{};
  std::vector<std::pair<uint64_t, uint64_t>> retvals{};
  std::mutex mtx{};
  for (int iter = 1; iter <= 50; iter++) {
    for (int svr = 0; svr < NSERVERS; svr++) {
      CAArgs *args = new CAArgs{};
      args->cmd = ++cmd;
      args->svr = svr;
      args->dkey = "8";
      args->mtx = &mtx;
      args->retvals = &retvals;
      args->config = config_;
      pthread_t thread;
      verify(pthread_create(&thread,
                            nullptr,
                            doConcurrentAgreement,
                            (void*)args) == 0);
      threads.push_back(thread);
    }
  }
  // join all threads
  for (auto thread : threads) {
    verify(pthread_join(thread, nullptr) == 0);
  }
  Assert2(retvals.size() == 250, "Failed to reach agreement");
  for (auto retval : retvals) {
    AssertNCommitted(retval.first, retval.second, NSERVERS);
  }
  Passed2();
}

int EpaxosLabTest::testConcurrentUnreliableAgree1(void) {
  Init2(13, "Unreliable concurrent agreement (takes a few minutes)");
  config_->PauseExecution(true);
  config_->SetUnreliable(true);
  std::vector<pthread_t> threads{};
  std::vector<std::pair<uint64_t, uint64_t>> retvals{};
  std::mutex mtx{};
  for (int iter = 1; iter <= 50; iter++) {
    for (int svr = 0; svr < NSERVERS; svr++) {
      CAArgs *args = new CAArgs{};
      args->cmd = ++cmd;
      args->svr = svr;
      args->dkey = "9";
      args->mtx = &mtx;
      args->retvals = &retvals;
      args->config = config_;
      pthread_t thread;
      verify(pthread_create(&thread,
                            nullptr,
                            doConcurrentAgreement,
                            (void*)args) == 0);
      threads.push_back(thread);
    }
  }
  // join all threads
  for (auto thread : threads) {
    verify(pthread_join(thread, nullptr) == 0);
  }
  config_->SetUnreliable(false);
  Coroutine::Sleep(1000);
  for (auto retval : retvals) {
    auto nc = config_->NCommitted(retval.first, retval.second, NSERVERS);
    int CMD_LEADER = rand() % NSERVERS;
    config_->Prepare(CMD_LEADER, retval.first, retval.second);
  }
  for (auto retval : retvals) {
    AssertNCommitted(retval.first, retval.second, NSERVERS);
  }
  Coroutine::Sleep(1000000);
  Passed2();
}

int EpaxosLabTest::testConcurrentUnreliableAgree2(void) {
  Init2(13, "Unreliable concurrent agreement (takes a few minutes) - Prepare hell");
  config_->SetUnreliable(true);
  std::vector<pthread_t> threads{};
  std::vector<std::pair<uint64_t, uint64_t>> retvals{};
  std::mutex mtx{};
  for (int iter = 1; iter <= 100; iter++) {
    for (int svr = 0; svr < NSERVERS; svr++) {
      CAArgs *args = new CAArgs{};
      args->cmd = ++cmd;
      args->svr = svr;
      args->dkey = "10";
      args->mtx = &mtx;
      args->retvals = &retvals;
      args->config = config_;
      pthread_t thread;
      verify(pthread_create(&thread,
                            nullptr,
                            doConcurrentAgreement,
                            (void*)args) == 0);
      threads.push_back(thread);
    }
  }
  // join all threads
  for (auto thread : threads) {
    verify(pthread_join(thread, nullptr) == 0);
  }
  config_->PrepareAll();
  config_->PrepareAll();
  config_->PrepareAll();
  Coroutine::Sleep(60000000);
  config_->SetUnreliable(false);
  Coroutine::Sleep(1000);
  for (auto retval : retvals) {
    auto nc = config_->NCommitted(retval.first, retval.second, NSERVERS);
    int CMD_LEADER = rand() % NSERVERS;
    config_->Prepare(CMD_LEADER, retval.first, retval.second);
  }
  for (auto retval : retvals) {
    AssertNCommitted(retval.first, retval.second, NSERVERS);
  }
  Coroutine::Sleep(1000000);
  Passed2();
}

int EpaxosLabTest::testExecutionOrder(void) {
  Init2(14, "Test if all commands were executed in same order");
  config_->PauseExecution(false);
  Coroutine::Sleep(20000000);
  auto executed_cmds = config_->GetExecutedCommands(0);
  string s = "";
  for (auto cmd : executed_cmds) {
    s = s + to_string(cmd) + ", ";
  }
  Log_debug("Exec order in %d is %s", 0, s.c_str());
  for (int svr = 1; svr < NSERVERS; svr++) {
    auto cmds = config_->GetExecutedCommands(svr);
    s = "";
    for (auto cmd : cmds) {
      s = s + to_string(cmd) + ", ";
    }
    Log_debug("Exec order in %d is %s", svr, s.c_str());
    // verify(executed_cmds == cmds);
  }
  config_->PauseExecution(true);
  Passed2();
}

#endif

}
