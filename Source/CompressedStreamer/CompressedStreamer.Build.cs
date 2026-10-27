using UnrealBuildTool;

public class CompressedStreamer : ModuleRules
{
	public CompressedStreamer(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"InputCore",
				"RenderCore",
				"RHI",
				"Niagara",
				"Json",
				"JsonUtilities",
				"Slate",
				"SlateCore"
			}
		);

		AddEngineThirdPartyPrivateStaticDependencies(Target, "zlib");

		PublicIncludePaths.AddRange(new string[] { });
		PrivateIncludePaths.AddRange(new string[] { });
	}
}
