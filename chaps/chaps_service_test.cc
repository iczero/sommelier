// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chaps_service.h"

#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "session_mock.h"
#include "slot_manager_mock.h"

using std::string;
using std::vector;
using ::testing::_;
using ::testing::Return;
using ::testing::SetArgumentPointee;

namespace chaps {

// Invalid initialization test.
TEST(InitDeathTest, InvalidInit) {
  ChapsServiceImpl service(NULL);
  EXPECT_DEATH_IF_SUPPORTED(service.Init(), "Check failed");
}

// Test fixture for an initialized service instance.
class TestService : public ::testing::Test {
protected:
  virtual void SetUp() {
    service_.reset(new ChapsServiceImpl(&slot_manager_));
    ASSERT_TRUE(service_->Init());
    // Setup parsable and un-parsable serialized attributes.
    CK_ATTRIBUTE attributes[] = {{CKA_VALUE, NULL, 0}};
    Attributes tmp(attributes, 1);
    tmp.Serialize(&good_attributes_);
    bad_attributes_ = vector<uint8_t>(100, 0xAA);
  }
  virtual void TearDown() {
    service_->TearDown();
  }
  SlotManagerMock slot_manager_;
  SessionMock session_;
  scoped_ptr<ChapsServiceImpl> service_;
  vector<uint8_t> bad_attributes_;
  vector<uint8_t> good_attributes_;
};

TEST_F(TestService, GetSlotList) {
  EXPECT_CALL(slot_manager_, GetSlotCount())
    .WillRepeatedly(Return(2));
  EXPECT_CALL(slot_manager_, IsTokenPresent(_))
    .WillRepeatedly(Return(false));
  // Try bad arguments.
  EXPECT_EQ(CKR_ARGUMENTS_BAD, service_->GetSlotList(false, NULL));
  vector<uint32_t> slot_list;
  slot_list.push_back(0);
  EXPECT_EQ(CKR_ARGUMENTS_BAD, service_->GetSlotList(false, &slot_list));
  // Try normal use cases.
  slot_list.clear();
  EXPECT_EQ(CKR_OK, service_->GetSlotList(true, &slot_list));
  EXPECT_EQ(0, slot_list.size());
  EXPECT_EQ(CKR_OK, service_->GetSlotList(false, &slot_list));
  EXPECT_EQ(2, slot_list.size());
}

TEST_F(TestService, GetSlotInfo) {
  CK_SLOT_INFO test_info;
  memset(&test_info, 0, sizeof(CK_SLOT_INFO));
  test_info.flags = 17;
  EXPECT_CALL(slot_manager_, GetSlotCount())
    .WillRepeatedly(Return(2));
  EXPECT_CALL(slot_manager_, GetSlotInfo(0, _))
    .WillRepeatedly(SetArgumentPointee<1>(test_info));

  // Try bad arguments.
  void* p[7];
  for (int i = 0; i < 7; ++i) {
    memset(p, 1, sizeof(p));
    p[i] = NULL;
    EXPECT_EQ(CKR_ARGUMENTS_BAD, service_->GetSlotInfo(
        0,
        reinterpret_cast<vector<uint8_t>*>(p[0]),
        reinterpret_cast<vector<uint8_t>*>(p[1]),
        reinterpret_cast<uint32_t*>(p[2]),
        reinterpret_cast<uint8_t*>(p[3]),
        reinterpret_cast<uint8_t*>(p[4]),
        reinterpret_cast<uint8_t*>(p[5]),
        reinterpret_cast<uint8_t*>(p[6])));
  }
  vector<uint8_t> slot_description;
  vector<uint8_t> manufacturer_id;
  uint32_t flags;
  uint8_t hardware_version_major;
  uint8_t hardware_version_minor;
  uint8_t firmware_version_major;
  uint8_t firmware_version_minor;
  // Try invalid slot ID.
  EXPECT_EQ(CKR_SLOT_ID_INVALID,
            service_->GetSlotInfo(2,
                                  &slot_description,
                                  &manufacturer_id,
                                  &flags,
                                  &hardware_version_major,
                                  &hardware_version_minor,
                                  &firmware_version_major,
                                  &firmware_version_minor));
  // Try the normal case.
  EXPECT_EQ(CKR_OK, service_->GetSlotInfo(0,
                                          &slot_description,
                                          &manufacturer_id,
                                          &flags,
                                          &hardware_version_major,
                                          &hardware_version_minor,
                                          &firmware_version_major,
                                          &firmware_version_minor));
  EXPECT_EQ(flags, 17);
}

TEST_F(TestService, GetTokenInfo) {
  CK_TOKEN_INFO test_info;
  memset(&test_info, 0, sizeof(CK_TOKEN_INFO));
  test_info.flags = 17;
  EXPECT_CALL(slot_manager_, GetSlotCount())
    .WillRepeatedly(Return(2));
  EXPECT_CALL(slot_manager_, GetTokenInfo(0, _))
    .WillRepeatedly(SetArgumentPointee<1>(test_info));

  // Try bad arguments.
  void* p[19];
  for (int i = 0; i < 19; ++i) {
    memset(p, 1, sizeof(p));
    p[i] = NULL;
    EXPECT_EQ(CKR_ARGUMENTS_BAD, service_->GetTokenInfo(
        0,
        reinterpret_cast<vector<uint8_t>*>(p[0]),
        reinterpret_cast<vector<uint8_t>*>(p[1]),
        reinterpret_cast<vector<uint8_t>*>(p[2]),
        reinterpret_cast<vector<uint8_t>*>(p[3]),
        reinterpret_cast<uint32_t*>(p[4]),
        reinterpret_cast<uint32_t*>(p[5]),
        reinterpret_cast<uint32_t*>(p[6]),
        reinterpret_cast<uint32_t*>(p[7]),
        reinterpret_cast<uint32_t*>(p[8]),
        reinterpret_cast<uint32_t*>(p[9]),
        reinterpret_cast<uint32_t*>(p[10]),
        reinterpret_cast<uint32_t*>(p[11]),
        reinterpret_cast<uint32_t*>(p[12]),
        reinterpret_cast<uint32_t*>(p[13]),
        reinterpret_cast<uint32_t*>(p[14]),
        reinterpret_cast<uint8_t*>(p[15]),
        reinterpret_cast<uint8_t*>(p[16]),
        reinterpret_cast<uint8_t*>(p[17]),
        reinterpret_cast<uint8_t*>(p[18])));
  }
  vector<uint8_t> label;
  vector<uint8_t> manufacturer_id;
  vector<uint8_t> model;
  vector<uint8_t> serial_number;
  uint32_t flags;
  uint32_t max_session_count;
  uint32_t session_count;
  uint32_t max_session_count_rw;
  uint32_t session_count_rw;
  uint32_t max_pin_len;
  uint32_t min_pin_len;
  uint32_t total_public_memory;
  uint32_t free_public_memory;
  uint32_t total_private_memory;
  uint32_t free_private_memory;
  uint8_t hardware_version_major;
  uint8_t hardware_version_minor;
  uint8_t firmware_version_major;
  uint8_t firmware_version_minor;
  // Try invalid slot ID.
  EXPECT_EQ(CKR_SLOT_ID_INVALID,
            service_->GetTokenInfo(3,
                                   &label,
                                   &manufacturer_id,
                                   &model,
                                   &serial_number,
                                   &flags,
                                   &max_session_count,
                                   &session_count,
                                   &max_session_count_rw,
                                   &session_count_rw,
                                   &max_pin_len,
                                   &min_pin_len,
                                   &total_public_memory,
                                   &free_public_memory,
                                   &total_private_memory,
                                   &free_private_memory,
                                   &hardware_version_major,
                                   &hardware_version_minor,
                                   &firmware_version_major,
                                   &firmware_version_minor));
  // Try the normal case.
  EXPECT_EQ(CKR_OK, service_->GetTokenInfo(0,
                                           &label,
                                           &manufacturer_id,
                                           &model,
                                           &serial_number,
                                           &flags,
                                           &max_session_count,
                                           &session_count,
                                           &max_session_count_rw,
                                           &session_count_rw,
                                           &max_pin_len,
                                           &min_pin_len,
                                           &total_public_memory,
                                           &free_public_memory,
                                           &total_private_memory,
                                           &free_private_memory,
                                           &hardware_version_major,
                                           &hardware_version_minor,
                                           &firmware_version_major,
                                           &firmware_version_minor));
  EXPECT_EQ(flags, 17);
}

TEST_F(TestService, GetMechanismList) {
  MechanismMap test_list;
  CK_MECHANISM_INFO test_info;
  memset(&test_info, 0, sizeof(CK_MECHANISM_INFO));
  test_list[123UL] = test_info;
  EXPECT_CALL(slot_manager_, GetSlotCount())
    .WillRepeatedly(Return(2));
  EXPECT_CALL(slot_manager_, GetMechanismInfo(0))
    .WillRepeatedly(Return(&test_list));
  // Try bad arguments.
  EXPECT_EQ(CKR_ARGUMENTS_BAD, service_->GetMechanismList(0, NULL));
  // Try invalid slot ID.
  vector<uint32_t> output;
  EXPECT_EQ(CKR_SLOT_ID_INVALID, service_->GetMechanismList(2, &output));
  // Try the normal case.
  ASSERT_EQ(CKR_OK, service_->GetMechanismList(0, &output));
  ASSERT_EQ(output.size(), 1);
  EXPECT_EQ(output[0], 123);
}

TEST_F(TestService, GetMechanismInfo) {
  MechanismMap test_list;
  CK_MECHANISM_INFO test_info;
  memset(&test_info, 0, sizeof(CK_MECHANISM_INFO));
  test_info.flags = 17;
  test_list[123UL] = test_info;
  EXPECT_CALL(slot_manager_, GetSlotCount())
    .WillRepeatedly(Return(2));
  EXPECT_CALL(slot_manager_, GetMechanismInfo(0))
    .WillRepeatedly(Return(&test_list));
  uint32_t min_key, max_key, flags;
  // Try bad arguments.
  EXPECT_EQ(CKR_ARGUMENTS_BAD,
            service_->GetMechanismInfo(0, 123, NULL, &max_key, &flags));
  EXPECT_EQ(CKR_ARGUMENTS_BAD,
            service_->GetMechanismInfo(0, 123, &min_key, NULL, &flags));
  EXPECT_EQ(CKR_ARGUMENTS_BAD,
            service_->GetMechanismInfo(0, 123, &min_key, &max_key, NULL));
  // Try invalid slot ID.
  EXPECT_EQ(CKR_SLOT_ID_INVALID,
            service_->GetMechanismInfo(2, 123, &min_key, &max_key, &flags));
  // Try the normal case.
  ASSERT_EQ(CKR_OK,
            service_->GetMechanismInfo(0, 123, &min_key, &max_key, &flags));
  EXPECT_EQ(flags, 17);
}

TEST_F(TestService, InitToken) {
  EXPECT_CALL(slot_manager_, GetSlotCount())
    .WillRepeatedly(Return(2));
  EXPECT_CALL(slot_manager_, IsTokenPresent(_))
    .WillOnce(Return(false))
    .WillOnce(Return(true));
  vector<uint8_t> bad_label;
  vector<uint8_t> good_label(chaps::kTokenLabelSize, 0x20);
  EXPECT_EQ(CKR_ARGUMENTS_BAD, service_->InitToken(0, NULL, bad_label));
  EXPECT_EQ(CKR_SLOT_ID_INVALID, service_->InitToken(2, NULL, good_label));
  EXPECT_EQ(CKR_TOKEN_NOT_PRESENT, service_->InitToken(0, NULL, good_label));
  EXPECT_EQ(CKR_PIN_INCORRECT, service_->InitToken(0, NULL, good_label));
}

TEST_F(TestService, InitPIN) {
  EXPECT_CALL(slot_manager_, GetSession(0, _))
    .WillOnce(Return(false))
    .WillOnce(Return(true));
  EXPECT_EQ(CKR_SESSION_HANDLE_INVALID, service_->InitPIN(0, NULL));
  EXPECT_EQ(CKR_USER_NOT_LOGGED_IN, service_->InitPIN(0, NULL));
}

TEST_F(TestService, SetPIN) {
  EXPECT_CALL(slot_manager_, GetSession(0, _))
    .WillOnce(Return(false))
    .WillOnce(Return(true));
  EXPECT_EQ(CKR_SESSION_HANDLE_INVALID, service_->SetPIN(0, NULL, NULL));
  EXPECT_EQ(CKR_PIN_INVALID, service_->SetPIN(0, NULL, NULL));
}

TEST_F(TestService, OpenSession) {
  EXPECT_CALL(slot_manager_, GetSlotCount())
    .WillRepeatedly(Return(2));
  EXPECT_CALL(slot_manager_, IsTokenPresent(_))
    .WillOnce(Return(false))
    .WillRepeatedly(Return(true));
  EXPECT_CALL(slot_manager_, OpenSession(0, true))
    .WillRepeatedly(Return(10));
  EXPECT_EQ(CKR_ARGUMENTS_BAD, service_->OpenSession(0, 0, NULL));
  uint32_t session;
  EXPECT_EQ(CKR_SLOT_ID_INVALID, service_->OpenSession(2, 0, &session));
  EXPECT_EQ(CKR_TOKEN_NOT_PRESENT, service_->OpenSession(0, 0, &session));
  EXPECT_EQ(CKR_SESSION_PARALLEL_NOT_SUPPORTED,
            service_->OpenSession(0, 0, &session));
  ASSERT_EQ(CKR_OK, service_->OpenSession(0, CKF_SERIAL_SESSION, &session));
  EXPECT_EQ(session, 10);
}

TEST_F(TestService, CloseSession) {
  EXPECT_CALL(slot_manager_, CloseSession(0))
    .WillOnce(Return(false))
    .WillOnce(Return(true));
  EXPECT_EQ(CKR_SESSION_HANDLE_INVALID, service_->CloseSession(0));
  EXPECT_EQ(CKR_OK, service_->CloseSession(0));
}

TEST_F(TestService, CloseAllSessions) {
  EXPECT_CALL(slot_manager_, GetSlotCount())
    .WillRepeatedly(Return(2));
  EXPECT_CALL(slot_manager_, CloseAllSessions(1));
  EXPECT_EQ(CKR_SLOT_ID_INVALID, service_->CloseAllSessions(2));
  EXPECT_EQ(CKR_OK, service_->CloseAllSessions(1));
}

TEST_F(TestService, GetSessionInfo) {
  EXPECT_CALL(slot_manager_, GetSession(1, _))
    .WillOnce(Return(false))
    .WillRepeatedly(DoAll(SetArgumentPointee<1>(&session_), Return(true)));
  EXPECT_CALL(session_, GetSlot())
    .WillRepeatedly(Return(15));
  EXPECT_CALL(session_, GetState())
    .WillRepeatedly(Return(16));
  EXPECT_CALL(session_, IsReadOnly())
    .WillRepeatedly(Return(false));
  // Try bad arguments.
  uint32_t slot, state, flags, err;
  EXPECT_EQ(CKR_ARGUMENTS_BAD,
            service_->GetSessionInfo(1, NULL, &state, &flags, &err));
  EXPECT_EQ(CKR_ARGUMENTS_BAD,
            service_->GetSessionInfo(1, &slot, NULL, &flags, &err));
  EXPECT_EQ(CKR_ARGUMENTS_BAD,
            service_->GetSessionInfo(1, &slot, &state, NULL, &err));
  EXPECT_EQ(CKR_ARGUMENTS_BAD,
            service_->GetSessionInfo(1, &slot, &state, &flags, NULL));
  EXPECT_EQ(CKR_SESSION_HANDLE_INVALID,
            service_->GetSessionInfo(1, &slot, &state, &flags, &err));
  // Try normal case.
  ASSERT_EQ(CKR_OK, service_->GetSessionInfo(1, &slot, &state, &flags, &err));
  EXPECT_EQ(slot, 15);
  EXPECT_EQ(state, 16);
  EXPECT_EQ(flags, CKF_RW_SESSION|CKF_SERIAL_SESSION);
}

TEST_F(TestService, GetOperationState) {
  EXPECT_CALL(slot_manager_, GetSession(1, _))
    .WillOnce(Return(false))
    .WillRepeatedly(DoAll(SetArgumentPointee<1>(&session_), Return(true)));
  EXPECT_CALL(session_, IsOperationActive(_))
    .WillRepeatedly(Return(false));
  EXPECT_EQ(CKR_ARGUMENTS_BAD, service_->GetOperationState(1, NULL));
  vector<uint8_t> state;
  EXPECT_EQ(CKR_SESSION_HANDLE_INVALID, service_->GetOperationState(1, &state));
  EXPECT_EQ(CKR_OPERATION_NOT_INITIALIZED,
            service_->GetOperationState(1, &state));
  EXPECT_CALL(session_, IsOperationActive(_))
    .WillRepeatedly(Return(true));
  EXPECT_EQ(CKR_STATE_UNSAVEABLE,
            service_->GetOperationState(1, &state));
}

TEST_F(TestService, SetOperationState) {
  EXPECT_CALL(slot_manager_, GetSession(1, _))
    .WillOnce(Return(false))
    .WillRepeatedly(DoAll(SetArgumentPointee<1>(&session_), Return(true)));
  vector<uint8_t> state;
  EXPECT_EQ(CKR_SESSION_HANDLE_INVALID,
            service_->SetOperationState(1, state, 0, 0));
  EXPECT_EQ(CKR_SAVED_STATE_INVALID,
            service_->SetOperationState(1, state, 0, 0));
}

TEST_F(TestService, Login) {
  EXPECT_CALL(slot_manager_, GetSession(1, _))
    .WillOnce(Return(false))
    .WillRepeatedly(DoAll(SetArgumentPointee<1>(&session_), Return(true)));
  EXPECT_EQ(CKR_SESSION_HANDLE_INVALID, service_->Login(1, CKU_USER, NULL));
  string bad_pin("1234");
  string good_pin("111111");
  EXPECT_EQ(CKR_PIN_INCORRECT, service_->Login(1, CKU_SO, &good_pin));
  EXPECT_EQ(CKR_PIN_INCORRECT, service_->Login(1, CKU_USER, &bad_pin));
  EXPECT_EQ(CKR_OK, service_->Login(1, CKU_USER, &good_pin));
  EXPECT_EQ(CKR_OK, service_->Login(1, CKU_USER, NULL));
}

TEST_F(TestService, Logout) {
  EXPECT_CALL(slot_manager_, GetSession(1, _))
    .WillOnce(Return(false))
    .WillRepeatedly(DoAll(SetArgumentPointee<1>(&session_), Return(true)));
  EXPECT_EQ(CKR_SESSION_HANDLE_INVALID, service_->Logout(1));
  EXPECT_EQ(CKR_OK, service_->Logout(1));
}

TEST_F(TestService, CreateObject) {
  EXPECT_CALL(slot_manager_, GetSession(1, _))
    .WillOnce(Return(false))
    .WillRepeatedly(DoAll(SetArgumentPointee<1>(&session_), Return(true)));
  EXPECT_CALL(session_, CreateObject(_, _))
    .WillOnce(Return(CKR_FUNCTION_FAILED))
    .WillRepeatedly(DoAll(SetArgumentPointee<1>(2), Return(CKR_OK)));
  EXPECT_EQ(CKR_ARGUMENTS_BAD,
            service_->CreateObject(1, good_attributes_, NULL));
  uint32_t object_handle;
  EXPECT_EQ(CKR_SESSION_HANDLE_INVALID,
            service_->CreateObject(1, good_attributes_, &object_handle));
  EXPECT_EQ(CKR_TEMPLATE_INCONSISTENT,
            service_->CreateObject(1, bad_attributes_, &object_handle));
  EXPECT_EQ(CKR_FUNCTION_FAILED,
            service_->CreateObject(1, good_attributes_, &object_handle));
  EXPECT_EQ(CKR_OK,
            service_->CreateObject(1, good_attributes_, &object_handle));
  EXPECT_EQ(object_handle, 2);
}

TEST_F(TestService, CopyObject) {
  EXPECT_CALL(slot_manager_, GetSession(1, _))
    .WillOnce(Return(false))
    .WillRepeatedly(DoAll(SetArgumentPointee<1>(&session_), Return(true)));
  EXPECT_CALL(session_, CopyObject(_, 2, _))
    .WillOnce(Return(CKR_FUNCTION_FAILED))
    .WillRepeatedly(DoAll(SetArgumentPointee<2>(3), Return(CKR_OK)));
  EXPECT_EQ(CKR_ARGUMENTS_BAD,
            service_->CopyObject(1, 2, good_attributes_, NULL));
  uint32_t object_handle;
  EXPECT_EQ(CKR_SESSION_HANDLE_INVALID,
            service_->CopyObject(1, 2, good_attributes_, &object_handle));
  EXPECT_EQ(CKR_TEMPLATE_INCONSISTENT,
            service_->CopyObject(1, 2, bad_attributes_, &object_handle));
  EXPECT_EQ(CKR_FUNCTION_FAILED,
            service_->CopyObject(1, 2, good_attributes_, &object_handle));
  EXPECT_EQ(CKR_OK,
            service_->CopyObject(1, 2, good_attributes_, &object_handle));
  EXPECT_EQ(object_handle, 3);
}

TEST_F(TestService, DestroyObject) {
  EXPECT_CALL(slot_manager_, GetSession(1, _))
    .WillOnce(Return(false))
    .WillRepeatedly(DoAll(SetArgumentPointee<1>(&session_), Return(true)));
  EXPECT_CALL(session_, DestroyObject(_))
    .WillOnce(Return(CKR_FUNCTION_FAILED))
    .WillRepeatedly(Return(CKR_OK));
  EXPECT_EQ(CKR_SESSION_HANDLE_INVALID, service_->DestroyObject(1, 2));
  EXPECT_EQ(CKR_FUNCTION_FAILED, service_->DestroyObject(1, 2));
  EXPECT_EQ(CKR_OK, service_->DestroyObject(1, 2));
}

}  // namespace chaps

int main(int argc, char** argv) {
  ::testing::InitGoogleMock(&argc, argv);
  return RUN_ALL_TESTS();
}
