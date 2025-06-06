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

Filename    :   BeamRenderer.cpp
Content     :   Class that manages and renders view-oriented beams.
Created     :   October 23; 2015
Authors     :   Jonathan E. Wright

*************************************************************************************/

#include "BeamRenderer.h"
#include "TextureAtlas.h"

#include "Misc/Log.h"

using OVR::Matrix4f;
using OVR::Posef;
using OVR::Quatf;
using OVR::Vector2f;
using OVR::Vector3f;
using OVR::Vector4f;

inline Vector3f GetViewMatrixPosition(Matrix4f const& m) {
    return m.Inverted().GetTranslation();
}

namespace OVRFW {

static const char* BeamVertexSrc = R"glsl(
attribute highp vec4 Position;
attribute lowp vec4 VertexColor;
attribute highp vec2 TexCoord;

varying lowp vec4 outColor;
varying highp vec2 oTexCoord;

void main()
{
	gl_Position = TransformVertex( Position );
	oTexCoord = TexCoord;
   	outColor = VertexColor;
}
)glsl";

static const char* TextureFragmentSrc = R"glsl(
uniform sampler2D Texture0;

varying lowp vec4 outColor;
varying highp vec2 oTexCoord;

void main()
{
	gl_FragColor = outColor * texture2D( Texture0, oTexCoord );
}
)glsl";

static const char* ParametricFragmentSrc = R"glsl(
precision highp float;

varying lowp vec4 outColor;
varying highp vec2 oTexCoord;

void main()
{
    vec2 v = (oTexCoord - vec2(0.5)) * vec2(2.0);

    // Fade toward the end of the beam
    float forwardFade = 1.0 - oTexCoord.y * oTexCoord.y;

    // Fade out from center of beam to the sides
    float sideFade = 1.0 - abs((oTexCoord.x - 0.5) * 2.0);

    float r = sideFade * forwardFade;
    gl_FragColor = outColor * vec4(r,r,r,r);
}
)glsl";

float ovrBeamRenderer::LIFETIME_INFINITE = FLT_MAX;

//==============================
// ovrBeamRenderer::ovrBeamRenderer
ovrBeamRenderer::ovrBeamRenderer() : MaxBeams(0) {}

//==============================
// ovrBeamRenderer::ovrBeamRenderer
ovrBeamRenderer::~ovrBeamRenderer() {
    Shutdown();
}

//==============================
// ovrBeamRenderer::Init
void ovrBeamRenderer::Init(const int maxBeams, const bool depthTest) {
    Shutdown();

    MaxBeams = maxBeams;

    if (TextureProgram.VertexShader == 0 || TextureProgram.FragmentShader == 0) {
        OVRFW::ovrProgramParm uniformParms[] = {
            /// Vertex
            /// Fragment
            {"Texture0", OVRFW::ovrProgramParmType::TEXTURE_SAMPLED},
        };
        const int uniformCount = sizeof(uniformParms) / sizeof(OVRFW::ovrProgramParm);
        TextureProgram =
            OVRFW::GlProgram::Build(BeamVertexSrc, TextureFragmentSrc, uniformParms, uniformCount);
    }
    if (ParametricProgram.VertexShader == 0 || ParametricProgram.FragmentShader == 0) {
        ParametricProgram =
            OVRFW::GlProgram::Build(BeamVertexSrc, ParametricFragmentSrc, nullptr, 0);
    }

    const int numVerts = maxBeams * 4;

    VertexAttribs attr;
    attr.position.resize(numVerts);
    attr.uv0.resize(numVerts);
    attr.color.resize(numVerts);

    // the indices will never change once we've set them up; we just won't necessarily
    // use all of the index buffer to render.
    std::vector<TriangleIndex> indices;
    indices.resize(MaxBeams * 6);

    for (int i = 0; i < MaxBeams; i++) {
        indices[i * 6 + 0] = static_cast<TriangleIndex>(i * 4 + 0);
        indices[i * 6 + 1] = static_cast<TriangleIndex>(i * 4 + 1);
        indices[i * 6 + 2] = static_cast<TriangleIndex>(i * 4 + 3);
        indices[i * 6 + 3] = static_cast<TriangleIndex>(i * 4 + 0);
        indices[i * 6 + 4] = static_cast<TriangleIndex>(i * 4 + 3);
        indices[i * 6 + 5] = static_cast<TriangleIndex>(i * 4 + 2);
    }

    Surf.surfaceName = "beams";
    Surf.geo.Create(attr, indices);
    Surf.geo.primitiveType = GlGeometry::kPrimitiveTypeTriangles;
    Surf.geo.indexCount = 0;

    ovrGraphicsCommand& gc = Surf.graphicsCommand;
    gc.GpuState.depthEnable = gc.GpuState.depthMaskEnable = depthTest;
    gc.GpuState.blendEnable = ovrGpuState::BLEND_ENABLE;
    gc.GpuState.blendSrc = ovrGpuState::kGL_SRC_ALPHA;
    gc.GpuState.blendDst = ovrGpuState::kGL_ONE;
    gc.Program = TextureProgram;
    gc.GpuState.lineWidth = 1.0f;
}

