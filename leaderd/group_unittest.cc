// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#include <base/message_loop/message_loop.h>
#include <base/run_loop.h>
#include <base/timer/mock_timer.h>
#include <chromeos/mime_utils.h>
#include <chromeos/http/http_transport_fake.h>
#include <dbus/bus.h>
#include <dbus/mock_bus.h>
#include <dbus/mock_exported_object.h>
#include <dbus/object_path.h>
#include <gtest/gtest.h>

#include "leaderd/group.h"
#include "leaderd/manager.h"

using base::MockTimer;
using base::Timer;
using chromeos::dbus_utils::ExportedObjectManager;
using dbus::MockBus;
using dbus::MockExportedObject;
using dbus::ObjectPath;
using testing::AnyNumber;
using testing::Invoke;
using testing::ReturnRef;
using testing::_;

namespace leaderd {

namespace {

const char kObjectManagerPath[] = "/objman";
const char kGroupPath[] = "/objman/group";
const char kGroupName[] = "ABC";
const char kScore = 25;
const char kTestDBusSource[] = "TestDBusSource";
const char kSelfUUID[] = "this is my own uuid";
const int32_t kSelfScore = 100;
const char kPeerIdLessThanSelf[] = "a peer id that is less than self";
const char kPeerIdGreaterThanSelf[] = "wow, an id that is greater than self";

const char kTestGroupMember[] = "a peer that could be in our group";
const uint16_t kTestGroupMemberPort{8080};
const uint8_t kTestGroupMemberOctetValue{10};
const char kTestGroupMemberChallengeUrl[] =
    "http://10.10.10.10:8080/privet/v3/leadership/challenge";
const char kTestGroupMemberAnnounceUrl[] =
    "http://10.10.10.10:8080/privet/v3/leadership/announce";

// Chrome doesn't bother mocking out their objects completely.
class EspeciallyMockedBus : public dbus::MockBus {
 public:
  using dbus::MockBus::MockBus;

  MOCK_METHOD2(GetServiceOwner, void(const std::string& service_name,
                                     const GetServiceOwnerCallback& callback));

  MOCK_METHOD2(ListenForServiceOwnerChange,
               void(const std::string& service_name,
                    const GetServiceOwnerCallback& callback));

  MOCK_METHOD2(UnlistenForServiceOwnerChange,
               void(const std::string& service_name,
                    const GetServiceOwnerCallback& callback));

 protected:
  virtual ~EspeciallyMockedBus() = default;
};

std::vector<std::tuple<std::vector<uint8_t>, uint16_t>> ReturnsFakeIpInfo(
        const std::string& peer_uuid) {
  using PeerIp = std::vector<uint8_t>;
  using PeerAddress = std::tuple<PeerIp, uint16_t>;
  PeerAddress peer_ip{PeerIp(4, kTestGroupMemberOctetValue),
                      kTestGroupMemberPort};
  return std::vector<PeerAddress>{peer_ip};
}

class MockGroupDelegate : public Group::Delegate {
 public:
  MockGroupDelegate() {
    EXPECT_CALL(*this, GetUUID()).WillRepeatedly(ReturnRef(uuid_));
    EXPECT_CALL(*this, GetIPInfo(kTestGroupMember))
      .WillRepeatedly(Invoke(&ReturnsFakeIpInfo));
  }

  MOCK_CONST_METHOD0(GetUUID, const std::string&());
  MOCK_METHOD1(RemoveGroup, void(const std::string&));
  MOCK_CONST_METHOD1(GetIPInfo,
                     std::vector<std::tuple<std::vector<uint8_t>, uint16_t>>(
                         const std::string&));

  // The delegate makes us return a reference, not a value.
  const std::string uuid_{kSelfUUID};
};

void HandlerNotFound(const chromeos::http::fake::ServerRequest&,
                     chromeos::http::fake::ServerResponse* response) {
  response->ReplyText(404, std::string{}, chromeos::mime::text::kPlain);
}

class FakePeerHttpHandler {
 public:
  FakePeerHttpHandler(
      std::shared_ptr<chromeos::http::fake::Transport> transport) {
    transport->AddHandler(
        kTestGroupMemberChallengeUrl, chromeos::http::request_type::kPost,
        base::Bind(&FakePeerHttpHandler::HandleChallenge,
                   weak_ptr_factory_.GetWeakPtr()));
    transport->AddHandler(
        kTestGroupMemberAnnounceUrl, chromeos::http::request_type::kPost,
        base::Bind(&FakePeerHttpHandler::HandleAnnouncement,
                   weak_ptr_factory_.GetWeakPtr()));

    // By default, let everything go.
    EXPECT_CALL(*this, HandleAnnouncement(_, _)).Times(AnyNumber());
    EXPECT_CALL(*this, HandleChallenge(_, _)).Times(AnyNumber());
  }

