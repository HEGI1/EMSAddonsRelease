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
				"EasyMultiSave",
				"EMSAddonsCore"
			});
	}
}
