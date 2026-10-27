#include "GSCompressedStream.h"
#include "GSDataPath.h"
#include "Misc/Paths.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "RenderingThread.h"
#include "RHI.h"
#include "GameFramework/PlayerController.h"
#include "Async/Async.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SWeakWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Framework/Application/SlateApplication.h"
#include "Engine/GameViewportClient.h"
#include "Styling/CoreStyle.h"

// ============================================================================
// Project-relative path helpers
// ============================================================================

FString UGSCompressedStream::ResolveGSVPath(const FString& InPath)
{
	return GSDataPath::ResolvePath(InPath);
}

bool UGSCompressedStream::NormalizeStoredPathsToRelative()
{
	bool bChanged = false;

	if (GSDataPath::TryNormalizeFilePath(GSVFilePath))
	{
		bChanged = true;
	}

	for (FGSVFileEntry& Entry : FileList)
	{
		if (GSDataPath::TryNormalizeFilePath(Entry.GSVFilePath))
		{
			bChanged = true;
		}
	}

	return bChanged;
}

#if WITH_EDITOR
void UGSCompressedStream::PostLoad()
{
	Super::PostLoad();
	if (NormalizeStoredPathsToRelative())
	{
		Modify();
	}
}

void UGSCompressedStream::PostEditChangeChainProperty(FPropertyChangedChainEvent& PropertyChangedEvent)
{
	Super::PostEditChangeChainProperty(PropertyChangedEvent);

	if (NormalizeStoredPathsToRelative())
	{
		Modify();
	}
}
#endif

// ============================================================================
// TransientFrame helpers
// ============================================================================

bool UGSCompressedStream::FTransientFrame::IsValid() const
{
	return Position && ::IsValid(Position) && Position->IsValidLowLevel()
		&& Rotation && ::IsValid(Rotation) && Rotation->IsValidLowLevel()
		&& ScaleOpacity && ::IsValid(ScaleOpacity) && ScaleOpacity->IsValidLowLevel()
		&& SH0 && ::IsValid(SH0) && SH0->IsValidLowLevel();
}

void UGSCompressedStream::FTransientFrame::Destroy()
{
	auto Kill = [](UTexture2D*& T)
	{
		if (T && ::IsValid(T) && T->IsValidLowLevel())
		{
			T->ReleaseResource();
			T->ClearFlags(RF_Standalone | RF_Public);
			T->MarkAsGarbage();
			T = nullptr;
		}
	};
	Kill(Position);
	Kill(Rotation);
	Kill(ScaleOpacity);
	Kill(SH0);
	for (auto& Tex : SHRest) Kill(Tex);
	SHRest.Empty();
	FrameIndex = -1;
}

// ============================================================================
// Lifecycle
// ============================================================================

UGSCompressedStream::UGSCompressedStream()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

void UGSCompressedStream::BeginPlay()
{
	Super::BeginPlay();
	DecompressAhead = FMath::Clamp(DecompressAhead, 1, 8);
	BufferCount = FMath::Clamp(BufferCount, 2, 4);

	NiagaraComponents.SetNum(BufferCount);
	TransientFrames.SetNum(BufferCount);
	PreparedFrameIndex.SetNum(BufferCount);
	for (int32 i = 0; i < BufferCount; i++)
	{
		NiagaraComponents[i] = nullptr;
		PreparedFrameIndex[i] = -1;
	}

	SetupNiagaraComponents();

	if (FileList.Num() > 0)
	{
		SwitchFile(FMath::Clamp(DefaultFileIndex, 0, FileList.Num() - 1));
	}
	else if (!GSVFilePath.FilePath.IsEmpty())
	{
		if (LoadGSVFile(GSVFilePath.FilePath) && bAutoPlay)
			Play();
	}

	if (bShowPlaybackUI)
		CreatePlaybackUI();
}

void UGSCompressedStream::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	DestroyPlaybackUI();
	Stop();

	for (auto& Job : InFlightJobs)
	{
		if (Job.IsValid())
		{
			while (!Job->bDone.Load())
				FPlatformProcess::Sleep(0.001f);
		}
	}
	InFlightJobs.Empty();
	DecodeCache.Empty();

	for (int32 i = 0; i < TransientFrames.Num(); i++)
	{
		DestroyFrame(i);
	}

	FlushRenderingCommands();
	CleanupNiagaraComponents();

	if (GEngine)
		GEngine->ForceGarbageCollection(true);

	if (GSVReader.IsValid())
	{
		GSVReader->Close();
		GSVReader.Reset();
	}

	Super::EndPlay(EndPlayReason);
}

