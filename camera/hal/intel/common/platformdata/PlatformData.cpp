/*
 * Copyright (C) 2013-2017 Intel Corporation
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

#define LOG_TAG "PlatformData"

#include "LogHelper.h"
#include "PlatformData.h"
#include "CameraProfiles.h"
#include "CameraMetadataHelper.h"
#include "v4l2dev/v4l2device.h"
#include "UtilityMacros.h"
#include "ChromeCameraProfiles.h"

#include <unistd.h>        // usleep()
#include <ctype.h>         // tolower()
#include <dirent.h>        // opendir()
#include <fcntl.h>         // open(), close()
#include <linux/media.h>   // media controller
#include <linux/kdev_t.h>  // MAJOR(), MINOR()
#include <string>
#include <sstream>

// TODO this should come from the crl header file
// crl is a common code module in sensor driver, which contains
// basic functions for driver control
#define CRL_CID_SENSOR_MODE 0x00982954

#include "MediaController.h"
#include "MediaEntity.h"

NAMESPACE_DECLARATION {
using std::string;

#define KERNEL_MODULE_LOAD_DELAY 200000
#ifdef MEDIA_CTRL_INIT_DELAYED
#define RETRY_COUNTER 20
#else
#define RETRY_COUNTER 0
#endif

CameraProfiles* PlatformData::mInstance = nullptr;
CameraHWInfo* PlatformData::mCameraHWInfo = nullptr;

/**
 * index to this array is the camera id
 */
CpfStore* PlatformData::sKnownCPFConfigurations[MAX_CPF_CACHED];

GcssKeyMap* PlatformData::mGcssKeyMap = nullptr;

/**
 * Sensor drivers have been registered to media controller
 * or to the main device (old style driver)
 * The actual main device or media controller is detected at runtime
 * (see CameraDetect)
 * These are the default values in case is not detected correctly
 */

static const char *DEFAULT_MAIN_DEVICE = "/dev/video0";

GcssKeyMap::GcssKeyMap()
{
#define GCSS_KEY(key, str) std::make_pair(#str, GCSS_KEY_##key),
#define GCSS_KEY_SECTION_START(key, str, val) GCSS_KEY(key, str)
    mMap = {
        #include "gcss_keys.h"
    };
#undef GCSS_KEY
#undef GCSS_KEY_SECTION_START
}

GcssKeyMap::~GcssKeyMap()
{
}

void GcssKeyMap::gcssKeyMapInsert(std::map<std::string, ia_uid> &customMap)
{
    mMap.insert(customMap.begin(), customMap.end());
}

int GcssKeyMap::gcssKeyMapSize()
{
    return mMap.size();
}

const char* GcssKeyMap::key2str(const ia_uid key)
{
    std::map<std::string, ia_uid>::const_iterator it = mMap.begin();
    for (;it != mMap.end(); ++it)
        if (it->second == key)
            return it->first.c_str();
    return mMap.begin()->first.c_str();
}
ia_uid GcssKeyMap::str2key(const std::string &key_str)
{
    auto f = mMap.find(key_str);
    if (f != mMap.end())
        return f->second;
    return GCSS_KEY_NA;
}


/**
 * This method is only called once when the HAL library is loaded
 *
 * At this time we can load the XML config (camera3_prfiles.xml)and find the
 * CPF files for all the sensors. Once we load the CPF file
 * we fill the parts of the static metadata that is stored in the CMC
 *
 * Please note that the CpfStore objects are created once but not released
 * This is because this array is only freed if the process is destroyed.
 */
