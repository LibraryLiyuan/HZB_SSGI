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

	// 1. 获取 SceneDepth
	const FSceneTextureShaderParameters& SceneTextures = Inputs.SceneTextures;
	FRDGTextureRef SceneDepth = nullptr;

	// 简化获取逻辑：优先从 SceneTextureUniformBuffer 获取
	if (SceneTextures.SceneTextures)
	{
		auto* UniformBuffer = SceneTextures.SceneTextures.GetUniformBuffer();
		if (UniformBuffer) SceneDepth = UniformBuffer->GetParameters()->SceneDepthTexture;
	}
	// Fallback for Mobile (Optional)
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

	// 2. 构建 HZB (Hi-Z Buffer)
	auto SceneSize = SceneDepth->Desc.Extent;
	int32 NumMips = FMath::FloorLog2(FMath::Max(SceneSize.X, SceneSize.Y)) + 1;

	FRDGTextureDesc HZBDesc = FRDGTextureDesc::Create2D(
		SceneSize, PF_R32_FLOAT, FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV | TexCreate_RenderTargetable
	);
	HZBDesc.NumMips = NumMips;
	FRDGTextureRef HZBTexture = GraphBuilder.CreateTexture(HZBDesc, TEXT("HZB Texture"));

	// Mip 0 Generation (Draw Pass)
	FScreenPassTexture SceneDepthInput(SceneDepth);
	FScreenPassRenderTarget HZBMip0(HZBTexture, ERenderTargetLoadAction::ENoAction);
	AddDrawTexturePass(GraphBuilder, View, SceneDepthInput, HZBMip0);

	// Mip 1..N Generation (Compute Pass)
	UE::Math::TIntPoint<int32> CurrentInputSize = SceneSize;
	for (int32 MipLevel = 1; MipLevel < NumMips; MipLevel++)
	{
		UE::Math::TIntPoint<int32> NextOutputSize;
		NextOutputSize.X = FMath::Max(1, CurrentInputSize.X / 2);
		NextOutputSize.Y = FMath::Max(1, CurrentInputSize.Y / 2);

		FHZBBuildCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHZBBuildCS::FParameters>();
		PassParameters->InputDepthTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(HZBTexture, MipLevel - 1));
		PassParameters->InputViewportMaxBound = FVector2f(CurrentInputSize.X - 1, CurrentInputSize.Y - 1);
		PassParameters->OutputDepthTexture = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(HZBTexture, MipLevel));
		PassParameters->OutputViewportSize = FVector2f(NextOutputSize.X, NextOutputSize.Y);

		TShaderMapRef<FHZBBuildCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FIntVector GroupCount(FMath::DivideAndRoundUp(NextOutputSize.X, 8), FMath::DivideAndRoundUp(NextOutputSize.Y, 8), 1);

		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("HZB Mip%d", MipLevel), ComputeShader, PassParameters, GroupCount);
		CurrentInputSize = NextOutputSize;
	}

	// 3. SSGI Trace Pass
	FScreenPassTextureSlice SceneColorSlice = Inputs.GetInput(EPostProcessMaterialInput::SceneColor);
	if (!SceneColorSlice.IsValid()) return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);

	FRDGTextureDesc SSGIOutputDesc = FRDGTextureDesc::Create2D(
		SceneColorSlice.ViewRect.Size(), PF_FloatRGBA, FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_UAV
	);
	FRDGTextureRef SSGIOutputTexture = GraphBuilder.CreateTexture(SSGIOutputDesc, TEXT("SSGI_Raw_Output"));
	auto SceneTexturesParams = CreateSceneTextureUniformBuffer(GraphBuilder, View);
	{
		TShaderMapRef<FSSGICS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FSSGICS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSSGICS::FParameters>();

		// Textures
		PassParameters->HZBTexture = HZBTexture;
		PassParameters->SceneColorTexture = SceneColorSlice.TextureSRV;
		PassParameters->InputSceneDepthTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SceneDepth));

		// GBuffer Binding
		FRDGTextureRef Dummy = GSystemTextures.GetBlackDummy(GraphBuilder);
		auto& StParams = SceneTexturesParams->GetParameters();
		PassParameters->SSGI_GBufferA = StParams->GBufferATexture ? StParams->GBufferATexture : Dummy;
		PassParameters->SSGI_GBufferB = StParams->GBufferBTexture ? StParams->GBufferBTexture : Dummy;
		PassParameters->SSGI_GBufferC = StParams->GBufferCTexture ? StParams->GBufferCTexture : Dummy;
		PassParameters->SSGI_GBufferD = StParams->GBufferDTexture ? StParams->GBufferDTexture : Dummy;
		PassParameters->SSGI_GBufferE = StParams->GBufferETexture ? StParams->GBufferETexture : Dummy;
		PassParameters->SSGI_GBufferF = StParams->GBufferFTexture ? StParams->GBufferFTexture : Dummy;
		PassParameters->SSGI_GBufferVelocity = StParams->GBufferVelocityTexture ? StParams->GBufferVelocityTexture : Dummy;

		// Settings
		PassParameters->HZBSize = FVector4f(HZBTexture->Desc.Extent.X, HZBTexture->Desc.Extent.Y, 1.0f / HZBTexture->Desc.Extent.X, 1.0f / HZBTexture->Desc.Extent.Y);
		PassParameters->MaxMipLevel = NumMips - 1;
		PassParameters->MaxIterations = 64;
		PassParameters->Thickness = 10.0f;
		PassParameters->RayLength = 100.0f; // 调整为更合理的长度
		PassParameters->Intensity = 5.0f;
		PassParameters->DebugMode = CVarSSGIDebug.GetValueOnRenderThread();
		// [新增] 获取帧序号并取模 (防止数值过大溢出)
		// View.Family->FrameNumber 是引擎全局帧数
		uint32 FrameIndex = View.Family->FrameNumber % 1024; 
		PassParameters->FrameIndex = (int)FrameIndex;
		
		PassParameters->SSGI_Raw_Output = GraphBuilder.CreateUAV(SSGIOutputTexture);
		PassParameters->View = View.ViewUniformBuffer;

		// Manual View Data
		PassParameters->ManualViewRectMin = FVector4f(SceneColorSlice.ViewRect.Min.X, SceneColorSlice.ViewRect.Min.Y, 0, 0);
		FVector2f ViewSize(SceneColorSlice.ViewRect.Width(), SceneColorSlice.ViewRect.Height());
		PassParameters->ManualViewSizeAndInvSize = FVector4f(ViewSize.X, ViewSize.Y, 1.0f / ViewSize.X, 1.0f / ViewSize.Y);
		FVector2f BufferSize(SceneDepth->Desc.Extent.X, SceneDepth->Desc.Extent.Y);
		PassParameters->ManualBufferSizeAndInvSize = FVector4f(BufferSize.X, BufferSize.Y, 1.0f / BufferSize.X, 1.0f / BufferSize.Y);
		PassParameters->ManualSVPositionToTranslatedWorld = FMatrix44f(View.ViewMatrices.GetInvTranslatedViewProjectionMatrix());
		PassParameters->ManualTranslatedWorldToClip = FMatrix44f(View.ViewMatrices.GetTranslatedViewProjectionMatrix());

		FIntVector GroupCount(FMath::DivideAndRoundUp(SceneColorSlice.ViewRect.Width(), 8), FMath::DivideAndRoundUp(SceneColorSlice.ViewRect.Height(), 8), 1);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SSGI Trace"), ComputeShader, PassParameters, GroupCount);
	}

	// Temporal Pass
	FRDGTextureRef TemporalOutputTexture = nullptr;
    {
        // 1. 准备历史纹理
        FRDGTextureRef HistoryTextureRef = nullptr;
        bool bIsFirstFrame = !HistoryRenderTarget.IsValid();

        // 如果历史纹理不存在（第一帧）或尺寸不匹配，就用黑图代替
        if (bIsFirstFrame || HistoryRenderTarget->GetDesc().Extent != SSGIOutputTexture->Desc.Extent)
        {
            HistoryTextureRef = GSystemTextures.GetBlackDummy(GraphBuilder);
        }
        else
        {
            // 将上一帧保存的 RenderTarget 注册回 RDG
            HistoryTextureRef = GraphBuilder.RegisterExternalTexture(HistoryRenderTarget);
        }

        // 2. 创建这一帧的输出纹理 (也是下一帧的历史)
        FRDGTextureDesc Desc = SSGIOutputTexture->Desc;
        TemporalOutputTexture = GraphBuilder.CreateTexture(Desc, TEXT("SSGI_Temporal_Output"));

        // 3. 设置参数
        TShaderMapRef<FSSGITemporalCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FSSGITemporalCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSSGITemporalCS::FParameters>();

        PassParameters->CurrentFrameTexture = SSGIOutputTexture; // 输入：Raw Trace
        PassParameters->HistoryTexture = HistoryTextureRef;      // 输入：History
        
        // 获取 Velocity (从 SceneTextures)
        auto& StParams = SceneTexturesParams->GetParameters();
        PassParameters->VelocityTexture = StParams->GBufferVelocityTexture ? StParams->GBufferVelocityTexture : GSystemTextures.GetBlackDummy(GraphBuilder);

        PassParameters->OutputTexture = GraphBuilder.CreateUAV(TemporalOutputTexture); // 输出
        
        FVector2f ViewSize(SceneColorSlice.ViewRect.Width(), SceneColorSlice.ViewRect.Height());
        PassParameters->ViewportSize = FVector4f(ViewSize.X, ViewSize.Y, 1.0f/ViewSize.X, 1.0f/ViewSize.Y);
        
        // 权重：0.9 表示 90% 来自历史，画面会非常平滑。
        // 如果拖影太严重，可以降到 0.8
        PassParameters->HistoryWeight = bIsFirstFrame ? 0.0f : 0.9f; 
        PassParameters->BilinearSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

        FIntVector GroupCount(
            FMath::DivideAndRoundUp(SceneColorSlice.ViewRect.Width(), 8),
            FMath::DivideAndRoundUp(SceneColorSlice.ViewRect.Height(), 8),
            1
        );

        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("SSGI Temporal"),
            ComputeShader,
            PassParameters,
            GroupCount
        );
        
        // 4. [关键] 提取结果，保存为下一帧的历史
        // 注意：这里我们提取 TemporalOutputTexture，作为下一帧的 History
        GraphBuilder.QueueTextureExtraction(TemporalOutputTexture, &HistoryRenderTarget);
    }
	
	// Bilateral Denoise Pass

	// 1. 创建降噪后的纹理
	FRDGTextureDesc DenoiseDesc = SSGIOutputTexture->Desc;
	FRDGTextureRef DenoisedTexture = GraphBuilder.CreateTexture(DenoiseDesc, TEXT("SSGI_Denoised"));

	// 2. 设置并执行 Denoise Pass
	{
		TShaderMapRef<FSSGIDenoiserCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FSSGIDenoiserCS::FParameters* DenoiserParams = GraphBuilder.AllocParameters<FSSGIDenoiserCS::FParameters>();

		// 输入：刚刚算出来的噪点图
		DenoiserParams->SSGIInputTexture = TemporalOutputTexture;

		// 输入：辅助 GBuffer (用于保边)
		// 这里的 SceneTexturesParams 是你在 Trace Pass 里创建的那个 CreateSceneTextureUniformBuffer
		auto& StParams = SceneTexturesParams->GetParameters();
		FRDGTextureRef Dummy = GSystemTextures.GetBlackDummy(GraphBuilder);

		DenoiserParams->SceneDepthTexture = StParams->SceneDepthTexture ? StParams->SceneDepthTexture : Dummy;
		DenoiserParams->GBufferATexture = StParams->GBufferATexture ? StParams->GBufferATexture : Dummy;

		// 输出
		DenoiserParams->SSGIDenoiseOutput = GraphBuilder.CreateUAV(DenoisedTexture);

		// 尺寸
		FVector2f ViewSize(SceneColorSlice.ViewRect.Width(), SceneColorSlice.ViewRect.Height());
		DenoiserParams->ViewportSize = FVector4f(ViewSize.X, ViewSize.Y, 1.0f / ViewSize.X, 1.0f / ViewSize.Y);

		FIntVector GroupCount(
			FMath::DivideAndRoundUp(SceneColorSlice.ViewRect.Width(), 8),
			FMath::DivideAndRoundUp(SceneColorSlice.ViewRect.Height(), 8),
			1
		);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("SSGI Spatial Denoise"),
			ComputeShader,
			DenoiserParams,
			GroupCount
		);
	}
	// ===================================================================

	// 4. Composite Pass
	FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(GraphBuilder, SceneColorSlice);
	FRDGTextureDesc OutputDesc = SceneColor.Texture->Desc;
	OutputDesc.Flags |= TexCreate_UAV;
	OutputDesc.Flags &= ~(TexCreate_RenderTargetable | TexCreate_FastVRAM);
	FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(OutputDesc, TEXT("SSGI_Composite_Output"));

	{
		TShaderMapRef<FSSGICompositeCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FSSGICompositeCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSSGICompositeCS::FParameters>();

		PassParameters->SceneColorTexture = SceneColor.Texture;
		PassParameters->SSGIResultTexture = TemporalOutputTexture;
		PassParameters->OutputTexture = GraphBuilder.CreateUAV(OutputTexture);
		PassParameters->ViewportSize = FVector2f(SceneColor.ViewRect.Width(), SceneColor.ViewRect.Height());
		PassParameters->DebugMode = CVarSSGIDebug.GetValueOnRenderThread();

		FIntVector GroupCount(FMath::DivideAndRoundUp(SceneColor.ViewRect.Width(), 8), FMath::DivideAndRoundUp(SceneColor.ViewRect.Height(), 8), 1);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SSGI Composite"), ComputeShader, PassParameters, GroupCount);
	}

	AddCopyTexturePass(GraphBuilder, OutputTexture, SceneColor.Texture);
	return SceneColor;
}
