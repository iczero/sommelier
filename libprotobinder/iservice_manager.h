// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIBPROTOBINDER_ISERVICE_MANAGER_H_
#define LIBPROTOBINDER_ISERVICE_MANAGER_H_

#include <stdint.h>

#include "libprotobinder/ibinder.h"
#include "libprotobinder/iinterface.h"

#define BINDER_EXPORT __attribute__((visibility("default")))

namespace protobinder {

class BINDER_EXPORT IServiceManager : public IInterface {
 public:
  DECLARE_META_INTERFACE(ServiceManager)

  virtual int AddService(const char* name, IBinder* binder) = 0;
  virtual IBinder* GetService(const char* name) = 0;

  enum {
    GET_SERVICE_TRANSACTION = IBinder::FIRST_CALL_TRANSACTION,
    CHECK_SERVICE_TRANSACTION,
    ADD_SERVICE_TRANSACTION,
    LIST_SERVICES_TRANSACTION,
  };
};

class BINDER_EXPORT IServiceManagerHostInterface
    : BinderHostInterface<IServiceManager> {
 public:
  virtual int OnTransact(uint32_t code,
                         const Parcel& data,
                         Parcel* reply,
                         uint32_t flags);
};

BINDER_EXPORT IServiceManager* GetServiceManager();

}  // namespace protobinder

#endif  // LIBPROTOBINDER_ISERVICE_MANAGER_H_
