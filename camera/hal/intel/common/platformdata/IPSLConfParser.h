
/*
 * Copyright (C) 2015-2017 Intel Corporation
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

#ifndef _CAMERA3_HAL_PSLCONFPARSER_H_
#define _CAMERA3_HAL_PSLCONFPARSER_H_

#include <string>
#include <vector>

NAMESPACE_DECLARATION {
typedef std::vector<SensorDriverDescriptor> SensorNameVector;

// forward declaration
class CameraCapInfo;

/**
 * \class IPSLConfParser
 *
 * Base class for all the per sensor PSL parsers.
 *
 * It implements the parsing of XML configuration common sections for all
 * PSL's that are sensor specific.
 *
 * The ICameraHw::getPSLParser shall return a value of this type
 *
 */
class IPSLConfParser {
public:
    explicit IPSLConfParser(std::string &xmlName,
                  const SensorNameVector &sensorNames = SensorNameVector())
                        { mXmlFileName = xmlName; mDetectedSensors = sensorNames;}
    virtual ~IPSLConfParser() {};

    virtual CameraCapInfo* getCameraCapInfo(int cameraId) = 0;
    virtual camera_metadata_t *constructDefaultMetadata(int cameraId, int reqTemplate) = 0;

public:
    static const char *getSensorMediaDeviceName() { return "nullptr"; }

protected:  /* types */
    static const size_t SECTION_NAME_MAX_LENGTH = 64;

protected: /* methods */
    static int getPixelFormatAsValue(const char* format);

protected:
    std::string mXmlFileName;
    std::vector<SensorDriverDescriptor> mDetectedSensors;
    std::vector<CameraCapInfo *> mCaps;
    std::vector<camera_metadata_t *> mDefaultRequests;
};

} NAMESPACE_DECLARATION_END
#endif  // _CAMERA3_HAL_PSLCONFPARSER_H_
