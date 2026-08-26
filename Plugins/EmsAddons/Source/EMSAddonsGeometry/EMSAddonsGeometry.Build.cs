using UnrealBuildTool;

public class EMSAddonsGeometry : ModuleRules
{
	public EMSAddonsGeometry(ReadOnlyTargetRules Target) : base(Target)
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
				"EMSAddonsPhysics",
				"GeometryCollectionEngine"
			});

		PrivateDependencyModuleNames.AddRange(
			new[]
			{
				"Chaos",
				"PhysicsCore"
			});
	}
}
