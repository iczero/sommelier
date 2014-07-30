// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEBUGD_SRC_MEMORY_TOOL_H_
#define DEBUGD_SRC_MEMORY_TOOL_H_

#include <string>

#include <base/basictypes.h>
#include <dbus-c++/dbus.h>

#include "debugd/src/subprocess_tool.h"

namespace debugd {

class MemtesterTool : public SubprocessTool {
 public:
  MemtesterTool();
  virtual ~MemtesterTool();

  std::string Start(const DBus::FileDescriptor& outfd,
                    const uint32_t& memory,
                    DBus::Error* error);
};

}  // namespace debugd

#endif  // DEBUGD_SRC_MEMORY_TOOL_H_
