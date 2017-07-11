/*
 * Copyright (C) 2016-2017 Intel Corporation.
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

#define LOG_TAG "CaptureUnit"

#include "CaptureUnit.h"
#include "Camera3Request.h"
#include "LogHelper.h"
#include "BufferPools.h"
#include "SettingsProcessor.h"
#include "MediaController.h"
#include "PSLConfParser.h"
#include "MediaEntity.h"
#include "PerformanceTraces.h"

namespace android {
namespace camera2 {

CaptureUnit::CaptureUnit(int camId, IStreamConfigProvider &aStreamCfgProv, std::shared_ptr<MediaController> mc) :
        mCameraId(camId),
        mActiveIsysNodes(0),
        mMediaCtl(mc),
        mThreadRunning(false),
        mMessageQueue("Camera_CaptureUnit", (int)MESSAGE_ID_MAX),
        mMessageThread(nullptr),
        mStreamCfgProvider(aStreamCfgProv),
        mBufferPools(nullptr),
        mSettingProcessor(nullptr),
        mLensController(nullptr),
        mLensSupported(false),
        mPipelineDepth(0),
        mInflightRequestPool("CaptureUnit"),
        mSensorSettingsDelay(0),
        mGainDelay(0)
{}

CaptureUnit::~CaptureUnit()
{
    if (mMessageThread != nullptr) {
        Message msg;
        msg.id = MESSAGE_ID_EXIT;
        mMessageQueue.send(&msg);
        mMessageThread->requestExitAndWait();
        mMessageThread.reset();
        mMessageThread = nullptr;
    }

    if (mIsys != nullptr) {
        if (mIsys->isStarted())
            mIsys->stop(true);

        // Exit ISYS thread before CaptureUnit thread exited
        mIsys->requestExitAndWait();
    }

    if(mSyncManager) {
        mSyncManager->stop();
        mSyncManager = nullptr;
    }

    mInflightRequests.clear();
    mQueuedCaptureBuffers.clear();
    mLastInflightRequest = nullptr;
    mInflightRequestPool.deInit();

    if (mBufferPools) {
        delete mBufferPools;
        mBufferPools = nullptr;
    }
}

/**
 * initStaticMetadata
 *
 * Create CameraMetadata object to retrieve the static tags used in this class
 * we cache them as members so that we do not need to query CameraMetadata class
 * everytime we need them. This is more efficient since find() is not cheap
 */
status_t CaptureUnit::initStaticMetadata()
{
    //Initialize the CameraMetadata object with the static metadata tags
    camera_metadata_t* plainStaticMeta;
    plainStaticMeta = (camera_metadata_t*)PlatformData::getStaticMetadata(mCameraId);
    if (plainStaticMeta == nullptr) {
        LOGE("Failed to get camera %d StaticMetadata", mCameraId);
        return UNKNOWN_ERROR;
    }

    CameraMetadata staticMeta(plainStaticMeta);
    camera_metadata_entry entry;
    entry = staticMeta.find(ANDROID_LENS_INFO_MINIMUM_FOCUS_DISTANCE);
    if (entry.count == 1) {
        LOG1("camera %d minimum focus distance:%f", mCameraId, entry.data.f[0]);
        mLensSupported = (entry.data.f[0] > 0) ? true : false;
        LOG1("Lens movement %s for camera id %d",
             mLensSupported ? "supported" : "NOT supported", mCameraId);
    }
    staticMeta.release();

    const IPU3CameraCapInfo *cap = getIPU3CameraCapInfo(mCameraId);
    if (cap == nullptr) {
        LOGE("Failed to get cameraCapInfo");
        return UNKNOWN_ERROR;
    }
    mSensorSettingsDelay = MAX(cap->mExposureLag, cap->mGainLag);
    mGainDelay = cap->mGainLag;

    return NO_ERROR;
}

