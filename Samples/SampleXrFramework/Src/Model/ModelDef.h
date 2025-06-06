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

Filename    :   ModelDef.h
Content     :   Model file definitions.
Created     :   December 2013
Authors     :   John Carmack, J.M.P. van Waveren

*************************************************************************************/

#pragma once

#include <functional>
#include <string>
#include <vector>
#include <cstdint>

#include "Render/GlProgram.h"
#include "Render/GlTexture.h"
#include "Render/SurfaceRender.h"
#include "ModelCollision.h"
#include "ModelTrace.h"

namespace OVRFW {

class ModelFile;
class ModelState;

struct MaterialParms {
    MaterialParms()
        : UseSrgbTextureFormats(false),
          EnableDiffuseAniso(false),
          EnableEmissiveLodClamp(true),
          Transparent(false),
          PolygonOffset(false) {}

    bool UseSrgbTextureFormats; // use sRGB textures
    bool EnableDiffuseAniso; // enable anisotropic filtering on the diffuse texture
    bool EnableEmissiveLodClamp; // enable LOD clamp on the emissive texture to avoid light bleeding
    bool Transparent; // surfaces with this material flag need to render in a transparent pass
    bool PolygonOffset; // render with polygon offset enabled
    std::function<bool(ModelFile&, const std::string&)> ImageUriHandler; // custom image URI handler
};

enum ModelJointAnimation {
    MODEL_JOINT_ANIMATION_NONE,
    MODEL_JOINT_ANIMATION_ROTATE,
    MODEL_JOINT_ANIMATION_SWAY,
    MODEL_JOINT_ANIMATION_BOB
};

struct ModelJoint {
    int index;
    std::string name;
    OVR::Matrix4f transform;
    ModelJointAnimation animation;
    OVR::Vector3f parameters;
    float timeOffset;
    float timeScale;
};

struct ModelTag {
    std::string name;
    OVR::Matrix4f matrix;
    OVR::Vector4i jointIndices;
    OVR::Vector4f jointWeights;
};

enum ModelComponentType {
    MODEL_COMPONENT_TYPE_BYTE = 0X1400, // GL_BYTE
    MODEL_COMPONENT_TYPE_UNSIGNED_BYTE = 0X1401, // GL_UNSIGNED_BYTE
    MODEL_COMPONENT_TYPE_SHORT = 0x1402, // GL_SHORT
    MODEL_COMPONENT_TYPE_UNSIGNED_SHORT = 0x1403, // GL_UNSIGNED_SHORT
    MODEL_COMPONENT_TYPE_UNSIGNED_INT = 0x1405, // GL_UNSIGNED_INT
    MODEL_COMPONENT_TYPE_FLOAT = 0x1406, // GL_FLOAT
};

struct ModelBuffer {
    ModelBuffer() : byteLength(0) {}

    std::string name;
    std::vector<uint8_t> bufferData;
    size_t byteLength;
    ModelComponentType componentType = MODEL_COMPONENT_TYPE_UNSIGNED_BYTE;
    int componentCount;
};

struct ModelBufferView {
    ModelBufferView() : buffer(nullptr), byteOffset(0), byteLength(0), byteStride(0), target(0) {}

    std::string name;
    const ModelBuffer* buffer;
    size_t byteOffset;
    size_t byteLength;
    int byteStride;
    int target;
};

enum ModelAccessorType {
    ACCESSOR_UNKNOWN,
    ACCESSOR_SCALAR,
    ACCESSOR_VEC2,
    ACCESSOR_VEC3,
    ACCESSOR_VEC4,
    ACCESSOR_MAT2,
    ACCESSOR_MAT3,
    ACCESSOR_MAT4
};

#define MAX_MODEL_ACCESSOR_COMPONENT_SIZE 16
class ModelAccessor {
   public:
    ModelAccessor()
        : bufferView(nullptr),
          byteOffset(0),
          componentType(0),
          count(0),
          type(ACCESSOR_UNKNOWN),
          minMaxSet(false),
          normalized(false) {
        memset(intMin, 0, sizeof(int) * MAX_MODEL_ACCESSOR_COMPONENT_SIZE);
        memset(intMax, 0, sizeof(int) * MAX_MODEL_ACCESSOR_COMPONENT_SIZE);
        memset(floatMin, 0, sizeof(float) * MAX_MODEL_ACCESSOR_COMPONENT_SIZE);
        memset(floatMax, 0, sizeof(float) * MAX_MODEL_ACCESSOR_COMPONENT_SIZE);
    }

