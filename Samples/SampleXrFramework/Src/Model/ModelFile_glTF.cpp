/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * Licensed under the Oculus SDK License Agreement (the "License");
 * you may not use the Oculus SDK except in compliance with the License,
 * which is provided at the time of installation or download, or which
 * otherwise accompanies this software in either electronic or hard copy form.
 *
 * You may obtain a copy of the License at
 * https://developer.oculus.com/licenses/oculussdk/
 *
 * Unless required by applicable law or agreed to in writing, the Oculus SDK
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/************************************************************************************

Filename    :   ModelFile_OvrScene.cpp
Content     :   Model file loading glTF elements.
Created     :   December 2013
Authors     :   John Carmack, J.M.P. van Waveren

*************************************************************************************/

#include "Model/ModelDef.h"
#include "ModelFileLoading.h"

#include "OVR_Std.h"
#include "OVR_JSON.h"
#include "StringUtils.h"

#include "Misc/Log.h"
#include "OVR_BinaryFile2.h"

#include <unordered_map>

// #include "Render/Egl.h"

/// Aliasing some GL constant for defaults and commonly used behavior
/// without needing the explicit GL include

#ifndef GL_BYTE
#define GL_BYTE 0x1400
#endif
#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE 0x1401
#endif
#ifndef GL_SHORT
#define GL_SHORT 0x1402
#endif
#ifndef GL_UNSIGNED_SHORT
#define GL_UNSIGNED_SHORT 0x1403
#endif
#ifndef GL_INT
#define GL_INT 0x1404
#endif
#ifndef GL_UNSIGNED_INT
#define GL_UNSIGNED_INT 0x1405
#endif
#ifndef GL_FLOAT
#define GL_FLOAT 0x1406
#endif

using OVR::Bounds3f;
using OVR::Matrix4f;
using OVR::Quatf;
using OVR::Vector2f;
using OVR::Vector3f;
using OVR::Vector4f;

namespace OVRFW {

#define GLTF_BINARY_MAGIC (('g' << 0) | ('l' << 8) | ('T' << 16) | ('F' << 24))
#define GLTF_BINARY_VERSION 2
#define GLTF_BINARY_CHUNKTYPE_JSON 0x4E4F534A
#define GLTF_BINARY_CHUNKTYPE_BINARY 0x004E4942

typedef struct glTFBinaryHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t length;
} glTFBinaryHeader;

static uint8_t*
ReadBufferFromZipFile(unzFile zfp, const uint8_t* fileData, const unz_file_info& finfo) {
    const int size = finfo.uncompressed_size;
    uint8_t* buffer = nullptr;

    if (unzOpenCurrentFile(zfp) != UNZ_OK) {
        return buffer;
    }

    if (finfo.compression_method == 0 && fileData != nullptr) {
        buffer = (uint8_t*)fileData + unzGetCurrentFileZStreamPos64(zfp);
    } else {
        buffer = new uint8_t[size + 1];
        buffer[size] = '\0'; // always zero terminate text files

        if (unzReadCurrentFile(zfp, buffer, size) != size) {
            delete[] buffer;
            buffer = nullptr;
        }
    }

    return buffer;
}

static uint8_t* ReadFileBufferFromZipFile(
    unzFile zfp,
    const char* fileName,
    int& bufferLength,
    const uint8_t* fileData) {
    for (int ret = unzGoToFirstFile(zfp); ret == UNZ_OK; ret = unzGoToNextFile(zfp)) {
        unz_file_info finfo;
        char entryName[256];
        unzGetCurrentFileInfo(zfp, &finfo, entryName, sizeof(entryName), nullptr, 0, nullptr, 0);

        if (OVR::OVR_stricmp(entryName, fileName) == 0) {
            bufferLength = finfo.uncompressed_size;
            uint8_t* buffer = ReadBufferFromZipFile(zfp, fileData, finfo);
            return buffer;
        }
    }

    bufferLength = 0;
    return nullptr;
}

static void ParseIntArray(int* elements, const int count, const OVR::JsonReader arrayNode) {
    int i = 0;
    if (arrayNode.IsArray()) {
        while (!arrayNode.IsEndOfArray() && i < count) {
            auto node = arrayNode.GetNextArrayElement();
            elements[i] = node->GetInt32Value();
            i++;
        }
    }

    for (; i < count; i++) {
        elements[i] = 0;
    }
}

static void ParseFloatArray(float* elements, const int count, OVR::JsonReader arrayNode) {
    int i = 0;
    if (arrayNode.IsArray()) {
        while (!arrayNode.IsEndOfArray() && i < count) {
            auto node = arrayNode.GetNextArrayElement();
            elements[i] = node->GetFloatValue();
            i++;
        }
    }

    for (; i < count; i++) {
        elements[i] = 0.0f;
    }
}

static size_t getComponentCount(ModelAccessorType type) {
    switch (type) {
        case ACCESSOR_SCALAR:
            return 1;
        case ACCESSOR_VEC2:
            return 2;
        case ACCESSOR_VEC3:
            return 3;
        case ACCESSOR_VEC4:
            return 4;
        case ACCESSOR_MAT2:
            return 4;
        case ACCESSOR_MAT3:
            return 9;
        case ACCESSOR_MAT4:
            return 16;
        case ACCESSOR_UNKNOWN:
        default:
            return 0;
    }
}

static size_t getComponentSize(int componentType) {
    switch (componentType) {
        case MODEL_COMPONENT_TYPE_UNSIGNED_BYTE:
        case MODEL_COMPONENT_TYPE_BYTE:
            return 1;
        case MODEL_COMPONENT_TYPE_SHORT:
        case MODEL_COMPONENT_TYPE_UNSIGNED_SHORT:
            return 2;
        case MODEL_COMPONENT_TYPE_UNSIGNED_INT:
        case MODEL_COMPONENT_TYPE_FLOAT:
            return 4;
        default:
            return 0;
    }
}

template <typename _type_>
bool ReadSurfaceDataFromAccessor(
    std::vector<_type_>& out,
    ModelFile& modelFile,
    const int index,
    const ModelAccessorType type,
    const int componentType,
    const int count,
    const bool append) {
    bool loaded = true;
    if (index >= 0) {
        if (index >= int(modelFile.Accessors.size())) {
            ALOGW(
                "Error: Invalid index on gltfPrimitive accessor %d %d",
                index,
                static_cast<int>(modelFile.Accessors.size()));
            loaded = false;
        }

        const ModelAccessor* accessor = &(modelFile.Accessors[index]);
        const ModelBufferView* bufferView = accessor->bufferView;
        const ModelBuffer* buffer = bufferView->buffer;

        if (count >= 0 && accessor->count != count) {
            ALOGW(
                "Error: Invalid count on gltfPrimitive accessor %d %d %d",
                index,
                count,
                accessor->count);
            loaded = false;
        }
        if (accessor->type != type) {
            ALOGW(
                "Error: Invalid type on gltfPrimitive accessor %d %d %d",
                index,
                type,
                accessor->type);
            loaded = false;
        }

        size_t srcComponentSize = getComponentSize(accessor->componentType);
        size_t srcComponentCount = getComponentCount(accessor->type);
        size_t srcValueSize = srcComponentSize * srcComponentCount;

        int readStride = srcValueSize;
        if (bufferView->byteStride > 0) {
            readStride = bufferView->byteStride;
        }

        size_t dstComponentSize = getComponentSize(componentType);
        size_t dstComponentCount = getComponentCount(type);
        size_t dstValueSize = dstComponentSize * dstComponentCount;

        const size_t offset = accessor->byteOffset + bufferView->byteOffset;

        const size_t srcRequiredSize = accessor->count * readStride;

        if ((offset + srcRequiredSize) > buffer->byteLength) {
            ALOGW(
                "Error: accessor requesting too much data in gltfPrimitive %d %d %d",
                index,
                (int)accessor->bufferView->byteLength,
                (int)(offset + srcRequiredSize));
            loaded = false;
        }

        if (loaded) {
            const size_t startIndex = append ? out.size() : 0;
            out.resize(startIndex + accessor->count);
            int valueCount = (int)(accessor->count);
            const char* src = (char*)buffer->bufferData.data() + offset;
            if (accessor->componentType != componentType) {
                if (componentType == MODEL_COMPONENT_TYPE_FLOAT) {
                    float* dst = (float*)&out[0];
                    // for normalized signed integers, we need them to map to whole [-1.0f, 1.0f]
                    // while having the 0 exactly at 0.0f for byte:
                    //-128 -> - 1.0f
                    // 0 ->  0.0f
                    // 127 ->  1.0f
                    // to achieve that we do std::max((float)value / MaxValue, -1.0f);
                    switch (accessor->componentType) {
                        case MODEL_COMPONENT_TYPE_BYTE:
                            for (int i = 0; i < valueCount; i++) {
                                const int8_t* valueSrc = (int8_t*)(src + i * readStride);
                                float* valueDst = (float*)(dst + i * srcComponentCount);
                                for (int j = 0; j < (int)srcComponentCount; j++) {
                                    valueDst[j] = std::max(((float)valueSrc[j]) / 127.0f, -1.0f);
                                }
                            }
                            break;
                        case MODEL_COMPONENT_TYPE_UNSIGNED_BYTE:
                        default:
                            for (int i = 0; i < valueCount; i++) {
                                const uint8_t* valueSrc = (uint8_t*)(src + i * readStride);
                                float* valueDst = (float*)(dst + i * srcComponentCount);
                                for (int j = 0; j < (int)srcComponentCount; j++) {
                                    valueDst[j] = ((float)valueSrc[j]) / 255.0f;
                                }
                            }
                            break;
                        case MODEL_COMPONENT_TYPE_SHORT:
                            for (int i = 0; i < valueCount; i++) {
                                const int16_t* valueSrc = (int16_t*)(src + i * readStride);
                                float* valueDst = (float*)(dst + i * srcComponentCount);
                                for (int j = 0; j < (int)srcComponentCount; j++) {
                                    valueDst[j] = std::max(((float)valueSrc[j]) / 32767.0f, -1.0f);
                                }
                            }
                            break;
                        case MODEL_COMPONENT_TYPE_UNSIGNED_SHORT:
                            for (int i = 0; i < valueCount; i++) {
                                const uint16_t* valueSrc = (uint16_t*)(src + i * readStride);
                                float* valueDst = (float*)(dst + i * srcComponentCount);
                                for (int j = 0; j < (int)srcComponentCount; j++) {
                                    valueDst[j] = ((float)valueSrc[j]) / 65535.0f;
                                }
                            }
                            break;
                        case MODEL_COMPONENT_TYPE_UNSIGNED_INT:
                            for (int i = 0; i < valueCount; i++) {
                                const uint32_t* valueSrc = (uint32_t*)(src + i * readStride);
                                float* valueDst = (float*)(dst + i * srcComponentCount);
                                for (int j = 0; j < (int)srcComponentCount; j++) {
                                    valueDst[j] = (float)(((double)valueSrc[j]) / 4294967295.0);
                                }
                            }
                            break;
                    }
                } else {
                    // slow path for rare cases
                    if (accessor->componentType == MODEL_COMPONENT_TYPE_FLOAT ||
                        componentType == MODEL_COMPONENT_TYPE_FLOAT) {
                        char* dst = (char*)&out[0];
                        for (int i = 0; i < valueCount; i++) {
                            for (int j = 0; j < (int)srcComponentCount; j++) {
                                float value;
                                const char* valueSrc = src + i * readStride + j * srcComponentSize;
                                switch (accessor->componentType) {
                                    case MODEL_COMPONENT_TYPE_BYTE:
                                        value =
                                            std::max(((float)(*(int8_t*)valueSrc)) / 127.0f, -1.0f);
                                        break;
                                    case MODEL_COMPONENT_TYPE_UNSIGNED_BYTE:
                                    default:
                                        value = ((float)(*(uint8_t*)valueSrc)) / 255.0f;
                                        break;
                                    case MODEL_COMPONENT_TYPE_SHORT:
                                        value = std::max(
                                            ((float)(*(int16_t*)valueSrc)) / 32767.0f, -1.0f);
                                        break;
                                    case MODEL_COMPONENT_TYPE_UNSIGNED_SHORT:
                                        value = ((float)(*(uint16_t*)valueSrc)) / 65535.0f;
                                        break;
                                    case MODEL_COMPONENT_TYPE_UNSIGNED_INT:
                                        value = (float)(((double)(*(uint32_t*)valueSrc)) /
                                                        4294967295.0);
                                        break;
                                    case MODEL_COMPONENT_TYPE_FLOAT:
                                        value = (*(float*)valueSrc);
                                        break;
                                }
                                char* valueDst = dst + i * dstValueSize + j * dstComponentSize;
                                switch (componentType) {
                                    case MODEL_COMPONENT_TYPE_BYTE:
                                        // -1.0f -> -128, 1.0f -> +127, 0.0f -> 0
                                        *(int8_t*)valueDst = (int8_t)(value * 128.0f);
                                        break;
                                    case MODEL_COMPONENT_TYPE_UNSIGNED_BYTE:
                                    default:
                                        *(uint8_t*)valueDst = (uint8_t)(value * 255.0f);
                                        break;
                                        // SPECIAL CASES, we don't know if the float is normalized
                                        // or not normalized we assume that the float was not a
                                        // normalized value when someone asks for an uint16 or
                                        // uint32
                                    case MODEL_COMPONENT_TYPE_SHORT:
                                        *(int16_t*)valueDst = (int16_t)(value);
                                        break;
                                    case MODEL_COMPONENT_TYPE_UNSIGNED_SHORT:
                                        *(uint16_t*)valueDst = (uint16_t)(value);
                                        break;
                                    case MODEL_COMPONENT_TYPE_UNSIGNED_INT:
                                        *(uint32_t*)valueDst = (uint32_t)(value);
                                        break;
                                    case MODEL_COMPONENT_TYPE_FLOAT:
                                        *(float*)valueDst = value;
                                        break;
                                }
                            }
                        }
                    } else {
                        // integer to integer, no "proportional" conversion, just change of storage
                        char* dst = (char*)&out[0];
                        for (int i = 0; i < valueCount; i++) {
                            for (int j = 0; j < (int)srcComponentCount; j++) {
                                int64_t value;
                                const char* valueSrc = src + i * readStride + j * srcComponentSize;
                                switch (accessor->componentType) {
                                    case MODEL_COMPONENT_TYPE_BYTE:
                                        value = (int64_t)(*(int8_t*)valueSrc);
                                        break;
                                    case MODEL_COMPONENT_TYPE_UNSIGNED_BYTE:
                                    default:
                                        value = (int64_t)(*(uint8_t*)valueSrc);
                                        break;
                                    case MODEL_COMPONENT_TYPE_SHORT:
                                        value = (int64_t)(*(int16_t*)valueSrc);
                                        break;
                                    case MODEL_COMPONENT_TYPE_UNSIGNED_SHORT:
                                        value = (int64_t)(*(uint16_t*)valueSrc);
                                        break;
                                    case MODEL_COMPONENT_TYPE_UNSIGNED_INT:
                                        value = (int64_t)(*(uint32_t*)valueSrc);
                                        break;
                                }
                                char* valueDst = dst + i * dstValueSize + j * dstComponentSize;
                                switch (componentType) {
                                    case MODEL_COMPONENT_TYPE_BYTE:
                                        *(int8_t*)valueDst = (int8_t)value;
                                        break;
                                    case MODEL_COMPONENT_TYPE_UNSIGNED_BYTE:
                                    default:
                                        *(uint8_t*)valueDst = (uint8_t)value;
                                        break;
                                    case MODEL_COMPONENT_TYPE_SHORT:
                                        *(int16_t*)valueDst = (int16_t)value;
                                        break;
                                    case MODEL_COMPONENT_TYPE_UNSIGNED_SHORT:
                                        *(uint8_t*)valueDst = (uint16_t)value;
                                        break;
                                    case MODEL_COMPONENT_TYPE_UNSIGNED_INT:
                                        *(uint32_t*)valueDst = (uint32_t)value;
                                        break;
                                }
                            }
                        }
                    }
                }
            } else {
                if (readStride == (int)srcValueSize) {
                    memcpy(&out[startIndex], buffer->bufferData.data() + offset, srcRequiredSize);
                } else {
                    char* dst = (char*)&out[0];
                    for (int i = 0; i < valueCount; i++) {
                        const char* valueSrc = src + i * readStride;
                        char* valueDst = dst + i * srcValueSize;
                        memcpy(valueDst, valueSrc, srcValueSize);
                    }
                }
            }
        }
    }

    return loaded;
}

