/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <thread>
#include <unistd.h>

#include <folly/Memory.h>
#include <folly/container/Array.h>
#include <gtest/gtest.h>

#include "logdevice/common/Connection.h"
#include "logdevice/common/FlowGroup.h"
#include "logdevice/common/Processor.h"
#include "logdevice/common/Semaphore.h"
#include "logdevice/common/Sender.h"
#include "logdevice/common/SocketCallback.h"
#include "logdevice/common/SocketDependencies.h"
#include "logdevice/common/Worker.h"
#include "logdevice/common/debug.h"
#include "logdevice/common/network/AsyncSocketAdapter.h"
#include "logdevice/common/protocol/ACK_Message.h"
#include "logdevice/common/protocol/CONFIG_ADVISORY_Message.h"
#include "logdevice/common/protocol/GET_SEQ_STATE_Message.h"
#include "logdevice/common/protocol/HELLO_Message.h"
#include "logdevice/common/protocol/STORED_Message.h"
#include "logdevice/common/request_util.h"
#include "logdevice/common/settings/Settings.h"
#include "logdevice/common/stats/Stats.h"
#include "logdevice/common/test/SocketTest_fixtures.h"
#include "logdevice/common/test/TestUtil.h"
#include "logdevice/include/ClientSettings.h"
#include "logdevice/lib/ClientImpl.h"
#include "logdevice/server/NewConnectionRequest.h"
#include "logdevice/test/utils/IntegrationTestBase.h"
#include "logdevice/test/utils/IntegrationTestUtils.h"
#include "logdevice/test/utils/port_selection.h"

using namespace facebook::logdevice;
using PortOwner = facebook::logdevice::IntegrationTestUtils::detail::PortOwner;

class MessagingSocketTest
    : public testing::TestWithParam<facebook::logdevice::EvBase::EvBaseType> {
 protected:
  explicit MessagingSocketTest(
      std::chrono::milliseconds timeout = getDefaultTestTimeout())
      : timeout_(timeout), alarm_(timeout) {}
  void SetUp() override {
    dbg::currentLevel = getLogLevelFromEnv().value_or(dbg::Level::INFO);
    dbg::assertOnData = true;
  }

  std::chrono::milliseconds testTimeout() const {
    return timeout_;
  }

 private:
  std::chrono::milliseconds timeout_;
  Alarm alarm_;
};

// The name of the cluster used in testing
static const char* CLUSTER_NAME = "logdevice_test_MessagingSocketTest.cpp";
static NodeID firstNodeID{0, 3};  // id of first node in config
static NodeID secondNodeID{1, 3}; // id of second node in config
static NodeID clNodeID{2, 3};     // id of second node in config
static NodeID badNodeID{332, 3};  // a node id that does not appear in
                                  // config

// Infrastructure that must be publicly visible to LogDevice code
// (e.g. so Worker can friend SocketConnectRequest).
namespace testing {

// see SocketConnect below
struct SocketConnectRequest : public Request {
  SocketConnectRequest()
      : Request(RequestType::TEST_MESSAGING_SOCKET_CONNECT_REQUEST) {}

  Request::Execution execute() override {
    ThreadID::set(ThreadID::SERVER_WORKER, "");
    bool constructor_failed = false;
    int rv;
    auto& base = Worker::onThisThread()->getEvBase();
    auto base_type = base.getType();

    if (SocketConnectRequest::conn) {
      // This is the second request. Test is done. Clean up. Simulate
      // Worker shutdown here to avoid tripping asserts in Connection that
      // expects to be destroyed only when Worker shuts down.
      Worker::onThisThread()->shutting_down_ = true;
      SocketConnectRequest::conn.reset();
      return Execution::COMPLETE;
    }

    try {
      if (base_type == EvBase::LEGACY_EVENTBASE) {
        Connection s(badNodeID,
                     SocketType::DATA,
                     ConnectionType::PLAIN,
                     PeerType::CLIENT,
                     flow_group,
                     std::make_unique<SocketDependencies>(
                         Worker::onThisThread()->processor_,
                         &Worker::onThisThread()->sender()));
      } else {
        Connection s(badNodeID,
                     SocketType::DATA,
                     ConnectionType::PLAIN,
                     PeerType::CLIENT,
                     flow_group,
                     std::make_unique<SocketDependencies>(
                         Worker::onThisThread()->processor_,
                         &Worker::onThisThread()->sender()),
                     std::make_unique<AsyncSocketAdapter>(base.getEventBase()));
      }
    } catch (const ConstructorFailed&) {
      constructor_failed = true;
    }

    EXPECT_TRUE(constructor_failed);
    EXPECT_EQ(E::NOTINCONFIG, err);

    constructor_failed = false;
    try {
      auto deps = std::make_unique<SocketDependencies>(
          Worker::onThisThread()->processor_,
          &Worker::onThisThread()->sender());
      const auto throttle_settings = deps->getSettings().connect_throttle;
      connect_throttle = std::make_unique<ConnectThrottle>(throttle_settings);
      if (base_type == EvBase::LEGACY_EVENTBASE) {
        SocketConnectRequest::conn =
            std::make_unique<Connection>(firstNodeID,
                                         SocketType::DATA,
                                         ConnectionType::PLAIN,
                                         PeerType::CLIENT,
                                         flow_group,
                                         std::move(deps));
      } else {
        SocketConnectRequest::conn = std::make_unique<Connection>(
            firstNodeID,
            SocketType::DATA,
            ConnectionType::PLAIN,
            PeerType::CLIENT,
            flow_group,
            std::move(deps),
            std::make_unique<AsyncSocketAdapter>(base.getEventBase()));
      }
      SocketConnectRequest::conn->setConnectThrottle(connect_throttle.get());
    } catch (const ConstructorFailed&) {
      constructor_failed = true;
    }

    EXPECT_FALSE(constructor_failed);

    EXPECT_NE(nullptr, SocketConnectRequest::conn);

    rv = SocketConnectRequest::conn->connect();
    EXPECT_EQ(0, rv) << "Socket::connect() failed: " << error_description(err);

    rv = SocketConnectRequest::conn->connect(); // this should fail because s is
    EXPECT_EQ(-1, rv); // already connected or connecting
    EXPECT_TRUE(err == E::ISCONN || err == E::ALREADY);

    return Execution::COMPLETE;
  }

  static std::unique_ptr<ConnectThrottle> connect_throttle;
  static std::unique_ptr<Connection> conn;
  static FlowGroup flow_group;
};

std::unique_ptr<ConnectThrottle> SocketConnectRequest::connect_throttle{};
std::unique_ptr<Connection> SocketConnectRequest::conn{};
FlowGroup SocketConnectRequest::flow_group{nullptr};

} // namespace testing

// Wrap test infrastructure in an anonymous namespace to prevent ODR issues.
namespace {

struct ProtocolHeaderWithoutChecksum {
  message_len_t len;
  MessageType type;
} __attribute__((__packed__));
static_assert(sizeof(ProtocolHeaderWithoutChecksum) ==
                  sizeof(ProtocolHeader) - sizeof(ProtocolHeader::cksum),
              "Invalid size of ProtocolHeaderWithoutChecksum");

// HELLO_Message on the wire
struct HELLO_Raw {
  ProtocolHeaderWithoutChecksum ph;
  HELLO_Header hdr;
  NodeID destination_node;
  uint16_t size_of_cluster_name;
  // Used to simulate the cluster name sent on the wire.
  char cluster_name_[38]; // "logdevice_test_MessagingSocketTest.cpp"
  uint16_t size_of_build_information;
  char build_information[2]; // {}
} __attribute__((__packed__));

// ACK_Message on the wire
struct ACK_Raw {
  ProtocolHeaderWithoutChecksum ph;
  ACK_Header hdr;
} __attribute__((__packed__));

// STORED_Message on the wire
struct STORED_Raw {
  ProtocolHeader ph;
  STORED_Header hdr;
} __attribute__((__packed__));

// CONFIG_ADVISORY_Message on the wire
struct CONFIG_ADVISORY_Raw {
  ProtocolHeader ph;
  CONFIG_ADVISORY_Header hdr;
} __attribute__((__packed__));

static std::shared_ptr<UpdateableConfig>
create_config(std::vector<int> ld_ports) {
  int node_idx = 0;
  std::unordered_map<node_index_t, Configuration::Node> nodes;
  for (auto ld_port : ld_ports) {
    Configuration::Node node;
    node.address = Sockaddr("127.0.0.1", std::to_string(ld_port).c_str());
    node.gossip_address =
        Sockaddr("127.0.0.1", std::to_string(ld_port + 1).c_str());
    node.generation = 3;
    node.addStorageRole(/*num_shards*/ 2);
    nodes.insert(std::make_pair(node_idx, std::move(node)));
    node_idx++;
  }
  configuration::MetaDataLogsConfig meta_config;
  auto updateable_config = std::make_shared<UpdateableConfig>();
  Configuration::NodesConfig node_config{nodes};

  updateable_config->updateableServerConfig()->update(
      ServerConfig::fromDataTest(CLUSTER_NAME, node_config, meta_config));
  return updateable_config;
}

// A dummy message that tests can use for checking that onSent is called with an
// expected value.
class DummyMessage : public GET_SEQ_STATE_Message {
 public:
  /**
   * @param sem      Semaphore that can be used for waiting for onSent to be
   *                 called.
   * @param expected Expected error code given by onSent.
   */
  DummyMessage(Semaphore& sem, Status expected)
      : GET_SEQ_STATE_Message(logid_t(1),
                              request_id_t(1),
                              GET_SEQ_STATE_flags_t(0),
                              GetSeqStateRequest::Context::UNKNOWN),
        sem_(sem),
        expected_(expected) {}
  void onSent(Status st, const Address&) const override {
    EXPECT_EQ(expected_, st);
    sem_.post();
  }
  uint16_t getMinProtocolVersion() const override {
    return min_proto_;
  }
  uint16_t min_proto_{0};

