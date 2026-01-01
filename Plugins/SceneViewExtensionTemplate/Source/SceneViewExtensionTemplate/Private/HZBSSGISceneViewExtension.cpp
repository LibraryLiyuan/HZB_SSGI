#include "HZBSSGISceneViewExtension.h"
#include "PixelShaderUtils.h"
#include "SystemTextures.h"
#include "PostProcess/PostProcessMaterialInputs.h"

// -----------------------------------------------------------------------------
// [Shader 绑定]
// 将 C++ 类与 USF 文件中的 Shader 入口点绑定
// -----------------------------------------------------------------------------
IMPLEMENT_GLOBAL_SHADER(FHZBBuildCS, "/Plugins/SceneViewExtensionTemplate/HZB.usf", "HZBBuildCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSSGICS, "/Plugins/SceneViewExtensionTemplate/SSGI.usf", "SSGICS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSSGICompositeCS, "/Plugins/SceneViewExtensionTemplate/SSGIComposite.usf", "CompositeCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSSGIDenoiserCS, "/Plugins/SceneViewExtensionTemplate/SSGIDenoiser.usf", "DenoiserCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSSGITemporalCS, "/Plugins/SceneViewExtensionTemplate/SSGITemporal.usf", "TemporalCS", SF_Compute);

// -----------------------------------------------------------------------------
// [控制台变量 (CVars)]
// 允许在运行时通过控制台 (~键) 动态开启/关闭功能或切换调试模式
// -----------------------------------------------------------------------------
namespace
{
	// r.HZBSSGI 1: 开启, 0: 关闭
	TAutoConsoleVariable<int32> CVarHZBSSGIOn(
		TEXT("r.HZBSSGI"), 0, TEXT("Enable HZB SSGI SceneViewExtension"), ECVF_RenderThreadSafe);
	
	// r.HZBSSGI.Debug 
	// 0: 最终结果 (Temporal)
	// 1: 原始光追结果 (Raw Trace)
	// 2: 空间降噪结果 (Spatial Denoise)
	TAutoConsoleVariable<int32> CVarSSGIDebug(
		TEXT("r.HZBSSGI.Debug"), 0, TEXT("Debug mode for SSGI"), ECVF_RenderThreadSafe);
}

FHZBSSGISceneViewExtension::FHZBSSGISceneViewExtension(const FAutoRegister& AutoRegister) : FSceneViewExtensionBase(AutoRegister)
{
}

// -----------------------------------------------------------------------------
// [挂载点订阅]
// 告诉引擎我们想在渲染管线的哪个阶段插入代码
// -----------------------------------------------------------------------------
void FHZBSSGISceneViewExtension::SubscribeToPostProcessingPass(EPostProcessingPass PassId, const FSceneView& View, FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
    // EPostProcessingPass::BeforeDOF (景深之前) 是大多数 SSGI/SSAO 效果的标准插入点。
    // 因为 GI 应该被景深模糊，且此时 SceneColor 已经包含了不透明物体的光照。
    // 注意：此时 TSR (Temporal Super Resolution) 还没运行，所以我们是在 Render Resolution (低分) 下计算的，性能更好。
	if (PassId == EPostProcessingPass::BeforeDOF)
	{
		// 绑定回调函数 HZBSSGIProcessPass
		InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(this, &FHZBSSGISceneViewExtension::HZBSSGIProcessPass));
	}
}

