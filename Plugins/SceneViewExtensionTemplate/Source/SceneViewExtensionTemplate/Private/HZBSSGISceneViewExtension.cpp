#include "HZBSSGISceneViewExtension.h"

#include "PixelShaderUtils.h"
#include "PostProcess/PostProcessMaterialInputs.h"

IMPLEMENT_GLOBAL_SHADER(FHZBBuildCS, "/Plugins/SceneViewExtensionTemplate/HZB.usf", "HZBBuildCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSSGICS, "/Plugins/SceneViewExtensionTemplate/SSGI.usf", "SSGICS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSSGICompositeCS, "/Plugins/SceneViewExtensionTemplate/SSGIComposite.usf", "CompositeCS", SF_Compute);  // 改为 CS

namespace
{
	TAutoConsoleVariable<int32> CVarHZBSSGIOn(
		TEXT("r.HZBSSGI"),
		0,
		TEXT("Enable HZB SSGI SceneViewExtension \n")
		TEXT(" 0: OFF;")
		TEXT(" 1: ON."),
		ECVF_RenderThreadSafe);
	TAutoConsoleVariable<int32> CVarSSGIDebug(
		TEXT("r.HZBSSGI.Debug"),
		0,
		TEXT("Debug mode for SSGI \n")
		TEXT(" 0: Normal (Composite Output);\n")
		TEXT(" 1: Ray 迭代热力图;\n")
		TEXT(" 2: Ray 击中 UV;\n")
		TEXT(" 3: Ray 光线方向;\n")
		TEXT(" 4: Visualize HZB (Mip 0);")
		TEXT(" 5: 世界坐标重建检查 ;")
		TEXT(" 6: 屏幕空间光线起点;")
		TEXT(" 7: 深度检查;"),
		ECVF_RenderThreadSafe);
}

FHZBSSGISceneViewExtension::FHZBSSGISceneViewExtension(const FAutoRegister& AutoRegister) : FSceneViewExtensionBase(AutoRegister)
{
	UE_LOG(LogTemp, Log, TEXT("HZBSSGISceneViewExtension: Extension registered"));
}

