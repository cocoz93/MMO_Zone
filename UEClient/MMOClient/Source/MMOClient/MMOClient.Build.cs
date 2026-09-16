// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class MMOClient : ModuleRules
{
	public MMOClient(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"AIModule",
			"StateTreeModule",
			"GameplayStateTreeModule",
			"UMG",
			"Slate",
			// 아래 둘은 언리얼 리플리케이션 대신 자체 TCP 로 붙기 위한 것이다.
			"Sockets",
			"Networking"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });

		PublicIncludePaths.AddRange(new string[] {
			"MMOClient",
			"MMOClient/Variant_Platforming",
			"MMOClient/Variant_Platforming/Animation",
			"MMOClient/Variant_Combat",
			"MMOClient/Variant_Combat/AI",
			"MMOClient/Variant_Combat/Animation",
			"MMOClient/Variant_Combat/Gameplay",
			"MMOClient/Variant_Combat/Interfaces",
			"MMOClient/Variant_Combat/UI",
			"MMOClient/Variant_SideScrolling",
			"MMOClient/Variant_SideScrolling/AI",
			"MMOClient/Variant_SideScrolling/Gameplay",
			"MMOClient/Variant_SideScrolling/Interfaces",
			"MMOClient/Variant_SideScrolling/UI"
		});

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
		// ------------------------------------------------------------------
		// Shared/Protocol/Protocol.h 를 그대로 참조한다.
		//   콘솔·웹·언리얼 셋이 같은 헤더 한 벌을 본다. 복사본을 만들지 않는다.
		//   프로젝트가 리포 안 어느 깊이에 있든 찾도록 위로 올라가며 탐색한다.
		// ------------------------------------------------------------------
		string Dir = ModuleDirectory;
		bool Found = false;
		for (int i = 0; i < 8; i++)
		{
			string Candidate = Path.Combine(Dir, "Shared", "Protocol");
			if (File.Exists(Path.Combine(Candidate, "Protocol.h")))
			{
				PublicIncludePaths.Add(Candidate);
				Found = true;
				break;
			}
			DirectoryInfo Parent = Directory.GetParent(Dir);
			if (Parent == null) break;
			Dir = Parent.FullName;
		}
		if (!Found)
		{
			throw new BuildException(
				"Shared/Protocol/Protocol.h 를 찾지 못했다. 언리얼 프로젝트가 MMO 리포 안에 있는지 확인할 것. ModuleDirectory=" + ModuleDirectory);
		}
	}
}