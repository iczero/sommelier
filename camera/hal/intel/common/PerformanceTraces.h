/*
 * Copyright (C) 2012-2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ANDROID_LIBCAMERA_PERFORMANCE_TRACES
#define ANDROID_LIBCAMERA_PERFORMANCE_TRACES
#include <Utils.h>
#include <string.h>
#include <mutex>
#include "LogHelper.h"
#include "IaAtrace.h"


/**
 * \class PerformanceTraces
 *
 * Interface for managing R&D traces used for performance
 * analysis and testing.
 *
 * This interface is designed to minimize call overhead and it can
 * be disabled altogether in product builds. Calling the functions
 * from different threads is safe (no crashes), but may lead to
 * at least transient incorrect results, so the output values need
 * to be postprocessed for analysis.
 *
 * This code should be disabled in product builds.
 */
NAMESPACE_DECLARATION {
namespace PerformanceTraces {

  // this is a bit ugly but this is a compact way to define a no-op
  // implementation in case LIBCAMERA_RD_FEATURES is not set
#undef STUB_BODY
#ifdef LIBCAMERA_RD_FEATURES
#define STUB_BODY ;
#else
#define STUB_BODY {}
#endif

  class Launch2Preview {
  public:
    static void enable(bool set) STUB_BODY
    static void start(void) STUB_BODY
    static void stop(int mFrameNum) STUB_BODY
  };

  class Launch2FocusLock {
  public:
    static void enable(bool set) STUB_BODY
    static void start(void) STUB_BODY
    static void stop(void) STUB_BODY
  };

  class Shot2Shot {
  public:
    static void enable(bool set) STUB_BODY
    static void start(void) STUB_BODY
    static void takePictureCalled(void) STUB_BODY
    static void stop(void) STUB_BODY
  };

  class ShutterLag {
  public:
    static void enable(bool set) STUB_BODY
    static void takePictureCalled(void) STUB_BODY
    static void snapshotTaken(struct timeval *ts) STUB_BODY
  };

  class AAAProfiler {
  public:
    static void enable(bool set) STUB_BODY
    static void start(void) STUB_BODY
    static void stop(void) STUB_BODY
  };

  class SwitchCameras {
  public:
    static void enable(bool set) STUB_BODY
    static void start(int cameraid) STUB_BODY
    static void getOriginalMode(bool videomode) STUB_BODY
    static void called(bool videomode) STUB_BODY
    static void stop(void) STUB_BODY
  };

/**
 * PnP use following to breakdown all current PI/KPIs(L2P, S2S, HDRS2P,
 * FocusLock, Back/FrontCameraSwitch, StillVideoModeSwitch)
 */
  class PnPBreakdown {
  public:
    static void start(void) STUB_BODY
    static void enable(bool set) STUB_BODY
    static void step(const char *func, const char* note = 0, const int mFrameNum = -1) STUB_BODY
    static void stop(void) STUB_BODY
  };

  class IOBreakdown {
  public:
    IOBreakdown(const char* /*func*/, const char* /*note*/) STUB_BODY
    ~IOBreakdown() STUB_BODY
  public:
    static void start(void) STUB_BODY
    static void enableBD(bool) STUB_BODY
    static void enableMemInfo(bool) STUB_BODY
    static void stop(void) STUB_BODY
  private:
    const char *mFuncName;
    const char *mNote;
    static bool mMemInfoEnabled;
    static int mPipeFD;
    static int mDbgFD;
    static int mPipeflushFD;
    static std::mutex mMemMutex; /* serialize write/read op to mDbgFD/mPipeFD */
  };

/**
 * \class HalAtrace
 *
 * This class allows tracing the execution of a method by writing some magic
 * data to trace_marker.
 * By declaring object of this class at the beginning of a method the
 * constructor code which writes method name and some extra information to
 * trace_marker is executed then.
 * When the method finishes the object is automatically destroyed. The code
 * in the destructor which writes "E" to trace_marker is executed then.
 * The tool, like Camtune, can offline visualizes those traces recorded
 * from trace_marker, and thus greatly improve the efficiency of performance
 * profiling.
 */
  class HalAtrace {
      public:
          HalAtrace(const char* func, const char* tag, const char* note = NULL, int value = -1);
          ~HalAtrace();
          static void reset(void);
      private:
          static int mTraceLevel;
  };

  /**
   * Helper function to disable all the performance traces
   */
  void reset(void);

 /**
   * Helper macro to call PerformanceTraces::Breakdown::step() with
   * the proper function name, and pass additional arguments.
   *
   * @param note textual description of the trace point
   */
  #define PERFORMANCE_TRACES_BREAKDOWN_STEP(note) \
    PerformanceTraces::PnPBreakdown::step(__FUNCTION__, note)


 /**
   * Helper macro to call PerformanceTraces::Breakdown::step() with
   * the proper function name, and pass additional arguments.
   *
   * @param note textual description of the trace point
   * @param frameCounter frame id this trace relates to
   *
   * See also PERFORMANCE_TRACES_BREAKDOWN_STEP_NOPARAM()
   */
  #define PERFORMANCE_TRACES_BREAKDOWN_STEP_PARAM(note, mFrameNum) \
    PerformanceTraces::PnPBreakdown::step(__FUNCTION__, note, mFrameNum)

  #define PERFORMANCE_TRACES_BREAKDOWN_STEP_NOPARAM() \
    PerformanceTraces::PnPBreakdown::step(__FUNCTION__)

  /**
   * Helper macro to call when a take picture message
   * is actually handled.
   */
  #define PERFORMANCE_TRACES_SHOT2SHOT_TAKE_PICTURE_HANDLE() \
      do { \
          PerformanceTraces::Shot2Shot::takePictureCalled(); \
          PerformanceTraces::PnPBreakdown::step(__FUNCTION__); \
      } while(0)

 /**
   * Helper macro to call when takePicture HAL method is called.
   * This step is used in multiple metrics.
   */
  #define PERFORMANCE_TRACES_TAKE_PICTURE_QUEUE() \
      do { \
          PerformanceTraces::PnPBreakdown::step(__FUNCTION__);  \
          PerformanceTraces::ShutterLag::takePictureCalled(); \
      } while(0)

  /**
   * Helper macro to call when preview frame has been sent
   * to display subsystem. This step is used in multiple metrics.
   *
   * @param x preview frame counter
   */
  #define PERFORMANCE_TRACES_PREVIEW_SHOWN(x) \
      do { \
          PerformanceTraces::Launch2Preview::stop(x); \
          PerformanceTraces::SwitchCameras::stop(); \
      } while(0)

  /**
   * Helper macro to call PerformanceTraces::Shot2Shot::takePictureCalled() with
   * the proper function name.
   */
  #define PERFORMANCE_TRACES_LAUNCH_START() \
      do { \
          PerformanceTraces::PnPBreakdown::start(); \
          PerformanceTraces::Launch2FocusLock::start(); \
          PerformanceTraces::Launch2Preview::start(); \
          PerformanceTraces::IOBreakdown::start(); \
      } while(0)

  #define PERFORMANCE_TRACES_IO_STOP() \
      PerformanceTraces::IOBreakdown::stop();

  #define PERFORMANCE_TRACES_IO_BREAKDOWN(note) \
      PerformanceTraces::IOBreakdown p(__FUNCTION__, note); \

  /**
   * Helper macro to use HalAtrace.
   */
#ifdef CAMERA_HAL_DEBUG
  #define PERFORMANCE_HAL_ATRACE() \
      PerformanceTraces::HalAtrace atrace(__FUNCTION__, LOG_TAG);
  #define PERFORMANCE_HAL_ATRACE_PARAM1(note, value) \
      PerformanceTraces::HalAtrace atrace(__FUNCTION__, LOG_TAG, note, value);
#else
  #define PERFORMANCE_HAL_ATRACE()
  #define PERFORMANCE_HAL_ATRACE_PARAM1(note, value)
#endif

} // ns PerformanceTraces

/**
 * \class ScopedPerfTrace
 *
 * This class allows tracing the execution of a method. By declaring object
 * of this class at the beginning of a method/function the constructor code is
 * executed then.
 * When the method finishes the object is automatically destroyed.
 * The code in the destructor is useful to trace how long it took to execute
 * a method.
 * If a maxExecTime is provided an error message will be printed in case the
 * execution time took longer than expected
 */
class ScopedPerfTrace {
public:
inline ScopedPerfTrace(int level, const char* name, nsecs_t maxExecTime = 0) :
       mLevel(level),
       mName(name),
       mMaxExecTime(maxExecTime)
{
    mStartTime = systemTime();
}

inline ~ScopedPerfTrace()
{
    nsecs_t actualExecTime = systemTime()- mStartTime;
    if (LogHelper::isPerfDumpTypeEnable(mLevel)) {
        LOGD("%s took %" PRId64 " ns", mName, actualExecTime);
    }

    if (CC_UNLIKELY(mMaxExecTime > 0 && actualExecTime > mMaxExecTime)){
        LOGW("KPI:%s took longer than expected. Actual %" PRId64 " us expected %" PRId64 " us",
                mName, actualExecTime/1000, mMaxExecTime/1000);
    }
}

private:
    nsecs_t mStartTime;     /*!> systemTime when this object was created */
    int mLevel;             /*!> Trace level used */
    const char* mName;      /*!> Name of this trace object */
    nsecs_t mMaxExecTime;   /*!> Maximum time this object is expected to live */
};

/**
 * HAL_KPI_TRACE_CALL
 * Prints traces of the execution time of the method and checks if it took
 * longer than maxTime. In that case it prints an warning trace
 */
#define HAL_KPI_TRACE_CALL(level, maxTime)  ScopedPerfTrace __kpiTracer(level, __FUNCTION__, maxTime)
#ifdef CAMERA_HAL_DEBUG
#define HAL_PER_TRACE_NAME(level, name) ScopedPerfTrace  ___tracer(level, name )
#define HAL_PER_TRACE_CALL(level)  HAL_PER_TRACE_NAME(level, __FUNCTION__)
#else
#define HAL_PER_TRACE_NAME(level, name)
#define HAL_PER_TRACE_CALL(level)
#endif
} NAMESPACE_DECLARATION_END
#endif // ANDROID_LIBCAMERA_PERFORMANCE_TRACES