// ============================================================================
// N-Buffered Niagara
// ============================================================================

void UGSCompressedStream::SetupNiagaraComponents()
{
	if (!NiagaraSystemAsset)
	{
		UE_LOG(LogTemp, Warning, TEXT("GSCompressedStream: No NiagaraSystemAsset set"));
		return;
	}

	AActor* Owner = GetOwner();
	if (!Owner || !Owner->GetRootComponent())
		return;

	for (int32 i = 0; i < BufferCount; i++)
	{
		NiagaraComponents[i] = NewObject<UNiagaraComponent>(Owner, NAME_None, RF_Transient);
		if (!NiagaraComponents[i])
			continue;

		NiagaraComponents[i]->SetAutoActivate(false);
		NiagaraComponents[i]->SetAsset(NiagaraSystemAsset);
		NiagaraComponents[i]->SetupAttachment(Owner->GetRootComponent());
		NiagaraComponents[i]->RegisterComponent();
		NiagaraComponents[i]->SetVisibility(true);
		NiagaraComponents[i]->SetHiddenInGame(false);

		if (i == 0)
			NiagaraComponents[i]->SetRelativeLocation(FVector::ZeroVector);
		else
			NiagaraComponents[i]->SetRelativeLocation(FVector(0, 0, -1000000.0f));
	}

	ActiveComponentIndex = 0;
	UE_LOG(LogTemp, Log, TEXT("GSCompressedStream: Created %d Niagara components for N-buffering"), BufferCount);
}

void UGSCompressedStream::CleanupNiagaraComponents()
{
	for (int32 i = 0; i < NiagaraComponents.Num(); i++)
	{
		if (NiagaraComponents[i])
		{
			NiagaraComponents[i]->DeactivateImmediate();
			NiagaraComponents[i]->DestroyComponent();
			NiagaraComponents[i] = nullptr;
		}
	}
}

// ============================================================================
// Textures
// ============================================================================

UTexture2D* UGSCompressedStream::CreateHalfTexture(int32 W, int32 H, const TArray<uint8>& Data)
{
	const int32 Expected = W * H * 8;
	if (Data.Num() != Expected)
	{
		UE_LOG(LogTemp, Error, TEXT("GSCompressed: Tex data %d != expected %d"), Data.Num(), Expected);
		return nullptr;
	}

	UTexture2D* Tex = UTexture2D::CreateTransient(W, H, PF_FloatRGBA);
	if (!Tex) return nullptr;

	Tex->SRGB = false;
	Tex->CompressionSettings = TC_HDR;
	Tex->Filter = TF_Nearest;
	Tex->MipGenSettings = TMGS_NoMipmaps;
	Tex->NeverStream = true;

	FTexture2DMipMap& Mip = Tex->GetPlatformData()->Mips[0];
	void* Dst = Mip.BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(Dst, Data.GetData(), Data.Num());
	Mip.BulkData.Unlock();
	Tex->UpdateResource();

	return Tex;
}

bool UGSCompressedStream::LoadDecodedFrameToSlot(int32 SlotIndex, const FGSVDecompressedFrame& Frame)
{
	const int32 W = Frame.TextureWidth;

	FTransientFrame& Slot = TransientFrames[SlotIndex];
	Slot.Position = CreateHalfTexture(W, W, Frame.PositionData);
	Slot.Rotation = CreateHalfTexture(W, W, Frame.RotationData);
	Slot.ScaleOpacity = CreateHalfTexture(W, W, Frame.ScaleOpacityData);
	Slot.SH0 = CreateHalfTexture(W, W, Frame.SH0Data);

	Slot.SHRest.Empty();
	for (const auto& RestData : Frame.SHRestData)
	{
		Slot.SHRest.Add(CreateHalfTexture(W, W, RestData));
	}

	if (!Slot.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("GSCompressed: Failed to create textures for frame %d in slot %d"),
			Frame.FrameIndex, SlotIndex);
		Slot.Destroy();
		return false;
	}

	Slot.FrameIndex = Frame.FrameIndex;
	return true;
}

