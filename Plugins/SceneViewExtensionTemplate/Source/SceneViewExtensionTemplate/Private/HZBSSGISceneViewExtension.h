#pragma once
#include "SceneViewExtension.h"
#include "ShaderParameterStruct.h"
#include "SceneTextureParameters.h"

class FHZBSSGISceneViewExtension : public FSceneViewExtensionBase
{
public:
	FHZBSSGISceneViewExtension(const FAutoRegister& AutoRegister);
	
	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {};
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {};
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {};

	virtual void SubscribeToPostProcessingPass(EPostProcessingPass PassId, const FSceneView& View, FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override ;
	FScreenPassTexture HZBSSGIProcessPass(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs);
};

// HZB Mipmap Generation Shader
class SCENEVIEWEXTENSIONTEMPLATE_API FHZBBuildCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FHZBBuildCS)
	SHADER_USE_PARAMETER_STRUCT(FHZBBuildCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, InputDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutputDepthTexture)
		SHADER_PARAMETER(FVector2f, InputViewportMaxBound)
		SHADER_PARAMETER(FVector2f, OutputViewportSize)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADS_X"), 8);
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), 8);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
	}
};

// Main SSGI Shader
class SCENEVIEWEXTENSIONTEMPLATE_API FSSGICS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSSGICS);
	SHADER_USE_PARAMETER_STRUCT(FSSGICS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Textures
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HZBTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, SceneColorTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, InputSceneDepthTexture)
		
		// GBuffer Textures
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSGI_GBufferA)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSGI_GBufferB)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSGI_GBufferC)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSGI_GBufferD)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSGI_GBufferE)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSGI_GBufferF)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSGI_GBufferVelocity)

		// Settings
		SHADER_PARAMETER(FVector4f, HZBSize)
		SHADER_PARAMETER(int, MaxMipLevel)
		SHADER_PARAMETER(int, MaxIterations)
		SHADER_PARAMETER(float, Thickness)
		SHADER_PARAMETER(float, RayLength)
		SHADER_PARAMETER(float, Intensity)
		SHADER_PARAMETER(int, DebugMode)

		// Output
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, SSGI_Raw_Output)

		// Manual View Data (Fixed for consistency)
		SHADER_PARAMETER(FVector4f, ManualViewRectMin)
		SHADER_PARAMETER(FVector4f, ManualViewSizeAndInvSize)
		SHADER_PARAMETER(FVector4f, ManualBufferSizeAndInvSize)
		SHADER_PARAMETER(FMatrix44f, ManualSVPositionToTranslatedWorld)
		SHADER_PARAMETER(FMatrix44f, ManualTranslatedWorldToClip)
	
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADS_X"), 8);
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), 8);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
	}
};

// Composite Shader
class SCENEVIEWEXTENSIONTEMPLATE_API FSSGICompositeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSSGICompositeCS);
	SHADER_USE_PARAMETER_STRUCT(FSSGICompositeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSGIResultTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)
		SHADER_PARAMETER(FVector2f, ViewportSize)
		SHADER_PARAMETER(int, DebugMode)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADS_X"), 8);
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), 8);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
	}
};