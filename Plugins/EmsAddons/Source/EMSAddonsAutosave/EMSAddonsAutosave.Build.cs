using UnrealBuildTool;

public class EMSAddonsAutosave : ModuleRules
{
	public EMSAddonsAutosave(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

		PublicDependencyModuleNames.AddRange(
			new[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"DeveloperSettings",
				"EasyMultiSave"
			});
	}
}