bool ReadVertexAttributes(
    const OVR::JsonReader& attributes,
    ModelFile& modelFile,
    VertexAttribs& attribs,
    bool isMorphTarget) {
    bool loaded = true;

    { // POSITION and BOUNDS
        const int positionIndex = attributes.GetChildInt32ByName("POSITION", -1);
        // must have positions unless this is a morph target
        if (!isMorphTarget) {
            if (positionIndex < 0 ||
                positionIndex >= static_cast<int>(modelFile.Accessors.size())) {
                ALOGW(
                    "Error: Invalid position accessor index %i, accessor count = %zu",
                    positionIndex,
                    modelFile.Accessors.size());
                loaded = false;
                return loaded;
            }
        }

        loaded = ReadSurfaceDataFromAccessor(
            attribs.position, modelFile, positionIndex, ACCESSOR_VEC3, GL_FLOAT, -1, false);
    }

    // attribute count must match positions unless this is a morph target
    const int numVertices = isMorphTarget ? -1 : static_cast<int>(attribs.position.size());

    if (loaded) {
        loaded = ReadSurfaceDataFromAccessor(
            attribs.normal,
            modelFile,
            attributes.GetChildInt32ByName("NORMAL", -1),
            ACCESSOR_VEC3,
            GL_FLOAT,
            numVertices,
            false);
    }
    // #TODO:  we have tangent as a vec3, the spec has it as a vec4.
    // so we will have to one off the loading of it.
    // if (loaded) {
    //     // loaded = ReadSurfaceDataFromAccessor(
    //     //     attribs.tangent,
    //     //     modelFile,
    //     //     attributes.GetChildInt32ByName("TANGENT", -1),
    //     //     ACCESSOR_VEC4,
    //     //     GL_FLOAT,
    //     //     numVertices,
    //     //     false);
    // }
    if (loaded) {
        loaded = ReadSurfaceDataFromAccessor(
            attribs.binormal,
            modelFile,
            attributes.GetChildInt32ByName("BINORMAL", -1),
            ACCESSOR_VEC3,
            GL_FLOAT,
            numVertices,
            false);
    }
    if (loaded) {
        loaded = ReadSurfaceDataFromAccessor(
            attribs.color,
            modelFile,
            attributes.GetChildInt32ByName("COLOR", -1),
            ACCESSOR_VEC4,
            GL_FLOAT,
            numVertices,
            false);
    }
    if (loaded) {
        loaded = ReadSurfaceDataFromAccessor(
            attribs.uv0,
            modelFile,
            attributes.GetChildInt32ByName("TEXCOORD_0", -1),
            ACCESSOR_VEC2,
            GL_FLOAT,
            numVertices,
            false);
    }
    if (loaded) {
        loaded = ReadSurfaceDataFromAccessor(
            attribs.uv1,
            modelFile,
            attributes.GetChildInt32ByName("TEXCOORD_1", -1),
            ACCESSOR_VEC2,
            GL_FLOAT,
            numVertices,
            false);
    }
    // #TODO:  TEXCOORD_2 is in the gltf spec, but we only support 2
    // uv sets. support more uv coordinates, skipping for now.
    // if ( loaded ) { loaded = ReadSurfaceDataFromAccessor(
    // attribs.uv2, modelFile,
    // attributes.GetChildInt32ByName( "TEXCOORD_2", -1 ),
    // ACCESSOR_VEC2, GL_FLOAT, static_cast< int
    // >( newPrimitive.attribs.position.size() ) ); }
    // #TODO: get weights of type unsigned_byte and unsigned_short
    // working.
    if (loaded) {
        loaded = ReadSurfaceDataFromAccessor(
            attribs.jointWeights,
            modelFile,
            attributes.GetChildInt32ByName("WEIGHTS_0", -1),
            ACCESSOR_VEC4,
            GL_FLOAT,
            numVertices,
            false);
    }
    if (loaded) {
        int jointIndex = attributes.GetChildInt32ByName("JOINTS_0", -1);
        if (jointIndex >= 0 && jointIndex < static_cast<int>(modelFile.Accessors.size())) {
            ModelAccessor& acc = modelFile.Accessors[jointIndex];
            loaded = ReadSurfaceDataFromAccessor(
                attribs.jointIndices,
                modelFile,
                attributes.GetChildInt32ByName("JOINTS_0", -1),
                ACCESSOR_VEC4,
                acc.componentType,
                numVertices,
                false);
            /// List unique joints
            std::unordered_map<int, size_t> uniqueJoints;
            for (const auto& index : attribs.jointIndices) {
                for (int i = 0; i < 4; ++i) {
                    int jointID = index[i];
                    auto it = uniqueJoints.find(jointID);
                    if (it == uniqueJoints.end()) {
                        uniqueJoints[jointID] = 1u;
                    } else {
                        uniqueJoints[jointID] = uniqueJoints[jointID] + 1;
                    }
                }
            }
            /// print them
            ALOGW("Enumerating skinning joints:");
            for (const auto& u : uniqueJoints) {
                ALOGW(" - joint: %02d count: %llu", u.first, u.second);
            }
        }
    }
    return loaded;
}

