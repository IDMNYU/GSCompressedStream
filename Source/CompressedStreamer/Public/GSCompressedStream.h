#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/Texture2D.h"
#include "InputCoreTypes.h"
#include "GSVFormat.h"
#include "GSVDecoder.h"
#include "GSDataPath.h"
#include "GSCompressedStream.generated.h"

class UNiagaraComponent;
class UNiagaraSystem;

UENUM(BlueprintType)
enum class EGSPlaybackMode : uint8
{
	Once,
	Loop,
};

USTRUCT(BlueprintType)
struct COMPRESSEDSTREAMER_API FGSVFileEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (FilePathFilter = "gsv", RelativeToGameDir,
		ToolTip = "Path to .gsv file, relative to the project folder"))
	FFilePath GSVFilePath;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnGSVSequenceComplete);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnGSVFrameChanged, int32, NewFrame);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnGSVFileSwitched, int32, NewIndex, const FString&, FileName);

/**
 * N-buffered compressed Gaussian Splatting Video streamer.
 *
 * Architecture:
 *   - N Niagara components (configurable 2-4) with position-based visibility swap
 *   - Async decompression into a RAM cache (TMap of decoded frames)
 *   - Per-component transient textures, destroyed on reuse
 *   - ReinitializeSystem on each frame transition for reliable texture binding
 *   - Fixed texture width from GSV header for constant particle count
 */
UCLASS(ClassGroup=(GaussianSplatting), meta=(BlueprintSpawnableComponent, DisplayName="GS Compressed Stream"))
class COMPRESSEDSTREAMER_API UGSCompressedStream : public UActorComponent
{
	GENERATED_BODY()

public:
	UGSCompressedStream();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Source",
		meta = (FilePathFilter = "gsv", RelativeToGameDir,
			ToolTip = "Path to a .gsv file, relative to the project folder (e.g. StreamingData/dance.gsv)"))
	FFilePath GSVFilePath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Source")
	TArray<FGSVFileEntry> FileList;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Source", meta = (ClampMin = "0"))
	int32 DefaultFileIndex = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Render")
	UNiagaraSystem* NiagaraSystemAsset;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Playback", meta = (ClampMin = "0.1", ClampMax = "5.0"))
	float PlaybackSpeed = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Playback")
	EGSPlaybackMode PlaybackMode = EGSPlaybackMode::Loop;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Playback")
	bool bAutoPlay = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Playback")
	bool bEnableKeyboardControl = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Playback")
	FKey PauseKey = EKeys::P;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Playback")
	FKey PrevFrameKey = EKeys::Comma;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Playback")
	FKey NextFrameKey = EKeys::Period;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Playback")
	FKey PrevFileKey = EKeys::LeftBracket;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Playback")
	FKey NextFileKey = EKeys::RightBracket;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Performance",
		meta = (ToolTip = "Preload entire .gsv into RAM for zero disk I/O during playback."))
	bool bPreloadToRAM = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Performance",
		meta = (ClampMin = "1", ClampMax = "8",
			ToolTip = "Number of frames to keep decompressed in RAM ahead of playback."))
	int32 DecompressAhead = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Performance",
		meta = (ClampMin = "2", ClampMax = "4",
			ToolTip = "Number of Niagara components for N-buffered rendering."))
	int32 BufferCount = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Playback", meta = (ClampMin = "-1"))
	int32 StartFrame = -1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Playback", meta = (ClampMin = "-1"))
	int32 EndFrame = -1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
	bool bShowPlaybackUI = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI")
	bool bShowMouseCursor = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "UI", meta = (ClampMin = "0", ClampMax = "500"))
	float UIVerticalOffset = 30.0f;

	UPROPERTY(BlueprintAssignable, Category = "Events")
	FOnGSVSequenceComplete OnSequenceComplete;

	UPROPERTY(BlueprintAssignable, Category = "Events")
	FOnGSVFrameChanged OnFrameChanged;

	UPROPERTY(BlueprintAssignable, Category = "Events")
	FOnGSVFileSwitched OnFileSwitched;

	UFUNCTION(BlueprintCallable, Category = "Setup")
	bool LoadGSVFile(const FString& FilePath);

	UFUNCTION(BlueprintCallable, Category = "Playback")
	void Play();

	UFUNCTION(BlueprintCallable, Category = "Playback")
	void Pause();

	UFUNCTION(BlueprintCallable, Category = "Playback")
	void Stop();

	UFUNCTION(BlueprintCallable, Category = "Playback")
	void GoToFrame(int32 Frame);

	UFUNCTION(BlueprintCallable, Category = "Playback")
	void TogglePlayPause();

	UFUNCTION(BlueprintCallable, Category = "Playback")
	bool IsPlaying() const { return bIsPlaying; }

	UFUNCTION(BlueprintCallable, Category = "Playback")
	int32 GetCurrentFrame() const { return CurrentFrameIndex; }

	UFUNCTION(BlueprintCallable, Category = "Playback")
	int32 GetFrameCount() const;

	UFUNCTION(BlueprintCallable, Category = "Playback")
	bool IsFileLoaded() const { return bFileLoaded; }

	UFUNCTION(BlueprintCallable, Category = "File")
	bool SwitchFile(int32 Index);

	UFUNCTION(BlueprintCallable, Category = "File")
	void NextFile();

	UFUNCTION(BlueprintCallable, Category = "File")
	void PreviousFile();

	UFUNCTION(BlueprintCallable, Category = "Debug")
	FString GetActiveFileName() const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