 protected:
  Semaphore& sem_;
  Status expected_;
};

// A dummy message that reports it has been cancelled, and sends
// another message from within its onSent() handler.
class ReentrantDummyMessage : public DummyMessage {
 public:
  /**
   * @param sem      Semaphore that can be used for waiting for onSent to be
   *                 called.
   * @param expected Expected error code given by onSent.
   */
  ReentrantDummyMessage(Semaphore& sem, Status expected)
      : DummyMessage(sem, expected) {}

  void onSent(Status st, const Address& to) const override {
    EXPECT_EQ(expected_, st);

    auto msg = std::make_unique<DummyMessage>(sem_, E::OK);
    Worker* w = Worker::onThisThread();
    EXPECT_EQ(0, w->sender().sendMessage(std::move(msg), to));
    EXPECT_FALSE(msg);

    sem_.post();
  }

  bool cancelled() const override {
    return true;
  }
};

// A utility class for tests to spawn a connection listener they can use to talk
// with the client.
class ServerSocket {
 public:
  explicit ServerSocket() {
    // try to claim any port from range [4445-5445), give up if that fails.
    for (int port = 4445; port < 5445; port++) {
      auto p = IntegrationTestUtils::detail::claim_port(port);
      if (p.has_value()) {
        sock_ = std::move(p.value());
        break;
      }
    }
    EXPECT_TRUE(sock_.valid());
  }

  int accept() {
    struct sockaddr_in6 cli_addr;
    socklen_t clilen = sizeof(cli_addr);
    const int fd = ::accept(sock_.fd, (struct sockaddr*)&cli_addr, &clilen);
    EXPECT_TRUE(fd > 0) << folly::errnoStr(errno);
    fds_.push_back(fd);
    return fd;
  }

  Sockaddr accept(int& fd) {
    struct sockaddr_in6 cli_addr;
    socklen_t clilen = sizeof(cli_addr);
    fd = ::accept(sock_.fd, (struct sockaddr*)&cli_addr, &clilen);
    EXPECT_TRUE(fd > 0) << folly::errnoStr(errno);
    fds_.push_back(fd);
    return Sockaddr((struct sockaddr*)&cli_addr, clilen);
  }

  int getPort() const {
    return sock_.port;
  }

  ~ServerSocket() {
    for (int fd : fds_) {
      close(fd);
    }
  }

 private:
  PortOwner sock_;
  // Keep track of which fds we need to close.
  std::list<int> fds_;
};

struct WorkerAndEventLoop {
  std::unique_ptr<EventLoop> loop;
  std::unique_ptr<Worker> worker;

  WorkerAndEventLoop(std::unique_ptr<EventLoop> loop,
                     std::unique_ptr<Worker> worker)
      : loop(std::move(loop)), worker(std::move(worker)) {}

  ~WorkerAndEventLoop() {
    loop->getTaskQueue().shutdown();
    loop->getThread().join();
    worker.reset();
    loop.reset();
  }
};

WorkerAndEventLoop createWorker(Processor* p,
                                std::shared_ptr<UpdateableConfig>& config,
                                EvBase::EvBaseType base_type) {
  static int idx_alloc = 0;
  int idx = idx_alloc++;
  ld_info("Creating worker %d", idx);
  auto h = std::make_unique<EventLoop>("test" + toString(idx),
                                       ThreadID::Type::UNKNOWN_EVENT_LOOP,
                                       1024,
                                       true,
                                       folly::make_array<uint32_t>(13, 3, 1),
                                       base_type);
  auto w = std::make_unique<Worker>(
      folly::getKeepAliveToken(h.get()), p, worker_id_t(0), config);

  w->add([w = w.get()] { w->setupWorker(); });

  return WorkerAndEventLoop(std::move(h), std::move(w));
}

/**
 * A basic connection test.
 *
 * Executes nc to listen on a predefined port. Starts a
 * Worker. Posts a SocketConnectRequest that creates a new
 * server connection. Connects to the remote and Sends HELLO.
 */
TEST_P(MessagingSocketTest, SocketConnect) {
  int rv;
  Settings settings = create_default_settings<Settings>();
  settings.include_cluster_name_on_handshake = true;
  settings.include_destination_on_handshake = true;
  settings.use_legacy_eventbase =
      GetParam() == EvBase::EvBaseType::LEGACY_EVENTBASE;
  UpdateableSettings<Settings> updateable_settings(settings);
  settings.num_workers = 1;
  ServerSocket server;

  std::shared_ptr<UpdateableConfig> config(
      create_config(std::vector<int>{server.getPort()}));
  Processor processor(config, updateable_settings);

  ld_check((bool)config);
  auto out = createWorker(&processor, config, GetParam());
  auto w = out.worker.get();

  SCOPE_EXIT {
    w->sender().shutdownSockets(w);
  };

  ASSERT_FALSE(out.loop->getThread().isCurrentThread());

  config.reset();

  std::unique_ptr<Request> rq1 =
      std::make_unique<testing::SocketConnectRequest>();

  EXPECT_EQ(0, w->tryPost(rq1));

  const int fd = server.accept();

  HELLO_Raw hello;
  ASSERT_EQ(sizeof(hello), read(fd, &hello, sizeof(hello)));
  EXPECT_EQ(MessageType::HELLO, hello.ph.type);
  EXPECT_EQ(sizeof(hello), hello.ph.len);
  EXPECT_EQ(Compatibility::MIN_PROTOCOL_SUPPORTED, hello.hdr.proto_min);
  EXPECT_EQ(Compatibility::MAX_PROTOCOL_SUPPORTED, hello.hdr.proto_max);

  std::unique_ptr<Request> rq2 =
      std::make_unique<testing::SocketConnectRequest>();
  // Block for request to execute, as worker will be destructed first.
  Semaphore sem;
  rq2->setClientBlockedSemaphore(&sem);
  EXPECT_EQ(0, w->tryPost(rq2));
  sem.wait();

  dbg::currentLevel = dbg::Level::ERROR;
}

// see SenderBasicSend below
struct SenderBasicSendRequest : public Request {
  SenderBasicSendRequest()
      : Request(RequestType::TEST_MESSAGING_SENDER_BASIC_SEND_REQUEST) {}
  Request::Execution execute() override {
    ThreadID::set(ThreadID::SERVER_WORKER, "");
    int rv;
    Worker* w = Worker::onThisThread();
    EXPECT_TRUE(w);

    auto msg1out =
        std::make_unique<STORED_Message>(SenderBasicSendRequest::hdr1out,
                                         0,
                                         0,
                                         log_rebuilding_id_t(0),
                                         0,
                                         ServerInstanceId_INVALID);
    EXPECT_EQ(0, w->sender().sendMessage(std::move(msg1out), firstNodeID));
    EXPECT_FALSE(msg1out);

    auto msg2out =
        std::make_unique<STORED_Message>(SenderBasicSendRequest::hdr2out,
                                         0,
                                         0,
                                         log_rebuilding_id_t(0),
                                         0,
                                         ServerInstanceId_INVALID);
    EXPECT_EQ(0, w->sender().sendMessage(std::move(msg2out), firstNodeID));
    EXPECT_FALSE(msg2out);

    auto msg3nogo =
        std::make_unique<STORED_Message>(SenderBasicSendRequest::hdr1out,
                                         0,
                                         0,
                                         log_rebuilding_id_t(0),
                                         0,
                                         ServerInstanceId_INVALID);
    EXPECT_EQ(-1, w->sender().sendMessage(std::move(msg3nogo), badNodeID));
    EXPECT_EQ(E::NOTINCONFIG, err);
    EXPECT_TRUE(msg3nogo);

    return Execution::COMPLETE;
  }

