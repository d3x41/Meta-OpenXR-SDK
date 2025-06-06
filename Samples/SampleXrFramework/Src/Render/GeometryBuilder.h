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

Filename    :   GeometryBuilder.h
Content     :   OpenGL geometry setup.
Created     :   July 2020
Authors     :   Federico Schliemann

************************************************************************************/

#pragma once

#include <vector>
#include "OVR_Math.h"
#include "GlGeometry.h"

namespace OVRFW {

class GeometryBuilder {
   public:
    GeometryBuilder() = default;
    ~GeometryBuilder() = default;

    static constexpr int kInvalidIndex = -1;

    struct Node {
        Node(
            const OVRFW::GlGeometry::Descriptor& g,
            int parent,
            const OVR::Vector4f& c,
            const OVR::Matrix4f& t)
            : geometry(g), parentIndex(parent), color(c), transform(t) {}

        OVRFW::GlGeometry::Descriptor geometry;
        int parentIndex = -1;
        OVR::Vector4f color = OVR::Vector4f(0.5f, 0.5f, 0.5f, 1.0f);
        OVR::Matrix4f transform = OVR::Matrix4f::Identity();
    };

    int Add(
        const OVRFW::GlGeometry::Descriptor& geometry,
        int parentIndex = kInvalidIndex,
        const OVR::Vector4f& color = OVR::Vector4f(0.5f, 0.5f, 0.5f, 1.0f),
        const OVR::Matrix4f& transform = OVR::Matrix4f::Identity());

    OVRFW::GlGeometry::Descriptor ToGeometryDescriptor(
        const OVR::Matrix4f& rootTransform = OVR::Matrix4f::Identity()) const;

    OVRFW::GlGeometry ToGeometry(
        const OVR::Matrix4f& rootTransform = OVR::Matrix4f::Identity()) const {
        OVRFW::GlGeometry::Descriptor d = ToGeometryDescriptor(rootTransform);
        return OVRFW::GlGeometry(d.attribs, d.indices);
    }

    const std::vector<OVRFW::GeometryBuilder::Node>& Nodes() const {
        return nodes_;
    }

    void clear_nodes();

   private:
    std::vector<OVRFW::GeometryBuilder::Node> nodes_;
};

} // namespace OVRFW
