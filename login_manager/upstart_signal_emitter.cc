// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "login_manager/upstart_signal_emitter.h"

#include <string>
#include <vector>

#include <base/logging.h>
#include <dbus/message.h>
#include <dbus/object_proxy.h>

namespace login_manager {

namespace {

constexpr char kInterface[] = "com.ubuntu.Upstart0_6";
constexpr char kMethodName[] = "EmitEvent";

}  // namespace

constexpr char UpstartSignalEmitter::kServiceName[] = "com.ubuntu.Upstart";
constexpr char UpstartSignalEmitter::kPath[] = "/com/ubuntu/Upstart";

UpstartSignalEmitter::UpstartSignalEmitter(dbus::ObjectProxy* proxy)
    : upstart_dbus_proxy_(proxy) {
}

UpstartSignalEmitter::~UpstartSignalEmitter() = default;

std::unique_ptr<dbus::Response> UpstartSignalEmitter::TriggerImpulse(
      const std::string& name,
      const std::vector<std::string>& args_keyvals,
      TriggerMode mode) {
  DLOG(INFO) << "Emitting " << name << " Upstart signal";

  dbus::MethodCall method_call(kInterface, kMethodName);
  dbus::MessageWriter writer(&method_call);
  writer.AppendString(name);
  writer.AppendArrayOfStrings(args_keyvals);
  writer.AppendBool(true);

  switch (mode) {
    case TriggerMode::SYNC:
      return upstart_dbus_proxy_->CallMethodAndBlock(
          &method_call, dbus::ObjectProxy::TIMEOUT_USE_DEFAULT);
    case TriggerMode::ASYNC:
      upstart_dbus_proxy_->CallMethod(
          &method_call, dbus::ObjectProxy::TIMEOUT_USE_DEFAULT,
          dbus::ObjectProxy::EmptyResponseCallback());
      return nullptr;
  }
  NOTREACHED() << "Invalid trigger mode " << mode;
  return nullptr;
}

}  // namespace login_manager
