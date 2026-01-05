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
private:
	TRefCountPtr<IPooledRenderTarget> HistoryRenderTarget;
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

		// Settings
		SHADER_PARAMETER(FVector4f, HZBSize)
		SHADER_PARAMETER(int, MaxMipLevel)
		SHADER_PARAMETER(int, MaxIterations)
		SHADER_PARAMETER(float, Thickness)
		SHADER_PARAMETER(float, RayLength)
		SHADER_PARAMETER(float, Intensity)
		//SHADER_PARAMETER(int, DebugMode)
		SHADER_PARAMETER(int, FrameIndex)
	
		// Output
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, SSGI_Raw_Output)

		// Manual View Data (Fixed for consistency)
		SHADER_PARAMETER(FVector4f, ViewRectMin)
		SHADER_PARAMETER(FVector4f, ViewSizeAndInvSize)
		SHADER_PARAMETER(FVector4f, BufferSizeAndInvSize)
		SHADER_PARAMETER(FMatrix44f, SVPositionToTranslatedWorld)
		SHADER_PARAMETER(FMatrix44f, TranslatedWorldToClip)
	
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
		SHADER_PARAMETER(FVector4f, ViewSizeAndInvSize)
		SHADER_PARAMETER(FVector4f, BufferSizeAndInvSize)
		SHADER_PARAMETER(FVector4f, ViewRectMin)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADS_X"), 8);
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), 8);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
	}
};
class SCENEVIEWEXTENSIONTEMPLATE_API FSSGIDenoiserCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSSGIDenoiserCS);
	SHADER_USE_PARAMETER_STRUCT(FSSGIDenoiserCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SSGIInputTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, GBufferATexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, SSGIDenoiseOutput)
		SHADER_PARAMETER(FVector4f, ViewSizeAndInvSize)
		SHADER_PARAMETER(FVector4f, BufferSizeAndInvSize)
		SHADER_PARAMETER(FVector4f, ViewRectMin)
		SHADER_PARAMETER(float, Intensity)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADS_X"), 8);
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), 8);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
	}
};
class SCENEVIEWEXTENSIONTEMPLATE_API FSSGITemporalCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSSGITemporalCS);
	SHADER_USE_PARAMETER_STRUCT(FSSGITemporalCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CurrentFrameTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HistoryTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, VelocityTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)
		SHADER_PARAMETER(FVector4f, ViewSizeAndInvSize)
		SHADER_PARAMETER(FVector4f, BufferSizeAndInvSize)
		SHADER_PARAMETER(FVector4f, ViewRectMin)
		SHADER_PARAMETER(float, HistoryWeight)
		SHADER_PARAMETER_SAMPLER(SamplerState, BilinearSampler)
	END_SHADER_PARAMETER_STRUCT()

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADS_X"), 8);
		OutEnvironment.SetDefine(TEXT("THREADS_Y"), 8);
		OutEnvironment.SetDefine(TEXT("THREADS_Z"), 1);
	}
};