void PlatformData::init()
{
    LOGD("Camera HAL static init");

    if (mCameraHWInfo) {
        delete mCameraHWInfo;
        mCameraHWInfo = nullptr;
    }
    if (mInstance) {
        delete mInstance;
        mInstance = nullptr;
    }

    CLEAR(sKnownCPFConfigurations);

    if (mGcssKeyMap) {
        delete mGcssKeyMap;
        mGcssKeyMap = nullptr;
    }
    mGcssKeyMap = new GcssKeyMap;

    mCameraHWInfo = new CameraHWInfo();

    mInstance = new ChromeCameraProfiles(mCameraHWInfo);
    if (!mInstance) {
        LOGE("Not enough memory to create Camera profiles");
        return;
    }

    int ret = mInstance->init();
    if (ret != OK) {
        LOGE("Failed to initialize Camera profiles");
        delete mInstance;
        mInstance = nullptr;
        return;
    }

    int numberOfCameras = PlatformData::numberOfCameras();
    if (numberOfCameras == 0 || numberOfCameras > MAX_CPF_CACHED) {
        LOGE("Camera HAL Basic Platform initialization failed !!number of camera: %d", numberOfCameras);
        deinit();
        return;
    }

    /**
     * This number currently comes from the number if sections in the XML
     * in the future this is not reliable if we want to have multiple cameras
     * supported in the same XML.
     * TODO: add a common field in the XML that lists the camera's OR
     * query from driver at runtime
     */
    CpfStore *cpf;
    for (int i = 0; i < numberOfCameras; i++) {
        const CameraCapInfo *cci = PlatformData::getCameraCapInfo(i);
        if (cci == nullptr)
            continue;
        if (cci->sensorType() != SENSOR_TYPE_RAW)
            continue;

        int xmlIndex = PlatformData::getXmlCameraId(i);
        // Cpf is using id from XML because aiqb files are named using that id.
        cpf = new CpfStore(xmlIndex, mCameraHWInfo);
        /*
         * As we should not change static metadata on fly, one assumption
         * is that tuning team must make sure that the CMCs keep the same
         * between CPFs of the same sensor.
         * So here fillStaticMetadataFromCMC() is called just once for
         * filling the static metadata. So Please note that only for item 0,
         * AiqConf::mMetadata is initialized.
         */
        for (auto &aiqConfig : cpf->mAiqConfig)
            aiqConfig.second->initCMC();

        auto it = cpf->mAiqConfig.begin();
        if (it != cpf->mAiqConfig.end())
            it->second->fillStaticMetadataFromCMC(mInstance->mStaticMeta[i]);

        sKnownCPFConfigurations[i] = cpf;
    }
    LOGD("Camera HAL static init - Done!");
}

/**
 * This method is only called once when the HAL library is unloaded
 */
void PlatformData::deinit() {
    for (int i = 0; i < PlatformData::numberOfCameras(); i++) {
        delete sKnownCPFConfigurations[i];
        sKnownCPFConfigurations[i] = nullptr;
    }

    if (mGcssKeyMap) {
        delete mGcssKeyMap;
        mGcssKeyMap = nullptr;
    }

    if (mInstance) {
        delete mInstance;
        mInstance = nullptr;
    }
    if (mCameraHWInfo) {
        delete mCameraHWInfo;
        mCameraHWInfo = nullptr;
    }
}

/**
 * static acces method to implement the singleton
 * mInstance should have been instantiated when the library loaded.
 * Having nullptr is a serious error
 */
CameraProfiles *PlatformData::getInstance(void)
{
    if (mInstance == nullptr) {
        LOGE("@%s Failed to create CameraProfiles instance", __FUNCTION__);
        return nullptr;
    }
    return mInstance;
}

GcssKeyMap* PlatformData::getGcssKeyMap()
{
    return mGcssKeyMap;
}

int PlatformData::numberOfCameras(void)
{
    CameraProfiles * i = getInstance();

    if (!i)
        return 0;

    int num = (int)i->mStaticMeta.size();
    return (num <= MAX_CAMERAS) ? num : MAX_CAMERAS;
}

int PlatformData::getXmlCameraId(int cameraId)
{
    CameraProfiles * i = getInstance();

    if (!i)
        return -1;

    return (int)i->getXmlCameraId(cameraId);
}

const CameraCapInfo *PlatformData::getCameraCapInfoForXmlCameraId(int xmlCameraId)
{
    CameraProfiles * i = getInstance();

    if (!i)
        return nullptr;

    return i->getCameraCapInfoForXmlCameraId(xmlCameraId);
}


void PlatformData::getCameraInfo(int cameraId, struct camera_info * info)
{
    info->facing = facing(cameraId);
    info->orientation = orientation(cameraId);
    info->device_version = getCameraDeviceAPIVersion();
    info->static_camera_characteristics = getStaticMetadata(cameraId);
}

bool PlatformData::isCpfModeAvailable(int cameraId, string mode)
{
    auto it = sKnownCPFConfigurations[cameraId]->mAiqConfig.find(mode);
    return it != sKnownCPFConfigurations[cameraId]->mAiqConfig.end();
}

