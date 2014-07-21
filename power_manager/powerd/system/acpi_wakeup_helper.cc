// Copyright 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "power_manager/powerd/system/acpi_wakeup_helper.h"

#include <base/file_util.h>
#include <base/logging.h>
#include <base/macros.h>
#include <base/strings/string_piece.h>
#include <base/strings/string_tokenizer.h>

namespace power_manager {
namespace system {

namespace {
const base::FilePath kAcpiWakeupPath("/proc/acpi/wakeup");

class AcpiWakeupFile : public AcpiWakeupFileInterface {
 public:
  AcpiWakeupFile() {}
  virtual ~AcpiWakeupFile() {}

  virtual bool Exists() OVERRIDE {
    return base::PathExists(kAcpiWakeupPath);
  }

  virtual bool Read(std::string* contents) OVERRIDE {
    CHECK(contents);
    return base::ReadFileToString(kAcpiWakeupPath, contents);
  }

  virtual bool Write(const std::string& contents) OVERRIDE {
    int bytes_written = base::WriteFile(kAcpiWakeupPath, contents.data(),
                                        contents.size());
    return bytes_written == static_cast<int>(contents.size());
  }
};
}  // namespace

AcpiWakeupHelper::AcpiWakeupHelper()
    : file_(new AcpiWakeupFile()) {
}

void AcpiWakeupHelper::set_file_for_testing(
    scoped_ptr<AcpiWakeupFileInterface> file) {
  file_ = file.Pass();
}

bool AcpiWakeupHelper::IsSupported() {
  return file_->Exists();
}

bool AcpiWakeupHelper::GetWakeupEnabled(const std::string& device_name,
                                        bool* enabled_out) {
  CHECK(enabled_out);

  std::string contents;
  if (!file_->Read(&contents)) {
    LOG(ERROR) << "Failed to read state from " << kAcpiWakeupPath.value();
    return false;
  }

  // /proc/acpi/wakeup looks like this:
  //   Device  S-state  Status  Sysfs node
  //   TPAD      S3   *enabled  pnp:00:00
  //
  // This is a mess to parse, since some of the whitespace is tabs and some is
  // space padding.  To keep things simple, we just look for "enabled/disabled",
  // which should never conflict with values of other fields anyway.

  base::StringTokenizer lines(contents, "\n");
  while (lines.GetNext()) {
    std::string line = lines.token();
    base::StringTokenizer parts(line, "\t *");
    // Check whether first part matches device name.
    if (!parts.GetNext())
      continue;
    if (parts.token_piece() != device_name)
      continue;
    // Find enabled/disabled in later parts.
    while (parts.GetNext()) {
      base::StringPiece part = parts.token_piece();
      if (part == base::StringPiece("enabled")) {
        *enabled_out = true;
        return true;
      } else if (part == base::StringPiece("disabled")) {
        *enabled_out = false;
        return true;
      }
    }
    LOG(WARNING) << "Found device '" << device_name << "' in "
                 << kAcpiWakeupPath.value()
                 << ", but failed to determine wakeup state";
    return false;
  }
  VLOG(1) << "Device '" << device_name << "' not found in "
          << kAcpiWakeupPath.value();
  return false;
}

bool AcpiWakeupHelper::SetWakeupEnabled(const std::string& device_name,
                                        bool enabled) {
  // The kernel does not exhibit an interface to set the state directly, we can
  // only get and toggle.
  bool readback;
  if (!GetWakeupEnabled(device_name, &readback))
    return false;
  if (readback != enabled) {
    if (!ToggleWakeupEnabled(device_name))
      return false;
    if (!GetWakeupEnabled(device_name, &readback) || readback != enabled)
      return false;
    VLOG(1) << "ACPI wakeup for " << device_name << " is now "
            << (enabled ? "enabled" : "disabled");
  }
  return true;
}

bool AcpiWakeupHelper::ToggleWakeupEnabled(const std::string& device_name) {
  if (!file_->Write(device_name)) {
    LOG(ERROR) << "Failed to write to " << kAcpiWakeupPath.value();
    return false;
  }
  return true;
}

}  // namespace system
}  // namespace power_manager