status_t CaptureUnit::init()
{
    mBufferPools = new BufferPools(mCameraId);

    mMessageThread = std::unique_ptr<MessageThread>(new MessageThread(this, "CaptureUnit"));
    mMessageThread->run();

    mInflightRequestPool.init(MAX_REQUEST_IN_PROCESS_NUM, InflightRequestState::reset);

    mIsys = std::make_shared<InputSystem>(this, mMediaCtl);

    //Cache the static metadata values we are going to need in the capture unit
    if (initStaticMetadata() != NO_ERROR) {
        LOGE("Cannot initialize static metadata");
        return NO_INIT;
    }

    mSyncManager = std::make_shared<SyncManager>(mCameraId,mMediaCtl, mIsys.get());

    status_t status = mSyncManager->init(mSensorSettingsDelay, mGainDelay);
    if (status != NO_ERROR) {
        LOGE("Cannot initialize SyncManager (status= 0x%X)", status);
        return status;
    }

    if (!mLensSupported) {
        mLensController = nullptr;
        return OK;
    }

    mLensController = std::make_shared<LensHw>(mCameraId, mMediaCtl);
    if (mLensController == nullptr) {
        LOGE("@%s: Error creating LensHw", __FUNCTION__);
        return NO_INIT;
    }

    status = mLensController->init();
    if (status != NO_ERROR) {
        LOGE("%s:Cannot initialize LensHw (status= 0x%X)", __FUNCTION__, status);
        return status;
    }

    return OK;
}

LensHw *CaptureUnit::getLensControlInterface()
{
    return static_cast<LensHw*>(mLensController.get());
}

void CaptureUnit::setSettingsProcessor(SettingsProcessor *settingsProcessor)
{
    mSettingProcessor = settingsProcessor;
    if (mSettingProcessor) {
        mSettingProcessor->getStaticMetadataCache().getPipelineDepth(mPipelineDepth);
    }
}

status_t CaptureUnit::flush()
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL1);
    status_t status = NO_ERROR;

    Message msg;
    msg.id = MESSAGE_ID_FLUSH;
    mMessageQueue.remove(MESSAGE_ID_CAPTURE);
    status = mMessageQueue.send(&msg, MESSAGE_ID_FLUSH);
    return status;
}

status_t CaptureUnit::handleMessageFlush(Message &msg)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL1);
    UNUSED(msg);
    status_t status = NO_ERROR;

    if (mLastInflightRequest) {
        if (mLastInflightRequest->aiqCaptureSettings)
            mLastInflightRequest->aiqCaptureSettings.reset();
        mLastInflightRequest->graphConfig.reset();
        mLastInflightRequest->request = nullptr;
    }

    mSyncManager->flush();

    mIsys->flush();

    mMessageQueue.reply(MESSAGE_ID_FLUSH, status);
    return status;
}

status_t CaptureUnit::configStreams(std::vector<camera3_stream_t*> &activeStreams,
        bool configChanged)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2);
    status_t status = NO_ERROR;

    Message msg;
    msg.id = MESSAGE_ID_CONFIGSTREAM;
    msg.data.config.activeStreams = &activeStreams;
    msg.data.config.configChanged = configChanged; // written to in msg handler

    status = mMessageQueue.send(&msg, MESSAGE_ID_CONFIGSTREAM);
    return status;
}

