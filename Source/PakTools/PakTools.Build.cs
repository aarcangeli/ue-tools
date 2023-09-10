// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using UnrealBuildBase;
using UnrealBuildTool;

public class PakTools : ModuleRules {
    public PakTools(ReadOnlyTargetRules Target)
        : base(Target) {
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core",
            "CoreUObject",
            "AssetRegistry",
            "PakFile",
            "Json",
            "Projects",
            "PakFileUtilities",
            "RSA",
            "ApplicationCore",
            "Json",
        });

        // Hack to get private includes from Core
        Console.WriteLine("Unreal.EngineDirectory: " + Unreal.EngineDirectory);
        PrivateIncludePaths.Add(Unreal.EngineDirectory + "/Source/Runtime/Launch/Public");
        PrivateIncludePaths.Add(Unreal.EngineDirectory + "/Source/Runtime/Core/Internal");
        PrivateIncludePaths.Add(Unreal.EngineDirectory + "/Source/Runtime/PakFile/Internal");
    }
}