//==============================
// ovrBeamRenderer::Shutdown
void ovrBeamRenderer::Shutdown() {
    Surf.geo.Free();
    OVRFW::GlProgram::Free(TextureProgram);
    OVRFW::GlProgram::Free(ParametricProgram);

    MaxBeams = 0;
    FreeBeams.resize(0);
    ActiveBeams.resize(0);
    BeamInfos.resize(0);
}

//==============================
// ovrBeamRenderer::AddBeam
ovrBeamRenderer::handle_t ovrBeamRenderer::AddBeam(
    const OVRFW::ovrApplFrameIn& frame,
    const ovrTextureAtlas& atlas,
    const int atlasIndex,
    const float width,
    const Vector3f& startPos,
    const Vector3f& endPos,
    const Vector4f& initialColor,
    const float lifeTime) {
    handle_t handle;

    // ALOG( "ovrBeamRenderer::AddDebugLine" );
    if (FreeBeams.size() > 0) {
        handle = FreeBeams[static_cast<int>(FreeBeams.size()) - 1];
        FreeBeams.pop_back();
    } else {
        handle = handle_t(static_cast<uint16_t>(BeamInfos.size()));
        if (handle.Get() >= MaxBeams || handle.Get() >= MAX_BEAMS) {
            return handle_t();
        }
        BeamInfos.push_back(ovrBeamInfo());
    }

    assert(handle.IsValid());
    assert(handle.Get() < static_cast<int>(BeamInfos.size()));
    assert(handle.Get() < MAX_BEAMS);

    ActiveBeams.push_back(handle);

    UpdateBeamInternal(
        frame, handle, &atlas, atlasIndex, width, startPos, endPos, initialColor, lifeTime);

    return (lifeTime == LIFETIME_INFINITE) ? handle : handle_t();
}

//==============================
// ovrBeamRenderer::AddBeam
ovrBeamRenderer::handle_t ovrBeamRenderer::AddBeam(
    const OVRFW::ovrApplFrameIn& frame,
    const float width,
    const Vector3f& startPos,
    const Vector3f& endPos,
    const OVR::Vector4f& initialColor) {
    handle_t handle;

    // ALOG( "ovrBeamRenderer::AddDebugLine" );
    if (FreeBeams.size() > 0) {
        handle = FreeBeams[static_cast<int>(FreeBeams.size()) - 1];
        FreeBeams.pop_back();
    } else {
        handle = handle_t(static_cast<uint16_t>(BeamInfos.size()));
        if (handle.Get() >= MaxBeams || handle.Get() >= MAX_BEAMS) {
            return handle_t();
        }
        BeamInfos.push_back(ovrBeamInfo());
    }

    assert(handle.IsValid());
    assert(handle.Get() < static_cast<int>(BeamInfos.size()));
    assert(handle.Get() < MAX_BEAMS);
    ActiveBeams.push_back(handle);

    UpdateBeamInternal(
        frame, handle, nullptr, 0, width, startPos, endPos, initialColor, LIFETIME_INFINITE);

    return handle;
}

