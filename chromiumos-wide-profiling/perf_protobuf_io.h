// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERF_PROTOBUF_IO_H_
#define PERF_PROTOBUF_IO_H_

#include <string>
#include "perf_data.pb.h"

bool WriteProtobufToFile(const PerfDataProto & perf_data_proto,
                         const std::string & filename);

bool ReadProtobufFromFile(PerfDataProto * perf_data_proto,
                          const std::string & filename);


#endif /*PERF_PROTOBUF_IO_H_*/