void UGSCompressedStream::ApplyFrameToComponent(int32 ComponentIndex)
{
	if (ComponentIndex < 0 || ComponentIndex >= NiagaraComponents.Num() || !NiagaraComponents[ComponentIndex])
		return;

	const FTransientFrame& Frame = TransientFrames[ComponentIndex];
	if (!Frame.IsValid())
		return;

	UNiagaraComponent* Comp = NiagaraComponents[ComponentIndex];
	Comp->SetVariableInt(FName("texture_width"), FixedTextureWidth);
	Comp->SetVariableTexture(FName("texture_position"), Frame.Position);
	Comp->SetVariableTexture(FName("texture_rotation"), Frame.Rotation);
	Comp->SetVariableTexture(FName("texture_scaleAndOpacity"), Frame.ScaleOpacity);
	Comp->SetVariableTexture(FName("texture_sh_0"), Frame.SH0);

	static const FName SHNames[] = {
		FName("texture_sh_1"), FName("texture_sh_2"), FName("texture_sh_3"),
		FName("texture_sh_4"), FName("texture_sh_5"), FName("texture_sh_6"),
		FName("texture_sh_7"), FName("texture_sh_8"), FName("texture_sh_9"),
		FName("texture_sh_10"), FName("texture_sh_11"),
	};

	for (int32 i = 0; i < Frame.SHRest.Num() && i < UE_ARRAY_COUNT(SHNames); i++)
	{
		Comp->SetVariableTexture(SHNames[i], Frame.SHRest[i]);
	}
}

void UGSCompressedStream::ClearNiagaraTextures(int32 ComponentIndex)
{
	if (ComponentIndex < 0 || ComponentIndex >= NiagaraComponents.Num() || !NiagaraComponents[ComponentIndex])
		return;

	UNiagaraComponent* Comp = NiagaraComponents[ComponentIndex];
	Comp->SetVariableTexture(FName("texture_position"), nullptr);
	Comp->SetVariableTexture(FName("texture_rotation"), nullptr);
	Comp->SetVariableTexture(FName("texture_scaleAndOpacity"), nullptr);
	Comp->SetVariableTexture(FName("texture_sh_0"), nullptr);

	static const FName SHNames[] = {
		FName("texture_sh_1"), FName("texture_sh_2"), FName("texture_sh_3"),
		FName("texture_sh_4"), FName("texture_sh_5"), FName("texture_sh_6"),
		FName("texture_sh_7"), FName("texture_sh_8"), FName("texture_sh_9"),
		FName("texture_sh_10"), FName("texture_sh_11"),
	};

	for (int32 i = 0; i < UE_ARRAY_COUNT(SHNames); i++)
	{
		Comp->SetVariableTexture(SHNames[i], nullptr);
	}
}

void UGSCompressedStream::DestroyFrame(int32 SlotIndex)
{
	if (SlotIndex < 0 || SlotIndex >= TransientFrames.Num())
		return;

	FTransientFrame& Frame = TransientFrames[SlotIndex];
	if (Frame.IsValid() && SlotIndex != ActiveComponentIndex)
	{
		ClearNiagaraTextures(SlotIndex);
	}
	Frame.Destroy();
	PreparedFrameIndex[SlotIndex] = -1;

	FramesDestroyedSinceGC++;
	if (FramesDestroyedSinceGC >= 30)
	{
		if (GEngine)
			GEngine->ForceGarbageCollection(false);
		FramesDestroyedSinceGC = 0;
	}
}

// ============================================================================
// Async Decompression + RAM Cache
// ============================================================================

void UGSCompressedStream::ScheduleDecompression()
{
	if (!bFileLoaded) return;

	TArray<int32> Wanted;
	Wanted.Add(CurrentFrameIndex);
	for (int32 i = 1; i <= DecompressAhead; i++)
	{
		int32 F = CurrentFrameIndex + i;
		if (PlaybackMode == EGSPlaybackMode::Loop)
			F = WrapFrameIndex(F);
		else if (F > GetEffectiveEndFrame())
			break;
		Wanted.AddUnique(F);
	}

	int32 InFlight = 0;
	for (auto& J : InFlightJobs)
	{
		if (J.IsValid() && !J->bDone.Load())
			InFlight++;
	}

	for (int32 F : Wanted)
	{
		if (DecodeCache.Contains(F))
			continue;

		bool bInFlight = false;
		for (auto& J : InFlightJobs)
		{
			if (J.IsValid() && J->FrameIndex == F)
			{
				bInFlight = true;
				break;
			}
		}
		if (bInFlight) continue;

		if (InFlight >= DecompressAhead) break;

		auto Job = MakeShared<FAsyncJob>();
		Job->FrameIndex = F;
		InFlightJobs.Add(Job);

		TSharedPtr<FGSVFileReader> Reader = GSVReader;
		FGSVFrameInfo Info = Reader->GetHeader().Frames[F];
		int32 FixedW = FixedTextureWidth;

		Async(EAsyncExecution::ThreadPool, [Job, Reader, Info, F, FixedW]()
		{
			TArray<uint8> Blob;
			if (Reader->ReadFrameBlob(F, Blob))
			{
				FGSVFrameInfo Adj = Info;
				Adj.TextureWidth = FixedW;
				auto Decoded = MakeShared<FGSVDecompressedFrame>();
				if (FGSVDecoder::DecodeFrame(Blob, Adj, F, Reader->GetHeader(), *Decoded))
				{
					Job->Result = Decoded;
				}
				else
				{
					UE_LOG(LogTemp, Error, TEXT("GSCompressed: DecodeFrame failed for frame %d"), F);
				}
			}
			else
			{
				UE_LOG(LogTemp, Error, TEXT("GSCompressed: ReadFrameBlob failed for frame %d"), F);
			}
			Job->bDone.Store(true);
		});

		InFlight++;
	}
}

