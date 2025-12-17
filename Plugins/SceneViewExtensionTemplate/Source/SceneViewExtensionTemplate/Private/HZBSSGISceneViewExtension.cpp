#include "HZBSSGISceneViewExtension.h"

#include "PostProcess/PostProcessMaterialInputs.h"

IMPLEMENT_GLOBAL_SHADER(FHZBBuildCS, "/Plugins/SceneViewExtensionTemplate/HZB.usf", "HZBBuildCS", SF_Compute);

namespace
{
	TAutoConsoleVariable<int32> CVarHZBSSGIOn(
		TEXT("r.HZBSSGI"),
		0,
		TEXT("Enable HZB SSGI SceneViewExtension \n")
		TEXT(" 0: OFF;")
		TEXT(" 1: ON."),
		ECVF_RenderThreadSafe);
	// 添加可视化控制变量
	TAutoConsoleVariable<int32> CVarHZBSSGIVisualize(
		TEXT("r.HZBSSGI.Visualize"),
		-1,
		TEXT("Visualize HZB mip level \n")
		TEXT(" -1: OFF (normal rendering);")
		TEXT(" 0-N: Show specific mip level."),
		ECVF_RenderThreadSafe);
}

FHZBSSGISceneViewExtension::FHZBSSGISceneViewExtension(const FAutoRegister& AutoRegister) : FSceneViewExtensionBase(AutoRegister)
{
	UE_LOG(LogTemp, Log, TEXT("HZBSSGISceneViewExtension: Extension registered"));
}

void FHZBSSGISceneViewExtension::SubscribeToPostProcessingPass(EPostProcessingPass PassId, const FSceneView& View, FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
	// 临时换成 MotionBlur 来测试
	if (PassId == EPostProcessingPass::SSRInput)
	{
		InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(this,&FHZBSSGISceneViewExtension::HZBSSGIProcessPass));
	}
}


FScreenPassTexture FHZBSSGISceneViewExtension::HZBSSGIProcessPass(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
{
	
	// 检查是否启用
	if (CVarHZBSSGIOn.GetValueOnRenderThread() == 0)
	{
		return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	}
	
	
	const FSceneTextureShaderParameters& SceneTextures = Inputs.SceneTextures;
	FRDGTextureRef SceneDepth = nullptr;
	if (SceneTextures.SceneTextures)
	{
		TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextureUniformBuffer = SceneTextures.SceneTextures.GetUniformBuffer();
		if (SceneTextureUniformBuffer)
		{
			SceneDepth = SceneTextureUniformBuffer->GetParameters()->SceneDepthTexture;
		}
	}
	else if (SceneTextures.MobileSceneTextures)
	{
		TRDGUniformBufferRef<FMobileSceneTextureUniformParameters> MobileSceneTextureUniformBuffer = SceneTextures.MobileSceneTextures.GetUniformBuffer();
		if (MobileSceneTextureUniformBuffer)
		{
			SceneDepth = MobileSceneTextureUniformBuffer->GetParameters()->SceneDepthTexture;
		}
	}
	if (!SceneDepth)
	{
		return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	}
	
	RDG_EVENT_SCOPE(GraphBuilder, "HZBSSGI Generate HZB");
	
	auto SceneSize = SceneDepth->Desc.Extent;
	int32 NumMips = FMath::FloorLog2(FMath::Max(SceneSize.X,SceneSize.Y)) + 1;


	FRDGTextureDesc HZBDesc = FRDGTextureDesc::Create2D(
		SceneSize,
		PF_R32_FLOAT,
		FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV
	);
	HZBDesc.NumMips = NumMips;
	FRDGTextureRef HZBTexture = GraphBuilder.CreateTexture(HZBDesc, TEXT("HZB Texture"));

	// 使用 AddCopyTexturePass 代替 AddDrawTexturePass，它可以处理 UAV 纹理
	AddCopyTexturePass(
		GraphBuilder,
		SceneDepth,
		HZBTexture,
		FRHICopyTextureInfo()
	);
	
	UE::Math::TIntPoint<int32> CurrentInputSize = SceneSize;
	for (int32 MipLevel = 1; MipLevel < NumMips; MipLevel++)
	{
		UE::Math::TIntPoint<int32> NextOutputSize;
		NextOutputSize.X = FMath::Max(1,CurrentInputSize.X/2);
		NextOutputSize.Y = FMath::Max(1,CurrentInputSize.Y/2);

		FHZBBuildCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHZBBuildCS::FParameters>();

		FRDGTextureSRVRef InputSRV = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(HZBTexture,MipLevel - 1));
		PassParameters->InputDepthTexture = InputSRV;
		PassParameters->InputViewportMaxBound = FVector2f(CurrentInputSize.X-1,CurrentInputSize.Y-1);

		FRDGTextureUAVRef OutputUAV = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(HZBTexture,MipLevel));
		PassParameters->OutputDepthTexture = OutputUAV;
		PassParameters->OutputViewportSize = FVector2f(NextOutputSize.X,NextOutputSize.Y);

		TShaderMapRef<FHZBBuildCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		
		FIntVector GroupCount(
			FMath::DivideAndRoundUp(NextOutputSize.X,8),
			FMath::DivideAndRoundUp(NextOutputSize.Y,8),
			1);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("HZBSSGI Generate HZB Mip%d (%dx%d)",MipLevel,NextOutputSize.X,NextOutputSize.Y),
			ComputeShader,
			PassParameters,
			GroupCount
		);
		CurrentInputSize = NextOutputSize;
	}
	
	GraphBuilder.QueueTextureExtraction(HZBTexture, &ExtractedHZBTexture);

	// 可视化 HZB
	int32 VisualizeMipLevel = CVarHZBSSGIVisualize.GetValueOnRenderThread();
	if (VisualizeMipLevel >= 0)
	{
		VisualizeMipLevel = FMath::Clamp(VisualizeMipLevel, 0, NumMips - 1);
		
		const FScreenPassTexture& SceneColor = FScreenPassTexture::CopyFromSlice(GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
		if (SceneColor.IsValid())
		{
			// 创建 SRV 指向特定的 mip level
			FRDGTextureSRVRef HZBMipSRV = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(HZBTexture, VisualizeMipLevel));
			
			// 使用正确的 AddDrawTexturePass 重载
			AddDrawTexturePass(
				GraphBuilder,
				View,
				HZBMipSRV->Desc.Texture,
				SceneColor.Texture,
				FIntPoint::ZeroValue,
				FIntPoint::ZeroValue,
				SceneColor.ViewRect.Size()
			);
		}
	}
	
	return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
}