const AiqConf* PlatformData::getAiqConfiguration(int cameraId, string mode)
{
    auto it = sKnownCPFConfigurations[cameraId]->mAiqConfig.find(mode);
    if (it == sKnownCPFConfigurations[cameraId]->mAiqConfig.end()) {
       LOGE("mode %s does not map to any AiqConfig!- using default one", mode.c_str());
       return sKnownCPFConfigurations[cameraId]->mAiqConfig.begin()->second;
    }
    LOG1("%s: mode %s, Get AIQ configure", __FUNCTION__, mode.c_str());
    return it->second;
}

/**
 * Function converts the "lens.facing" android static meta data value to
 * value needed by camera service
 * Camera service uses different values from the android metadata
 * Refer system/core/include/system/camera.h
 */

int PlatformData::facing(int cameraId)
{
    uint8_t facing;
    CameraMetadata staticMeta;
    staticMeta = getStaticMetadata(cameraId);
    MetadataHelper::getMetadataValue(staticMeta, ANDROID_LENS_FACING, facing);
    facing = (facing == FRONT_CAMERA_ID) ? CAMERA_FACING_BACK : CAMERA_FACING_FRONT;

    return facing;
}

int PlatformData::orientation(int cameraId)
{
    int orientation;
    CameraMetadata staticMeta;
    staticMeta = getStaticMetadata(cameraId);
    MetadataHelper::getMetadataValue(staticMeta, ANDROID_SENSOR_ORIENTATION, orientation);

    return orientation;
}

/**
 * Retrieves the partial result count from the static metadata
 * This number is the pieces that we return the the result for a single
 * capture request. This number is specific to PSL implementations
 * It has to be at least 1.
 * \param cameraId[IN]: Camera Id  that we are querying the value for
 * \return value
 */
int PlatformData::getPartialMetadataCount(int cameraId)
{
    int partialMetadataCount = 0;
    CameraMetadata staticMeta;
    staticMeta = getStaticMetadata(cameraId);
    MetadataHelper::getMetadataValue(staticMeta,
            ANDROID_REQUEST_PARTIAL_RESULT_COUNT, partialMetadataCount);
    if (partialMetadataCount <= 0) {
        LOGW("Invalid value (%d) for ANDROID_REQUEST_PARTIAL_RESULT_COUNT"
                "FIX your config", partialMetadataCount);
        partialMetadataCount = 1;
    }
    return partialMetadataCount;
}

const camera_metadata_t * PlatformData::getStaticMetadata(int cameraId)
{
    if (cameraId >= numberOfCameras()) {
        LOGE("ERROR @%s: Invalid camera: %d", __FUNCTION__, cameraId);
        cameraId = 0;
        return nullptr;
    }

    CameraProfiles * i = getInstance();

    if (!i)
        return nullptr;

    camera_metadata_t * metadata = i->mStaticMeta[cameraId];
    return metadata;
}

camera_metadata_t *PlatformData::getDefaultMetadata(int cameraId, int requestType)
{
    if (cameraId >= numberOfCameras()) {
        LOGE("ERROR @%s: Invalid camera: %d", __FUNCTION__, cameraId);
        cameraId = 0;
    }
    CameraProfiles * i = getInstance();

    if (!i)
        return nullptr;

    return i->constructDefaultMetadata(cameraId, requestType);
}

const CameraCapInfo * PlatformData::getCameraCapInfo(int cameraId)
{
    // Use MAX_CAMERAS instead of numberOfCameras() as it will cause a recursive loop
    if (cameraId > MAX_CAMERAS) {
        LOGE("ERROR @%s: Invalid camera: %d", __FUNCTION__, cameraId);
        cameraId = 0;
    }
    CameraProfiles * i = getInstance();

    if (!i)
        return nullptr;

    return i->getCameraCapInfo(cameraId);
}

/**
 * getDeviceIds
 * returns a vector of strings with a list of names to identify the device
 * the HAL is running on.
 * The list has the most specific name first and then more generic names as
 * fallback
 * The values for this strings are:
 * - If the platform supports spid the first string is a concatenation of the
 * vendor_id + platform_family_id + produc_line_id. This is always the first
 * for backwards compatibility reasons.
 *
 *  This list can be used to find the correct configuration file, either CPF or
 *  camera XML (camera3_profiles)
 *
 *  Please Note:
 *  If in non-spid platforms the identifiers in the system properties are not
 *  precise enough and a new property is used, it should be returned first.
 *
 */