  static STORED_Header hdr1out, hdr2out;
};

STORED_Header SenderBasicSendRequest::hdr1out =
    {{esn_t(1), epoch_t(2), logid_t(3)},
     0,
     Status::FORWARD,
     NodeID(),
     STORED_Header::SYNCED | STORED_Header::OVERLOADED},
              SenderBasicSendRequest::hdr2out = {
                  {esn_t(2), epoch_t(3), logid_t(4)},
                  1,
                  Status::NOSPC,
                  NodeID(),
                  STORED_Header::AMENDABLE_DEPRECATED};

/**
 * A basic Sender::sendMessage() test. Starts a Worker, launches
 * nc to stand in for a server. Posts a SenderBasicSendRequest, which
 * sends two STORED messages to node 0, and attempts to send a message
 * to a bad address.  Reads the two messages back from nc's stdout.
 * Validates headers and payloads. Destroys the Worker handle. This
 * must lead to the destruction of Connection to nc, closing the
 * underlying TCP socket, and nc exiting. pclose() will block until nc
 * exits.
 */
TEST_P(MessagingSocketTest, SenderBasicSend) {
  int rv;
  Settings settings = create_default_settings<Settings>();
  settings.include_cluster_name_on_handshake = true;
  settings.include_destination_on_handshake = true;
  settings.use_legacy_eventbase =
      GetParam() == EvBase::EvBaseType::LEGACY_EVENTBASE;
  UpdateableSettings<Settings> updateable_settings(settings);

  ServerSocket server;
  std::shared_ptr<UpdateableConfig> config(
      create_config(std::vector<int>{server.getPort()}));

  Processor processor(config, updateable_settings);

  ld_check((bool)config);

  auto out = createWorker(&processor, config, GetParam());
  auto w = out.worker.get();
  SCOPE_EXIT {
    w->sender().shutdownSockets(w);
  };
  ASSERT_FALSE(out.loop->getThread().isCurrentThread());

  config.reset();

  std::unique_ptr<Request> rq = std::make_unique<SenderBasicSendRequest>();

  EXPECT_EQ(0, w->tryPost(rq));

  const int fd = server.accept();

  HELLO_Raw hin;
  CONFIG_ADVISORY_Raw cin;
  STORED_Raw r1in, r2in;

  // Skip initial HELLO message.
  ASSERT_EQ(sizeof(HELLO_Raw), read(fd, &hin, sizeof(HELLO_Raw)));

  // Construct and send an ACK message in response.
  ACK_Raw ack;
  ack.ph.len = sizeof(ACK_Raw);
  ack.ph.type = MessageType::ACK;
  ack.hdr.options = 0;
  ack.hdr.rqid = request_id_t(42);
  ack.hdr.client_idx = 1;
  ack.hdr.proto = Compatibility::MAX_PROTOCOL_SUPPORTED;
  ack.hdr.status = E::OK;
  ASSERT_EQ(sizeof(ACK_Raw), write(fd, &ack, sizeof(ACK_Raw)));

  // Skip CONFIG_ADVISORY
  size_t expected_size_of_config_advisory = sizeof(CONFIG_ADVISORY_Raw) -
      (ProtocolHeader::needChecksumInHeader(
           MessageType::CONFIG_ADVISORY, ack.hdr.proto)
           ? 0
           : sizeof(ProtocolHeader::cksum));
  ASSERT_EQ(expected_size_of_config_advisory,
            read(fd, &cin, expected_size_of_config_advisory));
  EXPECT_EQ(MessageType::CONFIG_ADVISORY, cin.ph.type);
  EXPECT_EQ(expected_size_of_config_advisory, cin.ph.len);

  bool need_checksum_for_stored =
      ProtocolHeader::needChecksumInHeader(MessageType::STORED, ack.hdr.proto);
  size_t expected_size_of_stored_ph = sizeof(STORED_Raw::ph) -
      (!need_checksum_for_stored ? sizeof(ProtocolHeader::cksum) : 0);
  size_t expected_size_of_stored = sizeof(STORED_Raw) -
      (!need_checksum_for_stored ? sizeof(ProtocolHeader::cksum) : 0);

  // Read 1st STORED message.
  ASSERT_EQ(expected_size_of_stored_ph,
            read(fd, &r1in.ph, expected_size_of_stored_ph));
  EXPECT_EQ(expected_size_of_stored, r1in.ph.len);
  EXPECT_EQ(MessageType::STORED, r1in.ph.type);
  ASSERT_EQ(
      sizeof(STORED_Raw::hdr), read(fd, &r1in.hdr, sizeof(STORED_Raw::hdr)));
  EXPECT_EQ(
      0, memcmp(&r1in.hdr, &SenderBasicSendRequest::hdr1out, sizeof(r1in.hdr)));

  // Read 2nd STORED message.
  ASSERT_EQ(expected_size_of_stored_ph,
            read(fd, &r2in.ph, expected_size_of_stored_ph));
  EXPECT_EQ(expected_size_of_stored, r2in.ph.len);
  EXPECT_EQ(MessageType::STORED, r2in.ph.type);
  ASSERT_EQ(
      sizeof(STORED_Raw::hdr), read(fd, &r2in.hdr, sizeof(STORED_Raw::hdr)));
  EXPECT_EQ(
      0, memcmp(&r2in.hdr, &SenderBasicSendRequest::hdr2out, sizeof(r2in.hdr)));

  dbg::currentLevel = dbg::Level::ERROR;
}

struct SenderVarLenMessageRequest : public Request {
  SenderVarLenMessageRequest(Semaphore& sem,
                             std::unique_ptr<VarLengthTestMessage>& msg,
                             E expected_err,
                             Address node_addr)
      : Request(RequestType::TEST_MESSAGING_SEND_VARLEN_REQUEST),
        msg_(std::move(msg)),
        sem_(sem),
        expected_err_(expected_err),
        node_addr_(node_addr) {}
  Request::Execution execute() override {
    ThreadID::set(ThreadID::SERVER_WORKER, "");
    Worker* w = Worker::onThisThread();
    EXPECT_TRUE(w);

    if (expected_err_ == E::OK) {
      EXPECT_EQ(0, w->sender().sendMessage(std::move(msg_), node_addr_));
    } else {
      EXPECT_EQ(-1, w->sender().sendMessage(std::move(msg_), node_addr_));
      EXPECT_EQ(expected_err_, err);
    }
    sem_.post();

    return Execution::COMPLETE;
  }

