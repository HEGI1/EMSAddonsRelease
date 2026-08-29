//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.

using UnrealBuildTool;
using System.Collections.Generic;

public class EMSAddonsDevTarget : TargetRules
{
	public EMSAddonsDevTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V7;

		ExtraModuleNames.AddRange( new string[] { "EMSAddonsDev" } );
	}
}