status_t CaptureUnit::handleMessageConfigStreams(Message &msg)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL1);
    status_t status = NO_ERROR;
    ICaptureEventListener::CaptureMessage outMsg;
    std::vector<camera3_stream_t*> &activeStreams = *msg.data.config.activeStreams;
    bool configChanged = msg.data.config.configChanged;
    MediaCtlHelper::ConfigurationResults isysConfigResult;
    std::shared_ptr<GraphConfig> baseConfig = mStreamCfgProvider.getBaseGraphConfig();

    mActiveStreams.clear();
    for (unsigned int i = 0; i < activeStreams.size(); i++) {
         mActiveStreams.push_back(activeStreams.at(i));
    }
    /*
     * Check if the new selected media controller configuration is the same as
     * the one we were using until now.
     *
     * TODO: There seems to be bug somewhere and reusing old state without resetting
     * doesn't work. As WA do the reset always.
     */
    configChanged = true;
    const MediaCtlConfig *cfg = mStreamCfgProvider.getMediaCtlConfig(IStreamConfigProvider::CIO2);
    if (CC_UNLIKELY(cfg == nullptr)) {
        LOGE("Failed to retrieve media ctl configuration");
        return UNKNOWN_ERROR;
    }

    LOG1("Selected MediaCtl pipe config id: %d resolution %dx%d %s",
            cfg->mCameraProps.id,
            cfg->mCameraProps.outputWidth,
            cfg->mCameraProps.outputHeight,
            configChanged ? "(config changed, ISYS restart needed)":"");

    LOG1("@%s: restarting and reconfiguring ISYS", __FUNCTION__);

    status = mSyncManager->stop();
    if (status != OK)
        LOGE("failed to flush events before stopping - BUG");

    // Stop streaming and reconfigure ISYS
    if (mIsys->isStarted()) {
        status = mIsys->stop(true);
        if (status != NO_ERROR) {
            LOGE("Failed to stop streaming!");
            mMessageQueue.reply(MESSAGE_ID_CONFIGSTREAM, status);
            return status;
        }

    }
    // Free and unmap buffers before closing video nodes
    mBufferPools->freeBuffers();

    mQueuedCaptureBuffers.clear();
    if (mBufferPools) {
        delete mBufferPools;
        mBufferPools = nullptr;
    }
    mBufferPools = new BufferPools(mCameraId);

    // Configure ISYS based stream configuration from Graph Config Manager
    // Get output format from ISYS.
    status = mIsys->configure(mStreamCfgProvider,
                              isysConfigResult);
    if (status != NO_ERROR) {
        LOGE("Error configuring InputSystem");
        mMessageQueue.reply(MESSAGE_ID_CONFIGSTREAM, status);
        return status;
    }

    /*
     * After configuring the Input system, query the active nodes and store
     * them in the bit-mask mActiveIsysNodes.
     * Based on this we will do more configurations.
     */
    mActiveIsysNodes = getActiveIsysNodes(baseConfig);
    LOG1("Active ISYS nodes: %x", mActiveIsysNodes);

    status = setSensorFrameTimings();
    if (status != NO_ERROR) {
        LOGE("Failed to set sensor frame timings", status);
        mMessageQueue.reply(MESSAGE_ID_CONFIGSTREAM, status);
        return status;
    }

    int skipCount = mSensorSettingsDelay;//mSyncManager->getFrameSyncDelay();
    int poolSize = mPipelineDepth + 1;

    status = mBufferPools->createBufferPools(poolSize, skipCount, mIsys);
    if (status != NO_ERROR) {
        LOGE("Failed to create buffer pools (status= 0x%X)", status);
        mMessageQueue.reply(MESSAGE_ID_CONFIGSTREAM, status);
        return status;
    }
    mQueuedCaptureBuffers.reserve(poolSize);

    /**
     * Notify Control Unit of the new sensor mode information
     * - exposure descriptor: from sensor Hw class
     * - frame params: from Isys
     */
    outMsg.id = ICaptureEventListener::CAPTURE_MESSAGE_ID_EVENT;
    outMsg.data.event.type = ICaptureEventListener::CAPTURE_EVENT_NEW_SENSOR_DESCRIPTOR;

    status = getSensorModeData(outMsg.data.event.exposureDesc);
    if (CC_UNLIKELY(status != OK)) {
        LOGE("Failed to retrieve sensor mode data - BUG");
        mMessageQueue.reply(MESSAGE_ID_CONFIGSTREAM, status);
        return status;
    }

    outMsg.data.event.frameParams = isysConfigResult.sensorFrameParams;

    notifyListeners(&outMsg);

    mMessageQueue.reply(MESSAGE_ID_CONFIGSTREAM, status);

    return status;
}

