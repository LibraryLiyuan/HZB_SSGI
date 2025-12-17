// Custom SceneViewExtension Template for Unreal Engine
// Copyright 2023 - 2025 Ossi Luoto
// 
// Subsystem to keep custom SceneViewExtension alive

#include "TemplateSubsystem.h"
#include "CustomSceneViewExtension.h"
#include "HZBSSGISceneViewExtension.h"
#include "SceneViewExtension.h"

void UTemplateSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	HZBSSGISceneViewExtension = FSceneViewExtensions::NewExtension<FHZBSSGISceneViewExtension>();
	UE_LOG(LogTemp, Log, TEXT("SceneViewExtensionTemplate: Subsystem initialized & SceneViewExtension created"));
}

void UTemplateSubsystem::Deinitialize()
{
	{
		HZBSSGISceneViewExtension->IsActiveThisFrameFunctions.Empty();

		FSceneViewExtensionIsActiveFunctor IsActiveFunctor;

		IsActiveFunctor.IsActiveFunction = [](const ISceneViewExtension* SceneViewExtension, const FSceneViewExtensionContext& Context)
		{
			return TOptional<bool>(false);
		};

		HZBSSGISceneViewExtension->IsActiveThisFrameFunctions.Add(IsActiveFunctor);
	}

	HZBSSGISceneViewExtension.Reset();
	HZBSSGISceneViewExtension = nullptr;
}
