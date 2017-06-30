// Copyright 2017 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <semaphore.h>

#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>

#include <unordered_map>
#include <unordered_set>

#include <base/at_exit.h>
#include <base/bind.h>
#include <base/command_line.h>
#include <base/logging.h>
#include <gtest/gtest.h>

#include "arc/camera_algorithm_bridge.h"
#include "arc/common.h"
#include "common/libcab_test_internal.h"

namespace libcab_test {

// This class helps to forward the callback to test cases because
// CameraAlgorithmBridge accepts initialization and callback registration once
// and only once.
class CallbackSwitcher : public camera_algorithm_callback_ops_t {
 public:
  static CallbackSwitcher* GetInstance() {
    static CallbackSwitcher switcher;
    return &switcher;
  }

  void RegisterCallback(base::Callback<int32_t(uint32_t, int32_t)> callback) {
    callback_ = callback;
  }

 private:
  CallbackSwitcher() {
    CallbackSwitcher::return_callback =
        CallbackSwitcher::ReturnCallbackForwarder;
  }

  static int32_t ReturnCallbackForwarder(
      const camera_algorithm_callback_ops_t* callback_ops,
      uint32_t status,
      int32_t buffer_handle) {
    if (!callback_ops) {
      return -EINVAL;
    }
    auto s = const_cast<CallbackSwitcher*>(
        static_cast<const CallbackSwitcher*>(callback_ops));
    return s->callback_.Run(status, buffer_handle);
  }

  base::Callback<int32_t(uint32_t, int32_t)> callback_;
};

// This test should be run against the fake libcam_algo.so created with
// fake_libcam_algo.cc.
class CameraAlgorithmBridgeFixture : public testing::Test {
 public:
  const size_t kShmBufferSize = 2048;

  CameraAlgorithmBridgeFixture()
      : bridge_(arc::CameraAlgorithmBridge::GetInstance()) {
    CallbackSwitcher::GetInstance()->RegisterCallback(base::Bind(
        &CameraAlgorithmBridgeFixture::ReturnCallback, base::Unretained(this)));
    sem_init(&return_sem_, 0, 0);
  }

  ~CameraAlgorithmBridgeFixture() { sem_destroy(&return_sem_); }

 protected:
  virtual int32_t ReturnCallback(uint32_t status, int32_t buffer_handle) {
    base::AutoLock l(request_set_lock_);
    if (request_set_.find(buffer_handle) == request_set_.end()) {
      ADD_FAILURE() << "Invalid handle received from the return callback";
      return -EINVAL;
    }
    request_set_.erase(buffer_handle);
    sem_post(&return_sem_);
    return 0;
  }

  arc::CameraAlgorithmBridge* bridge_;

  base::Lock request_set_lock_;

  std::unordered_set<int32_t> request_set_;

  sem_t return_sem_;

