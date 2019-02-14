// Copyright 2017 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ARC_NETWORK_SUBNET_POOL_H_
#define ARC_NETWORK_SUBNET_POOL_H_

#include <stdint.h>

#include <bitset>
#include <memory>

#include <base/callback.h>
#include <base/macros.h>
#include <base/memory/weak_ptr.h>
#include <brillo/brillo_export.h>

#include "arc/network/subnet.h"

namespace arc_networkd {

// Manages IPv4 subnets that can be assigned to virtual machines and containers.
// These use non-publicly routable addresses in the range 100.115.92.0/24.
class BRILLO_EXPORT SubnetPool {
 public:
  SubnetPool() = default;
  ~SubnetPool();

  // Allocates and returns a new VM Subnet in the range 100.115.92.0/24. Returns
  // nullptr if no subnets are available.
  std::unique_ptr<Subnet> AllocateVM();

  // Allocates and returns a new Container Subnet in the range 100.115.92.0/24.
  // Returns nullptr if no subnets are available.
  std::unique_ptr<Subnet> AllocateContainer();

 private:
  // Called by Subnets on destruction to free a given subnet.
  void ReleaseVM(size_t index);

  // Called by Subnets on destruction to free a given subnet.
  void ReleaseContainer(size_t index);

  // There are 26 /30 subnets.
  std::bitset<26> vm_subnets_;

  // There are 4 /28 subnets.
  std::bitset<4> container_subnets_;

  base::WeakPtrFactory<SubnetPool> weak_ptr_factory_{this};
  DISALLOW_COPY_AND_ASSIGN(SubnetPool);
};

}  // namespace arc_networkd

#endif  // ARC_NETWORK_SUBNET_POOL_H_