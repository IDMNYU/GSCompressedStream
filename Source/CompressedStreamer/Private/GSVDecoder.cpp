#include "GSVDecoder.h"
#include "Math/Float16.h"

THIRD_PARTY_INCLUDES_START
#include <zlib.h>
THIRD_PARTY_INCLUDES_END

static constexpr float SH_C0 = 0.28209479177387814f;
static constexpr float SQRT1_2 = 0.70710678118654752f;
static constexpr float COLOR_SCALE = 0.15f;
static constexpr float SH_SCALE = 128.0f;

bool FGSVDecoder::DecodeFrame(
	const TArray<uint8>& CompressedBlob,
	const FGSVFrameInfo& FrameInfo,
	int32 FrameIndex,
	const FGSVHeader& Header,
	FGSVDecompressedFrame& OutFrame)
{
	TArray<uint8> Decompressed;
	if (!GzipDecompress(CompressedBlob, Decompressed))
	{
		UE_LOG(LogTemp, Error, TEXT("GSVDecoder: Gzip decompression failed for frame %d"), FrameIndex);
		return false;
	}

	const int32 N = FrameInfo.GaussianCount;
	const int32 TexW = FrameInfo.TextureWidth;

	if (Header.bIsStandaloneFormat)
	{
		// Standalone: AoS layout — [4B n][N*6B pos][N*3B scl][N*4B rot][N opa][N*3B col][N*sh_dim sh]
		const int32 SHDim = Header.SHDim;
		const int32 ExpectedSize = 4 + N * 6 + N * 3 + N * 4 + N + N * 3 + N * SHDim;
		if (Decompressed.Num() < ExpectedSize)
		{
			UE_LOG(LogTemp, Error, TEXT("GSVDecoder: Standalone decompressed %d < expected %d for frame %d"),
				Decompressed.Num(), ExpectedSize, FrameIndex);
			return false;
		}

		uint32 BlobN = 0;
		FMemory::Memcpy(&BlobN, Decompressed.GetData(), 4);
		if (static_cast<int32>(BlobN) != N)
		{
			UE_LOG(LogTemp, Error, TEXT("GSVDecoder: Blob gaussian count %d != expected %d for frame %d"),
				BlobN, N, FrameIndex);
			return false;
		}

		OutFrame.FrameIndex = FrameIndex;
		OutFrame.GaussianCount = N;
		OutFrame.TextureWidth = TexW;
		OutFrame.SHDegree = Header.SHDegree;

		DequantizeStandaloneToTextures(Decompressed.GetData(), Decompressed.Num(), N, TexW, Header, OutFrame);
	}
	else
	{
		// GSV1: SoA layout
		const int32 SHRestCount = GetSHRestCount(Header.SHDegree);
		const int32 ExpectedSize = 4 + N * 6 + N * 4 + N * 3 + N + N * 3 + N * SHRestCount * 2;
		if (Decompressed.Num() < ExpectedSize)
		{
			UE_LOG(LogTemp, Error, TEXT("GSVDecoder: GSV1 decompressed %d < expected %d for frame %d (SH%d)"),
				Decompressed.Num(), ExpectedSize, FrameIndex, Header.SHDegree);
			return false;
		}

		uint32 BlobN = 0;
		FMemory::Memcpy(&BlobN, Decompressed.GetData(), 4);
		if (static_cast<int32>(BlobN) != N)
		{
			UE_LOG(LogTemp, Error, TEXT("GSVDecoder: Blob gaussian count %d != expected %d for frame %d"),
				BlobN, N, FrameIndex);
			return false;
		}

		OutFrame.FrameIndex = FrameIndex;
		OutFrame.GaussianCount = N;
		OutFrame.TextureWidth = TexW;
		OutFrame.SHDegree = Header.SHDegree;

		DequantizeGSV1ToTextures(Decompressed.GetData(), Decompressed.Num(), N, TexW, Header.SHDegree, OutFrame);
	}

	return true;
}