status_t PlatformData::getDeviceIds(std::vector<string> &names)
{
    status_t status = OK;
    char prop[PATH_MAX] = {0};
    static const char *deviceIdKeys[] = {
       "ro.product.device",
       "ro.product.board",
       "ro.board.platform",
    };
    static const int DEVICE_ID_KEYS_COUNT =
        (sizeof(deviceIdKeys)/sizeof(deviceIdKeys[0]));

    for (int i = 0; i < DEVICE_ID_KEYS_COUNT ; i++) {
        if (LogHelper::__getEnviromentValue(deviceIdKeys[i], prop, sizeof(prop)) == 0)
            continue;

        names.push_back(string(prop));
    }

    return OK;
}

CameraHwType PlatformData::getCameraHwType(int cameraId)
{
    CameraProfiles * i = getInstance();

    if (!i)
        return SUPPORTED_HW_UNKNOWN;

    return i->getCameraHwforId(cameraId);
}

const char* PlatformData::boardName(void)
{
    return mCameraHWInfo->boardName();
}

const char* PlatformData::productName(void)
{
    return mCameraHWInfo->productName();
}

const char* PlatformData::manufacturerName(void)
{
    return mCameraHWInfo->manufacturerName();
}

/**
 *
 * retrieves the AIQD data file path saved on the host file system
 *
 * AIQD is triggered to store into filesystem by 3a to rembmer 3a parameters before
 * the camera exist, 3a will read the AIQD file for better 3a quality when camera starts
 *
 * \param[IN] sensorName the name of the sensor
 *
 * \return the AIQD data file path on the host file system
 */
std::string PlatformData::getAiqdFileName(const std::string &sensorName)
{
    std::string aiqdFileName = "";

    if (!sensorName.empty()) {
        aiqdFileName.append(gDumpPath);
        aiqdFileName.append(sensorName);
        aiqdFileName.append(".aiqd");
    }

    return aiqdFileName;
}

bool PlatformData::supportDualVideo(void)
{
    return mCameraHWInfo->supportDualVideo();
}

int PlatformData::getCameraDeviceAPIVersion(void)
{
    return mCameraHWInfo->getCameraDeviceAPIVersion();
}

bool PlatformData::supportExtendedMakernote(void)
{
    return mCameraHWInfo->supportExtendedMakernote();
}

bool PlatformData::supportFullColorRange(void)
{
    return mCameraHWInfo->supportFullColorRange();
}

bool PlatformData::supportIPUAcceleration(void)
{
    return mCameraHWInfo->supportIPUAcceleration();
}

unsigned int PlatformData::getNumOfCPUCores()
{
    LOG1("@%s, line:%d", __FUNCTION__, __LINE__);
    unsigned int cpuCores = 1;

    char buf[20];
    FILE *cpuOnline = fopen("/sys/devices/system/cpu/online", "r");
    if (cpuOnline) {
        CLEAR(buf);
        size_t size = fread(buf, 1, sizeof(buf), cpuOnline);
        if (size != sizeof(buf)) {
            LOGW("Failed to read number of CPU's ");
        }
        buf[sizeof(buf) - 1] = '\0';
        char *p = strchr(buf, '-');
        if (p)
            cpuCores = 1 + atoi(p + 1);
        else
            cpuCores = 1;
        fclose(cpuOnline);
    }
    LOG1("@%s, line:%d, cpu core number:%d", __FUNCTION__, __LINE__, cpuCores);
    return cpuCores;
}

status_t PlatformData::readSpId(string& spIdName, unsigned int& spIdValue)
{
    FILE *file;
    status_t ret = OK;
    string sysfsSpIdPath = "/sys/spid/";
    string fullPath;

    fullPath = sysfsSpIdPath;
    fullPath.append(spIdName);

    file = fopen(fullPath.c_str(), "rb");
    if (!file) {
        LOGE("ERROR in opening file %s", fullPath.c_str());
        return NAME_NOT_FOUND;
    }
    ret = fscanf(file, "%x", &spIdValue);
    if (ret < 0) {
        LOGE("ERROR in reading %s", fullPath.c_str());
        spIdValue = 0;
        fclose(file);
        return UNKNOWN_ERROR;
    }
    fclose(file);
    return ret;
}

