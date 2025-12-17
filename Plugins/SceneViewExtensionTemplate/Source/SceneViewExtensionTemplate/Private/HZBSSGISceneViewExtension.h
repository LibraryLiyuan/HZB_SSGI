#pragma once
#include "SceneViewExtension.h"
#include "ShaderParameterStruct.h"

class FHZBSSGISceneViewExtension : public FSceneViewExtensionBase
{
public:
	FHZBSSGISceneViewExtension(const FAutoRegister& AutoRegister);
	
public:
	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {};
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {};
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {};

	virtual void SubscribeToPostProcessingPass(EPostProcessingPass PassId, const FSceneView& View, FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override ;
	FScreenPassTexture HZBSSGIProcessPass(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs);
private:
	TRefCountPtr<IPooledRenderTarget> ExtractedHZBTexture;
};

class SCENEVIEWEXTENSIONTEMPLATE_API FHZBBuildCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FHZBBuildCS)

	SHADER_USE_PARAMETER_STRUCT(FHZBBuildCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D,InputDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>,OutputDepthTexture)
		SHADER_PARAMETER(FVector2f,InputViewportMaxBound)
		SHADER_PARAMETER(FVector2f,OutputViewportSize)
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