  std::unique_ptr<VarLengthTestMessage> msg_;
  Semaphore& sem_;
  E expected_err_;
  Address node_addr_;
};

void testOutBufsLimit(bool outBufsLimitPerPeerTypeDisabled,
                      EvBase::EvBaseType type) {
  Settings settings = create_default_settings<Settings>();
  settings.include_cluster_name_on_handshake = true;
  settings.include_destination_on_handshake = true;
  settings.outbufs_mb_max_per_thread = 1;
  settings.outbuf_socket_min_kb = 1;
  settings.use_legacy_eventbase = type == EvBase::EvBaseType::LEGACY_EVENTBASE;
  // Tell Sender to not actually send any messages (except HELLO).
  // Instead, the messages will be kept in flight indefinitely.
  settings.message_error_injection_status = E::DROPPED;
  settings.message_error_injection_chance_percent = 100;

  Address firstNodeAddress(firstNodeID);
  Address secondNodeAddress(secondNodeID);

  if (outBufsLimitPerPeerTypeDisabled) {
    settings.outbufs_limit_per_peer_type_enabled = false;
  }

  ServerSocket server1;
  ServerSocket server2;
  ServerSocket cl_node;

  settings.server = false;
  UpdateableSettings<Settings> updateable_cl_settings(settings);
  std::shared_ptr<UpdateableConfig> config(create_config(std::vector<int>{
      server1.getPort(), server2.getPort(), cl_node.getPort()}));

  // Client processor
  auto cl_processor =
      Processor::createNoInit(config, updateable_cl_settings, firstNodeID);
  auto out = createWorker(cl_processor.get(), config, type);
  auto cl_w = out.worker.get();
  auto cl_processor2 =
      Processor::createNoInit(config, updateable_cl_settings, clNodeID);
  auto out2 = createWorker(cl_processor2.get(), config, type);
  auto cl_w2 = out2.worker.get();

  // Create server processor.
  settings.server = true;
  UpdateableSettings<Settings> updateable_srv_settings(settings);
  auto srv_processor =
      Processor::createNoInit(config, updateable_srv_settings, secondNodeID);
  auto out3 = createWorker(srv_processor.get(), config, type);
  auto srv_w = out3.worker.get();

  ld_check((bool)config);

  ASSERT_FALSE(out.loop->getThread().isCurrentThread());

  // Fill up the client sender output buffer by sending to node server 1.
  ld_info("Sending 600K to first node");
  Semaphore sem;
  auto msg1 = std::make_unique<VarLengthTestMessage>(
      Compatibility::MAX_PROTOCOL_SUPPORTED, 600 * 1024);
  std::unique_ptr<Request> rq1 = std::make_unique<SenderVarLenMessageRequest>(
      sem, msg1, E::OK, firstNodeAddress);
  EXPECT_EQ(0, cl_w->tryPost(rq1));
  sem.wait();

  // For client, outbufs-limit-per-peer-type is disabled, expect E::OK as the
  //  sender's output buffer limit is not yet full.
  ld_info("Sending another 600K to first node");
  sem = Semaphore();
  auto msg2 = std::make_unique<VarLengthTestMessage>(
      Compatibility::MAX_PROTOCOL_SUPPORTED, 600 * 1024);
  std::unique_ptr<Request> rq2 = std::make_unique<SenderVarLenMessageRequest>(
      sem, msg2, E::OK, firstNodeAddress);
  EXPECT_EQ(0, cl_w->tryPost(rq2));
  sem.wait();

  // Expect ENOBUFS now as it is over combined out-bufs limit
  //  (outbufs-limit-per-peer-type is disabled for client).
  ld_info("Sending yet another 600K to first node");
  sem = Semaphore();
  auto msg = std::make_unique<VarLengthTestMessage>(
      Compatibility::MAX_PROTOCOL_SUPPORTED, 600 * 1024);
  std::unique_ptr<Request> rq = std::make_unique<SenderVarLenMessageRequest>(
      sem, msg, E::NOBUFS, firstNodeAddress);
  EXPECT_EQ(0, cl_w->tryPost(rq));
  sem.wait();

  // Send to a different server node and expect success due to new connection's
  // outbuf_socket_min_kb  guaranteed budget.
  ld_info("Sending 2K to second node");
  sem = Semaphore();
  auto msg3 = std::make_unique<VarLengthTestMessage>(
      Compatibility::MAX_PROTOCOL_SUPPORTED, 2 * 1024);
  std::unique_ptr<Request> rq3 = std::make_unique<SenderVarLenMessageRequest>(
      sem, msg3, E::OK, secondNodeAddress);
  EXPECT_EQ(0, cl_w->tryPost(rq3));
  sem.wait();

  // Expect message over new connection to fail with ENOBUF as this is over both
  //  sender's outbuf limit and connections's outbuf_socket_min_kb.
  ld_info("Sending another 2K to second node");
  sem = Semaphore();
  auto msg4 = std::make_unique<VarLengthTestMessage>(
      Compatibility::MAX_PROTOCOL_SUPPORTED, 2 * 1024);
  std::unique_ptr<Request> rq4 = std::make_unique<SenderVarLenMessageRequest>(
      sem, msg4, E::NOBUFS, secondNodeAddress);
  EXPECT_EQ(0, cl_w->tryPost(rq4));
  sem.wait();

  // Test server->client connection and CLIENT output buffer limits.

  // Create second client and add to the server processor.
  ld_info("Sending 600K to second node on second client");
  sem = Semaphore();
  auto cl_msg = std::make_unique<VarLengthTestMessage>(
      Compatibility::MAX_PROTOCOL_SUPPORTED, 600 * 1024);
  std::unique_ptr<Request> cl_rq = std::make_unique<SenderVarLenMessageRequest>(
      sem, cl_msg, E::OK, secondNodeAddress);
  EXPECT_EQ(0, cl_w2->tryPost(cl_rq));
  sem.wait();

  // Create first client connection for server2 on srv_processor.
  ld_info("Creating first client connection");
  int client_fd;
  Sockaddr client_socket = server2.accept(client_fd);
  std::unique_ptr<Request> ncrq =
      std::make_unique<NewConnectionRequest>(client_fd,
                                             srv_w->idx_,
                                             client_socket,
                                             ResourceBudget::Token(),
                                             ResourceBudget::Token(),
                                             SocketType::DATA,
                                             ConnectionType::NONE);
  sem = Semaphore();
  ncrq->setClientBlockedSemaphore(&sem);
  EXPECT_EQ(0, srv_w->tryPost(ncrq));
  sem.wait();

  // Create second client connection for server2 on srv_processor.
  ld_info("Creating second client connection");
  int client_fd2;
  Sockaddr client_socket2 = server2.accept(client_fd2);
  std::unique_ptr<Request> ncrq2 =
      std::make_unique<NewConnectionRequest>(client_fd2,
                                             srv_w->idx_,
                                             client_socket2,
                                             ResourceBudget::Token(),
                                             ResourceBudget::Token(),
                                             SocketType::DATA,
                                             ConnectionType::NONE);
  sem = Semaphore();
  ncrq2->setClientBlockedSemaphore(&sem);
  EXPECT_EQ(0, srv_w->tryPost(ncrq2));
  sem.wait();

  std::vector<ClientID> clientIDs;

  // Get the ClientIDs that the server's Sender has assigned to the incoming
  // connections. Also wait for the server to receive HELLO messages from
  // both clients; otherwise server's Sender will refuse to send messages with
  // E::UNREACHABLE.
  ld_info("Collecting ClientIDs and waiting for handshakes");
  while (true) {
    bool all_handshaken = true;
    clientIDs.clear();
    sem = Semaphore();
    srv_w->add([&]() {
      auto* worker = Worker::onThisThread();
      worker->sender().forAllClientConnections([&](Connection& c) {
        ClientID cid = c.peer_name_.asClientID();
        bool h = c.isHandshaken();
        all_handshaken &= h;
        clientIDs.push_back(c.peer_name_.asClientID());
        ld_debug("%s is %shandshaken", cid.toString().c_str(), h ? "" : "not ");
      });
      sem.post();
      return true;
    });
    sem.wait();

    if (all_handshaken) {
      break;
    }

    /* sleep override */
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  ASSERT_EQ(2, clientIDs.size());
  Address clientNodeAddress1(clientIDs[0]);
  Address clientNodeAddress2(clientIDs[1]);

  // Fill up the sender output buffer by sending to client 1.
  ld_info("Sending 700K to first client");
  sem = Semaphore();
  auto msg5 = std::make_unique<VarLengthTestMessage>(
      Compatibility::MAX_PROTOCOL_SUPPORTED, 700 * 1024);
  std::unique_ptr<Request> rq5 = std::make_unique<SenderVarLenMessageRequest>(
      sem, msg5, E::OK, clientNodeAddress1);
  EXPECT_EQ(0, srv_w->tryPost(rq5));
  sem.wait();

  sem = Semaphore();

  // If outbufs-limit-per-peer-type is enabled, expect ENOBUF as the sender's
  //  output buffer limit. Peer-type limit is enforced in server.
  E expected_err = E::NOBUFS;
  if (outBufsLimitPerPeerTypeDisabled) {
    expected_err = E::OK;
  }
  ld_info("Sending another 700K to first client");
  auto msg6 = std::make_unique<VarLengthTestMessage>(
      Compatibility::MAX_PROTOCOL_SUPPORTED, 700 * 1024);
  std::unique_ptr<Request> rq6 = std::make_unique<SenderVarLenMessageRequest>(
      sem, msg6, expected_err, clientNodeAddress1);
  EXPECT_EQ(0, srv_w->tryPost(rq6));
  sem.wait();

  // Expect ENOBUFS when outbufs-limit-per-peer-type is disabled now that
  //  outbufs sender limit is full.
  if (outBufsLimitPerPeerTypeDisabled) {
    ld_info("Sending yet another 700K to first client");
    sem = Semaphore();
    auto pmsg = std::make_unique<VarLengthTestMessage>(
        Compatibility::MAX_PROTOCOL_SUPPORTED, 700 * 1024);
    std::unique_ptr<Request> prq = std::make_unique<SenderVarLenMessageRequest>(
        sem, pmsg, E::NOBUFS, clientNodeAddress1);
    EXPECT_EQ(0, srv_w->tryPost(prq));
    sem.wait();
  }

  // Send to a different client and expect success due to new connection's
  // outbuf_socket_min_kb  guaranteed budget.
  ld_info("Sending 400K to second client");
  sem = Semaphore();
  auto msg7 = std::make_unique<VarLengthTestMessage>(
      Compatibility::MAX_PROTOCOL_SUPPORTED, 400 * 1024);
  std::unique_ptr<Request> rq7 = std::make_unique<SenderVarLenMessageRequest>(
      sem, msg7, E::OK, clientNodeAddress2);
  EXPECT_EQ(0, srv_w->tryPost(rq7));
  sem.wait();

  // Expect message over new connection to fail with ENOBUF as this is over both
  //  sender's outbuf limit and connection's outbuf_socket_min_kb.
  ld_info("Sending 1K to second client");
  sem = Semaphore();
  auto msg8 = std::make_unique<VarLengthTestMessage>(
      Compatibility::MAX_PROTOCOL_SUPPORTED, 1 * 1024);
  std::unique_ptr<Request> rq8 = std::make_unique<SenderVarLenMessageRequest>(
      sem, msg8, E::NOBUFS, clientNodeAddress2);
  EXPECT_EQ(0, srv_w->tryPost(rq8));
  sem.wait();
}

/**
 * Tests to verify Sender and Connection outbuf limits.
 *   Tests the CLIENT outbufs budget by
 *     -  sending the client messages and using up sender's
 *        outbufs_mb_max_per_thread / 2 budget for NODE connections.
 *     - Verifies that ENOBUF error is received after NODE output buffer is full
 *     - Verified that messages of upto outbuf_socket_min_kb can be sent over
 *       a different server connection while the sender's NODE output buf is
 * full.
 *
 *   The above set of tests are repeated for CLIENT output buffer budget as
 *   well ( by having server node sending to two client end points).
 */
TEST_P(MessagingSocketTest, SenderOutBufLimitsPerPeerType) {
  testOutBufsLimit(false, GetParam());
}

/**
 * Tests to verify Sender and Connection outbuf limits.
 *   This test verifies the sender outbuf limit without the per peer-type
 *   limit.
 *  It also verifies the per connection minimum guaranteed budget.
 */
TEST_P(MessagingSocketTest, SenderOutBufPerPeerLimitsDisabled) {
  testOutBufsLimit(true, GetParam());
}

struct SendStoredWithTimeoutRequest : public Request {
  SendStoredWithTimeoutRequest()
      : Request(RequestType::TEST_MESSAGING_SEND_STORED_WITH_TIMEOUT_REQUEST) {}
  Request::Execution execute() override {
    ThreadID::set(ThreadID::SERVER_WORKER, "");
    Worker* w = Worker::onThisThread();

    auto msg = std::make_unique<STORED_Message>(SenderBasicSendRequest::hdr1out,
                                                0,
                                                0,
                                                log_rebuilding_id_t(0),
                                                0,
                                                ServerInstanceId_INVALID);
    int rv = w->sender().sendMessage(std::move(msg), firstNodeID, new OnClose);
    EXPECT_EQ(0, rv);
    EXPECT_FALSE(msg);

    return Execution::COMPLETE;
  }

  class OnClose : public SocketCallback {
   public:
    void operator()(Status st, const Address& /*name*/) override {
      EXPECT_EQ(E::TIMEDOUT, st);
      delete this;
    }
  };
};

/**
 * Use nc as a server and send HELLO to it. Make sure that the client socket
 * is closed after some time (since we haven't received an ACK).
 */
TEST_P(MessagingSocketTest, OnHandshakeTimeout) {
  Settings settings = create_default_settings<Settings>();
  settings.include_cluster_name_on_handshake = true;
  settings.include_destination_on_handshake = true;
  settings.handshake_timeout = std::chrono::milliseconds(1000);
  settings.use_legacy_eventbase =
      GetParam() == EvBase::EvBaseType::LEGACY_EVENTBASE;
  UpdateableSettings<Settings> updateable_settings(settings);

  ServerSocket server;
  std::shared_ptr<UpdateableConfig> config(
      create_config(std::vector<int>{server.getPort()}));

  Processor processor(config, updateable_settings);

  auto out = createWorker(&processor, config, GetParam());
  auto w = out.worker.get();
  SCOPE_EXIT {
    w->sender().shutdownSockets(w);
  };
  std::unique_ptr<Request> req =
      std::make_unique<SendStoredWithTimeoutRequest>();
  EXPECT_EQ(0, w->tryPost(req));

  // Accept the connection, swallow the HELLO message but do not send ACK.
  const int fd = server.accept();
  HELLO_Raw hello;
  ASSERT_EQ(sizeof(hello), read(fd, &hello, sizeof(hello)));

  // Wait until the connection is closed.
  char c;
  ASSERT_EQ(0, read(fd, &c, 1));
}

struct SendMessageOnCloseRequest : public Request {
  explicit SendMessageOnCloseRequest(Semaphore* s)
      : Request(RequestType::TEST_MESSAGING_SEND_MESSAGE_FROM_CLOSE_CB_REQUEST),
        sem(s) {}
  Request::Execution execute() override {
    ThreadID::set(ThreadID::SERVER_WORKER, "");
    Worker* w = Worker::onThisThread();
    auto& sender = w->sender();
    auto msg = std::make_unique<VarLengthTestMessage>(
        Compatibility::MAX_PROTOCOL_SUPPORTED, 10);
    if (first_msg_) {
      // First message connects to remote and completes the handshake.
      msg->setOnSent([waiter = sem](Status st, const Address&) {
        ASSERT_TRUE(st == E::OK);
        auto onclose_req = std::make_unique<SendMessageOnCloseRequest>(waiter);
        onclose_req->first_msg_ = false;
        std::unique_ptr<Request> rq(std::move(onclose_req));
        EXPECT_EQ(0, Worker::onThisThread()->tryPost(rq));
      });
      int rv = sender.sendMessage(std::move(msg), firstNodeID);
      EXPECT_EQ(0, rv);
      EXPECT_FALSE(msg);
    } else {
      auto on_close = new OnClose;
      int rv = sender.sendMessage(std::move(msg), firstNodeID, on_close);
      EXPECT_EQ(0, rv);
      EXPECT_FALSE(msg);
      auto conn = sender.findServerConnection(firstNodeID.index());
      EXPECT_GT(conn->getBufferedBytesSize(), 0);
      conn->close(E::INTERNAL);
      EXPECT_EQ(conn->getBufferedBytesSize(), 0);
      EXPECT_EQ(conn->getBytesPending(), 0);
      EXPECT_TRUE(conn->isClosed());
      sem->post();
    }

    return Execution::COMPLETE;
  }

  class OnClose : public SocketCallback {
   public:
    void operator()(Status st, const Address& /*name*/) override {
      ASSERT_EQ(st, E::INTERNAL);
      Worker* w = Worker::onThisThread();
      auto& sender = w->sender();
      auto conn = sender.findServerConnection(firstNodeID.index());
      int rv =
          sender.sendMessage(std::make_unique<VarLengthTestMessage>(
                                 Compatibility::MAX_PROTOCOL_SUPPORTED, 10),
                             firstNodeID);
      EXPECT_EQ(0, rv);
      EXPECT_NE(conn, sender.findServerConnection(firstNodeID.index()));
      delete this;
    }
  };

  Semaphore* sem{nullptr};
  bool first_msg_{true};
};

TEST_P(MessagingSocketTest, SendFromCloseCB) {
  Settings settings = create_default_settings<Settings>();
  settings.include_cluster_name_on_handshake = true;
  settings.include_destination_on_handshake = true;
  settings.use_legacy_eventbase =
      GetParam() == EvBase::EvBaseType::LEGACY_EVENTBASE;
  settings.connect_throttle =
      chrono_expbackoff_t<std::chrono::milliseconds>(0, 0);
  UpdateableSettings<Settings> updateable_settings(settings);

  ServerSocket server;
  std::shared_ptr<UpdateableConfig> config(
      create_config(std::vector<int>{server.getPort()}));

  std::shared_ptr<Processor> processor =
      std::make_shared<Processor>(config, updateable_settings);

  ld_check((bool)config);

  auto out = createWorker(processor.get(), config, GetParam());
  auto w = out.worker.get();
  SCOPE_EXIT {
    w->sender().shutdownSockets(w);
  };
  ASSERT_FALSE(out.loop->getThread().isCurrentThread());

  config.reset();

  Semaphore sem;
  std::unique_ptr<Request> rq =
      std::make_unique<SendMessageOnCloseRequest>(&sem);

  EXPECT_EQ(0, w->tryPost(rq));

  // Wait for worker to connect and send message.
  int fd = server.accept();
  ASSERT_NE(fd, -1);

  // Complete the handshake so that message get written into the real socket
  // instead of being held up in serializeq.
  HELLO_Raw hin;
  CONFIG_ADVISORY_Raw cin;
  STORED_Raw r1in, r2in;

  // Skip initial HELLO message.
  ASSERT_EQ(sizeof(HELLO_Raw), read(fd, &hin, sizeof(HELLO_Raw)));

  // Construct and send an ACK message in response.
  ACK_Raw ack;
  ack.ph.len = sizeof(ACK_Raw);
  ack.ph.type = MessageType::ACK;
  ack.hdr.options = 0;
  ack.hdr.rqid = request_id_t(42);
  ack.hdr.client_idx = 1;
  ack.hdr.proto = Compatibility::MAX_PROTOCOL_SUPPORTED;
  ack.hdr.status = E::OK;
  ASSERT_EQ(sizeof(ACK_Raw), write(fd, &ack, sizeof(ACK_Raw)));

  // Skip CONFIG_ADVISORY
  size_t expected_size_of_config_advisory = sizeof(CONFIG_ADVISORY_Raw) -
      (ProtocolHeader::needChecksumInHeader(
           MessageType::CONFIG_ADVISORY, ack.hdr.proto)
           ? 0
           : sizeof(ProtocolHeader::cksum));
  ASSERT_EQ(expected_size_of_config_advisory,
            read(fd, &cin, expected_size_of_config_advisory));
  EXPECT_EQ(MessageType::CONFIG_ADVISORY, cin.ph.type);
  EXPECT_EQ(expected_size_of_config_advisory, cin.ph.len);

  // Once the first connection is closed and onclose is invoked sem post will be
  // invoked to finish this wait.
  sem.wait();

  // New message sent from onClose should create this new connection.
  fd = server.accept();
  // OnClose should lead to a new connection request to server.
  ASSERT_NE(fd, -1);
}

// Used by AckProtoNoSupportClose test. Send a DummyMessage. Expect
// DummyMessage::onSent() and the connection close callback to be called with
// E::PROTONOSUPPORT because the other end sent ACK with E::PROTONOSUPPORT.
struct SendMessageOnCloseProtoNoSupport : public Request {
  explicit SendMessageOnCloseProtoNoSupport(Semaphore& sem)
      : Request(RequestType::TEST_SENDMESSAGE_ON_CLOSE_PROTONOSUPPORT_REQUEST),
        sem_(sem),
        close_callback_(new OnClose(sem)) {}
  Request::Execution execute() override {
    ThreadID::set(ThreadID::SERVER_WORKER, "");
    Worker* w = Worker::onThisThread();

    // Since the connection will be closed with E::PROTONOSUPPORT, the message
    // should be rejected with that error code as well.
    auto msg = std::make_unique<DummyMessage>(sem_, E::PROTONOSUPPORT);
    const int rv =
        w->sender().sendMessage(std::move(msg), firstNodeID, close_callback_);
    EXPECT_EQ(0, rv);
    EXPECT_FALSE(msg);

    return Execution::COMPLETE;
  }

 private:
  class OnClose : public SocketCallback {
   public:
    explicit OnClose(Semaphore& sem) : sem_(sem) {}
    void operator()(Status st, const Address& /*name*/) override {
      EXPECT_EQ(E::PROTONOSUPPORT, st);
      sem_.post();
      delete this;
    }
    Semaphore& sem_;
  };

  Semaphore& sem_;
  OnClose* close_callback_;
};

// Used by MessageProtoNoSupportOnSent. Here the other end sends ACK with proto
// equal to Compatibility::MIN_PROTOCOL_SUPPORTED. The connection does not close
// since we support that prototocol. However, two messages were enqueued. One
// that is not compatible with this protocol, and one that is compatible. We
// verify that the first one gets its onSent() method called with
// E::PROTONOSUPPORT and the second one is successfully sent.
struct SendMessageExpectBadProtoRequest : public Request {
  explicit SendMessageExpectBadProtoRequest(Semaphore& sem, bool sync)
      : Request(RequestType::TEST_SENDMESSAGE_EXPECT_BADPROTO_REQUEST),
        sem_(sem),
        synchronous_error_(sync) {}
  Request::Execution execute() override {
    ThreadID::set(ThreadID::SERVER_WORKER, "");
    Worker* w = Worker::onThisThread();

    // Protocol version validation can only occur once we are connected
    // and handshake processing has completed. If synchronous_error_ is
    // false, we are not yet connected, and sendMessage() should return
    // success for both of these requests. The success status indicates all
    // checks that can be performed pre-handshake were successful and the
    // message was queued at the connection layer to await completion of
    // connection processing. If synchronous_error_ is true, we are running
    // after handshake negotiation and so should see a synchronous
    // E::PROTONOSUPPORT error for the first message.

    // first send a message that should be rejected.
    auto msg = std::make_unique<DummyMessage>(sem_, E::PROTONOSUPPORT);
    msg->min_proto_ = Compatibility::MIN_PROTOCOL_SUPPORTED + 1;
    int rv = w->sender().sendMessage(std::move(msg), firstNodeID);
    if (synchronous_error_) {
      // Protocol negotiation is complete, so this should fail immediately.
      EXPECT_EQ(-1, rv);
      EXPECT_EQ(E::PROTONOSUPPORT, err);
      EXPECT_TRUE(msg);
    } else {
      // The message is queued and will be failed asynchronously once
      // protocol negotiation completes.
      EXPECT_EQ(0, rv);
      EXPECT_FALSE(msg);
    }

    // Then send a message that should be accepted.
    msg = std::make_unique<DummyMessage>(sem_, E::OK);
    msg->min_proto_ = Compatibility::MIN_PROTOCOL_SUPPORTED;
    rv = w->sender().sendMessage(std::move(msg), firstNodeID);
    EXPECT_EQ(0, rv);
    EXPECT_FALSE(msg);
    return Execution::COMPLETE;
  }

 private:
  Semaphore& sem_;
  bool synchronous_error_;
};

/**
 * If server sends ACK with E::PROTONOSUPPORT error, client should close
 * connection. Even if server doesns't close it and never reads from it.
 */
TEST_P(MessagingSocketTest, AckProtoNoSupportClose) {
  Settings settings = create_default_settings<Settings>();
  settings.use_legacy_eventbase =
      GetParam() == EvBase::EvBaseType::LEGACY_EVENTBASE;
  UpdateableSettings<Settings> updateable_settings(settings);
  ServerSocket server;
  std::shared_ptr<UpdateableConfig> config(
      create_config(std::vector<int>{server.getPort()}));

  Processor processor(config, updateable_settings);
  auto out = createWorker(&processor, config, GetParam());
  auto w = out.worker.get();
  SCOPE_EXIT {
    w->sender().shutdownSockets(w);
  };
  Semaphore sem;
  auto raw_req = new SendMessageOnCloseProtoNoSupport(sem);
  std::unique_ptr<Request> req(raw_req);

  EXPECT_EQ(0, w->tryPost(req));

  const int fd = server.accept();

  // Construct and send an ACK message with E::PROTONOSUPPORT error.
  ACK_Raw ack;
  ack.ph.len = sizeof(ACK_Raw);
  ack.ph.type = MessageType::ACK;
  ack.hdr.options = 0;
  ack.hdr.rqid = request_id_t(42);
  ack.hdr.client_idx = 1;
  ack.hdr.proto = 0;
  ack.hdr.status = E::PROTONOSUPPORT;
  ASSERT_EQ(sizeof(ACK_Raw), write(fd, &ack, sizeof(ACK_Raw)));

  // Wait for DummyMessage::onSent() and OnClose() to be called.
  sem.wait();
  sem.wait();
}

// Test a case where the other end sends ACK with proto equal to
// Compatibility::MIN_PROTOCOL_SUPPORTED. The connection does not close since we
// support that prototocol. However, two messages were enqueued. One that is
// not compatible with this protocol, and one that is compatible. We verify
// that the first one gets its onSent() method called with E::PROTONOSUPPORT
// and the second one is successfully sent.
TEST_P(MessagingSocketTest, MessageProtoNoSupportOnSent) {
  Settings settings = create_default_settings<Settings>();
  settings.include_cluster_name_on_handshake = true;
  settings.include_destination_on_handshake = true;
  settings.handshake_timeout = std::chrono::milliseconds(1000);
  settings.use_legacy_eventbase =
      GetParam() == EvBase::EvBaseType::LEGACY_EVENTBASE;
  UpdateableSettings<Settings> updateable_settings(settings);
  ServerSocket server;
  std::shared_ptr<UpdateableConfig> config(
      create_config(std::vector<int>{server.getPort()}));

  Processor processor(config, updateable_settings);
  auto out = createWorker(&processor, config, GetParam());
  auto w = out.worker.get();
  SCOPE_EXIT {
    w->sender().shutdownSockets(w);
  };
  Semaphore sem;
  std::unique_ptr<Request> req;
  req.reset(new SendMessageExpectBadProtoRequest(sem, /*sync*/ false));

  EXPECT_EQ(0, w->tryPost(req));

  const int fd = server.accept();
  HELLO_Raw hello;
  ASSERT_EQ(sizeof(hello), read(fd, &hello, sizeof(hello)));
  EXPECT_EQ(MessageType::HELLO, hello.ph.type);

  // Construct and send an ACK message with protocol
  ACK_Raw ack;
  ack.ph.len = sizeof(ACK_Raw);
  ack.ph.type = MessageType::ACK;
  ack.hdr.options = 0;
  ack.hdr.rqid = request_id_t(42);
  ack.hdr.client_idx = 1;
  ack.hdr.status = E::OK;
  ack.hdr.proto = Compatibility::MIN_PROTOCOL_SUPPORTED;
  ASSERT_EQ(sizeof(ACK_Raw), write(fd, &ack, sizeof(ACK_Raw)));

  // Wait for the two messages to be sent.
  sem.wait();
  sem.wait();

  // With the handshake complete, messages sent with an unsupported
  // protocol version should fail synchronously.
  req.reset(new SendMessageExpectBadProtoRequest(sem, /*sync*/ true));
  EXPECT_EQ(0, w->tryPost(req));

  // Only one of the two messages will actually be transmitted and have
  // their onSent() callback invoked.
  sem.wait();
}

// Used by AckInvalidClusterClose test. Very similar to
// SendMessageOnCloseProtoNoSupport (look above)
struct SendMessageOnCloseInvalidCluster : public Request {
  explicit SendMessageOnCloseInvalidCluster(Semaphore& sem)
      : Request(RequestType::TEST_SENDMESSAGE_ON_CLOSE_INVALID_CLUSTER_REQUEST),
        sem_(sem),
        close_callback_(new OnClose(sem)) {}
  Request::Execution execute() override {
    ThreadID::set(ThreadID::SERVER_WORKER, "");
    Worker* w = Worker::onThisThread();

    // Since the connection will be closed with E::INVALID_CLUSTER, the message
    // should be rejected with that error code as well.
    auto msg = std::make_unique<DummyMessage>(sem_, E::INVALID_CLUSTER);
    const int rv =
        w->sender().sendMessage(std::move(msg), firstNodeID, close_callback_);
    EXPECT_EQ(0, rv);
    EXPECT_FALSE(msg);

    return Execution::COMPLETE;
  }

 private:
  class OnClose : public SocketCallback {
   public:
    explicit OnClose(Semaphore& sem) : sem_(sem) {}
    void operator()(Status st, const Address& /*name*/) override {
      EXPECT_EQ(E::INVALID_CLUSTER, st);
      sem_.post();
      delete this;
    }
    Semaphore& sem_;
  };

  Semaphore& sem_;
  OnClose* close_callback_;
};

/**
 * If server sends ACK with E::INVALID_CLUSTER error, client should close
 * connection. Even if server doesns't close it and never reads from it.
 */
TEST_P(MessagingSocketTest, AckInvalidClusterClose) {
  Settings settings = create_default_settings<Settings>();
  settings.include_cluster_name_on_handshake = true;
  settings.include_destination_on_handshake = true;
  settings.use_legacy_eventbase =
      GetParam() == EvBase::EvBaseType::LEGACY_EVENTBASE;
  UpdateableSettings<Settings> updateable_settings(settings);
  ServerSocket server;
  std::shared_ptr<UpdateableConfig> config(
      create_config(std::vector<int>{server.getPort()}));

  Processor processor(config, updateable_settings);
  auto out = createWorker(&processor, config, GetParam());
  auto w = out.worker.get();
  SCOPE_EXIT {
    w->sender().shutdownSockets(w);
  };
  Semaphore sem;
  auto raw_req = new SendMessageOnCloseInvalidCluster(sem);
  std::unique_ptr<Request> req(raw_req);

  EXPECT_EQ(0, w->tryPost(req));

  const int fd = server.accept();

  // Construct and send an ACK message with E::INVALID_CLUSTER error.
  ACK_Raw ack;
  ack.ph.len = sizeof(ACK_Raw);
  ack.ph.type = MessageType::ACK;
  ack.hdr.options = 0;
  ack.hdr.rqid = request_id_t(42);
  ack.hdr.client_idx = 1;
  ack.hdr.status = E::INVALID_CLUSTER;
  ASSERT_EQ(sizeof(ACK_Raw), write(fd, &ack, sizeof(ACK_Raw)));

  // Wait for DummyMessage::onSent() and OnClose() to be called.
  sem.wait();
  sem.wait();
}

// Used by ReentrantOnSent test.
struct SendReentrantMessage : public Request {
  explicit SendReentrantMessage(Semaphore& sem)
      : Request(RequestType::TEST_SENDMESSAGE_EXPECT_TWO_MESSAGES_SENT),
        sem_(sem) {}
  Request::Execution execute() override {
    ThreadID::set(ThreadID::SERVER_WORKER, "");
    Worker* w = Worker::onThisThread();

    auto msg = std::make_unique<ReentrantDummyMessage>(sem_, E::CANCELLED);
    int rv = w->sender().sendMessage(std::move(msg), firstNodeID);
    EXPECT_EQ(0, rv);
    EXPECT_FALSE(msg);
    return Execution::COMPLETE;
  }

 private:
  Semaphore& sem_;
};

/**
 * Queue message and complete handshake. Both the original message and the
 * message sent from the ReentrantDummyMessage::onSent() handler should be
 * transmitted. Queue message again post handshake and again both messages
 * should be sent.
 */
TEST_P(MessagingSocketTest, ReentrantOnSent) {
  Settings settings = create_default_settings<Settings>();
  settings.use_legacy_eventbase =
      GetParam() == EvBase::EvBaseType::LEGACY_EVENTBASE;
  UpdateableSettings<Settings> updateable_settings(settings);
  ServerSocket server;
  std::shared_ptr<UpdateableConfig> config(
      create_config(std::vector<int>{server.getPort()}));
  Processor processor(config, updateable_settings);
  auto out = createWorker(&processor, config, GetParam());
  auto w = out.worker.get();
  SCOPE_EXIT {
    w->sender().shutdownSockets(w);
  };
  Semaphore sem;
  std::unique_ptr<Request> req;
  req.reset(new SendReentrantMessage(sem));

  // Queue up prior to handshake so that our messages is processed from
  // handshake completion context.
  EXPECT_EQ(0, w->tryPost(req));

  const int fd = server.accept();

  HELLO_Raw hello;
  ASSERT_EQ(sizeof(hello), read(fd, &hello, sizeof(hello)));
  // Construct and send an ACK message in response.
  ACK_Raw ack;
  ack.ph.len = sizeof(ACK_Raw);
  ack.ph.type = MessageType::ACK;
  ack.hdr.options = 0;
  ack.hdr.rqid = request_id_t(42);
  ack.hdr.client_idx = 1;
  ack.hdr.proto = Compatibility::MAX_PROTOCOL_SUPPORTED;
  ack.hdr.status = E::OK;
  ASSERT_EQ(sizeof(ACK_Raw), write(fd, &ack, sizeof(ACK_Raw)));

  // Wait for ReentrantDummyMessage::onSent() and DummyMessage::onSent()
  // to be called.
  sem.wait();
  sem.wait();

  // Now that handshake processing is complete, queue up again so the
  // message is sent from Sender::sendMessage() context.
  req.reset(new SendReentrantMessage(sem));
  EXPECT_EQ(0, w->tryPost(req));

  // Wait for ReentrantDummyMessage::onSent() and DummyMessage::onSent()
  // to be called.
  sem.wait();
  sem.wait();
}

/**
 * Starts a cluster with --test-reject-hello=PROTONOSUPPORT on the
 * sequencer node. Suspends the sequencer logdeviced, makes several append()
 * requests. Sends a SIGCONT to the sequencer. Expects all requests to
 * fail with CONNFAILED.
 */
TEST_P(MessagingSocketTest, PROTONOSUPPORT) {
  auto cluster =
      IntegrationTestUtils::ClusterFactory()
          .setParam("--test-reject-hello",
                    "PROTONOSUPPORT",
                    IntegrationTestUtils::ParamScope::SEQUENCER)
          .setParam("--use-legacy-eventbase",
                    GetParam() == EvBase::EvBaseType::LEGACY_EVENTBASE
                        ? "true"
                        : "false")
          .create(1);

  cluster->getSequencerNode().suspend();

  std::unique_ptr<ClientSettings> client_settings(ClientSettings::create());
  ASSERT_EQ(0,
            client_settings->set(
                "use-legacy-eventbase",
                GetParam() == EvBase::EvBaseType::LEGACY_EVENTBASE ? "true"
                                                                   : "false"));

  std::shared_ptr<Client> client =
      cluster->createClient(testTimeout(), std::move(client_settings));
  ASSERT_TRUE((bool)client);

  char data[128]; // send the contents of this array as payload
  std::atomic<int> cb_called(0);

  auto check_status_cb = [&](Status st, const DataRecord& /*r*/) {
    cb_called++;
    EXPECT_EQ(E::CONNFAILED, st);
  };

  Payload payload1(data, 1);
  Payload payload2(data, 2);
  Payload payload3(data, 3);

  client->append(logid_t(2), payload1, check_status_cb);
  client->append(logid_t(2), payload2, check_status_cb);
  client->append(logid_t(2), payload3, check_status_cb);

  cluster->getSequencerNode().resume();

  while (cb_called.load() < 3) {
    /* sleep override */
    sleep(1);
  }

  client.reset(); // this blocks until all Worker threads shut down
}

/**
 * Starts a cluster with --test-reject-hello=DESTINATION_MISMATCH on the
 * sequencer node. Suspends the sequencer logdeviced, makes several append()
 * requests. Sends a SIGCONT to the sequencer. Expects all requests to
 * fail with CONNFAILED.
 */
TEST_P(MessagingSocketTest, DestinationMismatchTestReject) {
  auto cluster =
      IntegrationTestUtils::ClusterFactory()
          .setParam("--test-reject-hello",
                    "DESTINATION_MISMATCH",
                    IntegrationTestUtils::ParamScope::SEQUENCER)
          .setParam("--use-legacy-eventbase",
                    GetParam() == EvBase::EvBaseType::LEGACY_EVENTBASE
                        ? "true"
                        : "false")
          .create(1);

  cluster->getSequencerNode().suspend();

  std::unique_ptr<ClientSettings> client_settings(ClientSettings::create());
  ASSERT_EQ(0,
            client_settings->set(
                "use-legacy-eventbase",
                GetParam() == EvBase::EvBaseType::LEGACY_EVENTBASE ? "true"
                                                                   : "false"));

  std::shared_ptr<Client> client =
      cluster->createClient(testTimeout(), std::move(client_settings));
  ASSERT_TRUE((bool)client);

  char data[128]; // send the contents of this array as payload
  std::atomic<int> cb_called(0);

  auto check_status_cb = [&](Status st, const DataRecord& /*r*/) {
    cb_called++;
    EXPECT_EQ(E::CONNFAILED, st);
  };

  Payload payload1(data, 1);
  Payload payload2(data, 2);
  Payload payload3(data, 3);

  client->append(logid_t(2), payload1, check_status_cb);
  client->append(logid_t(2), payload2, check_status_cb);
  client->append(logid_t(2), payload3, check_status_cb);

  cluster->getSequencerNode().resume();

  while (cb_called.load() < 3) {
    /* sleep override */
    sleep(1);
  }

  client.reset(); // this blocks until all Worker threads shut down
}

/**
 * Starts a cluster with --test-reject-hello=INVALID_CLUSTER on the
 * sequencer node. Suspends the sequencer logdeviced, makes several append()
 * requests. Sends a SIGCONT to the sequencer. Expects all requests to
 * fail with CONNFAILED.
 */
TEST_P(MessagingSocketTest, InvalidClusterNameTestReject) {
  auto cluster =
      IntegrationTestUtils::ClusterFactory()
          .setParam("--test-reject-hello",
                    "INVALID_CLUSTER",
                    IntegrationTestUtils::ParamScope::SEQUENCER)
          .setParam("--use-legacy-eventbase",
                    GetParam() == EvBase::EvBaseType::LEGACY_EVENTBASE
                        ? "true"
                        : "false")
          .create(1);

  cluster->getSequencerNode().suspend();

  std::unique_ptr<ClientSettings> client_settings(ClientSettings::create());
  ASSERT_EQ(0,
            client_settings->set(
                "use-legacy-eventbase",
                GetParam() == EvBase::EvBaseType::LEGACY_EVENTBASE ? "true"
                                                                   : "false"));

  std::shared_ptr<Client> client =
      cluster->createClient(std::chrono::hours(1), std::move(client_settings));
  ASSERT_TRUE((bool)client);

  char data[128]; // send the contents of this array as payload
  std::atomic<int> cb_called(0);

  auto check_status_cb = [&](Status st, const DataRecord& /*r*/) {
    cb_called++;
    EXPECT_EQ(E::CONNFAILED, st);
  };

  Payload payload1(data, 1);
  Payload payload2(data, 2);
  Payload payload3(data, 3);

  client->append(logid_t(2), payload1, check_status_cb);
  client->append(logid_t(2), payload2, check_status_cb);
  client->append(logid_t(2), payload3, check_status_cb);

  cluster->getSequencerNode().resume();

  while (cb_called.load() < 3) {
    /* sleep override */
    sleep(1);
  }

  client.reset(); // this blocks until all Worker threads shut down
}

/**
 * Sends an APPEND request to a logdeviced to establish a connection.
 * Suspends logdeviced. Sends another append with a large timeout.
 * Kills logdeviced. Expects the second append to fail with E::CONNFAILED.
 */
TEST_P(MessagingSocketTest, ServerCloses) {
  auto cluster =
      IntegrationTestUtils::ClusterFactory()
          .setParam("--use-legacy-eventbase",
                    GetParam() == EvBase::EvBaseType::LEGACY_EVENTBASE
                        ? "true"
                        : "false")
          .create(1);

  std::unique_ptr<ClientSettings> client_settings(ClientSettings::create());
  ASSERT_EQ(0,
            client_settings->set(
                "use-legacy-eventbase",
                GetParam() == EvBase::EvBaseType::LEGACY_EVENTBASE ? "true"
                                                                   : "false"));

  std::shared_ptr<Client> client =
      cluster->createClient(testTimeout(), std::move(client_settings));

  ASSERT_TRUE((bool)client);

  char data[128]; // send the contents of this array as payload
  Payload payload(data, sizeof(data));
  int rv;

  lsn_t lsn = client->appendSync(logid_t(1), payload);

  // Should have succeeded
  EXPECT_NE(LSN_INVALID, lsn);

  client->setTimeout(std::chrono::milliseconds::max());

  cluster->getSequencerNode().suspend();

  std::atomic<bool> cb_called(false);
  rv = client->append(logid_t(1), payload, [&](Status st, const DataRecord& r) {
    cb_called.store(true);
    ASSERT_EQ(LSN_INVALID, r.attrs.lsn);
    EXPECT_EQ(E::CONNFAILED, st);
  });
  EXPECT_EQ(0, rv);

  cluster->getSequencerNode().kill();
  /* sleep override */
  sleep(1);

  client.reset(); // this blocks until all Worker threads shut down
  ASSERT_TRUE(cb_called.load());
}

TEST_P(MessagingSocketTest, ServerShutdownWithOpenConnections) {
  auto cluster =
      IntegrationTestUtils::ClusterFactory()
          .setNumLogs(1)
          .setParam("--num-workers", "1")
          .setParam("--use-legacy-eventbase",
                    GetParam() == EvBase::EvBaseType::LEGACY_EVENTBASE
                        ? "true"
                        : "false")
          .create(1);

  std::unique_ptr<ClientSettings> settings(ClientSettings::create());
  ASSERT_EQ(0, settings->set("num-workers", "1"));
  ASSERT_EQ(0,
            settings->set("use-legacy-eventbase",
                          GetParam() == EvBase::EvBaseType::LEGACY_EVENTBASE
                              ? "true"
                              : "false"));

  auto client = cluster->createClient(testTimeout(), std::move(settings));

  ASSERT_TRUE((bool)client);

  char data[128]; // send the contents of this array as payload
  Payload payload(data, sizeof(data));

  lsn_t lsn = client->appendSync(logid_t(1), payload);

  ASSERT_NE(LSN_INVALID, lsn);

  // Kill sequencer node
  IntegrationTestUtils::Node& node = cluster->getSequencerNode();
  node.signal(SIGTERM);
  node.waitUntilExited();

  Stats stats = checked_downcast<ClientImpl&>(*client).stats()->aggregate();
  ASSERT_EQ(1,
            stats.per_message_type_stats[(int)MessageType::SHUTDOWN]
                .message_received);

  lsn = client->appendSync(logid_t(1), payload);
  ASSERT_EQ(LSN_INVALID, lsn);
  EXPECT_EQ(E::CONNFAILED, err);
}

// Verifies that messages that have different sizes when they're queued by the
// connection layer (put into serializeq_) as opposed to being flushed to the
// output evbuffer (when the protocol version of the peer is finally known)
// don't cause crashes. See t6281298 for more details.
TEST_P(MessagingSocketTest, DifferentProtocolsT6281298) {
  std::string proto = std::to_string(Compatibility::MIN_PROTOCOL_SUPPORTED);
  auto cluster =
      IntegrationTestUtils::ClusterFactory()
          .setParam("--max-protocol", proto) // use an old protocol version.
          .doPreProvisionEpochMetaData()     // avoids running a STORE that has
                                         // flags incompatible with the proto
                                         // version
          .setParam("--use-legacy-eventbase",
                    GetParam() == EvBase::EvBaseType::LEGACY_EVENTBASE
                        ? "true"
                        : "false")
          .create(1);

  std::unique_ptr<ClientSettings> client_settings(ClientSettings::create());
  ASSERT_EQ(0, client_settings->set("connect-timeout", "5s"));
  ASSERT_EQ(0, client_settings->set("handshake-timeout", "5s"));
  ASSERT_EQ(0,
            client_settings->set(
                "use-legacy-eventbase",
                GetParam() == EvBase::EvBaseType::LEGACY_EVENTBASE ? "true"
                                                                   : "false"));
  auto client =
      cluster->createClient(testTimeout(), std::move(client_settings));
  ASSERT_TRUE((bool)client);

  // This is what happens: we start reading while the cluster is temporarily
  // suspended. As a result, HELLO and START messages (the one for the newest
  // protocol version) to the node get queued by the client. Once the cluster
  // is resumed and the handshake completes, a different START message needs
  // to be sent (since we now know that the server can only speak protocol
  // v5).
  cluster->getNode(0).suspend();
  auto reader = client->createReader(1);
  ASSERT_EQ(0, reader->startReading(logid_t(1), LSN_OLDEST));
  cluster->getNode(0).resume();
}

INSTANTIATE_TEST_CASE_P(MessagingSocketTestAllBase,
                        MessagingSocketTest,
                        ::testing::Values(EvBase::EvBaseType::LEGACY_EVENTBASE,
                                          EvBase::EvBaseType::FOLLY_EVENTBASE));

} // namespace
