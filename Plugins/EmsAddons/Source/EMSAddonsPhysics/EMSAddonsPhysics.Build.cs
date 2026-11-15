//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.

using UnrealBuildTool;

public class EMSAddonsPhysics : ModuleRules
{
	public EMSAddonsPhysics(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

		PublicDependencyModuleNames.AddRange(
			new[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"DeveloperSettings"
			});

		// Held AI characters pause their Move To instead of losing it.
		PrivateDependencyModuleNames.Add("AIModule");
	}
}
