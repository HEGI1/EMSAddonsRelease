//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.

using UnrealBuildTool;
using System.Collections.Generic;

public class EMSAddonsDevEditorTarget : TargetRules
{
	public EMSAddonsDevEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V7;

		ExtraModuleNames.AddRange( new string[] { "EMSAddonsDev" } );
	}
}
