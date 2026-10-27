#include "GSVFormat.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"

FGSVFileReader::~FGSVFileReader()
{
	Close();
}

bool FGSVFileReader::Open(const FString& FilePath)
{
	Close();

	OpenFilePath = FilePath;
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	FileHandle = PlatformFile.OpenRead(*FilePath);

	if (!FileHandle)
	{
		UE_LOG(LogTemp, Error, TEXT("GSVFormat: Failed to open file: %s"), *FilePath);
		return false;
	}

	if (!ParseHeader())
	{
		Close();
		return false;
	}

	if (Header.bIsStandaloneFormat)
	{
		UE_LOG(LogTemp, Log, TEXT("GSVFormat: Opened standalone '%s' - %d frames, SH mode %s (dim %d)"),
			*FilePath, Header.NumFrames, *Header.SHMode, Header.SHDim);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("GSVFormat: Opened GSV1 '%s' - %d frames at %.1f FPS, SH degree %d"),
			*FilePath, Header.NumFrames, Header.TargetFPS, Header.SHDegree);
	}

	return true;
}

void FGSVFileReader::Close()
{
	if (FileHandle)
	{
		delete FileHandle;
		FileHandle = nullptr;
	}
	PreloadedData.Empty();
	Header = FGSVHeader();
}

bool FGSVFileReader::ParseHeader()
{
	// Read first 4 bytes to detect format
	uint32 First4 = 0;
	if (!FileHandle->Read(reinterpret_cast<uint8*>(&First4), 4))
	{
		UE_LOG(LogTemp, Error, TEXT("GSVFormat: Failed to read first 4 bytes"));
		return false;
	}

	uint32 HeaderJsonLen = 0;

	if (First4 == GSV_MAGIC)
	{
		// GSV1 format: magic already consumed, next 4 bytes are header length
		if (!FileHandle->Read(reinterpret_cast<uint8*>(&HeaderJsonLen), 4))
		{
			UE_LOG(LogTemp, Error, TEXT("GSVFormat: Failed to read GSV1 header length"));
			return false;
		}
	}
	else
	{
		// Standalone format: first 4 bytes ARE the header length
		HeaderJsonLen = First4;
	}

	if (HeaderJsonLen > 100 * 1024 * 1024)
	{
		UE_LOG(LogTemp, Error, TEXT("GSVFormat: Header too large (%u bytes)"), HeaderJsonLen);
		return false;
	}

	TArray<uint8> JsonBytes;
	JsonBytes.SetNumUninitialized(HeaderJsonLen);
	if (!FileHandle->Read(JsonBytes.GetData(), HeaderJsonLen))
	{
		UE_LOG(LogTemp, Error, TEXT("GSVFormat: Failed to read header JSON"));
		return false;
	}

	FString JsonString;
	{
		FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(JsonBytes.GetData()), HeaderJsonLen);
		JsonString = FString(Converter.Length(), Converter.Get());
	}

	if (First4 == GSV_MAGIC)
	{
		return ParseGSV1Header(JsonString);
	}
	else
	{
		int64 DataStartOffset = 4 + HeaderJsonLen; // header_len(4) + JSON
		return ParseStandaloneHeader(JsonString, DataStartOffset);
	}
}

