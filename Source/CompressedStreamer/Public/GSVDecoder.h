#pragma once

#include "CoreMinimal.h"
#include "GSVFormat.h"

/**
 * Decompressed frame data ready for texture upload.
 * All arrays are half-float (4 components per texel) in row-major order.
 * Texture dimensions: TextureWidth x TextureWidth
 *
 * SH texture packing (sequential 4-channel):
 *   sh_0: (dc_r, dc_g, dc_b, rest_0 | 0)
 *   sh_1: (rest_1, rest_2, rest_3, rest_4)   -- only for SH degree >= 1
 *   ...up to sh_11 for SH degree 3
 */
struct FGSVDecompressedFrame
{
	int32 FrameIndex = -1;
	int32 GaussianCount = 0;
	int32 TextureWidth = 0;
	int32 SHDegree = 0;

	TArray<uint8> PositionData;       // PF_FloatRGBA: half4 per texel (xyz, 0)
	TArray<uint8> RotationData;       // PF_FloatRGBA: half4 per texel (xyzw)
	TArray<uint8> ScaleOpacityData;   // PF_FloatRGBA: half4 per texel (sx, sy, sz, opacity)
	TArray<uint8> SH0Data;            // PF_FloatRGBA: half4 per texel (dc_r, dc_g, dc_b, rest_0|0)
	TArray<TArray<uint8>> SHRestData; // sh_1..sh_N, each PF_FloatRGBA (empty for SH degree 0)

	bool IsValid() const { return GaussianCount > 0 && PositionData.Num() > 0; }

	int32 GetSHTextureCount() const
	{
		int32 TotalValues = 3 * (SHDegree + 1) * (SHDegree + 1);
		return (TotalValues + 3) / 4;
	}

	int64 GetMemorySize() const
	{
		int64 Size = PositionData.Num() + RotationData.Num()
			+ ScaleOpacityData.Num() + SH0Data.Num();
		for (const auto& D : SHRestData) Size += D.Num();
		return Size;
	}
};

/**
 * Decodes compressed GSV frame blobs into texture-ready half-float data.
 * Supports both GSV1 and standalone formats.
 * All methods are thread-safe and suitable for background threads.
 */
class COMPRESSEDSTREAMER_API FGSVDecoder
{
public:
	/**
	 * Decompress and decode a single frame from a compressed blob.
	 * Uses header info to determine format (GSV1 vs standalone) and SH handling.
	 */
	static bool DecodeFrame(
		const TArray<uint8>& CompressedBlob,
		const FGSVFrameInfo& FrameInfo,
		int32 FrameIndex,
		const FGSVHeader& Header,
		FGSVDecompressedFrame& OutFrame);

	static int32 GetSHRestCount(int32 SHDegree)
	{
		return SHDegree > 0 ? 3 * ((SHDegree + 1) * (SHDegree + 1) - 1) : 0;
	}

	static int32 GetSHTextureCount(int32 SHDegree)
	{
		int32 TotalValues = 3 * (SHDegree + 1) * (SHDegree + 1);
		return (TotalValues + 3) / 4;
	}

private:
	static bool GzipDecompress(const TArray<uint8>& CompressedData, TArray<uint8>& OutDecompressed);

	static void DecodeQuaternionSmallestThree(const uint8* Data, float OutQuat[4]);

	// GSV1 format: SoA layout, float16 positions, smallest-three rotations
	static void DequantizeGSV1ToTextures(
		const uint8* RawData, int32 RawDataSize,
		int32 GaussianCount, int32 TextureWidth,
		int32 SHDegree,
		FGSVDecompressedFrame& OutFrame);

	// Standalone format: AoS layout, uint16 bbox positions, uint8 rotations
	// Includes PLY→UE coordinate conversion
	static void DequantizeStandaloneToTextures(
		const uint8* RawData, int32 RawDataSize,
		int32 GaussianCount, int32 TextureWidth,
		const FGSVHeader& Header,
		FGSVDecompressedFrame& OutFrame);
};