void UGSCompressedStream::CollectDecompressedFrames()
{
	for (int32 i = InFlightJobs.Num() - 1; i >= 0; --i)
	{
		auto& Job = InFlightJobs[i];
		if (!Job.IsValid() || !Job->bDone.Load())
			continue;

		if (Job->Result.IsValid())
		{
			DecodeCache.Add(Job->FrameIndex, Job->Result);
		}

		InFlightJobs.RemoveAtSwap(i);
	}
}

void UGSCompressedStream::EvictOldCacheEntries()
{
	const int32 MaxCache = DecompressAhead + BufferCount + 2;
	if (DecodeCache.Num() <= MaxCache)
		return;

	TArray<int32> Keys;
	DecodeCache.GetKeys(Keys);

	Keys.Sort([this](int32 A, int32 B)
	{
		int32 DA = FMath::Abs(A - CurrentFrameIndex);
		int32 DB = FMath::Abs(B - CurrentFrameIndex);
		return DA < DB;
	});

	for (int32 i = MaxCache; i < Keys.Num(); i++)
	{
		DecodeCache.Remove(Keys[i]);
	}
}

// ============================================================================
// Visibility Swap (N-Buffered)
// ============================================================================

void UGSCompressedStream::UpdateVisibleFrame()
{
	if (NiagaraComponents.Num() == 0 || !NiagaraComponents[0])
		return;

	// Process pending hide from previous swap
	if (PendingHideComponentIndex >= 0 && PendingHideComponentIndex < NiagaraComponents.Num())
	{
		NiagaraComponents[PendingHideComponentIndex]->SetRelativeLocation(FVector(0, 0, -1000000.0f));
		NiagaraComponents[PendingHideComponentIndex]->Deactivate();
		DestroyFrame(PendingHideComponentIndex);
		PendingHideComponentIndex = -1;
	}

	// Case 1: active component already has the current frame
	if (PreparedFrameIndex[ActiveComponentIndex] == CurrentFrameIndex)
	{
		if (LastDisplayedFrame != CurrentFrameIndex)
		{
			UNiagaraComponent* ActiveComp = NiagaraComponents[ActiveComponentIndex];
			ActiveComp->SetRelativeLocation(FVector::ZeroVector);
			ActiveComp->Activate(true);
			ActiveComp->ReinitializeSystem();
			LastDisplayedFrame = CurrentFrameIndex;
			OnFrameChanged.Broadcast(CurrentFrameIndex);
		}
	}
	else
	{
		// Case 2: find the component that has the current frame
		int32 TargetComponentIndex = -1;
		for (int32 i = 0; i < BufferCount; i++)
		{
			if (PreparedFrameIndex[i] == CurrentFrameIndex)
			{
				TargetComponentIndex = i;
				break;
			}
		}

		if (TargetComponentIndex >= 0 && TargetComponentIndex != ActiveComponentIndex)
		{
			NiagaraComponents[TargetComponentIndex]->SetRelativeLocation(FVector::ZeroVector);
			NiagaraComponents[TargetComponentIndex]->Activate(true);
			NiagaraComponents[TargetComponentIndex]->ReinitializeSystem();

			PendingHideComponentIndex = ActiveComponentIndex;
			ActiveComponentIndex = TargetComponentIndex;
			LastDisplayedFrame = CurrentFrameIndex;
			OnFrameChanged.Broadcast(CurrentFrameIndex);
		}
	}

	PrepareNextFrames();
}

