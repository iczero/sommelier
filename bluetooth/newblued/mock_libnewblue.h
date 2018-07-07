// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BLUETOOTH_NEWBLUED_MOCK_LIBNEWBLUE_H_
#define BLUETOOTH_NEWBLUED_MOCK_LIBNEWBLUE_H_

#include <gmock/gmock.h>

#include "bluetooth/newblued/libnewblue.h"

namespace bluetooth {

class MockLibNewblue : public LibNewblue {
 public:
  using LibNewblue::LibNewblue;

  // att.h
  MOCK_METHOD0(AttInit, bool());
  MOCK_METHOD0(AttDeinit, void());

  // gatt.h
  MOCK_METHOD0(GattProfileInit, bool());
  MOCK_METHOD0(GattProfileDeinit, void());

  // gatt-builtin.h
  MOCK_METHOD0(GattBuiltinInit, bool());
  MOCK_METHOD0(GattBuiltinDeinit, void());

  // hci.h
  MOCK_METHOD3(HciUp, bool(const uint8_t*, hciReadyForUpCbk, void*));
  MOCK_METHOD0(HciDown, void());
  MOCK_METHOD0(HciIsUp, bool());
  MOCK_METHOD4(HciDiscoverLeStart,
               uniq_t(hciDeviceDiscoveredLeCbk, void*, bool, bool));
  MOCK_METHOD1(HciDiscoverLeStop, bool(uniq_t));

  // l2cap.h
  MOCK_METHOD0(L2cInit, int());
  MOCK_METHOD0(L2cDeinit, void());

  // sm.h
  MOCK_METHOD1(SmInit, bool(uint8_t));
  MOCK_METHOD0(SmDeinit, void());
};

}  // namespace bluetooth

#endif  // BLUETOOTH_NEWBLUED_MOCK_LIBNEWBLUE_H_