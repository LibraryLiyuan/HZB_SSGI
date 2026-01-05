#include "HZBSSGISceneViewExtension.h"
#include "PixelShaderUtils.h"
#include "SystemTextures.h"
#include "PostProcess/PostProcessMaterialInputs.h"

IMPLEMENT_GLOBAL_SHADER(FHZBBuildCS, "/Plugins/SceneViewExtensionTemplate/HZB.usf", "HZBBuildCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSSGICS, "/Plugins/SceneViewExtensionTemplate/SSGI.usf", "SSGICS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSSGICompositeCS, "/Plugins/SceneViewExtensionTemplate/SSGIComposite.usf", "CompositeCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSSGIDenoiserCS, "/Plugins/SceneViewExtensionTemplate/SSGIDenoiser.usf", "DenoiserCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSSGITemporalCS, "/Plugins/SceneViewExtensionTemplate/SSGITemporal.usf", "TemporalCS", SF_Compute);

namespace
{
	TAutoConsoleVariable<int32> CVarHZBSSGIOn(
		TEXT("r.HZBSSGI"), 0, TEXT("Enable HZB SSGI SceneViewExtension"), ECVF_RenderThreadSafe);
	TAutoConsoleVariable<int32> CVarSSGIDebug(
		TEXT("r.HZBSSGI.Debug"), 0, TEXT("Debug mode for SSGI"), ECVF_RenderThreadSafe);
}

FHZBSSGISceneViewExtension::FHZBSSGISceneViewExtension(const FAutoRegister& AutoRegister) : FSceneViewExtensionBase(AutoRegister)
{
}

void FHZBSSGISceneViewExtension::SubscribeToPostProcessingPass(EPostProcessingPass PassId, const FSceneView& View, FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
    //UE_LOG(LogTemp, Warning, TEXT("Checking PassId: %d"), (int32)PassId);
	if (PassId == EPostProcessingPass::BeforeDOF)
	{
		//UE_LOG(LogTemp, Warning, TEXT(">> HZB SSGI Subscribed at SSRInput (Pre-TSR)!"));
		InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(this, &FHZBSSGISceneViewExtension::HZBSSGIProcessPass));
	}
}