//==============================
// ovrBeamRenderer::UpdateBeam
void ovrBeamRenderer::UpdateBeam(
    const OVRFW::ovrApplFrameIn& frame,
    const handle_t handle,
    const ovrTextureAtlas& atlas,
    const int atlasIndex,
    const float width,
    const Vector3f& startPos,
    const Vector3f& endPos,
    const Vector4f& initialColor) {
    assert(BeamInfos[handle.Get()].Handle.IsValid());
    UpdateBeamInternal(
        frame,
        handle,
        &atlas,
        atlasIndex,
        width,
        startPos,
        endPos,
        initialColor,
        LIFETIME_INFINITE);
}

void ovrBeamRenderer::UpdateBeam(
    const OVRFW::ovrApplFrameIn& frame,
    const handle_t handle,
    const float width,
    const Vector3f& startPos,
    const Vector3f& endPos,
    const Vector4f& initialColor) {
    assert(BeamInfos[handle.Get()].Handle.IsValid());
    UpdateBeamInternal(
        frame, handle, nullptr, 0, width, startPos, endPos, initialColor, LIFETIME_INFINITE);
}

void ovrBeamRenderer::RemoveBeam(const handle_t handle) {
    if (!handle.IsValid() || handle.Get() >= BeamInfos.size()) {
        return;
    }
    BeamInfos[handle.Get()].StartTime = -1.0;
    BeamInfos[handle.Get()].LifeTime = -1.0f;
}

//==============================
// ovrBeamRenderer::UpdateBeamInternal
void ovrBeamRenderer::UpdateBeamInternal(
    const OVRFW::ovrApplFrameIn& frame,
    const handle_t handle,
    const ovrTextureAtlas* atlas,
    const int atlasIndex,
    const float width,
    const Vector3f& startPos,
    const Vector3f& endPos,
    const Vector4f& initialColor,
    float const lifeTime) {
    if (!handle.IsValid()) {
        assert(handle.IsValid());
        return;
    }

    ovrBeamInfo& beam = BeamInfos[handle.Get()];

    beam.Handle = handle;
    beam.StartTime = frame.PredictedDisplayTime;
    beam.LifeTime = lifeTime;
    beam.Width = width;
    beam.AtlasIndex = static_cast<uint16_t>(atlasIndex);
    beam.StartPos = startPos;
    beam.EndPos = endPos;
    beam.InitialColor = initialColor;
    if (atlas == nullptr) {
        beam.TexCoords[0] = {0.0f, 0.0f}; // min tex coords
        beam.TexCoords[1] = {1.0f, 1.0f}; // max tex coords
    } else {
        const ovrTextureAtlas::ovrSpriteDef& sd = atlas->GetSpriteDef(atlasIndex);
        beam.TexCoords[0] = sd.uvMins; // min tex coords
        beam.TexCoords[1] = sd.uvMaxs; // max tex coords
    }
}

//==============================
// ovrBeamRenderer::Frame
void ovrBeamRenderer::Frame(
    const OVRFW::ovrApplFrameIn& frame,
    const Matrix4f& centerViewMatrix,
    const ovrTextureAtlas& atlas) {
    FrameInternal(frame, centerViewMatrix, &atlas);
}
void ovrBeamRenderer::Frame(const OVRFW::ovrApplFrameIn& frame, const Matrix4f& centerViewMatrix) {
    FrameInternal(frame, centerViewMatrix, nullptr);
}