/**
 * getActivePixelArray
 *
 * retrieves the Active Pixel Array (APA) static metadata entry and initializes
 * a CameraWindow structure correctly from it.
 * APA is defined as a rectangle that stores the values as
 * (xmin,ymin, width,height)
 *
 * \param cameraId [IN]: Camera id of the device to query the APA
 * \returns CameraWindow initialized with the APA or empty if it was not found
 *                       but this should not happen
 */
CameraWindow PlatformData::getActivePixelArray(int cameraId)
{
    CameraWindow apa;
    camera_metadata_ro_entry entry;
    CLEAR(apa);
    const camera_metadata *staticMeta = getStaticMetadata(cameraId);
    if (CC_UNLIKELY(staticMeta == nullptr)) {
        LOGE("@%s: Invalid camera id (%d) could not get static metadata",
                __FUNCTION__, cameraId);
        return apa;
    }

    find_camera_metadata_ro_entry(staticMeta,
                                  ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE,
                                  &entry);
    if (entry.count >= 4) {
        ia_coordinate topLeft;
        INIT_COORDINATE(topLeft,entry.data.i32[0],entry.data.i32[1]);
        apa.init(topLeft,
                 entry.data.i32[2], //width
                 entry.data.i32[3], //height
                 0);
    } else {
        LOGE("could not find ACTIVE_ARRAY_SIZE- INVALID XML configuration!!");
    }
    return apa;
}

float PlatformData::getStepEv(int cameraId)
{
    // Get the ev step
    CameraMetadata staticMeta;
    float stepEV = 1 / 3.0f;
    int count = 0;
    staticMeta = getStaticMetadata(cameraId);
    camera_metadata_rational_t* aeCompStep =
        (camera_metadata_rational_t*)MetadataHelper::getMetadataValues(staticMeta,
                                     ANDROID_CONTROL_AE_COMPENSATION_STEP,
                                     TYPE_RATIONAL,
                                     &count);
    if (count == 1 && aeCompStep != nullptr) {
        stepEV = (float)aeCompStep->numerator / aeCompStep->denominator;
    }
    return stepEV;

}

/**
 * static convenience getter for cpf and cmc data.
 */
status_t PlatformData::getCpfAndCmc(ia_binary_data &cpfData,
                                    ia_cmc_t* &cmcData,
                                    int cameraId,
                                    string mode)
{
    const AiqConf* aiqConf = getAiqConfiguration(cameraId, mode);
    if (CC_UNLIKELY(aiqConf == nullptr)) {
        LOGE("CPF file was not initialized ");
        return NO_INIT;
    }
    cpfData.data = aiqConf->ptr();
    cpfData.size = aiqConf->size();

    cmcData = const_cast<AiqConf*>(aiqConf)->getCMCHandler();
    if CC_UNLIKELY(cmcData == 0) {
        LOGE("Could not parse cmc data.");
        return NO_INIT;
    }

    return OK;
}

CameraHWInfo::CameraHWInfo() :
        mMainDevicePathName(DEFAULT_MAIN_DEVICE),
        mHasMediaController(false)
{
    mBoardName = "<not set>";
    mProductName = "<not_set>";
    mManufacturerName = "<not set>";
    mCameraDeviceAPIVersion = CAMERA_DEVICE_API_VERSION_3_3;
    mSupportDualVideo = false;
    mSupportExtendedMakernote = false;
    mSupportFullColorRange = true;
    mSupportIPUAcceleration = false;

    // -1 means mPreviewHALFormat is not set
    mPreviewHALFormat = -1;

    CLEAR(mDeviceInfo);
}

status_t CameraHWInfo::init(const std::string &mediaDevicePath)
{
    mMediaControllerPathName = mediaDevicePath;
    return initDriverList();
}

