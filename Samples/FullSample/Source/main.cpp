/***************************************************************************
 # Copyright (c) 2020-2023, NVIDIA CORPORATION.  All rights reserved.
 #
 # NVIDIA CORPORATION and its licensors retain all intellectual property
 # and proprietary rights in and to this software, related documentation
 # and any modifications thereto.  Any use, reproduction, disclosure or
 # distribution of this software and related documentation without an express
 # license agreement from NVIDIA CORPORATION is strictly prohibited.
 **************************************************************************/

// Include this first just to test the cleanliness
#include <Rtxdi/ImportanceSamplingContext.h>

#include <donut/render/ToneMappingPasses.h>
#include <donut/app/ApplicationBase.h>
#include <donut/app/Camera.h>
#include <donut/engine/BindingCache.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/TextureCache.h>
#include <donut/render/SkyPass.h>
#include <donut/engine/Scene.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/DescriptorTableManager.h>
#include <donut/engine/View.h>
#include <donut/engine/IesProfile.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <donut/core/math/math.h>
#include <nvrhi/utils.h>
#ifdef DONUT_WITH_TASKFLOW
#include <taskflow/taskflow.hpp>
#endif

#include "RenderPasses/CompositingPass.h"
#include "RenderPasses/GBufferPass.h"
#include "RenderPasses/GenerateMipsPass.h"
#include "RenderPasses/LightingPasses.h"
#include "RenderPasses/PrepareLightsPass.h"
#include "RenderPasses/RenderEnvironmentMapPass.h"
#include "Profiler.h"
#include "RenderTargets.h"
#include "RtxdiResources.h"
#include "SampleScene.h"
#include "UserInterface.h"

#include <jni.h>
#include <GL/glew.h>

#include "../../../External/donut/nvrhi/src/vulkan/vulkan-backend.h"

#ifndef _WIN32
#include <unistd.h>
#else
extern "C" {
  // Prefer using the discrete GPU on Optimus laptops
  _declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
}
#endif

using namespace donut;
using namespace donut::math;
using namespace std::chrono;
#include "../shaders/ShaderParameters.h"

static int g_ExitCode = 0;

class SceneRenderer : public app::ApplicationBase
{
public:
    inline static std::function<void()> renderGBuffer;
    
    SceneRenderer(app::DeviceManager* deviceManager, UIData& ui)
        : ApplicationBase(deviceManager)
        , m_bindingCache(deviceManager->GetDevice())
        , m_ui(ui)
    { 
        m_ui.resources->camera = &m_camera;
    }

    [[nodiscard]] std::shared_ptr<engine::ShaderFactory> GetShaderFactory() const
    {
        return m_shaderFactory;
    }

    [[nodiscard]] std::shared_ptr<vfs::IFileSystem> GetRootFs() const
    {
        return m_rootFs;
    }