//==============================
// ovrBeamRenderer::Frame
void ovrBeamRenderer::FrameInternal(
    const OVRFW::ovrApplFrameIn& frame,
    const OVR::Matrix4f& centerViewMatrix,
    const class ovrTextureAtlas* atlas) {
    /// we
    if (atlas) {
        Surf.geo.indexCount = 0;
        Surf.graphicsCommand.Textures[0] = atlas->GetTexture();
        Surf.graphicsCommand.BindUniformTextures();
        Surf.graphicsCommand.Program = TextureProgram;
    } else {
        Surf.graphicsCommand.Program = ParametricProgram;
    }

    VertexAttribs attr;
    attr.position.resize(ActiveBeams.size() * 4);
    attr.color.resize(ActiveBeams.size() * 4);
    attr.uv0.resize(ActiveBeams.size() * 4);

    const Vector3f viewPos = GetViewMatrixPosition(centerViewMatrix);

    int quadIndex = 0;
    for (int i = 0; i < static_cast<int>(ActiveBeams.size()); ++i) {
        const handle_t beamHandle = ActiveBeams[i];
        if (!beamHandle.IsValid()) {
            continue;
        }

        const ovrBeamInfo& cur = BeamInfos[beamHandle.Get()];
        double const timeAlive = frame.PredictedDisplayTime - cur.StartTime;
        if (timeAlive > cur.LifeTime) {
            BeamInfos[beamHandle.Get()].Handle = handle_t();
            FreeBeams.push_back(beamHandle);
            ActiveBeams[i] = ActiveBeams.back();
            ActiveBeams.pop_back();
            i--;
            continue;
        }

        // Vector describing length and direction of beam (but not position)
        const Vector3f beamVector = cur.EndPos - cur.StartPos;
        // Center of the beam in "world space". Start + half-way along the beam
        const Vector3f beamCenter = cur.StartPos + beamVector * 0.5f;
        const Vector3f beamDir = beamVector.Normalized();
        // Vector from centerView to the center of the beam
        const Vector3f viewToCenter = beamCenter - viewPos;
        // Cross product gives us an offset direction for the beam such that the flat side is facing
        // the viewer. Classic billboarding.
        const Vector3f cross = beamDir.Cross(viewToCenter).Normalized() * cur.Width * 0.5f;

        const float t = static_cast<float>(frame.PredictedDisplayTime - cur.StartTime);

        const Vector4f color = EaseFunctions[cur.EaseFunc](cur.InitialColor, t / cur.LifeTime);
        const Vector2f uvOfs(0.0f);

        attr.position[quadIndex * 4 + 0] = cur.StartPos + cross;
        attr.position[quadIndex * 4 + 1] = cur.StartPos - cross;
        attr.position[quadIndex * 4 + 2] = cur.EndPos + cross;
        attr.position[quadIndex * 4 + 3] = cur.EndPos - cross;
        attr.color[quadIndex * 4 + 0] = color;
        attr.color[quadIndex * 4 + 1] = color;
        attr.color[quadIndex * 4 + 2] = color;
        attr.color[quadIndex * 4 + 3] = color;
        attr.uv0[quadIndex * 4 + 0] = Vector2f(cur.TexCoords[0].x, cur.TexCoords[0].y) + uvOfs;
        attr.uv0[quadIndex * 4 + 1] = Vector2f(cur.TexCoords[1].x, cur.TexCoords[0].y) + uvOfs;
        attr.uv0[quadIndex * 4 + 2] = Vector2f(cur.TexCoords[0].x, cur.TexCoords[1].y) + uvOfs;
        attr.uv0[quadIndex * 4 + 3] = Vector2f(cur.TexCoords[1].x, cur.TexCoords[1].y) + uvOfs;

        quadIndex++;
    }

    // Surf.graphicsCommand.GpuState.polygonMode = GL_LINE;
    Surf.graphicsCommand.GpuState.cullEnable = false;
    Surf.geo.indexCount = quadIndex * 6;
    Surf.geo.Update(attr);
}

//==============================
// ovrBeamRenderer::RenderEyeView
void ovrBeamRenderer::RenderEyeView(
    const Matrix4f& /*viewMatrix*/,
    const Matrix4f& /*projMatrix*/,
    std::vector<ovrDrawSurface>& surfaceList) {
    if (Surf.geo.indexCount > 0) {
        surfaceList.push_back(ovrDrawSurface(ModelMatrix, &Surf));
    }
}

void ovrBeamRenderer::Render(std::vector<ovrDrawSurface>& surfaceList) {
    if (Surf.geo.indexCount > 0) {
        surfaceList.push_back(ovrDrawSurface(ModelMatrix, &Surf));
    }
}

//==============================
// ovrBeamRenderer::SetPose
void ovrBeamRenderer::SetPose(const Posef& pose) {
    ModelMatrix = Matrix4f(pose);
}

} // namespace OVRFW