status_t CameraHWInfo::initDriverList()
{
    LOG1("@%s", __FUNCTION__);
    status_t ret = OK;
    if (mSensorInfo.size() > 0) {
        // We only need to go through the drivers once
        return OK;
    }

    // check whether we are in a platform that supports media controller (mc)
    // or in one where a main device (md) can enumerate the sensors
    struct stat sb;

    for (int retryTimes = RETRY_COUNTER; retryTimes >= 0; retryTimes--) {
        if (retryTimes > 0) {
            // Because module loading is delayed also need to delay HAL initialization
            usleep(KERNEL_MODULE_LOAD_DELAY);
        }
    }

    int mcExist = stat(mMediaControllerPathName.c_str(), &sb);

    if (mcExist == 0) {
        mHasMediaController = true;
        ret = findMediaControllerSensors();
        ret |= findMediaDeviceInfo();
    } else {
        LOGE("Could not find sensor names");
        ret = NO_INIT;
    }


    for (unsigned i = 0 ;i < mSensorInfo.size(); ++i)
        LOG1("@%s, mSensorName:%s, mDeviceName:%s, port:%d", __FUNCTION__,
            mSensorInfo[i].mSensorName.c_str(), mSensorInfo[i].mDeviceName.c_str(), mSensorInfo[i].mIspPort);

    return ret;
}


status_t CameraHWInfo::findMediaControllerSensors()
{
    status_t ret = OK;
    int fd = open(mMediaControllerPathName.c_str(), O_RDONLY);
    if (fd == -1) {
        LOGW("Could not openg media controller device: %s!", strerror(errno));
        return ENXIO;
    }

    struct media_entity_desc entity;
    CLEAR(entity);
    do {
        // Go through the list of media controller entities
        entity.id |= MEDIA_ENT_ID_FLAG_NEXT;
        if (ioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &entity) < 0) {
            if (errno == EINVAL) {
                // Ending up here when no more entities left.
                // Will simply 'break' if everything was ok
                if (mSensorInfo.size() == 0) {
                    // No registered drivers found
                    LOGE("ERROR no sensor driver registered in media controller!");
                    ret = NO_INIT;
                }
            } else {
                LOGE("ERROR in browsing media controller entities: %s!", strerror(errno));
                ret = FAILED_TRANSACTION;
            }
            break;
        } else {
            if (entity.type == MEDIA_ENT_T_V4L2_SUBDEV_SENSOR) {
                // A driver has been found!
                // The driver is using sensor name when registering
                // to media controller (we will truncate that to
                // first space, if any)
                SensorDriverDescriptor drvInfo;
                drvInfo.mSensorName = entity.name;
                drvInfo.mSensorDevType = SENSOR_DEVICE_MC;

                unsigned major = entity.v4l.major;
                unsigned minor = entity.v4l.minor;

                // Go through the subdevs one by one, see which one
                // corresponds to this driver (if there is an error,
                // the looping ends at 'while')
                ret = initDriverListHelper(major, minor, drvInfo);
            }
        }
    } while (!ret);

    if (close(fd)) {
        LOGE("ERROR in closing media controller: %s!", strerror(errno));
        if (!ret) ret = EPERM;
    }

    return ret;
}

status_t CameraHWInfo::findMediaDeviceInfo()
{
    status_t ret = OK;
    int fd = open(mMediaControllerPathName.c_str(), O_RDONLY);
    if (fd == -1) {
        LOGW("Could not openg media controller device: %s!", strerror(errno));
        return UNKNOWN_ERROR;
    }

    CLEAR(mDeviceInfo);
    if (ioctl(fd, MEDIA_IOC_DEVICE_INFO, &mDeviceInfo) < 0) {
        LOGE("ERROR in browsing media device information: %s!", strerror(errno));
        ret = FAILED_TRANSACTION;
    } else {
        LOG1("Media device: %s", mDeviceInfo.driver);
    }

    if (close(fd)) {
        LOGE("ERROR in closing media controller: %s!", strerror(errno));

        if (!ret) {
            ret = PERMISSION_DENIED;
        }
    }

    return ret;
}

/**
 * Function to get the CSI port number a sensor is connected to.
 * Use mediacontroller to go through the links to find the CSI entity
 * and trim the port number from it.
 *
 * \param[IN] deviceName sensor full name
 * \param[OUT] portId CSI port number
 *
 */
