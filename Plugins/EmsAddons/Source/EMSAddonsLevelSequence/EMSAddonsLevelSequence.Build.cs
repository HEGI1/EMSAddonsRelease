using UnrealBuildTool;

public class EMSAddonsLevelSequence : ModuleRules
{
	public EMSAddonsLevelSequence(ReadOnlyTargetRules Target) : base(Target)
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
				"EMSAddonsCore",
				"LevelSequence",
				"MovieScene"
			});
	}
}
