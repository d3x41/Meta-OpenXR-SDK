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
/*******************************************************************************

Filename    :   GeometryRenderer.cpp
Content     :   Simple rendering for geometry-based types
Created     :   Mar 2021
Authors     :   Federico Schliemann
Language    :   C++

*******************************************************************************/

#include "GeometryRenderer.h"
#include "Misc/Log.h"

using OVR::Matrix4f;
using OVR::Posef;
using OVR::Quatf;
using OVR::Vector2f;
using OVR::Vector3f;
using OVR::Vector4f;

namespace OVRFW {

const char* GeometryVertexShaderSrc = R"glsl(
    attribute highp vec4 Position;
    attribute highp vec3 Normal;
#ifdef HAS_VERTEX_COLORS
    attribute lowp vec4 VertexColor;
    varying lowp vec4 oColor;
#endif /// HAS_VERTEX_COLORS
    varying lowp vec3 oEye;
    varying lowp vec3 oNormal;

    vec3 multiply( mat4 m, vec3 v )
    {
        return vec3(
            m[0].x * v.x + m[1].x * v.y + m[2].x * v.z,
            m[0].y * v.x + m[1].y * v.y + m[2].y * v.z,
            m[0].z * v.x + m[1].z * v.y + m[2].z * v.z );
    }
    vec3 transposeMultiply( mat4 m, vec3 v )
    {
        return vec3(
            m[0].x * v.x + m[0].y * v.y + m[0].z * v.z,
            m[1].x * v.x + m[1].y * v.y + m[1].z * v.z,
            m[2].x * v.x + m[2].y * v.y + m[2].z * v.z );
    }

    void main()
    {
        gl_Position = TransformVertex( Position );

#ifdef HAS_VERTEX_COLORS
        oColor = VertexColor;
#endif /// HAS_VERTEX_COLORS
        lowp vec3 eye = transposeMultiply( sm.ViewMatrix[VIEW_ID], -vec3( sm.ViewMatrix[VIEW_ID][3] ) );
        oEye = eye - vec3( ModelMatrix * Position );
        // This matrix math should ideally not be done in the shader for perf reasons:
        oNormal = multiply( transpose(inverse(ModelMatrix)), Normal );
    }
)glsl";

static const char* GeometryFragmentShaderSrc = R"glsl(
    precision lowp float;

    uniform lowp vec4 ChannelControl;
    uniform lowp vec4 DiffuseColor;
    uniform lowp vec3 SpecularLightDirection;
    uniform lowp vec3 SpecularLightColor;
    uniform lowp vec3 AmbientLightColor;

#ifdef HAS_VERTEX_COLORS
    varying lowp vec4 oColor;
#endif /// HAS_VERTEX_COLORS
    varying lowp vec3 oEye;
    varying lowp vec3 oNormal;

    lowp float pow16( float x )
    {
        float x2 = x * x;
        float x4 = x2 * x2;
        float x8 = x4 * x4;
        float x16 = x8 * x8;
        return x16;
    }

    void main()
    {
        lowp vec3 eyeDir = normalize( oEye.xyz );
        lowp vec3 Normal = normalize( oNormal );

        lowp vec4 diffuse = DiffuseColor;
#ifdef HAS_VERTEX_COLORS
        diffuse = oColor;
#endif /// HAS_VERTEX_COLORS
        lowp vec3 ambientValue = diffuse.xyz * AmbientLightColor;

        lowp float nDotL = max( dot( Normal, SpecularLightDirection ), 0.0 );
        lowp vec3 diffuseValue = diffuse.xyz * nDotL;

        lowp vec3 reflectDir = reflect( -SpecularLightDirection, Normal );
        lowp float specular = pow16(max(dot(eyeDir, reflectDir), 0.0));
        lowp float specularStrength = 1.0;
        lowp vec3 specularValue = specular * specularStrength * SpecularLightColor;

        lowp vec3 color = diffuseValue * ChannelControl.x
                        + ambientValue * ChannelControl.y
                        + specularValue * ChannelControl.z
                        ;
        gl_FragColor.xyz = color;
        gl_FragColor.w = diffuse.w * ChannelControl.w;
    }
)glsl";

void GeometryRenderer::Init(const GlGeometry::Descriptor& d) {
    /// Program
    static ovrProgramParm GeometryUniformParms[] = {
        {"ChannelControl", ovrProgramParmType::FLOAT_VECTOR4},
        {"DiffuseColor", ovrProgramParmType::FLOAT_VECTOR4},
        {"SpecularLightDirection", ovrProgramParmType::FLOAT_VECTOR3},
        {"SpecularLightColor", ovrProgramParmType::FLOAT_VECTOR3},
        {"AmbientLightColor", ovrProgramParmType::FLOAT_VECTOR3},
    };

    std::string programDefs = "";

    /// Do we support vertex color in the goemetyr
    const bool hasVertexColors = (d.attribs.color.size() > 0);
    if (hasVertexColors) {
        programDefs += "#define HAS_VERTEX_COLORS 1\n";
    }
    const bool hasMultipleParts = (d.attribs.jointIndices.size() > 0);
    if (hasMultipleParts) {
        programDefs += "#define HAS_MULTIPLE_PARTS 1\n";
    }

    Program_ = GlProgram::Build(
        programDefs.c_str(),
        GeometryVertexShaderSrc,
        programDefs.c_str(),
        GeometryFragmentShaderSrc,
        GeometryUniformParms,
        sizeof(GeometryUniformParms) / sizeof(ovrProgramParm));

    SurfaceDef_.geo = GlGeometry(d.attribs, d.indices);

    /// Hook the graphics command
    ovrGraphicsCommand& gc = SurfaceDef_.graphicsCommand;
    gc.Program = Program_;
    gc.UniformData[0].Data = &ChannelControl;
    gc.UniformData[1].Data = &DiffuseColor;
    gc.UniformData[2].Data = &SpecularLightDirection;
    gc.UniformData[3].Data = &SpecularLightColor;
    gc.UniformData[4].Data = &AmbientLightColor;

    /// gpu state needs alpha blending
    gc.GpuState.depthEnable = gc.GpuState.depthMaskEnable = true;
    gc.GpuState.blendEnable = ovrGpuState::BLEND_ENABLE;
}

void GeometryRenderer::Shutdown() {
    OVRFW::GlProgram::Free(Program_);
    SurfaceDef_.geo.Free();
}

void GeometryRenderer::Update() {
    ModelPose_.Rotation.Normalize();
    ModelMatrix_ = OVR::Matrix4f(ModelPose_) * OVR::Matrix4f::Scaling(ModelScale_);
}

void GeometryRenderer::UpdateGeometry(const GlGeometry::Descriptor& d) {
    SurfaceDef_.geo.Update(d.attribs);
}

void GeometryRenderer::Render(std::vector<ovrDrawSurface>& surfaceList) {
    ovrGraphicsCommand& gc = SurfaceDef_.graphicsCommand;
    gc.GpuState.blendMode = BlendMode;
    gc.GpuState.blendSrc = BlendSrc;
    gc.GpuState.blendDst = BlendDst;
    surfaceList.push_back(ovrDrawSurface(ModelMatrix_, &SurfaceDef_));
}

} // namespace OVRFW