void UGSCompressedStream::PrepareNextFrames()
{
	if (!bFileLoaded) return;

	TArray<int32> FramesToPrepare;
	FramesToPrepare.Add(CurrentFrameIndex);
	for (int32 i = 1; i < BufferCount; i++)
	{
		int32 NextFrame = CurrentFrameIndex + i;
		if (PlaybackMode == EGSPlaybackMode::Loop)
			NextFrame = WrapFrameIndex(NextFrame);
		else if (NextFrame > GetEffectiveEndFrame())
			break;
		FramesToPrepare.AddUnique(NextFrame);
	}

	TSet<int32> AlreadyPrepared;
	for (int32 i = 0; i < BufferCount; i++)
	{
		if (PreparedFrameIndex[i] >= 0)
			AlreadyPrepared.Add(PreparedFrameIndex[i]);
	}

	for (int32 NeededFrame : FramesToPrepare)
	{
		if (AlreadyPrepared.Contains(NeededFrame))
			continue;

		// Is the decoded data available in the RAM cache?
		TSharedPtr<FGSVDecompressedFrame>* Found = DecodeCache.Find(NeededFrame);
		if (!Found || !Found->IsValid())
			continue;

		// Find an available component slot
		int32 AvailableSlot = -1;
		for (int32 i = 0; i < BufferCount; i++)
		{
			if (i == ActiveComponentIndex && AlreadyPrepared.Num() > 0)
				continue;
			if (PreparedFrameIndex[i] < 0)
			{
				AvailableSlot = i;
				break;
			}
			if (!FramesToPrepare.Contains(PreparedFrameIndex[i]))
			{
				AvailableSlot = i;
			}
		}

		if (AvailableSlot < 0)
			break;

		if (TransientFrames[AvailableSlot].IsValid())
		{
			DestroyFrame(AvailableSlot);
		}

		if (!LoadDecodedFrameToSlot(AvailableSlot, **Found))
			continue;

		ApplyFrameToComponent(AvailableSlot);
		PreparedFrameIndex[AvailableSlot] = NeededFrame;
		AlreadyPrepared.Add(NeededFrame);

		// Only prepare one frame per tick to spread the cost
		break;
	}
}

// ============================================================================
// Tick
// ============================================================================

void UGSCompressedStream::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (bEnableKeyboardControl)
		HandleKeyboardInput(DeltaTime);

	if (bUICreated)
		UpdatePlaybackUI();

	if (!bFileLoaded)
		return;

	// 1. Collect completed async jobs into RAM cache
	CollectDecompressedFrames();

	// 2. Update visible frame and prepare next frames on Niagara components
	UpdateVisibleFrame();

	// 3. Schedule decompression for upcoming frames
	ScheduleDecompression();

	// 4. Evict old cache entries to bound RAM usage
	EvictOldCacheEntries();

	// 5. Advance playback
	if (bIsPlaying)
	{
		const float FPS = GSVReader->GetTargetFPS() * PlaybackSpeed;
		if (FPS > 0.0f)
		{
			const float Duration = 1.0f / FPS;
			FrameAccumulator += DeltaTime;
			FrameAccumulator = FMath::Min(FrameAccumulator, Duration * 2.0f);

			if (FrameAccumulator >= Duration)
			{
				int32 Next = CurrentFrameIndex + 1;
				if (PlaybackMode == EGSPlaybackMode::Loop)
					Next = WrapFrameIndex(Next);
				else if (Next > GetEffectiveEndFrame())
				{
					bIsPlaying = false;
					OnSequenceComplete.Broadcast();
					return;
				}

				// Only advance if the next frame is prepared on a Niagara component
				bool bNextReady = false;
				for (int32 i = 0; i < BufferCount; i++)
				{
					if (PreparedFrameIndex[i] == Next)
					{
						bNextReady = true;
						break;
					}
				}

				if (bNextReady)
				{
					CurrentFrameIndex = Next;
					FrameAccumulator -= Duration;
				}
			}
		}
	}
}

// ============================================================================
// File Loading
// ============================================================================

bool UGSCompressedStream::LoadGSVFile(const FString& FilePath)
{
	Stop();
	bFileLoaded = false;

	const FString ResolvedPath = ResolveGSVPath(FilePath);

	GSVReader = MakeShared<FGSVFileReader>();
	if (!GSVReader->Open(ResolvedPath))
	{
		GSVReader.Reset();
		return false;
	}

	if (bPreloadToRAM)
		GSVReader->PreloadToRAM();

	FixedTextureWidth = GSVReader->GetHeader().MaxTextureWidth;
	if (FixedTextureWidth <= 0)
	{
		UE_LOG(LogTemp, Error, TEXT("GSCompressed: Invalid max_texture_width"));
		GSVReader.Reset();
		return false;
	}

	bFileLoaded = true;
	CurrentFrameIndex = GetEffectiveStartFrame();

	UE_LOG(LogTemp, Log, TEXT("GSCompressed: Loaded '%s' — %d frames, %.0f FPS, tex %dx%d"),
		*ResolvedPath, GSVReader->GetFrameCount(), GSVReader->GetTargetFPS(),
		FixedTextureWidth, FixedTextureWidth);

	return true;
}

