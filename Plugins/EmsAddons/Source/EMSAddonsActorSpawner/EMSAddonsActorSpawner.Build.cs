using UnrealBuildTool;

public class EMSAddonsActorSpawner : ModuleRules
{
	public EMSAddonsActorSpawner(ReadOnlyTargetRules Target) : base(Target)
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
