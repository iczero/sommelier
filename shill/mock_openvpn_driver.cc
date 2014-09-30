// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/mock_openvpn_driver.h"

namespace shill {

MockOpenVPNDriver::MockOpenVPNDriver()
    : OpenVPNDriver(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) {}

MockOpenVPNDriver::~MockOpenVPNDriver() {}

}  // namespace shill