// ============================================================================
// Playback
// ============================================================================

void UGSCompressedStream::Play()
{
	if (!bFileLoaded) return;
	bIsPlaying = true;
}

void UGSCompressedStream::Pause()
{
	bIsPlaying = false;
}

void UGSCompressedStream::Stop()
{
	bIsPlaying = false;
	FrameAccumulator = 0.0f;
	CurrentFrameIndex = GetEffectiveStartFrame();
	LastDisplayedFrame = -1;
	PendingHideComponentIndex = -1;
	FramesDestroyedSinceGC = 0;

	InFlightJobs.Empty();
	DecodeCache.Empty();

	ActiveComponentIndex = 0;
	for (int32 i = 0; i < TransientFrames.Num(); i++)
	{
		PreparedFrameIndex[i] = -1;
		if (TransientFrames[i].IsValid())
		{
			ClearNiagaraTextures(i);
			TransientFrames[i].Destroy();
		}
	}

	for (int32 i = 0; i < NiagaraComponents.Num(); i++)
	{
		if (NiagaraComponents[i])
		{
			NiagaraComponents[i]->Deactivate();
			if (i == 0)
				NiagaraComponents[i]->SetRelativeLocation(FVector::ZeroVector);
			else
				NiagaraComponents[i]->SetRelativeLocation(FVector(0, 0, -1000000.0f));
		}
	}

	if (GEngine)
		GEngine->ForceGarbageCollection(true);
}

void UGSCompressedStream::GoToFrame(int32 Frame)
{
	if (!bFileLoaded) return;
	CurrentFrameIndex = FMath::Clamp(Frame, GetEffectiveStartFrame(), GetEffectiveEndFrame());
	FrameAccumulator = 0.0f;
}

void UGSCompressedStream::TogglePlayPause()
{
	if (bIsPlaying) Pause(); else Play();
}

int32 UGSCompressedStream::GetFrameCount() const
{
	if (!bFileLoaded) return 0;
	return FMath::Max(0, GetEffectiveEndFrame() - GetEffectiveStartFrame() + 1);
}

int32 UGSCompressedStream::GetEffectiveStartFrame() const
{
	if (!bFileLoaded) return 0;
	int32 M = GSVReader->GetFrameCount() - 1;
	return (StartFrame < 0) ? 0 : FMath::Clamp(StartFrame, 0, M);
}

int32 UGSCompressedStream::GetEffectiveEndFrame() const
{
	if (!bFileLoaded) return 0;
	int32 M = GSVReader->GetFrameCount() - 1;
	return (EndFrame < 0) ? M : FMath::Clamp(EndFrame, 0, M);
}

int32 UGSCompressedStream::WrapFrameIndex(int32 Index) const
{
	if (!bFileLoaded) return 0;
	int32 S = GetEffectiveStartFrame();
	int32 R = GetEffectiveEndFrame() - S + 1;
	int32 Rel = (Index - S) % R;
	if (Rel < 0) Rel += R;
	return S + Rel;
}

// ============================================================================
// File Switching
// ============================================================================

bool UGSCompressedStream::SwitchFile(int32 Index)
{
	if (Index < 0 || Index >= FileList.Num()) return false;
	if (Index == ActiveFileIndex && bFileLoaded) return true;

	const FGSVFileEntry& E = FileList[Index];
	if (E.GSVFilePath.FilePath.IsEmpty()) return false;

	bool bWas = bIsPlaying;
	Stop();

	if (!LoadGSVFile(E.GSVFilePath.FilePath))
	{
		ActiveFileIndex = -1;
		return false;
	}

	ActiveFileIndex = Index;
	OnFileSwitched.Broadcast(Index, E.DisplayName);
	if (bWas || bAutoPlay) Play();
	return true;
}

void UGSCompressedStream::NextFile()
{
	if (FileList.Num() == 0) return;
	SwitchFile((ActiveFileIndex + 1) % FileList.Num());
}

void UGSCompressedStream::PreviousFile()
{
	if (FileList.Num() == 0) return;
	int32 P = ActiveFileIndex - 1;
	if (P < 0) P = FileList.Num() - 1;
	SwitchFile(P);
}

