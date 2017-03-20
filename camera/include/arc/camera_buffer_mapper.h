/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef INCLUDE_ARC_CAMERA_BUFFER_MAPPER_H_
#define INCLUDE_ARC_CAMERA_BUFFER_MAPPER_H_

#include <memory>
#include <unordered_map>
#include <utility>

#include <base/synchronization/lock.h>
#include <gbm.h>

#define EXPORTED __attribute__((__visibility__("default")))

struct native_handle;
typedef const native_handle* buffer_handle_t;
struct android_ycbcr;

namespace arc {

namespace tests {

class CameraBufferMapperTest;

}  // namespace tests

// The enum definition here should match |Camera3DeviceOps::BufferType| in
// hal_adapter/arc_camera3.mojom.
enum BufferType {
  GRALLOC = 0,
  SHM = 1,
};

struct GbmDeviceDeleter {
  inline void operator()(struct gbm_device* device) {
    if (device) {
      close(gbm_device_get_fd(device));
      gbm_device_destroy(device);
    }
  }
};

typedef std::unique_ptr<struct gbm_device, struct GbmDeviceDeleter>
    GbmDeviceUniquePtr;

struct GbmBoInfo {
  struct gbm_bo* bo;
  uint32_t usage;

  GbmBoInfo() : bo(nullptr), usage(0) {}
};

struct GbmBoInfoDeleter {
  inline void operator()(struct GbmBoInfo* info) {
    if (info->bo) {
      gbm_bo_destroy(info->bo);
    }
  }
};

typedef std::unique_ptr<struct GbmBoInfo, struct GbmBoInfoDeleter>
    GbmBoInfoUniquePtr;
typedef std::unordered_map<buffer_handle_t, GbmBoInfoUniquePtr> GbmBoCache;

struct MappedBufferInfo {
  enum BufferType type;
  // The gbm_bo associated with the imported buffer (for gralloc buffer only).
  struct gbm_bo* bo;
  // The per-bo data returned by gbm_bo_map() (for gralloc buffer only).
  void* map_data;
  // For refcounting.
  uint32_t usage;

  MappedBufferInfo() : bo(nullptr), map_data(nullptr), usage(0) {}
};

struct MappedBufferInfoDeleter {
  inline void operator()(struct MappedBufferInfo* info) {
    if (info->type == GRALLOC) {
      // Unmap the bo once for each active usage.
      while (info->usage) {
        gbm_bo_unmap(info->bo, info->map_data);
        --info->usage;
      }
    } else if (info->type == SHM) {
      // TODO(jcliang): Implement deleter for shared memory buffer.
      return;
    }
  }
};

typedef std::unique_ptr<struct MappedBufferInfo, struct MappedBufferInfoDeleter>
    MappedBufferInfoUniquePtr;
typedef std::pair<buffer_handle_t, uint32_t> MappedBufferInfoKeyType;

struct MappedBufferInfoKeyHash {
  size_t operator()(const MappedBufferInfoKeyType& key) const {
    // The key is (buffer_handle_t pointer, plane number).  Plane number is less
    // than 4, so shifting the pointer value left by 8 and filling the lowest
    // byte with the plane number gives us a unique value to represent a key.
    return (reinterpret_cast<size_t>(key.first) << 8 | key.second);
  }
};

typedef std::unordered_map<MappedBufferInfoKeyType,
                           MappedBufferInfoUniquePtr,
                           struct MappedBufferInfoKeyHash>
    MappedBufferInfoCache;

// Generic camera buffer mapper.  The class is for a camera HAL to map and unmap
// the buffer handles received in camera3_stream_buffer_t.
//
// The class is thread-safe.
//
// Example usage:
//
//  #include <camera_buffer_mapper.h>
//  CameraBufferMapper* mapper = CameraBufferMapper::GetInstance();
//  if (!mapper) {
//    /* Error handling */
//  }
//  mapper->Register(buffer_handle);
//  void* addr;
//  mapper->Lock(buffer_handle, ..., &addr);
//  /* Access the buffer mapped to |addr| */
//  mapper->Unlock(buffer_handle);
//  mapper->Deregister(buffer_handle);

class EXPORTED CameraBufferMapper {
 public:
  // Gets the singleton instance.  Returns nullptr if any error occurrs during
  // instance creation.
  static CameraBufferMapper* GetInstance();

  // This method is analogous to the register() function in Android gralloc
  // module.  This method needs to be called before |buffer| can be mapped.
  //
  // Args:
  //    |buffer|: The buffer handle to register.
  //
  // Returns:
  //    0 on success; corresponding error code on failure.
  int Register(buffer_handle_t buffer);

  // This method is analogous to the unregister() function in Android gralloc
  // module.  After |buffer| is deregistered, calling Lock(), LockYCbCr(), or
  // Unlock() on |buffer| will fail.
  //
  // Args:
  //    |buffer|: The buffer handle to deregister.
  //
  // Returns:
  //    0 on success; corresponding error code on failure.
  int Deregister(buffer_handle_t buffer);

