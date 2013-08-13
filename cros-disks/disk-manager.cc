// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cros-disks/disk-manager.h"

#include <libudev.h>
#include <string.h>
#include <sys/mount.h>

#include <base/logging.h>
#include <base/memory/scoped_ptr.h>
#include <base/stl_util.h>
#include <base/string_util.h>
#include <base/stringprintf.h>

#include "cros-disks/disk.h"
#include "cros-disks/exfat-mounter.h"
#include "cros-disks/external-mounter.h"
#include "cros-disks/filesystem.h"
#include "cros-disks/metrics.h"
#include "cros-disks/mount-options.h"
#include "cros-disks/ntfs-mounter.h"
#include "cros-disks/platform.h"
#include "cros-disks/system-mounter.h"
#include "cros-disks/udev-device.h"

using std::map;
using std::set;
using std::string;
using std::vector;

namespace {

const char kBlockSubsystem[] = "block";
const char kScsiSubsystem[] = "scsi";
const char kScsiDevice[] = "scsi_device";
const char kUdevAddAction[] = "add";
const char kUdevChangeAction[] = "change";
const char kUdevRemoveAction[] = "remove";
const char kPropertyDiskEjectRequest[] = "DISK_EJECT_REQUEST";
const char kPropertyDiskMediaChange[] = "DISK_MEDIA_CHANGE";

}  // namespace

namespace cros_disks {

DiskManager::DiskManager(const string& mount_root, Platform* platform,
                         Metrics* metrics, DeviceEjector* device_ejector)
    : MountManager(mount_root, platform, metrics),
      device_ejector_(device_ejector),
      udev_(udev_new()),
      udev_monitor_fd_(0),
      eject_device_on_unmount_(true) {
  CHECK(device_ejector_) << "Invalid device ejector";
  CHECK(udev_) << "Failed to initialize udev";
  udev_monitor_ = udev_monitor_new_from_netlink(udev_, "udev");
  CHECK(udev_monitor_) << "Failed to create a udev monitor";
  udev_monitor_filter_add_match_subsystem_devtype(udev_monitor_,
                                                  kBlockSubsystem, NULL);
  udev_monitor_filter_add_match_subsystem_devtype(udev_monitor_,
                                                  kScsiSubsystem, kScsiDevice);
  udev_monitor_enable_receiving(udev_monitor_);
  udev_monitor_fd_ = udev_monitor_get_fd(udev_monitor_);
}

DiskManager::~DiskManager() {
  UnmountAll();
  udev_monitor_unref(udev_monitor_);
  udev_unref(udev_);
}

bool DiskManager::Initialize() {
  RegisterDefaultFilesystems();

  // Initialize |disks_detected_| with auto-mountable devices that already
  // exist when disk manager starts since there is no udev add event that adds
  // these devices to |disks_detected_|.
  vector<Disk> disks = EnumerateDisks();
  for (vector<Disk>::const_iterator disk_iterator = disks.begin();
       disk_iterator != disks.end(); ++disk_iterator) {
    if (disk_iterator->is_auto_mountable()) {
      disks_detected_.insert(
          std::make_pair(disk_iterator->native_path(), set<string>()));
    }
  }

  return MountManager::Initialize();
}

bool DiskManager::StopSession() {
  return UnmountAll();
}

vector<Disk> DiskManager::EnumerateDisks() const {
  vector<Disk> disks;

  struct udev_enumerate *enumerate = udev_enumerate_new(udev_);
  udev_enumerate_add_match_subsystem(enumerate, kBlockSubsystem);
  udev_enumerate_scan_devices(enumerate);

  struct udev_list_entry *device_list, *device_list_entry;
  device_list = udev_enumerate_get_list_entry(enumerate);
  udev_list_entry_foreach(device_list_entry, device_list) {
    const char *path = udev_list_entry_get_name(device_list_entry);
    udev_device *dev = udev_device_new_from_syspath(udev_, path);
    if (dev == NULL) continue;

    LOG(INFO) << "Device";
    LOG(INFO) << "   Node: " << udev_device_get_devnode(dev);
    LOG(INFO) << "   Subsystem: " << udev_device_get_subsystem(dev);
    LOG(INFO) << "   Devtype: " << udev_device_get_devtype(dev);
    LOG(INFO) << "   Devpath: " << udev_device_get_devpath(dev);
    LOG(INFO) << "   Sysname: " << udev_device_get_sysname(dev);
    LOG(INFO) << "   Syspath: " << udev_device_get_syspath(dev);
    LOG(INFO) << "   Properties: ";
    struct udev_list_entry *property_list, *property_list_entry;
    property_list = udev_device_get_properties_list_entry(dev);
    udev_list_entry_foreach(property_list_entry, property_list) {
      const char *key = udev_list_entry_get_name(property_list_entry);
      const char *value = udev_list_entry_get_value(property_list_entry);
      LOG(INFO) << "      " << key << " = " << value;
    }

    UdevDevice device(dev);
    if (!device.IsIgnored()) {
      disks.push_back(device.ToDisk());
    }
    udev_device_unref(dev);
  }

  udev_enumerate_unref(enumerate);

  return disks;
}

void DiskManager::ProcessBlockDeviceEvents(
    struct udev_device* dev, const char* action, DeviceEventList* events) {
  UdevDevice device(dev);
  if (device.IsIgnored())
    return;

