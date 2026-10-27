#pragma once

#include "CoreMinimal.h"

/**
 * .gsv (Gaussian Splatting Video) file format — supports two variants:
 *
 * === "GSV1" format (legacy) ===
 *   [4B] Magic: "GSV1" (0x31565347 little-endian)
 *   [4B] Header JSON length (uint32 LE)
 *   [NB] Header JSON (UTF-8)
 *   [..] Frame blobs at absolute offsets (gzip-compressed)
 *
 *   Per-frame blob (SoA, UE-space, float16 positions, smallest-three rotations):
 *     [4B] gaussian_count, [N*6B] pos_f16, [N*4B] rot_st3, [N*3B] scl_u8,
 *     [N*1B] opa_u8, [N*3B] col_u8, [N*SH*2B] sh_rest_f16
 *
 * === Standalone format ===
 *   [4B] Header JSON length (uint32 LE)
 *   [NB] Header JSON (UTF-8)
 *   [4B blob_len + gzip blob] × N_frames
 *
 *   Per-frame blob (AoS, PLY-space, uint16 bbox-relative positions, uint8 rotations):
 *     [4B] n, [N*6B] pos_u16, [N*3B] scl_u8, [N*4B] rot_u8,
 *     [N*1B] opa_u8, [N*3B] col_u8, [N*sh_dim B] sh_u8
 */

constexpr uint32 GSV_MAGIC = 0x31565347; // "GSV1" little-endian

struct FGSVFrameInfo
{
	int32 GaussianCount = 0;
	int32 TextureWidth = 0;
	int64 BlobOffset = 0;
	int64 BlobSize = 0;
};

struct FGSVHeader
{
	int32 Version = 0;
	int32 NumFrames = 0;
	float TargetFPS = 30.0f;
	int32 SHDegree = 0;
	int32 MaxGaussianCount = 0;
	int32 MaxTextureWidth = 0;
	TArray<FGSVFrameInfo> Frames;

	// Standalone format fields
	bool bIsStandaloneFormat = false;
	FString Method;
	FString SHMode;
	int32 SHDim = 0;       // number of SH rest coefficients per gaussian
	FVector BBoxMin = FVector::ZeroVector;
	FVector BBoxRange = FVector::ZeroVector;
};

/**
 * Reads a .gsv file header and provides frame blob access.
 * Auto-detects format: if first 4 bytes == GSV_MAGIC, uses GSV1 format;
 * otherwise treats them as header length for standalone format.
 */
class COMPRESSEDSTREAMER_API FGSVFileReader
{
public:
	~FGSVFileReader();

	bool Open(const FString& FilePath);
	void Close();

	bool IsOpen() const { return FileHandle != nullptr; }

	const FGSVHeader& GetHeader() const { return Header; }
	int32 GetFrameCount() const { return Header.NumFrames; }
	float GetTargetFPS() const { return Header.TargetFPS; }

	/** Read compressed blob for a single frame. Thread-safe for different frames. */
	bool ReadFrameBlob(int32 FrameIndex, TArray<uint8>& OutCompressedData) const;

	/** Preload entire file into RAM for zero-I/O playback */
	bool PreloadToRAM();
	bool IsPreloaded() const { return PreloadedData.Num() > 0; }
	int64 GetPreloadedSize() const { return PreloadedData.Num(); }

private:
	bool ParseHeader();
	bool ParseGSV1Header(const FString& JsonString);
	bool ParseStandaloneHeader(const FString& JsonString, int64 DataStartOffset);

	FGSVHeader Header;
	IFileHandle* FileHandle = nullptr;
	FString OpenFilePath;

	TArray<uint8> PreloadedData;
	mutable FCriticalSection FileAccessLock;
};
