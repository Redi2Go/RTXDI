set(shaders
   LightingPasses/DI/FusedResampling.hlsl
   LightingPasses/DI/GenerateInitialSamples.hlsl
   LightingPasses/DI/ShadeSamples.hlsl
   LightingPasses/DI/SpatialResampling.hlsl
   LightingPasses/DI/TemporalResampling.hlsl
   LightingPasses/GI/FinalShading.hlsl
   LightingPasses/GI/FusedResampling.hlsl
   LightingPasses/GI/SpatialResampling.hlsl
   LightingPasses/GI/TemporalResampling.hlsl
   LightingPasses/Presampling/PresampleEnvironmentMap.hlsl
   LightingPasses/Presampling/PresampleLights.hlsl
   LightingPasses/Presampling/PresampleReGIR.hlsl
   LightingPasses/BrdfRayTracing.hlsl
   LightingPasses/RtxdiApplicationBridge/RAB_Buffers.hlsli
   LightingPasses/RtxdiApplicationBridge/RAB_LightInfo.hlsli
   LightingPasses/RtxdiApplicationBridge/RAB_LightSample.hlsli
   LightingPasses/RtxdiApplicationBridge/RAB_LightSampling.hlsli
   LightingPasses/RtxdiApplicationBridge/RAB_Material.hlsli
   LightingPasses/RtxdiApplicationBridge/RAB_RandomSamplerState.hlsli
   LightingPasses/RtxdiApplicationBridge/RAB_RayPayload.hlsli
   LightingPasses/RtxdiApplicationBridge/RAB_RTShaders.hlsli
   LightingPasses/RtxdiApplicationBridge/RAB_SpatialHelpers.hlsli
   LightingPasses/RtxdiApplicationBridge/RAB_Surface.hlsli
   LightingPasses/RtxdiApplicationBridge/RAB_VisibilityTest.hlsli
   LightingPasses/RtxdiApplicationBridge/RtxdiApplicationBridge.hlsli
   LightingPasses/ShadeSecondarySurfaces.hlsl
   LightingPasses/ShadingHelpers.hlsli
   BRDFPTParameters.h
   CompositingPass.hlsl
   GBufferHelpers.hlsli
   HelperFunctions.hlsli
   Helperfunctions.hlsli
   LightShaping.hlsli
   PolymorphicLight.hlsli
   PostprocessGBuffer.hlsl
   PrepareLights.hlsl
   PreprocessEnvironmentMap.hlsl
   RasterizedGBuffer.hlsl
   RenderEnvironmentMap.hlsl
   SceneGeometry.hlsli
   ShaderParameters.h)

# Organize MSVS filters (the little folders in the solution explorer) to match the folder structure
foreach(source IN LISTS shaders)
    get_filename_component(source_path "${source}" PATH)
    string(REPLACE "/" "\\" source_path_msvc "${source_path}")
    source_group("${source_path_msvc}" FILES "${source}")
endforeach()

set(project FullSample)
set(folder "RTXDI SDK")
set(shaders_target FullSampleShaders)

add_custom_target(${shaders_target}
    SOURCES ${shaders} Shaders.cfg)

set (OUTPUT_PATH_BASE "${CMAKE_BINARY_DIR}/bin/shaders/full-sample")

 if (WIN32)
     set (USE_API_OPTION --useAPI)
 else()
     set (USE_API_OPTION "")
 endif()

set(DXC_SPIRV_PATH "" CACHE STRING "Path to dxc (SPIR-V capable)")
if(DXC_SPIRV_PATH STREQUAL "")
    message(FATAL_ERROR "DXC_SPIRV_PATH must be set (e.g. -DDXC_SPIRV_PATH=/path/to/dxc)")
endif()

set(SHADERMAKE_EXE_PATH "" CACHE STRING "Path to shadermake executable")
if(SHADERMAKE_EXE_PATH STREQUAL "")
    message(FATAL_ERROR "SHADERMAKE_EXE_PATH must be set (e.g. -DSHADERMAKE_EXE_PATH=/path/to/shadermake)")
endif()

set(VULKAN_COMPILER_OPTIONS
      --platform SPIRV
      --shaderModel 6_5
      --vulkanVersion 1.2
      --binaryBlob
      --outputExt .bin
      -I ${RTXDI_RUNTIME_INCLUDE_PATH}
      --relaxedInclude "../Types.h"
      ${USE_API_OPTION}
      -D SPIRV
      --compiler ${DXC_SPIRV_PATH})

   add_custom_command(TARGET ${shaders_target} PRE_BUILD
                     COMMAND ${SHADERMAKE_EXE_PATH}
                              --config ${CMAKE_CURRENT_SOURCE_DIR}/Shaders.cfg
                              --out ${OUTPUT_PATH_BASE}/spirv
                              ${VULKAN_COMPILER_OPTIONS})

set_target_properties(${shaders_target} PROPERTIES FOLDER ${folder})
set_source_files_properties(${shaders} PROPERTIES VS_TOOL_OVERRIDE "None") 