    uint8_t* BufferData() const;

    std::string name;
    const ModelBufferView* bufferView;
    size_t byteOffset;
    int componentType;
    int count;
    ModelAccessorType type;
    bool minMaxSet;
    // Minimum and Maximum values for each element in accessor data if the component type is a byte
    // or int.
    int intMin[MAX_MODEL_ACCESSOR_COMPONENT_SIZE];
    int intMax[MAX_MODEL_ACCESSOR_COMPONENT_SIZE];
    // Minimum and Maximum values for each element in accessor data if the component type is a
    // float.
    float floatMin[MAX_MODEL_ACCESSOR_COMPONENT_SIZE];
    float floatMax[MAX_MODEL_ACCESSOR_COMPONENT_SIZE];
    bool normalized;
    // #TODO: implement Sparse Accessors
};

struct ModelTexture {
    std::string name;
    GlTexture texid; // texture id. will need to be freed when the object destroys itself.
};

struct ModelSampler {
    /// Aliasing some GL constant for defaults and commonly used behavior
    /// without needing the explicit GL include
    static constexpr uint32_t kGL_NEAREST = 0x2600;
    static constexpr uint32_t kGL_LINEAR = 0x2601;
    static constexpr uint32_t kGL_NEAREST_MIPMAP_NEAREST = 0x2700;
    static constexpr uint32_t kGL_LINEAR_MIPMAP_NEAREST = 0x2701;
    static constexpr uint32_t kGL_NEAREST_MIPMAP_LINEAR = 0x2702;
    static constexpr uint32_t kGL_LINEAR_MIPMAP_LINEAR = 0x2703;
    static constexpr uint32_t kGL_CLAMP = 0x2900;
    static constexpr uint32_t kGL_REPEAT = 0x2901;
    static constexpr uint32_t kGL_CLAMP_TO_EDGE = 0x812F;
    static constexpr uint32_t kGL_MIRRORED_REPEAT = 0x8370;

    ModelSampler()
        : magFilter(kGL_LINEAR),
          minFilter(kGL_NEAREST_MIPMAP_LINEAR),
          wrapS(kGL_REPEAT),
          wrapT(kGL_REPEAT) {}

    std::string name;
    int magFilter;
    int minFilter;
    int wrapS;
    int wrapT;
};

struct ModelTextureWrapper {
    ModelTextureWrapper() : image(nullptr), sampler(nullptr) {}

    std::string name;
    const ModelTexture* image;
    const ModelSampler* sampler;
};

enum ModelAlphaMode { ALPHA_MODE_OPAQUE, ALPHA_MODE_MASK, ALPHA_MODE_BLEND };

struct ModelMaterial {
    ModelMaterial()
        : baseColorTextureWrapper(nullptr),
          metallicRoughnessTextureWrapper(nullptr),
          normalTextureWrapper(nullptr),
          occlusionTextureWrapper(nullptr),
          emissiveTextureWrapper(nullptr),
          detailTextureWrapper(nullptr),
          baseColorFactor(1.0f, 1.0f, 1.0f, 1.0f),
          emmisiveFactor(0.0f, 0.0f, 0.0f),
          metallicFactor(1.0f),
          roughnessFactor(1.0f),
          alphaCutoff(0.5f),
          alphaMode(ALPHA_MODE_OPAQUE),
          normalTexCoord(0),
          normalScale(1.0f),
          occlusionTexCoord(0),
          occlusionStrength(1.0f),
          doubleSided(false) {}