bool FGSVDecoder::GzipDecompress(const TArray<uint8>& CompressedData, TArray<uint8>& OutDecompressed)
{
	z_stream Stream;
	FMemory::Memzero(&Stream, sizeof(Stream));

	Stream.next_in = const_cast<Bytef*>(CompressedData.GetData());
	Stream.avail_in = static_cast<uInt>(CompressedData.Num());

	int32 Ret = inflateInit2(&Stream, 16 + MAX_WBITS);
	if (Ret != Z_OK)
	{
		return false;
	}

	OutDecompressed.Empty();
	TArray<uint8> Buffer;
	Buffer.SetNumUninitialized(65536);

	bool bSuccess = false;
	while (true)
	{
		Stream.next_out = Buffer.GetData();
		Stream.avail_out = static_cast<uInt>(Buffer.Num());

		Ret = inflate(&Stream, Z_NO_FLUSH);
		if (Ret != Z_OK && Ret != Z_STREAM_END)
		{
			break;
		}

		int32 Produced = Buffer.Num() - Stream.avail_out;
		if (Produced > 0)
		{
			OutDecompressed.Append(Buffer.GetData(), Produced);
		}

		if (Ret == Z_STREAM_END)
		{
			bSuccess = true;
			break;
		}
	}

	inflateEnd(&Stream);
	return bSuccess;
}

void FGSVDecoder::DecodeQuaternionSmallestThree(const uint8* Data, float OutQuat[4])
{
	uint32 Comp = Data[0] | (Data[1] << 8) | (Data[2] << 16) | (Data[3] << 24);

	constexpr uint32 CMask = (1u << 9u) - 1u;

	const int32 ILargest = Comp >> 30;
	float SumSquares = 0.0f;

	for (int32 i = 3; i >= 0; --i)
	{
		if (i != ILargest)
		{
			uint32 Mag = Comp & CMask;
			uint32 NegBit = (Comp >> 9u) & 0x1u;
			Comp >>= 10u;

			OutQuat[i] = SQRT1_2 * static_cast<float>(Mag) / static_cast<float>(CMask);
			if (NegBit == 1) OutQuat[i] = -OutQuat[i];
			SumSquares += OutQuat[i] * OutQuat[i];
		}
	}

	OutQuat[ILargest] = FMath::Sqrt(FMath::Max(0.0f, 1.0f - SumSquares));
}

// ============================================================================
// GSV1 format dequantization (SoA, UE-space, float16 positions)
// ============================================================================

