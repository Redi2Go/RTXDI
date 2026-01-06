/***************************************************************************
 # Copyright (c) 2020-2023, NVIDIA CORPORATION.  All rights reserved.
 #
 # NVIDIA CORPORATION and its licensors retain all intellectual property
 # and proprietary rights in and to this software, related documentation
 # and any modifications thereto.  Any use, reproduction, disclosure or
 # distribution of this software and related documentation without an express
 # license agreement from NVIDIA CORPORATION is strictly prohibited.
 **************************************************************************/

#pragma once

#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>
#include <memory>

namespace donut::engine
{
    class Scene;
    class CommonRenderPasses;
    class IView;
    class ShaderFactory;
}

class RenderTargets;
class Profiler;
class SampleScene;

typedef int ibool;

struct GBufferSettings
{
    float roughnessOverride = 0.5f;
    float metalnessOverride = 0.5f;
    bool enableRoughnessOverride = false;
    bool enableMetalnessOverride = false;
    float normalMapScale = 1.f;
    ibool enableAlphaTestedGeometry = true;
    float textureLodBias = -1.f;

    bool enableMaterialReadback = false;
    dm::int2 materialReadbackPosition = 0;
};

class RasterizedGBufferPass
{
public:
    RasterizedGBufferPass(
        nvrhi::IDevice* device,
        std::shared_ptr<donut::engine::ShaderFactory> shaderFactory,
        std::shared_ptr<donut::engine::CommonRenderPasses> commonPasses,
        std::shared_ptr<donut::engine::Scene> scene,
        std::shared_ptr<Profiler> profiler,
        nvrhi::IBindingLayout* bindlessLayout);

    void CreatePipeline(const RenderTargets& renderTargets);

    void CreateBindingSet();

    void Render(
        nvrhi::ICommandList* commandList,
        const donut::engine::IView& view,
        const donut::engine::IView& viewPrev,
        const RenderTargets& renderTargets,
        const GBufferSettings& settings);

private:
    nvrhi::DeviceHandle m_device;

    nvrhi::GraphicsPipelineHandle m_opaquePipeline;
    nvrhi::GraphicsPipelineHandle m_alphaTestedPipeline;
    nvrhi::BindingLayoutHandle m_bindingLayout;
    nvrhi::BindingLayoutHandle m_bindlessLayout;
    nvrhi::BindingSetHandle m_bindingSet;

    nvrhi::BufferHandle m_constantBuffer;

    std::shared_ptr<donut::engine::ShaderFactory> m_shaderFactory;
    std::shared_ptr<donut::engine::CommonRenderPasses> m_commonPasses;
    std::shared_ptr<donut::engine::Scene> m_scene;
    std::shared_ptr<Profiler> m_profiler;
};

class PostprocessGBufferPass
{
public:
    PostprocessGBufferPass(
        nvrhi::IDevice* device,
        std::shared_ptr<donut::engine::ShaderFactory> shaderFactory);

    void CreatePipeline();

    void CreateBindingSet(
        const RenderTargets& renderTargets);

    void Render(
        nvrhi::ICommandList* commandList,
        const donut::engine::IView& view);

    void NextFrame();

private:
    nvrhi::DeviceHandle m_device;

    nvrhi::ShaderHandle m_computeShader;
    nvrhi::ComputePipelineHandle m_computePipeline;
    nvrhi::BindingLayoutHandle m_bindingLayout;
    nvrhi::BindingLayoutHandle m_bindlessLayout;
    nvrhi::BindingSetHandle m_bindingSet;
    nvrhi::BindingSetHandle m_prevBindingSet;

    std::shared_ptr<donut::engine::ShaderFactory> m_shaderFactory;
};