    std::string name;
    const ModelTextureWrapper* baseColorTextureWrapper;
    const ModelTextureWrapper* metallicRoughnessTextureWrapper;
    const ModelTextureWrapper* normalTextureWrapper;
    const ModelTextureWrapper* occlusionTextureWrapper;
    const ModelTextureWrapper* emissiveTextureWrapper;
    const ModelTextureWrapper* detailTextureWrapper;
    OVR::Vector4f baseColorFactor;
    OVR::Vector3f emmisiveFactor;
    float metallicFactor;
    float roughnessFactor;
    float alphaCutoff;
    ModelAlphaMode alphaMode;
    int normalTexCoord;
    float normalScale;
    int occlusionTexCoord;
    float occlusionStrength;
    bool doubleSided;
};

struct ModelSurface {
    ModelSurface() : material(nullptr) {}

    const ModelMaterial* material; // material used to render this surface
    ovrSurfaceDef surfaceDef;
    VertexAttribs attribs; // Only populated if morph targets are used
    std::vector<VertexAttribs> targets;
};

struct Model {
    std::string name;
    std::vector<ModelSurface> surfaces;
    std::vector<float> weights;
};

typedef enum { MODEL_CAMERA_TYPE_PERSPECTIVE, MODEL_CAMERA_TYPE_ORTHOGRAPHIC } ModelCameraType;

struct ModelPerspectiveCameraData {
    ModelPerspectiveCameraData()
        : aspectRatio(0.0f), fovDegreesX(0.0f), fovDegreesY(0.0f), nearZ(0.0f), farZ(0.0f) {}

    float aspectRatio;
    float fovDegreesX;
    float fovDegreesY;
    float nearZ;
    float farZ;
};

struct ModelOthographicCameraData {
    ModelOthographicCameraData() : magX(0.0f), magY(0.0f), nearZ(0.0f), farZ(0.0f) {}

    float magX;
    float magY;
    float nearZ;
    float farZ;
};

struct ModelCamera {
    ModelCamera() : type(MODEL_CAMERA_TYPE_PERSPECTIVE) {}

    std::string name;
    ModelCameraType type;
    ModelPerspectiveCameraData perspective;
    ModelOthographicCameraData orthographic;
};

class ModelNode {
   public:
    ModelNode()
        : rotation(0.0f, 0.0f, 0.0f, 1.0f),
          translation(0.0f, 0.0f, 0.0f),
          scale(1.0f, 1.0f, 1.0f),
          parentIndex(-1),
          skinIndex(-1),
          camera(nullptr),
          model(nullptr),
          localTransform(OVR::Matrix4f::Identity()),
          globalTransform(OVR::Matrix4f::Identity()) {}

    OVR::Matrix4f GetLocalTransform() const {
        return localTransform;
    }
    OVR::Matrix4f GetGlobalTransform() const {
        return globalTransform;
    }
    void SetLocalTransform(const OVR::Matrix4f matrix);
    void RecalculateGlobalTransform(ModelFile& modelFile);

    std::string name;
    std::string jointName;
    OVR::Quatf rotation;
    OVR::Vector3f translation;
    OVR::Vector3f scale;
    std::vector<float> weights;

    std::vector<int> children;
    int parentIndex;
    int skinIndex;
    const ModelCamera* camera;
    Model* model;

    // old ovrscene animation system
    std::vector<ModelJoint> JointsOvrScene;

   private:
    OVR::Matrix4f localTransform;
    OVR::Matrix4f globalTransform;
};

typedef enum {
    MODEL_ANIMATION_INTERPOLATION_LINEAR,
    MODEL_ANIMATION_INTERPOLATION_STEP,
    MODEL_ANIMATION_INTERPOLATION_CATMULLROMSPLINE,
    MODEL_ANIMATION_INTERPOLATION_CUBICSPLINE
} ModelAnimationInterpolation;

struct ModelAnimationSampler {
    ModelAnimationSampler()
        : input(nullptr),
          output(nullptr),
          timeLineIndex(-1),
          interpolation(MODEL_ANIMATION_INTERPOLATION_LINEAR) {}

