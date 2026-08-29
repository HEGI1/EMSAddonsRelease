//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.

using UnrealBuildTool;

public class EMSAddonsCore : ModuleRules
{
	public EMSAddonsCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

		PublicDependencyModuleNames.AddRange(
			new[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"EasyMultiSave"
			});
	}
}
