/***************************************************************************
 # Copyright (c) 2020-2023, NVIDIA CORPORATION.  All rights reserved.
 #
 # NVIDIA CORPORATION and its licensors retain all intellectual property
 # and proprietary rights in and to this software, related documentation
 # and any modifications thereto.  Any use, reproduction, disclosure or
 # distribution of this software and related documentation without an express
 # license agreement from NVIDIA CORPORATION is strictly prohibited.
 **************************************************************************/

#include "RenderTargets.h"

#include <donut/engine/FramebufferFactory.h>
#include <GL/glew.h>

#include "../../../External/donut/nvrhi/src/vulkan/vulkan-backend.h"
#include "GLFW/glfw3.h"

using namespace dm;
using namespace donut;

#include "../shaders/ShaderParameters.h"

RenderTargets::RenderTargets(nvrhi::IDevice* device, int2 size)
    : Size(size)
{
    nvrhi::TextureDesc desc;
    desc.width = size.x;
    desc.height = size.y;
    desc.keepInitialState = true;

    // Render targets

    desc.isUAV = false;
    desc.isRenderTarget = true;
    desc.initialState = nvrhi::ResourceStates::RenderTarget;

    desc.format = nvrhi::Format::SRGBA8_UNORM;
    desc.debugName = "LdrColor";
    LdrColor = device->createTexture(desc);

    LdrFramebuffer = std::make_shared<engine::FramebufferFactory>(device);
    LdrFramebuffer->RenderTargets = { LdrColor };
    
    desc.format = nvrhi::Format::D32;
    desc.debugName = "DeviceDepth";
    desc.initialState = nvrhi::ResourceStates::DepthWrite;
    desc.clearValue = 0.f;
    desc.useClearValue = true;
    DeviceDepth = device->createTexture(desc);

    // G-buffer targets

    desc.isUAV = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;

    desc.format = nvrhi::Format::R32_FLOAT;
    desc.sharedResourceFlags = nvrhi::SharedResourceFlags::Shared;
    desc.clearValue = BACKGROUND_DEPTH;
    desc.debugName = "DepthBuffer";
    Depth = static_cast<nvrhi::vulkan::Device*>(device)->createTextureForOpenGL(desc);
    desc.debugName = "PrevDepthBuffer";
    PrevDepth = static_cast<nvrhi::vulkan::Device*>(device)->createTextureForOpenGL(desc);
    
    glfwInit();

    // 🔴 REQUIRED — tell GLFW to create an OpenGL context
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);

    // Core profile required for EXT_memory_object
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    // (optional but recommended)
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

    GLFWwindow* window =
        glfwCreateWindow(800, 600, "GL interop", nullptr, nullptr);

    if (!window) {
        fprintf(stderr, "Failed to create GLFW window\n");
        abort();
    }

    // 🔴 REQUIRED
    glfwMakeContextCurrent(window);
    
    glewInit();
    
    fprintf(stderr, "GL version: %s\n", glGetString(GL_VERSION));
    fprintf(stderr, "GL profile: %s\n", glGetString(GL_CONTEXT_PROFILE_MASK));
    
    int depth = static_cast<nvrhi::vulkan::Device*>(device)->importTextureToOpenGL(Depth);

    desc.useClearValue = false;
    desc.clearValue = 0.f;

    desc.format = nvrhi::Format::R32_UINT;
    desc.debugName = "GBufferDiffuseAlbedo";
    GBufferDiffuseAlbedo = dynamic_cast<nvrhi::vulkan::Device*>(device)->createTextureForOpenGL(desc);
    desc.debugName = "PrevGBufferDiffuseAlbedo";
    PrevGBufferDiffuseAlbedo = dynamic_cast<nvrhi::vulkan::Device*>(device)->createTextureForOpenGL(desc);

    desc.format = nvrhi::Format::R32_UINT;
    desc.debugName = "GBufferSpecularRough";
    GBufferSpecularRough = dynamic_cast<nvrhi::vulkan::Device*>(device)->createTextureForOpenGL(desc);
    desc.debugName = "PrevGBufferSpecularRough";
    PrevGBufferSpecularRough = dynamic_cast<nvrhi::vulkan::Device*>(device)->createTextureForOpenGL(desc);

    desc.format = nvrhi::Format::R32_UINT;
    desc.debugName = "GBufferNormals";
    GBufferNormals = dynamic_cast<nvrhi::vulkan::Device*>(device)->createTextureForOpenGL(desc);
    desc.debugName = "PrevGBufferNormals";
    PrevGBufferNormals = dynamic_cast<nvrhi::vulkan::Device*>(device)->createTextureForOpenGL(desc);
    
    desc.format = nvrhi::Format::R32_UINT;
    desc.debugName = "GBufferGeoNormals";
    GBufferGeoNormals = dynamic_cast<nvrhi::vulkan::Device*>(device)->createTextureForOpenGL(desc);
    desc.debugName = "PrevGBufferGeoNormals";
    PrevGBufferGeoNormals = dynamic_cast<nvrhi::vulkan::Device*>(device)->createTextureForOpenGL(desc);

    desc.format = nvrhi::Format::RGBA8_UNORM;
    desc.sharedResourceFlags = nvrhi::SharedResourceFlags::None;
    desc.debugName = "NormalRoughness";
    NormalRoughness = device->createTexture(desc);

    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.sharedResourceFlags = nvrhi::SharedResourceFlags::Shared;
    desc.debugName = "GBufferEmissive";
    GBufferEmissive = dynamic_cast<nvrhi::vulkan::Device*>(device)->createTextureForOpenGL(desc);

    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.debugName = "MotionVectors";
    MotionVectors = dynamic_cast<nvrhi::vulkan::Device*>(device)->createTextureForOpenGL(desc);

    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.sharedResourceFlags = nvrhi::SharedResourceFlags::None;
    desc.debugName = "ResolvedColor";
    ResolvedColor = device->createTexture(desc);

    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.debugName = "ReferenceColor";
    ReferenceColor = device->createTexture(desc);

    GBufferFramebuffer = std::make_shared<engine::FramebufferFactory>(device);
    GBufferFramebuffer->DepthTarget = DeviceDepth;
    GBufferFramebuffer->RenderTargets = {
        Depth,
        GBufferDiffuseAlbedo,
        GBufferSpecularRough,
        GBufferNormals,
        GBufferGeoNormals,
        GBufferEmissive,
        MotionVectors
    };

    PrevGBufferFramebuffer = std::make_shared<engine::FramebufferFactory>(device);
    PrevGBufferFramebuffer->DepthTarget = DeviceDepth;
    PrevGBufferFramebuffer->RenderTargets = {
        PrevDepth,
        PrevGBufferDiffuseAlbedo,
        PrevGBufferSpecularRough,
        PrevGBufferNormals,
        PrevGBufferGeoNormals,
        GBufferEmissive,
        MotionVectors
    };

    ResolvedFramebuffer = std::make_shared<engine::FramebufferFactory>(device);
    ResolvedFramebuffer->RenderTargets = { ResolvedColor };

    // UAV-only textures

    desc.isRenderTarget = false;

    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.debugName = "DiffuseLighting";
    DiffuseLighting = device->createTexture(desc);

    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.debugName = "SpecularLighting";
    SpecularLighting = device->createTexture(desc);

    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.debugName = "HdrColor";
    HdrColor = device->createTexture(desc);

    desc.format = nvrhi::Format::RGBA32_FLOAT;
    desc.debugName = "AccumulatedColor";
    AccumulatedColor = device->createTexture(desc);

    desc.format = nvrhi::Format::RG16_FLOAT;
    desc.debugName = "RestirLuminance";
    RestirLuminance = device->createTexture(desc);
    desc.debugName = "PrevRestirLuminance";
    PrevRestirLuminance = device->createTexture(desc);

    desc.format = nvrhi::Format::R8_UNORM;
    desc.debugName = "DiffuseConfidence";
    DiffuseConfidence = device->createTexture(desc);
    desc.debugName = "PrevDiffuseConfidence";
    PrevDiffuseConfidence = device->createTexture(desc);
    desc.debugName = "SpecularConfidence";
    SpecularConfidence = device->createTexture(desc);
    desc.debugName = "PrevSpecularConfidence";
    PrevSpecularConfidence = device->createTexture(desc);

    desc.format = nvrhi::Format::RG16_SINT;
    desc.debugName = "TemporalSamplePositions";
    TemporalSamplePositions = device->createTexture(desc);

    desc.dimension = nvrhi::TextureDimension::Texture2DArray;
    desc.arraySize = 2;
    desc.width = (size.x + RTXDI_GRAD_FACTOR - 1) / RTXDI_GRAD_FACTOR;
    desc.height = (size.y + RTXDI_GRAD_FACTOR - 1) / RTXDI_GRAD_FACTOR;
    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.debugName = "Gradients";
    Gradients = device->createTexture(desc);

    nvrhi::TextureDesc debugDesc;
    debugDesc.width = size.x;
    debugDesc.height = size.y;
    debugDesc.keepInitialState = false;
    debugDesc.isUAV = true;
    debugDesc.isRenderTarget = false;
    debugDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    debugDesc.format = nvrhi::Format::RGBA16_FLOAT;
    debugDesc.debugName = "DebugColor";
    DebugColor = device->createTexture(debugDesc);
}

bool RenderTargets::IsUpdateRequired(int2 size)
{
    if (any(Size != size))
        return true;

    return false;
}

void RenderTargets::NextFrame()
{
    std::swap(Depth, PrevDepth);
    std::swap(GBufferDiffuseAlbedo, PrevGBufferDiffuseAlbedo);
    std::swap(GBufferSpecularRough, PrevGBufferSpecularRough);
    std::swap(GBufferNormals, PrevGBufferNormals);
    std::swap(GBufferGeoNormals, PrevGBufferGeoNormals);
    std::swap(GBufferFramebuffer, PrevGBufferFramebuffer);
    std::swap(DiffuseConfidence, PrevDiffuseConfidence);
    std::swap(SpecularConfidence, PrevSpecularConfidence);
}
