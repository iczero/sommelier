//
// Copyright 2017 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "shill/vpn/arc_vpn_driver.h"

#include <base/bind.h>
#include <gtest/gtest.h>

#include "shill/mock_adaptors.h"
#include "shill/mock_device_info.h"
#include "shill/mock_event_dispatcher.h"
#include "shill/mock_manager.h"
#include "shill/mock_metrics.h"
#include "shill/mock_service.h"
#include "shill/mock_store.h"
#include "shill/mock_virtual_device.h"
#include "shill/nice_mock_control.h"
#include "shill/vpn/mock_vpn_provider.h"
#include "shill/vpn/mock_vpn_service.h"

using testing::_;
using testing::Mock;
using testing::NiceMock;
using testing::Return;
using testing::SetArgumentPointee;

namespace shill {

namespace {

const char kInterfaceName[] = "arcbr0";
const int kInterfaceIndex = 123;
const char kStorageId[] = "dummystorage";

MATCHER(ChromeEnabledIPConfig, "") {
  IPConfig::Properties ip_properties = arg;
  return ip_properties.blackhole_ipv6 == true &&
         ip_properties.default_route == false &&
         !ip_properties.allowed_uids.empty();
}

MATCHER(ChromeDisabledIPConfig, "") {
  IPConfig::Properties ip_properties = arg;
  return ip_properties.blackhole_ipv6 == false;
}

}  // namespace

class ArcVpnDriverTest : public testing::Test {
 public:
  ArcVpnDriverTest()
      : device_info_(&control_, &dispatcher_, &metrics_, &manager_),
        metrics_(&dispatcher_),
        manager_(&control_, &dispatcher_, &metrics_),
        device_(new MockVirtualDevice(&control_,
                                      &dispatcher_,
                                      &metrics_,
                                      &manager_,
                                      kInterfaceName,
                                      kInterfaceIndex,
                                      Technology::kVPN)),
        store_(),
        driver_(new ArcVpnDriver(
            &control_, &dispatcher_, &metrics_, &manager_, &device_info_)),
        service_(new MockVPNService(
            &control_, &dispatcher_, &metrics_, &manager_, driver_)) {}

  virtual ~ArcVpnDriverTest() {}

  virtual void SetUp() {
    MockVPNProvider* provider = new MockVPNProvider;
    provider->allowed_uids_.push_back(1000);
    provider->arc_device_ = device_;
    manager_.vpn_provider_.reset(provider);
    manager_.UpdateProviderMapping();
  }

  virtual void TearDown() {
    manager_.vpn_provider_->arc_device_ = nullptr;
    manager_.vpn_provider_.reset();
    driver_->device_ = nullptr;
    driver_->service_ = nullptr;
    device_ = nullptr;
  }

  void LoadPropertiesFromStore(bool tunnel_chrome) {
    const std::string kProviderHostValue = "arcvpn";
    const std::string kProviderTypeValue = "arcvpn";

    EXPECT_CALL(store_, GetString(kStorageId, kProviderHostProperty, _))
        .WillOnce(
            DoAll(SetArgumentPointee<2>(kProviderHostValue), Return(true)));
    EXPECT_CALL(store_, GetString(kStorageId, kProviderTypeProperty, _))
        .WillOnce(
            DoAll(SetArgumentPointee<2>(kProviderTypeValue), Return(true)));
    EXPECT_CALL(store_, GetString(kStorageId, kArcVpnTunnelChromeProperty, _))
        .WillOnce(DoAll(SetArgumentPointee<2>(tunnel_chrome ? "true" : "false"),
                        Return(true)));
    driver_->Load(&store_, kStorageId);
  }

 protected:
  NiceMockControl control_;
  NiceMock<MockDeviceInfo> device_info_;
  MockEventDispatcher dispatcher_;
  MockMetrics metrics_;
  MockManager manager_;
  scoped_refptr<MockVirtualDevice> device_;
  MockStore store_;
  ArcVpnDriver* driver_;  // Owned by |service_|
  scoped_refptr<MockVPNService> service_;
};

TEST_F(ArcVpnDriverTest, ConnectAndDisconnect) {
  LoadPropertiesFromStore(true);

  EXPECT_CALL(*service_, SetState(Service::kStateConnected)).Times(1);
  EXPECT_CALL(*service_, SetState(Service::kStateOnline)).Times(1);

  EXPECT_CALL(*device_, SetEnabled(true));
  EXPECT_CALL(*device_, UpdateIPConfig(ChromeEnabledIPConfig()));

  Error error;
  driver_->Connect(service_, &error);
  EXPECT_TRUE(error.IsSuccess());

  EXPECT_CALL(*device_, SetEnabled(false));
  EXPECT_CALL(*device_, DropConnection());
  EXPECT_CALL(*service_, SetState(Service::kStateIdle));
  driver_->Disconnect();
}

TEST_F(ArcVpnDriverTest, ChromeTrafficDisabled) {
  LoadPropertiesFromStore(false);

  EXPECT_CALL(*service_, SetState(Service::kStateConnected)).Times(1);
  EXPECT_CALL(*service_, SetState(Service::kStateOnline)).Times(1);

  EXPECT_CALL(*device_, SetEnabled(true));
  EXPECT_CALL(*device_, UpdateIPConfig(ChromeDisabledIPConfig()));

  Error error;
  driver_->Connect(service_, &error);
  EXPECT_TRUE(error.IsSuccess());
}

}  // namespace shill