// -----------------------------------------------------------------------------
// [核心处理逻辑]
// 这里是每一帧渲染时实际执行的代码 (Render Thread)
// -----------------------------------------------------------------------------
FScreenPassTexture FHZBSSGISceneViewExtension::HZBSSGIProcessPass(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
{
	// 检查总开关
	if (CVarHZBSSGIOn.GetValueOnRenderThread() == 0)
	{
		// 如果关闭，直接把输入的 SceneColor 原样返回，不做任何处理
		return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	}

	// =========================================================================
	// 1. 资源准备 (SceneDepth)
	// =========================================================================
	const FSceneTextureShaderParameters& SceneTextures = Inputs.SceneTextures;
	FRDGTextureRef SceneDepth = nullptr;

	// 获取场景深度纹理。UE5 现在的架构推荐从 UniformBuffer 获取。
	if (SceneTextures.SceneTextures)
	{
		auto* UniformBuffer = SceneTextures.SceneTextures.GetUniformBuffer();
		if (UniformBuffer) SceneDepth = UniformBuffer->GetParameters()->SceneDepthTexture;
	}
	// 移动端兼容 (可选)
	else if (SceneTextures.MobileSceneTextures)
	{
		auto* UniformBuffer = SceneTextures.MobileSceneTextures.GetUniformBuffer();
		if (UniformBuffer) SceneDepth = UniformBuffer->GetParameters()->SceneDepthTexture;
	}

	if (!SceneDepth)
	{
		return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	}

	// 开启 RDG Event Scope，方便在 RenderDoc/Insights 中调试查看
	RDG_EVENT_SCOPE(GraphBuilder, "HZBSSGI");

	// =========================================================================
	// 2. 构建 HZB (Hi-Z Buffer)
	// -------------------------------------------------------------------------
	// 目标：创建一个包含完整 Mipmap 链的深度纹理。
	// Mip 0 = 原始深度, Mip N = 降采样后的最大深度 (Reverse-Z 下是最小值)
	// =========================================================================
	auto SceneSize = SceneDepth->Desc.Extent;
	// 计算需要的 Mip 层级数 (Log2)
	int32 NumMips = FMath::FloorLog2(FMath::Max(SceneSize.X, SceneSize.Y)) + 1;

	FRDGTextureDesc HZBDesc = FRDGTextureDesc::Create2D(
		SceneSize, PF_R32_FLOAT, FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV | TexCreate_RenderTargetable
	);
	HZBDesc.NumMips = NumMips;
	FRDGTextureRef HZBTexture = GraphBuilder.CreateTexture(HZBDesc, TEXT("HZB Texture"));

	// Step 2.1: Mip 0 生成 (直接拷贝 SceneDepth)
	// 使用 DrawPass (Pixel Shader) 进行拷贝比 Compute Shader 兼容性更好
	FScreenPassTexture SceneDepthInput(SceneDepth);
	FScreenPassRenderTarget HZBMip0(HZBTexture, ERenderTargetLoadAction::ENoAction);
	AddDrawTexturePass(GraphBuilder, View, SceneDepthInput, HZBMip0);

	// Step 2.2: Mip 1..N 生成 (Compute Shader 降采样)
	UE::Math::TIntPoint<int32> CurrentInputSize = SceneSize;
	for (int32 MipLevel = 1; MipLevel < NumMips; MipLevel++)
	{
		UE::Math::TIntPoint<int32> NextOutputSize;
		NextOutputSize.X = FMath::Max(1, CurrentInputSize.X / 2);
		NextOutputSize.Y = FMath::Max(1, CurrentInputSize.Y / 2);

		FHZBBuildCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHZBBuildCS::FParameters>();
		// 输入：上一级 Mip (SRV)
		PassParameters->InputDepthTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(HZBTexture, MipLevel - 1));
		PassParameters->InputViewportMaxBound = FVector2f(CurrentInputSize.X - 1, CurrentInputSize.Y - 1);
		// 输出：当前级 Mip (UAV)
		PassParameters->OutputDepthTexture = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(HZBTexture, MipLevel));
		PassParameters->OutputViewportSize = FVector2f(NextOutputSize.X, NextOutputSize.Y);

		TShaderMapRef<FHZBBuildCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		// 线程组计算：每组 8x8 线程
		FIntVector GroupCount(FMath::DivideAndRoundUp(NextOutputSize.X, 8), FMath::DivideAndRoundUp(NextOutputSize.Y, 8), 1);

		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("HZB Mip%d", MipLevel), ComputeShader, PassParameters, GroupCount);
		CurrentInputSize = NextOutputSize;
	}
	
	// =========================================================================
	// [关键] 统一坐标系参数计算
	// -------------------------------------------------------------------------
	// 解决编辑器下 SceneColor 比 Viewport 大导致的 UV 错位问题。
	// ViewRect: 视口在 Buffer 中的位置 (例如 0,0 到 1920,1080)
	// BufferSize: 纹理实际大小 (可能也是 1920,1080，也可能更大)
	// =========================================================================
	FIntRect ViewRect = View.UnconstrainedViewRect;
	FIntPoint ViewSize = ViewRect.Size();
	FIntPoint BufferSize = SceneDepth->Desc.Extent;

	// 1. ViewRectMin (左上角偏移)
	FVector4f CommonViewRectMin = FVector4f(ViewRect.Min.X, ViewRect.Min.Y, 0.0f, 0.0f);

	// 2. ViewSizeAndInvSize (视口尺寸及倒数)
	FVector4f CommonViewSizeAndInvSize = FVector4f(
		ViewSize.X, ViewSize.Y, 
		1.0f / ViewSize.X, 1.0f / ViewSize.Y
	);

	// 3. BufferSizeAndInvSize (全局缓冲尺寸及倒数)
	FVector4f CommonBufferSizeAndInvSize = FVector4f(
		BufferSize.X, BufferSize.Y, 
		1.0f / BufferSize.X, 1.0f / BufferSize.Y
	);

	// 4. 矩阵：用于光线追踪时的坐标重构
	// Screen Position (NDC) -> Translated World Space
	FMatrix44f MatSVPosToWorld = FMatrix44f(View.ViewMatrices.GetInvTranslatedViewProjectionMatrix());
	FMatrix44f MatWorldToClip = FMatrix44f(View.ViewMatrices.GetTranslatedViewProjectionMatrix());

	// =========================================================================
	// 3. SSGI Trace Pass (光线追踪)
	// =========================================================================
	float SSGIIntensity = 1.0f;
	// SceneColorSlice 包含了 SceneColor 纹理以及它对应的 ViewRect 信息
	FScreenPassTextureSlice SceneColorSlice = Inputs.GetInput(EPostProcessMaterialInput::SceneColor);
	if (!SceneColorSlice.IsValid()) return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);

	// 创建输出纹理 (Raw Output)
	FRDGTextureDesc SSGIOutputDesc = FRDGTextureDesc::Create2D(
		SceneColorSlice.ViewRect.Size(), PF_FloatRGBA, FClearValueBinding::Black,
		TexCreate_ShaderResource | TexCreate_UAV
	);
	FRDGTextureRef SSGIOutputTexture = GraphBuilder.CreateTexture(SSGIOutputDesc, TEXT("SSGI_Raw_Output"));
	
	// 获取 GBuffer 绑定所需的 Uniform Buffer
	auto SceneTexturesParams = CreateSceneTextureUniformBuffer(GraphBuilder, View);
	{
		TShaderMapRef<FSSGICS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FSSGICS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSSGICS::FParameters>();

		// 绑定纹理
		PassParameters->HZBTexture = HZBTexture;
		PassParameters->SceneColorTexture = SceneColorSlice.TextureSRV;
		PassParameters->InputSceneDepthTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SceneDepth));

		// 绑定 GBuffer (如果没有则绑定黑图 Dummy)
		FRDGTextureRef Dummy = GSystemTextures.GetBlackDummy(GraphBuilder);
		auto& StParams = SceneTexturesParams->GetParameters();
		PassParameters->SSGI_GBufferA = StParams->GBufferATexture ? StParams->GBufferATexture : Dummy;
		PassParameters->SSGI_GBufferVelocity = StParams->GBufferVelocityTexture ? StParams->GBufferVelocityTexture : Dummy;

		// 绑定参数
		PassParameters->HZBSize = FVector4f(HZBTexture->Desc.Extent.X, HZBTexture->Desc.Extent.Y, 1.0f / HZBTexture->Desc.Extent.X, 1.0f / HZBTexture->Desc.Extent.Y);
		PassParameters->MaxMipLevel = NumMips - 1;
		PassParameters->MaxIterations = 64;   // HZB 追踪最大步数
		PassParameters->Thickness = 10.0f;    // 表面厚度 (cm)
		PassParameters->RayLength = 100.0f;   // 光线最大长度 (cm)
		PassParameters->Intensity = SSGIIntensity;
		
		// FrameIndex 用于 Temporal Dithering (让噪点随时间变化)
		uint32 FrameIndex = View.Family->FrameNumber % 1024; 
		PassParameters->FrameIndex = (int)FrameIndex;
		
		PassParameters->SSGI_Raw_Output = GraphBuilder.CreateUAV(SSGIOutputTexture);
		PassParameters->View = View.ViewUniformBuffer;

		// 传入前面计算好的统一坐标系参数
		PassParameters->ViewRectMin = CommonViewRectMin;
		PassParameters->ViewSizeAndInvSize = CommonViewSizeAndInvSize;
		PassParameters->BufferSizeAndInvSize = CommonBufferSizeAndInvSize;
        
		PassParameters->SVPositionToTranslatedWorld = MatSVPosToWorld;
		PassParameters->TranslatedWorldToClip = MatWorldToClip;

		// 添加 Trace Pass
		FIntVector GroupCount(FMath::DivideAndRoundUp(SceneColorSlice.ViewRect.Width(), 8), FMath::DivideAndRoundUp(SceneColorSlice.ViewRect.Height(), 8), 1);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SSGI Trace"), ComputeShader, PassParameters, GroupCount);
	}
	
	// =========================================================================
    // 4. Spatial Denoise (空间降噪)
    // -------------------------------------------------------------------------
	// 使用联合双边滤波 (Joint Bilateral Filter) 对 Raw Output 进行单帧内的平滑。
    // =========================================================================
    FRDGTextureDesc DenoiseDesc = SSGIOutputTexture->Desc;
    FRDGTextureRef DenoisedTexture = GraphBuilder.CreateTexture(DenoiseDesc, TEXT("SSGI_Denoised"));

    {
        TShaderMapRef<FSSGIDenoiserCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FSSGIDenoiserCS::FParameters* DenoiserParams = GraphBuilder.AllocParameters<FSSGIDenoiserCS::FParameters>();

        // 输入：Trace 出来的噪点图
        DenoiserParams->SSGIInputTexture = SSGIOutputTexture;
        
        // 辅助纹理：用于计算法线/深度权重
        auto& StParams = SceneTexturesParams->GetParameters();
        FRDGTextureRef Dummy = GSystemTextures.GetBlackDummy(GraphBuilder);
        DenoiserParams->SceneDepthTexture = StParams->SceneDepthTexture ? StParams->SceneDepthTexture : Dummy;
        DenoiserParams->GBufferATexture = StParams->GBufferATexture ? StParams->GBufferATexture : Dummy;
        
		// 输出
        DenoiserParams->SSGIDenoiseOutput = GraphBuilder.CreateUAV(DenoisedTexture);
        
		// 参数
		DenoiserParams->ViewSizeAndInvSize = CommonViewSizeAndInvSize;
		DenoiserParams->BufferSizeAndInvSize = CommonBufferSizeAndInvSize;
		DenoiserParams->ViewRectMin = CommonViewRectMin;
		DenoiserParams->Intensity = SSGIIntensity; // 用于自适应调整 Sigma

        FIntVector GroupCount(FMath::DivideAndRoundUp(SceneColorSlice.ViewRect.Width(), 8), FMath::DivideAndRoundUp(SceneColorSlice.ViewRect.Height(), 8), 1);
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SSGI Spatial Denoise"), ComputeShader, DenoiserParams, GroupCount);
    }

    // =========================================================================
    // 5. Temporal Accumulation (时域累积)
    // -------------------------------------------------------------------------
	// 混合当前帧与历史帧，这是消除闪烁、实现实时画质的核心。
    // =========================================================================
    FRDGTextureRef TemporalOutputTexture = nullptr;
    {
		// 5.1 准备 History Buffer
		// HistoryRenderTarget 是类成员变量，会在帧与帧之间持久存在
        FRDGTextureRef HistoryTextureRef = nullptr;
        bool bIsFirstFrame = !HistoryRenderTarget.IsValid();
        
        // [核心稳定性检查] 
		// 如果视口大小发生变化 (例如用户拖拽窗口)，历史 buffer 尺寸就不匹配了。
		// 必须丢弃旧历史，重新创建，否则会导致 Crash 或严重的画面错乱。
        FIntPoint CurrentViewSize = SceneColorSlice.ViewRect.Size();
        bool bSizeChanged = false;
        if (!bIsFirstFrame)
        {
            FIntPoint HistorySize = HistoryRenderTarget->GetDesc().Extent;
            if (HistorySize != CurrentViewSize)
            {
                bSizeChanged = true;
                HistoryRenderTarget.SafeRelease(); // 释放旧的
            }
        }

        if (bIsFirstFrame || bSizeChanged)
        {
            // 如果没有历史 (首帧或尺寸变化)，给一个黑色的 Dummy 纹理作为历史
            HistoryTextureRef = GSystemTextures.GetBlackDummy(GraphBuilder);
        }
        else
        {
            // 将外部的 POOLED RenderTarget 注册进当前的 RDG 图中
            HistoryTextureRef = GraphBuilder.RegisterExternalTexture(HistoryRenderTarget);
        }

        // 5.2 创建当前帧输出
        FRDGTextureDesc Desc = SSGIOutputTexture->Desc;
        TemporalOutputTexture = GraphBuilder.CreateTexture(Desc, TEXT("SSGI_Temporal_Output"));

        // 5.3 配置参数
        TShaderMapRef<FSSGITemporalCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FSSGITemporalCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSSGITemporalCS::FParameters>();

        // 输入：空间降噪后的结果
        PassParameters->CurrentFrameTexture = DenoisedTexture; 
        PassParameters->HistoryTexture = HistoryTextureRef;
        
		// 速度矢量：用于重投影 (Reprojection)
        auto& StParams = SceneTexturesParams->GetParameters();
        PassParameters->VelocityTexture = StParams->GBufferVelocityTexture ? StParams->GBufferVelocityTexture : GSystemTextures.GetBlackDummy(GraphBuilder);
        
        PassParameters->OutputTexture = GraphBuilder.CreateUAV(TemporalOutputTexture);
        
		PassParameters->ViewSizeAndInvSize = CommonViewSizeAndInvSize;
		PassParameters->BufferSizeAndInvSize = CommonBufferSizeAndInvSize;
		PassParameters->ViewRectMin = CommonViewRectMin;
		
        // [权重控制] 
		// 如果是首帧或尺寸改变，HistoryWeight = 0 (完全不信任历史)
		// 正常情况下 = 0.9 (但实际权重由 Shader 内的 Accumulation Count 动态决定)
        PassParameters->HistoryWeight = (bIsFirstFrame || bSizeChanged) ? 0.0f : 0.9f; 
        PassParameters->BilinearSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

        FIntVector GroupCount(FMath::DivideAndRoundUp(SceneColorSlice.ViewRect.Width(), 8), FMath::DivideAndRoundUp(SceneColorSlice.ViewRect.Height(), 8), 1);
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SSGI Temporal"), ComputeShader, PassParameters, GroupCount);
        
        // 5.4 提取历史 (Extraction)
		// 这一步至关重要：告诉 RDG "请把 TemporalOutputTexture 的内容保留下来，存到 HistoryRenderTarget 里，供下一帧使用"
		// 这就是 RDG 中的 "Ping-Pong" 机制
        GraphBuilder.QueueTextureExtraction(TemporalOutputTexture, &HistoryRenderTarget);
    }
	
	// =========================================================================
    // 6. Composite Pass (合成)
    // -------------------------------------------------------------------------
	// 将计算好的 SSGI 叠加回 SceneColor
    // =========================================================================
	
	// 拷贝一份 SceneColorSlice 的描述来创建输出
	FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(GraphBuilder, SceneColorSlice);
	FRDGTextureDesc OutputDesc = SSGIOutputTexture->Desc;
	OutputDesc.Flags |= TexCreate_UAV;
	// 移除 RenderTargetable 标记，因为我们要用 Compute Shader 写入它
	OutputDesc.Flags &= ~(TexCreate_RenderTargetable | TexCreate_FastVRAM);
	FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(OutputDesc, TEXT("SSGI_Composite_Output"));
	{
		TShaderMapRef<FSSGICompositeCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FSSGICompositeCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSSGICompositeCS::FParameters>();

		PassParameters->SceneColorTexture = SceneColor.Texture;
		
		// [Debug 路由] 根据控制台变量选择要显示的纹理
		int32 CurrentDebugMode = CVarSSGIDebug.GetValueOnRenderThread();
		FRDGTextureRef SelectedTexture = TemporalOutputTexture; // 默认：最终结果
		if (CurrentDebugMode == 1)
		{
			// 查看 Raw Trace 结果 (未降噪)
			SelectedTexture = SSGIOutputTexture;
		}
		else if (CurrentDebugMode == 2)
		{
			// 查看 Spatial Denoise 结果 (单帧降噪)
			SelectedTexture = DenoisedTexture;
		}
		
		PassParameters->SSGIResultTexture = SelectedTexture;
		PassParameters->OutputTexture = GraphBuilder.CreateUAV(OutputTexture);
		
		PassParameters->ViewSizeAndInvSize = CommonViewSizeAndInvSize;
		PassParameters->BufferSizeAndInvSize = CommonBufferSizeAndInvSize;
		PassParameters->ViewRectMin = CommonViewRectMin;
		
		FIntVector GroupCount(FMath::DivideAndRoundUp(SceneColor.ViewRect.Width(), 8), FMath::DivideAndRoundUp(SceneColor.ViewRect.Height(), 8), 1);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("SSGI Composite"), ComputeShader, PassParameters, GroupCount);
	}

	// 将合成后的结果拷贝回管线的 SceneColor
	// (AddCopyTexturePass 会自动处理资源状态转换)
	AddCopyTexturePass(GraphBuilder, OutputTexture, SceneColor.Texture);
	
	return SceneColor;
}