  MOCK_METHOD2(HandleAnnouncement,
               void(const chromeos::http::fake::ServerRequest&,
                    chromeos::http::fake::ServerResponse*));
  MOCK_METHOD2(HandleChallenge,
               void(const chromeos::http::fake::ServerRequest&,
                    chromeos::http::fake::ServerResponse*));

 private:
  base::WeakPtrFactory<FakePeerHttpHandler> weak_ptr_factory_{this};
  DISALLOW_COPY_AND_ASSIGN(FakePeerHttpHandler);
};

void AssertChallengeWellFormed(
    const chromeos::http::fake::ServerRequest& request,
    chromeos::http::fake::ServerResponse*) {
  ASSERT_EQ(request.GetURL(), kTestGroupMemberChallengeUrl);
  ASSERT_EQ(request.GetMethod(), chromeos::http::request_type::kPost);
  int score;
  std::string str_field;
  auto json_dict = request.GetDataAsJson();
  ASSERT_NE(json_dict.get(), nullptr);
  ASSERT_EQ(json_dict->size(), 3);
  ASSERT_TRUE(json_dict->GetString(http_api::kChallengeGroupKey, &str_field));
  ASSERT_TRUE(json_dict->GetString(http_api::kChallengeIdKey, &str_field));
  ASSERT_TRUE(json_dict->GetInteger(http_api::kChallengeScoreKey, &score));
}

void AssertAnnouncementWellFormed(
    const chromeos::http::fake::ServerRequest& request,
    chromeos::http::fake::ServerResponse*) {
  ASSERT_EQ(request.GetURL(), kTestGroupMemberAnnounceUrl);
  ASSERT_EQ(request.GetMethod(), chromeos::http::request_type::kPost);
  int score;
  std::string str_field;
  auto json_dict = request.GetDataAsJson();
  ASSERT_NE(json_dict.get(), nullptr);
  ASSERT_EQ(json_dict->size(), 3);
  ASSERT_TRUE(json_dict->GetString(http_api::kAnnounceGroupKey, &str_field));
  ASSERT_TRUE(json_dict->GetString(http_api::kAnnounceLeaderIdKey, &str_field));
  ASSERT_TRUE(json_dict->GetInteger(http_api::kAnnounceScoreKey, &score));
}

}  // namespace

class GroupTest : public testing::Test {
 public:
  void SetUp() override {
    // Ignore threading concerns.
    EXPECT_CALL(*bus_, AssertOnOriginThread()).Times(AnyNumber());
    EXPECT_CALL(*bus_, AssertOnDBusThread()).Times(AnyNumber());
    EXPECT_CALL(*bus_, ListenForServiceOwnerChange(kTestDBusSource, _));
    EXPECT_CALL(*bus_, GetServiceOwner(kTestDBusSource, _));

    group_.reset(new Group{kGroupName,
                           std::unique_ptr<GroupConfig>{new GroupConfig()},
                           bus_,
                           object_manager_.get(),
                           dbus::ObjectPath(kGroupPath),
                           kTestDBusSource,
                           {},
                           &group_delegate_});
    wanderer_timer_ = new MockTimer{false, false};
    heartbeat_timer_ = new MockTimer{true, true};
    group_->ReplaceTimersWithMocksForTest(
        std::unique_ptr<Timer>{wanderer_timer_},
        std::unique_ptr<Timer>{heartbeat_timer_});
    group_->ReplaceHTTPTransportForTest(transport_);
  }

  void AssertState(Group::State state, const std::string& leader_id) {
    EXPECT_EQ(state, group_->state_);
    EXPECT_EQ(leader_id, group_->leader_);
  }

  void SetRole(Group::State state, const std::string& leader_id) {
    group_->SetRole(state, leader_id);
  }

