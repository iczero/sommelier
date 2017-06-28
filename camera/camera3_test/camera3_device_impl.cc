// Copyright 2017 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "camera3_test/camera3_device_impl.h"

#include <utility>

#include <sync/sync.h>

namespace camera3_test {

static void ExpectKeyValueGreaterThanI64(const camera_metadata_t* settings,
                                         int32_t key,
                                         const char* key_name,
                                         int64_t value) {
  camera_metadata_ro_entry_t entry;
  ASSERT_EQ(0, find_camera_metadata_ro_entry(settings, key, &entry))
      << "Cannot find the metadata " << key_name;
  ASSERT_GT(entry.data.i64[0], value) << "Wrong value of metadata " << key_name;
}
#define EXPECT_KEY_VALUE_GT_I64(settings, key, value) \
  ExpectKeyValueGreaterThanI64(settings, key, #key, value)

Camera3DeviceImpl::Camera3DeviceImpl(int cam_id)
    : cam_id_(cam_id),
      hal_thread_(GetThreadName(cam_id).c_str()),
      initialized_(false),
      cam_device_(NULL),
      cam_stream_idx_(0),
      gralloc_(Camera3TestGralloc::GetInstance()),
      request_frame_number_(kInitialFrameNumber),
      result_frame_number_(kInitialFrameNumber) {
  thread_checker_.DetachFromThread();
}

int Camera3DeviceImpl::Initialize(Camera3Module* cam_module) {
  VLOGF_ENTER();
  if (!cam_module || !hal_thread_.Start()) {
    return -EINVAL;
  }
  int result = -EIO;
  hal_thread_.PostTaskSync(
      FROM_HERE, base::Bind(&Camera3DeviceImpl::InitializeOnThread,
                            base::Unretained(this), cam_module, &result));
  return result;
}

void Camera3DeviceImpl::Destroy() {
  VLOGF_ENTER();
  int result = -EIO;
  hal_thread_.PostTaskSync(FROM_HERE,
                           base::Bind(&Camera3DeviceImpl::DestroyOnThread,
                                      base::Unretained(this), &result));
  EXPECT_EQ(0, result) << "Camera device close failed";
  hal_thread_.Stop();
}

void Camera3DeviceImpl::RegisterProcessCaptureResultCallback(
    Camera3Device::ProcessCaptureResultCallback cb) {
  hal_thread_.PostTaskSync(
      FROM_HERE,
      base::Bind(
          &Camera3DeviceImpl::RegisterProcessCaptureResultCallbackOnThread,
          base::Unretained(this), cb));
}

void Camera3DeviceImpl::RegisterNotifyCallback(
    Camera3Device::NotifyCallback cb) {
  hal_thread_.PostTaskSync(
      FROM_HERE, base::Bind(&Camera3DeviceImpl::RegisterNotifyCallbackOnThread,
                            base::Unretained(this), cb));
}

void Camera3DeviceImpl::RegisterResultMetadataOutputBufferCallback(
    Camera3Device::ProcessResultMetadataOutputBuffersCallback cb) {
  hal_thread_.PostTaskSync(
      FROM_HERE,
      base::Bind(&Camera3DeviceImpl::
                     RegisterResultMetadataOutputBufferCallbackOnThread,
                 base::Unretained(this), cb));
}

void Camera3DeviceImpl::RegisterPartialMetadataCallback(
    Camera3Device::ProcessPartialMetadataCallback cb) {
  hal_thread_.PostTaskSync(
      FROM_HERE,
      base::Bind(&Camera3DeviceImpl::RegisterPartialMetadataCallbackOnThread,
                 base::Unretained(this), cb));
}

bool Camera3DeviceImpl::IsTemplateSupported(int32_t type) {
  bool result = false;
  hal_thread_.PostTaskSync(
      FROM_HERE, base::Bind(&Camera3DeviceImpl::IsTemplateSupportedOnThread,
                            base::Unretained(this), type, &result));
  return result;
}

const camera_metadata_t* Camera3DeviceImpl::ConstructDefaultRequestSettings(
    int type) {
  const camera_metadata_t* metadata = nullptr;
  hal_thread_.PostTaskSync(
      FROM_HERE,
      base::Bind(&Camera3DeviceImpl::ConstructDefaultRequestSettingsOnThread,
                 base::Unretained(this), type, &metadata));
  return metadata;
}

void Camera3DeviceImpl::AddOutputStream(int format, int width, int height) {
  VLOGF_ENTER();
  hal_thread_.PostTaskSync(
      FROM_HERE, base::Bind(&Camera3DeviceImpl::AddOutputStreamOnThread,
                            base::Unretained(this), format, width, height));
}

int Camera3DeviceImpl::ConfigureStreams(
    std::vector<const camera3_stream_t*>* streams) {
  VLOGF_ENTER();
  int32_t result = -EIO;
  hal_thread_.PostTaskSync(
      FROM_HERE, base::Bind(&Camera3DeviceImpl::ConfigureStreamsOnThread,
                            base::Unretained(this), streams, &result));
  return result;
}

int Camera3DeviceImpl::AllocateOutputStreamBuffers(
    std::vector<camera3_stream_buffer_t>* output_buffers) {
  VLOGF_ENTER();
  int32_t result = -EIO;
  hal_thread_.PostTaskSync(
      FROM_HERE,
      base::Bind(&Camera3DeviceImpl::AllocateOutputStreamBuffersOnThread,
                 base::Unretained(this), output_buffers, &result));
  return result;
}

int Camera3DeviceImpl::AllocateOutputBuffersByStreams(
    const std::vector<const camera3_stream_t*>& streams,
    std::vector<camera3_stream_buffer_t>* output_buffers) {
  VLOGF_ENTER();
  int32_t result = -EIO;
  hal_thread_.PostTaskSync(
      FROM_HERE,
      base::Bind(&Camera3DeviceImpl::AllocateOutputBuffersByStreamsOnThread,
                 base::Unretained(this), &streams, output_buffers, &result));
  return result;
}

int Camera3DeviceImpl::RegisterOutputBuffer(
    const camera3_stream_t& stream,
    BufferHandleUniquePtr unique_buffer) {
  VLOGF_ENTER();
  int32_t result = -EIO;
  hal_thread_.PostTaskSync(
      FROM_HERE, base::Bind(&Camera3DeviceImpl::RegisterOutputBufferOnThread,
                            base::Unretained(this), &stream,
                            base::Passed(&unique_buffer), &result));
  return result;
}

int Camera3DeviceImpl::ProcessCaptureRequest(
    camera3_capture_request_t* capture_request) {
  VLOGF_ENTER();
  int32_t result = -EIO;
  hal_thread_.PostTaskSync(
      FROM_HERE, base::Bind(&Camera3DeviceImpl::ProcessCaptureRequestOnThread,
                            base::Unretained(this), capture_request, &result));
  return result;
}

int Camera3DeviceImpl::WaitShutter(const struct timespec& timeout) {
  if (!initialized_) {
    return -ENODEV;
  }
  if (!process_capture_result_cb_.is_null()) {
    LOG(ERROR) << "Test has registered its own process_capture_result callback "
                  "function and thus must provide its own "
               << __func__;
    return -EINVAL;
  }
  return sem_timedwait(&shutter_sem_, &timeout);
}

int Camera3DeviceImpl::WaitCaptureResult(const struct timespec& timeout) {
  if (!initialized_) {
    return -ENODEV;
  }
  if (!process_capture_result_cb_.is_null()) {
    LOG(ERROR) << "Test has registered its own process_capture_result callback "
                  "function and thus must provide its own "
               << __func__;
    return -EINVAL;
  }
  return sem_timedwait(&capture_result_sem_, &timeout);
}

int Camera3DeviceImpl::Flush() {
  DCHECK(cam_device_);
  VLOGF_ENTER();
  return cam_device_->ops->flush(cam_device_);
}

void Camera3DeviceImpl::InitializeOnThread(Camera3Module* cam_module,
                                           int* result) {
  DCHECK(thread_checker_.CalledOnValidThread());
  if (initialized_) {
    LOG(ERROR) << "Device " << cam_id_ << " is already initialized";
    *result = -EINVAL;
    return;
  }
  // Open camera device
  *result = -ENODEV;
  cam_device_ = cam_module->OpenDevice(cam_id_);
  ASSERT_NE(nullptr, cam_device_) << "Failed to open device " << cam_id_;

  *result = -EINVAL;
  ASSERT_GE(((const hw_device_t*)cam_device_)->version,
            (uint16_t)HARDWARE_MODULE_API_VERSION(3, 3))
      << "The device must support at least HALv3.3";

  ASSERT_NE(nullptr, gralloc_) << "Gralloc initialization fails";

  camera_info cam_info;
  ASSERT_EQ(0, cam_module->GetCameraInfo(cam_id_, &cam_info));
  static_info_.reset(new Camera3Device::StaticInfo(cam_info));
  ASSERT_TRUE(static_info_->IsHardwareLevelAtLeastLimited())
      << "The device must support at least LIMITED hardware level";

  // Initialize camera device
  Camera3DeviceImpl::notify = Camera3DeviceImpl::NotifyForwarder;
  Camera3DeviceImpl::process_capture_result =
      Camera3DeviceImpl::ProcessCaptureResultForwarder;
  *result = cam_device_->ops->initialize(cam_device_, this);
  ASSERT_EQ(0, *result) << "Camera device initialization fails";

  sem_init(&shutter_sem_, 0, 0);
  sem_init(&capture_result_sem_, 0, 0);
  initialized_ = true;
}

void Camera3DeviceImpl::RegisterProcessCaptureResultCallbackOnThread(
    Camera3Device::ProcessCaptureResultCallback cb) {
  DCHECK(thread_checker_.CalledOnValidThread());
  process_capture_result_cb_ = cb;
}

void Camera3DeviceImpl::RegisterNotifyCallbackOnThread(
    Camera3Device::NotifyCallback cb) {
  DCHECK(thread_checker_.CalledOnValidThread());
  notify_cb_ = cb;
}

void Camera3DeviceImpl::RegisterResultMetadataOutputBufferCallbackOnThread(
    Camera3Device::ProcessResultMetadataOutputBuffersCallback cb) {
  DCHECK(thread_checker_.CalledOnValidThread());
  process_result_metadata_output_buffers_cb_ = cb;
}

void Camera3DeviceImpl::RegisterPartialMetadataCallbackOnThread(
    Camera3Device::ProcessPartialMetadataCallback cb) {
  DCHECK(thread_checker_.CalledOnValidThread());
  process_partial_metadata_cb_ = cb;
}

void Camera3DeviceImpl::IsTemplateSupportedOnThread(int32_t type,
                                                    bool* result) {
  if (initialized_) {
    *result =
        (type != CAMERA3_TEMPLATE_MANUAL ||
         static_info_->IsCapabilitySupported(
             ANDROID_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR)) &&
        (type != CAMERA3_TEMPLATE_ZERO_SHUTTER_LAG ||
         static_info_->IsCapabilitySupported(
             ANDROID_REQUEST_AVAILABLE_CAPABILITIES_PRIVATE_REPROCESSING));
  }
}

void Camera3DeviceImpl::ConstructDefaultRequestSettingsOnThread(
    int type,
    const camera_metadata_t** result) {
  DCHECK(thread_checker_.CalledOnValidThread());
  if (initialized_) {
    *result =
        cam_device_->ops->construct_default_request_settings(cam_device_, type);
  }
}

void Camera3DeviceImpl::AddOutputStreamOnThread(int format,
                                                int width,
                                                int height) {
  DCHECK(thread_checker_.CalledOnValidThread());
  if (initialized_) {
    // Push to the bin that is not used currently
    camera3_stream_t stream = {};
    stream.stream_type = CAMERA3_STREAM_OUTPUT;
    stream.width = width;
    stream.height = height;
    stream.format = format;
    cam_stream_[!cam_stream_idx_].push_back(stream);
  }
}

void Camera3DeviceImpl::ConfigureStreamsOnThread(
    std::vector<const camera3_stream_t*>* streams,
    int* result) {
  DCHECK(thread_checker_.CalledOnValidThread());
  *result = -EINVAL;
  if (!initialized_) {
    *result = -ENODEV;
    return;
  }

  // Check whether there are streams
  if (cam_stream_[!cam_stream_idx_].size() == 0) {
    return;
  }

  // Prepare stream configuration
  std::vector<camera3_stream_t*> cam_streams;
  camera3_stream_configuration_t cam_stream_config = {};
  for (size_t i = 0; i < cam_stream_[!cam_stream_idx_].size(); i++) {
    cam_streams.push_back(&cam_stream_[!cam_stream_idx_][i]);
  }
  cam_stream_config.num_streams = cam_streams.size();
  cam_stream_config.streams = cam_streams.data();
  cam_stream_config.operation_mode = CAMERA3_STREAM_CONFIGURATION_NORMAL_MODE;

  // Configure streams now
  *result =
      cam_device_->ops->configure_streams(cam_device_, &cam_stream_config);
  if (*result == 0) {
    for (const auto& it : cam_stream_[!cam_stream_idx_]) {
      if (it.max_buffers == 0) {
        LOGF(ERROR) << "Max number of buffers equal to zero is invalid";
        *result = -EINVAL;
        return;
      }
    }
  }

  // Swap to the other bin
  cam_stream_[cam_stream_idx_].clear();
  cam_stream_idx_ = !cam_stream_idx_;

  if (*result == 0 && streams) {
    streams->clear();
    for (auto const& it : cam_stream_[cam_stream_idx_]) {
      streams->push_back(&it);
    }
  }
}

void Camera3DeviceImpl::AllocateOutputStreamBuffersOnThread(
    std::vector<camera3_stream_buffer_t>* output_buffers,
    int32_t* result) {
  DCHECK(thread_checker_.CalledOnValidThread());
  std::vector<const camera3_stream_t*> streams;
  for (const auto& it : cam_stream_[cam_stream_idx_]) {
    streams.push_back(&it);
  }
  AllocateOutputBuffersByStreamsOnThread(&streams, output_buffers, result);
}

void Camera3DeviceImpl::AllocateOutputBuffersByStreamsOnThread(
    const std::vector<const camera3_stream_t*>* streams,
    std::vector<camera3_stream_buffer_t>* output_buffers,
    int32_t* result) {
  DCHECK(thread_checker_.CalledOnValidThread());
  *result = -EINVAL;
  if (!initialized_) {
    *result = -ENODEV;
    return;
  }

  if (!output_buffers || streams->empty()) {
    return;
  }
  int32_t jpeg_max_size = 0;
  if (std::find_if(streams->begin(), streams->end(),
                   [](const camera3_stream_t* stream) {
                     return stream->format == HAL_PIXEL_FORMAT_BLOB;
                   }) != streams->end()) {
    jpeg_max_size = static_info_->GetJpegMaxSize();
    if (jpeg_max_size <= 0) {
      return;
    }
  }

  for (const auto& it : *streams) {
    BufferHandleUniquePtr buffer = gralloc_->Allocate(
        (it->format == HAL_PIXEL_FORMAT_BLOB) ? jpeg_max_size : it->width,
        (it->format == HAL_PIXEL_FORMAT_BLOB) ? 1 : it->height, it->format,
        GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_HW_CAMERA_WRITE);
    if (!buffer) {
      LOG(ERROR) << "Gralloc allocation fails";
      *result = -ENOMEM;
      return;
    }

    camera3_stream_buffer_t stream_buffer;
    camera3_stream_t* stream = const_cast<camera3_stream_t*>(it);

    stream_buffer = {.stream = stream,
                     .buffer = buffer.get(),
                     .status = CAMERA3_BUFFER_STATUS_OK,
                     .acquire_fence = -1,
                     .release_fence = -1};
    stream_buffer_map_[stream].push_back(std::move(buffer));
    output_buffers->push_back(stream_buffer);
  }
  *result = 0;
}

void Camera3DeviceImpl::RegisterOutputBufferOnThread(
    const camera3_stream_t* stream,
    BufferHandleUniquePtr unique_buffer,
    int32_t* result) {
  DCHECK(thread_checker_.CalledOnValidThread());
  VLOGF_ENTER();
  if (!initialized_) {
    *result = -ENODEV;
    return;
  }
  if (!unique_buffer) {
    *result = -EINVAL;
    return;
  }
  stream_buffer_map_[stream].emplace_back(std::move(unique_buffer));
  *result = 0;
}

void Camera3DeviceImpl::ProcessCaptureRequestOnThread(
    camera3_capture_request_t* request,
    int* result) {
  DCHECK(thread_checker_.CalledOnValidThread());
  VLOGF_ENTER();
  if (!initialized_) {
    *result = -ENODEV;
    return;
  }
  if (request) {
    capture_request_map_[request_frame_number_] = *request;
    capture_request_map_[request_frame_number_].frame_number =
        request_frame_number_;
  }
  *result = cam_device_->ops->process_capture_request(
      cam_device_,
      request ? &capture_request_map_[request_frame_number_] : request);
  if (*result == 0) {
    request->frame_number = request_frame_number_;
    request_frame_number_++;
  }
}

void Camera3DeviceImpl::ProcessCaptureResultOnThread(
    const camera3_capture_result* result) {
  DCHECK(thread_checker_.CalledOnValidThread());
  if (!process_capture_result_cb_.is_null()) {
    process_capture_result_cb_.Run(result);
    return;
  }
  ASSERT_NE(nullptr, result) << "Capture result is null";
  // At least one of metadata or output buffers or input buffer should be
  // returned
  ASSERT_TRUE((result->result != NULL) || (result->num_output_buffers != 0) ||
              (result->input_buffer != NULL))
      << "No result data provided by HAL for frame " << result->frame_number;
  if (result->num_output_buffers) {
    ASSERT_NE(nullptr, result->output_buffers)
        << "No output buffer is returned while " << result->num_output_buffers
        << " are expected";
  }
  ASSERT_NE(capture_request_map_.end(),
            capture_request_map_.find(result->frame_number))
      << "A result is received for nonexistent request (frame number "
      << result->frame_number << ")";

  // For HAL3.2 or above, If HAL doesn't support partial, it must always set
  // partial_result to 1 when metadata is included in this result.
  ASSERT_TRUE(UsePartialResult() || result->result == NULL ||
              result->partial_result == 1)
      << "Result is malformed: partial_result must be 1 if partial result is "
         "not supported";
  // If partial_result > 0, there should be metadata returned in this result;
  // otherwise, there should be none.
  ASSERT_TRUE((result->partial_result > 0) == (result->result != NULL));

  if (result->result) {
    ProcessPartialResult(*result);
    EXPECT_KEY_VALUE_GT_I64(result->result, ANDROID_SENSOR_TIMESTAMP, 0);
  }

  for (uint32_t i = 0; i < result->num_output_buffers; i++) {
    camera3_stream_buffer_t stream_buffer = result->output_buffers[i];
    ASSERT_NE(nullptr, stream_buffer.buffer)
        << "Capture result output buffer is null";
    // An error may be expected while flushing
    EXPECT_EQ(CAMERA3_BUFFER_STATUS_OK, stream_buffer.status)
        << "Capture result buffer status error";
    ASSERT_EQ(-1, stream_buffer.acquire_fence)
        << "Capture result buffer fence error";

    // TODO(hywu): check buffers for a given streams are returned in order
    if (stream_buffer.release_fence != -1) {
      ASSERT_EQ(0, sync_wait(stream_buffer.release_fence, 1000))
          << "Error waiting on buffer acquire fence";
      close(stream_buffer.release_fence);
    }
    capture_result_info_map_[result->frame_number].output_buffers_.push_back(
        result->output_buffers[i]);
  }

  capture_result_info_map_[result->frame_number].num_output_buffers_ +=
      result->num_output_buffers;
  ASSERT_LE(capture_result_info_map_[result->frame_number].num_output_buffers_,
            capture_request_map_[result->frame_number].num_output_buffers);
  if (capture_result_info_map_[result->frame_number].num_output_buffers_ ==
          capture_request_map_[result->frame_number].num_output_buffers &&
      capture_result_info_map_[result->frame_number].have_result_metadata_) {
    ASSERT_EQ(completed_request_set_.end(),
              completed_request_set_.find(result->frame_number))
        << "Multiple results are received for the same request";
    completed_request_set_.insert(result->frame_number);

    // Process all received metadata and output buffers
    std::vector<BufferHandleUniquePtr> unique_buffers;
    if (GetOutputStreamBufferHandles(
            capture_result_info_map_[result->frame_number].output_buffers_,
            &unique_buffers)) {
      ADD_FAILURE() << "Failed to get output buffers";
    }
    CameraMetadataUniquePtr final_metadata(
        capture_result_info_map_[result->frame_number].MergePartialMetadata());
    if (!process_result_metadata_output_buffers_cb_.is_null()) {
      process_result_metadata_output_buffers_cb_.Run(result->frame_number,
                                                     std::move(final_metadata),
                                                     std::move(unique_buffers));
    }
    std::vector<CameraMetadataUniquePtr> partial_metadata;
    partial_metadata.swap(
        capture_result_info_map_[result->frame_number].partial_metadata_);
    if (!process_partial_metadata_cb_.is_null()) {
      process_partial_metadata_cb_.Run(&partial_metadata);
    }

    ASSERT_EQ(result->frame_number, result_frame_number_)
        << "Capture result is out of order";
    result_frame_number_++;
    capture_request_map_.erase(result->frame_number);
    capture_result_info_map_.erase(result->frame_number);

    // Everything looks fine. Post it now.
    sem_post(&capture_result_sem_);
  }
}

void Camera3DeviceImpl::NotifyOnThread(const camera3_notify_msg* msg) {
  DCHECK(thread_checker_.CalledOnValidThread());
  if (!notify_cb_.is_null()) {
    notify_cb_.Run(msg);
    return;
  }
  EXPECT_EQ(CAMERA3_MSG_SHUTTER, msg->type)
      << "Shutter error = " << msg->message.error.error_code;

  if (msg->type == CAMERA3_MSG_SHUTTER) {
    sem_post(&shutter_sem_);
  }
}

void Camera3DeviceImpl::DestroyOnThread(int* result) {
  DCHECK(thread_checker_.CalledOnValidThread());
  *result = -EINVAL;
  if (!initialized_) {
    *result = -ENODEV;
    return;
  }
  ASSERT_NE(nullptr, cam_device_->common.close)
      << "Camera close() is not implemented";
  *result = cam_device_->common.close(&cam_device_->common);

  sem_destroy(&shutter_sem_);
  sem_destroy(&capture_result_sem_);
  initialized_ = false;
}

const Camera3Device::StaticInfo* Camera3DeviceImpl::GetStaticInfo() const {
  return static_info_.get();
}

void Camera3DeviceImpl::ProcessCaptureResultForwarder(
    const camera3_callback_ops* cb,
    const camera3_capture_result* result) {
  // Forward to callback of instance
  Camera3DeviceImpl* d =
      const_cast<Camera3DeviceImpl*>(static_cast<const Camera3DeviceImpl*>(cb));
  d->ProcessCaptureResult(result);
}

void Camera3DeviceImpl::NotifyForwarder(const camera3_callback_ops* cb,
                                        const camera3_notify_msg* msg) {
  // Forward to callback of instance
  Camera3DeviceImpl* d =
      const_cast<Camera3DeviceImpl*>(static_cast<const Camera3DeviceImpl*>(cb));
  d->Notify(msg);
}

void Camera3DeviceImpl::ProcessCaptureResult(
    const camera3_capture_result* result) {
  VLOGF_ENTER();
  hal_thread_.PostTaskSync(
      FROM_HERE, base::Bind(&Camera3DeviceImpl::ProcessCaptureResultOnThread,
                            base::Unretained(this), result));
}

void Camera3DeviceImpl::Notify(const camera3_notify_msg* msg) {
  hal_thread_.PostTaskSync(FROM_HERE,
                           base::Bind(&Camera3DeviceImpl::NotifyOnThread,
                                      base::Unretained(this), msg));
}

int Camera3DeviceImpl::GetOutputStreamBufferHandles(
    const std::vector<camera3_stream_buffer_t>& output_buffers,
    std::vector<BufferHandleUniquePtr>* unique_buffers) {
  DCHECK(thread_checker_.CalledOnValidThread());
  VLOGF_ENTER();

  for (const auto& output_buffer : output_buffers) {
    if (!output_buffer.buffer ||
        stream_buffer_map_.find(output_buffer.stream) ==
            stream_buffer_map_.end()) {
      LOG(ERROR) << "Failed to find configured stream or buffer is invalid";
      return -EINVAL;
    }

    auto stream_buffers = &stream_buffer_map_[output_buffer.stream];
    auto it = std::find_if(stream_buffers->begin(), stream_buffers->end(),
                           [=](const BufferHandleUniquePtr& buffer) {
                             return *buffer == *output_buffer.buffer;
                           });
    if (it != stream_buffers->end()) {
      unique_buffers->push_back(std::move(*it));
      stream_buffers->erase(it);
    } else {
      LOG(ERROR) << "Failed to find output buffer";
      return -EINVAL;
    }
  }
  return 0;
}

bool Camera3DeviceImpl::UsePartialResult() const {
  DCHECK(thread_checker_.CalledOnValidThread());
  return static_info_->GetPartialResultCount() > 1;
}

void Camera3DeviceImpl::ProcessPartialResult(
    const camera3_capture_result& result) {
  DCHECK(thread_checker_.CalledOnValidThread());
  // True if this partial result is the final one. If HAL does not use partial
  // result, the value is True by default.
  bool is_final_partial_result = !UsePartialResult();
  // Check if this result carries only partial metadata
  if (UsePartialResult() && result.result != NULL) {
    EXPECT_LE(result.partial_result, static_info_->GetPartialResultCount());
    EXPECT_GE(result.partial_result, 1);
    camera_metadata_ro_entry_t entry;
    if (find_camera_metadata_ro_entry(
            result.result, ANDROID_QUIRKS_PARTIAL_RESULT, &entry) == 0) {
      is_final_partial_result =
          (entry.data.i32[0] == ANDROID_QUIRKS_PARTIAL_RESULT_FINAL);
    }
  }

  // Did we get the (final) result metadata for this capture?
  if (result.result != NULL && is_final_partial_result) {
    EXPECT_FALSE(
        capture_result_info_map_[result.frame_number].have_result_metadata_)
        << "Called multiple times with final metadata";
    capture_result_info_map_[result.frame_number].have_result_metadata_ = true;
  }

  capture_result_info_map_[result.frame_number].AllocateAndCopyMetadata(
      *result.result);
}

Camera3DeviceImpl::CaptureResultInfo::CaptureResultInfo()
    : num_output_buffers_(0), have_result_metadata_(false) {
  thread_checker_.DetachFromThread();
}

void Camera3DeviceImpl::CaptureResultInfo::AllocateAndCopyMetadata(
    const camera_metadata_t& src) {
  DCHECK(thread_checker_.CalledOnValidThread());
  CameraMetadataUniquePtr metadata(allocate_copy_camera_metadata_checked(
      &src, get_camera_metadata_size(&src)));
  ASSERT_NE(nullptr, metadata.get()) << "Copying result partial metadata fails";
  partial_metadata_.push_back(std::move(metadata));
}

bool Camera3DeviceImpl::CaptureResultInfo::IsMetadataKeyAvailable(
    int32_t key) const {
  DCHECK(thread_checker_.CalledOnValidThread());
  return GetMetadataKeyEntry(key, nullptr);
}

int32_t Camera3DeviceImpl::CaptureResultInfo::GetMetadataKeyValue(
    int32_t key) const {
  DCHECK(thread_checker_.CalledOnValidThread());
  camera_metadata_ro_entry_t entry;
  return GetMetadataKeyEntry(key, &entry) ? entry.data.i32[0] : -EINVAL;
}

int64_t Camera3DeviceImpl::CaptureResultInfo::GetMetadataKeyValue64(
    int32_t key) const {
  DCHECK(thread_checker_.CalledOnValidThread());
  camera_metadata_ro_entry_t entry;
  return GetMetadataKeyEntry(key, &entry) ? entry.data.i64[0] : -EINVAL;
}

CameraMetadataUniquePtr
Camera3DeviceImpl::CaptureResultInfo::MergePartialMetadata() {
  DCHECK(thread_checker_.CalledOnValidThread());
  size_t entry_count = 0;
  size_t data_count = 0;
  for (const auto& it : partial_metadata_) {
    entry_count += get_camera_metadata_entry_count(it.get());
    data_count += get_camera_metadata_data_count(it.get());
  }
  camera_metadata_t* metadata =
      allocate_camera_metadata(entry_count, data_count);
  if (!metadata) {
    ADD_FAILURE() << "Can't allocate larger metadata buffer";
    return nullptr;
  }
  for (const auto& it : partial_metadata_) {
    append_camera_metadata(metadata, it.get());
  }
  return CameraMetadataUniquePtr(metadata);
}

bool Camera3DeviceImpl::CaptureResultInfo::GetMetadataKeyEntry(
    int32_t key,
    camera_metadata_ro_entry_t* entry) const {
  DCHECK(thread_checker_.CalledOnValidThread());
  camera_metadata_ro_entry_t local_entry;
  entry = entry ? entry : &local_entry;
  for (const auto& it : partial_metadata_) {
    if (find_camera_metadata_ro_entry(it.get(), key, entry) == 0) {
      return true;
    }
  }
  return false;
}

const std::string Camera3DeviceImpl::GetThreadName(int cam_id) {
  const size_t kThreadNameLength = 30;
  char thread_name[kThreadNameLength];
  snprintf(thread_name, kThreadNameLength, "Camera3 Test Device %d Thread",
           cam_id);
  return std::string(thread_name);
}

}  // namespace camera3_test