  bool disk_added = false;
  bool disk_removed = false;
  bool child_disk_removed = false;
  if (strcmp(action, kUdevAddAction) == 0) {
    disk_added = true;
  } else if (strcmp(action, kUdevRemoveAction) == 0) {
    disk_removed = true;
  } else if (strcmp(action, kUdevChangeAction) == 0) {
    // For removable devices like CD-ROM, an eject request event
    // is treated as disk removal, while a media change event with
    // media available is treated as disk insertion.
    if (device.IsPropertyTrue(kPropertyDiskEjectRequest)) {
      disk_removed = true;
    } else if (device.IsPropertyTrue(kPropertyDiskMediaChange)) {
      if (device.IsMediaAvailable()) {
        disk_added = true;
      } else {
        child_disk_removed = true;
      }
    }
  }

  string device_path = device.NativePath();
  if (disk_added) {
    if (device.IsAutoMountable()) {
      if (ContainsKey(disks_detected_, device_path)) {
        // Disk already exists, so remove it and then add it again.
        events->push_back(DeviceEvent(DeviceEvent::kDiskRemoved, device_path));
      } else {
        disks_detected_[device_path] = set<string>();

        // Add the disk as a child of its parent if the parent is already
        // added to |disks_detected_|.
        struct udev_device* parent = udev_device_get_parent(dev);
        if (parent) {
          string parent_device_path = UdevDevice(parent).NativePath();
          if (ContainsKey(disks_detected_, parent_device_path)) {
            disks_detected_[parent_device_path].insert(device_path);
          }
        }
      }
      events->push_back(DeviceEvent(DeviceEvent::kDiskAdded, device_path));
    }
  } else if (disk_removed) {
    disks_detected_.erase(device_path);
    events->push_back(DeviceEvent(DeviceEvent::kDiskRemoved, device_path));
  } else if (child_disk_removed) {
    if (ContainsKey(disks_detected_, device_path)) {
      set<string>& child_disks = disks_detected_[device_path];
      for (set<string>::const_iterator child_iter = child_disks.begin();
           child_iter != child_disks.end(); ++child_iter) {
        events->push_back(DeviceEvent(DeviceEvent::kDiskRemoved, *child_iter));
      }
    }
  }
}

void DiskManager::ProcessScsiDeviceEvents(
    struct udev_device* dev, const char* action, DeviceEventList* events) {
  UdevDevice device(dev);
  if (device.IsMobileBroadbandDevice())
    return;

