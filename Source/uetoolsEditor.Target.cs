using UnrealBuildTool;
using System.Collections.Generic;

public class uetoolsEditorTarget : TargetRules {
    public uetoolsEditorTarget(TargetInfo Target)
        : base(Target) {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.V2;

        ExtraModuleNames.AddRange(new string[] { "uetools" });
    }
}