 private:
  DISALLOW_COPY_AND_ASSIGN(CameraAlgorithmBridgeFixture);
};

TEST_F(CameraAlgorithmBridgeFixture, BasicOperation) {
  int fd = shm_open("/myshm", O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
  ASSERT_LE(0, fd) << "Failed to create shared memory";
  ASSERT_LE(0, fcntl(fd, F_GETFD));
  ASSERT_EQ(0, ftruncate(fd, kShmBufferSize));
  int32_t handle = bridge_->RegisterBuffer(fd);
  ASSERT_LE(0, handle) << "Handle should be of positive value";
  ASSERT_LE(0, fcntl(fd, F_GETFD));
  std::vector<uint8_t> req_header(1, REQUEST_TEST_COMMAND_NORMAL);
  {
    base::AutoLock l(request_set_lock_);
    request_set_.insert(handle);
  }
  EXPECT_EQ(0, bridge_->Request(req_header, handle));
  struct timespec timeout = {};
  clock_gettime(CLOCK_REALTIME, &timeout);
  timeout.tv_sec += 1;
  ASSERT_EQ(0, sem_timedwait(&return_sem_, &timeout));
  std::vector<int32_t> handles({handle});
  bridge_->DeregisterBuffers(handles);
  close(fd);
  shm_unlink("/myshm");
}

TEST_F(CameraAlgorithmBridgeFixture, InvalidFdOrHandle) {
  int32_t handle = bridge_->RegisterBuffer(-1);
  ASSERT_GT(0, handle) << "Registering invalid fd should have failed";

  int fd = shm_open("/myshm", O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
  ASSERT_LE(0, fd) << "Failed to create shared memory";
  ASSERT_EQ(0, ftruncate(fd, kShmBufferSize));
  handle = bridge_->RegisterBuffer(fd);
  ASSERT_LE(0, handle) << "Handle should be of positive value";
  std::vector<uint8_t> req_header(1, REQUEST_TEST_COMMAND_NORMAL);
  EXPECT_NE(0, bridge_->Request(req_header, handle - 1));
  EXPECT_NE(0, bridge_->Request(req_header, handle + 1));
  std::vector<int32_t> handles({handle});
  bridge_->DeregisterBuffers(handles);
  close(fd);
  shm_unlink("/myshm");

  ASSERT_GT(0, bridge_->RegisterBuffer(fd))
      << "Registering invalid fd should have failed";
}

TEST_F(CameraAlgorithmBridgeFixture, MultiRequests) {
  const uint32_t kNumberOfFds = 256;
  auto GetShmName = [](uint32_t num) {
    std::stringstream ss;
    ss << num;
    return std::string("/myshm") + ss.str();
  };

  std::vector<int> fds, handles;
  for (uint32_t i = 1; i <= kNumberOfFds; i++) {
    fds.push_back(
        shm_open(GetShmName(i).c_str(), O_CREAT | O_RDWR, S_IRUSR | S_IWUSR));
    ASSERT_LE(0, fds.back()) << "Failed to create shared memory";
    ASSERT_EQ(0, ftruncate(fds.back(), kShmBufferSize));
    handles.push_back(bridge_->RegisterBuffer(fds.back()));
    ASSERT_LE(0, handles.back()) << "Handle should be of positive value";
  }
  for (const auto handle : handles) {
    std::vector<uint8_t> req_header(1, REQUEST_TEST_COMMAND_NORMAL);
    {
      base::AutoLock l(request_set_lock_);
      request_set_.insert(handle);
    }
    EXPECT_EQ(0, bridge_->Request(req_header, handle));
  }
  struct timespec timeout = {};
  clock_gettime(CLOCK_REALTIME, &timeout);
  timeout.tv_sec += 1;
  for (size_t i = 0; i < handles.size(); ++i) {
    ASSERT_EQ(0, sem_timedwait(&return_sem_, &timeout));
  }
  bridge_->DeregisterBuffers(handles);
  for (const auto fd : fds) {
    close(fd);
  }
  for (uint32_t i = 1; i <= kNumberOfFds; i++) {
    shm_unlink(GetShmName(i).c_str());
  }
}

static int GenerateRandomHeader(uint32_t max_header_len,
                                std::vector<uint8_t>* header) {
  if (max_header_len == 0 || !header) {
    return -EINVAL;
  }
  static unsigned int seed = time(NULL) + getpid();
  header->resize((rand_r(&seed) % max_header_len) + 1);
  for (auto& it : *header) {
    it = rand_r(&seed);
  }
  return 0;
}

TEST_F(CameraAlgorithmBridgeFixture, VerifyRequestHeader) {
  const uint32_t kNumberOfTests = 256;
  const uint32_t kMaxReqHeaderSize = 64;
  for (uint32_t i = 0; i < kNumberOfTests; ++i) {
    std::vector<uint8_t> req_header;
    GenerateRandomHeader(kMaxReqHeaderSize, &req_header);
    req_header[0] = REQUEST_TEST_COMMAND_VERIFY_REQ_HEADER;
    ASSERT_EQ(SimpleHash(req_header.data(), req_header.size()),
              bridge_->Request(req_header, -1));
  }
}

// This test should be run against the fake libcam_algo.so created with
// fake_libcam_algo.cc.
class CameraAlgorithmBridgeStatusFixture : public CameraAlgorithmBridgeFixture {
 public:
  CameraAlgorithmBridgeStatusFixture() {}

 protected:
  int32_t ReturnCallback(uint32_t status, int32_t buffer_handle) override {
    base::AutoLock l(hash_codes_lock_);
    if (buffer_handle < 0 ||
        static_cast<size_t>(buffer_handle) >= hash_codes_.size() ||
        hash_codes_[buffer_handle] != status) {
      ADD_FAILURE() << "Invalid status received from the return callback";
      return -EINVAL;
    }
    sem_post(&return_sem_);
    return 0;
  }

  base::Lock hash_codes_lock_;

  // Stores hashcode generated from |req_header| of the Request calls.
  std::vector<uint32_t> hash_codes_;

  sem_t return_sem_;

 private:
  DISALLOW_COPY_AND_ASSIGN(CameraAlgorithmBridgeStatusFixture);
};

TEST_F(CameraAlgorithmBridgeStatusFixture, VerifyReturnStatus) {
  const uint32_t kNumberOfTests = 256;
  const uint32_t kMaxReqHeaderSize = 64;
  for (uint32_t i = 0; i <= kNumberOfTests; i++) {
    std::vector<uint8_t> req_header;
    GenerateRandomHeader(kMaxReqHeaderSize, &req_header);
    req_header[0] = REQUEST_TEST_COMMAND_VERIFY_STATUS;
    {
      base::AutoLock l(hash_codes_lock_);
      hash_codes_.push_back(SimpleHash(req_header.data(), req_header.size()));
    }
    EXPECT_EQ(0, bridge_->Request(req_header, i));
  }
  struct timespec timeout = {};
  clock_gettime(CLOCK_REALTIME, &timeout);
  timeout.tv_sec += 1;
  for (uint32_t i = 0; i <= kNumberOfTests; i++) {
    ASSERT_EQ(0, sem_timedwait(&return_sem_, &timeout));
  }
}

}  // namespace libcab_test

int main(int argc, char** argv) {
  static base::AtExitManager exit_manager;

  // Set up logging so we can enable VLOGs with -v / --vmodule.
  base::CommandLine::Init(argc, argv);
  logging::LoggingSettings settings;
  settings.logging_dest = logging::LOG_TO_SYSTEM_DEBUG_LOG;
  LOG_ASSERT(logging::InitLogging(settings));

  auto bridge = arc::CameraAlgorithmBridge::GetInstance();
  if (bridge->Initialize(libcab_test::CallbackSwitcher::GetInstance()) != 0) {
    ADD_FAILURE() << "Failed to initialize camera algorithm bridge";
    return EXIT_FAILURE;
  }

  // Initialize and run all tests
  ::testing::InitGoogleTest(&argc, argv);
  int result = RUN_ALL_TESTS();

  return result;
}