FString UGSCompressedStream::GetActiveFileName() const
{
	if (ActiveFileIndex >= 0 && ActiveFileIndex < FileList.Num())
		return FileList[ActiveFileIndex].DisplayName;
	if (bFileLoaded)
		return FPaths::GetBaseFilename(GSVFilePath.FilePath);
	return TEXT("");
}

// ============================================================================
// Keyboard
// ============================================================================

void UGSCompressedStream::HandleKeyboardInput(float DeltaTime)
{
	if (!bFileLoaded) return;
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC) return;

	bool bP = PauseKey.IsValid() && PC->IsInputKeyDown(PauseKey);
	if (bP && !bPrevPausePressed) TogglePlayPause();
	bPrevPausePressed = bP;

	auto StepFrame = [&](bool bDown, bool& bPrev, float& Hold, int32 Dir)
	{
		if (bDown && !bIsPlaying)
		{
			if (!bPrev)
			{
				int32 F = CurrentFrameIndex + Dir;
				if (Dir < 0 && F < GetEffectiveStartFrame()) F = GetEffectiveEndFrame();
				if (Dir > 0 && F > GetEffectiveEndFrame()) F = GetEffectiveStartFrame();
				GoToFrame(F);
				Hold = 0.0f;
				KeyRepeatAccumulator = 0.0f;
			}
			else
			{
				Hold += DeltaTime;
				if (Hold >= KeyHoldDelay)
				{
					KeyRepeatAccumulator += DeltaTime;
					while (KeyRepeatAccumulator >= KeyRepeatRate)
					{
						KeyRepeatAccumulator -= KeyRepeatRate;
						int32 F = CurrentFrameIndex + Dir;
						if (Dir < 0 && F < GetEffectiveStartFrame()) F = GetEffectiveEndFrame();
						if (Dir > 0 && F > GetEffectiveEndFrame()) F = GetEffectiveStartFrame();
						GoToFrame(F);
					}
				}
			}
		}
		else { Hold = 0.0f; }
		bPrev = bDown;
	};

	bool bL = PrevFrameKey.IsValid() && PC->IsInputKeyDown(PrevFrameKey);
	StepFrame(bL, bPrevLeftPressed, PrevKeyHoldTime, -1);

	bool bR = NextFrameKey.IsValid() && PC->IsInputKeyDown(NextFrameKey);
	StepFrame(bR, bPrevRightPressed, NextKeyHoldTime, +1);

	if (FileList.Num() > 1)
	{
		bool bPF = PrevFileKey.IsValid() && PC->IsInputKeyDown(PrevFileKey);
		if (bPF && !bPrevPrevFilePressed) PreviousFile();
		bPrevPrevFilePressed = bPF;

		bool bNF = NextFileKey.IsValid() && PC->IsInputKeyDown(NextFileKey);
		if (bNF && !bPrevNextFilePressed) NextFile();
		bPrevNextFilePressed = bNF;
	}
}

// ============================================================================
// UI
// ============================================================================

