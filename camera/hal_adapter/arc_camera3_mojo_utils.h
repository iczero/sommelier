/*
 * Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef HAL_ADAPTER_ARC_CAMERA3_MOJO_UTILS_H_
#define HAL_ADAPTER_ARC_CAMERA3_MOJO_UTILS_H_

#include <map>
#include <memory>
#include <unordered_map>
#include <utility>

#include <base/bind.h>
#include <base/bind_helpers.h>
#include <base/threading/thread.h>
#include <base/threading/thread_checker.h>
#include <mojo/public/cpp/bindings/binding.h>
#include <mojo/public/cpp/system/data_pipe.h>

#include "arc/common.h"
#include "arc/future.h"
#include "hal_adapter/arc_camera3.mojom.h"
#include "hal_adapter/common_types.h"
#include "hardware/camera3.h"

namespace internal {

// Serialize / deserialize helper functions.

mojo::ScopedHandle WrapPlatformHandle(int handle);

int UnwrapPlatformHandle(mojo::ScopedHandle handle);

// SerializeStreamBuffer is used in CameraDeviceAdapter::ProcessCaptureResult to
// pass a result buffer handle to ARC++.  For the input / output buffers, we do
// not need to serialize the whole native handle but instead we can simply
// return their corresponding handle IDs.  When ARC++ receives the result it
// will restore using the handle ID the original buffer handles which were
// passed down when the frameworks called process_capture_request.
arc::mojom::Camera3StreamBufferPtr SerializeStreamBuffer(
    const camera3_stream_buffer_t* buffer,
    const UniqueStreams& streams,
    const std::unordered_map<uint64_t,
                             internal::ArcCameraBufferHandleUniquePtr>&
        buffer_handles);

int DeserializeStreamBuffer(
    const arc::mojom::Camera3StreamBufferPtr& ptr,
    const UniqueStreams& streams,
    const std::unordered_map<uint64_t,
                             internal::ArcCameraBufferHandleUniquePtr>&
        buffer_handles_,
    camera3_stream_buffer_t* buffer);

arc::mojom::CameraMetadataPtr SerializeCameraMetadata(
    const camera_metadata_t* metadata);

internal::CameraMetadataUniquePtr DeserializeCameraMetadata(
    const arc::mojom::CameraMetadataPtr& metadata);
// Template classes for Mojo IPC delegates

template <typename T>
class MojoInterfaceDelegate {
 public:
  explicit MojoInterfaceDelegate(mojo::InterfacePtrInfo<T> interface_ptr_info)
      : thread_("Delegate thread") {
    VLOGF_ENTER();
    if (!thread_.Start()) {
      LOGF(ERROR) << "Delegate thread failed to start";
      exit(-1);
    }
    thread_checker_.DetachFromThread();

    auto future = internal::Future<void>::Create(&relay_);
    thread_.task_runner()->PostTask(
        FROM_HERE,
        base::Bind(&MojoInterfaceDelegate<T>::BindOnThread,
                   base::Unretained(this), base::Passed(&interface_ptr_info),
                   internal::GetFutureCallback(future)));
    future->Wait();
  }

  ~MojoInterfaceDelegate() {
    VLOGF_ENTER();
    auto future = internal::Future<void>::Create(&relay_);
    thread_.task_runner()->PostTask(
        FROM_HERE,
        base::Bind(&MojoInterfaceDelegate<T>::ResetInterfacePtrOnThread,
                   base::Unretained(this),
                   internal::GetFutureCallback(future)));
    future->Wait();
    thread_.Stop();
  }

 protected:
  base::Thread thread_;

  base::ThreadChecker thread_checker_;

  mojo::InterfacePtr<T> interface_ptr_;

  CancellationRelay relay_;

 private:
  void BindOnThread(mojo::InterfacePtrInfo<T> interface_ptr_info,
                    const base::Callback<void()>& cb) {
    VLOGF_ENTER();
    DCHECK(thread_checker_.CalledOnValidThread());
    interface_ptr_ = mojo::MakeProxy(std::move(interface_ptr_info));
    if (!interface_ptr_.is_bound()) {
      LOGF(ERROR) << "Failed to bind interface_ptr_";
      exit(-1);
    }
    interface_ptr_.set_connection_error_handler(
        base::Bind(&MojoInterfaceDelegate<T>::OnIpcConnectionLostOnThread,
                   base::Unretained(this)));
    interface_ptr_.QueryVersion(
        base::Bind(&MojoInterfaceDelegate<T>::OnQueryVersionOnThread,
                   base::Unretained(this)));
    cb.Run();
  }

  void OnIpcConnectionLostOnThread() {
    VLOGF_ENTER();
    DCHECK(thread_checker_.CalledOnValidThread());
    LOGF(INFO) << "Mojo interface connection lost";
    relay_.CancelAllFutures();
    interface_ptr_.reset();
  }

  void OnQueryVersionOnThread(uint32_t version) {
    VLOGF_ENTER();
    DCHECK(thread_checker_.CalledOnValidThread());
    LOGF(INFO) << "Bridge ready (version=" << version << ")";
  }

  void ResetInterfacePtrOnThread(const base::Callback<void()>& cb) {
    VLOGF_ENTER();
    DCHECK(thread_checker_.CalledOnValidThread());
    interface_ptr_.reset();
    cb.Run();
  }

  DISALLOW_COPY_AND_ASSIGN(MojoInterfaceDelegate);
};

template <typename T>
class MojoBindingDelegate : public T {
 public:
  explicit MojoBindingDelegate(base::Closure quit_cb = base::Closure())
      : thread_("Delegate thread"), binding_(this) {
    VLOGF_ENTER();
    if (!thread_.Start()) {
      LOGF(ERROR) << "Delegate thread failed to start";
      exit(-1);
    }
    thread_checker_.DetachFromThread();
    quit_cb_ = quit_cb;
  }

  ~MojoBindingDelegate() {
    VLOGF_ENTER();
    auto future = internal::Future<void>::Create(&relay_);
    thread_.task_runner()->PostTask(
        FROM_HERE, base::Bind(&MojoBindingDelegate<T>::CloseBindingOnThread,
                              base::Unretained(this),
                              internal::GetFutureCallback(future)));
    future->Wait();
    thread_.Stop();
  }

  mojo::InterfacePtr<T> CreateInterfacePtr() {
    VLOGF_ENTER();
    auto future = internal::Future<mojo::InterfacePtr<T>>::Create(&relay_);
    thread_.task_runner()->PostTask(
        FROM_HERE,
        base::Bind(&MojoBindingDelegate<T>::CreateInterfacePtrOnThread,
                   base::Unretained(this),
                   internal::GetFutureCallback(future)));
    return future->Get();
  }

  void Bind(mojo::ScopedMessagePipeHandle handle) {
    VLOGF_ENTER();
    auto future = internal::Future<void>::Create(&relay_);
    thread_.task_runner()->PostTask(
        FROM_HERE, base::Bind(&MojoBindingDelegate<T>::BindOnThread,
                              base::Unretained(this), base::Passed(&handle),
                              internal::GetFutureCallback(future)));
    future->Wait();
  }

 protected:
  base::ThreadChecker thread_checker_;

 private:
  void CloseBindingOnThread(const base::Callback<void()>& cb) {
    VLOGF_ENTER();
    DCHECK(thread_checker_.CalledOnValidThread());
    if (binding_.is_bound()) {
      binding_.Close();
    }
    if (!quit_cb_.is_null()) {
      quit_cb_.Run();
    }
    cb.Run();
  }

  void CreateInterfacePtrOnThread(
      const base::Callback<void(mojo::InterfacePtr<T>)>& cb) {
    // Call CreateInterfacePtrAndBind() on thread_ to serve the RPC.
    VLOGF_ENTER();
    DCHECK(thread_checker_.CalledOnValidThread());
    mojo::InterfacePtr<T> interfacePtr = binding_.CreateInterfacePtrAndBind();
    binding_.set_connection_error_handler(
        base::Bind(&MojoBindingDelegate<T>::OnChannelClosedOnThread,
                   base::Unretained(this)));
    cb.Run(std::move(interfacePtr));
  }

  void BindOnThread(mojo::ScopedMessagePipeHandle handle,
                    const base::Callback<void()>& cb) {
    VLOGF_ENTER();
    DCHECK(thread_checker_.CalledOnValidThread());
    binding_.Bind(std::move(handle));
    binding_.set_connection_error_handler(
        base::Bind(&MojoBindingDelegate<T>::OnChannelClosedOnThread,
                   base::Unretained(this)));
    cb.Run();
  }

  void OnChannelClosedOnThread() {
    VLOGF_ENTER();
    DCHECK(thread_checker_.CalledOnValidThread());
    LOGF(INFO) << "Mojo binding channel closed";
    if (binding_.is_bound()) {
      relay_.CancelAllFutures();
      binding_.Close();
    }
    if (!quit_cb_.is_null()) {
      quit_cb_.Run();
    }
  }

  base::Closure quit_cb_;

  base::Thread thread_;

  mojo::Binding<T> binding_;

  CancellationRelay relay_;

  DISALLOW_COPY_AND_ASSIGN(MojoBindingDelegate);
};

}  // namespace internal

#endif  // HAL_ADAPTER_ARC_CAMERA3_MOJO_UTILS_H_