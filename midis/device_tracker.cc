// Copyright 2017 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>

#include <utility>

#include <base/bind.h>
#include <base/files/file_enumerator.h>
#include <base/files/file_util.h>
#include <base/location.h>
#include <base/memory/ptr_util.h>
#include <base/posix/safe_strerror.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/string_util.h>
#include <base/threading/thread_task_runner_handle.h>
#include <brillo/message_loops/message_loop.h>

#include "midis/device_tracker.h"

namespace {

// udev constants.
const char kUdev[] = "udev";
const char kUdevSubsystemSound[] = "sound";
const char kUdevPropertySoundInitialized[] = "SOUND_INITIALIZED";
const char kUdevActionChange[] = "change";
const char kUdevActionRemove[] = "remove";

const char kMidiPrefix[] = "midi";

const int kIoctlMaxRetries = 10;

}  // namespace

namespace midis {

DeviceTracker::DeviceTracker() : udev_handler_(new UdevHandler(this)) {}

uint32_t DeviceTracker::GenerateDeviceId(uint32_t sys_num,
                                         uint32_t device_num) {
  return (sys_num << 8) | device_num;
}

UdevHandler::UdevHandler(DeviceTracker* ptr)
    : dev_tracker_(ptr), weak_factory_(this) {}

bool UdevHandler::InitUdevHandler() {
  // Initialize UDEV monitoring.
  udev_.reset(udev_new());
  udev_monitor_.reset(udev_monitor_new_from_netlink(udev_.get(), kUdev));
  if (!udev_monitor_) {
    LOG(ERROR) << "udev_monitor_new_from_netlink fails.";
    return false;
  }

  int err = udev_monitor_filter_add_match_subsystem_devtype(
      udev_monitor_.get(), kUdevSubsystemSound, nullptr);
  if (err != 0) {
    LOG(ERROR) << "udev_monitor_add_match_subsystem fails: "
               << base::safe_strerror(-err);
    return false;
  }

  err = udev_monitor_enable_receiving(udev_monitor_.get());
  if (err != 0) {
    LOG(ERROR) << "udev_monitor_enable_receiving fails: "
               << base::safe_strerror(-err);
    return false;
  }

  udev_monitor_fd_ = base::ScopedFD(udev_monitor_get_fd(udev_monitor_.get()));

  brillo::MessageLoop::current()->WatchFileDescriptor(
      FROM_HERE,
      udev_monitor_fd_.get(),
      brillo::MessageLoop::kWatchRead,
      true,
      base::Bind(&UdevHandler::ProcessUdevFd,
                 weak_factory_.GetWeakPtr(),
                 udev_monitor_fd_.get()));
  return true;
}

struct udev_device* UdevHandler::MonitorReceiveDevice() {
  return udev_monitor_receive_device(udev_monitor_.get());
}

bool DeviceTracker::InitDeviceTracker() {
  if (!udev_handler_->InitUdevHandler()) {
    LOG(ERROR) << "Failed to init UdevHandler.";
    return false;
  }

  return true;
}

void UdevHandler::ProcessUdevFd(int fd) {
  struct udev_device* dev = MonitorReceiveDevice();
  if (dev) {
    ProcessUdevEvent(dev);
  }
}

std::string UdevHandler::GetMidiDeviceDname(struct udev_device* device) {
  std::string result;

  const char* syspath = udev_device_get_syspath(device);
  if (syspath == nullptr) {
    LOG(ERROR) << "udev_device_get_syspath failed.";
    return result;
  }

  base::FileEnumerator enume(
      base::FilePath(syspath), false, base::FileEnumerator::DIRECTORIES);
  for (base::FilePath name = enume.Next(); !name.empty(); name = enume.Next()) {
    const std::string cur_name = name.BaseName().value();
    if (base::StartsWith(cur_name, kMidiPrefix, base::CompareCase::SENSITIVE)) {
      result = cur_name;
      LOG(INFO) << "Located MIDI Device: " << result;
      break;
    }
  }

  return result;
}

std::unique_ptr<struct snd_rawmidi_info> UdevHandler::GetDeviceInfo(
    const std::string& dname) {
  uint32_t card, device_num;
  int ret;

  ret = sscanf(dname.c_str(), "midiC%uD%u", &card, &device_num);
  if (ret != 2) {
    LOG(ERROR) << "Couldn't parse card,device number for entry: " << dname;
    return nullptr;
  }

  base::FilePath dev_path("/dev/snd/controlC");
  dev_path = dev_path.InsertBeforeExtension(std::to_string(card));

  base::ScopedFD fd;
  for (int retry_counter = 0; retry_counter < kIoctlMaxRetries;
       ++retry_counter) {
    fd = base::ScopedFD(open(dev_path.value().c_str(), O_RDWR | O_CLOEXEC));
    if (fd.is_valid())
      break;

    base::PlatformThread::Sleep(
        base::TimeDelta::FromMilliseconds(2 * (retry_counter + 1)));
  }
  if (!fd.is_valid()) {
    PLOG(ERROR) << "Not able to open device for ioctl: " << dev_path.value();
    return nullptr;
  }

  auto info = base::MakeUnique<snd_rawmidi_info>();
  memset(info.get(), 0, sizeof(snd_rawmidi_info));
  info->device = device_num;
  ret = ioctl(fd.get(), SNDRV_CTL_IOCTL_RAWMIDI_INFO, info.get());

  if (ret < 0) {
    PLOG(ERROR) << "IOCTL SNDRV_CTL_IOCTL_RAWMIDI_INFO failed: "
                << dev_path.value();
    return nullptr;
  }
  return info;
}

dev_t UdevHandler::GetDeviceDevNum(struct udev_device* device) {
  return udev_device_get_devnum(device);
}

const char* UdevHandler::GetDeviceSysNum(struct udev_device* device) {
  return udev_device_get_sysnum(device);
}

void DeviceTracker::AddDevice(struct udev_device* device) {
  std::string dname = udev_handler_->GetMidiDeviceDname(device);

  if (dname.empty()) {
    LOG(INFO) << "Device connected wasn't a MIDI device.";
    return;
  }

  auto info = udev_handler_->GetDeviceInfo(dname);
  if (!info) {
    LOG(ERROR) << "Couldn't parse info for device: " << dname;
    return;
  }

  dev_t dev_num = udev_handler_->GetDeviceDevNum(device);

  uint32_t sys_num;
  if (!base::StringToUint(udev_handler_->GetDeviceSysNum(device), &sys_num)) {
    LOG(ERROR) << "Error retrieving sysnum of device: " << dname;
    return;
  }

  uint32_t device_id = GenerateDeviceId(static_cast<uint32_t>(sys_num),
                                        static_cast<uint32_t>(dev_num));
  std::string dev_name(reinterpret_cast<char*>(info->name));

  devices_.emplace(device_id,
                   base::MakeUnique<Device>(dev_name,
                                            info->card,
                                            info->device,
                                            info->subdevices_count,
                                            info->flags));
}

void DeviceTracker::RemoveDevice(struct udev_device* device) {
  uint32_t sys_num;
  if (!base::StringToUint(udev_handler_->GetDeviceSysNum(device), &sys_num)) {
    LOG(ERROR) << "Error retrieving sysnum of device.";
    return;
  }

  uint32_t dev_num =
      static_cast<uint32_t>(udev_handler_->GetDeviceDevNum(device));

  auto it = devices_.find(GenerateDeviceId(sys_num, dev_num));
  if (it != devices_.end()) {
    // TODO(pmalani): Whole bunch of book-keeping has to be done here.
    // and notifications need to be sent to all clients.
    devices_.erase(it);
    LOG(INFO) << "Device: " << sys_num << "," << dev_num << " removed.";
  } else {
    LOG(ERROR) << "Device: " << sys_num << "," << dev_num << " not listed.";
  }
}

void UdevHandler::ProcessUdevEvent(struct udev_device* device) {
  // We're only interested in card devices, and that too those that are
  // initialized.
  if (!udev_device_get_property_value(device, kUdevPropertySoundInitialized))
    return;

  // Get the action. If no action, then we are doing first time enumeration
  // and the device is treated as new.
  const char* action = udev_device_get_action(device);
  if (!action) {
    action = kUdevActionChange;
  }

  if (strncmp(action, kUdevActionChange, sizeof(kUdevActionChange) - 1) == 0) {
    dev_tracker_->AddDevice(device);
  } else if (strncmp(action,
                     kUdevActionRemove,
                     sizeof(kUdevActionRemove) - 1) == 0) {
    dev_tracker_->RemoveDevice(device);
  } else {
    LOG(ERROR) << "Unknown action: " << action;
  }
}

Device::Device(const std::string& name,
               uint32_t card,
               uint32_t device,
               uint32_t num_subdevices,
               uint32_t flags)
    : name_(name),
      card_(card),
      device_(device),
      num_subdevices_(num_subdevices),
      flags_(flags) {
  LOG(INFO) << "Device created: " << name_;
}
}  // namespace midis