int CaptureUnit::getActiveIsysNodes(std::shared_ptr<GraphConfig> graphConfig)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL1);
    status_t status = NO_ERROR;
    int nodeCount = 0;
    InputSystem::ConfiguredNodesPerName *nodes = nullptr;
    std::string gcNodeName;
    uid_t portTerminalId;

    status = mIsys->getOutputNodes(&nodes, nodeCount);
    if (status != NO_ERROR) {
        LOGE("ISYS output nodes not configured");
        return status;
    }

    int activeNodes = IMGU_NODE_NULL;
    for (const auto &node : *nodes) {
        // Add an active ISYS node in mActiveIsysNodes.
        activeNodes |= node.first;

        switch(node.first) {
            case ISYS_NODE_CSI_BE_SOC:
                LOG1("ISYS_NODE_CSI_BE_SOC");
                gcNodeName = "csi_be:output";
                break;
            default:
                LOGE("Unknown node: %d", node.first);
                break;
        }

        status = graphConfig->portGetPeerIdByName(gcNodeName,
                                                  portTerminalId);
        if (CC_UNLIKELY(status != OK)) {
            LOG1("Could not find peer port for %s", gcNodeName.c_str());
            status = OK;
        } else {
            mNodeToPortMap[node.first] = portTerminalId;
            LOG1("Mapping isys node %d port %x added to the map",
                    node.first, portTerminalId);
        }
        gcNodeName.clear();
    }

    return activeNodes;
}


status_t CaptureUnit::setSensorFrameTimings()
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL1);
    status_t status = NO_ERROR;

    const MediaCtlConfig *mediaCtlConfig = mStreamCfgProvider.getMediaCtlConfig(IStreamConfigProvider::CIO2);
    if (CC_UNLIKELY(mediaCtlConfig == nullptr)) {
        LOGE("Failed to retrieve media ctl configuration");
        return UNKNOWN_ERROR;
    }

    if (!mediaCtlConfig->mFTCSize.Width || !mediaCtlConfig->mFTCSize.Height) {
        LOGE("Error in FTC size, check xml, %dx%d",
             mediaCtlConfig->mFTCSize.Width,
             mediaCtlConfig->mFTCSize.Height);
        // Right now values only set and used for CRL driver. So ignoring in
        // case of SMIA
        return NO_ERROR;
    }

    return status;
}

/**
 * getSensorModeData
 *
 * Retrieves the exposure sensor descriptor that the 3A algorithms need to run
 * This information is relayed to control unit.
 * The other piece of information related to sensor mode (frame params) is
 * provided by the Input System as part of the configuration results.
 *
 * \param [out] desc: Sensor descriptor to fill in
 * \return OK if everything went fine.
 * \return BAD_VALUE if any of the sensor driver queries failed.
 */
status_t CaptureUnit::getSensorModeData(ia_aiq_exposure_sensor_descriptor &desc)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL1);
    status_t status = NO_ERROR;

    status = mSyncManager->getSensorModeData(desc);

    return status;
}

status_t CaptureUnit::doCapture(Camera3Request* request,
        std::shared_ptr<CaptureUnitSettings>  &aiqCaptureSettings,
        std::shared_ptr<GraphConfig> graphConfig)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2);
    status_t status = NO_ERROR;
    Message msg;

    status = mInflightRequestPool.acquireItem(msg.data.request.inFlightRequest);
    if (status != NO_ERROR) {
        LOGE("Failed to acquire free inflight request from pool - BUG");
        return UNKNOWN_ERROR;
    }
    msg.id = MESSAGE_ID_CAPTURE;
    msg.data.request.inFlightRequest->request = request;
    msg.data.request.inFlightRequest->graphConfig = graphConfig;
    msg.data.request.inFlightRequest->aiqCaptureSettings = aiqCaptureSettings;
    msg.data.request.inFlightRequest->shutterDone = false;

    if (CC_UNLIKELY(aiqCaptureSettings.get() == nullptr)) {
        LOGE("AIQ capture settings is nullptr");
        return BAD_VALUE;
    }

    status = mMessageQueue.send(&msg);
    return status;
}