status_t CameraHWInfo::getCSIPortID(const std::string &deviceName, int &portId)
{
    LOG1("@%s", __FUNCTION__);
    status_t status = NO_ERROR;
    std::shared_ptr<MediaEntity> mediaEntity = nullptr;
    std::vector<string> names;
    string name;
    std::vector<string> nameTemplateVec;

    // Kernel driver should follow one of these 3 templates to report the CSI port, else this
    // parsing will fail. The format is <...CSI...> port-number
    const char *CSI_RX_PORT_NAME_TEMPLATE1 = "CSI-2";
    const char *CSI_RX_PORT_NAME_TEMPLATE2 = "CSI2-port";
    const char *CSI_RX_PORT_NAME_TEMPLATE3 = "TPG";
    nameTemplateVec.push_back(CSI_RX_PORT_NAME_TEMPLATE1);
    nameTemplateVec.push_back(CSI_RX_PORT_NAME_TEMPLATE2);
    nameTemplateVec.push_back(CSI_RX_PORT_NAME_TEMPLATE3);

    std::shared_ptr<MediaController> mediaCtl = std::make_shared<MediaController>(mMediaControllerPathName.c_str());
    if (!mediaCtl) {
        LOGE("Error creating MediaController");
        return UNKNOWN_ERROR;
    }

    status = mediaCtl->init();
    if (status != NO_ERROR) {
        LOGE("Error initializing Media Controller");
        return status;
    }

    status = mediaCtl->getMediaEntity(mediaEntity, deviceName.c_str());
    if (status != NO_ERROR) {
        LOGE("Failed to get media entity by sensor name %s", deviceName.c_str());
        return status;
    }

    if (mediaEntity->getType() != SUBDEV_SENSOR) {
        LOGE("Media entity not sensor type");
        return UNKNOWN_ERROR;
    }

    // Traverse the sinks till we get CSI port
    while (1) {
        names.clear();
        status = mediaCtl->getSinkNamesForEntity(mediaEntity, names);
        if (status != NO_ERROR) {
            LOGE("Error getting sink names for entity %s", mediaEntity->getName());
            return UNKNOWN_ERROR;
        }

        // For sensor type there will be only one sink
        if (names.size() != 1) {
            LOGW("Number of sinks for sensor not 1 it is %d", names.size());
        }
        if (names.size() == 0) {
            LOGW("No sink names available for %s", deviceName.c_str());
            return 0;
        }

        name = names[0];
        size_t pos = 0;
        for (auto &nameTemplate : nameTemplateVec) {
            // check if name is CSI port
            if ((pos = name.find(nameTemplate)) != string::npos) {
                LOG2("found CSI port name = %s", name.c_str());
                // Trim the port id from CSI port name
                name = name.substr(pos + nameTemplate.length());
                portId = atoi(name.c_str());
                if (portId < 0) {
                    LOGE("Error getting port id %d", portId);
                    return UNKNOWN_ERROR;
                }
                return status;
            }
        }
        // Get media entity for new name
        mediaEntity = nullptr;
        status = mediaCtl->getMediaEntity(mediaEntity, name.c_str());
        if (status != NO_ERROR) {
            LOGE("Failed to get media entity by name %s", name.c_str());
            break;
        }
    }

    return status;
}

/**
 * Get all available sensor modes from the driver
 *
 * Function gets all currently available sensor modes from the driver
 * and returns them in a vector.
 *
 * \param[in] sensorName    Name of the sensor
 * \param[out] sensorModes  Vector of available sensor modes for this sensor
 */
status_t CameraHWInfo::getAvailableSensorModes(const std::string &sensorName,
                                               SensorModeVector &sensorModes) const
{
    status_t ret = NO_ERROR;
    const char *devname;
    std::string sDevName;

    for (size_t i = 0; i < mSensorInfo.size(); i++) {
        if (mSensorInfo[i].mSensorName != sensorName)
            continue;

        std::ostringstream stringStream;
        stringStream << "/dev/" << mSensorInfo[i].mDeviceName.c_str();
        sDevName = stringStream.str();
    }
    devname = sDevName.c_str();
    std::shared_ptr<V4L2Subdevice> device = std::make_shared<V4L2Subdevice>(devname);
    if (device.get() == nullptr) {
        LOGE("Couldn't open device %s", devname);
        return UNKNOWN_ERROR;
    }

    ret = device->open();
    if (ret != NO_ERROR) {
        LOGE("Error opening device (%s)", devname);
        return ret;
    }

    // Get query control for sensor mode to determine max value
    v4l2_queryctrl sensorModeControl;
    CLEAR(sensorModeControl);
    sensorModeControl.id = CRL_CID_SENSOR_MODE;
    ret = device->queryControl(sensorModeControl);
    if (ret != NO_ERROR) {
        LOGE("Couldn't get sensor mode range");
        device->close();
        return UNKNOWN_ERROR;
    }
    uint32_t max = sensorModeControl.maximum;
    v4l2_querymenu menu;
    CLEAR(menu);
    menu.id = CRL_CID_SENSOR_MODE;

    // Loop through menu and add indexes and names to vector
    for (menu.index = 0; menu.index <= max; menu.index++) {

        ret = (device->queryMenu(menu));
        if (ret != NO_ERROR) {
            LOGE("Error opening query menu at index: %d", menu.index);
        } else {
            sensorModes.push_back(std::make_pair(uint32_t(menu.index),
                                  reinterpret_cast<char *>(menu.name)));
        }
    }
    ret = device->close();
    if (ret != NO_ERROR)
        LOGE("Error closing device (%s)", devname);

    return NO_ERROR;
}

