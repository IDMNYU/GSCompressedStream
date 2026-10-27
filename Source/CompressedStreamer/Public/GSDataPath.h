#pragma once

#include "Misc/Paths.h"
#include "UObject/SoftObjectPath.h"

namespace GSDataPath
{
	inline FString GetProjectRoot()
	{
		FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		FPaths::NormalizeDirectoryName(Dir);
		return Dir;
	}

	/** Convert absolute or /Game/ paths to a portable project-relative path string. */
	inline bool TryNormalizeToProjectRelative(FString& InOutPath)
	{
		if (InOutPath.IsEmpty())
		{
			return false;
		}

		if (InOutPath.StartsWith(TEXT("/Game/")))
		{
			InOutPath = FString(TEXT("Content/")) + InOutPath.Mid(6);
			FPaths::NormalizeFilename(InOutPath);
			return true;
		}

		if (FPaths::IsRelative(InOutPath))
		{
			FPaths::NormalizeFilename(InOutPath);
			return false;
		}

		const FString Original = InOutPath;
		FString FullPath = FPaths::ConvertRelativePathToFull(InOutPath);
		FPaths::NormalizeFilename(FullPath);

		const FString ProjDir = GetProjectRoot();
		if (FullPath.Len() >= ProjDir.Len()
			&& FullPath.Left(ProjDir.Len()).Equals(ProjDir, ESearchCase::IgnoreCase))
		{
			FString Relative = FullPath.Mid(ProjDir.Len());
			Relative.RemoveFromStart(TEXT("/"));
			Relative.RemoveFromStart(TEXT("\\"));
			FPaths::NormalizeFilename(Relative);
			InOutPath = Relative;
			return InOutPath != Original;
		}

		FString RelativePath = FullPath;
		if (FPaths::MakePathRelativeTo(RelativePath, *FPaths::ProjectDir()))
		{
			FPaths::NormalizeFilename(RelativePath);
			InOutPath = RelativePath;
			return InOutPath != Original;
		}

		return false;
	}

	inline bool TryNormalizeFilePath(FFilePath& Path)
	{
		return TryNormalizeToProjectRelative(Path.FilePath);
	}

	/** Resolve a stored path to an absolute on-disk path for reading/writing. */
	inline FString ResolvePath(const FString& InPath)
	{
		if (InPath.IsEmpty())
		{
			return InPath;
		}

		if (InPath.StartsWith(TEXT("/Game/")))
		{
			return FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir(), InPath.Mid(6));
		}

		if (FPaths::IsRelative(InPath))
		{
			const FString FromProject = FPaths::ConvertRelativePathToFull(GetProjectRoot(), InPath);
			if (FPaths::FileExists(FromProject))
			{
				return FromProject;
			}

			const FString FromContent = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir(), InPath);
			if (FPaths::FileExists(FromContent))
			{
				return FromContent;
			}

			return FromProject;
		}

		return FPaths::ConvertRelativePathToFull(InPath);
	}
}