status_t CaptureUnit::handleMessageCapture(Message &msg)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2);
    status_t status = NO_ERROR;
    std::shared_ptr<InflightRequestState> inflightRequest = msg.data.request.inFlightRequest;
    mLastInflightRequest = inflightRequest;
    mLastInflightRequest->request = nullptr;
    int32_t reqId = inflightRequest->aiqCaptureSettings->aiqResults.requestId;

    bool needSkipping = !mIsys->isStarted();

    if (needSkipping) {
        /* Issue skips. But no captures yet, if not streaming.
         * It would be more logical to not send settings for skips and only
         * send the settings for the actual capture, thus advancing the settings
         * latching for the amount of skips.
         * 3A convergence issues during ITS testing is the reason to have "true"
         * for settings here.
         * TODO: revisit and try to make settings sending "false" below.
         */
        issueSkips(mSensorSettingsDelay, true, true, false);
    }

    /*
     * Enqueue the data buffers first
     */
    status = enqueueBuffers(inflightRequest, false);
    if (status != NO_ERROR) {
        LOGE("Failed to enqueue buffers!");
        return UNKNOWN_ERROR;
    }

    /*
     * And then the settings. We send the settings only if we are not skipping,
     * because ATM we are unfortunately sending settings for skips above, see
     * the comment above "issueSkips".
     */
    if (!needSkipping) {
        status = applyAeParams(inflightRequest->aiqCaptureSettings);
        if (status != NO_ERROR) {
            LOGE("Failed to apply AE settings for request %d", reqId);
            return status;
        }
    }

    bool started = false;
    mSyncManager->isStarted(started);
    if (!started) {
        LOG1("@%s: Starting SyncManager", __FUNCTION__);
        mSyncManager->start();

        // issue the capture calls for the skips
        //issueSkips(mSensorSettingsDelay, false, false, true);

        LOG1("@%s: Starting ISYS", __FUNCTION__);
        status = mIsys->start();
        if (status != NO_ERROR) {
            LOGE("Failed to start streaming!");
            return status;
        }

        // issue the capture calls for the skips
        issueSkips(mSensorSettingsDelay, false, false, true);
    }

    mInflightRequests.insert(std::make_pair(reqId, inflightRequest));
    mIsys->capture(reqId);

    return status;
}

status_t CaptureUnit::applyAeParams(std::shared_ptr<CaptureUnitSettings> &aiqCaptureSettings)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2);
    status_t status = NO_ERROR;

    status = mSyncManager->setParameters(aiqCaptureSettings);

    return status;
}

/**
 * enqueueBuffers
 *
 * Check if we have a raw stream and get a buffer from the stream or
 * acquire a buffer from the capture buffer pool and do a v4l2 putframe
 * of the capture buffer
 * \param [IN] request Capture request
 * \param [IN] skip Skip frame
 */
status_t CaptureUnit::enqueueBuffers(std::shared_ptr<InflightRequestState> &reqState, bool skip)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2);
    status_t status = NO_ERROR;

    status = enqueueIsysBuffer(reqState, skip);
    if (status != NO_ERROR) {
        LOGE("Failed to enqueue a ISYS capture buffer!");
        return status;
    }

    return status;
}