  base::MessageLoop message_loop_;
  scoped_refptr<EspeciallyMockedBus> bus_{
      new EspeciallyMockedBus{dbus::Bus::Options{}}};
  std::unique_ptr<ExportedObjectManager> object_manager_{
      new ExportedObjectManager(bus_,
                                dbus::ObjectPath(kObjectManagerPath))};
  MockGroupDelegate group_delegate_;
  std::unique_ptr<Group> group_;
  // We'll fill these in at SetUp time, but the timers are owned by the Group.
  MockTimer* wanderer_timer_{nullptr};
  MockTimer* heartbeat_timer_{nullptr};
  std::shared_ptr<chromeos::http::fake::Transport> transport_{
      new chromeos::http::fake::Transport()};
  std::unique_ptr<FakePeerHttpHandler> fake_handler_{
      new FakePeerHttpHandler{transport_}};
};

TEST_F(GroupTest, LeaveGroup) {
  chromeos::ErrorPtr error;
  EXPECT_TRUE(group_->LeaveGroup(&error));
  EXPECT_EQ(nullptr, error.get());
}

TEST_F(GroupTest, SetScore) {
  chromeos::ErrorPtr error;
  LOG(INFO) << "Set score";
  EXPECT_TRUE(group_->SetScore(&error, kScore));
  EXPECT_EQ(nullptr, error.get());
}

TEST_F(GroupTest, ShouldBecomeLeaderWithoutPeers) {
  SetRole(Group::State::WANDERER, "");
  // A couple heartbearts in wanderer state should change
  // nothing if we don't have any peers.
  heartbeat_timer_->Fire();
  AssertState(Group::State::WANDERER, "");
  heartbeat_timer_->Fire();
  AssertState(Group::State::WANDERER, "");
  // And if nothing happens for long enough, we should promote
  // ourselves to leader.
  wanderer_timer_->Fire();
  AssertState(Group::State::LEADER, kSelfUUID);
}

TEST_F(GroupTest, ShouldIgnoreAnnouncementsFromUnknownPeers) {
  group_->AddPeer(kTestGroupMember);
  // Ignore spurious announcements in wanderer.
  SetRole(Group::State::WANDERER, "");
  group_->HandleLeaderAnnouncement("a peer we've never heard of", 100);
  AssertState(Group::State::WANDERER, "");
  // Ignore spurious announcements in leader.
  SetRole(Group::State::LEADER, kSelfUUID);
  group_->HandleLeaderAnnouncement("another strange peer", 100);
  AssertState(Group::State::LEADER, kSelfUUID);
}

TEST_F(GroupTest, BecomesFollowerOnAnnouncementWhileWanderer) {
  group_->AddPeer(kTestGroupMember);
  SetRole(Group::State::WANDERER, "");
  group_->HandleLeaderAnnouncement(kTestGroupMember, 100);
  AssertState(Group::State::FOLLOWER, kTestGroupMember);
}

TEST_F(GroupTest, BecomesWandererOnAnnouncementWhileLeader) {
  group_->AddPeer(kTestGroupMember);
  SetRole(Group::State::LEADER, kSelfUUID);
  group_->HandleLeaderAnnouncement(kTestGroupMember, 100);
  AssertState(Group::State::WANDERER, "");
}

TEST_F(GroupTest, LeaderChallengeIsWellFormed) {
  transport_->AddHandler(kTestGroupMemberChallengeUrl,
                         chromeos::http::request_type::kPost,
                         base::Bind(&AssertChallengeWellFormed));
  group_->SendLeaderChallenge(kTestGroupMember);
}

TEST_F(GroupTest, LeaderAnnouncementIsWellFormed) {
  transport_->AddHandler(kTestGroupMemberAnnounceUrl,
                         chromeos::http::request_type::kPost,
                         base::Bind(&AssertAnnouncementWellFormed));
  group_->SendLeaderAnnouncement(kTestGroupMember);
}

TEST_F(GroupTest, SendsAnnouncementsPeriodicallyWhenLeader) {
  testing::Mock::VerifyAndClearExpectations(fake_handler_.get());
  group_->AddPeer(kTestGroupMember);
  // We'll announce our leadership immediately on assuming the role.
  EXPECT_CALL(*fake_handler_, HandleAnnouncement(_, _));
  SetRole(Group::State::LEADER, kSelfUUID);
  testing::Mock::VerifyAndClearExpectations(fake_handler_.get());
  // We'll also announce periodically, on the heartbeat timer.
  EXPECT_CALL(*fake_handler_, HandleAnnouncement(_, _));
  heartbeat_timer_->Fire();
}

TEST_F(GroupTest, SendsChallengesPeriodicallyWhenFollower) {
  testing::Mock::VerifyAndClearExpectations(fake_handler_.get());
  group_->AddPeer(kTestGroupMember);
  SetRole(Group::State::FOLLOWER, kTestGroupMember);
  // On every heartbeat, we should challenge the leader.
  EXPECT_CALL(*fake_handler_, HandleChallenge(_, _)).Times(2);
  heartbeat_timer_->Fire();
  heartbeat_timer_->Fire();
}

TEST_F(GroupTest, IgnoresChallengesWhileNotLeader) {
  std::string leader_id, self_id;
  group_->AddPeer(kTestGroupMember);
  // When we're following, we basically ignores scores, and just return what we
  // know about the leader.
  SetRole(Group::State::FOLLOWER, kTestGroupMember);
  group_->HandleLeaderChallenge(kPeerIdGreaterThanSelf,
                                kSelfScore + 1,
                                &leader_id, &self_id);
  ASSERT_EQ(kTestGroupMember, leader_id);
  AssertState(Group::State::FOLLOWER, kTestGroupMember);
  // In wanderer, the same applies.
  SetRole(Group::State::WANDERER, std::string{});
  group_->HandleLeaderChallenge(kPeerIdGreaterThanSelf,
                                kSelfScore + 1,
                                &leader_id, &self_id);
  ASSERT_EQ(std::string{}, leader_id);
  AssertState(Group::State::WANDERER, std::string{});
}

TEST_F(GroupTest, ShouldRetainLeadershipWhenMoreFit) {
  std::string leader_id, self_id;
  SetRole(Group::State::LEADER, kSelfUUID);
  group_->SetScore(nullptr, kSelfScore);
  // If the challenger score is less, we should ignore the peer ID.
  group_->HandleLeaderChallenge(kPeerIdGreaterThanSelf,
                                kSelfScore - 1,
                                &leader_id, &self_id);
  AssertState(Group::State::LEADER, kSelfUUID);
  ASSERT_EQ(kSelfUUID, leader_id);
  // If the challenger score is equal, we check the peer ID.
  group_->HandleLeaderChallenge(kPeerIdLessThanSelf,
                                kSelfScore,
                                &leader_id, &self_id);
  AssertState(Group::State::LEADER, kSelfUUID);
  ASSERT_EQ(kSelfUUID, leader_id);
}

TEST_F(GroupTest, ShouldAbdicateToPeerWithHigherScore) {
  std::string leader_id, self_id;
  SetRole(Group::State::LEADER, kSelfUUID);
  group_->SetScore(nullptr, kSelfScore);
  group_->HandleLeaderChallenge(kPeerIdLessThanSelf,
                                kSelfScore + 1,
                                &leader_id, &self_id);
  AssertState(Group::State::FOLLOWER, kPeerIdLessThanSelf);
  ASSERT_EQ(kPeerIdLessThanSelf, leader_id);
}

TEST_F(GroupTest, ShouldAbdicateToPeerWithHigherID) {
  std::string leader_id, self_id;
  SetRole(Group::State::LEADER, kSelfUUID);
  group_->SetScore(nullptr, kSelfScore);
  group_->HandleLeaderChallenge(kPeerIdGreaterThanSelf,
                                kSelfScore,
                                &leader_id, &self_id);
  AssertState(Group::State::FOLLOWER, kPeerIdGreaterThanSelf);
  ASSERT_EQ(kPeerIdGreaterThanSelf, leader_id);
}

TEST_F(GroupTest, MayOnlyPokeLeaderWhenFollowing) {
  group_->AddPeer(kTestGroupMember);
  chromeos::ErrorPtr error;
  // We only allow ourselves to poke the leader if we're actually following
  // a leader.
  SetRole(Group::State::LEADER, kSelfUUID);
  EXPECT_FALSE(group_->PokeLeader(&error));
  EXPECT_NE(nullptr, error.get());
  error.reset();
  SetRole(Group::State::WANDERER, std::string{});
  EXPECT_FALSE(group_->PokeLeader(&error));
  EXPECT_NE(nullptr, error.get());
  error.reset();
  // Check that we do actually challenge the leader when poked.
  testing::Mock::VerifyAndClearExpectations(fake_handler_.get());
  EXPECT_CALL(*fake_handler_, HandleChallenge(_, _));
  SetRole(Group::State::FOLLOWER, kTestGroupMember);
  EXPECT_TRUE(group_->PokeLeader(&error));
  EXPECT_EQ(nullptr, error.get());
}

TEST_F(GroupTest, BecomesWandererOnLeaderFailure) {
  testing::Mock::VerifyAndClearExpectations(fake_handler_.get());
  group_->AddPeer(kTestGroupMember);
  SetRole(Group::State::FOLLOWER, kTestGroupMember);
  // On every heartbeat, we should challenge the leader.
  EXPECT_CALL(*fake_handler_, HandleChallenge(_, _)).Times(2)
      .WillRepeatedly(Invoke(&HandlerNotFound));
  heartbeat_timer_->Fire();
  heartbeat_timer_->Fire();
  AssertState(Group::State::WANDERER, std::string{});
}

TEST_F(GroupTest, ShouldChallengeOnScoreIncreaseWhenFollower) {
  testing::Mock::VerifyAndClearExpectations(fake_handler_.get());
  group_->AddPeer(kTestGroupMember);
  group_->SetScore(nullptr, 0);
  SetRole(Group::State::FOLLOWER, kTestGroupMember);
  EXPECT_CALL(*fake_handler_, HandleChallenge(_, _));
  group_->SetScore(nullptr, 10);
  testing::Mock::VerifyAndClearExpectations(fake_handler_.get());
  // If we're not a follower, we should not attempt to challenge a leader.
  SetRole(Group::State::WANDERER, std::string{});
  group_->SetScore(nullptr, 20);
}

}  // namespace leaderd