status_t CameraHWInfo::initDriverListHelper(unsigned major, unsigned minor, SensorDriverDescriptor& drvInfo)
{
    LOG1("@%s", __FUNCTION__);
    int portId = 0;
    status_t status = UNKNOWN_ERROR;
    std::size_t pos = string::npos;

    string subdevPathName = "/dev/v4l-subdev";
    string subdevPathNameN;

    for (int n = 0; n < MAX_SUBDEV_ENUMERATE; n++) {
        subdevPathNameN = subdevPathName + std::to_string(n);
        struct stat fileInfo;
        CLEAR(fileInfo);
        if (stat(subdevPathNameN.c_str(), &fileInfo) < 0) {
            if (errno == ENOENT) {
                // We end up here when there is no Nth subdevice
                // but there might be more subdevices, so continue.
                // For an example if there are v4l subdevices 0, 4, 5 and 6
                // we come here for subdevices 1, 2 and 3.
                LOGI("Subdev missing: \"%s\"!", subdevPathNameN.c_str());
                continue;
            } else {
                LOGE("ERROR querying sensor subdev filestat for \"%s\": %s!",
                     subdevPathNameN.c_str(), strerror(errno));
                return FAILED_TRANSACTION;
            }
        }
        if ((major == MAJOR(fileInfo.st_rdev)) && (minor == MINOR(fileInfo.st_rdev))) {
            drvInfo.mDeviceName = subdevPathNameN;
            pos = subdevPathNameN.rfind('/');
            if (pos != std::string::npos)
                drvInfo.mDeviceName = subdevPathNameN.substr(pos + 1);

            drvInfo.mIspPort = (ISP_PORT)n; // Unused for media-ctl sensors
            status = getCSIPortID(drvInfo.mSensorName, portId);
            if (status != NO_ERROR) {
                LOGE("error getting CSI port id %d", portId);
                return status;
            }

            /*
             * Parse i2c address from sensor name.
             * It is the last word in the sensor name string, so find last
             * space and take the rest of the string.
             */
            pos = drvInfo.mSensorName.rfind(" ");
            drvInfo.mI2CAddress = drvInfo.mSensorName.substr(pos + 1);

            /*
             *  Now that we are done using the sensor name cut the name to
             *  first space, to get the actual name. First we check if
             *  it is tpg.
             */
            size_t i = drvInfo.mSensorName.find("TPG");
            if (CC_LIKELY(i != std::string::npos)) {
                drvInfo.mSensorName = drvInfo.mSensorName.substr(i,3);
                drvInfo.csiPort = portId;
                /* Because of several ports for TPG in media entity,
                 * just use port 0 as source input.
                 */
                if (drvInfo.csiPort == 0)
                    mSensorInfo.push_back(drvInfo);

            } else {
                i = drvInfo.mSensorName.find(" ");
                if (CC_LIKELY(i != std::string::npos)) {
                    drvInfo.mSensorName = drvInfo.mSensorName.substr(0, i);
                } else {
                    LOGW("Could not extract sensor name correctly");
                }

                drvInfo.csiPort = portId;
                mSensorInfo.push_back(drvInfo);
            }
            LOG1("Registered sensor driver \"%s\" found for sensor \"%s\", CSI port:%d",
                 drvInfo.mDeviceName.c_str(), drvInfo.mSensorName.c_str(),
                 drvInfo.csiPort);
            // All ok
            break;
        }
    }

    return OK;
}
} NAMESPACE_DECLARATION_END