    bool Init()
    {
        JavaVMInitArgs vm_args;
        int index = 0;
        JavaVMOption* options = new JavaVMOption[30];
        char arg1[] = "-Djava.library.path=C:/Users/Lukas/Documents/05_RandomProjects/RayTracing/Industrial/lib";
        char arg3[] = "-Djava.class.path=C:/Users/Lukas/Documents/05_RandomProjects/RayTracing/Industrial/target/classes;C:/Users/Lukas/.m2/repository/org/lwjgl/lwjgl/3.3.6/lwjgl-3.3.6.jar;C:/Users/Lukas/.m2/repository/org/lwjgl/lwjgl-glfw/3.3.6/lwjgl-glfw-3.3.6.jar;C:/Users/Lukas/.m2/repository/org/lwjgl/lwjgl-opengl/3.3.6/lwjgl-opengl-3.3.6.jar;C:/Users/Lukas/.m2/repository/org/lwjgl/lwjgl/3.3.6/lwjgl-3.3.6-natives-windows.jar;C:/Users/Lukas/.m2/repository/org/lwjgl/lwjgl-glfw/3.3.6/lwjgl-glfw-3.3.6-natives-windows.jar;C:/Users/Lukas/.m2/repository/org/lwjgl/lwjgl-opengl/3.3.6/lwjgl-opengl-3.3.6-natives-windows.jar;C:/Users/Lukas/.m2/repository/org/joml/joml/1.10.8/joml-1.10.8.jar;C:/Users/Lukas/.m2/repository/io/github/spair/imgui-java-lwjgl3/1.89.0/imgui-java-lwjgl3-1.89.0.jar;C:/Users/Lukas/.m2/repository/io/github/spair/imgui-java-binding/1.89.0/imgui-java-binding-1.89.0.jar;C:/Users/Lukas/.m2/repository/io/github/spair/imgui-java-natives-windows/1.89.0/imgui-java-natives-windows-1.89.0.jar";
        char arg4[] = "-Xlog:class+load=info";
        options[index++].optionString = arg1; 
        options[index++].optionString = arg3;
        // options[index++].optionString = arg4;
        
        vm_args.version = JNI_VERSION_21;
        vm_args.nOptions = index;
        vm_args.options = options;
        vm_args.ignoreUnrecognized = false;
        /* load and initialize a Java VM, return a JNI interface
         * pointer in env */

        JNI_CreateJavaVM(&vm, (void**)&env, &vm_args);

        jclass cls = env->FindClass("at/redi2go/industrial/client/LWJGLWindow");
        if (!cls) {
            env->ExceptionDescribe();  // 🔴 REQUIRED
            env->ExceptionClear();
        }
        
        jmethodID ctor = env->GetMethodID(cls, "<init>", "()V");
        jobject lwjglWindow = env->NewObject(cls, ctor);
        env->ExceptionDescribe();  // 🔴 REQUIRED
        env->ExceptionClear();
        
        glewInit();
        
        SceneRenderer::renderGBuffer = [this, cls, lwjglWindow]
        {
            // renderGBuffer(int viewDepth, int albedo, int specularRough, int normal, int geoNormal, int emissive, int motion, int depth)
            jmethodID renderGBufferId = env->GetMethodID(cls, "renderGBuffer", "(IIIIIIII)V");
            env->CallVoidMethod(lwjglWindow, renderGBufferId,
                dynamic_cast<nvrhi::vulkan::Texture*>(m_renderTargets->Depth.Get())->openglTexture,
                dynamic_cast<nvrhi::vulkan::Texture*>(m_renderTargets->GBufferDiffuseAlbedo.Get())->openglTexture,
                dynamic_cast<nvrhi::vulkan::Texture*>(m_renderTargets->GBufferSpecularRough.Get())->openglTexture,
                dynamic_cast<nvrhi::vulkan::Texture*>(m_renderTargets->GBufferNormals.Get())->openglTexture,
                dynamic_cast<nvrhi::vulkan::Texture*>(m_renderTargets->GBufferGeoNormals.Get())->openglTexture,
                dynamic_cast<nvrhi::vulkan::Texture*>(m_renderTargets->GBufferEmissive.Get())->openglTexture,
                dynamic_cast<nvrhi::vulkan::Texture*>(m_renderTargets->MotionVectors.Get())->openglTexture,
                dynamic_cast<nvrhi::vulkan::Texture*>(m_renderTargets->DeviceDepth.Get())->openglTexture
            );
            env->ExceptionDescribe();  // 🔴 REQUIRED
            env->ExceptionClear();
        };
        
        std::filesystem::path mediaPath = app::GetDirectoryWithExecutable().parent_path() / "Assets/Media";
        if (!std::filesystem::exists(mediaPath))
        {
            mediaPath = app::GetDirectoryWithExecutable().parent_path().parent_path() / "Assets/Media";
            if (!std::filesystem::exists(mediaPath))
            {
                log::error("Couldn't locate the 'Assets/Media' folder.");
                return false;
            }
        }

        std::filesystem::path frameworkShaderPath = app::GetDirectoryWithExecutable() / "shaders/framework" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());
        std::filesystem::path appShaderPath = app::GetDirectoryWithExecutable() / "shaders/full-sample" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());

        log::debug("Mounting %s to %s", mediaPath.string().c_str(), "/Assets/Media");
        log::debug("Mounting %s to %s", frameworkShaderPath.string().c_str(), "/shaders/donut");
        log::debug("Mounting %s to %s", appShaderPath.string().c_str(), "/shaders/app");

        m_rootFs = std::make_shared<vfs::RootFileSystem>();
        m_rootFs->mount("/Assets/Media", mediaPath);
        m_rootFs->mount("/shaders/donut", frameworkShaderPath);
        m_rootFs->mount("/shaders/app", appShaderPath);

        m_shaderFactory = std::make_shared<engine::ShaderFactory>(GetDevice(), m_rootFs, "/shaders");
        m_CommonPasses = std::make_shared<engine::CommonRenderPasses>(GetDevice(), m_shaderFactory);

        {
            nvrhi::BindlessLayoutDesc bindlessLayoutDesc;
            bindlessLayoutDesc.firstSlot = 0;
            bindlessLayoutDesc.registerSpaces = {
                nvrhi::BindingLayoutItem::RawBuffer_SRV(1),
                nvrhi::BindingLayoutItem::Texture_SRV(2),
                nvrhi::BindingLayoutItem::Texture_UAV(3)
            };
            bindlessLayoutDesc.visibility = nvrhi::ShaderType::All;
            bindlessLayoutDesc.maxCapacity = 1024;
            m_bindlessLayout = GetDevice()->createBindlessLayout(bindlessLayoutDesc);
        }

        std::filesystem::path scenePath = "/Assets/Media/PlatformGltf/Platform.gltf";

        m_descriptorTableManager = std::make_shared<engine::DescriptorTableManager>(GetDevice(), m_bindlessLayout);

        m_TextureCache = std::make_shared<donut::engine::TextureCache>(GetDevice(), m_rootFs, m_descriptorTableManager);
        m_TextureCache->SetInfoLogSeverity(donut::log::Severity::Debug);
        
        m_iesProfileLoader = std::make_unique<engine::IesProfileLoader>(GetDevice(), m_shaderFactory, m_descriptorTableManager);

        auto sceneTypeFactory = std::make_shared<SampleSceneTypeFactory>();
        m_scene = std::make_shared<SampleScene>(GetDevice(), *m_shaderFactory, m_rootFs, m_TextureCache, m_descriptorTableManager, sceneTypeFactory);
        m_ui.resources->scene = m_scene;

        SetAsynchronousLoadingEnabled(true);
        BeginLoadingScene(m_rootFs, scenePath);
        GetDeviceManager()->SetVsyncEnabled(true);

        if (!GetDevice()->queryFeatureSupport(nvrhi::Feature::RayQuery))
            m_ui.useRayQuery = false;

        m_profiler = std::make_shared<Profiler>(*GetDeviceManager());
        m_ui.resources->profiler = m_profiler;

        m_compositingPass = std::make_unique<CompositingPass>(GetDevice(), m_shaderFactory, m_CommonPasses, m_scene, m_bindlessLayout);
        m_rasterizedGBufferPass = std::make_unique<RasterizedGBufferPass>(GetDevice(), m_shaderFactory, m_CommonPasses, m_scene, m_profiler, m_bindlessLayout);
        m_postprocessGBufferPass = std::make_unique<PostprocessGBufferPass>(GetDevice(), m_shaderFactory);
        m_prepareLightsPass = std::make_unique<PrepareLightsPass>(GetDevice(), m_shaderFactory, m_CommonPasses, m_scene, m_bindlessLayout);
        m_lightingPasses = std::make_unique<LightingPasses>(GetDevice(), m_shaderFactory, m_CommonPasses, m_scene, m_profiler, m_bindlessLayout);

        LoadShaders();

        std::vector<std::string> profileNames;
        m_rootFs->enumerateFiles("/Assets/Media/ies-profiles", { ".ies" }, vfs::enumerate_to_vector(profileNames));

        for (const std::string& profileName : profileNames)
        {
            auto profile = m_iesProfileLoader->LoadIesProfile(*m_rootFs, "/Assets/Media/ies-profiles/" + profileName);

            if (profile)
            {
                m_iesProfiles.push_back(profile);
            }
        }
        m_ui.resources->iesProfiles = m_iesProfiles;

        m_commandList = GetDevice()->createCommandList();

        return true;
    }

    void AssignIesProfiles(nvrhi::ICommandList* commandList)
    {
        for (const auto& light : m_scene->GetSceneGraph()->GetLights())
        {
            if (light->GetLightType() == LightType_Spot)
            {
                SpotLightWithProfile& spotLight = static_cast<SpotLightWithProfile&>(*light);

                if (spotLight.profileName.empty())
                    continue;

                if (spotLight.profileTextureIndex >= 0)
                    continue;

                auto foundProfile = std::find_if(m_iesProfiles.begin(), m_iesProfiles.end(),
                    [&spotLight](auto it) { return it->name == spotLight.profileName; });

                if (foundProfile != m_iesProfiles.end())
                {
                    m_iesProfileLoader->BakeIesProfile(**foundProfile, commandList);

                    spotLight.profileTextureIndex = (*foundProfile)->textureIndex;
                }
            }
        }
    }

    virtual void SceneLoaded() override
    {
        ApplicationBase::SceneLoaded();

        m_scene->FinishedLoading(GetFrameIndex());

        // m_camera.LookAt({ 1.71, 3.59, -3.16 }, float3{ 1.71, 3.59, -2.16 });
        m_camera.LookAt({ 2.71, 3.59, -2.16 }, float3{ 2.71, 3.59, -2.16 } + float3{ 0.0f, -0.342f, 0.9397f });
        // m_camera.LookAt({ 1.71, 3.59, -3.16 }, { 0.53, 1.53, 1.47 });
        m_camera.SetMoveSpeed(3.f);

        const auto& sceneGraph = m_scene->GetSceneGraph();
    
        auto pointLight = std::make_shared<engine::PointLight>();
        pointLight->color = { 1.0f, 0.0f, 0.0f };
        pointLight->radius = 0.5;
        
        auto pointLightNode = sceneGraph->AttachLeafNode(sceneGraph->GetRootNode(), pointLight);
        pointLightNode->SetTranslation({ 1.53, 1.53, 2.47 });

        for (const auto& pLight : sceneGraph->GetLights())
        {
            if (pLight->GetLightType() == LightType_Directional)
            {
                m_sunLight = std::static_pointer_cast<engine::DirectionalLight>(pLight);
                break;
            }
        }

        if (!m_sunLight)
        {
            // TODO: Redi Lukas Sun Light Directional
            m_sunLight = std::make_shared<engine::DirectionalLight>();
            // sceneGraph->AttachLeafNode(sceneGraph->GetRootNode(), m_sunLight);
            // m_sunLight->SetDirection(dm::double3(0.15, -1.0, 0.3));
            m_sunLight->angularSize = 1.f;
        }

        m_commandList->open();
        AssignIesProfiles(m_commandList);
        m_commandList->close();
        GetDevice()->executeCommandList(m_commandList);

        // Create an environment light
        m_environmentLight = std::make_shared<EnvironmentLight>();
        // TODO: Redi Lukas Env Map 
        // sceneGraph->AttachLeafNode(sceneGraph->GetRootNode(), m_environmentLight);
        // m_environmentLight->SetName("Environment");
        m_ui.environmentMapDirty = 2;
        m_ui.environmentMapIndex = 0;
        
        m_rasterizedGBufferPass->CreateBindingSet();

        m_scene->BuildMeshBLASes(GetDevice());

        GetDeviceManager()->SetVsyncEnabled(false);

        m_ui.isLoading = false;
    }
    
    void LoadShaders()
    {
        m_compositingPass->CreatePipeline();
        m_postprocessGBufferPass->CreatePipeline();
        m_prepareLightsPass->CreatePipeline();
    }

    virtual bool LoadScene(std::shared_ptr<vfs::IFileSystem> fs, const std::filesystem::path& sceneFileName) override 
    {
        if (m_scene->Load(sceneFileName))
        {
            return true;
        }

        return false;
    }

    bool KeyboardUpdate(int key, int scancode, int action, int mods) override
    {
        if (key == GLFW_KEY_GRAVE_ACCENT && action == GLFW_PRESS)
        {
            m_ui.showUI = !m_ui.showUI;
            return true;
        }

        if (mods == GLFW_MOD_CONTROL && key == GLFW_KEY_R && action == GLFW_PRESS)
        {
            m_ui.reloadShaders = true;
            return true;
        }

        if (mods == 0 && key == GLFW_KEY_F1 && action == GLFW_PRESS)
        {
            m_frameStepMode = (m_frameStepMode == FrameStepMode::Disabled) ? FrameStepMode::Wait : FrameStepMode::Disabled;
            return true;
        }

        if (mods == 0 && key == GLFW_KEY_F2 && action == GLFW_PRESS)
        {
            if (m_frameStepMode == FrameStepMode::Wait)
                m_frameStepMode = FrameStepMode::Step;
            return true;
        }

        if (mods == 0 && key == GLFW_KEY_F5 && action == GLFW_PRESS)
        {
            if (m_ui.animationFrame.has_value())
            {
                // Stop benchmark if it's running
                m_ui.animationFrame.reset();
            }
            else
            {
                // Start benchmark otherwise
                m_ui.animationFrame = std::optional<int>(0);
            }
            return true;
        }
        
        m_camera.KeyboardUpdate(key, scancode, action, mods);

        return true;
    }

    virtual bool MousePosUpdate(double xpos, double ypos) override
    {
        m_camera.MousePosUpdate(xpos, ypos);
        return true;
    }

    virtual bool MouseButtonUpdate(int button, int action, int mods) override
    {
        if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS)
        {
            double mousex = 0, mousey = 0;
            glfwGetCursorPos(GetDeviceManager()->GetWindow(), &mousex, &mousey);

            // Scale the mouse position according to the render resolution scale
            mousex *= m_view.GetViewport().width() / m_upscaledView.GetViewport().width();
            mousey *= m_view.GetViewport().height() / m_upscaledView.GetViewport().height();

            m_ui.gbufferSettings.materialReadbackPosition = int2(int(mousex), int(mousey));
            m_ui.gbufferSettings.enableMaterialReadback = true;
            return true;
        }

        m_camera.MouseButtonUpdate(button, action, mods);
        return true;
    }

    virtual void Animate(float fElapsedTimeSeconds) override
    {
        if (m_ui.isLoading)
            return;

        m_camera.Animate(fElapsedTimeSeconds);

        if (m_ui.enableAnimations)
            m_scene->Animate(fElapsedTimeSeconds * m_ui.animationSpeed);

        if (m_toneMappingPass)
            m_toneMappingPass->AdvanceFrame(fElapsedTimeSeconds);
    }

    virtual void BackBufferResized(const uint32_t width, const uint32_t height, const uint32_t sampleCount) override
    {
        if (m_renderTargets && m_renderTargets->Size.x == int(width) && m_renderTargets->Size.y == int(height))
            return;

        m_bindingCache.Clear();
        m_renderTargets = nullptr;
        m_isContext = nullptr;
        m_rtxdiResources = nullptr;
        m_toneMappingPass = nullptr;
    }

    void LoadEnvironmentMap()
    {
        if (m_environmentMap)
        {
            // Make sure there is no rendering in-flight before we unload the texture and erase its descriptor.
            // Decsriptor manipulations are synchronous and immediately affect whatever is executing on the GPU.
            GetDevice()->waitForIdle();

            m_TextureCache->UnloadTexture(m_environmentMap);
            
            m_environmentMap = nullptr;
        }

        if (m_ui.environmentMapIndex > 0)
        {
            auto& environmentMaps = m_scene->GetEnvironmentMaps();
            const std::string& environmentMapPath = environmentMaps[m_ui.environmentMapIndex];

            m_environmentMap = m_TextureCache->LoadTextureFromFileDeferred(environmentMapPath, false);

            if (m_TextureCache->IsTextureLoaded(m_environmentMap))
            {
                m_TextureCache->ProcessRenderingThreadCommands(*m_CommonPasses, 0.f);
                m_TextureCache->LoadingFinished();

                m_environmentMap->bindlessDescriptor = m_descriptorTableManager->CreateDescriptorHandle(nvrhi::BindingSetItem::Texture_SRV(0, m_environmentMap->texture));
            }
            else
            {
                // Failed to load the file: revert to the procedural map and remove this file from the list.
                m_environmentMap = nullptr;
                environmentMaps.erase(environmentMaps.begin() + m_ui.environmentMapIndex);
                m_ui.environmentMapIndex = 0;
            }
        }
    }

    void SetupView(uint32_t renderWidth, uint32_t renderHeight, const engine::PerspectiveCamera* activeCamera)
    {
        nvrhi::Viewport windowViewport((float)renderWidth, (float)renderHeight);

        nvrhi::Viewport renderViewport = windowViewport;
        renderViewport.maxX = roundf(renderViewport.maxX * m_ui.resolutionScale);
        renderViewport.maxY = roundf(renderViewport.maxY * m_ui.resolutionScale);

        m_view.SetViewport(renderViewport);
        {
            m_view.SetPixelOffset(0.f);
        }

        const float aspectRatio = windowViewport.width() / windowViewport.height();
        if (activeCamera)
            m_view.SetMatrices(activeCamera->GetWorldToViewMatrix(), perspProjD3DStyleReverse(activeCamera->verticalFov, aspectRatio, activeCamera->zNear));
        else
            m_view.SetMatrices(m_camera.GetWorldToViewMatrix(), perspProjD3DStyleReverse(radians(m_ui.verticalFov), aspectRatio, 0.01f));
        m_view.UpdateCache();

        if (m_viewPrevious.GetViewExtent().width() == 0)
            m_viewPrevious = m_view;

        m_upscaledView = m_view;
        m_upscaledView.SetViewport(windowViewport);
    }

    void SetupRenderPasses(uint32_t renderWidth, uint32_t renderHeight, bool& exposureResetRequired)
    {
        if (m_ui.environmentMapDirty == 2)
        {
            m_environmentMapPdfMipmapPass = nullptr;

            m_ui.environmentMapDirty = 1;
        }

        if (m_ui.reloadShaders)
        {
            GetDevice()->waitForIdle();

            m_shaderFactory->ClearCache();
            m_renderEnvironmentMapPass = nullptr;
            m_environmentMapPdfMipmapPass = nullptr;
            m_localLightPdfMipmapPass = nullptr;
            m_ui.environmentMapDirty = 1;

            LoadShaders();
        }

        bool renderTargetsCreated = false;
        bool rtxdiResourcesCreated = false;

        if (!m_renderEnvironmentMapPass)
        {
            m_renderEnvironmentMapPass = std::make_unique<RenderEnvironmentMapPass>(GetDevice(), m_shaderFactory, m_descriptorTableManager, 2048);
        }
        
        const auto environmentMap = (m_ui.environmentMapIndex > 0)
            ? m_environmentMap->texture.Get()
            : m_renderEnvironmentMapPass->GetTexture();

        uint32_t numEmissiveMeshes, numEmissiveTriangles;
        m_prepareLightsPass->CountLightsInScene(numEmissiveMeshes, numEmissiveTriangles);
        uint32_t numPrimitiveLights = uint32_t(m_scene->GetSceneGraph()->GetLights().size());
        uint32_t numGeometryInstances = uint32_t(m_scene->GetSceneGraph()->GetGeometryInstancesCount());
        
        uint2 environmentMapSize = uint2(environmentMap->getDesc().width, environmentMap->getDesc().height);

        if (m_rtxdiResources && (
            environmentMapSize.x != m_rtxdiResources->EnvironmentPdfTexture->getDesc().width ||
            environmentMapSize.y != m_rtxdiResources->EnvironmentPdfTexture->getDesc().height ||
            numEmissiveMeshes > m_rtxdiResources->GetMaxEmissiveMeshes() ||
            numEmissiveTriangles > m_rtxdiResources->GetMaxEmissiveTriangles() || 
            numPrimitiveLights > m_rtxdiResources->GetMaxPrimitiveLights() ||
            numGeometryInstances > m_rtxdiResources->GetMaxGeometryInstances()))
        {
            m_rtxdiResources = nullptr;
        }

        if (!m_isContext)
        {
            rtxdi::ImportanceSamplingContext_StaticParameters isStaticParams;
            isStaticParams.CheckerboardSamplingMode = m_ui.restirDIStaticParams.CheckerboardSamplingMode;
            isStaticParams.renderHeight = renderHeight;
            isStaticParams.renderWidth = renderWidth;

            m_isContext = std::make_unique<rtxdi::ImportanceSamplingContext>(isStaticParams);
        }

        if (!m_renderTargets)
        {
            m_renderTargets = std::make_shared<RenderTargets>(GetDevice(), int2((int)renderWidth, (int)renderHeight));

            m_profiler->SetRenderTargets(m_renderTargets);

            m_postprocessGBufferPass->CreateBindingSet(*m_renderTargets);

            m_rasterizedGBufferPass->CreatePipeline(*m_renderTargets);

            m_compositingPass->CreateBindingSet(*m_renderTargets);

            renderTargetsCreated = true;
        }

        if (!m_rtxdiResources)
        {
            uint32_t meshAllocationQuantum = 128;
            uint32_t triangleAllocationQuantum = 1024;
            uint32_t primitiveAllocationQuantum = 128;

            m_rtxdiResources = std::make_unique<RtxdiResources>(
                GetDevice(), 
                m_isContext->GetReSTIRDIContext(),
                m_isContext->GetRISBufferSegmentAllocator(),
                (numEmissiveMeshes + meshAllocationQuantum - 1) & ~(meshAllocationQuantum - 1),
                (numEmissiveTriangles + triangleAllocationQuantum - 1) & ~(triangleAllocationQuantum - 1),
                (numPrimitiveLights + primitiveAllocationQuantum - 1) & ~(primitiveAllocationQuantum - 1),
                numGeometryInstances,
                environmentMapSize.x,
                environmentMapSize.y);

            m_prepareLightsPass->CreateBindingSet(*m_rtxdiResources);
            
            rtxdiResourcesCreated = true;

            // Make sure that the environment PDF map is re-generated
            m_ui.environmentMapDirty = 1;
        }
        
        if (!m_environmentMapPdfMipmapPass || rtxdiResourcesCreated)
        {
            m_environmentMapPdfMipmapPass = std::make_unique<GenerateMipsPass>(
                GetDevice(),
                m_shaderFactory,
                environmentMap,
                m_rtxdiResources->EnvironmentPdfTexture);
        }

        if (!m_localLightPdfMipmapPass || rtxdiResourcesCreated)
        {
            m_localLightPdfMipmapPass = std::make_unique<GenerateMipsPass>(
                GetDevice(),
                m_shaderFactory,
                nullptr,
                m_rtxdiResources->LocalLightPdfTexture);
        }

        if (renderTargetsCreated || rtxdiResourcesCreated)
        {
            m_lightingPasses->CreateBindingSet(
                m_scene->GetTopLevelAS(),
                m_scene->GetPrevTopLevelAS(),
                *m_renderTargets,
                *m_rtxdiResources);
        }

        if (rtxdiResourcesCreated || m_ui.reloadShaders)
        {
            // Some RTXDI context settings affect the shader permutations
            m_lightingPasses->CreatePipelines(m_ui.useRayQuery);
        }

        m_ui.reloadShaders = false;

        exposureResetRequired = false;
        if (!m_toneMappingPass)
        {
            render::ToneMappingPass::CreateParameters toneMappingParams;
            m_toneMappingPass = std::make_unique<render::ToneMappingPass>(GetDevice(), m_shaderFactory, m_CommonPasses, m_renderTargets->LdrFramebuffer, m_upscaledView, toneMappingParams);
            exposureResetRequired = true;
        }
    }

    virtual void RenderSplashScreen(nvrhi::IFramebuffer* framebuffer) override
    {
        nvrhi::ITexture* framebufferTexture = framebuffer->getDesc().colorAttachments[0].texture;
        m_commandList->open();
        m_commandList->clearTextureFloat(framebufferTexture, nvrhi::AllSubresources, nvrhi::Color(0.f));
        m_commandList->close();
        GetDevice()->executeCommandList(m_commandList);

        uint32_t loadedObjects = engine::Scene::GetLoadingStats().ObjectsLoaded;
        uint32_t requestedObjects = engine::Scene::GetLoadingStats().ObjectsTotal;
        uint32_t loadedTextures = m_TextureCache->GetNumberOfLoadedTextures();
        uint32_t finalizedTextures = m_TextureCache->GetNumberOfFinalizedTextures();
        uint32_t requestedTextures = m_TextureCache->GetNumberOfRequestedTextures();
        uint32_t objectMultiplier = 20;
        m_ui.loadingPercentage = (requestedTextures > 0) 
            ? float(loadedTextures + finalizedTextures + loadedObjects * objectMultiplier) / float(requestedTextures * 2 + requestedObjects * objectMultiplier) 
            : 0.f;
    }

    void Resolve(nvrhi::ICommandList* commandList, float accumulationWeight) const
    {
        ProfilerScope scope(*m_profiler, commandList, ProfilerSection::Resolve);

        switch (m_ui.aaMode)
        {
        case AntiAliasingMode::None: {
            engine::BlitParameters blitParams;
            blitParams.sourceTexture = m_renderTargets->HdrColor;
            blitParams.sourceBox.m_maxs.x = m_view.GetViewport().width() / m_upscaledView.GetViewport().width();
            blitParams.sourceBox.m_maxs.y = m_view.GetViewport().height() / m_upscaledView.GetViewport().height();
            blitParams.targetFramebuffer = m_renderTargets->ResolvedFramebuffer->GetFramebuffer(m_upscaledView);
            m_CommonPasses->BlitTexture(commandList, blitParams);
            break;
        }
        }
    }

    void UpdateReSTIRDIContextFromUI()
    {
        rtxdi::ReSTIRDIContext& restirDIContext = m_isContext->GetReSTIRDIContext();
        ReSTIRDI_InitialSamplingParameters initialSamplingParams = m_ui.restirDI.initialSamplingParams;
        switch (initialSamplingParams.localLightSamplingMode)
        {
        default:
        case ReSTIRDI_LocalLightSamplingMode::Uniform:
            initialSamplingParams.numPrimaryLocalLightSamples = m_ui.restirDI.numLocalLightUniformSamples;
            break;
        case ReSTIRDI_LocalLightSamplingMode::Power_RIS:
            initialSamplingParams.numPrimaryLocalLightSamples = m_ui.restirDI.numLocalLightPowerRISSamples;
            break;
        case ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS:
            initialSamplingParams.numPrimaryLocalLightSamples = m_ui.restirDI.numLocalLightReGIRRISSamples;
            break;
        }
        restirDIContext.SetResamplingMode(m_ui.restirDI.resamplingMode);
        restirDIContext.SetInitialSamplingParameters(initialSamplingParams);
        restirDIContext.SetTemporalResamplingParameters(m_ui.restirDI.temporalResamplingParams);
        restirDIContext.SetSpatialResamplingParameters(m_ui.restirDI.spatialResamplingParams);
        restirDIContext.SetShadingParameters(m_ui.restirDI.shadingParams);
    }

    void UpdateReSTIRGIContextFromUI()
    {
        rtxdi::ReSTIRGIContext& restirGIContext = m_isContext->GetReSTIRGIContext();
        restirGIContext.SetResamplingMode(m_ui.restirGI.resamplingMode);
        restirGIContext.SetTemporalResamplingParameters(m_ui.restirGI.temporalResamplingParams);
        restirGIContext.SetSpatialResamplingParameters(m_ui.restirGI.spatialResamplingParams);
        restirGIContext.SetFinalShadingParameters(m_ui.restirGI.finalShadingParams);
    }

    bool IsLocalLightPowerRISEnabled()
    {
        if (m_ui.indirectLightingMode == IndirectLightingMode::ReStirGI)
        {
            ReSTIRDI_InitialSamplingParameters indirectReSTIRDISamplingParams = m_ui.lightingSettings.brdfptParams.secondarySurfaceReSTIRDIParams.initialSamplingParams;
            bool enabled = (indirectReSTIRDISamplingParams.localLightSamplingMode == ReSTIRDI_LocalLightSamplingMode::Power_RIS) ||
                           (indirectReSTIRDISamplingParams.localLightSamplingMode == ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS && m_isContext->GetReGIRContext().IsLocalLightPowerRISEnable());
            if (enabled)
                return true;
        }
        return m_isContext->IsLocalLightPowerRISEnabled();
    }

    void RenderScene(nvrhi::IFramebuffer* framebuffer) override
    {
        if (m_frameStepMode == FrameStepMode::Wait)
        {
            nvrhi::TextureHandle finalImage;

            if (m_ui.enableToneMapping)
                finalImage = m_renderTargets->LdrColor;
            else
                finalImage = m_renderTargets->HdrColor;

            m_commandList->open();

            m_CommonPasses->BlitTexture(m_commandList, framebuffer, finalImage, &m_bindingCache);

            m_commandList->close();
            GetDevice()->executeCommandList(m_commandList);

            return;
        }

        if (m_frameStepMode == FrameStepMode::Step)
            m_frameStepMode = FrameStepMode::Wait;

        const engine::PerspectiveCamera* activeCamera = nullptr;
        uint effectiveFrameIndex = m_renderFrameIndex;

        if (m_ui.animationFrame.has_value())
        {
            const float animationTime = float(m_ui.animationFrame.value()) * (1.f / 240.f);
            
            auto* animation = m_scene->GetBenchmarkAnimation();
            if (animation && animationTime < animation->GetDuration())
            {
                (void)animation->Apply(animationTime);
                activeCamera = m_scene->GetBenchmarkCamera();
                effectiveFrameIndex = m_ui.animationFrame.value();
                m_ui.animationFrame = effectiveFrameIndex + 1;
            }
            else
            {
                m_ui.benchmarkResults = m_profiler->GetAsText();
                m_ui.animationFrame.reset();
            }
        }

        bool exposureResetRequired = false;

        if (m_ui.enableFpsLimit && GetFrameIndex() > 0)
        {
            uint64_t expectedFrametime = 1000000 / m_ui.fpsLimit;

            while (true)
            {
                uint64_t currentFrametime = duration_cast<microseconds>(steady_clock::now() - m_previousFrameTimeStamp).count();

                if(currentFrametime >= expectedFrametime)
                    break;
#ifdef _WIN32
                Sleep(0);
#else
                usleep(100);
#endif
            }
        }

        m_previousFrameTimeStamp = steady_clock::now();

        if (m_ui.resetISContext)
        {
            GetDevice()->waitForIdle();

            m_isContext = nullptr;
            m_rtxdiResources = nullptr;
            m_ui.resetISContext = false;
        }

        if (m_ui.environmentMapDirty == 2)
        {
            LoadEnvironmentMap();
        }

        m_scene->RefreshSceneGraph(GetFrameIndex());

        const auto& fbinfo = framebuffer->getFramebufferInfo();
        uint32_t renderWidth = fbinfo.width;
        uint32_t renderHeight = fbinfo.height;
        SetupView(renderWidth, renderHeight, activeCamera);
        SetupRenderPasses(renderWidth, renderHeight, exposureResetRequired);
        if (!m_ui.freezeRegirPosition)
            m_regirCenter = m_camera.GetPosition();
        UpdateReSTIRDIContextFromUI();
        UpdateReSTIRGIContextFromUI();

        m_postprocessGBufferPass->NextFrame();
        m_lightingPasses->NextFrame();
        m_compositingPass->NextFrame();
        m_renderTargets->NextFrame();
        m_scene->NextFrame();
        
        bool cameraIsStatic = m_previousViewValid && m_view.GetViewMatrix() == m_viewPrevious.GetViewMatrix();
        m_ui.numAccumulatedFrames = 1;
        m_profiler->EnableAccumulation(m_ui.animationFrame.has_value());

        float accumulationWeight = 1.f / (float)m_ui.numAccumulatedFrames;

        m_profiler->ResolvePreviousFrame();
        
        int materialIndex = m_profiler->GetMaterialReadback();
        if (materialIndex >= 0)
        {
            for (const auto& material : m_scene->GetSceneGraph()->GetMaterials())
            {
                if (material->materialID == materialIndex)
                {
                    m_ui.resources->selectedMaterial = material;
                    break;
                }
            }
        }
        
        if (m_ui.environmentMapIndex >= 0)
        {
            if (m_environmentMap)
            {
                m_environmentLight->textureIndex = m_environmentMap->bindlessDescriptor.Get();
                const auto& textureDesc = m_environmentMap->texture->getDesc();
                m_environmentLight->textureSize = uint2(textureDesc.width, textureDesc.height);
            }
            else
            {
                m_environmentLight->textureIndex = m_renderEnvironmentMapPass->GetTextureIndex();
                const auto& textureDesc = m_renderEnvironmentMapPass->GetTexture()->getDesc();
                m_environmentLight->textureSize = uint2(textureDesc.width, textureDesc.height);
            }
            m_environmentLight->radianceScale = ::exp2f(m_ui.environmentIntensityBias);
            m_environmentLight->rotation = m_ui.environmentRotation / 360.f;  //  +/- 0.5
            m_sunLight->irradiance = (m_ui.environmentMapIndex > 0) ? 0.f : 1.f;
        }
        else
        {
            m_environmentLight->textureIndex = -1;
            m_sunLight->irradiance = 0.f;
        }
        
        uint32_t denoiserMode = DENOISER_MODE_OFF;

        m_commandList->open();

        m_profiler->BeginFrame(m_commandList);

        AssignIesProfiles(m_commandList);
        m_scene->RefreshBuffers(m_commandList, GetFrameIndex());
        m_rtxdiResources->InitializeNeighborOffsets(m_commandList, m_isContext->GetNeighborOffsetCount());

        if (m_framesSinceAnimation < 2)
        {
            ProfilerScope scope(*m_profiler, m_commandList, ProfilerSection::TlasUpdate);

            m_scene->UpdateSkinnedMeshBLASes(m_commandList, GetFrameIndex());
            m_scene->BuildTopLevelAccelStruct(m_commandList);
        }
        m_commandList->compactBottomLevelAccelStructs();

        if (m_ui.environmentMapDirty)
        {
            ProfilerScope scope(*m_profiler, m_commandList, ProfilerSection::EnvironmentMap);

            if (m_ui.environmentMapIndex == 0)
            {
                donut::render::SkyParameters params;
                m_renderEnvironmentMapPass->Render(m_commandList, *m_sunLight, params);
            }
            
            m_environmentMapPdfMipmapPass->Process(m_commandList);

            m_ui.environmentMapDirty = 0;
        }

        nvrhi::utils::ClearColorAttachment(m_commandList, framebuffer, 0, nvrhi::Color(0.f));

        {
            ProfilerScope scope(*m_profiler, m_commandList, ProfilerSection::GBufferFill);

            GBufferSettings gbufferSettings = m_ui.gbufferSettings;
            float upscalingLodBias = ::log2f(m_view.GetViewport().width() / m_upscaledView.GetViewport().width());
            gbufferSettings.textureLodBias += upscalingLodBias;
            
            renderGBuffer();

            /*m_rasterizedGBufferPass->Render(m_commandList, m_view, m_viewPrevious, *m_renderTargets, m_ui.gbufferSettings);

            m_postprocessGBufferPass->Render(m_commandList, m_view);*/            
        }

        // The light indexing members of frameParameters are written by PrepareLightsPass below
        rtxdi::ReSTIRDIContext& restirDIContext = m_isContext->GetReSTIRDIContext();
        restirDIContext.SetFrameIndex(effectiveFrameIndex);
        m_isContext->GetReSTIRGIContext().SetFrameIndex(effectiveFrameIndex);

        {
            ProfilerScope scope(*m_profiler, m_commandList, ProfilerSection::MeshProcessing);
            
            RTXDI_LightBufferParameters lightBufferParams = m_prepareLightsPass->Process(
                m_commandList,
                restirDIContext,
                m_scene->GetSceneGraph()->GetLights(),
                m_environmentMapPdfMipmapPass != nullptr && m_ui.environmentMapImportanceSampling);
            m_isContext->SetLightBufferParams(lightBufferParams);

            auto initialSamplingParams = restirDIContext.GetInitialSamplingParameters();
            initialSamplingParams.environmentMapImportanceSampling = lightBufferParams.environmentLightParams.lightPresent;
            m_ui.restirDI.initialSamplingParams.environmentMapImportanceSampling = initialSamplingParams.environmentMapImportanceSampling;
            restirDIContext.SetInitialSamplingParameters(initialSamplingParams);
        }

        if (IsLocalLightPowerRISEnabled())
        {
            ProfilerScope scope(*m_profiler, m_commandList, ProfilerSection::LocalLightPdfMap);
            
            m_localLightPdfMipmapPass->Process(m_commandList);
        }
        
        LightingPasses::RenderSettings lightingSettings = m_ui.lightingSettings;
        lightingSettings.enablePreviousTLAS &= m_ui.enableAnimations;
        lightingSettings.enableAlphaTestedGeometry = m_ui.gbufferSettings.enableAlphaTestedGeometry;
        lightingSettings.denoiserMode = DENOISER_MODE_OFF;
        if (lightingSettings.denoiserMode == DENOISER_MODE_OFF)
            lightingSettings.enableGradients = false;

        const bool checkerboard = restirDIContext.GetStaticParameters().CheckerboardSamplingMode != rtxdi::CheckerboardMode::Off;

        bool enableDirectReStirPass = m_ui.directLightingMode == DirectLightingMode::ReStir;
        bool enableIndirect = m_ui.indirectLightingMode != IndirectLightingMode::None;

        // When indirect lighting is enabled, we don't want ReSTIR to be the NRD front-end,
        // it should just write out the raw color data.
        ReSTIRDI_ShadingParameters restirDIShadingParams = m_isContext->GetReSTIRDIContext().GetShadingParameters();
        restirDIShadingParams.enableDenoiserInputPacking = !enableIndirect;
        m_isContext->GetReSTIRDIContext().SetShadingParameters(restirDIShadingParams);

        if (!enableDirectReStirPass)
        {
            // Secondary resampling can only be done as a post-process of ReSTIR direct lighting
            lightingSettings.brdfptParams.enableSecondaryResampling = false;

            // Gradients are only produced by the direct ReSTIR pass
            lightingSettings.enableGradients = false;
        }

        if (enableDirectReStirPass || enableIndirect)
        {
            m_lightingPasses->PrepareForLightSampling(m_commandList,
                *m_isContext,
                m_view, m_viewPrevious,
                lightingSettings,
                /* enableAccumulation = */ false);
        }

        if (enableDirectReStirPass)
        {
            m_commandList->clearTextureFloat(m_renderTargets->Gradients, nvrhi::AllSubresources, nvrhi::Color(0.f));

            m_lightingPasses->RenderDirectLighting(m_commandList,
                restirDIContext,
                m_view,
                lightingSettings);
        }

        // If none of the passes above were executed, clear the textures to avoid stale data there.
        // It's a weird mode but it can be selected from the UI.
        if (!enableDirectReStirPass)
        {
            m_commandList->clearTextureFloat(m_renderTargets->DiffuseLighting, nvrhi::AllSubresources, nvrhi::Color(0.f));
            m_commandList->clearTextureFloat(m_renderTargets->SpecularLighting, nvrhi::AllSubresources, nvrhi::Color(0.f));
        }

        m_compositingPass->Render(
            m_commandList,
            m_view,
            m_viewPrevious,
            denoiserMode,
            checkerboard,
            m_ui,
            *m_environmentLight);

        Resolve(m_commandList, accumulationWeight);

        // Reference image functionality:
        {
            // When the camera is moved, discard the previously stored image, if any, and disable its display.
            if (!cameraIsStatic)
            {
                m_ui.referenceImageCaptured = false;
                m_ui.referenceImageSplit = 0.f;
            }

            // When the user clicks the "Store" button, copy the ResolvedColor texture into ReferenceColor.
            if (m_ui.storeReferenceImage)
            {
                m_commandList->copyTexture(m_renderTargets->ReferenceColor, nvrhi::TextureSlice(), m_renderTargets->ResolvedColor, nvrhi::TextureSlice());
                m_ui.storeReferenceImage = false;
                m_ui.referenceImageCaptured = true;
            }

            // When the "Split Display" parameter is nonzero, show a portion of the previously stored
            // ReferenceColor texture on the left side of the screen by copying it into the ResolvedColor texture.
            if (m_ui.referenceImageSplit > 0.f)
            {
                engine::BlitParameters blitParams;
                blitParams.sourceTexture = m_renderTargets->ReferenceColor;
                blitParams.sourceBox.m_maxs = float2(m_ui.referenceImageSplit, 1.f);
                blitParams.targetFramebuffer = m_renderTargets->ResolvedFramebuffer->GetFramebuffer(nvrhi::AllSubresources);
                blitParams.targetBox = blitParams.sourceBox;
                blitParams.sampler = engine::BlitSampler::Point;
                m_CommonPasses->BlitTexture(m_commandList, blitParams, &m_bindingCache);
            }
        }

        if(m_ui.enableToneMapping)
        { // Tone mapping
            render::ToneMappingParameters ToneMappingParams;
            ToneMappingParams.minAdaptedLuminance = 0.002f;
            ToneMappingParams.maxAdaptedLuminance = 0.2f;
            ToneMappingParams.exposureBias = m_ui.exposureBias;
            ToneMappingParams.eyeAdaptationSpeedUp = 2.0f;
            ToneMappingParams.eyeAdaptationSpeedDown = 1.0f;

            if (exposureResetRequired)
            {
                ToneMappingParams.eyeAdaptationSpeedUp = 0.f;
                ToneMappingParams.eyeAdaptationSpeedDown = 0.f;
            }

            m_toneMappingPass->SimpleRender(m_commandList, ToneMappingParams, m_upscaledView, m_renderTargets->ResolvedColor);
        }
        else
        {
            m_CommonPasses->BlitTexture(m_commandList, m_renderTargets->LdrFramebuffer->GetFramebuffer(m_upscaledView), m_renderTargets->ResolvedColor, &m_bindingCache);
        }

        switch (m_ui.debugRenderOutputBuffer)
        {
            case DebugRenderOutput::LDRColor:
                m_CommonPasses->BlitTexture(m_commandList, framebuffer, m_renderTargets->LdrColor, &m_bindingCache);
                break;
            case DebugRenderOutput::Depth:
                m_CommonPasses->BlitTexture(m_commandList, framebuffer, m_renderTargets->Depth, &m_bindingCache);
                break;
            case GBufferDiffuseAlbedo:
                m_CommonPasses->BlitTexture(m_commandList, framebuffer, m_renderTargets->DebugColor, &m_bindingCache);
                break;
            case GBufferSpecularRough:
                m_CommonPasses->BlitTexture(m_commandList, framebuffer, m_renderTargets->DebugColor, &m_bindingCache);
                break;
            case GBufferNormals:
                m_CommonPasses->BlitTexture(m_commandList, framebuffer, m_renderTargets->DebugColor, &m_bindingCache);
                break;
            case GBufferGeoNormals:
                m_CommonPasses->BlitTexture(m_commandList, framebuffer, m_renderTargets->DebugColor, &m_bindingCache);
                break;
            case GBufferEmissive:
                m_CommonPasses->BlitTexture(m_commandList, framebuffer, m_renderTargets->GBufferEmissive, &m_bindingCache);
                break;
            case DiffuseLighting:
                m_CommonPasses->BlitTexture(m_commandList, framebuffer, m_renderTargets->DiffuseLighting, &m_bindingCache);
                break;
            case SpecularLighting:
                m_CommonPasses->BlitTexture(m_commandList, framebuffer, m_renderTargets->SpecularLighting, &m_bindingCache);
                break;
            case RestirLuminance:
                m_CommonPasses->BlitTexture(m_commandList, framebuffer, m_renderTargets->RestirLuminance, &m_bindingCache);
                break;
            case PrevRestirLuminance:
                m_CommonPasses->BlitTexture(m_commandList, framebuffer, m_renderTargets->PrevRestirLuminance, &m_bindingCache);
                break;
            case DiffuseConfidence:
                m_CommonPasses->BlitTexture(m_commandList, framebuffer, m_renderTargets->DiffuseConfidence, &m_bindingCache);
                break;
            case SpecularConfidence:
                m_CommonPasses->BlitTexture(m_commandList, framebuffer, m_renderTargets->SpecularConfidence, &m_bindingCache);
                break;
            case MotionVectors:
                m_CommonPasses->BlitTexture(m_commandList, framebuffer, m_renderTargets->MotionVectors, &m_bindingCache);
        }
        
        m_profiler->EndFrame(m_commandList);

        m_commandList->close();
        GetDevice()->executeCommandList(m_commandList);
        
        m_ui.gbufferSettings.enableMaterialReadback = false;
        
        if (m_ui.enableAnimations)
            m_framesSinceAnimation = 0;
        else
            m_framesSinceAnimation++;
        
        m_viewPrevious = m_view;
        m_previousViewValid = true;
        m_ui.resetAccumulation = false;
        ++m_renderFrameIndex;
    }