  // This method is analogous to the lock() function in Android gralloc module.
  // Here the buffer handle is mapped with the given args.
  //
  // Args:
  //    |buffer|: The buffer handle to map.
  //    |flags|:  Currently omitted and is reserved for future use.
  //    |x|: The base x coordinate in pixels.
  //    |y|: The base y coordinate in pixels.
  //    |width|: The width in pixels of the area to map.
  //    |height|: The height in pixels of the area to map.
  //    |out_addr|: The mapped address.
  //
  // Returns:
  //    0 on success with |out_addr| set with the mapped address;
  //    -EINVAL on invalid buffer handle or invalid buffer format.
  int Lock(buffer_handle_t buffer,
           uint32_t flags,
           uint32_t x,
           uint32_t y,
           uint32_t width,
           uint32_t height,
           void** out_addr);

  // This method is analogous to the lock_ycbcr() function in Android gralloc
  // module.  Here all the physical planes of the buffer handle are mapped with
  // the given args.
  //
  // Args:
  //    |buffer|: The buffer handle to map.
  //    |flags|:  Currently omitted and is reserved for future use.
  //    |x|: The base x coordinate in pixels.
  //    |y|: The base y coordinate in pixels.
  //    |width|: The width in pixels of the area to map.
  //    |height|: The height in pixels of the area to map.
  //    |out_ycbcr|: The mapped addresses, plane strides and chroma offset.
  //        - |out_ycbcr.y| stores the mapped address of the Y-plane.
  //        - |out_ycbcr.cb| stores the mapped address of the Cb-plane.
  //        - |out_ycbcr.cr| stores the mapped address of the Cr-plane.
  //        - |out_ycbcr.ystride| stores the stride of the Y-plane.
  //        - |out_ycbcr.cstride| stores the stride of the chroma planes.
  //        - |out_ycbcr.chroma_step| stores the distance between two adjacent
  //          pixels on the chroma plane. The value is 1 for normal planar
  //          formats, and 2 for semi-planar formats.
  //
  // Returns:
  //    0 on success with |out_ycbcr.y| set with the mapped buffer info;
  //    -EINVAL on invalid buffer handle or invalid buffer format.
  int LockYCbCr(buffer_handle_t buffer,
                uint32_t flags,
                uint32_t x,
                uint32_t y,
                uint32_t width,
                uint32_t height,
                struct android_ycbcr* out_ycbcr);

  // This method is analogous to the unlock() function in Android gralloc
  // module.  Here the buffer is simply unmapped.
  //
  // Args:
  //    |buffer|: The buffer handle to unmap.
  //
  // Returns:
  //    0 on success; -EINVAL on invalid buffer handle.
  int Unlock(buffer_handle_t buffer);

  // Get the number of physical planes associated with |buffer|.
  //
  // Args:
  //    |buffer|: The buffer handle to query.
  //
  // Returns:
  //    Number of planes on success; -EINVAL if |buffer| is invalid.
  static int GetNumPlanes(buffer_handle_t buffer);

 private:
  // Allow unit tests to call constructor directly.
  friend class tests::CameraBufferMapperTest;

  CameraBufferMapper();

  // Maps |buffer| and returns the mapped address.
  //
  // Args:
  //    |buffer|: The buffer handle to map.
  //    |flags|:  Currently omitted and is reserved for future use.
  //    |x|: The base x coordinate in pixels.
  //    |y|: The base y coordinate in pixels.
  //    |width|: The width in pixels of the area to map.
  //    |height|: The height in pixels of the area to map.
  //    |plane|: The plane to map.
  //
  // Returns:
  //    The mapped address on success; MAP_FAILED on failure.
  void* Map(buffer_handle_t buffer,
            uint32_t flags,
            uint32_t x,
            uint32_t y,
            uint32_t width,
            uint32_t height,
            uint32_t plane);

  // Unmaps |buffer|.
  //
  // Args:
  //    |buffer|: The buffer handle to unmap.
  //    |plane|: The plane to unmap.
  //
  // Returns:
  //    0 on success; -EINVAL if |buffer| is invalid.
  int Unmap(buffer_handle_t buffer, uint32_t plane);

  // Lock to guard access member variables..
  base::Lock lock_;

  // ** Start of lock_ scope **

  // The handle to the opened GBM device.
  GbmDeviceUniquePtr gbm_device_;

  // A cache which stores all the imported GBM buffer objects.  |gbm_bo_| needs
  // to be placed before |buffer_info_| to make sure the GBM buffer objects are
  // valid when we unmap them in |buffer_info_|'s destructor.
  GbmBoCache gbm_bo_;

  // The private info about all the mapped (buffer, plane) pairs.
  // |buffer_info_| has to be placed after |gbm_device_| so that the GBM device
  // is still valid when we delete the MappedBufferInfoUniquePtr.
  MappedBufferInfoCache buffer_info_;

  // ** End of lock_ scope **

  DISALLOW_COPY_AND_ASSIGN(CameraBufferMapper);
};

}  // namespace arc

#endif  // INCLUDE_ARC_CAMERA_BUFFER_MAPPER_H_