FScreenPassTexture FHZBSSGISceneViewExtension::HZBSSGIProcessPass(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
{
	if (CVarHZBSSGIOn.GetValueOnRenderThread() == 0)
	{
		return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	}
	/**
	 * HZB Pass
	 */
	const FSceneTextureShaderParameters& SceneTextures = Inputs.SceneTextures;
	FRDGTextureRef SceneDepth = nullptr;

	if (SceneTextures.SceneTextures)
	{
		auto* UniformBuffer = SceneTextures.SceneTextures.GetUniformBuffer();
		if (UniformBuffer) SceneDepth = UniformBuffer->GetParameters()->SceneDepthTexture;
	}
	else if (SceneTextures.MobileSceneTextures)
	{
		auto* UniformBuffer = SceneTextures.MobileSceneTextures.GetUniformBuffer();
		if (UniformBuffer) SceneDepth = UniformBuffer->GetParameters()->SceneDepthTexture;
	}
	if (!SceneDepth)
	{
		return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	}
	
	RDG_EVENT_SCOPE(GraphBuilder, "HZBSSGI");
	// Buffer尺寸的Depth
	auto SceneSize = SceneDepth->Desc.Extent;
	int32 NumMips = FMath::FloorLog2(FMath::Max(SceneSize.X, SceneSize.Y)) + 1;

	FRDGTextureDesc HZBDesc = FRDGTextureDesc::Create2D(
		SceneSize, PF_R32_FLOAT, FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV | TexCreate_RenderTargetable
	);
	HZBDesc.NumMips = NumMips;
	FRDGTextureRef HZBTexture = GraphBuilder.CreateTexture(HZBDesc, TEXT("HZB Texture"));
	// 生成 Mip 0 (直接拷贝 SceneDepth)
	FScreenPassTexture SceneDepthInput(SceneDepth);
	FScreenPassRenderTarget HZBMip0(HZBTexture, ERenderTargetLoadAction::ENoAction);
	AddDrawTexturePass(GraphBuilder, View, SceneDepthInput, HZBMip0);

	UE::Math::TIntPoint<int32> CurrentInputSize = SceneSize;
	for (int32 MipLevel = 1; MipLevel < NumMips; MipLevel++)
	{
		UE::Math::TIntPoint<int32> NextOutputSize;
		NextOutputSize.X = FMath::Max(1, CurrentInputSize.X / 2);
		NextOutputSize.Y = FMath::Max(1, CurrentInputSize.Y / 2);

		FHZBBuildCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHZBBuildCS::FParameters>();
		// 上一级的SRV
		PassParameters->InputDepthTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(HZBTexture, MipLevel - 1));
		PassParameters->InputViewportMaxBound = FVector2f(CurrentInputSize.X - 1, CurrentInputSize.Y - 1);
		// 当前级的UAV
		PassParameters->OutputDepthTexture = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(HZBTexture, MipLevel));
		PassParameters->OutputViewportSize = FVector2f(NextOutputSize.X, NextOutputSize.Y);

		TShaderMapRef<FHZBBuildCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		// 由于使用的是Buffer尺寸，所以GroupCount也需要传入的是Buffer尺寸的
		FIntVector GroupCount(FMath::DivideAndRoundUp(NextOutputSize.X, 8), FMath::DivideAndRoundUp(NextOutputSize.Y, 8), 1);

		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("HZB Mip%d", MipLevel), ComputeShader, PassParameters, GroupCount);
		CurrentInputSize = NextOutputSize;
	}

	/**
	 * SSGI Trace Pass
	 */
	float SSGIIntensity = 1.0f;
	FScreenPassTextureSlice SceneColorSlice = Inputs.GetInput(EPostProcessMaterialInput::SceneColor);
	if (!SceneColorSlice.IsValid()) return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	// Buffer的大小和View的坐标映射
	FIntRect ViewRect = SceneColorSlice.ViewRect;
	FIntPoint ViewSize = ViewRect.Size();
	FIntPoint BufferSize = SceneDepth->Desc.Extent;
	ensure(ViewSize.X <= BufferSize.X && ViewSize.Y <= BufferSize.Y);
	FVector4f CommonViewRectMin = FVector4f(ViewRect.Min.X, ViewRect.Min.Y, 0.0f, 0.0f);
	FVector4f CommonViewSizeAndInvSize = FVector4f(
		ViewSize.X, ViewSize.Y, 
		1.0f / ViewSize.X, 1.0f / ViewSize.Y
	);
	FVector4f CommonBufferSizeAndInvSize = FVector4f(
		BufferSize.X, BufferSize.Y, 
		1.0f / BufferSize.X, 1.0f / BufferSize.Y
	);
	FMatrix44f MatSVPosToWorld = FMatrix44f(View.ViewMatrices.GetInvTranslatedViewProjectionMatrix());
	FMatrix44f MatWorldToClip = FMatrix44f(View.ViewMatrices.GetTranslatedViewProjectionMatrix());
	
	FRDGTextureDesc SSGIOutputDesc = FRDGTextureDesc::Create2D(
		ViewSize,
		PF_FloatRGBA, FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_UAV
	);
	FRDGTextureRef SSGIOutputTexture = GraphBuilder.CreateTexture(SSGIOutputDesc, TEXT("SSGI_Raw_Output"));
	auto SceneTexturesParams = CreateSceneTextureUniformBuffer(GraphBuilder, View);
	{
		TShaderMapRef<FSSGICS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FSSGICS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSSGICS::FParameters>();

		PassParameters->HZBTexture = HZBTexture;
		PassParameters->SceneColorTexture = SceneColorSlice.TextureSRV;
		PassParameters->InputSceneDepthTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SceneDepth));

		FRDGTextureRef Dummy = GSystemTextures.GetBlackDummy(GraphBuilder);
		auto& StParams = SceneTexturesParams->GetParameters();
		PassParameters->SSGI_GBufferA = StParams->GBufferATexture ? StParams->GBufferATexture : Dummy;
		PassParameters->SSGI_GBufferB = StParams->GBufferBTexture ? StParams->GBufferBTexture : Dummy;
		PassParameters->SSGI_GBufferC = StParams->GBufferCTexture ? StParams->GBufferCTexture : Dummy;

		PassParameters->HZBSize = FVector4f(HZBTexture->Desc.Extent.X, HZBTexture->Desc.Extent.Y, 1.0f / HZBTexture->Desc.Extent.X, 1.0f / HZBTexture->Desc.Extent.Y);
		PassParameters->MaxMipLevel = NumMips - 1;
		PassParameters->MaxIterations = 64;
		PassParameters->Thickness = 10.0f;
		PassParameters->RayLength = 100.0f;
		PassParameters->Intensity = SSGIIntensity;
		// 针对后续的Temporal Pass添加的FrameIndex 用于产生随机噪点种子
		uint32 FrameIndex = View.Family->FrameNumber % 1024; 
		PassParameters->FrameIndex = (int)FrameIndex;
		
		PassParameters->SSGI_Raw_Output = GraphBuilder.CreateUAV(SSGIOutputTexture);
		PassParameters->View = View.ViewUniformBuffer;

		PassParameters->ViewRectMin = CommonViewRectMin;
		PassParameters->ViewSizeAndInvSize = CommonViewSizeAndInvSize;
		PassParameters->BufferSizeAndInvSize = CommonBufferSizeAndInvSize;
        
		PassParameters->SVPositionToTranslatedWorld = MatSVPosToWorld;
		PassParameters->TranslatedWorldToClip = MatWorldToClip;
		// 由于使用的是View尺寸，所以GroupCount也需要传入的是View尺寸的
		FIntVector GroupCount(FMath::DivideAndRoundUp(ViewSize.X, 8), FMath::DivideAndRoundUp(ViewSize.Y, 8), 1);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SSGI Trace"), ComputeShader, PassParameters, GroupCount);
	}

	/**
	 * Denoise Pass
	 */
    FRDGTextureDesc DenoiseDesc = SSGIOutputTexture->Desc;
    FRDGTextureRef DenoisedTexture = GraphBuilder.CreateTexture(DenoiseDesc, TEXT("SSGI_Denoised"));
    {
        TShaderMapRef<FSSGIDenoiserCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FSSGIDenoiserCS::FParameters* DenoiserParams = GraphBuilder.AllocParameters<FSSGIDenoiserCS::FParameters>();

        DenoiserParams->SSGIInputTexture = SSGIOutputTexture;
        
        auto& StParams = SceneTexturesParams->GetParameters();
        FRDGTextureRef Dummy = GSystemTextures.GetBlackDummy(GraphBuilder);
        DenoiserParams->SceneDepthTexture = StParams->SceneDepthTexture ? StParams->SceneDepthTexture : Dummy;
        DenoiserParams->GBufferATexture = StParams->GBufferATexture ? StParams->GBufferATexture : Dummy;
        DenoiserParams->SSGIDenoiseOutput = GraphBuilder.CreateUAV(DenoisedTexture);
        
		DenoiserParams->ViewSizeAndInvSize = CommonViewSizeAndInvSize; // 注意名字变了
		DenoiserParams->BufferSizeAndInvSize = CommonBufferSizeAndInvSize;
		DenoiserParams->ViewRectMin = CommonViewRectMin;
		DenoiserParams->Intensity = SSGIIntensity;

		FIntVector GroupCount(FMath::DivideAndRoundUp(ViewSize.X, 8), FMath::DivideAndRoundUp(ViewSize.Y, 8), 1);
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SSGI Spatial Denoise"), ComputeShader, DenoiserParams, GroupCount);
    }
	
	/**
	 * Temporal Pass
	 */
    FRDGTextureRef TemporalOutputTexture = nullptr;
    {
        FRDGTextureRef HistoryTextureRef = nullptr;
        bool bIsFirstFrame = !HistoryRenderTarget.IsValid();
        
		FIntPoint CurrentViewSize = ViewSize;
        bool bSizeChanged = false;
		// 处理是否是第一帧
        if (!bIsFirstFrame)
        {
            FIntPoint HistorySize = HistoryRenderTarget->GetDesc().Extent;
            if (HistorySize != CurrentViewSize)
            {
                bSizeChanged = true;
                HistoryRenderTarget.SafeRelease();
            }
        }

        if (bIsFirstFrame || bSizeChanged)
        {
            HistoryTextureRef = GSystemTextures.GetBlackDummy(GraphBuilder);
        }
        else
        {
            HistoryTextureRef = GraphBuilder.RegisterExternalTexture(HistoryRenderTarget);
        }

        FRDGTextureDesc Desc = SSGIOutputTexture->Desc;
        TemporalOutputTexture = GraphBuilder.CreateTexture(Desc, TEXT("SSGI_Temporal_Output"));

        TShaderMapRef<FSSGITemporalCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FSSGITemporalCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSSGITemporalCS::FParameters>();

        PassParameters->CurrentFrameTexture = DenoisedTexture; 
        PassParameters->HistoryTexture = HistoryTextureRef;
        // 获取GBuffer里面存储的速度向量图
        auto& StParams = SceneTexturesParams->GetParameters();
        PassParameters->VelocityTexture = StParams->GBufferVelocityTexture ? StParams->GBufferVelocityTexture : GSystemTextures.GetBlackDummy(GraphBuilder);
        
        PassParameters->OutputTexture = GraphBuilder.CreateUAV(TemporalOutputTexture);
        
		PassParameters->ViewSizeAndInvSize = CommonViewSizeAndInvSize;
		PassParameters->BufferSizeAndInvSize = CommonBufferSizeAndInvSize;
		PassParameters->ViewRectMin = CommonViewRectMin;
        PassParameters->HistoryWeight = (bIsFirstFrame || bSizeChanged) ? 0.0f : 0.9f;
		// 使用双边插值采样，保证平滑
        PassParameters->BilinearSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

		FIntVector GroupCount(FMath::DivideAndRoundUp(ViewSize.X, 8), FMath::DivideAndRoundUp(ViewSize.Y, 8), 1);
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SSGI Temporal"), ComputeShader, PassParameters, GroupCount);
        
        GraphBuilder.QueueTextureExtraction(TemporalOutputTexture, &HistoryRenderTarget);
    }
	
	/**
	 * Composite Pass
	 */
	FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(GraphBuilder, SceneColorSlice);
	FRDGTextureDesc OutputDesc = SSGIOutputTexture->Desc;
	OutputDesc.Flags |= TexCreate_UAV;
	OutputDesc.Flags &= ~(TexCreate_RenderTargetable | TexCreate_FastVRAM);
	FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(OutputDesc, TEXT("SSGI_Composite_Output"));
	{
		TShaderMapRef<FSSGICompositeCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FSSGICompositeCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSSGICompositeCS::FParameters>();

		PassParameters->SceneColorTexture = SceneColor.Texture;
		int32 CurrentDebugMode = CVarSSGIDebug.GetValueOnRenderThread();
		FRDGTextureRef SelectedTexture = TemporalOutputTexture;
		if (CurrentDebugMode == 1)
		{
			// 查看SSGI Pass的结果
			SelectedTexture = SSGIOutputTexture;
		}
		else if (CurrentDebugMode == 2)
		{
			// 查看Denoise Pass的结果
			SelectedTexture = DenoisedTexture;
		}
		
		PassParameters->SSGIResultTexture = SelectedTexture;
		PassParameters->OutputTexture = GraphBuilder.CreateUAV(OutputTexture);
		PassParameters->ViewSizeAndInvSize = CommonViewSizeAndInvSize;
		PassParameters->BufferSizeAndInvSize = CommonBufferSizeAndInvSize;
		PassParameters->ViewRectMin = CommonViewRectMin;
		
		FIntVector GroupCount(FMath::DivideAndRoundUp(ViewSize.X, 8), FMath::DivideAndRoundUp(ViewSize.Y, 8), 1);
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SSGI Composite"), ComputeShader, PassParameters, GroupCount);
	}
	return FScreenPassTexture(OutputTexture, FIntRect(0, 0, ViewSize.X, ViewSize.Y));
}
