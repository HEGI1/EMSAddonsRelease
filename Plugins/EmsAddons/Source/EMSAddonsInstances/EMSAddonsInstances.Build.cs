using UnrealBuildTool;

public class EMSAddonsInstances : ModuleRules
{
	public EMSAddonsInstances(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

		PublicDependencyModuleNames.AddRange(
			new[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"Foliage",
				"GameplayTags",
				"EasyMultiSave",
				"EMSAddonsCore"
			});
	}
}