  string device_path = device.NativePath();
  if (strcmp(action, kUdevAddAction) == 0) {
    if (ContainsKey(devices_detected_, device_path)) {
      events->push_back(DeviceEvent(DeviceEvent::kDeviceScanned, device_path));
    } else {
      devices_detected_.insert(device_path);
      events->push_back(DeviceEvent(DeviceEvent::kDeviceAdded, device_path));
    }
  } else if (strcmp(action, kUdevRemoveAction) == 0) {
    if (ContainsKey(devices_detected_, device_path)) {
      devices_detected_.erase(device_path);
      events->push_back(DeviceEvent(DeviceEvent::kDeviceRemoved, device_path));
    }
  }
}

bool DiskManager::GetDeviceEvents(DeviceEventList* events) {
  CHECK(events) << "Invalid device event list";

  struct udev_device *dev = udev_monitor_receive_device(udev_monitor_);
  if (!dev) {
    LOG(WARNING) << "Ignore device event with no associated udev device.";
    return false;
  }

  LOG(INFO) << "Got Device";
  LOG(INFO) << "   Syspath: " << udev_device_get_syspath(dev);
  LOG(INFO) << "   Node: " << udev_device_get_devnode(dev);
  LOG(INFO) << "   Subsystem: " << udev_device_get_subsystem(dev);
  LOG(INFO) << "   Devtype: " << udev_device_get_devtype(dev);
  LOG(INFO) << "   Action: " << udev_device_get_action(dev);

  const char *sys_path = udev_device_get_syspath(dev);
  const char *subsystem = udev_device_get_subsystem(dev);
  const char *action = udev_device_get_action(dev);
  if (!sys_path || !subsystem || !action) {
    udev_device_unref(dev);
    return false;
  }

  // udev_monitor_ only monitors block or scsi device changes, so
  // subsystem is either "block" or "scsi".
  if (strcmp(subsystem, kBlockSubsystem) == 0) {
    ProcessBlockDeviceEvents(dev, action, events);
  } else {  // strcmp(subsystem, kScsiSubsystem) == 0
    ProcessScsiDeviceEvents(dev, action, events);
  }

  udev_device_unref(dev);
  return true;
}

bool DiskManager::GetDiskByDevicePath(const string& device_path,
                                      Disk *disk) const {
  if (device_path.empty())
    return false;

  bool is_sys_path = StartsWithASCII(device_path, "/sys/", true);
  bool is_dev_path = StartsWithASCII(device_path, "/devices/", true);
  bool is_dev_file = StartsWithASCII(device_path, "/dev/", true);
  bool disk_found = false;

  struct udev_enumerate *enumerate = udev_enumerate_new(udev_);
  udev_enumerate_add_match_subsystem(enumerate, kBlockSubsystem);
  udev_enumerate_scan_devices(enumerate);

  struct udev_list_entry *device_list, *device_list_entry;
  device_list = udev_enumerate_get_list_entry(enumerate);
  udev_list_entry_foreach(device_list_entry, device_list) {
    const char *sys_path = udev_list_entry_get_name(device_list_entry);
    udev_device *dev = udev_device_new_from_syspath(udev_, sys_path);
    if (dev == NULL) continue;

    const char *dev_path = udev_device_get_devpath(dev);
    const char *dev_file = udev_device_get_devnode(dev);
    if ((is_sys_path && device_path == sys_path) ||
        (is_dev_path && device_path == dev_path) ||
        (is_dev_file && dev_file && device_path == dev_file)) {
      disk_found = true;
      if (disk)
        *disk = UdevDevice(dev).ToDisk();
    }
    udev_device_unref(dev);
    if (disk_found)
      break;
  }

  udev_enumerate_unref(enumerate);

  return disk_found;
}

const Filesystem* DiskManager::GetFilesystem(
    const string& filesystem_type) const {
  map<string, Filesystem>::const_iterator filesystem_iterator =
      filesystems_.find(filesystem_type);
  if (filesystem_iterator == filesystems_.end())
    return NULL;

  if (!platform()->experimental_features_enabled() &&
      filesystem_iterator->second.is_experimental())
    return NULL;

  return &filesystem_iterator->second;
}

void DiskManager::RegisterDefaultFilesystems() {
  // TODO(benchan): Perhaps these settings can be read from a config file.
  Filesystem vfat_fs("vfat");
  vfat_fs.set_accepts_user_and_group_id(true);
  vfat_fs.AddExtraMountOption("flush");
  vfat_fs.AddExtraMountOption("shortname=mixed");
  vfat_fs.AddExtraMountOption("utf8");
  RegisterFilesystem(vfat_fs);

  Filesystem exfat_fs("exfat");
  exfat_fs.set_mounter_type(ExFATMounter::kMounterType);
  exfat_fs.set_accepts_user_and_group_id(true);
  RegisterFilesystem(exfat_fs);

  Filesystem ntfs_fs("ntfs");
  ntfs_fs.set_mounter_type(NTFSMounter::kMounterType);
  ntfs_fs.set_accepts_user_and_group_id(true);
  RegisterFilesystem(ntfs_fs);

  Filesystem hfsplus_fs("hfsplus");
  hfsplus_fs.set_accepts_user_and_group_id(true);
  RegisterFilesystem(hfsplus_fs);

  Filesystem iso9660_fs("iso9660");
  iso9660_fs.set_is_mounted_read_only(true);
  iso9660_fs.set_accepts_user_and_group_id(true);
  iso9660_fs.AddExtraMountOption("utf8");
  RegisterFilesystem(iso9660_fs);

  Filesystem udf_fs("udf");
  udf_fs.set_is_mounted_read_only(true);
  udf_fs.set_accepts_user_and_group_id(true);
  udf_fs.AddExtraMountOption("utf8");
  RegisterFilesystem(udf_fs);

  Filesystem ext2_fs("ext2");
  RegisterFilesystem(ext2_fs);

  Filesystem ext3_fs("ext3");
  RegisterFilesystem(ext3_fs);

  Filesystem ext4_fs("ext4");
  RegisterFilesystem(ext4_fs);
}

void DiskManager::RegisterFilesystem(const Filesystem& filesystem) {
  filesystems_.insert(std::make_pair(filesystem.type(), filesystem));
}

Mounter* DiskManager::CreateMounter(const Disk& disk,
                                    const Filesystem& filesystem,
                                    const string& target_path,
                                    const vector<string>& options) const {
  const vector<string>& extra_options = filesystem.extra_mount_options();
  vector<string> extended_options;
  extended_options.reserve(options.size() + extra_options.size());
  extended_options.assign(options.begin(), options.end());
  extended_options.insert(extended_options.end(),
                          extra_options.begin(), extra_options.end());

  string default_user_id, default_group_id;
  bool set_user_and_group_id = filesystem.accepts_user_and_group_id();
  if (set_user_and_group_id) {
    default_user_id = base::StringPrintf("%d", platform()->mount_user_id());
    default_group_id = base::StringPrintf("%d", platform()->mount_group_id());
  }

  MountOptions mount_options;
  mount_options.Initialize(extended_options, set_user_and_group_id,
                           default_user_id, default_group_id);

  if (filesystem.is_mounted_read_only() ||
      disk.is_read_only() || disk.is_optical_disk()) {
    mount_options.SetReadOnlyOption();
  }

  const string& mounter_type = filesystem.mounter_type();
  if (mounter_type == SystemMounter::kMounterType)
    return new(std::nothrow) SystemMounter(disk.device_file(), target_path,
                                           filesystem.mount_type(),
                                           mount_options);

  if (mounter_type == ExternalMounter::kMounterType)
    return new(std::nothrow) ExternalMounter(disk.device_file(), target_path,
                                             filesystem.mount_type(),
                                             mount_options);

  if (mounter_type == ExFATMounter::kMounterType)
    return new(std::nothrow) ExFATMounter(disk.device_file(), target_path,
                                          filesystem.mount_type(),
                                          mount_options, platform());

  if (mounter_type == NTFSMounter::kMounterType)
    return new(std::nothrow) NTFSMounter(disk.device_file(), target_path,
                                         filesystem.mount_type(),
                                         mount_options, platform());

  LOG(FATAL) << "Invalid mounter type '" << mounter_type << "'";
  return NULL;
}

bool DiskManager::CanMount(const string& source_path) const {
  // The following paths can be mounted:
  //     /sys/...
  //     /devices/...
  //     /dev/...
  return StartsWithASCII(source_path, "/sys/", true) ||
      StartsWithASCII(source_path, "/devices/", true) ||
      StartsWithASCII(source_path, "/dev/", true);
}

MountErrorType DiskManager::DoMount(const string& source_path,
                                    const string& filesystem_type,
                                    const vector<string>& options,
                                    const string& mount_path) {
  CHECK(!source_path.empty()) << "Invalid source path argument";
  CHECK(!mount_path.empty()) << "Invalid mount path argument";

  Disk disk;
  if (!GetDiskByDevicePath(source_path, &disk)) {
    LOG(ERROR) << "'" << source_path << "' is not a valid device.";
    return MOUNT_ERROR_INVALID_DEVICE_PATH;
  }

  const string& device_file = disk.device_file();
  if (device_file.empty()) {
    LOG(ERROR) << "'" << source_path << "' does not have a device file";
    return MOUNT_ERROR_INVALID_DEVICE_PATH;
  }

  string device_filesystem_type = filesystem_type.empty() ?
      disk.filesystem_type() : filesystem_type;
  metrics()->RecordDeviceMediaType(disk.media_type());
  metrics()->RecordFilesystemType(device_filesystem_type);
  if (device_filesystem_type.empty()) {
    LOG(ERROR) << "Failed to determine the file system type of device '"
               << source_path << "'";
    return MOUNT_ERROR_UNKNOWN_FILESYSTEM;
  }

  const Filesystem* filesystem = GetFilesystem(device_filesystem_type);
  if (filesystem == NULL) {
    LOG(ERROR) << "File system type '" << device_filesystem_type
               << "' on device '" << source_path << "' is not supported";
    return MOUNT_ERROR_UNSUPPORTED_FILESYSTEM;
  }

  scoped_ptr<Mounter> mounter(CreateMounter(disk, *filesystem, mount_path,
                                            options));
  CHECK(mounter.get() != NULL) << "Failed to create a mounter";

  MountErrorType error_type = mounter->Mount();
  if (error_type == MOUNT_ERROR_NONE) {
    ScheduleEjectOnUnmount(mount_path, disk);
  }

  return error_type;
}

MountErrorType DiskManager::DoUnmount(const string& path,
                                      const vector<string>& options) {
  CHECK(!path.empty()) << "Invalid path argument";

  int unmount_flags;
  if (!ExtractUnmountOptions(options, &unmount_flags)) {
    LOG(ERROR) << "Invalid unmount options";
    return MOUNT_ERROR_INVALID_UNMOUNT_OPTIONS;
  }

  if (umount2(path.c_str(), unmount_flags) != 0) {
    PLOG(ERROR) << "Failed to unmount '" << path << "'";
    // TODO(benchan): Extract error from low-level unmount operation.
    return MOUNT_ERROR_UNKNOWN;
  }

  EjectDeviceOfMountPath(path);

  return MOUNT_ERROR_NONE;
}

string DiskManager::SuggestMountPath(const string& source_path) const {
  Disk disk;
  GetDiskByDevicePath(source_path, &disk);
  // If GetDiskByDevicePath fails, disk.GetPresentationName() returns
  // the fallback presentation name.
  return string(mount_root()) + "/" + disk.GetPresentationName();
}

bool DiskManager::ShouldReserveMountPathOnError(
    MountErrorType error_type) const {
  return error_type == MOUNT_ERROR_UNKNOWN_FILESYSTEM ||
         error_type == MOUNT_ERROR_UNSUPPORTED_FILESYSTEM;
}

bool DiskManager::ScheduleEjectOnUnmount(const string& mount_path,
                                         const Disk& disk) {
  if (!disk.is_optical_disk())
    return false;

  devices_to_eject_on_unmount_[mount_path] = disk.device_file();
  return true;
}

bool DiskManager::EjectDeviceOfMountPath(const string& mount_path) {
  map<string, string>::iterator device_iterator =
      devices_to_eject_on_unmount_.find(mount_path);
  if (device_iterator == devices_to_eject_on_unmount_.end())
    return false;

  string device_file = device_iterator->second;
  devices_to_eject_on_unmount_.erase(device_iterator);

  if (!eject_device_on_unmount_)
    return false;

  LOG(INFO) << "Eject device '" << device_file << "'.";
  if (!device_ejector_->Eject(device_file)) {
    LOG(WARNING) << "Failed to eject media from optical device '"
                 << device_file << "'.";
    return false;
  }

  return true;
}

bool DiskManager::UnmountAll() {
  // UnmountAll() is called when a user session ends. We do not want to eject
  // devices in that situation and thus set |eject_device_on_unmount_| to
  // false temporarily to prevent devices from being ejected upon unmount.
  eject_device_on_unmount_ = false;
  bool all_unmounted = MountManager::UnmountAll();
  eject_device_on_unmount_ = true;
  return all_unmounted;
}

}  // namespace cros_disks