private:
    JavaVM *vm = nullptr;
    JNIEnv *env = nullptr;       /* pointer to native method interface */
    
    nvrhi::CommandListHandle m_commandList;

    nvrhi::BindingLayoutHandle m_bindlessLayout;

    std::shared_ptr<vfs::RootFileSystem> m_rootFs;
    std::shared_ptr<engine::ShaderFactory> m_shaderFactory;
    std::shared_ptr<SampleScene> m_scene;
    std::shared_ptr<engine::DescriptorTableManager> m_descriptorTableManager;
    std::unique_ptr<render::ToneMappingPass> m_toneMappingPass;
    std::shared_ptr<RenderTargets> m_renderTargets;
    app::FirstPersonCamera m_camera;
    engine::PlanarView m_view;
    engine::PlanarView m_viewPrevious;
    engine::PlanarView m_upscaledView;
    std::shared_ptr<engine::DirectionalLight> m_sunLight;
    std::shared_ptr<EnvironmentLight> m_environmentLight;
    std::shared_ptr<engine::LoadedTexture> m_environmentMap;
    engine::BindingCache m_bindingCache;

    std::unique_ptr<rtxdi::ImportanceSamplingContext> m_isContext;
    std::unique_ptr<RasterizedGBufferPass> m_rasterizedGBufferPass;
    std::unique_ptr<PostprocessGBufferPass> m_postprocessGBufferPass;
    std::unique_ptr<CompositingPass> m_compositingPass;
    std::unique_ptr<PrepareLightsPass> m_prepareLightsPass;
    std::unique_ptr<RenderEnvironmentMapPass> m_renderEnvironmentMapPass;
    std::unique_ptr<GenerateMipsPass> m_environmentMapPdfMipmapPass;
    std::unique_ptr<GenerateMipsPass> m_localLightPdfMipmapPass;
    std::unique_ptr<LightingPasses> m_lightingPasses;
    std::unique_ptr<RtxdiResources> m_rtxdiResources;
    std::unique_ptr<engine::IesProfileLoader> m_iesProfileLoader;
    std::shared_ptr<Profiler> m_profiler;

    uint32_t m_renderFrameIndex = 0;

    UIData& m_ui;
    uint m_framesSinceAnimation = 0;
    bool m_previousViewValid = false;
    time_point<steady_clock> m_previousFrameTimeStamp;

    std::vector<std::shared_ptr<engine::IesProfile>> m_iesProfiles;

    dm::float3 m_regirCenter;

    enum class FrameStepMode
    {
        Disabled,
        Wait,
        Step
    };

    FrameStepMode m_frameStepMode = FrameStepMode::Disabled;
};