status_t CaptureUnit::enqueueIsysBuffer(std::shared_ptr<InflightRequestState> &reqState,
                                        bool skip)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2);
    status_t status = NO_ERROR;
    std::shared_ptr<CaptureBuffer> capBufPtr = nullptr;
    std::shared_ptr<GraphConfig> gc = nullptr;
    int32_t reqId = reqState->aiqCaptureSettings->aiqResults.requestId;

    gc = reqState->graphConfig;
    if (CC_UNLIKELY(gc.get() == nullptr)) {
        LOGE("Failed to retrieve graphConfig");
        return UNKNOWN_ERROR;
    }

    if (!skip) {
        // get a capture buffer from the pool
        status = mBufferPools->acquireItem(capBufPtr);
    } else {
        status = mBufferPools->acquireCaptureSkipBuffer(capBufPtr);
    }

    if (status != NO_ERROR || capBufPtr.get() == nullptr) {
        LOGE("Failed to get a capture %s buffer!", skip ? "skip" : "");
        return UNKNOWN_ERROR;
    }

    if (mActiveIsysNodes & ISYS_NODE_CSI_BE_SOC) {
        capBufPtr->mDestinationTerminal = mNodeToPortMap[ISYS_NODE_CSI_BE_SOC];

        capBufPtr->v4l2Buf.flags |= V4L2_BUF_FLAG_NO_CACHE_INVALIDATE | V4L2_BUF_FLAG_NO_CACHE_CLEAN;

        status = mIsys->putFrame(ISYS_NODE_CSI_BE_SOC,
                                 &(capBufPtr->v4l2Buf), reqId);
    } else {
        LOGE("Unsupport ISYS capture type!");
        return UNKNOWN_ERROR;
    }

    if (CC_UNLIKELY(status != NO_ERROR)) {
        LOGE("Failed to queue a buffer!");
        return UNKNOWN_ERROR;
    }

    mQueuedCaptureBuffers.push_back(capBufPtr);

    return status;
}


status_t CaptureUnit::attachListener(ICaptureEventListener *aListener)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2);
    mListeners.push_back(aListener);
    return OK;
}

void CaptureUnit::cleanListeners()
{}

void CaptureUnit::notifyIsysEvent(IsysMessage &isysMsg)
{
    if (isysMsg.id == ISYS_MESSAGE_ID_EVENT) {
        LOG2("@%s: request ID: %d, node: %d", __FUNCTION__,
                isysMsg.data.event.requestId, isysMsg.data.event.isysNodeName);
        Message msg;
        msg.id = MESSAGE_ID_ISYS_EVENT;
        msg.data.buffer.requestId = isysMsg.data.event.requestId;
        msg.data.buffer.isysNodeName = isysMsg.data.event.isysNodeName;
        if (isysMsg.data.event.buffer != nullptr) {
            msg.data.buffer.v4l2Buf = *isysMsg.data.event.buffer;
        }

        mMessageQueue.send(&msg);
    } else {
        LOGE("Error from input system, ReqId: %d", isysMsg.id);
    }
}

/**
 * Generic function to enqueue buffer at sensor speed whenever
 * client captures are slower than sensor captures.
 *
 * \param[in] count number of capture to enqueue
 * \param[in] buffers flag to enqueue buffer
 * \param[in] settings flag to enqueue sensor/flash settings
 * \param[in] isys flag to start capture on isys side
 * \return status of execution.
 */
status_t CaptureUnit::issueSkips(int count, bool buffers, bool settings, bool isys)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL1);
    status_t status = OK;
    int32_t lastReqId;
    int32_t skipRequestId = 0;
    LOG1("@%s, count:%d, buffers:%d, settings:%d, isys:%d", __FUNCTION__, count, buffers, settings, isys);

    // saving before issue skip latest valid client reqId
    // This is because we need to use skipRequestId for aiqCaptureSettings tracking.
    lastReqId = mLastInflightRequest->aiqCaptureSettings->aiqResults.requestId;

    if (buffers) {
        LOG1("@%s: enqueue %d skip buffers", __FUNCTION__, count);
        for (int i = 0; i < count; i++) {
            skipRequestId--;
            mSkipRequestIdQueue.push_back(skipRequestId);
            mLastInflightRequest->aiqCaptureSettings->aiqResults.requestId = skipRequestId;
            status = enqueueBuffers(mLastInflightRequest, true);
            if (status != NO_ERROR) {
                LOGE("Failed to enqueue SKIP buffers!");
                return UNKNOWN_ERROR;
            }
        }
    }

    if (settings) {
        LOG2("@%s: enqueue skip capture settings to sync manager",
                __FUNCTION__, count);
        for (int i = 0; i < count; i++) {
            status = applyAeParams(mLastInflightRequest->aiqCaptureSettings);
            if (status != NO_ERROR) {
                LOGE("Failed to apply AE settings for delay for skip request");
                return status;
            }
        }
    }

    if (isys) {
        for (int i = 0; i < count; i++) {
            if (mSkipRequestIdQueue.empty()) {
                LOGE("Skip RequestID Queue empty! Should not happen! BUG!");
                return UNKNOWN_ERROR;
            }

            mIsys->capture(mSkipRequestIdQueue[0]);
            mSkipRequestIdQueue.erase(mSkipRequestIdQueue.begin());
        }
    }

    // restore last valid client reqID,
    mLastInflightRequest->aiqCaptureSettings->aiqResults.requestId = lastReqId;

    return status;
}