void FGSVDecoder::DequantizeGSV1ToTextures(
	const uint8* RawData, int32 RawDataSize,
	int32 GaussianCount, int32 TextureWidth,
	int32 SHDegree,
	FGSVDecompressedFrame& OutFrame)
{
	const int32 N = GaussianCount;
	const int32 TexW = TextureWidth;
	const int32 TexelCount = TexW * TexW;
	const int32 BytesPerTexel = 8;
	const int32 TextureBytes = TexelCount * BytesPerTexel;

	const int32 SHRestCount = GetSHRestCount(SHDegree);
	const int32 NumSHTextures = GetSHTextureCount(SHDegree);
	const int32 NumSHRestTextures = NumSHTextures - 1;

	OutFrame.PositionData.SetNumZeroed(TextureBytes);
	OutFrame.RotationData.SetNumZeroed(TextureBytes);
	OutFrame.ScaleOpacityData.SetNumZeroed(TextureBytes);
	OutFrame.SH0Data.SetNumZeroed(TextureBytes);

	OutFrame.SHRestData.SetNum(NumSHRestTextures);
	for (int32 t = 0; t < NumSHRestTextures; t++)
	{
		OutFrame.SHRestData[t].SetNumZeroed(TextureBytes);
	}

	const uint8* Ptr = RawData + 4;

	// SoA pointers
	const uint8* PosX = Ptr;
	const uint8* PosY = PosX + N * 2;
	const uint8* PosZ = PosY + N * 2;
	const uint8* Rot = PosZ + N * 2;
	const uint8* ScaleX = Rot + N * 4;
	const uint8* ScaleY = ScaleX + N;
	const uint8* ScaleZ = ScaleY + N;
	const uint8* Opac = ScaleZ + N;
	const uint8* ColR = Opac + N;
	const uint8* ColG = ColR + N;
	const uint8* ColB = ColG + N;
	const uint8* SHRestBase = ColB + N;

	FFloat16* PosOut = reinterpret_cast<FFloat16*>(OutFrame.PositionData.GetData());
	FFloat16* RotOut = reinterpret_cast<FFloat16*>(OutFrame.RotationData.GetData());
	FFloat16* SOOut = reinterpret_cast<FFloat16*>(OutFrame.ScaleOpacityData.GetData());
	FFloat16* SH0Out = reinterpret_cast<FFloat16*>(OutFrame.SH0Data.GetData());

	TArray<FFloat16*> SHRestOut;
	SHRestOut.SetNum(NumSHRestTextures);
	for (int32 t = 0; t < NumSHRestTextures; t++)
	{
		SHRestOut[t] = reinterpret_cast<FFloat16*>(OutFrame.SHRestData[t].GetData());
	}

	for (int32 i = 0; i < N; i++)
	{
		const int32 O = i * 4;

		// Position (float16 pass-through)
		uint16 PxRaw, PyRaw, PzRaw;
		FMemory::Memcpy(&PxRaw, PosX + i * 2, 2);
		FMemory::Memcpy(&PyRaw, PosY + i * 2, 2);
		FMemory::Memcpy(&PzRaw, PosZ + i * 2, 2);
		FMemory::Memcpy(&PosOut[O + 0], &PxRaw, 2);
		FMemory::Memcpy(&PosOut[O + 1], &PyRaw, 2);
		FMemory::Memcpy(&PosOut[O + 2], &PzRaw, 2);
		PosOut[O + 3].Set(0.0f);

		// Rotation (smallest-three)
		float Quat[4];
		DecodeQuaternionSmallestThree(Rot + i * 4, Quat);
		RotOut[O + 0].Set(Quat[0]);
		RotOut[O + 1].Set(Quat[1]);
		RotOut[O + 2].Set(Quat[2]);
		RotOut[O + 3].Set(Quat[3]);

		// Scale + Opacity
		float SX = FMath::Exp(static_cast<float>(ScaleX[i]) / 16.0f - 10.0f);
		float SY = FMath::Exp(static_cast<float>(ScaleY[i]) / 16.0f - 10.0f);
		float SZ = FMath::Exp(static_cast<float>(ScaleZ[i]) / 16.0f - 10.0f);
		float Opacity = static_cast<float>(Opac[i]) / 255.0f;
		SOOut[O + 0].Set(SX);
		SOOut[O + 1].Set(SY);
		SOOut[O + 2].Set(SZ);
		SOOut[O + 3].Set(Opacity);

		// SH DC (GSV1 uses SH_C0)
		float DCR = (static_cast<float>(ColR[i]) / 255.0f - 0.5f) / SH_C0;
		float DCG = (static_cast<float>(ColG[i]) / 255.0f - 0.5f) / SH_C0;
		float DCB = (static_cast<float>(ColB[i]) / 255.0f - 0.5f) / SH_C0;
		SH0Out[O + 0].Set(DCR);
		SH0Out[O + 1].Set(DCG);
		SH0Out[O + 2].Set(DCB);

		if (SHRestCount > 0)
		{
			uint16 Rest0Raw;
			FMemory::Memcpy(&Rest0Raw, SHRestBase + i * 2, 2);
			FMemory::Memcpy(&SH0Out[O + 3], &Rest0Raw, 2);
		}
		else
		{
			SH0Out[O + 3].Set(0.0f);
		}

		for (int32 t = 0; t < NumSHRestTextures; t++)
		{
			for (int32 c = 0; c < 4; c++)
			{
				int32 RestIdx = t * 4 + c + 1;
				if (RestIdx < SHRestCount)
				{
					const uint8* CoeffBase = SHRestBase + RestIdx * N * 2;
					uint16 ValRaw;
					FMemory::Memcpy(&ValRaw, CoeffBase + i * 2, 2);
					FMemory::Memcpy(&SHRestOut[t][O + c], &ValRaw, 2);
				}
				else
				{
					SHRestOut[t][O + c].Set(0.0f);
				}
			}
		}
	}
}

// ============================================================================
// Standalone format dequantization (AoS, PLY-space → UE-space)
// ============================================================================