// Requires the buffers and images to already be loaded in the model
bool LoadModelFile_glTF_Json(
    ModelFile& modelFile,
    const char* modelsJson,
    const ModelGlPrograms& programs,
    const MaterialParms& materialParms,
    ModelGeo* outModelGeo) {
    ALOG("LoadModelFile_glTF_Json parsing %s", modelFile.FileName.c_str());
    // LOGCPUTIME( "LoadModelFile_glTF_Json" );

    bool loaded = true;

    const char* error = nullptr;
    auto json = OVR::JSON::Parse(modelsJson, &error);
    if (json == nullptr) {
        ALOG("LoadModelFile_glTF_Json: Error loading %s : %s", modelFile.FileName.c_str(), error);
        loaded = false;
    } else {
        const OVR::JsonReader models(json);
        if (models.IsObject()) {
            if (loaded) { // ASSET
                const OVR::JsonReader asset(models.GetChildByName("asset"));
                if (!asset.IsObject()) {
                    ALOGW("Error: No asset on gltfSceneFile");
                    loaded = false;
                }
                std::string versionString = asset.GetChildStringByName("version");
                std::string minVersion = asset.GetChildStringByName("minVersion");
                if (OVR::OVR_stricmp(versionString.c_str(), "2.0") != 0 &&
                    OVR::OVR_stricmp(minVersion.c_str(), "2.0") != 0) {
                    ALOGW(
                        "Error: Invalid version number '%s' on gltfFile, currently only version 2.0 supported",
                        versionString.c_str());
                    loaded = false;
                }
            } // END ASSET

            if (loaded) { // ACCESSORS
                LOGV("Loading accessors");
                const OVR::JsonReader accessors(models.GetChildByName("accessors"));
                if (accessors.IsArray()) {
                    while (!accessors.IsEndOfArray() && loaded) {
                        const OVR::JsonReader accessor(accessors.GetNextArrayElement());
                        if (accessor.IsObject()) {
                            ModelAccessor newGltfAccessor;

                            newGltfAccessor.name = accessor.GetChildStringByName("name");
                            const int bufferView = accessor.GetChildInt32ByName("bufferView");
                            newGltfAccessor.byteOffset = accessor.GetChildInt32ByName("byteOffset");
                            newGltfAccessor.componentType =
                                accessor.GetChildInt32ByName("componentType");
                            newGltfAccessor.count = accessor.GetChildInt32ByName("count");
                            const std::string type = accessor.GetChildStringByName("type");
                            newGltfAccessor.normalized = accessor.GetChildBoolByName("normalized");

                            if (bufferView < 0 ||
                                bufferView >= (const int)modelFile.BufferViews.size()) {
                                ALOGW("Error: Invalid bufferView Index in gltfAccessor");
                                loaded = false;
                            }

                            int componentCount = 0;
                            if (OVR::OVR_stricmp(type.c_str(), "SCALAR") == 0) {
                                newGltfAccessor.type = ACCESSOR_SCALAR;
                                componentCount = 1;
                            } else if (OVR::OVR_stricmp(type.c_str(), "VEC2") == 0) {
                                newGltfAccessor.type = ACCESSOR_VEC2;
                                componentCount = 2;
                            } else if (OVR::OVR_stricmp(type.c_str(), "VEC3") == 0) {
                                newGltfAccessor.type = ACCESSOR_VEC3;
                                componentCount = 3;
                            } else if (OVR::OVR_stricmp(type.c_str(), "VEC4") == 0) {
                                newGltfAccessor.type = ACCESSOR_VEC4;
                                componentCount = 4;
                            } else if (OVR::OVR_stricmp(type.c_str(), "MAT2") == 0) {
                                newGltfAccessor.type = ACCESSOR_MAT2;
                                componentCount = 4;
                            } else if (OVR::OVR_stricmp(type.c_str(), "MAT3") == 0) {
                                newGltfAccessor.type = ACCESSOR_MAT3;
                                componentCount = 9;
                            } else if (OVR::OVR_stricmp(type.c_str(), "MAT4") == 0) {
                                newGltfAccessor.type = ACCESSOR_MAT4;
                                componentCount = 16;
                            } else {
                                ALOGW("Error: Invalid type in gltfAccessor");
                                loaded = false;
                            }

                            auto min = accessor.GetChildByName("min");
                            auto max = accessor.GetChildByName("max");
                            if (min != nullptr && max != nullptr) {
                                switch (newGltfAccessor.componentType) {
                                    case GL_BYTE:
                                    case GL_UNSIGNED_BYTE:
                                    case GL_SHORT:
                                    case GL_UNSIGNED_SHORT:
                                    case GL_UNSIGNED_INT:
                                        ParseIntArray(newGltfAccessor.intMin, componentCount, min);
                                        ParseIntArray(newGltfAccessor.intMax, componentCount, max);
                                        break;
                                    case GL_FLOAT:
                                        ParseFloatArray(
                                            newGltfAccessor.floatMin, componentCount, min);
                                        ParseFloatArray(
                                            newGltfAccessor.floatMax, componentCount, max);
                                        break;
                                    default:
                                        ALOGW("Error: Invalid componentType in gltfAccessor");
                                        loaded = false;
                                }
                                newGltfAccessor.minMaxSet = true;
                            }

                            newGltfAccessor.bufferView = &modelFile.BufferViews[bufferView];
                            modelFile.Accessors.push_back(newGltfAccessor);
                        }
                    }
                }
            } // END ACCESSORS

            if (loaded) { // SAMPLERS
                LOGV("Loading samplers");
                const OVR::JsonReader samplers(models.GetChildByName("samplers"));
                if (samplers.IsArray()) {
                    while (!samplers.IsEndOfArray() && loaded) {
                        const OVR::JsonReader sampler(samplers.GetNextArrayElement());
                        if (sampler.IsObject()) {
                            ModelSampler newGltfSampler;

                            newGltfSampler.name = sampler.GetChildStringByName("name");
                            newGltfSampler.magFilter =
                                sampler.GetChildInt32ByName("magFilter", ModelSampler::kGL_LINEAR);
                            newGltfSampler.minFilter = sampler.GetChildInt32ByName(
                                "minFilter", ModelSampler::kGL_NEAREST_MIPMAP_LINEAR);
                            newGltfSampler.wrapS =
                                sampler.GetChildInt32ByName("wrapS", ModelSampler::kGL_REPEAT);
                            newGltfSampler.wrapT =
                                sampler.GetChildInt32ByName("wrapT", ModelSampler::kGL_REPEAT);

                            if (newGltfSampler.magFilter != ModelSampler::kGL_NEAREST &&
                                newGltfSampler.magFilter != ModelSampler::kGL_LINEAR) {
                                ALOGW("Error: Invalid magFilter in gltfSampler");
                                loaded = false;
                            }
                            if (newGltfSampler.minFilter != ModelSampler::kGL_NEAREST &&
                                newGltfSampler.minFilter != ModelSampler::kGL_LINEAR &&
                                newGltfSampler.minFilter !=
                                    ModelSampler::kGL_LINEAR_MIPMAP_NEAREST &&
                                newGltfSampler.minFilter !=
                                    ModelSampler::kGL_NEAREST_MIPMAP_LINEAR &&
                                newGltfSampler.minFilter !=
                                    ModelSampler::kGL_LINEAR_MIPMAP_LINEAR) {
                                ALOGW("Error: Invalid minFilter in gltfSampler");
                                loaded = false;
                            }
                            if (newGltfSampler.wrapS != ModelSampler::kGL_CLAMP_TO_EDGE &&
                                newGltfSampler.wrapS != ModelSampler::kGL_MIRRORED_REPEAT &&
                                newGltfSampler.wrapS != ModelSampler::kGL_REPEAT) {
                                ALOGW("Error: Invalid wrapS in gltfSampler");
                                loaded = false;
                            }
                            if (newGltfSampler.wrapT != ModelSampler::kGL_CLAMP_TO_EDGE &&
                                newGltfSampler.wrapT != ModelSampler::kGL_MIRRORED_REPEAT &&
                                newGltfSampler.wrapT != ModelSampler::kGL_REPEAT) {
                                ALOGW("Error: Invalid wrapT in gltfSampler");
                                loaded = false;
                            }

                            modelFile.Samplers.push_back(newGltfSampler);
                        }
                    }
                }

                // default sampler
                ModelSampler defaultGltfSampler;
                defaultGltfSampler.name = "Default_Sampler";
                modelFile.Samplers.push_back(defaultGltfSampler);
            } // END SAMPLERS

            if (loaded) { // TEXTURES
                LOGV("Loading textures");
                const OVR::JsonReader textures(models.GetChildByName("textures"));
                if (textures.IsArray() && loaded) {
                    while (!textures.IsEndOfArray()) {
                        const OVR::JsonReader texture(textures.GetNextArrayElement());
                        if (texture.IsObject()) {
                            ModelTextureWrapper newGltfTexture;

                            newGltfTexture.name = texture.GetChildStringByName("name");
                            const int sampler = texture.GetChildInt32ByName("sampler", -1);
                            int image = texture.GetChildInt32ByName("source", -1);
                            const OVR::JsonReader textureExtensions =
                                texture.GetChildByName("extensions");
                            if (textureExtensions.IsObject()) {
                                const OVR::JsonReader basisuExtension =
                                    textureExtensions.GetChildByName("KHR_texture_basisu");
                                if (basisuExtension.IsObject()) {
                                    image = basisuExtension.GetChildInt32ByName("source", image);
                                }
                            }

                            if (sampler < -1 ||
                                sampler >= static_cast<int>(modelFile.Samplers.size())) {
                                ALOGW("Error: Invalid sampler Index in gltfTexture");
                                loaded = false;
                            }

                            if (image < -1 ||
                                image >= static_cast<int>(modelFile.Textures.size())) {
                                ALOGW("Error: Invalid source Index in gltfTexture");
                                loaded = false;
                            }

                            if (sampler < 0) {
                                newGltfTexture.sampler =
                                    &modelFile
                                         .Samplers[static_cast<int>(modelFile.Samplers.size()) - 1];
                            } else {
                                newGltfTexture.sampler = &modelFile.Samplers[sampler];
                            }
                            if (image < 0) {
                                newGltfTexture.image = nullptr;
                            } else {
                                newGltfTexture.image = &modelFile.Textures[image];
                            }
                            modelFile.TextureWrappers.push_back(newGltfTexture);
                        }
                    }
                }
            } // END TEXTURES

            if (loaded) { // MATERIALS
                LOGV("Loading materials");
                const OVR::JsonReader materials(models.GetChildByName("materials"));
                if (materials.IsArray() && loaded) {
                    while (!materials.IsEndOfArray()) {
                        const OVR::JsonReader material(materials.GetNextArrayElement());
                        if (material.IsObject()) {
                            ModelMaterial newGltfMaterial;

                            // material
                            newGltfMaterial.name = material.GetChildStringByName("name");

                            auto emissiveFactor = material.GetChildByName("emissiveFactor");
                            if (emissiveFactor != nullptr) {
                                if (emissiveFactor->GetItemCount() != 3) {
                                    ALOGW(
                                        "Error: Invalid Itemcount on emissiveFactor for gltfMaterial");
                                    loaded = false;
                                }
                                newGltfMaterial.emmisiveFactor.x =
                                    emissiveFactor->GetItemByIndex(0)->GetFloatValue();
                                newGltfMaterial.emmisiveFactor.y =
                                    emissiveFactor->GetItemByIndex(1)->GetFloatValue();
                                newGltfMaterial.emmisiveFactor.z =
                                    emissiveFactor->GetItemByIndex(2)->GetFloatValue();
                            }

                            const std::string alphaModeString =
                                material.GetChildStringByName("alphaMode", "OPAQUE");
                            if (OVR::OVR_stricmp(alphaModeString.c_str(), "OPAQUE") == 0) {
                                newGltfMaterial.alphaMode = ALPHA_MODE_OPAQUE;
                            } else if (OVR::OVR_stricmp(alphaModeString.c_str(), "MASK") == 0) {
                                newGltfMaterial.alphaMode = ALPHA_MODE_MASK;
                            } else if (OVR::OVR_stricmp(alphaModeString.c_str(), "BLEND") == 0) {
                                newGltfMaterial.alphaMode = ALPHA_MODE_BLEND;
                            } else {
                                ALOGW("Error: Invalid alphaMode in gltfMaterial");
                                loaded = false;
                            }

                            newGltfMaterial.alphaCutoff =
                                material.GetChildFloatByName("alphaCutoff", 0.5f);
                            newGltfMaterial.doubleSided =
                                material.GetChildBoolByName("doubleSided", false);

                            // pbrMetallicRoughness
                            const OVR::JsonReader pbrMetallicRoughness =
                                material.GetChildByName("pbrMetallicRoughness");
                            if (pbrMetallicRoughness.IsObject()) {
                                auto baseColorFactor =
                                    pbrMetallicRoughness.GetChildByName("baseColorFactor");
                                if (baseColorFactor != nullptr) {
                                    if (baseColorFactor->GetItemCount() != 4) {
                                        ALOGW(
                                            "Error: Invalid Itemcount on baseColorFactor for gltfMaterial");
                                        loaded = false;
                                    }
                                    newGltfMaterial.baseColorFactor.x =
                                        baseColorFactor->GetItemByIndex(0)->GetFloatValue();
                                    newGltfMaterial.baseColorFactor.y =
                                        baseColorFactor->GetItemByIndex(1)->GetFloatValue();
                                    newGltfMaterial.baseColorFactor.z =
                                        baseColorFactor->GetItemByIndex(2)->GetFloatValue();
                                    newGltfMaterial.baseColorFactor.w =
                                        baseColorFactor->GetItemByIndex(3)->GetFloatValue();
                                }

                                const OVR::JsonReader baseColorTexture =
                                    pbrMetallicRoughness.GetChildByName("baseColorTexture");
                                if (baseColorTexture.IsObject()) {
                                    int index = baseColorTexture.GetChildInt32ByName("index", -1);
                                    if (index < 0 ||
                                        index >=
                                            static_cast<int>(modelFile.TextureWrappers.size())) {
                                        ALOGW(
                                            "Error: Invalid baseColorTexture index in gltfMaterial");
                                        loaded = false;
                                    }
                                    newGltfMaterial.baseColorTextureWrapper =
                                        &modelFile.TextureWrappers[index];
                                }

                                newGltfMaterial.metallicFactor =
                                    pbrMetallicRoughness.GetChildFloatByName(
                                        "metallicFactor", 1.0f);
                                newGltfMaterial.roughnessFactor =
                                    pbrMetallicRoughness.GetChildFloatByName(
                                        "roughnessFactor", 1.0f);

                                const OVR::JsonReader metallicRoughnessTexture =
                                    pbrMetallicRoughness.GetChildByName("metallicRoughnessTexture");
                                if (metallicRoughnessTexture.IsObject()) {
                                    int index =
                                        metallicRoughnessTexture.GetChildInt32ByName("index", -1);
                                    if (index < 0 ||
                                        index >=
                                            static_cast<int>(modelFile.TextureWrappers.size())) {
                                        ALOGW(
                                            "Error: Invalid metallicRoughnessTexture index in gltfMaterial");
                                        loaded = false;
                                    }
                                    newGltfMaterial.metallicRoughnessTextureWrapper =
                                        &modelFile.TextureWrappers[index];
                                }
                            }

                            // normalTexture
                            const OVR::JsonReader normalTexture =
                                material.GetChildByName("normalTexture");
                            if (normalTexture.IsObject()) {
                                int index = normalTexture.GetChildInt32ByName("index", -1);
                                if (index < 0 ||
                                    index >= static_cast<int>(modelFile.TextureWrappers.size())) {
                                    ALOGW("Error: Invalid normalTexture index in gltfMaterial");
                                    loaded = false;
                                }
                                newGltfMaterial.normalTextureWrapper =
                                    &modelFile.TextureWrappers[index];
                                newGltfMaterial.normalTexCoord =
                                    normalTexture.GetChildInt32ByName("texCoord", 0);
                                newGltfMaterial.normalScale =
                                    normalTexture.GetChildFloatByName("scale", 1.0f);
                            }

                            // occlusionTexture
                            const OVR::JsonReader occlusionTexture =
                                material.GetChildByName("occlusionTexture");
                            if (occlusionTexture.IsObject()) {
                                int index = occlusionTexture.GetChildInt32ByName("index", -1);
                                if (index < 0 ||
                                    index >= static_cast<int>(modelFile.TextureWrappers.size())) {
                                    ALOGW("Error: Invalid occlusionTexture index in gltfMaterial");
                                    loaded = false;
                                }
                                newGltfMaterial.occlusionTextureWrapper =
                                    &modelFile.TextureWrappers[index];
                                newGltfMaterial.occlusionTexCoord =
                                    occlusionTexture.GetChildInt32ByName("texCoord", 0);
                                newGltfMaterial.occlusionStrength =
                                    occlusionTexture.GetChildFloatByName("strength", 1.0f);
                            }

                            // emissiveTexture
                            const OVR::JsonReader emissiveTexture =
                                material.GetChildByName("emissiveTexture");
                            if (emissiveTexture.IsObject()) {
                                int index = emissiveTexture.GetChildInt32ByName("index", -1);
                                if (index < 0 ||
                                    index >= static_cast<int>(modelFile.TextureWrappers.size())) {
                                    ALOGW("Error: Invalid emissiveTexture index in gltfMaterial");
                                    loaded = false;
                                }
                                newGltfMaterial.emissiveTextureWrapper =
                                    &modelFile.TextureWrappers[index];
                            }

                            // detailTexture
                            const OVR::JsonReader detailTexture =
                                material.GetChildByName("detailTexture");
                            if (detailTexture.IsObject()) {
                                int index = detailTexture.GetChildInt32ByName("index", -1);
                                if (index < 0 ||
                                    index >= static_cast<int>(modelFile.TextureWrappers.size())) {
                                    ALOGW(
                                        "Error: Invalid texture index in gltfMaterial '%s'",
                                        newGltfMaterial.name.c_str());
                                    loaded = false;
                                }
                                newGltfMaterial.detailTextureWrapper =
                                    &modelFile.TextureWrappers[index];
                            }

                            modelFile.Materials.push_back(newGltfMaterial);
                        }
                    }
                    // Add a default material at the end of the list for primitives with an
                    // unspecified material.
                    ModelMaterial defaultmaterial;
                    modelFile.Materials.push_back(defaultmaterial);
                }
            } // END MATERIALS

            if (loaded) { // MODELS (gltf mesh)
                LOGV("Loading meshes");
                const OVR::JsonReader meshes(models.GetChildByName("meshes"));
                if (meshes.IsArray()) {
                    while (!meshes.IsEndOfArray() && loaded) {
                        const OVR::JsonReader mesh(meshes.GetNextArrayElement());
                        if (mesh.IsObject()) {
                            Model newGltfModel;

                            newGltfModel.name = mesh.GetChildStringByName("name");

                            { // SURFACES (gltf primitive)
                                const OVR::JsonReader primitives(mesh.GetChildByName("primitives"));
                                if (!primitives.IsArray()) {
                                    ALOGW("Error: no primitives on gltfMesh");
                                    loaded = false;
                                }

                                while (!primitives.IsEndOfArray() && loaded) {
                                    const OVR::JsonReader primitive(
                                        primitives.GetNextArrayElement());

                                    ModelSurface newGltfSurface;

                                    const int materialIndex =
                                        primitive.GetChildInt32ByName("material", -1);
                                    if (materialIndex < 0) {
                                        LOGV(
                                            "Using default for material on %s",
                                            newGltfModel.name.c_str());
                                        newGltfSurface.material =
                                            &modelFile.Materials
                                                 [static_cast<int>(modelFile.Materials.size()) - 1];
                                    } else if (
                                        materialIndex >=
                                        static_cast<int>(modelFile.Materials.size())) {
                                        ALOGW("Error: Invalid materialIndex on gltfPrimitive");
                                        loaded = false;
                                    } else {
                                        newGltfSurface.material =
                                            &modelFile.Materials[materialIndex];
                                    }

                                    const int mode = primitive.GetChildInt32ByName("mode", 4);
                                    if (mode < static_cast<const int>(
                                                   GlGeometry::kPrimitiveTypePoints) ||
                                        mode > static_cast<const int>(
                                                   GlGeometry::kPrimitiveTypeTriangleFan)) {
                                        ALOGW("Error: Invalid mode on gltfPrimitive");
                                        loaded = false;
                                    }
                                    if (mode != GlGeometry::kPrimitiveTypeTriangles) {
                                        // #TODO: support modes other than triangle?
                                        ALOGW(
                                            "Error: Mode other then TRIANGLE (4) not currently supported on gltfPrimitive");
                                        loaded = false;
                                    }

                                    const OVR::JsonReader attributes(
                                        primitive.GetChildByName("attributes"));
                                    if (!attributes.IsObject()) {
                                        ALOGW("Error: no attributes on gltfPrimitive");
                                        loaded = false;
                                    }

                                    TriangleIndex outGeoIndexOffset = 0;
                                    if (outModelGeo != nullptr) {
                                        outGeoIndexOffset = static_cast<TriangleIndex>(
                                            (*outModelGeo).positions.size());
                                    }

                                    // VERTICES
                                    VertexAttribs attribs;
                                    loaded = ReadVertexAttributes(
                                        attributes, modelFile, attribs, false /*isMorphTarget*/);

                                    // MORPH TARGETS
                                    const OVR::JsonReader targets(
                                        primitive.GetChildByName("targets"));
                                    if (targets.IsValid()) {
                                        if (!targets.IsArray()) {
                                            ALOGW("Error: Invalid targets on primitive");
                                            loaded = false;
                                        }

                                        while (!targets.IsEndOfArray() && loaded) {
                                            const OVR::JsonReader target(
                                                targets.GetNextArrayElement());
                                            VertexAttribs targetAttribs;
                                            loaded = ReadVertexAttributes(
                                                target,
                                                modelFile,
                                                targetAttribs,
                                                true /*isMorphTarget*/);
                                            if (loaded) {
                                                // for each morph target attribute, an original
                                                // attribute MUST be present in the mesh primitive
#define CHECK_ATTRIB_COUNT(ATTRIB)                                                               \
    if (!targetAttribs.ATTRIB.empty() && targetAttribs.ATTRIB.size() != attribs.ATTRIB.size()) { \
        ALOGW("Error: target " #ATTRIB " count mismatch on gltfPrimitive");                      \
        loaded = false;                                                                          \
    }
                                                CHECK_ATTRIB_COUNT(position);
                                                CHECK_ATTRIB_COUNT(normal);
                                                CHECK_ATTRIB_COUNT(tangent);
                                                CHECK_ATTRIB_COUNT(color);
                                                CHECK_ATTRIB_COUNT(uv0);
                                                CHECK_ATTRIB_COUNT(uv1);
#undef CHECK_ATTRIB_COUNT
                                                newGltfSurface.targets.emplace_back(
                                                    std::move(targetAttribs));
                                            }
                                        }
                                    }

                                    // TRIANGLES
                                    std::vector<TriangleIndex> indices;
                                    const int indicesIndex =
                                        primitive.GetChildInt32ByName("indices", -1);
                                    if (indicesIndex < 0 ||
                                        indicesIndex >=
                                            static_cast<int>(modelFile.Accessors.size())) {
                                        ALOGW("Error: Invalid indices index on gltfPrimitive");
                                        loaded = false;
                                    }

                                    // Reduced severity to warning: this doesn't break most data
                                    // types, but can cause unexpected results.
                                    if (modelFile.Accessors[indicesIndex].componentType !=
                                        GL_UNSIGNED_SHORT) {
                                        ALOGW(
                                            "Warning: Currently, only componentType of %d supported for indices, %d requested",
                                            GL_UNSIGNED_SHORT,
                                            modelFile.Accessors[indicesIndex].componentType);
                                    }

                                    if (loaded) {
                                        ReadSurfaceDataFromAccessor(
                                            indices,
                                            modelFile,
                                            primitive.GetChildInt32ByName("indices", -1),
                                            ACCESSOR_SCALAR,
                                            GL_UNSIGNED_SHORT,
                                            -1,
                                            false);
                                    }

                                    newGltfSurface.surfaceDef.geo.Create(attribs, indices);
                                    bool skinned =
                                        (attribs.jointIndices.size() == attribs.position.size() &&
                                         attribs.jointWeights.size() == attribs.position.size());

                                    if (outModelGeo != nullptr) {
                                        for (int i = 0; i < static_cast<int>(indices.size()); ++i) {
                                            (*outModelGeo)
                                                .indices.push_back(indices[i] + outGeoIndexOffset);
                                        }
                                    }

                                    // CREATE COMMAND BUFFERS.
                                    if (newGltfSurface.material->alphaMode == ALPHA_MODE_MASK) {
                                        // #TODO: implement ALPHA_MODE_MASK if we need it.
                                        // Just blend because alpha testing is rather expensive.
                                        ALOGW(
                                            "gltfAlphaMode ALPHA_MODE_MASK requested, doing ALPHA_MODE_BLEND instead");
                                        newGltfSurface.surfaceDef.graphicsCommand.GpuState
                                            .blendEnable = ovrGpuState::BLEND_ENABLE;
                                        newGltfSurface.surfaceDef.graphicsCommand.GpuState
                                            .depthMaskEnable = false;
                                        newGltfSurface.surfaceDef.graphicsCommand.GpuState
                                            .blendSrc = ovrGpuState::kGL_SRC_ALPHA;
                                        newGltfSurface.surfaceDef.graphicsCommand.GpuState
                                            .blendDst = ovrGpuState::kGL_ONE_MINUS_SRC_ALPHA;
                                    } else if (
                                        newGltfSurface.material->alphaMode == ALPHA_MODE_BLEND ||
                                        materialParms.Transparent) {
                                        if (materialParms.Transparent &&
                                            newGltfSurface.material->alphaMode !=
                                                ALPHA_MODE_BLEND) {
                                            ALOGW(
                                                "gltfAlphaMode is %d but treating at ALPHA_MODE_BLEND due to materialParms.Transparent",
                                                newGltfSurface.material->alphaMode);
                                        }
                                        newGltfSurface.surfaceDef.graphicsCommand.GpuState
                                            .blendEnable = ovrGpuState::BLEND_ENABLE;
                                        newGltfSurface.surfaceDef.graphicsCommand.GpuState
                                            .depthMaskEnable = false;
                                        newGltfSurface.surfaceDef.graphicsCommand.GpuState
                                            .blendSrc = ovrGpuState::kGL_SRC_ALPHA;
                                        newGltfSurface.surfaceDef.graphicsCommand.GpuState
                                            .blendDst = ovrGpuState::kGL_ONE_MINUS_SRC_ALPHA;
                                    }
                                    // #TODO: GLTF doesn't have a concept of an ADDITIVE mode. maybe
                                    // it should?
                                    // else if ( newGltfSurface.material->alphaMode ==
                                    // MATERIAL_TYPE_ADDITIVE )
                                    //{
                                    //	newGltfSurface.surfaceDef.graphicsCommand.GpuState.blendEnable
                                    //= ovrGpuState::BLEND_ENABLE;
                                    //	newGltfSurface.surfaceDef.graphicsCommand.GpuState.depthMaskEnable
                                    //= false;
                                    //	newGltfSurface.surfaceDef.graphicsCommand.GpuState.blendSrc
                                    //= GL_ONE;
                                    //	newGltfSurface.surfaceDef.graphicsCommand.GpuState.blendDst
                                    //= GL_ONE;
                                    //}
                                    else if (
                                        newGltfSurface.material->alphaMode == ALPHA_MODE_OPAQUE) {
                                        // default GpuState;
                                    }

                                    if (newGltfSurface.material->baseColorTextureWrapper !=
                                        nullptr) {
                                        newGltfSurface.surfaceDef.graphicsCommand.Textures[0] =
                                            newGltfSurface.material->baseColorTextureWrapper->image
                                                ->texid;
                                        if (newGltfSurface.material->emissiveTextureWrapper !=
                                            nullptr) {
                                            if (programs.ProgBaseColorEmissivePBR == nullptr) {
                                                ALOGE_FAIL("No ProgBaseColorEmissivePBR set");
                                            }
                                            newGltfSurface.surfaceDef.graphicsCommand.Textures[1] =
                                                newGltfSurface.material->emissiveTextureWrapper
                                                    ->image->texid;
                                            if (skinned) {
                                                if (programs.ProgSkinnedBaseColorEmissivePBR ==
                                                    nullptr) {
                                                    ALOGE_FAIL(
                                                        "No ProgSkinnedBaseColorEmissivePBR set");
                                                }

                                                newGltfSurface.surfaceDef.graphicsCommand.Program =
                                                    *programs.ProgSkinnedBaseColorEmissivePBR;
                                                newGltfSurface.surfaceDef.surfaceName =
                                                    "ProgSkinnedBaseColorEmissivePBR";
                                            } else {
                                                newGltfSurface.surfaceDef.graphicsCommand.Program =
                                                    *programs.ProgBaseColorEmissivePBR;
                                                newGltfSurface.surfaceDef.surfaceName =
                                                    "ProgBaseColorEmissivePBR";
                                            }
                                        } else {
                                            if (newGltfSurface.material->detailTextureWrapper !=
                                                nullptr) {
                                                newGltfSurface.surfaceDef.graphicsCommand
                                                    .Textures[1] =
                                                    newGltfSurface.material->detailTextureWrapper
                                                        ->image->texid;
                                            }

                                            if (skinned) {
                                                if (programs.ProgSkinnedBaseColorPBR == nullptr) {
                                                    ALOGE_FAIL("No ProgSkinnedBaseColorPBR set");
                                                }
                                                newGltfSurface.surfaceDef.graphicsCommand.Program =
                                                    *programs.ProgSkinnedBaseColorPBR;
                                                newGltfSurface.surfaceDef.surfaceName =
                                                    "ProgSkinnedBaseColorPBR";
                                            } else {
                                                if (programs.ProgBaseColorPBR == nullptr) {
                                                    ALOGE_FAIL("No ProgBaseColorPBR set");
                                                }
                                                newGltfSurface.surfaceDef.graphicsCommand.Program =
                                                    *programs.ProgBaseColorPBR;
                                                newGltfSurface.surfaceDef.surfaceName =
                                                    "ProgBaseColorPBR";
                                            }
                                        }
                                    } else {
                                        if (skinned) {
                                            if (programs.ProgSkinnedSimplePBR == nullptr) {
                                                ALOGE_FAIL("No ProgSkinnedSimplePBR set");
                                            }
                                            newGltfSurface.surfaceDef.graphicsCommand.Program =
                                                *programs.ProgSkinnedSimplePBR;
                                            newGltfSurface.surfaceDef.surfaceName =
                                                "ProgSkinnedSimplePBR";
                                        } else {
                                            if (programs.ProgSimplePBR == nullptr) {
                                                ALOGE_FAIL("No ProgSimplePBR set");
                                            }
                                            newGltfSurface.surfaceDef.graphicsCommand.Program =
                                                *programs.ProgSimplePBR;
                                            newGltfSurface.surfaceDef.surfaceName = "ProgSimplePBR";
                                        }
                                    }

                                    if (materialParms.PolygonOffset) {
                                        newGltfSurface.surfaceDef.graphicsCommand.GpuState
                                            .polygonOffsetEnable = true;
                                    }

                                    if (newGltfSurface.material->doubleSided) {
                                        newGltfSurface.surfaceDef.graphicsCommand.GpuState
                                            .cullEnable = false;
                                    }

                                    // Retain original vertex data if we use morph targets
                                    if (!newGltfSurface.targets.empty()) {
                                        newGltfSurface.attribs = std::move(attribs);
                                    }
                                    newGltfModel.surfaces.emplace_back(std::move(newGltfSurface));
                                }
                            } // END SURFACES

                            // all primitives MUST have the same number of morph targets in the same
                            // order
                            for (const auto& surface : newGltfModel.surfaces) {
                                if (newGltfModel.surfaces[0].targets.size() !=
                                    surface.targets.size()) {
                                    ALOGW(
                                        "Error: not all primitives have the same number of morph targets");
                                    loaded = false;
                                }
                            }

                            { // WEIGHTS (optional)
                                if (loaded) {
                                    const OVR::JsonReader weights(mesh.GetChildByName("weights"));
                                    if (weights.IsArray()) {
                                        while (!weights.IsEndOfArray()) {
                                            newGltfModel.weights.push_back(
                                                weights.GetNextArrayFloat(0.0f));
                                        }
                                        if (newGltfModel.weights.size() !=
                                            newGltfModel.surfaces[0].targets.size()) {
                                            ALOGW(
                                                "Error: mesh weights and morph target count mismatch");
                                            loaded = false;
                                        }
                                    } else if (!newGltfModel.surfaces.empty()) {
                                        // when weights is undefined, the default targets' weights
                                        // are zeros
                                        newGltfModel.weights.resize(
                                            newGltfModel.surfaces[0].targets.size(), 0.0f);
                                    }
                                }
                            } // END WEIGHTS

                            modelFile.Models.emplace_back(std::move(newGltfModel));
                        }
                    }
                }
            } // END MODELS

            if (loaded) { // CAMERAS
                          // #TODO: best way to expose cameras to apps?
                LOGV("Loading cameras");
                const OVR::JsonReader cameras(models.GetChildByName("cameras"));
                if (cameras.IsArray() && loaded) {
                    while (!cameras.IsEndOfArray()) {
                        const OVR::JsonReader camera(cameras.GetNextArrayElement());
                        if (camera.IsObject()) {
                            ModelCamera newGltfCamera;

                            newGltfCamera.name = camera.GetChildStringByName("name");

                            const std::string cameraTypeString =
                                camera.GetChildStringByName("type");
                            if (OVR::OVR_stricmp(cameraTypeString.c_str(), "perspective") == 0) {
                                newGltfCamera.type = MODEL_CAMERA_TYPE_PERSPECTIVE;
                            } else if (
                                OVR::OVR_stricmp(cameraTypeString.c_str(), "orthographic") == 0) {
                                newGltfCamera.type = MODEL_CAMERA_TYPE_ORTHOGRAPHIC;
                            } else {
                                ALOGW(
                                    "Error: Invalid camera type on gltfCamera %s",
                                    cameraTypeString.c_str());
                                loaded = false;
                            }

                            if (newGltfCamera.type == MODEL_CAMERA_TYPE_ORTHOGRAPHIC) {
                                const OVR::JsonReader orthographic(
                                    camera.GetChildByName("orthographic"));
                                if (!orthographic.IsObject()) {
                                    ALOGW(
                                        "Error: No orthographic object on orthographic gltfCamera");
                                    loaded = false;
                                }
                                newGltfCamera.orthographic.magX =
                                    orthographic.GetChildFloatByName("xmag");
                                newGltfCamera.orthographic.magY =
                                    orthographic.GetChildFloatByName("ymag");
                                newGltfCamera.orthographic.nearZ =
                                    orthographic.GetChildFloatByName("znear");
                                newGltfCamera.orthographic.farZ =
                                    orthographic.GetChildFloatByName("zfar");
                                if (newGltfCamera.orthographic.magX <= 0.0f ||
                                    newGltfCamera.orthographic.magY <= 0.0f ||
                                    newGltfCamera.orthographic.nearZ <= 0.0f ||
                                    newGltfCamera.orthographic.farZ <=
                                        newGltfCamera.orthographic.nearZ) {
                                    ALOGW("Error: Invalid data in orthographic gltfCamera");
                                    loaded = false;
                                }
                            } else // MODEL_CAMERA_TYPE_PERSPECTIVE
                            {
                                const OVR::JsonReader perspective(
                                    camera.GetChildByName("perspective"));
                                if (!perspective.IsObject()) {
                                    ALOGW("Error: No perspective object on perspective gltfCamera");
                                    loaded = false;
                                }
                                newGltfCamera.perspective.aspectRatio =
                                    perspective.GetChildFloatByName("aspectRatio");
                                const float yfov = perspective.GetChildFloatByName("yfov");
                                newGltfCamera.perspective.fovDegreesX =
                                    (180.0f / 3.14159265358979323846f) * 2.0f *
                                    atanf(
                                        tanf(yfov * 0.5f) * newGltfCamera.perspective.aspectRatio);
                                newGltfCamera.perspective.fovDegreesY =
                                    (180.0f / 3.14159265358979323846f) * yfov;
                                newGltfCamera.perspective.nearZ =
                                    perspective.GetChildFloatByName("znear");
                                newGltfCamera.perspective.farZ =
                                    perspective.GetChildFloatByName("zfar", 10000.0f);
                                if (newGltfCamera.perspective.fovDegreesX <= 0.0f ||
                                    newGltfCamera.perspective.fovDegreesY <= 0.0f ||
                                    newGltfCamera.perspective.nearZ <= 0.0f ||
                                    newGltfCamera.perspective.farZ <= 0.0f) {
                                    ALOGW("Error: Invalid data in perspective gltfCamera");
                                    loaded = false;
                                }
                            }
                            modelFile.Cameras.push_back(newGltfCamera);
                        }
                    }
                }
            } // END CAMERAS

            if (loaded) { // NODES
                LOGV("Loading nodes");
                auto pNodes = models.GetChildByName("nodes");
                const OVR::JsonReader nodes(pNodes);
                if (nodes.IsArray() && loaded) {
                    modelFile.Nodes.resize(pNodes->GetItemCount());

                    int nodeIndex = 0;
                    while (!nodes.IsEndOfArray()) {
                        const OVR::JsonReader node(nodes.GetNextArrayElement());
                        if (node.IsObject()) {
                            ModelNode* pGltfNode = &modelFile.Nodes[nodeIndex];

                            pGltfNode->name = node.GetChildStringByName("name");
                            const OVR::JsonReader matrixReader = node.GetChildByName("matrix");
                            if (matrixReader.IsArray()) {
                                Matrix4f matrix;
                                ParseFloatArray(matrix.M[0], 16, matrixReader);
                                matrix.Transpose();
                                // TRANSLATION
                                pGltfNode->translation = matrix.GetTranslation();
                                // SCALE
                                pGltfNode->scale.x = sqrtf(
                                    matrix.M[0][0] * matrix.M[0][0] +
                                    matrix.M[0][1] * matrix.M[0][1] +
                                    matrix.M[0][2] * matrix.M[0][2]);
                                pGltfNode->scale.y = sqrtf(
                                    matrix.M[1][0] * matrix.M[1][0] +
                                    matrix.M[1][1] * matrix.M[1][1] +
                                    matrix.M[1][2] * matrix.M[1][2]);
                                pGltfNode->scale.z = sqrtf(
                                    matrix.M[2][0] * matrix.M[2][0] +
                                    matrix.M[2][1] * matrix.M[2][1] +
                                    matrix.M[2][2] * matrix.M[2][2]);
                                // ROTATION
                                const float rcpScaleX = OVR::RcpSqrt(
                                    matrix.M[0][0] * matrix.M[0][0] +
                                    matrix.M[0][1] * matrix.M[0][1] +
                                    matrix.M[0][2] * matrix.M[0][2]);
                                const float rcpScaleY = OVR::RcpSqrt(
                                    matrix.M[1][0] * matrix.M[1][0] +
                                    matrix.M[1][1] * matrix.M[1][1] +
                                    matrix.M[1][2] * matrix.M[1][2]);
                                const float rcpScaleZ = OVR::RcpSqrt(
                                    matrix.M[2][0] * matrix.M[2][0] +
                                    matrix.M[2][1] * matrix.M[2][1] +
                                    matrix.M[2][2] * matrix.M[2][2]);
                                const float m[9] = {
                                    matrix.M[0][0] * rcpScaleX,
                                    matrix.M[0][1] * rcpScaleX,
                                    matrix.M[0][2] * rcpScaleX,
                                    matrix.M[1][0] * rcpScaleY,
                                    matrix.M[1][1] * rcpScaleY,
                                    matrix.M[1][2] * rcpScaleY,
                                    matrix.M[2][0] * rcpScaleZ,
                                    matrix.M[2][1] * rcpScaleZ,
                                    matrix.M[2][2] * rcpScaleZ};
                                if (m[0 * 3 + 0] + m[1 * 3 + 1] + m[2 * 3 + 2] > 0.0f) {
                                    float t = +m[0 * 3 + 0] + m[1 * 3 + 1] + m[2 * 3 + 2] + 1.0f;
                                    float s = OVR::RcpSqrt(t) * 0.5f;
                                    pGltfNode->rotation.w = s * t;
                                    pGltfNode->rotation.z = (m[0 * 3 + 1] - m[1 * 3 + 0]) * s;
                                    pGltfNode->rotation.y = (m[2 * 3 + 0] - m[0 * 3 + 2]) * s;
                                    pGltfNode->rotation.x = (m[1 * 3 + 2] - m[2 * 3 + 1]) * s;
                                } else if (
                                    m[0 * 3 + 0] > m[1 * 3 + 1] && m[0 * 3 + 0] > m[2 * 3 + 2]) {
                                    float t = +m[0 * 3 + 0] - m[1 * 3 + 1] - m[2 * 3 + 2] + 1.0f;
                                    float s = OVR::RcpSqrt(t) * 0.5f;
                                    pGltfNode->rotation.x = s * t;
                                    pGltfNode->rotation.y = (m[0 * 3 + 1] + m[1 * 3 + 0]) * s;
                                    pGltfNode->rotation.z = (m[2 * 3 + 0] + m[0 * 3 + 2]) * s;
                                    pGltfNode->rotation.w = (m[1 * 3 + 2] - m[2 * 3 + 1]) * s;
                                } else if (m[1 * 3 + 1] > m[2 * 3 + 2]) {
                                    float t = -m[0 * 3 + 0] + m[1 * 3 + 1] - m[2 * 3 + 2] + 1.0f;
                                    float s = OVR::RcpSqrt(t) * 0.5f;
                                    pGltfNode->rotation.y = s * t;
                                    pGltfNode->rotation.x = (m[0 * 3 + 1] + m[1 * 3 + 0]) * s;
                                    pGltfNode->rotation.w = (m[2 * 3 + 0] - m[0 * 3 + 2]) * s;
                                    pGltfNode->rotation.z = (m[1 * 3 + 2] + m[2 * 3 + 1]) * s;
                                } else {
                                    float t = -m[0 * 3 + 0] - m[1 * 3 + 1] + m[2 * 3 + 2] + 1.0f;
                                    float s = OVR::RcpSqrt(t) * 0.5f;
                                    pGltfNode->rotation.z = s * t;
                                    pGltfNode->rotation.w = (m[0 * 3 + 1] - m[1 * 3 + 0]) * s;
                                    pGltfNode->rotation.x = (m[2 * 3 + 0] + m[0 * 3 + 2]) * s;
                                    pGltfNode->rotation.y = (m[1 * 3 + 2] + m[2 * 3 + 1]) * s;
                                }
                            }

                            auto rotation = node.GetChildByName("rotation");
                            if (rotation != nullptr) {
                                pGltfNode->rotation.x =
                                    rotation->GetItemByIndex(0)->GetFloatValue();
                                pGltfNode->rotation.y =
                                    rotation->GetItemByIndex(1)->GetFloatValue();
                                pGltfNode->rotation.z =
                                    rotation->GetItemByIndex(2)->GetFloatValue();
                                pGltfNode->rotation.w =
                                    rotation->GetItemByIndex(3)->GetFloatValue();
                            }

                            auto scale = node.GetChildByName("scale");
                            if (scale != nullptr) {
                                pGltfNode->scale.x = scale->GetItemByIndex(0)->GetFloatValue();
                                pGltfNode->scale.y = scale->GetItemByIndex(1)->GetFloatValue();
                                pGltfNode->scale.z = scale->GetItemByIndex(2)->GetFloatValue();
                            }

                            auto translation = node.GetChildByName("translation");
                            if (translation != nullptr) {
                                pGltfNode->translation.x =
                                    translation->GetItemByIndex(0)->GetFloatValue();
                                pGltfNode->translation.y =
                                    translation->GetItemByIndex(1)->GetFloatValue();
                                pGltfNode->translation.z =
                                    translation->GetItemByIndex(2)->GetFloatValue();
                            }

                            pGltfNode->skinIndex = node.GetChildInt32ByName("skin", -1);

                            int cameraIndex = node.GetChildInt32ByName("camera", -1);
                            if (cameraIndex >= 0) {
                                if (cameraIndex >= static_cast<int>(modelFile.Cameras.size())) {
                                    ALOGW(
                                        "Error: Invalid camera index %d on gltfNode", cameraIndex);
                                    loaded = false;
                                }
                                pGltfNode->camera = &modelFile.Cameras[cameraIndex];
                            }

                            int meshIndex = node.GetChildInt32ByName("mesh", -1);
                            if (meshIndex >= 0) {
                                if (meshIndex >= static_cast<int>(modelFile.Models.size())) {
                                    ALOGW("Error: Invalid Mesh index %d on gltfNode", meshIndex);
                                    loaded = false;
                                }
                                pGltfNode->model = &modelFile.Models[meshIndex];

                                // initialize morph target weights
                                if (!pGltfNode->model->weights.empty()) {
                                    const OVR::JsonReader weightsReader(
                                        node.GetChildByName("weights"));
                                    if (weightsReader.IsArray()) {
                                        // use node weights if it is defined
                                        while (!weightsReader.IsEndOfArray() && loaded) {
                                            pGltfNode->weights.push_back(
                                                weightsReader.GetNextArrayFloat(0.0f));
                                        }
                                        if (pGltfNode->weights.size() !=
                                            pGltfNode->model->weights.size()) {
                                            ALOGW("Error: weights count mismatch on gltfNode");
                                            loaded = false;
                                        }
                                    } else {
                                        // when node.weights is undefined, mesh.weights property
                                        // MUST be used instead
                                        pGltfNode->weights = pGltfNode->model->weights;
                                    }
                                }
                            }

                            Matrix4f localTransform;
                            CalculateTransformFromRTS(
                                &localTransform,
                                pGltfNode->rotation,
                                pGltfNode->translation,
                                pGltfNode->scale);
                            pGltfNode->SetLocalTransform(localTransform);

                            const OVR::JsonReader children = node.GetChildByName("children");
                            if (children.IsArray()) {
                                while (!children.IsEndOfArray()) {
                                    auto child = children.GetNextArrayElement();
                                    int childIndex = child->GetInt32Value();

                                    if (childIndex < 0 ||
                                        childIndex >= static_cast<int>(modelFile.Nodes.size())) {
                                        ALOGW(
                                            "Error: Invalid child node index %d for %d in gltfNode",
                                            childIndex,
                                            nodeIndex);
                                        loaded = false;
                                    }

                                    pGltfNode->children.push_back(childIndex);
                                    modelFile.Nodes[childIndex].parentIndex = nodeIndex;
                                }
                            }

                            nodeIndex++;
                        }
                    }
                }
            } // END NODES

            if (loaded) { // ANIMATIONS
                LOGV("loading Animations");
                auto animationsJSON = models.GetChildByName("animations");
                const OVR::JsonReader animations = animationsJSON;
                if (animations.IsArray()) {
                    int animationCount = 0;
                    while (!animations.IsEndOfArray() && loaded) {
                        modelFile.Animations.resize(animationsJSON->GetArraySize());
                        const OVR::JsonReader animation(animations.GetNextArrayElement());
                        if (animation.IsObject()) {
                            ModelAnimation& modelAnimation = modelFile.Animations[animationCount];

                            modelAnimation.name = animation.GetChildStringByName("name");

                            // ANIMATION SAMPLERS
                            const OVR::JsonReader samplers = animation.GetChildByName("samplers");
                            if (samplers.IsArray()) {
                                while (!samplers.IsEndOfArray() && loaded) {
                                    ModelAnimationSampler modelAnimationSampler;
                                    const OVR::JsonReader sampler = samplers.GetNextArrayElement();
                                    if (sampler.IsObject()) {
                                        int inputIndex = sampler.GetChildInt32ByName("input", -1);
                                        if (inputIndex < 0 ||
                                            inputIndex >=
                                                static_cast<int>(modelFile.Accessors.size())) {
                                            ALOGW(
                                                "bad input index %d on sample on %s",
                                                inputIndex,
                                                modelAnimation.name.c_str());
                                            loaded = false;
                                        } else {
                                            modelAnimationSampler.input =
                                                &modelFile.Accessors[inputIndex];
                                            if (modelAnimationSampler.input->componentType !=
                                                GL_FLOAT) {
                                                ALOGW(
                                                    "animation sampler input not of type GL_FLOAT on '%s'",
                                                    modelAnimation.name.c_str());
                                                loaded = false;
                                            }
                                        }

                                        int outputIndex = sampler.GetChildInt32ByName("output", -1);
                                        if (outputIndex < 0 ||
                                            outputIndex >=
                                                static_cast<int>(modelFile.Accessors.size())) {
                                            ALOGW(
                                                "bad input outputIndex %d on sample on %s",
                                                outputIndex,
                                                modelAnimation.name.c_str());
                                            loaded = false;
                                        } else {
                                            modelAnimationSampler.output =
                                                &modelFile.Accessors[outputIndex];
                                        }

                                        std::string interpolation =
                                            sampler.GetChildStringByName("interpolation", "LINEAR");
                                        if (OVR::OVR_stricmp(interpolation.c_str(), "LINEAR") ==
                                            0) {
                                            modelAnimationSampler.interpolation =
                                                MODEL_ANIMATION_INTERPOLATION_LINEAR;
                                        } else if (
                                            OVR::OVR_stricmp(interpolation.c_str(), "STEP") == 0) {
                                            modelAnimationSampler.interpolation =
                                                MODEL_ANIMATION_INTERPOLATION_STEP;
                                        } else if (
                                            OVR::OVR_stricmp(
                                                interpolation.c_str(), "CATMULLROMSPLINE") == 0) {
                                            modelAnimationSampler.interpolation =
                                                MODEL_ANIMATION_INTERPOLATION_CATMULLROMSPLINE;
                                        } else if (
                                            OVR::OVR_stricmp(
                                                interpolation.c_str(), "CUBICSPLINE") == 0) {
                                            modelAnimationSampler.interpolation =
                                                MODEL_ANIMATION_INTERPOLATION_CUBICSPLINE;
                                        } else {
                                            ALOGW(
                                                "Error: Invalid interpolation type '%s' on sampler on animation '%s'",
                                                interpolation.c_str(),
                                                modelAnimation.name.c_str());
                                            loaded = false;
                                        }

                                        modelAnimation.samplers.push_back(modelAnimationSampler);
                                    } else {
                                        ALOGW("bad sampler on '%s'", modelAnimation.name.c_str());
                                        loaded = false;
                                    }
                                }
                            } else {
                                ALOGW("bad samplers on '%s'", modelAnimation.name.c_str());
                                loaded = false;
                            } // END ANIMATION SAMPLERS

                            // ANIMATION CHANNELS
                            const OVR::JsonReader channels = animation.GetChildByName("channels");
                            if (channels.IsArray()) {
                                while (!channels.IsEndOfArray() && loaded) {
                                    const OVR::JsonReader channel = channels.GetNextArrayElement();
                                    if (channel.IsObject()) {
                                        ModelAnimationChannel modelAnimationChannel;

                                        int samplerIndex =
                                            channel.GetChildInt32ByName("sampler", -1);
                                        if (samplerIndex < 0 ||
                                            samplerIndex >=
                                                static_cast<int>(modelAnimation.samplers.size())) {
                                            ALOGW(
                                                "bad samplerIndex %d on channel on %s",
                                                samplerIndex,
                                                modelAnimation.name.c_str());
                                            loaded = false;
                                        } else {
                                            modelAnimationChannel.sampler =
                                                &modelAnimation.samplers[samplerIndex];
                                        }

                                        const OVR::JsonReader target =
                                            channel.GetChildByName("target");
                                        if (target.IsObject()) {
                                            // not required so -1 means do not do animation.
                                            int nodeIndex = target.GetChildInt32ByName("node", -1);
                                            if (nodeIndex >=
                                                static_cast<int>(modelFile.Nodes.size())) {
                                                ALOGW(
                                                    "bad nodeIndex %d on target on '%s'",
                                                    nodeIndex,
                                                    modelAnimation.name.c_str());
                                                loaded = false;
                                            } else {
                                                modelAnimationChannel.nodeIndex = nodeIndex;
                                            }

                                            std::string path = target.GetChildStringByName("path");

                                            if (OVR::OVR_stricmp(path.c_str(), "translation") ==
                                                0) {
                                                modelAnimationChannel.path =
                                                    MODEL_ANIMATION_PATH_TRANSLATION;
                                            } else if (
                                                OVR::OVR_stricmp(path.c_str(), "rotation") == 0) {
                                                modelAnimationChannel.path =
                                                    MODEL_ANIMATION_PATH_ROTATION;
                                            } else if (
                                                OVR::OVR_stricmp(path.c_str(), "scale") == 0) {
                                                modelAnimationChannel.path =
                                                    MODEL_ANIMATION_PATH_SCALE;
                                            } else if (
                                                OVR::OVR_stricmp(path.c_str(), "weights") == 0) {
                                                modelAnimationChannel.path =
                                                    MODEL_ANIMATION_PATH_WEIGHTS;
                                            } else {
                                                ALOGW(
                                                    " bad path '%s' on target on '%s'",
                                                    path.c_str(),
                                                    modelAnimation.name.c_str());
                                                loaded = false;
                                            }

                                            if (loaded) {
                                                // validation sampler now that we have the path
                                                auto sampler = modelAnimationChannel.sampler;
                                                int inputCount = sampler->input->count;
                                                int outputCount = sampler->output->count;
                                                if (modelAnimationChannel.path ==
                                                    MODEL_ANIMATION_PATH_WEIGHTS) {
                                                    auto node =
                                                        modelFile
                                                            .Nodes[modelAnimationChannel.nodeIndex];
                                                    outputCount /=
                                                        node.model->surfaces[0].targets.size();
                                                }

                                                if (sampler->interpolation ==
                                                        MODEL_ANIMATION_INTERPOLATION_LINEAR ||
                                                    sampler->interpolation ==
                                                        MODEL_ANIMATION_INTERPOLATION_STEP) {
                                                    if (inputCount != outputCount) {
                                                        ALOGW(
                                                            "input (%d) and output (%d) have different counts on sampler on animation '%s'",
                                                            inputCount,
                                                            outputCount,
                                                            modelAnimation.name.c_str());
                                                        loaded = false;

                                                        auto node =
                                                            modelFile.Nodes[modelAnimationChannel
                                                                                .nodeIndex];
                                                    }
                                                    if (inputCount < 2) {
                                                        ALOGW(
                                                            "invalid number of samples on animation sampler input %d '%s'",
                                                            inputCount,
                                                            modelAnimation.name.c_str());
                                                        loaded = false;
                                                    }
                                                } else if (
                                                    sampler->interpolation ==
                                                    MODEL_ANIMATION_INTERPOLATION_CATMULLROMSPLINE) {
                                                    if ((inputCount + 2) != outputCount) {
                                                        ALOGW(
                                                            "input and output have invalid counts on sampler on animation '%s'",
                                                            modelAnimation.name.c_str());
                                                        loaded = false;
                                                    }
                                                    if (inputCount < 4) {
                                                        ALOGW(
                                                            "invalid number of samples on animation sampler input %d '%s'",
                                                            inputCount,
                                                            modelAnimation.name.c_str());
                                                        loaded = false;
                                                    }
                                                } else if (
                                                    sampler->interpolation ==
                                                    MODEL_ANIMATION_INTERPOLATION_CUBICSPLINE) {
                                                    if (inputCount != (outputCount * 3)) {
                                                        ALOGW(
                                                            "input and output have invalid counts on sampler on animation '%s'",
                                                            modelAnimation.name.c_str());
                                                        loaded = false;
                                                    }
                                                    if (inputCount < 2) {
                                                        ALOGW(
                                                            "invalid number of samples on animation sampler input %d '%s'",
                                                            inputCount,
                                                            modelAnimation.name.c_str());
                                                        loaded = false;
                                                    }
                                                } else {
                                                    ALOGW(
                                                        "unkown animaiton interpolation on '%s'",
                                                        modelAnimation.name.c_str());
                                                    loaded = false;
                                                }
                                            }
                                        } else {
                                            ALOGW(
                                                "bad target object on '%s'",
                                                modelAnimation.name.c_str());
                                            loaded = false;
                                        }

                                        const OVR::JsonReader extras =
                                            channel.GetChildByName("extras");
                                        if (extras.IsObject()) {
                                            // additive index only make sense for weights
                                            if (modelAnimationChannel.path ==
                                                MODEL_ANIMATION_PATH_WEIGHTS) {
                                                modelAnimationChannel.additiveWeightIndex =
                                                    extras.GetChildInt32ByName(
                                                        "additiveWeightIndex", -1);
                                            }
                                        }

                                        modelAnimation.channels.push_back(modelAnimationChannel);
                                    } else {
                                        ALOGW("bad channel on '%s'", modelAnimation.name.c_str());
                                        loaded = false;
                                    }
                                }
                            } else {
                                ALOGW("bad channels on '%s'", modelAnimation.name.c_str());
                                loaded = false;
                            } // END ANIMATION CHANNELS

                            animationCount++;
                        } else {
                            ALOGW("bad animation object in animations");
                            loaded = false;
                        }
                    }
                }
            } // END ANIMATIONS

            if (loaded) { // ANIMATION TIMELINES
                // create the timelines
                for (int i = 0; i < static_cast<int>(modelFile.Animations.size()); i++) {
                    for (int j = 0; j < static_cast<int>(modelFile.Animations[i].samplers.size());
                         j++) {
                        // if there isn't already a timeline with this accessor,
                        // create a new one.
                        ModelAnimationSampler& sampler = modelFile.Animations[i].samplers[j];
                        bool foundTimeLine = false;
                        for (int timeLineIndex = 0;
                             timeLineIndex < static_cast<int>(modelFile.AnimationTimeLines.size());
                             timeLineIndex++) {
                            if (modelFile.AnimationTimeLines[timeLineIndex].accessor ==
                                sampler.input) {
                                foundTimeLine = true;
                                sampler.timeLineIndex = timeLineIndex;
                                break;
                            }
                        }

                        if (!foundTimeLine) {
                            ModelAnimationTimeLine timeline;
                            timeline.Initialize(sampler.input);
                            if (static_cast<int>(modelFile.AnimationTimeLines.size()) == 0) {
                                modelFile.animationStartTime = timeline.startTime;
                                modelFile.animationEndTime = timeline.endTime;
                            } else {
                                modelFile.animationStartTime = std::min<float>(
                                    modelFile.animationStartTime, timeline.startTime);
                                modelFile.animationEndTime =
                                    std::max<float>(modelFile.animationEndTime, timeline.endTime);
                            }

                            modelFile.AnimationTimeLines.push_back(timeline);
                            sampler.timeLineIndex =
                                static_cast<int>(modelFile.AnimationTimeLines.size()) - 1;
                        }
                    }
                }
            } // END ANIMATION TIMELINES

            if (loaded) { // SKINS
                LOGV("Loading skins");
                const OVR::JsonReader skins(models.GetChildByName("skins"));
                if (skins.IsArray()) {
                    while (!skins.IsEndOfArray() && loaded) {
                        const OVR::JsonReader skin(skins.GetNextArrayElement());
                        if (skin.IsObject()) {
                            ModelSkin newSkin;

                            newSkin.name = skin.GetChildStringByName("name");
                            newSkin.skeletonRootIndex = skin.GetChildInt32ByName("skeleton", -1);
                            int bindMatricesAccessorIndex =
                                skin.GetChildInt32ByName("inverseBindMatrices", -1);
                            if (bindMatricesAccessorIndex >=
                                static_cast<int>(modelFile.Accessors.size())) {
                                ALOGW(
                                    "inverseBindMatrices %d higher then number of accessors on model: %s",
                                    bindMatricesAccessorIndex,
                                    modelFile.FileName.c_str());
                                loaded = false;
                            } else if (bindMatricesAccessorIndex >= 0) {
                                ModelAccessor& acc = modelFile.Accessors[bindMatricesAccessorIndex];
                                newSkin.inverseBindMatricesAccessor =
                                    &modelFile.Accessors[bindMatricesAccessorIndex];
                                for (int i = 0; i < acc.count; i++) {
                                    Matrix4f matrix;
                                    memcpy(
                                        matrix.M[0],
                                        ((float*)(acc.BufferData())) + i * 16,
                                        sizeof(float) * 16);
                                    matrix.Transpose();
                                    newSkin.inverseBindMatrices.push_back(matrix);
                                }
                            }

                            const OVR::JsonReader joints = skin.GetChildByName("joints");
                            if (joints.IsArray()) {
                                while (!joints.IsEndOfArray() && loaded) {
                                    int jointIndex = joints.GetNextArrayInt32(-1);
                                    if (jointIndex < 0 ||
                                        jointIndex >= static_cast<int>(modelFile.Nodes.size())) {
                                        ALOGW(
                                            "bad jointindex %d on skin on model: %s",
                                            jointIndex,
                                            modelFile.FileName.c_str());
                                        loaded = false;
                                    } else {
                                        ALOGW(
                                            " SKIN - jointIndex: %02d name: %s",
                                            jointIndex,
                                            modelFile.Nodes[jointIndex].name.c_str());
                                    }
                                    newSkin.jointIndexes.push_back(jointIndex);
                                }
                            } else {
                                ALOGW("no joints on skin on model: %s", modelFile.FileName.c_str());
                                loaded = false;
                            }

                            /// Up the number here
                            const int maxJointsAllowed = 96; /// MAX_JOINTS

                            if (static_cast<int>(newSkin.jointIndexes.size()) > maxJointsAllowed) {
                                ALOGW(
                                    "%d joints on skin on model: %s, currently only %d allowed ",
                                    static_cast<int>(newSkin.jointIndexes.size()),
                                    modelFile.FileName.c_str(),
                                    maxJointsAllowed);
                                loaded = false;
                            }

                            modelFile.Skins.push_back(newSkin);
                        } else {
                            ALOGW("bad skin on model: %s", modelFile.FileName.c_str());
                            loaded = false;
                        }
                    }
                }

            } // END SKINS

            if (loaded) { // verify skin indexes on nodes
                for (int i = 0; i < static_cast<int>(modelFile.Nodes.size()); i++) {
                    if (modelFile.Nodes[i].skinIndex > static_cast<int>(modelFile.Skins.size())) {
                        ALOGW(
                            "bad skin index %d on node %d on model: %s",
                            modelFile.Nodes[i].skinIndex,
                            i,
                            modelFile.FileName.c_str());
                        loaded = false;
                    }
                }
            }

            if (loaded) { // SCENES
                LOGV("Loading scenes");
                const OVR::JsonReader scenes(models.GetChildByName("scenes"));
                if (scenes.IsArray()) {
                    while (!scenes.IsEndOfArray() && loaded) {
                        const OVR::JsonReader scene(scenes.GetNextArrayElement());
                        if (scene.IsObject()) {
                            ModelSubScene newGltfScene;

                            newGltfScene.name = scene.GetChildStringByName("name");

                            const OVR::JsonReader nodes = scene.GetChildByName("nodes");
                            if (nodes.IsArray()) {
                                while (!nodes.IsEndOfArray()) {
                                    const int nodeIndex = nodes.GetNextArrayInt32();
                                    if (nodeIndex < 0 ||
                                        nodeIndex >= static_cast<int>(modelFile.Nodes.size())) {
                                        ALOGW("Error: Invalid nodeIndex %d in Model", nodeIndex);
                                        loaded = false;
                                    }
                                    newGltfScene.nodes.push_back(nodeIndex);
                                }
                            }
                            modelFile.SubScenes.push_back(newGltfScene);
                        }
                    }
                }

                // Calculate the nodes global transforms;
                for (int i = 0; i < static_cast<int>(modelFile.SubScenes.size()); i++) {
                    for (int j = 0; j < static_cast<int>(modelFile.SubScenes[i].nodes.size());
                         j++) {
                        modelFile.Nodes[modelFile.SubScenes[i].nodes[j]].RecalculateGlobalTransform(
                            modelFile);
                    }
                }
            } // END SCENES

            if (loaded) {
                const int sceneIndex = models.GetChildInt32ByName("scene", -1);
                if (sceneIndex >= 0) {
                    if (sceneIndex >= static_cast<int>(modelFile.SubScenes.size())) {
                        ALOGW("Error: Invalid initial scene index %d on gltfFile", sceneIndex);
                        loaded = false;
                    }
                    modelFile.SubScenes[sceneIndex].visible = true;
                }
            }

            // print out the scene info
            if (loaded) {
                LOGV("Model Loaded:     '%s'", modelFile.FileName.c_str());
                LOGV("\tBuffers        : %d", static_cast<int>(modelFile.Buffers.size()));
                LOGV("\tBufferViews    : %d", static_cast<int>(modelFile.BufferViews.size()));
                LOGV("\tAccessors      : %d", static_cast<int>(modelFile.Accessors.size()));
                LOGV("\tTextures       : %d", static_cast<int>(modelFile.Textures.size()));
                LOGV("\tTextureWrappers: %d", static_cast<int>(modelFile.TextureWrappers.size()));
                LOGV("\tMaterials      : %d", static_cast<int>(modelFile.Materials.size()));
                LOGV("\tModels         : %d", static_cast<int>(modelFile.Models.size()));
                LOGV("\tCameras        : %d", static_cast<int>(modelFile.Cameras.size()));
                LOGV("\tNodes          : %d", static_cast<int>(modelFile.Nodes.size()));
                LOGV("\tAnimations     : %d", static_cast<int>(modelFile.Animations.size()));
                LOGV(
                    "\tAnimationTimeLines: %d",
                    static_cast<int>(modelFile.AnimationTimeLines.size()));
                LOGV("\tSkins          : %d", static_cast<int>(modelFile.Skins.size()));
                LOGV("\tSubScenes      : %d", static_cast<int>(modelFile.SubScenes.size()));
            } else {
                ALOGW("Could not load model '%s'", modelFile.FileName.c_str());
            }

            // #TODO: what to do with our collision?  One possible answer is extras on the data
            // tagging certain models as collision. Collision Model Ground Collision Model Ray-Trace
            // Model
        } else {
            loaded = false;
        }
    }
    return loaded;
}

// A gltf directory zipped up into an ovrscene file.
bool LoadModelFile_glTF_OvrScene(
    ModelFile* modelFilePtr,
    unzFile zfp,
    const char* fileName,
    const char* fileData,
    const int fileDataLength,
    const ModelGlPrograms& programs,
    const MaterialParms& materialParms,
    ModelGeo* outModelGeo) {
    ModelFile& modelFile = *modelFilePtr;

    // Since we are doing a zip file, we are going to parse through the zip file many times to find
    // the different data points.
    const char* gltfJson = nullptr;
    {
        // LOGCPUTIME( "Loading GLTF file" );
        for (int ret = unzGoToFirstFile(zfp); ret == UNZ_OK; ret = unzGoToNextFile(zfp)) {
            unz_file_info finfo;
            char entryName[256];
            unzGetCurrentFileInfo(
                zfp, &finfo, entryName, sizeof(entryName), nullptr, 0, nullptr, 0);
            const size_t entryLength = strlen(entryName);
            const char* extension = (entryLength >= 5) ? &entryName[entryLength - 5] : entryName;

            if (OVR::OVR_stricmp(extension, ".gltf") == 0) {
                LOGV("found %s", entryName);
                uint8_t* buffer = ReadBufferFromZipFile(zfp, (const uint8_t*)fileData, finfo);

                if (buffer == nullptr) {
                    ALOGW(
                        "LoadModelFile_glTF_OvrScene:Failed to read %s from %s",
                        entryName,
                        fileName);
                    continue;
                }

                if (gltfJson == nullptr) {
                    gltfJson = (const char*)buffer;
                } else {
                    ALOGW("LoadModelFile_glTF_OvrScene: multiple .gltf files found %s", fileName);
                    delete[] buffer;
                    continue;
                }
            }
        }
    }

    bool loaded = true;

    const char* error = nullptr;
    auto json = OVR::JSON::Parse(gltfJson, &error);
    if (json == nullptr) {
        ALOGW(
            "LoadModelFile_glTF_OvrScene: Error loading %s : %s",
            modelFilePtr->FileName.c_str(),
            error);
        loaded = false;
    } else {
        const OVR::JsonReader models(json);
        if (models.IsObject()) {
            // Buffers BufferViews and Images need access to the data location, in this case the zip
            // file.
            //	after they are loaded it should be identical wether the input is a zip file, a
            // folder structure or a bgltf file.
            if (loaded) { // BUFFERS
                // LOGCPUTIME( "Loading buffers" );
                // gather all the buffers, and try to load them from the zip file.
                const OVR::JsonReader buffers(models.GetChildByName("buffers"));
                if (buffers.IsArray()) {
                    while (!buffers.IsEndOfArray() && loaded) {
                        const OVR::JsonReader bufferReader(buffers.GetNextArrayElement());
                        if (bufferReader.IsObject()) {
                            ModelBuffer newGltfBuffer;

                            const std::string name = bufferReader.GetChildStringByName("name");
                            const std::string uri = bufferReader.GetChildStringByName("uri");
                            newGltfBuffer.byteLength =
                                bufferReader.GetChildInt32ByName("byteLength", -1);

                            // #TODO: proper uri reading.  right now, assuming its a file name.
                            if (OVR::OVR_stricmp(uri.c_str() + (uri.length() - 4), ".bin") != 0) {
                                // #TODO: support loading buffers from data other then a bin file.
                                // i.e. inline buffers etc.
                                ALOGW("Loading buffers other then bin files currently unsupported");
                                loaded = false;
                            }
                            int bufferLength = 0;
                            uint8_t* tempbuffer = ReadFileBufferFromZipFile(
                                zfp, uri.c_str(), bufferLength, (const uint8_t*)fileData);
                            if (tempbuffer == nullptr) {
                                ALOGW("could not load buffer for gltfBuffer");
                                loaded = false;
                            } else {
                                // ensure the buffer is aligned.
                                size_t alignedBufferSize = (bufferLength / 4 + 1) * 4;
                                newGltfBuffer.bufferData.resize(alignedBufferSize);
                                memcpy(newGltfBuffer.bufferData.data(), tempbuffer, bufferLength);
                            }

                            if (newGltfBuffer.byteLength > (size_t)bufferLength) {
                                ALOGW(
                                    "%d byteLength > bufferLength loading gltfBuffer %d",
                                    (int)newGltfBuffer.byteLength,
                                    bufferLength);
                                loaded = false;
                            }

                            const char* bufferName;
                            if (name.length() > 0) {
                                bufferName = name.c_str();
                            } else {
                                bufferName = uri.c_str();
                            }

                            newGltfBuffer.name = bufferName;

                            modelFile.Buffers.push_back(newGltfBuffer);
                        }
                    }
                }
            } // END BUFFERS

            if (loaded) { // BUFFERVIEW
                LOGV("Loading bufferviews");
                const OVR::JsonReader bufferViews(models.GetChildByName("bufferViews"));
                if (bufferViews.IsArray()) {
                    while (!bufferViews.IsEndOfArray() && loaded) {
                        const OVR::JsonReader bufferview(bufferViews.GetNextArrayElement());
                        if (bufferview.IsObject()) {
                            ModelBufferView newBufferView;

                            newBufferView.name = bufferview.GetChildStringByName("name");
                            const int buffer = bufferview.GetChildInt32ByName("buffer");
                            newBufferView.byteOffset = bufferview.GetChildInt32ByName("byteOffset");
                            newBufferView.byteLength = bufferview.GetChildInt32ByName("byteLength");
                            newBufferView.byteStride = bufferview.GetChildInt32ByName("byteStride");
                            newBufferView.target = bufferview.GetChildInt32ByName("target");

                            if (buffer < 0 || buffer >= (const int)modelFile.Buffers.size()) {
                                ALOGW("Error: Invalid buffer Index in gltfBufferView");
                                loaded = false;
                            }
                            if (newBufferView.byteStride < 0 || newBufferView.byteStride > 255) {
                                ALOGW("Error: Invalid byeStride in gltfBufferView");
                                loaded = false;
                            }
                            if (newBufferView.target < 0) {
                                ALOGW("Error: Invalid target in gltfBufferView");
                                loaded = false;
                            }

                            newBufferView.buffer = &modelFile.Buffers[buffer];
                            modelFile.BufferViews.push_back(newBufferView);
                        }
                    }
                }
            } // END BUFFERVIEWS

            if (loaded) { // IMAGES
                // LOGCPUTIME( "Loading image textures" );
                // gather all the images, and try to load them from the zip file.
                const OVR::JsonReader images(models.GetChildByName("images"));
                if (images.IsArray()) {
                    while (!images.IsEndOfArray()) {
                        const OVR::JsonReader image(images.GetNextArrayElement());
                        if (image.IsObject()) {
                            const std::string name = image.GetChildStringByName("name");
                            const std::string uri = image.GetChildStringByName("uri");
                            int bufferView = image.GetChildInt32ByName("bufferView", -1);
                            if (bufferView >= 0) {
                                // #TODO: support bufferView index for image files.
                                ALOGW(
                                    "Loading images from bufferView currently unsupported, defaulting image");
                                // Create a default texture.
                                LoadModelFileTexture(
                                    modelFile, "DefaultImage", nullptr, 0, materialParms);
                            } else {
                                // check to make sure the image is ktx.
                                if (OVR::OVR_stricmp(uri.c_str() + (uri.length() - 4), ".ktx") !=
                                    0) {
                                    // #TODO: Try looking for a ktx image before we load the non ktx
                                    // image.
                                    ALOGW(
                                        "Loading images other then ktx is not advised. %s",
                                        uri.c_str());

                                    int bufferLength = 0;
                                    uint8_t* buffer = ReadFileBufferFromZipFile(
                                        zfp, uri.c_str(), bufferLength, (const uint8_t*)fileData);
                                    const char* imageName = uri.c_str();

                                    LoadModelFileTexture(
                                        modelFile,
                                        imageName,
                                        (const char*)buffer,
                                        bufferLength,
                                        materialParms);
                                } else {
                                    int bufferLength = 0;
                                    uint8_t* buffer = ReadFileBufferFromZipFile(
                                        zfp, uri.c_str(), bufferLength, (const uint8_t*)fileData);
                                    const char* imageName = uri.c_str();

                                    LoadModelFileTexture(
                                        modelFile,
                                        imageName,
                                        (const char*)buffer,
                                        bufferLength,
                                        materialParms);
                                }
                            }
                        }
                    }
                }
            } // END images
            // End of section dependent on zip file.
        } else {
            ALOGW("error: could not parse json for gltf");
            loaded = false;
        }

        if (loaded) {
            loaded =
                LoadModelFile_glTF_Json(modelFile, gltfJson, programs, materialParms, outModelGeo);
        }
    }

    if (gltfJson != nullptr && (gltfJson < fileData || gltfJson > fileData + fileDataLength)) {
        delete gltfJson;
    }

    return loaded;
}

ModelFile* LoadModelFile_glB(
    const char* fileName,
    const char* fileData,
    const int fileDataLength,
    const ModelGlPrograms& programs,
    const MaterialParms& materialParms,
    ModelGeo* outModelGeo) {
    // LOGCPUTIME( "LoadModelFile_glB" );

    ModelFile* modelFilePtr = new ModelFile;
    ModelFile& modelFile = *modelFilePtr;

    modelFile.FileName = fileName;
    modelFile.UsingSrgbTextures = materialParms.UseSrgbTextureFormats;

    bool loaded = true;

    uint32_t fileDataIndex = 0;
    uint32_t fileDataRemainingLength = fileDataLength;
    glTFBinaryHeader header;
    if (fileDataRemainingLength < sizeof(header)) {
        ALOGW("Error: could not load glb gltfHeader");
        loaded = false;
    }

    if (loaded) {
        memcpy(&header, &fileData[fileDataIndex], sizeof(header));
        fileDataIndex += sizeof(header);
        fileDataRemainingLength -= sizeof(header);

        if (header.magic != GLTF_BINARY_MAGIC) {
            ALOGW("Error: invalid glb gltfHeader magic");
            loaded = false;
        }

        if (header.version != GLTF_BINARY_VERSION) {
            ALOGW("Error: invalid glb gltfHeader version");
            loaded = false;
        }

        if (header.length != (uint32_t)fileDataLength) {
            ALOGW("Error: invalid glb gltfHeader length");
            loaded = false;
        }
    }

    if (loaded && fileDataRemainingLength > sizeof(uint32_t) * 2) {
        uint32_t chunkType = 0;
        uint32_t chunkLength = 0;

        memcpy(&chunkLength, &fileData[fileDataIndex], sizeof(uint32_t));
        fileDataIndex += sizeof(uint32_t);
        fileDataRemainingLength -= sizeof(uint32_t);
        memcpy(&chunkType, &fileData[fileDataIndex], sizeof(uint32_t));
        fileDataIndex += sizeof(uint32_t);
        fileDataRemainingLength -= sizeof(uint32_t);

        if (chunkType != GLTF_BINARY_CHUNKTYPE_JSON) {
            ALOGW("Error: glb first chunk not JSON");
            loaded = false;
        }

        std::shared_ptr<OVR::JSON> json = nullptr;
        const char* gltfJson = nullptr;
        if (loaded) {
            const char* error = nullptr;
            gltfJson = &fileData[fileDataIndex];
            json = OVR::JSON::Parse(gltfJson, &error);
            fileDataIndex += chunkLength;
            fileDataRemainingLength -= chunkLength;

            if (json == nullptr) {
                ALOGW(
                    "LoadModelFile_glB: Error Parsing JSON %s : %s",
                    modelFilePtr->FileName.c_str(),
                    error);
                loaded = false;
            }
        }

        const char* buffer = nullptr;
        uint32_t bufferLength = 0;
        if (loaded) {
            if (fileDataRemainingLength > sizeof(uint32_t) * 2) {
                uint32_t bufferChunkType = 0;
                memcpy(&bufferLength, &fileData[fileDataIndex], sizeof(uint32_t));
                fileDataIndex += sizeof(uint32_t);
                fileDataRemainingLength -= sizeof(uint32_t);
                memcpy(&bufferChunkType, &fileData[fileDataIndex], sizeof(uint32_t));
                fileDataIndex += sizeof(uint32_t);
                fileDataRemainingLength -= sizeof(uint32_t);

                if (bufferChunkType != GLTF_BINARY_CHUNKTYPE_BINARY) {
                    ALOGW("Error: glb second chunk not binary");
                    loaded = false;
                } else if (bufferLength > fileDataRemainingLength) {
                    ALOGW("Error: glb binary chunk length greater then remaining buffer");
                    loaded = false;
                } else {
                    if (bufferLength < fileDataRemainingLength) {
                        ALOGW("Error: glb binary chunk length less then remaining buffer");
                    }
                    buffer = &fileData[fileDataIndex];
                }
            } else {
                ALOGW("Not enough data remaining to parse glB buffer");
                loaded = false;
            }
        }

        if (loaded) {
            const OVR::JsonReader models(json);
            if (models.IsObject()) {
                // Buffers BufferViews and Images need access to the data location, in this case the
                // buffer inside the glb file.
                //	after they are loaded it should be identical wether the input is a zip file, a
                // folder structure or a glb file.
                if (loaded) { // BUFFERS
                    LOGV("Loading buffers");
                    // gather all the buffers, and try to load them from the zip file.
                    const OVR::JsonReader buffers(models.GetChildByName("buffers"));
                    if (buffers.IsArray()) {
                        while (!buffers.IsEndOfArray() && loaded) {
                            if (static_cast<int>(modelFile.Buffers.size()) > 0) {
                                ALOGW("Error: glB file contains more then one buffer");
                                loaded = false;
                            }

                            const OVR::JsonReader bufferReader(buffers.GetNextArrayElement());
                            if (bufferReader.IsObject() && loaded) {
                                ModelBuffer newGltfBuffer;

                                const std::string name = bufferReader.GetChildStringByName("name");
                                const std::string uri = bufferReader.GetChildStringByName("uri");
                                newGltfBuffer.byteLength =
                                    bufferReader.GetChildInt32ByName("byteLength", -1);

                                //  #TODO: proper uri reading.  right now, assuming its a file name.
                                if (!uri.empty()) {
                                    ALOGW(
                                        "Loading buffers with an uri currently unsupported in glb");
                                    loaded = false;
                                }

                                if (newGltfBuffer.byteLength > (size_t)bufferLength) {
                                    ALOGW(
                                        "%d byteLength > bufferLength loading gltfBuffer %d",
                                        (int)newGltfBuffer.byteLength,
                                        bufferLength);
                                    loaded = false;
                                }

                                // ensure the buffer is aligned.
                                size_t alignedBufferSize = (bufferLength / 4 + 1) * 4;
                                newGltfBuffer.bufferData.resize(alignedBufferSize);
                                memcpy(
                                    newGltfBuffer.bufferData.data(),
                                    buffer,
                                    newGltfBuffer.byteLength);

                                const char* bufferName;
                                if (name.length() > 0) {
                                    bufferName = name.c_str();
                                } else {
                                    bufferName = "glB_Buffer";
                                }

                                newGltfBuffer.name = bufferName;

                                modelFile.Buffers.push_back(newGltfBuffer);
                            }
                        }
                    }
                } // END BUFFERS

                if (loaded) { // BUFFERVIEW
                    LOGV("Loading bufferviews");
                    const OVR::JsonReader bufferViews(models.GetChildByName("bufferViews"));
                    if (bufferViews.IsArray()) {
                        while (!bufferViews.IsEndOfArray() && loaded) {
                            const OVR::JsonReader bufferview(bufferViews.GetNextArrayElement());
                            if (bufferview.IsObject()) {
                                ModelBufferView newBufferView;

                                newBufferView.name = bufferview.GetChildStringByName("name");
                                const int bufferIndex = bufferview.GetChildInt32ByName("buffer");
                                newBufferView.byteOffset =
                                    bufferview.GetChildInt32ByName("byteOffset");
                                newBufferView.byteLength =
                                    bufferview.GetChildInt32ByName("byteLength");
                                newBufferView.byteStride =
                                    bufferview.GetChildInt32ByName("byteStride");
                                newBufferView.target = bufferview.GetChildInt32ByName("target");

                                if (bufferIndex < 0 ||
                                    bufferIndex >= (const int)modelFile.Buffers.size()) {
                                    ALOGW("Error: Invalid buffer Index in gltfBufferView");
                                    loaded = false;
                                }
                                if (newBufferView.byteStride < 0 ||
                                    newBufferView.byteStride > 255) {
                                    ALOGW("Error: Invalid byeStride in gltfBufferView");
                                    loaded = false;
                                }
                                if (newBufferView.target < 0) {
                                    ALOGW("Error: Invalid target in gltfBufferView");
                                    loaded = false;
                                }

                                newBufferView.buffer = &modelFile.Buffers[bufferIndex];
                                modelFile.BufferViews.push_back(newBufferView);
                            }
                        }
                    }
                } // END BUFFERVIEWS

                if (loaded) { // IMAGES
                    LOGV("Loading image textures");
                    // gather all the images, and try to load them from the zip file.
                    const OVR::JsonReader images(models.GetChildByName("images"));
                    if (images.IsArray()) {
                        while (!images.IsEndOfArray()) {
                            const OVR::JsonReader image(images.GetNextArrayElement());
                            if (image.IsObject()) {
                                const std::string name = image.GetChildStringByName("name");
                                const std::string uri = image.GetChildStringByName("uri");
                                const std::string mimeType = image.GetChildStringByName("mimeType");
                                int bufferView = image.GetChildInt32ByName("bufferView", -1);
                                LOGV(
                                    "LoadModelFile_glB: %s, %s, %d",
                                    name.c_str(),
                                    uri.c_str(),
                                    bufferView);
                                if (bufferView >= 0 &&
                                    bufferView < static_cast<int>(modelFile.BufferViews.size())) {
                                    ModelBufferView* pBufferView =
                                        &modelFile.BufferViews[bufferView];
                                    int imageBufferLength = (int)pBufferView->byteLength;
                                    uint8_t* imageBuffer =
                                        (uint8_t*)pBufferView->buffer->bufferData.data() +
                                        pBufferView->byteOffset;

                                    std::string path = name;
                                    const char* ext = strrchr(mimeType.c_str(), '/');
                                    if (ext) {
                                        path += ".";
                                        path += ext + 1;
                                    }

                                    LoadModelFileTexture(
                                        modelFile,
                                        path.c_str(),
                                        (const char*)imageBuffer,
                                        imageBufferLength,
                                        materialParms);
                                } else if (
                                    materialParms.ImageUriHandler &&
                                    materialParms.ImageUriHandler(modelFile, uri)) {
                                    LOGV("LoadModelFile_glB: uri processed by custom handler");
                                } else {
                                    ALOGW(
                                        "Loading images from othen then bufferView currently unsupported in glBfd, defaulting image");
                                    // Create a default texture.
                                    LoadModelFileTexture(
                                        modelFile, "DefaultImage", nullptr, 0, materialParms);
                                }
                            }
                        }
                    }
                } // END images

                // End of section dependent on buffer data in the glB file.
            }
        }

        if (loaded) {
            loaded =
                LoadModelFile_glTF_Json(modelFile, gltfJson, programs, materialParms, outModelGeo);
        }
    }

    // delete fileData;

    if (!loaded) {
        ALOGW("Error: failed to load %s", fileName);
        delete modelFilePtr;
        modelFilePtr = nullptr;
    }

    return modelFilePtr;
}

} // namespace OVRFW