status_t CaptureUnit::handleMessageIsysEvent(Message &msg)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2);
    status_t status = NO_ERROR;
    ICaptureEventListener::CaptureMessage outMsg;

    IPU3NodeNames isysNodeName = msg.data.buffer.isysNodeName;
    outMsg.id = ICaptureEventListener::CAPTURE_MESSAGE_ID_EVENT;

    switch(isysNodeName) {
    case ISYS_NODE_CSI_BE_SOC:
        status = processIsysBuffer(msg);
        break;
    // TODO: handle other types
    default:
        outMsg.id = ICaptureEventListener::CAPTURE_MESSAGE_ID_ERROR;
        LOGW("Unsupported event was returned from input system!Isys node: %d", isysNodeName);
        break;
    }

    return status;
}

status_t CaptureUnit::processIsysBuffer(Message &msg)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2);
    status_t status = NO_ERROR;
    std::shared_ptr<GraphConfig> gc = nullptr;
    std::shared_ptr<CaptureBuffer> isysBufferPtr = nullptr;
    v4l2_buffer_info *outBuf = nullptr;
    ICaptureEventListener::CaptureMessage outMsg;
    std::shared_ptr<InflightRequestState> state = nullptr;
    IPU3NodeNames isysNode = IMGU_NODE_NULL;
    int requestId = 0;

    requestId = msg.data.buffer.requestId;

    // Notify listeners, first fill the observer message
    outBuf = &msg.data.buffer.v4l2Buf;
    outMsg.data.event.timestamp.tv_sec = outBuf->vbuffer.timestamp.tv_sec;
    outMsg.data.event.timestamp.tv_usec = outBuf->vbuffer.timestamp.tv_usec;
    outMsg.data.event.sequence = outBuf->vbuffer.sequence;
    PERFORMANCE_HAL_ATRACE_PARAM1("seqId", outMsg.data.event.sequence);
    outMsg.id = ICaptureEventListener::CAPTURE_MESSAGE_ID_EVENT;
    outMsg.data.event.reqId = requestId;

    std::vector<std::shared_ptr<CaptureBuffer>> *bufferQueuePtr = nullptr;
    isysNode = msg.data.buffer.isysNodeName;
    bufferQueuePtr = &mQueuedCaptureBuffers;

    for (size_t i = 0; i < bufferQueuePtr->size(); i++) {
        if (outBuf->vbuffer.index == bufferQueuePtr->at(i)->v4l2Buf.index) {
            bufferQueuePtr->at(i)->buf->setRequestId(requestId);
            bufferQueuePtr->at(i)->buf->setTimeStamp(outMsg.data.event.timestamp);

            isysBufferPtr = bufferQueuePtr->at(i);
            isysBufferPtr->v4l2Buf.sequence = outMsg.data.event.sequence;
            // Remove the shared pointer reference from the vector
            bufferQueuePtr->erase(bufferQueuePtr->begin() + i);
            break;
        }
    }

    if (isysBufferPtr.get() == nullptr) {
        LOGE("ISYS buffer not found for request %d", requestId);
        return UNKNOWN_ERROR;
    }
    outMsg.data.event.pixelBuffer = isysBufferPtr;
    LOG2("@%s: Received buffer from ISYS node %d - Request %d", __FUNCTION__, isysNode, requestId);

    // Skip sending raw frames till the specified limit
    if (requestId < 0) {
        LOG2("@%s: skip frame %d received, isysNode:%d",
                __FUNCTION__, requestId, isysNode);
        mBufferPools->returnCaptureSkipBuffer(isysBufferPtr);
        return NO_ERROR;
    }

    // check if the request is in the queue.
    std::map<int, std::shared_ptr<InflightRequestState>>::iterator it =
                               mInflightRequests.find(requestId);
    if (it == mInflightRequests.end()) {
        LOGE("Request state not found for request %d - BUG", requestId);
        return UNKNOWN_ERROR;
    }
    state = it->second;

    gc = state->graphConfig;
    // notify shutter event
    if (state->shutterDone == false) {
        outMsg.data.event.type = ICaptureEventListener::CAPTURE_EVENT_SHUTTER;
        notifyListeners(&outMsg);
        state->shutterDone = true;
    }

    if (isysNode == ISYS_NODE_CSI_BE_SOC) {
        outMsg.data.event.type = ICaptureEventListener::CAPTURE_EVENT_RAW_BAYER;
        // Send notification if needed in this request
        if (gc->isIsaOutputDestinationActive(isysBufferPtr->mDestinationTerminal)) {
            LOG2("ISYS event %d arrived", outMsg.data.event.type);
            notifyListeners(&outMsg);
        } else {
            // buffer not needed in this request, recycle to the pool
            isysBufferPtr.reset();
        }
    } else {
        LOGE("Unsupprted isys node");
        return UNKNOWN_ERROR;
    }
    it = mInflightRequests.find(requestId);
    if (it != mInflightRequests.end())
        mInflightRequests.erase(it);
    return status;
}