#if defined(_WIN32) && !defined(IS_CONSOLE_APP)
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
#else
int main(int argc, char** argv)
#endif
{
    app::DeviceCreationParameters deviceParams;
    deviceParams.swapChainBufferCount = 3;
    deviceParams.enableRayTracingExtensions = true;
    deviceParams.backBufferWidth = 1280;
    deviceParams.backBufferHeight = 720;
    deviceParams.vsyncEnabled = true;
    deviceParams.infoLogSeverity = log::Severity::Debug;

    UIData ui;
    
    app::DeviceManager* deviceManager = app::DeviceManager::Create(nvrhi::GraphicsAPI::VULKAN);

#if DONUT_WITH_VULKAN
    // Set the extra device feature bit(s)
    deviceParams.deviceCreateInfoCallback = [](VkDeviceCreateInfo& info) {
        auto features = const_cast<VkPhysicalDeviceFeatures*>(info.pEnabledFeatures);
        features->fragmentStoresAndAtomics = VK_TRUE;
        };
#endif

    const char* apiString = nvrhi::utils::GraphicsAPIToString(deviceManager->GetGraphicsAPI());

    std::string windowTitle = std::string("RTXDI") + " (" + std::string(apiString) + ")";
    
    log::SetErrorMessageCaption(windowTitle.c_str());

#ifdef _WIN32
    // Disable Window scaling.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif

    if (!deviceManager->CreateWindowDeviceAndSwapChain(deviceParams, windowTitle.c_str()))
    {
        log::error("Cannot initialize a %s graphics device.", apiString);
        return 1;
    }

    bool rayPipelineSupported = deviceManager->GetDevice()->queryFeatureSupport(nvrhi::Feature::RayTracingPipeline);
    bool rayQuerySupported = deviceManager->GetDevice()->queryFeatureSupport(nvrhi::Feature::RayQuery);

    if (!rayPipelineSupported && !rayQuerySupported)
    {
        log::error("The GPU (%s) or its driver does not support ray tracing.", deviceManager->GetRendererString());
        return 1;
    }

    {
        SceneRenderer sceneRenderer(deviceManager, ui);
        if (sceneRenderer.Init())
        {
            UserInterface userInterface(deviceManager, *sceneRenderer.GetRootFs(), ui);
            userInterface.Init(sceneRenderer.GetShaderFactory());

            deviceManager->AddRenderPassToBack(&sceneRenderer);
            deviceManager->AddRenderPassToBack(&userInterface);
            deviceManager->RunMessageLoop();
            deviceManager->GetDevice()->waitForIdle();
            deviceManager->RemoveRenderPass(&sceneRenderer);
            deviceManager->RemoveRenderPass(&userInterface);
        }

        // Clear the shared pointers from 'ui' to graphics objects
        ui.resources.reset();
    }
    
    deviceManager->Shutdown();

    delete deviceManager;

    return g_ExitCode;
}