    const ModelAccessor* input;
    const ModelAccessor* output;
    int timeLineIndex;
    ModelAnimationInterpolation interpolation;
};

typedef enum {
    MODEL_ANIMATION_PATH_UNKNOWN,
    MODEL_ANIMATION_PATH_TRANSLATION,
    MODEL_ANIMATION_PATH_ROTATION,
    MODEL_ANIMATION_PATH_SCALE,
    MODEL_ANIMATION_PATH_WEIGHTS
} ModelAnimationPath;

struct ModelAnimationChannel {
    ModelAnimationChannel()
        : nodeIndex(-1),
          additiveWeightIndex(-1),
          sampler(nullptr),
          path(MODEL_ANIMATION_PATH_UNKNOWN) {}

    int nodeIndex;
    int additiveWeightIndex;
    const ModelAnimationSampler* sampler;
    ModelAnimationPath path;
};

class ModelAnimationTimeLine {
   public:
    ModelAnimationTimeLine()
        : accessor(nullptr),
          startTime(0.0f),
          endTime(0.0f),
          rcpStep(0.0f),
          sampleTimes(nullptr),
          sampleCount(0) {}

    void Initialize(const ModelAccessor* _accessor);

    const ModelAccessor* accessor;
    float startTime; // in seconds;
    float endTime; // in seconds
    float rcpStep; // in seconds
    const float* sampleTimes; // in seconds
    int sampleCount;
};

struct ModelAnimation {
    ModelAnimation() {}

    std::string name;
    std::vector<ModelAnimationSampler> samplers;
    std::vector<ModelAnimationChannel> channels;
};

struct ModelSkin {
    ModelSkin() : inverseBindMatricesAccessor(nullptr) {}

    std::string name;
    int skeletonRootIndex;
    std::vector<int> jointIndexes;
    const ModelAccessor* inverseBindMatricesAccessor;
    std::vector<OVR::Matrix4f> inverseBindMatrices;
};

struct ModelSubScene {
    ModelSubScene() : visible(false) {}

    std::string name;
    std::vector<int> nodes;
    bool visible;
};

class ModelNodeState {
   public:
    ModelNodeState()
        : node(nullptr),
          state(nullptr),
          rotation(0.0f, 0.0f, 0.0f, 1.0f),
          translation(0.0f, 0.0f, 0.0f),
          scale(1.0f, 1.0f, 1.0f),
          localTransform(OVR::Matrix4f::Identity()),
          globalTransform(OVR::Matrix4f::Identity()) {}

    void GenerateStateFromNode(const ModelNode* _node, ModelState* _modelState);
    void CalculateLocalTransform();
    void SetLocalTransform(const OVR::Matrix4f matrix);
    OVR::Matrix4f GetLocalTransform() const {
        return localTransform;
    }
    OVR::Matrix4f GetGlobalTransform() const {
        return globalTransform;
    }
    void RecalculateMatrix();
    const ModelNode* GetNode() const {
        return node;
    }

    void AddNodesToEmitList(std::vector<ModelNodeState*>& emitList);

    const ModelNode* node;
    ModelState* state;
    OVR::Quatf rotation;
    OVR::Vector3f translation;
    OVR::Vector3f scale;
    std::vector<float> weights;

   private:
    OVR::Matrix4f localTransform;
    OVR::Matrix4f globalTransform;
};

enum ModelAnimationTimeType {
    MODEL_ANIMATION_TIME_TYPE_ONCE_FORWARD,
    MODEL_ANIMATION_TIME_TYPE_LOOP_FORWARD,
    MODEL_ANIMATION_TIME_TYPE_LOOP_FORWARD_AND_BACK,
};

class ModelAnimationTimeLineState {
   public:
    ModelAnimationTimeLineState() : frame(0), fraction(0.0f), timeline(nullptr) {}

    void CalculateFrameAndFraction(float timeInSeconds);

    int frame;
    float fraction;
    const ModelAnimationTimeLine* timeline;
};

class ModelSubSceneState {
   public:
    ModelSubSceneState() : visible(false), subScene(nullptr) {}

    void GenerateStateFromSubScene(const ModelSubScene* _subScene);
    bool visible;
    std::vector<int> nodeStates;

   private:
    const ModelSubScene* subScene;
};

class ModelState {
   public:
    ModelState() : DontRenderForClientUid(0), mf(nullptr) {
        modelMatrix.Identity();
    }

