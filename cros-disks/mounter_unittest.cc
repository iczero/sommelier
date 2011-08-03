// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "cros-disks/mounter.h"
#include "cros-disks/mount-options.h"

using std::string;
using std::vector;
using testing::Return;

namespace cros_disks {

// A mock mounter class for testing the mounter base class.
class MounterUnderTest : public Mounter {
 public:
  MounterUnderTest(const string& source_path,
                   const string& target_path,
                   const string& filesystem_type,
                   const MountOptions& mount_options)
    : Mounter(source_path, target_path, filesystem_type, mount_options) {
  }

  // Mocks mount implementation.
  MOCK_METHOD0(MountImpl, MountErrorType());
};

class MounterTest : public ::testing::Test {
 protected:
  virtual void SetUp() {
    source_path_ = "/dev/sdb1";
    target_path_ = "/media/disk";
    filesystem_type_ = "vfat";
  }

  string filesystem_type_;
  string source_path_;
  string target_path_;
  MountOptions mount_options_;
  vector<string> options_;
};

TEST_F(MounterTest, MountReadOnlySucceeded) {
  mount_options_.Initialize(options_, false, "", "");
  MounterUnderTest mounter(source_path_, target_path_,
                           filesystem_type_, mount_options_);

  EXPECT_CALL(mounter, MountImpl()).WillOnce(Return(kMountErrorNone));
  EXPECT_TRUE(mounter.mount_options().IsReadOnlyOptionSet());
  EXPECT_EQ(kMountErrorNone, mounter.Mount());
}

TEST_F(MounterTest, MountReadOnlyFailed) {
  mount_options_.Initialize(options_, false, "", "");
  MounterUnderTest mounter(source_path_, target_path_,
                           filesystem_type_, mount_options_);

  EXPECT_CALL(mounter, MountImpl()).WillOnce(Return(kMountErrorInternal));
  EXPECT_TRUE(mounter.mount_options().IsReadOnlyOptionSet());
  EXPECT_EQ(kMountErrorInternal, mounter.Mount());
}

TEST_F(MounterTest, MountReadWriteSucceeded) {
  options_.push_back("rw");
  mount_options_.Initialize(options_, false, "", "");
  MounterUnderTest mounter(source_path_, target_path_,
                           filesystem_type_, mount_options_);

  EXPECT_CALL(mounter, MountImpl()).WillOnce(Return(kMountErrorNone));
  EXPECT_FALSE(mounter.mount_options().IsReadOnlyOptionSet());
  EXPECT_EQ(kMountErrorNone, mounter.Mount());
}

TEST_F(MounterTest, MountReadWriteFailedButReadOnlySucceeded) {
  options_.push_back("rw");
  mount_options_.Initialize(options_, false, "", "");
  MounterUnderTest mounter(source_path_, target_path_,
                           filesystem_type_, mount_options_);

  EXPECT_CALL(mounter, MountImpl())
    .WillOnce(Return(kMountErrorInternal))
    .WillOnce(Return(kMountErrorNone));
  EXPECT_FALSE(mounter.mount_options().IsReadOnlyOptionSet());
  EXPECT_EQ(kMountErrorNone, mounter.Mount());
  EXPECT_TRUE(mounter.mount_options().IsReadOnlyOptionSet());
}

TEST_F(MounterTest, MountReadWriteAndReadOnlyFailed) {
  options_.push_back("rw");
  mount_options_.Initialize(options_, false, "", "");
  MounterUnderTest mounter(source_path_, target_path_,
                           filesystem_type_, mount_options_);

  EXPECT_CALL(mounter, MountImpl())
    .WillOnce(Return(kMountErrorInternal))
    .WillOnce(Return(kMountErrorInternal));
  EXPECT_FALSE(mounter.mount_options().IsReadOnlyOptionSet());
  EXPECT_EQ(kMountErrorInternal, mounter.Mount());
  EXPECT_TRUE(mounter.mount_options().IsReadOnlyOptionSet());
}

}  // namespace cros_disks