void CaptureUnit::messageThreadLoop()
{
    LOG1("@%s - Start", __FUNCTION__);

    mThreadRunning = true;
    while (mThreadRunning) {
        status_t status = NO_ERROR;

        Message msg;
        mMessageQueue.receive(&msg);
        LOG2("@%s, receive message id:%d", __FUNCTION__, msg.id);
        PERFORMANCE_HAL_ATRACE_PARAM1("msg", msg.id);
        switch (msg.id) {
        case MESSAGE_ID_EXIT:
            mThreadRunning = false;
            break;
        case MESSAGE_ID_FLUSH:
            status = handleMessageFlush(msg);
            break;
        case MESSAGE_ID_CONFIGSTREAM:
            status = handleMessageConfigStreams(msg);
            break;
        case MESSAGE_ID_CAPTURE:
            status = handleMessageCapture(msg);
            break;
        case MESSAGE_ID_ISYS_EVENT:
            handleMessageIsysEvent(msg);
            break;
        default:
            LOGE("ERROR: Unknown message %d", msg.id);
            status = BAD_VALUE;
            break;
        }
        if (status != NO_ERROR)
            LOGE("error %d in handling message: %d", status,
                    static_cast<int>(msg.id));
        LOG2("@%s, finish message id:%d", __FUNCTION__, msg.id);
        mMessageQueue.reply(msg.id, status);
    }

    LOG1("%s: Exit", __FUNCTION__);
}

status_t CaptureUnit::notifyListeners(ICaptureEventListener::CaptureMessage *msg)
{
    HAL_TRACE_CALL(CAMERA_DEBUG_LOG_LEVEL2);
    bool ret = false;
    std::vector<ICaptureEventListener*>::iterator it = mListeners.begin();
    for (;it != mListeners.end(); ++it)
        ret |= (*it)->notifyCaptureEvent((ICaptureEventListener::CaptureMessage*)msg);

    return ret;
}

} // namespace camera2
} // namespace android