    void GenerateStateFromModelFile(const ModelFile* _mf);
    void SetMatrix(const OVR::Matrix4f matrix);
    OVR::Matrix4f GetMatrix() const {
        return modelMatrix;
    }

    void CalculateAnimationFrameAndFraction(const ModelAnimationTimeType type, float timeInSeconds);

    long long DontRenderForClientUid; // skip rendering the model if the current scene's client uid
                                      // matches this
    std::vector<ModelNodeState> nodeStates;
    std::vector<ModelAnimationTimeLineState> animationTimelineStates;
    std::vector<ModelSubSceneState> subSceneStates;

    const ModelFile* mf;

   private:
    OVR::Matrix4f modelMatrix;
};

struct ModelGlPrograms {
    ModelGlPrograms()
        : ProgVertexColor(nullptr),
          ProgSingleTexture(nullptr),
          ProgLightMapped(nullptr),
          ProgReflectionMapped(nullptr),
          ProgSimplePBR(nullptr),
          ProgBaseColorPBR(nullptr),
          ProgBaseColorEmissivePBR(nullptr),
          ProgSkinnedVertexColor(nullptr),
          ProgSkinnedSingleTexture(nullptr),
          ProgSkinnedLightMapped(nullptr),
          ProgSkinnedReflectionMapped(nullptr),
          ProgSkinnedSimplePBR(nullptr),
          ProgSkinnedBaseColorPBR(nullptr),
          ProgSkinnedBaseColorEmissivePBR(nullptr) {}
    ModelGlPrograms(const GlProgram* singleTexture)
        : ProgVertexColor(singleTexture),
          ProgSingleTexture(singleTexture),
          ProgLightMapped(singleTexture),
          ProgReflectionMapped(singleTexture),
          ProgSimplePBR(singleTexture),
          ProgBaseColorPBR(singleTexture),
          ProgBaseColorEmissivePBR(singleTexture),
          ProgSkinnedVertexColor(singleTexture),
          ProgSkinnedSingleTexture(singleTexture),
          ProgSkinnedLightMapped(singleTexture),
          ProgSkinnedReflectionMapped(singleTexture),
          ProgSkinnedSimplePBR(singleTexture),
          ProgSkinnedBaseColorPBR(singleTexture),
          ProgSkinnedBaseColorEmissivePBR(singleTexture) {}
    ModelGlPrograms(const GlProgram* singleTexture, const GlProgram* dualTexture)
        : ProgVertexColor(singleTexture),
          ProgSingleTexture(singleTexture),
          ProgLightMapped(dualTexture),
          ProgReflectionMapped(dualTexture),
          ProgSimplePBR(singleTexture),
          ProgBaseColorPBR(singleTexture),
          ProgBaseColorEmissivePBR(dualTexture),
          ProgSkinnedVertexColor(singleTexture),
          ProgSkinnedSingleTexture(singleTexture),
          ProgSkinnedLightMapped(dualTexture),
          ProgSkinnedReflectionMapped(dualTexture),
          ProgSkinnedSimplePBR(singleTexture),
          ProgSkinnedBaseColorPBR(singleTexture),
          ProgSkinnedBaseColorEmissivePBR(dualTexture) {}

    const GlProgram* ProgVertexColor;
    const GlProgram* ProgSingleTexture;
    const GlProgram* ProgLightMapped;
    const GlProgram* ProgReflectionMapped;
    const GlProgram* ProgSimplePBR;
    const GlProgram* ProgBaseColorPBR;
    const GlProgram* ProgBaseColorEmissivePBR;
    const GlProgram* ProgSkinnedVertexColor;
    const GlProgram* ProgSkinnedSingleTexture;
    const GlProgram* ProgSkinnedLightMapped;
    const GlProgram* ProgSkinnedReflectionMapped;
    const GlProgram* ProgSkinnedSimplePBR;
    const GlProgram* ProgSkinnedBaseColorPBR;
    const GlProgram* ProgSkinnedBaseColorEmissivePBR;
};

struct ModelGeo {
    std::vector<OVR::Vector3f> positions;
    std::vector<TriangleIndex> indices;
};

} // namespace OVRFW