void FHZBSSGISceneViewExtension::SubscribeToPostProcessingPass(EPostProcessingPass PassId, const FSceneView& View, FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
	// 临时换成 MotionBlur 来测试
	if (PassId == EPostProcessingPass::MotionBlur)
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
		UE_LOG(LogTemp, Error, TEXT("[SSGI] SceneDepth is NULL! Pass Aborted."));
		return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	}
	
	RDG_EVENT_SCOPE(GraphBuilder, "HZBSSGI Generate");
	
	auto SceneSize = SceneDepth->Desc.Extent;
	int32 NumMips = FMath::FloorLog2(FMath::Max(SceneSize.X,SceneSize.Y)) + 1;

	FRDGTextureDesc HZBDesc = FRDGTextureDesc::Create2D(
		SceneSize,
		PF_R32_FLOAT,
		FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV | TexCreate_RenderTargetable
	);
	HZBDesc.NumMips = NumMips;
	FRDGTextureRef HZBTexture = GraphBuilder.CreateTexture(HZBDesc, TEXT("HZB Texture"));

	// AddCopyTexturePass(
	// 	GraphBuilder,
	// 	SceneDepth,
	// 	HZBTexture,
	// 	FRHICopyTextureInfo()
	// );

	// 2. 将 SceneDepth 绘制到 HZB Mip 0
	// 创建输入包装
	FScreenPassTexture SceneDepthInput(SceneDepth);
    
	// [修正] 使用正确的构造函数
	// 方案 A: 使用 2 参数构造函数 (自动使用整个纹理大小，即 Mip 0)
	FScreenPassRenderTarget HZBMip0(HZBTexture, ERenderTargetLoadAction::ENoAction);

	/* // 方案 B: 如果你想显式使用 3 参数构造函数 (如你问题所述)，应该传入 FIntRect：
	FScreenPassRenderTarget HZBMip0(
		HZBTexture, 
		FIntRect(0, 0, SceneSize.X, SceneSize.Y), // ViewRect
		ERenderTargetLoadAction::ENoAction
	);
	*/
    
	// 执行绘制 Pass (替代原来的 Copy Pass)
	AddDrawTexturePass(
		GraphBuilder,
		View,            // 如果这里报错无法转换，请试着改成 static_cast<const FViewInfo&>(View)
		SceneDepthInput, // 输入
		HZBMip0          // 输出
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
	
	//GraphBuilder.QueueTextureExtraction(HZBTexture, &ExtractedHZBTexture);

	// SSGI Pass
	FScreenPassTextureSlice SceneColorSlice = Inputs.GetInput(EPostProcessMaterialInput::SceneColor);
	if (SceneColorSlice.IsValid())
	{
		FIntPoint SSGIPassSize = SceneColorSlice.ViewRect.Size();
		FRDGTextureDesc SSGIOutputDesc = FRDGTextureDesc::Create2D(
			SSGIPassSize,
			PF_FloatRGBA,
			FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_UAV
		);
		FRDGTextureRef SSGIOutputTexture = GraphBuilder.CreateTexture(SSGIOutputDesc, TEXT("SSGI_Raw_Output"));
		int32 DebugMode = CVarSSGIDebug.GetValueOnRenderThread();
		{
			TShaderMapRef<FSSGICS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FSSGICS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSSGICS::FParameters>();

			PassParameters->HZBTexture = HZBTexture;
			PassParameters->SceneColorTexture = SceneColorSlice.TextureSRV;  // 修正：使用 TextureSRV
			PassParameters->HZBSize = FVector4f(HZBTexture->Desc.Extent.X, HZBTexture->Desc.Extent.Y, 1.0f / HZBTexture->Desc.Extent.X, 1.0f / HZBTexture->Desc.Extent.Y);
			PassParameters->MaxMipLevel = NumMips - 1;
			PassParameters->MaxIterations = 64;
			PassParameters->Thickness = 2.0f;
			PassParameters->RayLength = 100.0f;
			PassParameters->Intensity = 10.0f;
			PassParameters->SSGI_Raw_Output = GraphBuilder.CreateUAV(SSGIOutputTexture);
			PassParameters->View = View.ViewUniformBuffer;
			PassParameters->InputSceneDepthTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(SceneDepth));
			PassParameters->DebugMode = DebugMode;

			// [新增] 填充手动参数
			// 1. ViewRectMin (xy是偏移)
			PassParameters->ManualViewRectMin = FVector4f(SceneColorSlice.ViewRect.Min.X, SceneColorSlice.ViewRect.Min.Y, 0, 0);
			
			// 2. ViewSize
			FVector2f ViewSize = FVector2f(SceneColorSlice.ViewRect.Width(), SceneColorSlice.ViewRect.Height());
			PassParameters->ManualViewSizeAndInvSize = FVector4f(ViewSize.X, ViewSize.Y, 1.0f/ViewSize.X, 1.0f/ViewSize.Y);

			// 3. BufferSize (深度图全尺寸)
			FVector2f BufferSize = FVector2f(SceneDepth->Desc.Extent.X, SceneDepth->Desc.Extent.Y);
			PassParameters->ManualBufferSizeAndInvSize = FVector4f(BufferSize.X, BufferSize.Y, 1.0f/BufferSize.X, 1.0f/BufferSize.Y);

			// 4. 矩阵 (注意 UE5 也是用 TranslatedWorld)
			PassParameters->ManualSVPositionToTranslatedWorld = FMatrix44f(View.ViewMatrices.GetInvTranslatedViewProjectionMatrix()); 
			PassParameters->ManualTranslatedWorldToClip = FMatrix44f(View.ViewMatrices.GetTranslatedViewProjectionMatrix());
			
			FIntVector GroupCount = FIntVector(
				FMath::DivideAndRoundUp(SceneColorSlice.ViewRect.Width(), 8),
				FMath::DivideAndRoundUp(SceneColorSlice.ViewRect.Height(), 8),
				1
			);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("SSGI Raw Trace"),
				ComputeShader,
				PassParameters,
				GroupCount
			);
		}
		
		// [修改开始] Composite Pass 部分
		FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(GraphBuilder, SceneColorSlice);
		
		FRDGTextureDesc OutputDesc = SceneColor.Texture->Desc;
		// 确保有 UAV 标记，以便 CompositeCS 可以写入
		OutputDesc.Flags |= TexCreate_UAV;
		// 移除 RenderTargetable 等可能不需要的标记，避免验证错误 (可选，视情况而定)
		OutputDesc.Flags &= ~(TexCreate_RenderTargetable | TexCreate_FastVRAM);

		FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(OutputDesc, TEXT("SSGI_Composite_Output"));
		
		{
			TShaderMapRef<FSSGICompositeCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FSSGICompositeCS::FParameters* CompositeParams = GraphBuilder.AllocParameters<FSSGICompositeCS::FParameters>();
			
			CompositeParams->SceneColorTexture = SceneColor.Texture;
			CompositeParams->SSGIResultTexture = SSGIOutputTexture;
			CompositeParams->OutputTexture = GraphBuilder.CreateUAV(OutputTexture);
			CompositeParams->ViewportSize = FVector2f(SceneColor.ViewRect.Width(), SceneColor.ViewRect.Height());
			CompositeParams->DebugMode = DebugMode;
			
			FIntVector GroupCount = FIntVector(
				FMath::DivideAndRoundUp(SceneColor.ViewRect.Width(), 8),
				FMath::DivideAndRoundUp(SceneColor.ViewRect.Height(), 8),
				1
			);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("Composite SSGI"),
				ComputeShader,
				CompositeParams,
				GroupCount
			);
		}

		// [核心修复] 将计算结果拷贝回 SceneColor，而不是返回新纹理
		// 这样能确保与后续 PostProcess 管线的完美衔接
		AddCopyTexturePass(GraphBuilder, OutputTexture, SceneColor.Texture);
		
		// 返回 SceneColor (它现在包含了 SSGI 的结果)
		return SceneColor;
	}

	return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
}
