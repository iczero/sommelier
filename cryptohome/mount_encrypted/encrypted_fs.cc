// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cryptohome/mount_encrypted/encrypted_fs.h"

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <memory>

#include <base/files/file_util.h>
#include <base/strings/string_number_conversions.h>

#include "cryptohome/cryptolib.h"
#include "cryptohome/mount_encrypted.h"
#include "cryptohome/mount_helpers.h"

namespace cryptohome {

namespace {

constexpr char kEncryptedFSType[] = "ext4";
constexpr char kCryptDevName[] = "encstateful";
constexpr char kDevMapperPath[] = "/dev/mapper";
constexpr char kProcDirtyExpirePath[] = "/proc/sys/vm/dirty_expire_centisecs";
constexpr float kSizePercent = 0.3;
constexpr uint64_t kSectorSize = 512;
constexpr uint64_t kExt4BlockSize = 4096;
constexpr uint64_t kExt4MinBytes = 16 * 1024 * 1024;
constexpr int kCryptAllowDiscard = 1;
constexpr unsigned int kResizeStepSeconds = 2;
constexpr uint64_t kExt4ResizeBlocks = 32768 * 10;
constexpr uint64_t kExt4BlocksPerGroup = 32768;
constexpr uint64_t kExt4InodeRatioDefault = 16384;
constexpr uint64_t kExt4InodeRatioMinimum = 2048;
constexpr char kExt4ExtendedOptions[] = "discard,lazy_itable_init";

// Temp. function to cleanly get the c_str() of FilePaths.
// TODO(sarthakkukreti): Remove once all functions use base::FilePath
const char* path_str(const base::FilePath& path) {
  return path.value().c_str();
}

bool CheckBind(Platform* platform, const BindMount& bind) {
  uid_t user;
  gid_t group;

  if (platform->Access(bind.src, R_OK) &&
      !platform->CreateDirectory(bind.src)) {
    PLOG(ERROR) << "mkdir " << bind.src;
    return false;
  }

  if (platform->Access(bind.dst, R_OK) &&
      !(platform->CreateDirectory(bind.dst) &&
        platform->SetPermissions(bind.dst, bind.mode))) {
    PLOG(ERROR) << "mkdir " << bind.dst;
    return false;
  }

  if (!platform->GetUserId(bind.owner, &user, &group)) {
    PLOG(ERROR) << "getpwnam" << bind.owner;
    return false;
  }

  // Destination may be on read-only filesystem, so skip tweaks.
  // Must do explicit chmod since mkdir()'s mode respects umask.
  if (!platform->SetPermissions(bind.src, bind.mode)) {
    PLOG(ERROR) << "chmod " << bind.src;
    return false;
  }
  if (!platform->SetOwnership(bind.src, user, group, true)) {
    PLOG(ERROR) << "chown " << bind.src;
    return false;
  }

  return true;
}

// Spawns a filesystem resizing process
void SpawnResizer(Platform* platform,
                  const base::FilePath& device,
                  uint64_t blocks,
                  uint64_t blocks_max) {
  // Ignore resizing if we know the filesystem was built to max size.
  if (blocks >= blocks_max) {
    PLOG(ERROR) << " Resizing aborted";
    return;
  }

  // TODO(keescook): Read superblock to find out the current size of
  // the filesystem (since statvfs does not report the correct value).
  // For now, instead of doing multi-step resizing, just resize to the
  // full size of the block device in one step.
  blocks = blocks_max;

  LOG(INFO) << "Resizing started in " << kResizeStepSeconds
             << " second steps.";

  do {
    sleep(kResizeStepSeconds);
    blocks += kExt4ResizeBlocks;

    if (blocks > blocks_max)
      blocks = blocks_max;

    // Run the resizing daemon
    platform->ResizeFilesystem(device, blocks);
  } while (blocks < blocks_max);

  LOG(INFO) << "Resizing daemon launched.";
  return;
}

std::string GetMountOpts() {
  // Use vm.dirty_expire_centisecs / 100 as the commit interval.
  std::string dirty_expire;
  uint64_t dirty_expire_centisecs;
  uint64_t commit_interval = 600;

  if (base::ReadFileToString(base::FilePath(kProcDirtyExpirePath),
                             &dirty_expire) &&
    base::StringToUint64(dirty_expire, &dirty_expire_centisecs)) {
    LOG(INFO) << "Using vm.dirty_expire_centisecs/100 as the commit interval";

    // Keep commit interval as 5 seconds (default for ext4) for smaller
    // values of dirty_expire_centisecs.
    if (dirty_expire_centisecs < 600)
      commit_interval = 5;
    else
      commit_interval = dirty_expire_centisecs / 100;
  }
  return "discard,commit=" + std::to_string(commit_interval);
}

// When creating a filesystem that will grow, the inode ratio is calculated
// using the starting size not the hinted "resize" size, which means the
// number of inodes can be highly constrained on tiny starting filesystems.
// Instead, calculate what the correct inode ratio should be for a given
// filesystem based on its expected starting and ending sizes.
//
// inode-ratio_mkfs =
//
//               ceil(blocks_max / group-ratio) * size_mkfs
//      ------------------------------------------------------------------
//      ceil(size_max / inode-ratio_max) * ceil(blocks_mkfs / group-ratio)
//
static uint64_t Ext4GetInodeRatio(uint64_t block_bytes_in,
                                  uint64_t blocks_mkfs_in,
                                  uint64_t blocks_max_in) {
  double block_bytes = static_cast<double>(block_bytes_in);
  double blocks_mkfs = static_cast<double>(blocks_mkfs_in);
  double blocks_max = static_cast<double>(blocks_max_in);

  double size_max, size_mkfs, groups_max;
  double inode_ratio_mkfs;

  uint64_t denom, inodes_max, groups_mkfs;

  size_max = block_bytes * blocks_max;
  size_mkfs = block_bytes * blocks_mkfs;

  groups_max = ceil(blocks_max / kExt4BlocksPerGroup);
  groups_mkfs = static_cast<uint64_t>(ceil(blocks_mkfs / kExt4BlocksPerGroup));

  inodes_max = static_cast<uint64_t>(ceil(size_max / kExt4InodeRatioDefault));

  denom = inodes_max * groups_mkfs;
  // Make sure we never trigger divide-by-zero.
  if (denom == 0)
    return kExt4InodeRatioDefault;

  inode_ratio_mkfs = (groups_max * size_mkfs) / denom;

  // Make sure we never calculate anything totally huge or totally tiny.
  if (inode_ratio_mkfs > blocks_mkfs ||
      inode_ratio_mkfs < kExt4InodeRatioMinimum)
    return kExt4InodeRatioDefault;

  return (uint64_t)inode_ratio_mkfs;
}

std::vector<std::string> BuildExt4FormatOpts(uint64_t block_bytes,
                                             uint64_t blocks_min,
                                             uint64_t blocks_max) {
  return {
      "-T",
      "default",
      "-b",
      std::to_string(block_bytes),
      "-m",
      "0",
      "-O",
      "^huge_file,^flex_bg",
      "-i",
      std::to_string(Ext4GetInodeRatio(block_bytes, blocks_min, blocks_max)),
      "-E",
      kExt4ExtendedOptions + ((blocks_min < blocks_max)
                                  ? ",resize=" + std::to_string(blocks_max)
                                  : "")};
}

}  // namespace

EncryptedFs::EncryptedFs(const base::FilePath& mount_root,
                         Platform* platform,
                         brillo::LoopDeviceManager* loop_device_manager) :
    platform_(platform), loopdev_manager_(loop_device_manager) {
  dmcrypt_name_ = std::string(kCryptDevName);
  rootdir_ = base::FilePath("/");
  if (!mount_root.empty()) {
    brillo::SecureBlob digest =
        cryptohome::CryptoLib::Sha256(brillo::SecureBlob(mount_root.value()));
    std::string hex = cryptohome::CryptoLib::BlobToHex(digest);
    dmcrypt_name_ += "_" + hex.substr(0, 16);
    rootdir_ = mount_root;
  }
  // Initialize remaining directories.
  stateful_mount_ = rootdir_.Append(STATEFUL_MNT);
  block_path_ = rootdir_.Append(STATEFUL_MNT "/encrypted.block");
  encrypted_mount_ = rootdir_.Append(ENCRYPTED_MNT);
  dmcrypt_dev_ = base::FilePath(kDevMapperPath).Append(dmcrypt_name_.c_str());

  // Create bind mounts.
  bind_mounts_.push_back(
      {rootdir_.Append(ENCRYPTED_MNT "/var"), rootdir_.Append("var"), "root",
       "root", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH, false});

  bind_mounts_.push_back({rootdir_.Append(ENCRYPTED_MNT "/chronos"),
                          rootdir_.Append("home/chronos"), "chronos", "chronos",
                          S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH,
                          true});
}

bool EncryptedFs::Purge() {
  LOG(INFO) << "Purging block file";
  return platform_->DeleteFile(block_path_, false);
}

bool EncryptedFs::CreateSparseBackingFile() {
  uint64_t fs_bytes_max;
  struct statvfs stateful_statbuf;

  // Calculate the desired size of the new partition.
  if (!platform_->StatVFS(stateful_mount_, &stateful_statbuf)) {
    PLOG(ERROR) << stateful_mount_;
    return false;
  }
  fs_bytes_max = stateful_statbuf.f_blocks;
  fs_bytes_max *= kSizePercent;
  fs_bytes_max *= stateful_statbuf.f_frsize;

  LOG(INFO) << "Creating sparse backing file with size " << fs_bytes_max;

  // Create the sparse file and return the descriptor.
  return platform_->CreateSparseFile(block_path_, fs_bytes_max) &&
    platform_->SetPermissions(block_path_, S_IRUSR | S_IWUSR);
}

// Do all the work needed to actually set up the encrypted partition.
result_code EncryptedFs::Setup(const brillo::SecureBlob& encryption_key,
                               bool rebuild) {
  result_code rc = RESULT_FAIL_FATAL;

  if (rebuild) {
    // Wipe out the old files, and ignore errors.
    Purge();

    // Create new sparse file.
    if (!CreateSparseBackingFile()) {
      PLOG(ERROR) << block_path_;
      return rc;
    }
  }

  // Set up loopback device.
  LOG(INFO) << "Loopback attaching " << block_path_ << " named "
            << dmcrypt_name_;
  std::unique_ptr<brillo::LoopDevice> lodev =
    loopdev_manager_->AttachDeviceToFile(block_path_);
  if (!lodev->IsValid()) {
    LOG(ERROR) << "Loop attach failed";
    return rc;
  }

  // Set loop device name.
  if (!lodev->SetName(dmcrypt_name_)) {
    LOG(ERROR) << "Loop set name failed";
    return rc;
  }

  base::FilePath lodev_path = lodev->GetDevicePath();

  // Get size as seen by block device.
  uint64_t blkdev_size;
  if (!platform_->GetBlkSize(lodev_path, &blkdev_size) &&
      blkdev_size < kExt4BlockSize) {
    LOG(ERROR) << "Failed to read device size";
    TeardownByStage(TeardownStage::kTeardownLoopDevice, true);
    return rc;
  }

  // Mount loopback device with dm-crypt using the encryption key.
  LOG(INFO) << "Setting up dm-crypt " << lodev_path << " as " << dmcrypt_dev_;

  uint64_t sectors = blkdev_size / kSectorSize;
  std::string encryption_key_hex =
      base::HexEncode(encryption_key.data(), encryption_key.size());
  if (!dm_setup(sectors, encryption_key_hex.c_str(), dmcrypt_name_.c_str(),
                lodev_path.value().c_str(), path_str(dmcrypt_dev_),
                kCryptAllowDiscard)) {
    // If dm_setup() fails, it could be due to lacking
    // "allow_discard" support, so try again with discard
    // disabled. There doesn't seem to be a way to query
    // the kernel for this feature short of a fallible
    // version test or just trying to set up the dm table
    // again, so do the latter.
    //
    if (!dm_setup(sectors, encryption_key_hex.c_str(), dmcrypt_name_.c_str(),
                  lodev_path.value().c_str(), path_str(dmcrypt_dev_),
                  !kCryptAllowDiscard)) {
      LOG(ERROR) << "dm_setup failed";
      TeardownByStage(TeardownStage::kTeardownLoopDevice, true);
      return rc;
    }
    LOG(INFO) << dmcrypt_dev_
              << ": dm-crypt does not support discard; disabling.";
  }

  // Calculate filesystem min/max size.
  uint64_t blocks_max = blkdev_size / kExt4BlockSize;
  uint64_t blocks_min = kExt4MinBytes / kExt4BlockSize;

  if (rebuild) {
    LOG(INFO) << "Building filesystem on " << dmcrypt_dev_
              << "(blocksize: " << kExt4BlockSize << ", min: " << blocks_min
              << ", max: " << blocks_max;
    if (!platform_->FormatExt4(
            dmcrypt_dev_,
            BuildExt4FormatOpts(kExt4BlockSize, blocks_min, blocks_max),
            blocks_min)) {
      TeardownByStage(TeardownStage::kTeardownDevmapper, true);
      return rc;
    }
  }

  // Mount the dm-crypt partition finally.
  LOG(INFO) << "Mounting " << dmcrypt_dev_ << " onto " << encrypted_mount_;
  if (platform_->Access(encrypted_mount_, R_OK) &&
      !(platform_->CreateDirectory(encrypted_mount_) &&
        platform_->SetPermissions(encrypted_mount_,
                                  S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH))) {
    PLOG(ERROR) << dmcrypt_dev_;
    TeardownByStage(TeardownStage::kTeardownDevmapper, true);
    return rc;
  }
  if (!platform_->Mount(dmcrypt_dev_, encrypted_mount_, kEncryptedFSType,
                        MS_NODEV | MS_NOEXEC | MS_NOSUID | MS_NOATIME,
                        GetMountOpts().c_str())) {
    PLOG(ERROR) << "mount: " << dmcrypt_dev_ << ", " << encrypted_mount_;
    TeardownByStage(TeardownStage::kTeardownDevmapper, true);
    return rc;
  }

  // Always spawn filesystem resizer, in case growth was interrupted.
  // TODO(keescook): if already full size, don't resize.
  SpawnResizer(platform_, dmcrypt_dev_, blocks_min, blocks_max);

  // Perform bind mounts.
  for (auto& bind : bind_mounts_) {
    LOG(INFO) << "Bind mounting " << bind.src << " onto " << bind.dst;
    if (!CheckBind(platform_, bind)) {
      TeardownByStage(TeardownStage::kTeardownUnbind, true);
      return rc;
    }
    if (!platform_->Bind(bind.src, bind.dst)) {
      PLOG(ERROR) << "mount: " << bind.src << ", " << bind.dst;
      TeardownByStage(TeardownStage::kTeardownUnbind, true);
      return rc;
    }
  }

  // Everything completed without error.
  return RESULT_SUCCESS;
}

// Clean up all bind mounts, mounts, attaches, etc. Only the final
// action informs the return value. This makes it so that failures
// can be cleaned up from, and continue the shutdown process on a
// second call. If the loopback cannot be found, claim success.
result_code EncryptedFs::Teardown() {
  return TeardownByStage(TeardownStage::kTeardownUnbind, false);
}

result_code EncryptedFs::TeardownByStage(TeardownStage stage,
                                         bool ignore_errors) {
  switch (stage) {
    case TeardownStage::kTeardownUnbind:
      for (auto& bind : bind_mounts_) {
        LOG(INFO) << "Unmounting " << bind.dst;
        errno = 0;
        // Allow either success or a "not mounted" failure.
        if (!platform_->Unmount(bind.dst, false, nullptr) && !ignore_errors) {
          if (errno != EINVAL) {
            PLOG(ERROR) << "umount " << bind.dst;
            return RESULT_FAIL_FATAL;
          }
        }
      }

      LOG(INFO) << "Unmounting " << encrypted_mount_;
      errno = 0;
      // Allow either success or a "not mounted" failure.
      if (!platform_->Unmount(encrypted_mount_, false, nullptr) &&
          !ignore_errors) {
        if (errno != EINVAL) {
          PLOG(ERROR) << "umount " << encrypted_mount_;
          return RESULT_FAIL_FATAL;
        }
      }

      // Force syncs to make sure we don't tickle racey/buggy kernel
      // routines that might be causing crosbug.com/p/17610.
      platform_->Sync();

    // Intentionally fall through here to teardown the lower dmcrypt device.
    case TeardownStage::kTeardownDevmapper:
      LOG(INFO) << "Removing " << dmcrypt_dev_;
      if (!dm_teardown(path_str(dmcrypt_dev_)) && !ignore_errors)
        LOG(ERROR) << "dm_teardown: " << dmcrypt_dev_;
      platform_->Sync();

    // Intentionally fall through here to teardown the lower loop device.
    case TeardownStage::kTeardownLoopDevice:
      LOG(INFO) << "Unlooping " << block_path_ << " named " << dmcrypt_name_;
      std::unique_ptr<brillo::LoopDevice> lodev =
        loopdev_manager_->GetAttachedDeviceByName(dmcrypt_name_);
      if (!(lodev->IsValid() && lodev->Detach()) && !ignore_errors) {
        LOG(ERROR) << "loop_detach_name: " << dmcrypt_name_;
        return RESULT_FAIL_FATAL;
      }
      platform_->Sync();
      return RESULT_SUCCESS;
  }

  LOG(ERROR) << "Teardown failed.";
  return RESULT_FAIL_FATAL;
}

result_code EncryptedFs::CheckStates(void) {
  // Verify stateful partition exists.
  if (platform_->Access(stateful_mount_, R_OK)) {
    LOG(INFO) << stateful_mount_ << "does not exist.";
    return RESULT_FAIL_FATAL;
  }
  // Verify stateful is either a separate mount, or that the
  // root directory is writable (i.e. a factory install, dev mode
  // where root remounted rw, etc).
  if (platform_->SameVFS(stateful_mount_, rootdir_) &&
      platform_->Access(rootdir_, W_OK)) {
    LOG(INFO) << stateful_mount_ << " is not mounted.";
    return RESULT_FAIL_FATAL;
  }

  // Verify encrypted partition is missing or not already mounted.
  if (platform_->Access(encrypted_mount_, R_OK) == 0 &&
      !platform_->SameVFS(encrypted_mount_, stateful_mount_)) {
    LOG(INFO) << encrypted_mount_ << " already appears to be mounted.";
    return RESULT_SUCCESS;
  }

  // Verify that bind mount targets exist.
  for (auto& bind : bind_mounts_) {
    if (platform_->Access(bind.dst, R_OK)) {
      PLOG(ERROR) << bind.dst << " mount point is missing.";
      return RESULT_FAIL_FATAL;
    }
  }

  // Verify that old bind mounts on stateful haven't happened yet.
  for (auto& bind : bind_mounts_) {
    if (bind.submount)
      continue;

    if (platform_->SameVFS(bind.dst, stateful_mount_)) {
      LOG(INFO) << bind.dst << " already bind mounted.";
      return RESULT_FAIL_FATAL;
    }
  }

  LOG(INFO) << "VFS mount state sanity check ok.";
  return RESULT_SUCCESS;
}

result_code EncryptedFs::ReportInfo(void) const {
  printf("rootdir: %s\n", path_str(rootdir_));
  printf("stateful_mount: %s\n", path_str(stateful_mount_));
  printf("block_path: %s\n", path_str(block_path_));
  printf("encrypted_mount: %s\n", path_str(encrypted_mount_));
  printf("dmcrypt_name: %s\n", dmcrypt_name_.c_str());
  printf("dmcrypt_dev: %s\n", path_str(dmcrypt_dev_));
  printf("bind mounts:\n");
  for (auto& mnt : bind_mounts_) {
    printf("\tsrc:%s\n", path_str(mnt.src));
    printf("\tdst:%s\n", path_str(mnt.dst));
    printf("\towner:%s\n", mnt.owner.c_str());
    printf("\tmode:%o\n", mnt.mode);
    printf("\tsubmount:%d\n", mnt.submount);
    printf("\n");
  }
  return RESULT_SUCCESS;
}

brillo::SecureBlob EncryptedFs::GetKey() const {
  char* key = dm_get_key(path_str(dmcrypt_dev_));
  brillo::SecureBlob encryption_key;
  if (!base::HexStringToBytes(key, &encryption_key)) {
    LOG(ERROR) << "Failed to decode encryption key.";
    return brillo::SecureBlob();
  }
  return encryption_key;
}

}  // namespace cryptohome
