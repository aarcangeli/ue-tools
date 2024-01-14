using UnrealBuildTool;
using System.Collections.Generic;

public class uetoolsTarget : TargetRules {
    public uetoolsTarget(TargetInfo Target)
        : base(Target) {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.V2;

        ExtraModuleNames.AddRange(new string[] { "uetools" });
    }
}