bool FGSVFileReader::ParseGSV1Header(const FString& JsonString)
{
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("GSVFormat: Failed to parse GSV1 header JSON"));
		return false;
	}

	Header.bIsStandaloneFormat = false;
	Header.Version = JsonObject->GetIntegerField(TEXT("version"));
	Header.NumFrames = JsonObject->GetIntegerField(TEXT("num_frames"));
	Header.TargetFPS = static_cast<float>(JsonObject->GetNumberField(TEXT("target_fps")));
	Header.SHDegree = JsonObject->GetIntegerField(TEXT("sh_degree"));

	if (JsonObject->HasField(TEXT("max_gaussian_count")))
	{
		Header.MaxGaussianCount = JsonObject->GetIntegerField(TEXT("max_gaussian_count"));
	}
	if (JsonObject->HasField(TEXT("max_texture_width")))
	{
		Header.MaxTextureWidth = JsonObject->GetIntegerField(TEXT("max_texture_width"));
	}

	const TArray<TSharedPtr<FJsonValue>>* FramesArray;
	if (!JsonObject->TryGetArrayField(TEXT("frames"), FramesArray))
	{
		UE_LOG(LogTemp, Error, TEXT("GSVFormat: No frames array in GSV1 header"));
		return false;
	}

	Header.Frames.Reserve(FramesArray->Num());
	for (const TSharedPtr<FJsonValue>& Val : *FramesArray)
	{
		const TSharedPtr<FJsonObject>& FrameObj = Val->AsObject();
		FGSVFrameInfo Info;
		Info.GaussianCount = FrameObj->GetIntegerField(TEXT("gaussian_count"));
		Info.TextureWidth = FrameObj->GetIntegerField(TEXT("texture_width"));
		Info.BlobOffset = static_cast<int64>(FrameObj->GetNumberField(TEXT("blob_offset")));
		Info.BlobSize = static_cast<int64>(FrameObj->GetNumberField(TEXT("blob_size")));
		Header.Frames.Add(Info);
	}

	if (Header.Frames.Num() != Header.NumFrames)
	{
		UE_LOG(LogTemp, Error, TEXT("GSVFormat: Frame count mismatch (header=%d, array=%d)"),
			Header.NumFrames, Header.Frames.Num());
		return false;
	}

	if (Header.MaxGaussianCount == 0 || Header.MaxTextureWidth == 0)
	{
		for (const FGSVFrameInfo& F : Header.Frames)
		{
			Header.MaxGaussianCount = FMath::Max(Header.MaxGaussianCount, F.GaussianCount);
			Header.MaxTextureWidth = FMath::Max(Header.MaxTextureWidth, F.TextureWidth);
		}
	}

	return true;
}

bool FGSVFileReader::ParseStandaloneHeader(const FString& JsonString, int64 DataStartOffset)
{
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("GSVFormat: Failed to parse standalone header JSON"));
		return false;
	}

	Header.bIsStandaloneFormat = true;
	Header.Version = JsonObject->GetIntegerField(TEXT("version"));
	Header.NumFrames = JsonObject->GetIntegerField(TEXT("num_frames"));
	Header.TargetFPS = 30.0f; // standalone format has no fps field

	if (JsonObject->HasField(TEXT("method")))
	{
		Header.Method = JsonObject->GetStringField(TEXT("method"));
	}
	if (JsonObject->HasField(TEXT("sh_mode")))
	{
		Header.SHMode = JsonObject->GetStringField(TEXT("sh_mode"));
	}
	Header.SHDim = JsonObject->HasField(TEXT("sh_dim"))
		? JsonObject->GetIntegerField(TEXT("sh_dim")) : 0;

	// Map sh_mode to SHDegree for the texture packing logic
	if (Header.SHMode == TEXT("sh3")) Header.SHDegree = 3;
	else if (Header.SHMode == TEXT("sh2")) Header.SHDegree = 2;
	else if (Header.SHMode == TEXT("sh1")) Header.SHDegree = 1;
	else Header.SHDegree = 0;

	// BBox for position dequantization
	const TArray<TSharedPtr<FJsonValue>>* BBoxMinArr;
	if (JsonObject->TryGetArrayField(TEXT("bbox_min"), BBoxMinArr) && BBoxMinArr->Num() == 3)
	{
		Header.BBoxMin = FVector(
			(*BBoxMinArr)[0]->AsNumber(),
			(*BBoxMinArr)[1]->AsNumber(),
			(*BBoxMinArr)[2]->AsNumber());
	}

	const TArray<TSharedPtr<FJsonValue>>* BBoxRangeArr;
	if (JsonObject->TryGetArrayField(TEXT("bbox_range"), BBoxRangeArr) && BBoxRangeArr->Num() == 3)
	{
		Header.BBoxRange = FVector(
			(*BBoxRangeArr)[0]->AsNumber(),
			(*BBoxRangeArr)[1]->AsNumber(),
			(*BBoxRangeArr)[2]->AsNumber());
	}

	// Parse frame metadata and compute blob offsets from the sequential layout
	const TArray<TSharedPtr<FJsonValue>>* FramesArray;
	if (!JsonObject->TryGetArrayField(TEXT("frames"), FramesArray))
	{
		UE_LOG(LogTemp, Error, TEXT("GSVFormat: No frames array in standalone header"));
		return false;
	}

	int64 CurrentOffset = DataStartOffset;
	Header.Frames.Reserve(FramesArray->Num());

	for (const TSharedPtr<FJsonValue>& Val : *FramesArray)
	{
		const TSharedPtr<FJsonObject>& FrameObj = Val->AsObject();
		FGSVFrameInfo Info;
		Info.GaussianCount = FrameObj->GetIntegerField(TEXT("num_gaussians"));

		int32 BlobLen = FrameObj->GetIntegerField(TEXT("blob_len"));
		Info.BlobOffset = CurrentOffset + 4; // skip the 4-byte blob_len prefix
		Info.BlobSize = BlobLen;
		Info.TextureWidth = FMath::CeilToInt(FMath::Sqrt(static_cast<float>(Info.GaussianCount)));

		CurrentOffset += 4 + BlobLen; // advance past [4B len][blob]
		Header.Frames.Add(Info);
	}

	if (Header.Frames.Num() != Header.NumFrames)
	{
		UE_LOG(LogTemp, Error, TEXT("GSVFormat: Frame count mismatch (header=%d, array=%d)"),
			Header.NumFrames, Header.Frames.Num());
		return false;
	}

	// Compute max values
	Header.MaxGaussianCount = 0;
	Header.MaxTextureWidth = 0;
	for (const FGSVFrameInfo& F : Header.Frames)
	{
		Header.MaxGaussianCount = FMath::Max(Header.MaxGaussianCount, F.GaussianCount);
		Header.MaxTextureWidth = FMath::Max(Header.MaxTextureWidth, F.TextureWidth);
	}

	return true;
}