void UGSCompressedStream::CreatePlaybackUI()
{
	if (bUICreated || !GetWorld()) return;
	UGameViewportClient* VC = GetWorld()->GetGameViewport();
	if (!VC) return;

	TWeakObjectPtr<UGSCompressedStream> W(this);

	PlaybackUIWidget = SNew(SOverlay)
	+ SOverlay::Slot()
	.HAlign(HAlign_Center).VAlign(VAlign_Bottom)
	.Padding(FMargin(0, 0, 0, UIVerticalOffset))
	[
		SNew(SBorder)
		.BorderBackgroundColor(FLinearColor(0.1f, 0.1f, 0.1f, 0.8f))
		.Padding(FMargin(10, 5))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0, 0, 0, 3)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(3, 0)
				[
					SNew(SButton)
					.Visibility_Lambda([W]() -> EVisibility {
						return (W.IsValid() && W->FileList.Num() > 1) ? EVisibility::Visible : EVisibility::Collapsed;
					})
					.OnClicked_Lambda([W]() -> FReply {
						if (W.IsValid()) W->PreviousFile();
						return FReply::Handled();
					})
					[ SNew(STextBlock).Text(FText::FromString(TEXT(" << "))).Font(FCoreStyle::GetDefaultFontStyle("Bold", 12)) ]
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(5, 0).VAlign(VAlign_Center)
				[
					SAssignNew(FileNameTextBlock, STextBlock)
					.Text(FText::FromString(GetActiveFileName()))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
					.ColorAndOpacity(FLinearColor(0.8f, 0.8f, 1.0f))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(3, 0)
				[
					SNew(SButton)
					.Visibility_Lambda([W]() -> EVisibility {
						return (W.IsValid() && W->FileList.Num() > 1) ? EVisibility::Visible : EVisibility::Collapsed;
					})
					.OnClicked_Lambda([W]() -> FReply {
						if (W.IsValid()) W->NextFile();
						return FReply::Handled();
					})
					[ SNew(STextBlock).Text(FText::FromString(TEXT(" >> "))).Font(FCoreStyle::GetDefaultFontStyle("Bold", 12)) ]
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(5)
				[
					SNew(SButton)
					.OnClicked_Lambda([W]() -> FReply {
						if (W.IsValid() && !W->bIsPlaying && W->bFileLoaded) {
							int32 F = W->CurrentFrameIndex - 1;
							if (F < W->GetEffectiveStartFrame()) F = W->GetEffectiveEndFrame();
							W->GoToFrame(F);
						}
						return FReply::Handled();
					})
					[ SNew(STextBlock).Text(FText::FromString(TEXT("  <  "))).Font(FCoreStyle::GetDefaultFontStyle("Bold", 16)) ]
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(5)
				[
					SNew(SButton)
					.OnClicked_Lambda([W]() -> FReply {
						if (W.IsValid()) W->TogglePlayPause();
						return FReply::Handled();
					})
					[
						SAssignNew(PlayPauseTextBlock, STextBlock)
						.Text(FText::FromString(bIsPlaying ? TEXT(" PAUSE ") : TEXT(" PLAY ")))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(5)
				[
					SNew(SButton)
					.OnClicked_Lambda([W]() -> FReply {
						if (W.IsValid() && !W->bIsPlaying && W->bFileLoaded) {
							int32 F = W->CurrentFrameIndex + 1;
							if (F > W->GetEffectiveEndFrame()) F = W->GetEffectiveStartFrame();
							W->GoToFrame(F);
						}
						return FReply::Handled();
					})
					[ SNew(STextBlock).Text(FText::FromString(TEXT("  >  "))).Font(FCoreStyle::GetDefaultFontStyle("Bold", 16)) ]
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(15, 5, 5, 5).VAlign(VAlign_Center)
				[
					SAssignNew(FrameCounterTextBlock, STextBlock)
					.Text(FText::FromString(TEXT("0 / 0")))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 14))
					.ColorAndOpacity(FLinearColor::White)
				]
			]
		]
	];

	VC->AddViewportWidgetContent(SNew(SWeakWidget).PossiblyNullContent(PlaybackUIWidget));

	if (bShowMouseCursor)
	{
		if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
		{
			PC->bShowMouseCursor = true;
			PC->SetInputMode(FInputModeGameAndUI().SetHideCursorDuringCapture(false));
		}
	}

	bUICreated = true;
}

void UGSCompressedStream::DestroyPlaybackUI()
{
	if (PlaybackUIWidget.IsValid() && GetWorld())
	{
		if (UGameViewportClient* VC = GetWorld()->GetGameViewport())
			VC->RemoveViewportWidgetContent(SNew(SWeakWidget).PossiblyNullContent(PlaybackUIWidget));
	}
	PlaybackUIWidget.Reset();
	PlayPauseTextBlock.Reset();
	FrameCounterTextBlock.Reset();
	FileNameTextBlock.Reset();
	bUICreated = false;
}

void UGSCompressedStream::UpdatePlaybackUI()
{
	if (!bUICreated) return;

	if (PlayPauseTextBlock.IsValid())
		PlayPauseTextBlock->SetText(FText::FromString(bIsPlaying ? TEXT(" PAUSE ") : TEXT(" PLAY ")));

	if (FrameCounterTextBlock.IsValid())
	{
		int32 Tot = bFileLoaded ? GetFrameCount() : 0;
		int32 Cur = bFileLoaded ? (CurrentFrameIndex - GetEffectiveStartFrame() + 1) : 0;
		FrameCounterTextBlock->SetText(FText::FromString(FString::Printf(TEXT("%d / %d"), Cur, Tot)));
	}

	if (FileNameTextBlock.IsValid())
	{
		FString N = GetActiveFileName();
		if (FileList.Num() > 1)
			N = FString::Printf(TEXT("%d/%d: %s"), ActiveFileIndex + 1, FileList.Num(), *N);
		FileNameTextBlock->SetText(FText::FromString(N));
	}
}