void FGSVDecoder::DequantizeStandaloneToTextures(
	const uint8* RawData, int32 RawDataSize,
	int32 GaussianCount, int32 TextureWidth,
	const FGSVHeader& Header,
	FGSVDecompressedFrame& OutFrame)
{
	const int32 N = GaussianCount;
	const int32 TexW = TextureWidth;
	const int32 TexelCount = TexW * TexW;
	const int32 BytesPerTexel = 8;
	const int32 TextureBytes = TexelCount * BytesPerTexel;

	const int32 SHDim = Header.SHDim;
	const int32 SHDegree = Header.SHDegree;
	const int32 NumSHTextures = GetSHTextureCount(SHDegree);
	const int32 NumSHRestTextures = FMath::Max(0, NumSHTextures - 1);

	const float BBoxMinX = static_cast<float>(Header.BBoxMin.X);
	const float BBoxMinY = static_cast<float>(Header.BBoxMin.Y);
	const float BBoxMinZ = static_cast<float>(Header.BBoxMin.Z);
	const float BBoxRangeX = static_cast<float>(Header.BBoxRange.X);
	const float BBoxRangeY = static_cast<float>(Header.BBoxRange.Y);
	const float BBoxRangeZ = static_cast<float>(Header.BBoxRange.Z);

	OutFrame.PositionData.SetNumZeroed(TextureBytes);
	OutFrame.RotationData.SetNumZeroed(TextureBytes);
	OutFrame.ScaleOpacityData.SetNumZeroed(TextureBytes);
	OutFrame.SH0Data.SetNumZeroed(TextureBytes);

	OutFrame.SHRestData.SetNum(NumSHRestTextures);
	for (int32 t = 0; t < NumSHRestTextures; t++)
	{
		OutFrame.SHRestData[t].SetNumZeroed(TextureBytes);
	}

	// AoS pointers — blob layout: [4B n][N*3*2B pos][N*3B scl][N*4B rot][N opa][N*3B col][N*sh_dim sh]
	const uint8* Ptr = RawData + 4;

	const uint8* PosPtr = Ptr;                          // uint16 x 3 per point (AoS)
	const uint8* SclPtr = PosPtr + N * 6;               // uint8 x 3 per point (AoS)
	const uint8* RotPtr = SclPtr + N * 3;               // uint8 x 4 per point (AoS)
	const uint8* OpaPtr = RotPtr + N * 4;               // uint8 per point
	const uint8* ColPtr = OpaPtr + N;                   // uint8 x 3 per point (AoS)
	const uint8* SHPtr  = ColPtr + N * 3;               // uint8 x sh_dim per point (AoS)

	FFloat16* PosOut = reinterpret_cast<FFloat16*>(OutFrame.PositionData.GetData());
	FFloat16* RotOut = reinterpret_cast<FFloat16*>(OutFrame.RotationData.GetData());
	FFloat16* SOOut = reinterpret_cast<FFloat16*>(OutFrame.ScaleOpacityData.GetData());
	FFloat16* SH0Out = reinterpret_cast<FFloat16*>(OutFrame.SH0Data.GetData());

	TArray<FFloat16*> SHRestOut;
	SHRestOut.SetNum(NumSHRestTextures);
	for (int32 t = 0; t < NumSHRestTextures; t++)
	{
		SHRestOut[t] = reinterpret_cast<FFloat16*>(OutFrame.SHRestData[t].GetData());
	}

	for (int32 i = 0; i < N; i++)
	{
		const int32 O = i * 4; // texel offset (4 half-floats per texel)

		// --- Position: uint16 AoS → dequantize → PLY→UE ---
		const uint16* PosU16 = reinterpret_cast<const uint16*>(PosPtr + i * 6);
		float PlyX = static_cast<float>(PosU16[0]) / 65535.0f * BBoxRangeX + BBoxMinX;
		float PlyY = static_cast<float>(PosU16[1]) / 65535.0f * BBoxRangeY + BBoxMinY;
		float PlyZ = static_cast<float>(PosU16[2]) / 65535.0f * BBoxRangeZ + BBoxMinZ;
		// PLY (x,y,z) → UE (z, x, -y)
		PosOut[O + 0].Set(PlyZ);
		PosOut[O + 1].Set(PlyX);
		PosOut[O + 2].Set(-PlyY);
		PosOut[O + 3].Set(0.0f);

		// --- Scale: uint8 AoS → dequantize → exp → PLY→UE ---
		const uint8* SclU8 = SclPtr + i * 3;
		float LogSX = static_cast<float>(SclU8[0]) / 16.0f - 10.0f;
		float LogSY = static_cast<float>(SclU8[1]) / 16.0f - 10.0f;
		float LogSZ = static_cast<float>(SclU8[2]) / 16.0f - 10.0f;
		float ScaleX = FMath::Exp(LogSX);
		float ScaleY = FMath::Exp(LogSY);
		float ScaleZ = FMath::Exp(LogSZ);
		// PLY (sx,sy,sz) → UE (sz, sx, sy)
		SOOut[O + 0].Set(ScaleZ);
		SOOut[O + 1].Set(ScaleX);
		SOOut[O + 2].Set(ScaleY);

		// --- Opacity: uint8 → sigmoid [0,1] ---
		float Opacity = static_cast<float>(OpaPtr[i]) / 255.0f;
		SOOut[O + 3].Set(Opacity);

		// --- Rotation: uint8 x 4 AoS → dequantize → normalize → PLY→UE ---
		const uint8* RotU8 = RotPtr + i * 4;
		float Rw = static_cast<float>(RotU8[0]) / 127.5f - 1.0f;
		float Rx = static_cast<float>(RotU8[1]) / 127.5f - 1.0f;
		float Ry = static_cast<float>(RotU8[2]) / 127.5f - 1.0f;
		float Rz = static_cast<float>(RotU8[3]) / 127.5f - 1.0f;
		float RNorm = FMath::Sqrt(FMath::Max(1e-16f, Rw * Rw + Rx * Rx + Ry * Ry + Rz * Rz));
		Rw /= RNorm; Rx /= RNorm; Ry /= RNorm; Rz /= RNorm;
		// PLY quat (w,x,y,z) → UE (qx,qy,qz,qw) = (z, x, -y, w)
		RotOut[O + 0].Set(Rz);   // qx_ue = z_ply
		RotOut[O + 1].Set(Rx);   // qy_ue = x_ply
		RotOut[O + 2].Set(-Ry);  // qz_ue = -y_ply
		RotOut[O + 3].Set(Rw);   // qw_ue = w_ply

		// --- SH DC Color: uint8 AoS → dequantize with COLOR_SCALE ---
		const uint8* ColU8 = ColPtr + i * 3;
		float DCR = (static_cast<float>(ColU8[0]) / 255.0f - 0.5f) / COLOR_SCALE;
		float DCG = (static_cast<float>(ColU8[1]) / 255.0f - 0.5f) / COLOR_SCALE;
		float DCB = (static_cast<float>(ColU8[2]) / 255.0f - 0.5f) / COLOR_SCALE;
		SH0Out[O + 0].Set(DCR);
		SH0Out[O + 1].Set(DCG);
		SH0Out[O + 2].Set(DCB);

		// --- SH rest: uint8 AoS → dequantize → pack into SH textures ---
		// sh_0 channel 3 = first SH rest coeff, or 0
		if (SHDim > 0)
		{
			const uint8* SHU8 = SHPtr + i * SHDim;
			float Rest0 = (static_cast<float>(SHU8[0]) - 128.0f) / SH_SCALE;
			SH0Out[O + 3].Set(Rest0);

			// Pack rest[1..end] into sh_1..sh_N textures, 4 channels each
			for (int32 t = 0; t < NumSHRestTextures; t++)
			{
				for (int32 c = 0; c < 4; c++)
				{
					int32 RestIdx = t * 4 + c + 1; // +1 because rest[0] is in sh_0 ch3
					if (RestIdx < SHDim)
					{
						float Val = (static_cast<float>(SHU8[RestIdx]) - 128.0f) / SH_SCALE;
						SHRestOut[t][O + c].Set(Val);
					}
					else
					{
						SHRestOut[t][O + c].Set(0.0f);
					}
				}
			}
		}
		else
		{
			SH0Out[O + 3].Set(0.0f);
		}
	}
}