bool FGSVFileReader::ReadFrameBlob(int32 FrameIndex, TArray<uint8>& OutCompressedData) const
{
	if (FrameIndex < 0 || FrameIndex >= Header.NumFrames)
	{
		return false;
	}

	const FGSVFrameInfo& Info = Header.Frames[FrameIndex];

	if (PreloadedData.Num() > 0)
	{
		if (Info.BlobOffset + Info.BlobSize > PreloadedData.Num())
		{
			UE_LOG(LogTemp, Error, TEXT("GSVFormat: Blob out of range for preloaded data (frame %d)"), FrameIndex);
			return false;
		}
		OutCompressedData.SetNumUninitialized(Info.BlobSize);
		FMemory::Memcpy(OutCompressedData.GetData(), PreloadedData.GetData() + Info.BlobOffset, Info.BlobSize);
		return true;
	}

	FScopeLock Lock(&FileAccessLock);

	if (!FileHandle)
	{
		return false;
	}

	if (!FileHandle->Seek(Info.BlobOffset))
	{
		UE_LOG(LogTemp, Error, TEXT("GSVFormat: Failed to seek to frame %d (offset %lld)"), FrameIndex, Info.BlobOffset);
		return false;
	}

	OutCompressedData.SetNumUninitialized(Info.BlobSize);
	if (!FileHandle->Read(OutCompressedData.GetData(), Info.BlobSize))
	{
		UE_LOG(LogTemp, Error, TEXT("GSVFormat: Failed to read frame %d blob (%lld bytes)"), FrameIndex, Info.BlobSize);
		return false;
	}

	return true;
}

bool FGSVFileReader::PreloadToRAM()
{
	if (!FileHandle)
	{
		return false;
	}

	int64 FileSize = FileHandle->Size();
	if (FileSize <= 0)
	{
		return false;
	}

	UE_LOG(LogTemp, Log, TEXT("GSVFormat: Preloading %.1f MB to RAM..."), FileSize / (1024.0f * 1024.0f));

	double StartTime = FPlatformTime::Seconds();

	PreloadedData.SetNumUninitialized(FileSize);
	FileHandle->Seek(0);
	if (!FileHandle->Read(PreloadedData.GetData(), FileSize))
	{
		PreloadedData.Empty();
		UE_LOG(LogTemp, Error, TEXT("GSVFormat: Failed to preload file"));
		return false;
	}

	double Elapsed = FPlatformTime::Seconds() - StartTime;
	UE_LOG(LogTemp, Log, TEXT("GSVFormat: Preloaded %.1f MB in %.2f seconds"),
		FileSize / (1024.0f * 1024.0f), Elapsed);

	return true;
}