#if WITH_EDITOR
	virtual void PostLoad() override;
	virtual void PostEditChangeChainProperty(FPropertyChangedChainEvent& PropertyChangedEvent) override;
#endif

private:
	static FString ResolveGSVPath(const FString& InPath);
	bool NormalizeStoredPathsToRelative();

	// N-buffered Niagara
	void SetupNiagaraComponents();
	void CleanupNiagaraComponents();

	// Per-component transient textures
	struct FTransientFrame
	{
		UTexture2D* Position = nullptr;
		UTexture2D* Rotation = nullptr;
		UTexture2D* ScaleOpacity = nullptr;
		UTexture2D* SH0 = nullptr;
		TArray<UTexture2D*> SHRest; // sh_1..sh_N (empty for SH0)
		int32 FrameIndex = -1;

		bool IsValid() const;
		void Destroy();
	};

	UTexture2D* CreateHalfTexture(int32 W, int32 H, const TArray<uint8>& Data);
	bool LoadDecodedFrameToSlot(int32 SlotIndex, const FGSVDecompressedFrame& Frame);
	void ApplyFrameToComponent(int32 ComponentIndex);
	void ClearNiagaraTextures(int32 ComponentIndex);
	void DestroyFrame(int32 SlotIndex);

	// Visibility swapping
	void UpdateVisibleFrame();
	void PrepareNextFrames();

	// Async decompression + RAM cache
	void ScheduleDecompression();
	void CollectDecompressedFrames();
	void EvictOldCacheEntries();

	int32 GetEffectiveStartFrame() const;
	int32 GetEffectiveEndFrame() const;
	int32 WrapFrameIndex(int32 Index) const;
	void HandleKeyboardInput(float DeltaTime);

	void CreatePlaybackUI();
	void DestroyPlaybackUI();
	void UpdatePlaybackUI();

private:
	TSharedPtr<FGSVFileReader> GSVReader;
	bool bFileLoaded = false;
	int32 ActiveFileIndex = -1;
	int32 FixedTextureWidth = 0;

	// N-buffered Niagara components
	UPROPERTY()
	TArray<UNiagaraComponent*> NiagaraComponents;

	TArray<FTransientFrame> TransientFrames;
	TArray<int32> PreparedFrameIndex;
	int32 ActiveComponentIndex = 0;
	int32 PendingHideComponentIndex = -1;

	// RAM decode cache: frame index -> decompressed data
	TMap<int32, TSharedPtr<FGSVDecompressedFrame>> DecodeCache;

	// In-flight async jobs
	struct FAsyncJob
	{
		int32 FrameIndex = -1;
		TSharedPtr<FGSVDecompressedFrame> Result;
		TAtomic<bool> bDone{false};
	};
	TArray<TSharedPtr<FAsyncJob>> InFlightJobs;

	// Playback state
	bool bIsPlaying = false;
	float FrameAccumulator = 0.0f;
	int32 CurrentFrameIndex = 0;
	int32 LastDisplayedFrame = -1;
	int32 FramesDestroyedSinceGC = 0;

	// Keyboard
	bool bPrevPausePressed = false;
	bool bPrevLeftPressed = false;
	bool bPrevRightPressed = false;
	bool bPrevPrevFilePressed = false;
	bool bPrevNextFilePressed = false;
	float PrevKeyHoldTime = 0.0f;
	float NextKeyHoldTime = 0.0f;
	static constexpr float KeyHoldDelay = 0.3f;
	static constexpr float KeyRepeatRate = 1.0f / 15.0f;
	float KeyRepeatAccumulator = 0.0f;

	// UI
	TSharedPtr<class SWidget> PlaybackUIWidget;
	TSharedPtr<class STextBlock> PlayPauseTextBlock;
	TSharedPtr<class STextBlock> FrameCounterTextBlock;
	TSharedPtr<class STextBlock> FileNameTextBlock;
	bool bUICreated = false;
};
