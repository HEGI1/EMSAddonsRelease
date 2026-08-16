using UnrealBuildTool;

public class EMSAddonsWorldPartition : ModuleRules
{
	public EMSAddonsWorldPartition(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

		PublicDependencyModuleNames.AddRange(
			new[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"EasyMultiSave",
				"EMSAddonsCore"
			});
	}
}
