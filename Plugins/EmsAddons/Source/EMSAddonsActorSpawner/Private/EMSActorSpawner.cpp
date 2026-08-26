//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSActorSpawner.h"

#include "Components/ActorComponent.h"
#include "Components/BillboardComponent.h"
#include "Components/SceneComponent.h"
#include "EMSAddonsActorSpawner.h"
#include "EMSAddonsActorBinary.h"
#include "EMSAddonsGameThread.h"
#include "EMSActors.h"
#include "EMSData.h"
#include "EMSObject.h"
#include "EMSTypes.h"
#include "Engine/Level.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Misc/SecureHash.h"
#include "UObject/ConstructorHelpers.h"
#include "WorldPartition/ActorInstanceGuids.h"

#define LOCTEXT_NAMESPACE "EMSActorSpawner"

namespace
{
	const FString SpawnerOwnerTagPrefix(TEXT("EMS_SpawnerOwner="));
	const FString SpawnedActorTagPrefix(TEXT("EMS_SpawnedActor="));

	bool TryGetSingleGuidTag(const AActor* Actor, const FString& Prefix, FGuid& OutGuid)
	{
		OutGuid.Invalidate();
		if (!Actor)
		{
			return false;
		}

		int32 MatchingTagCount = 0;
		FGuid ParsedGuid;
		for (const FName& Tag : Actor->Tags)
		{
			const FString TagString = Tag.ToString();
			if (!TagString.StartsWith(Prefix))
			{
				continue;
			}

			++MatchingTagCount;
			FGuid CandidateGuid;
			if (!FGuid::ParseExact(TagString.RightChop(Prefix.Len()), EGuidFormats::Digits, CandidateGuid)
				|| !CandidateGuid.IsValid())
			{
				return false;
			}
			ParsedGuid = CandidateGuid;
		}

		if (MatchingTagCount != 1)
		{
			return false;
		}

		OutGuid = ParsedGuid;
		return true;
	}

	bool HasTagWithPrefix(const AActor* Actor, const FString& Prefix)
	{
		return Actor && Actor->Tags.ContainsByPredicate(
			[&Prefix](const FName& Tag)
			{
				return Tag.ToString().StartsWith(Prefix);
			});
	}

	void RemoveTagsWithPrefix(AActor* Actor, const FString& Prefix)
	{
		if (!Actor)
		{
			return;
		}

		Actor->Tags.RemoveAll(
			[&Prefix](const FName& Tag)
			{
				return Tag.ToString().StartsWith(Prefix);
			});
	}

	bool IsTeardownReason(const EEndPlayReason::Type EndPlayReason)
	{
		return EndPlayReason == EEndPlayReason::LevelTransition
			|| EndPlayReason == EEndPlayReason::EndPlayInEditor
			|| EndPlayReason == EEndPlayReason::RemovedFromWorld
			|| EndPlayReason == EEndPlayReason::Quit;
	}

}

AEMSActorSpawner::AEMSActorSpawner()
{
	PrimaryActorTick.bCanEverTick = false;

	SpawnerRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SpawnerRoot"));
	SetRootComponent(SpawnerRoot);

#if WITH_EDITORONLY_DATA
	SpawnerSprite = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("SpawnerSprite"));
	if (SpawnerSprite)
	{
		static ConstructorHelpers::FObjectFinder<UTexture2D> SpriteTexture(TEXT("/Engine/EditorResources/S_TargetPoint"));
		if (SpriteTexture.Succeeded())
		{
			SpawnerSprite->SetSprite(SpriteTexture.Object);
		}
		SpawnerSprite->SpriteInfo.Category = TEXT("EasyMultiSave");
		SpawnerSprite->SpriteInfo.DisplayName = LOCTEXT("EMSSpriteCategory", "Easy Multi Save");
		SpawnerSprite->bIsScreenSizeScaled = true;
		SpawnerSprite->SetupAttachment(SpawnerRoot);
	}
#endif

	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		SpawnerId = FGuid::NewGuid();
	}
}

void AEMSActorSpawner::BeginPlay()
{
	Super::BeginPlay();

	// A sublevel that is hidden and shown again routes End Play and then Begin
	// Play on the same actor, so this has to clear or the spawner stays disabled
	// for the rest of the session after the first hide.
	bIsEndingPlay = false;
	if (!CanMutateSpawnedActors())
	{
		return;
	}

	EnsureSpawnerIdentity();
	EnsureActorDestroyedHandler();
	RebuildLiveBindings();
}

void AEMSActorSpawner::EnsureActorDestroyedHandler()
{
	if (ActorDestroyedHandle.IsValid())
	{
		return;
	}
	if (UWorld* World = GetWorld())
	{
		ActorDestroyedHandle = World->AddOnActorDestroyedHandler(
			FOnActorDestroyed::FDelegate::CreateUObject(this, &AEMSActorSpawner::HandleChildDestroyed));
	}
}

void AEMSActorSpawner::RemoveActorDestroyedHandler()
{
	if (!ActorDestroyedHandle.IsValid())
	{
		return;
	}
	if (UWorld* World = GetWorld())
	{
		World->RemoveOnActorDestroyedHandler(ActorDestroyedHandle);
	}
	ActorDestroyedHandle.Reset();
}

#if WITH_EDITOR
void AEMSActorSpawner::PostDuplicate(const bool bDuplicateForPIE)
{
	Super::PostDuplicate(bDuplicateForPIE);
	if (!bDuplicateForPIE && !HasAnyFlags(RF_ClassDefaultObject))
	{
		SpawnerId = FGuid::NewGuid();
	}
}
#endif

FGuid AEMSActorSpawner::GetSpawnerIdentity() const
{
	if (!SpawnerId.IsValid())
	{
		return FGuid();
	}

	const FGuid LevelInstanceGuid = FActorInstanceGuid::GetLevelInstanceGuid(GetLevel());
	if (!LevelInstanceGuid.IsValid())
	{
		return SpawnerId;
	}

	const FString IdentitySeed = FString::Printf(
		TEXT("%s:%s"),
		*SpawnerId.ToString(EGuidFormats::Digits),
		*LevelInstanceGuid.ToString(EGuidFormats::Digits));
	FGuid ResolvedIdentity;
	FGuid::ParseExact(FMD5::HashAnsiString(*IdentitySeed), EGuidFormats::Digits, ResolvedIdentity);
	return ResolvedIdentity;
}

void AEMSActorSpawner::EnsureSpawnerIdentity()
{
	const FGuid PreviousIdentity = GetSpawnerIdentity();
	if (!SpawnerId.IsValid())
	{
		SpawnerId = FGuid::NewGuid();
	}

	const ULevel* SpawnerLevel = GetLevel();
	if (SpawnerLevel)
	{
		auto IsDuplicateIdentity = [this, SpawnerLevel]()
		{
			const FGuid Identity = GetSpawnerIdentity();
			for (const AActor* LevelActor : SpawnerLevel->Actors)
			{
				const AEMSActorSpawner* OtherSpawner = Cast<AEMSActorSpawner>(LevelActor);
				if (IsValid(OtherSpawner)
					&& OtherSpawner != this
					&& OtherSpawner->SpawnerId.IsValid()
					&& OtherSpawner->GetSpawnerIdentity() == Identity)
				{
					return true;
				}
			}
			return false;
		};

		while (IsDuplicateIdentity())
		{
			SpawnerId = FGuid::NewGuid();
		}
	}

	if (PreviousIdentity.IsValid() && PreviousIdentity != GetSpawnerIdentity())
	{
		RetagLiveActorsForIdentityChange(PreviousIdentity);
	}
}

void AEMSActorSpawner::RetagLiveActorsForIdentityChange(const FGuid& PreviousIdentity)
{
	const FGuid CurrentIdentity = GetSpawnerIdentity();
	if (!PreviousIdentity.IsValid() || !CurrentIdentity.IsValid() || PreviousIdentity == CurrentIdentity)
	{
		return;
	}

	for (const TPair<FGuid, TObjectPtr<AActor>>& Pair : LiveActorsById)
	{
		AActor* Actor = Pair.Value.Get();
		FGuid TaggedOwnerId;
		if (!IsValid(Actor)
			|| !TryGetOwnerIdFromActorTag(Actor, TaggedOwnerId)
			|| TaggedOwnerId != PreviousIdentity)
		{
			continue;
		}

		RemoveTagsWithPrefix(Actor, SpawnerOwnerTagPrefix);
		RemoveTagsWithPrefix(Actor, SpawnedActorTagPrefix);
		Actor->Tags.Add(FName(*(SpawnerOwnerTagPrefix + CurrentIdentity.ToString(EGuidFormats::Digits))));
		Actor->Tags.Add(FName(*(SpawnedActorTagPrefix + Pair.Key.ToString(EGuidFormats::Digits))));
		Actor->Tags.AddUnique(EMS::SkipSaveTag);
		Actor->Tags.Remove(EMS::PersistentTag);
	}
}

void AEMSActorSpawner::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bIsEndingPlay = true;
	RemoveActorDestroyedHandler();

	if (EndPlayReason == EEndPlayReason::Destroyed)
	{
		TArray<FGuid> ActorIds;
		LiveActorsById.GetKeys(ActorIds);
		for (const FGuid& ActorId : ActorIds)
		{
			DestroyLiveActor(ActorId, EEMSSpawnerDestructionContext::Internal, false);
		}
	}

	LiveActorsById.Empty();
	LiveIdsByActor.Empty();
	Super::EndPlay(EndPlayReason);
}

bool AEMSActorSpawner::CanMutateSpawnedActors() const
{
	return IsInGameThread()
		&& !HasAnyFlags(RF_ClassDefaultObject)
		&& IsValid(GetWorld())
		&& HasAuthority()
		&& !bIsEndingPlay;
}

bool AEMSActorSpawner::IsRecordValid(const FEMSSpawnedActorRecord& Record, FText& OutReason) const
{
	if (!Record.LocalId.IsValid())
	{
		OutReason = LOCTEXT("InvalidRecordId", "The spawned actor ID is invalid.");
		return false;
	}
	if (Record.RecordVersion != FEMSSpawnedActorRecord::CurrentVersion)
	{
		OutReason = LOCTEXT("UnsupportedRecordVersion", "The spawned actor record version is unsupported.");
		return false;
	}
	if (!Record.SavedTransform.IsValid() || !Record.RelativeAttachmentTransform.IsValid())
	{
		OutReason = LOCTEXT("InvalidRecordTransform", "The spawned actor record contains an invalid transform.");
		return false;
	}
	if (Record.bExists)
	{
		UClass* ActorClass = FSpawnHelpers::ResolveSpawnClass(Record.ActorClass.ToString());
		if (!ActorClass || !ActorClass->IsChildOf(AActor::StaticClass())
			|| ActorClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			OutReason = LOCTEXT("InvalidRecordClass", "The saved actor class cannot be spawned.");
			return false;
		}
	}

	OutReason = FText::GetEmpty();
	return true;
}

AActor* AEMSActorSpawner::SpawnActor(
	TSubclassOf<AActor> ActorClass,
	const FTransform& WorldTransform,
	FEMSSpawnedActorId& OutActorId)
{
	OutActorId = FEMSSpawnedActorId();
	if (!CanMutateSpawnedActors() || !ActorClass || !WorldTransform.IsValid())
	{
		return nullptr;
	}

	FGuid LocalId;
	do
	{
		LocalId = FGuid::NewGuid();
	}
	while (FindRecord(LocalId) || LiveActorsById.Contains(LocalId));

	const FEMSSpawnedActorId CandidateActorId(LocalId);
	AActor* SpawnedActor = SpawnActorWithId(ActorClass, CandidateActorId, WorldTransform);
	if (SpawnedActor)
	{
		OutActorId = CandidateActorId;
		OnActorSpawned.Broadcast(CandidateActorId, SpawnedActor);
	}
	return SpawnedActor;
}

AActor* AEMSActorSpawner::SpawnActorWithId(
	TSubclassOf<AActor> ActorClass,
	FEMSSpawnedActorId ActorId,
	const FTransform& WorldTransform)
{
	if (!CanMutateSpawnedActors() || !ActorClass || !ActorId.IsValid() || !WorldTransform.IsValid())
	{
		return nullptr;
	}
	if (AActor* ExistingActor = GetSpawnedActor(ActorId))
	{
		return ExistingActor;
	}

	FEMSSpawnedActorRecord* Record = FindRecordMutable(ActorId.Guid);
	const bool bAddedRecord = Record == nullptr;
	FEMSSpawnedActorRecord PreviousRecord;
	if (!Record)
	{
		if (SpawnedActorManifest.Num() >= MaxSpawnedActors)
		{
			// Records of destroyed actors are otherwise only dropped during a
			// capture, so a spawn-and-destroy loop between two saves would reach
			// the limit with almost nothing actually alive.
			PruneObsoleteAbsentRecords();
		}
		if (SpawnedActorManifest.Num() >= MaxSpawnedActors)
		{
			UE_LOG(LogEMSAddonsActorSpawner, Warning, TEXT("Spawned actor limit reached. Spawner=%s Limit=%d"), *GetPathName(), MaxSpawnedActors);
			return nullptr;
		}
		Record = &SpawnedActorManifest.AddDefaulted_GetRef();
		Record->LocalId = ActorId.Guid;
	}
	else
	{
		PreviousRecord = *Record;
	}

	Record->ActorClass = FSoftClassPath(ActorClass.Get());
	Record->SavedTransform = WorldTransform;
	Record->bExists = true;
	Record->RecordVersion = FEMSSpawnedActorRecord::CurrentVersion;
	AActor* SpawnedActor = SpawnActorForRecord(*Record);

	// Spawning runs construction scripts and Begin Play, which can spawn through
	// this same spawner and either grow the manifest or prune it. Either moves
	// the elements, so the pointer from before that call is re-found here rather
	// than reused.
	Record = FindRecordMutable(ActorId.Guid);
	if (!SpawnedActor)
	{
		if (bAddedRecord)
		{
			SpawnedActorManifest.RemoveAll(
				[&ActorId](const FEMSSpawnedActorRecord& Candidate)
				{
					return Candidate.LocalId == ActorId.Guid;
				});
		}
		else if (Record)
		{
			*Record = MoveTemp(PreviousRecord);
		}
	}
	return SpawnedActor;
}

// Takes the record by value. Spawning below runs construction scripts and Begin
// Play, which can spawn through this same spawner and move the manifest's
// elements, so this must not hold a reference into it across that call.
AActor* AEMSActorSpawner::SpawnActorForRecord(FEMSSpawnedActorRecord Record, bool bRestore)
{
	if (!CanMutateSpawnedActors())
	{
		return nullptr;
	}
	if (AActor* ExistingActor = GetSpawnedActor(FEMSSpawnedActorId(Record.LocalId)))
	{
		return ExistingActor;
	}

	FText ValidationReason;
	if (!IsRecordValid(Record, ValidationReason))
	{
		return nullptr;
	}

	UClass* ActorClass = FSpawnHelpers::ResolveSpawnClass(Record.ActorClass.ToString());
	UWorld* World = GetWorld();
	ULevel* TargetLevel = GetLevel();
	if (!World || !TargetLevel || !ActorClass)
	{
		return nullptr;
	}

	const FName StableName = MakeStableActorName(Record.LocalId);
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.OverrideLevel = TargetLevel;
	SpawnParameters.Name = StableName;
	SpawnParameters.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Required_ErrorAndReturnNull;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParameters.bDeferConstruction = bRestore;

	AActor* SpawnedActor = World->SpawnActor<AActor>(ActorClass, Record.SavedTransform, SpawnParameters);
	if (SpawnedActor && bRestore)
	{
		UEMSObject* EMSObject = ResolveEMSObject();
		if (!EMSObject)
		{
			SpawnedActor->Destroy();
			return nullptr;
		}
		// Guarded like EMS Core does it: a spawned actor is not required to
		// implement the save interface, and Execute_ asserts that it does.
		if (UEMSObject::HasSaveInterface(SpawnedActor))
		{
			IEMSActorSaveInterface::Execute_ActorPreLoad(SpawnedActor);
		}
		if (IsValid(SpawnedActor))
		{
			SpawnedActor->FinishSpawning(Record.SavedTransform, false);
		}
	}
	if (!IsValid(SpawnedActor) || SpawnedActor->GetLevel() != TargetLevel)
	{
		if (SpawnedActor)
		{
			SpawnedActor->Destroy();
		}
		return nullptr;
	}

	if (!RegisterLiveActor(Record.LocalId, SpawnedActor))
	{
		SpawnedActor->Destroy();
		return nullptr;
	}

	ApplyRecordState(SpawnedActor, Record);
	return SpawnedActor;
}

bool AEMSActorSpawner::RegisterLiveActor(const FGuid& LocalId, AActor* Actor)
{
	if (!LocalId.IsValid() || !IsValid(Actor))
	{
		return false;
	}
	EnsureActorDestroyedHandler();
	if (const TObjectPtr<AActor>* ExistingActor = LiveActorsById.Find(LocalId))
	{
		if (IsValid(*ExistingActor) && *ExistingActor != Actor)
		{
			return false;
		}
	}

	const FGuid CurrentIdentity = GetSpawnerIdentity();
	FGuid TaggedOwnerIdentity;
	const bool bHasOwnerTag = TryGetOwnerIdFromActorTag(Actor, TaggedOwnerIdentity);
	const bool bHasAnyOwnerTag = HasTagWithPrefix(Actor, SpawnerOwnerTagPrefix);
	if ((bHasOwnerTag && TaggedOwnerIdentity != CurrentIdentity) || (bHasAnyOwnerTag && !bHasOwnerTag))
	{
		return false;
	}
	Actor->Tags.AddUnique(EMS::SkipSaveTag);
	Actor->Tags.Remove(EMS::PersistentTag);
	RemoveTagsWithPrefix(Actor, SpawnerOwnerTagPrefix);
	RemoveTagsWithPrefix(Actor, SpawnedActorTagPrefix);
	Actor->Tags.Add(FName(*(SpawnerOwnerTagPrefix + CurrentIdentity.ToString(EGuidFormats::Digits))));
	Actor->Tags.Add(FName(*(SpawnedActorTagPrefix + LocalId.ToString(EGuidFormats::Digits))));
	Actor->OnEndPlay.AddUniqueDynamic(this, &AEMSActorSpawner::HandleChildEndPlay);
	LiveActorsById.Add(LocalId, Actor);
	LiveIdsByActor.Add(FObjectKey(Actor), LocalId);
	return true;
}

void AEMSActorSpawner::UnregisterLiveActor(AActor* Actor)
{
	if (!Actor)
	{
		return;
	}
	const FObjectKey ActorKey(Actor);
	if (const FGuid* LocalId = LiveIdsByActor.Find(ActorKey))
	{
		LiveActorsById.Remove(*LocalId);
		LiveIdsByActor.Remove(ActorKey);
	}
	Actor->OnEndPlay.RemoveDynamic(this, &AEMSActorSpawner::HandleChildEndPlay);
}

bool AEMSActorSpawner::TryGetOwnerIdFromActorTag(const AActor* Actor, FGuid& OutId) const
{
	return TryGetSingleGuidTag(Actor, SpawnerOwnerTagPrefix, OutId);
}

bool AEMSActorSpawner::TryGetChildIdFromActorTag(const AActor* Actor, FGuid& OutId) const
{
	return TryGetSingleGuidTag(Actor, SpawnedActorTagPrefix, OutId);
}

void AEMSActorSpawner::RebuildLiveBindings()
{
	for (const TPair<FGuid, TObjectPtr<AActor>>& Pair : LiveActorsById)
	{
		if (AActor* BoundActor = Pair.Value.Get())
		{
			BoundActor->OnEndPlay.RemoveDynamic(this, &AEMSActorSpawner::HandleChildEndPlay);
		}
	}
	LiveActorsById.Empty();
	LiveIdsByActor.Empty();

	const ULevel* SpawnerLevel = GetLevel();
	if (!SpawnerLevel)
	{
		return;
	}

	// Resolving the identity hashes a string, and this runs over every actor in
	// the level on each save and load.
	const FGuid CurrentIdentity = GetSpawnerIdentity();
	TMap<FGuid, AActor*> CandidatesById;
	TArray<AActor*> DuplicateCandidates;
	for (AActor* Candidate : SpawnerLevel->Actors)
	{
		if (!IsValid(Candidate) || Candidate == this)
		{
			continue;
		}
		FGuid TaggedOwnerId;
		FGuid LocalId;
		if (TryGetOwnerIdFromActorTag(Candidate, TaggedOwnerId)
			&& TaggedOwnerId == CurrentIdentity
			&& TryGetChildIdFromActorTag(Candidate, LocalId)
			&& LocalId.IsValid())
		{
			if (AActor** ExistingCandidate = CandidatesById.Find(LocalId))
			{
				const FName StableName = MakeStableActorName(LocalId);
				if ((*ExistingCandidate)->GetFName() != StableName && Candidate->GetFName() == StableName)
				{
					DuplicateCandidates.Add(*ExistingCandidate);
					*ExistingCandidate = Candidate;
				}
				else
				{
					DuplicateCandidates.Add(Candidate);
				}
				continue;
			}
			CandidatesById.Add(LocalId, Candidate);
		}
	}

	for (const TPair<FGuid, AActor*>& Pair : CandidatesById)
	{
		RegisterLiveActor(Pair.Key, Pair.Value);
	}
	for (AActor* Duplicate : DuplicateCandidates)
	{
		if (IsValid(Duplicate))
		{
			Duplicate->Destroy();
		}
	}
}

void AEMSActorSpawner::PruneObsoleteAbsentRecords()
{
	// An absent record only exists to suppress a live actor. Once that actor is
	// gone the record restores exactly like no record at all, so drop it instead
	// of letting destroyed actors accumulate against MaxSpawnedActors forever.
	SpawnedActorManifest.RemoveAll(
		[this](const FEMSSpawnedActorRecord& Record)
		{
			return !Record.bExists && !LiveActorsById.Contains(Record.LocalId);
		});
}

void AEMSActorSpawner::CaptureLiveActorsForSave()
{
	RebuildLiveBindings();
	PruneObsoleteAbsentRecords();

	// EMS save callbacks can execute gameplay code and re-enter this spawner.
	// Snapshot the ids so no LiveActorsById iterator survives SaveActorState().
	TArray<FGuid> CaptureIds;
	LiveActorsById.GetKeys(CaptureIds);

	for (const FGuid& LocalId : CaptureIds)
	{
		AActor* Actor = GetSpawnedActor(FEMSSpawnedActorId(LocalId));
		if (!IsValid(Actor))
		{
			continue;
		}

		// Serialize before touching the record. A half-updated record would pair
		// a fresh transform with stale binary data, and an emptied one would lose
		// the last good state for good once the next save reaches disk.
		TArray<uint8> CapturedBinary;
		if (!SaveActorState(Actor, CapturedBinary))
		{
			UE_LOG(
				LogEMSAddonsActorSpawner,
				Warning,
				TEXT("Failed to serialize spawned actor with EMS. The previous saved state is kept. Spawner=%s Actor=%s"),
				*GetPathName(),
				*Actor->GetPathName());
			continue;
		}

		FEMSSpawnedActorRecord* Record = FindRecordMutable(LocalId);
		if (!Record)
		{
			if (SpawnedActorManifest.Num() >= MaxSpawnedActors)
			{
				UE_LOG(
					LogEMSAddonsActorSpawner,
					Warning,
					TEXT("A live spawned actor has no record and the spawned actor limit is reached, so it will not persist. Spawner=%s Actor=%s Limit=%d"),
					*GetPathName(),
					*Actor->GetPathName(),
					MaxSpawnedActors);
				continue;
			}
			Record = &SpawnedActorManifest.AddDefaulted_GetRef();
			Record->LocalId = LocalId;
		}
		UpdateRecordFromActor(*Record, Actor);
		Record->ActorBinaryData = MoveTemp(CapturedBinary);
	}
}

void AEMSActorSpawner::UpdateRecordFromActor(FEMSSpawnedActorRecord& Record, AActor* Actor)
{
	if (!IsValid(Actor))
	{
		return;
	}
	Record.ActorClass = FSoftClassPath(Actor->GetClass());
	Record.bExists = true;
	Record.RecordVersion = FEMSSpawnedActorRecord::CurrentVersion;
	Record.SavedTransform = Actor->GetActorTransform();

	Record.bAttachToSpawner = false;
	Record.AttachSocket = NAME_None;
	Record.RelativeAttachmentTransform = FTransform::Identity;
	if (Actor->GetAttachParentActor() == this)
	{
		Record.bAttachToSpawner = true;
		Record.AttachSocket = Actor->GetAttachParentSocketName();
		Record.RelativeAttachmentTransform = Actor->GetRootComponent()
			? Actor->GetRootComponent()->GetRelativeTransform()
			: FTransform::Identity;
	}
}

void AEMSActorSpawner::ApplyRecordState(AActor* Actor, const FEMSSpawnedActorRecord& Record) const
{
	if (IsValid(Actor))
	{
		Actor->SetActorTransform(Record.SavedTransform, false, nullptr, ETeleportType::TeleportPhysics);
	}
}

void AEMSActorSpawner::ApplyRecordAttachment(AActor* Actor, const FEMSSpawnedActorRecord& Record) const
{
	if (!IsValid(Actor))
	{
		return;
	}
	if (!Record.bAttachToSpawner)
	{
		if (Actor->GetAttachParentActor())
		{
			Actor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		}
		ApplyRecordState(Actor, Record);
		return;
	}

	AEMSActorSpawner* AttachParent = const_cast<AEMSActorSpawner*>(this);
	FName SocketToUse = Record.AttachSocket;
	if (!SocketToUse.IsNone()
		&& (!AttachParent->GetRootComponent() || !AttachParent->GetRootComponent()->DoesSocketExist(SocketToUse)))
	{
		SocketToUse = NAME_None;
	}
	Actor->AttachToActor(AttachParent, FAttachmentTransformRules::KeepRelativeTransform, SocketToUse);
	Actor->SetActorRelativeTransform(Record.RelativeAttachmentTransform);
}

bool AEMSActorSpawner::DestroySpawnedActor(FEMSSpawnedActorId ActorId)
{
	return CanMutateSpawnedActors()
		&& ActorId.IsValid()
		&& FindRecord(ActorId.Guid)
		&& DestroyLiveActor(ActorId.Guid, EEMSSpawnerDestructionContext::Gameplay, true);
}

bool AEMSActorSpawner::DestroyLiveActor(
	const FGuid& LocalId,
	const EEMSSpawnerDestructionContext Context,
	const bool bMarkAbsent)
{
	AActor* Actor = nullptr;
	if (const TObjectPtr<AActor>* FoundActor = LiveActorsById.Find(LocalId))
	{
		Actor = FoundActor->Get();
	}

	if (!IsValid(Actor))
	{
		LiveActorsById.Remove(LocalId);
		for (auto It = LiveIdsByActor.CreateIterator(); It; ++It)
		{
			if (It.Value() == LocalId)
			{
				It.RemoveCurrent();
			}
		}
		if (bMarkAbsent)
		{
			if (FEMSSpawnedActorRecord* Record = FindRecordMutable(LocalId))
			{
				Record->bExists = false;
			}
		}
		return true;
	}

	const EEMSSpawnerDestructionContext PreviousContext = ActiveDestructionContext;
	ActiveDestructionContext = Context;
	const FName OriginalName = Actor->GetFName();
	const FString OriginalPath = Actor->GetPathName();
	bool bRenamedForReplacement = false;

	if (Context == EEMSSpawnerDestructionContext::ReplaceClass)
	{
		const FName DisposalName(*FString::Printf(
			TEXT("EMS_ReplacedActor_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		bRenamedForReplacement = Actor->Rename(
			*DisposalName.ToString(),
			nullptr,
			REN_DontCreateRedirectors | REN_NonTransactional);
		if (!bRenamedForReplacement)
		{
			ActiveDestructionContext = PreviousContext;
			UE_LOG(
				LogEMSAddonsActorSpawner,
				Warning,
				TEXT("Failed to release the stable name before replacing a mismatched actor; the existing actor is retained. Actor=%s"),
				*OriginalPath);
			return false;
		}
	}

	const bool bDestroyed = Actor->Destroy() || !IsValid(Actor);
	ActiveDestructionContext = PreviousContext;
	if (!bDestroyed)
	{
		if (bRenamedForReplacement && IsValid(Actor)
			&& !Actor->Rename(*OriginalName.ToString(), nullptr, REN_DontCreateRedirectors | REN_NonTransactional))
		{
			UE_LOG(
				LogEMSAddonsActorSpawner,
				Error,
				TEXT("Actor destruction failed and its stable name could not be restored. Actor=%s ExpectedName=%s"),
				*Actor->GetPathName(),
				*OriginalName.ToString());
		}
		else
		{
			UE_LOG(
				LogEMSAddonsActorSpawner,
				Warning,
				TEXT("Actor destruction failed; the live binding and durable record were retained. Actor=%s"),
				*OriginalPath);
		}
		return false;
	}

	if (bMarkAbsent)
	{
		if (FEMSSpawnedActorRecord* Record = FindRecordMutable(LocalId))
		{
			Record->bExists = false;
		}
	}
	if (Context == EEMSSpawnerDestructionContext::Gameplay)
	{
		OnActorRemoved.Broadcast(FEMSSpawnedActorId(LocalId), Actor);
	}
	if (LiveActorsById.Contains(LocalId))
	{
		UnregisterLiveActor(Actor);
	}
	return true;
}

void AEMSActorSpawner::HandleChildEndPlay(AActor* Actor, const EEndPlayReason::Type EndPlayReason)
{
	if (!Actor)
	{
		return;
	}
	if (IsTeardownReason(EndPlayReason) || bIsEndingPlay || ActiveDestructionContext != EEMSSpawnerDestructionContext::None)
	{
		UnregisterLiveActor(Actor);
	}
}

void AEMSActorSpawner::HandleChildDestroyed(AActor* Actor)
{
	if (!Actor)
	{
		return;
	}
	const FGuid* FoundId = LiveIdsByActor.Find(FObjectKey(Actor));
	if (!FoundId)
	{
		return;
	}
	const FGuid LocalId = *FoundId;
	if (HasActorBegunPlay()
		&& !bIsEndingPlay
		&& ActiveDestructionContext == EEMSSpawnerDestructionContext::None)
	{
		if (FEMSSpawnedActorRecord* Record = FindRecordMutable(LocalId))
		{
			Record->bExists = false;
		}
		OnActorRemoved.Broadcast(FEMSSpawnedActorId(LocalId), Actor);
	}
	UnregisterLiveActor(Actor);
}

AActor* AEMSActorSpawner::GetSpawnedActor(FEMSSpawnedActorId ActorId) const
{
	if (!ActorId.IsValid())
	{
		return nullptr;
	}
	if (const TObjectPtr<AActor>* Actor = LiveActorsById.Find(ActorId.Guid))
	{
		return IsValid(Actor->Get()) ? Actor->Get() : nullptr;
	}
	return nullptr;
}

TArray<AActor*> AEMSActorSpawner::GetAllSpawnedActors() const
{
	TArray<AActor*> Result;
	for (const FEMSSpawnedActorRecord& Record : SpawnedActorManifest)
	{
		if (AActor* Actor = GetSpawnedActor(FEMSSpawnedActorId(Record.LocalId)))
		{
			Result.Add(Actor);
		}
	}
	return Result;
}

bool AEMSActorSpawner::WasSpawnedByThis(const AActor* Actor) const
{
	if (!Actor)
	{
		return false;
	}
	if (LiveIdsByActor.Contains(FObjectKey(Actor)))
	{
		return true;
	}
	FGuid TaggedId;
	FGuid TaggedOwnerId;
	return TryGetOwnerIdFromActorTag(Actor, TaggedOwnerId)
		&& TaggedOwnerId == GetSpawnerIdentity()
		&& TryGetChildIdFromActorTag(Actor, TaggedId)
		&& TaggedId.IsValid();
}

bool AEMSActorSpawner::HasSpawnedActorRecord(FEMSSpawnedActorId ActorId) const
{
	const FEMSSpawnedActorRecord* Record = FindRecord(ActorId.Guid);
	return Record != nullptr && Record->bExists;
}

bool AEMSActorSpawner::GetSpawnedActorId(const AActor* Actor, FEMSSpawnedActorId& OutActorId) const
{
	OutActorId = FEMSSpawnedActorId();
	if (!Actor)
	{
		return false;
	}
	if (const FGuid* FoundId = LiveIdsByActor.Find(FObjectKey(Actor)))
	{
		OutActorId = FEMSSpawnedActorId(*FoundId);
		return true;
	}
	FGuid TaggedId;
	FGuid TaggedOwnerId;
	if (TryGetOwnerIdFromActorTag(Actor, TaggedOwnerId)
		&& TaggedOwnerId == GetSpawnerIdentity()
		&& TryGetChildIdFromActorTag(Actor, TaggedId))
	{
		OutActorId = FEMSSpawnedActorId(TaggedId);
		return true;
	}
	return false;
}

FEMSSpawnedActorRecord* AEMSActorSpawner::FindRecordMutable(const FGuid& LocalId)
{
	return SpawnedActorManifest.FindByPredicate(
		[&LocalId](const FEMSSpawnedActorRecord& Record)
		{
			return Record.LocalId == LocalId;
		});
}

const FEMSSpawnedActorRecord* AEMSActorSpawner::FindRecord(const FGuid& LocalId) const
{
	return SpawnedActorManifest.FindByPredicate(
		[&LocalId](const FEMSSpawnedActorRecord& Record)
		{
			return Record.LocalId == LocalId;
		});
}

FName AEMSActorSpawner::MakeStableActorName(const FGuid& LocalId)
{
	return FName(*FString::Printf(
		TEXT("EMS_SpawnedActor_%s"),
		*LocalId.ToString(EGuidFormats::Digits)));
}

bool AEMSActorSpawner::SaveActorState(AActor* Actor, TArray<uint8>& OutBinary)
{
	OutBinary.Reset();
	if (!IsValid(Actor))
	{
		return false;
	}

	UEMSObject* EMSObject = ResolveEMSObject();
	if (!IsValid(EMSObject))
	{
		return false;
	}

	FGameObjectSaveData SaveData;
	EMSObject->SaveActorToBinary(Actor, SaveData);
	return EMSAddons::PackActorSaveData(SaveData, OutBinary);
}

bool AEMSActorSpawner::LoadActorState(
	const FEMSSpawnedActorRecord& Record,
	AActor* Actor,
	bool bDeferredSpawn)
{
	if (!IsValid(Actor) || !Record.LocalId.IsValid())
	{
		return false;
	}

	FGameObjectSaveData SaveData;
	if (!EMSAddons::UnpackActorSaveData(Record.ActorBinaryData, SaveData))
	{
		return false;
	}

	UEMSObject* EMSObject = ResolveEMSObject();
	if (!IsValid(EMSObject))
	{
		return false;
	}

	// Newly reconstructed actors execute ActorPreLoad before FinishSpawning,
	// matching EMS Core. Tell the binary loader not to execute it a second time.
	EMSObject->LoadActorFromBinary(Actor, SaveData, bDeferredSpawn);
	return true;
}

void AEMSActorSpawner::ActorPreSave_Implementation()
{
	const TWeakObjectPtr<AEMSActorSpawner> WeakThis(this);
	EMSAddons::RunOnGameThread(
		[WeakThis]()
		{
			AEMSActorSpawner* Spawner = WeakThis.Get();
			if (IsValid(Spawner))
			{
				Spawner->CaptureSpawnerStateOnGameThread();
			}
		});
}

void AEMSActorSpawner::CaptureSpawnerStateOnGameThread()
{
	check(IsInGameThread());
	if (!CanMutateSpawnedActors())
	{
		return;
	}
	EnsureSpawnerIdentity();
	CaptureLiveActorsForSave();
}

void AEMSActorSpawner::ActorPreLoad_Implementation()
{
	SpawnerIdentityBeforeLoad = GetSpawnerIdentity();
	bRestoreFinished = false;
	LastRestoreResult = FEMSAddonResult(
		EEMSAddonResultCode::Pending,
		LOCTEXT("SpawnerRestorePending", "Actor spawner restoration is pending."));
}

void AEMSActorSpawner::ActorLoaded_Implementation()
{
	EnsureSpawnerIdentity();
	if (SpawnerIdentityBeforeLoad.IsValid() && SpawnerIdentityBeforeLoad != GetSpawnerIdentity())
	{
		RetagLiveActorsForIdentityChange(SpawnerIdentityBeforeLoad);
	}
	SpawnerIdentityBeforeLoad.Invalidate();
	RestoreSpawnedActors();
}

void AEMSActorSpawner::RestoreSpawnedActors()
{
	if (!CanMutateSpawnedActors())
	{
		// Skipped, not Canceled. Canceled means a restore was torn down partway
		// through everywhere else in the plugin, and a client never starts one.
		LastRestoreResult = FEMSAddonResult(
			EEMSAddonResultCode::Skipped,
			LOCTEXT("InvalidRestoreWorld", "The actor spawner cannot restore in the current world or network role."));
		bRestoreFinished = true;

		// Pre-Load already announced a pending restore, so anything waiting on
		// completion has to hear that this load will never produce one.
		OnRestoreFinished.Broadcast(LastRestoreResult);
		return;
	}

	bRestoreFinished = false;
	bRestoreHadFailure = false;
	bRestoreHadSkippedRecord = false;
	RestoredActorIds.Empty();
	LastRestoreResult = FEMSAddonResult(
		EEMSAddonResultCode::Pending,
		LOCTEXT("SpawnerRestoreStarted", "Actor spawner restoration started."));
	OnRestoreStarted.Broadcast();

	RebuildLiveBindings();
	TSet<FGuid> DesiredIds;
	TSet<FGuid> DuplicateIds;
	TSet<FGuid> StateLoadEligibleIds;
	TSet<FGuid> DeferredRestoreIds;
	const int32 ProcessCount = FMath::Min(SpawnedActorManifest.Num(), FMath::Max(1, MaxSpawnedActors));
	if (ProcessCount < SpawnedActorManifest.Num())
	{
		// The records past the limit are kept, just not restored, so this is a
		// skip rather than a failure and nothing is lost from the manifest.
		bRestoreHadSkippedRecord = true;
		UE_LOG(
			LogEMSAddonsActorSpawner,
			Warning,
			TEXT("Spawn record processing was limited to %d records. Remaining records were retained. Spawner=%s Records=%d"),
			ProcessCount,
			*GetPathName(),
			SpawnedActorManifest.Num());
	}

	TSet<FGuid> SeenIds;
	for (int32 Index = 0; Index < ProcessCount; ++Index)
	{
		const FGuid& LocalId = SpawnedActorManifest[Index].LocalId;
		if (LocalId.IsValid() && SeenIds.Contains(LocalId))
		{
			DuplicateIds.Add(LocalId);
		}
		SeenIds.Add(LocalId);
	}

	for (int32 Index = 0; Index < ProcessCount; ++Index)
	{
		// Copied, and the count re-checked, because spawning below runs user
		// construction and Begin Play, which can grow or prune the manifest.
		if (!SpawnedActorManifest.IsValidIndex(Index))
		{
			break;
		}
		const FEMSSpawnedActorRecord Record = SpawnedActorManifest[Index];
		FText ValidationReason;
		const bool bIsValidRecord = IsRecordValid(Record, ValidationReason);
		if (!bIsValidRecord || DuplicateIds.Contains(Record.LocalId))
		{
			bRestoreHadFailure = true;
			UE_LOG(
				LogEMSAddonsActorSpawner,
				Warning,
				TEXT("A spawn record was not restored. Spawner=%s Record=%s Reason=%s"),
				*GetPathName(),
				*Record.LocalId.ToString(EGuidFormats::Digits),
				bIsValidRecord
					? TEXT("The manifest contains a duplicate record ID.")
					: *ValidationReason.ToString());
			continue;
		}
		if (!Record.bExists)
		{
			bRestoreHadFailure |= !DestroyLiveActor(
				Record.LocalId,
				EEMSSpawnerDestructionContext::Internal,
				false);
			continue;
		}

		DesiredIds.Add(Record.LocalId);
		AActor* ExistingActor = GetSpawnedActor(FEMSSpawnedActorId(Record.LocalId));
		bool bDeferredRestore = false;
		UClass* SavedClass = FSpawnHelpers::ResolveSpawnClass(Record.ActorClass.ToString());
		if (ExistingActor
			&& (ExistingActor->GetClass() != SavedClass
				|| ExistingActor->GetFName() != MakeStableActorName(Record.LocalId)))
		{
			if (!DestroyLiveActor(Record.LocalId, EEMSSpawnerDestructionContext::ReplaceClass, false))
			{
				bRestoreHadFailure = true;
				DesiredIds.Remove(Record.LocalId);
				continue;
			}
			ExistingActor = nullptr;
		}
		if (!ExistingActor)
		{
			ExistingActor = SpawnActorForRecord(Record, true);
			bDeferredRestore = ExistingActor != nullptr;
		}
		else
		{
			ApplyRecordState(ExistingActor, Record);
		}
		if (!ExistingActor)
		{
			bRestoreHadFailure = true;
			DesiredIds.Remove(Record.LocalId);
			continue;
		}
		StateLoadEligibleIds.Add(Record.LocalId);
		if (bDeferredRestore)
		{
			DeferredRestoreIds.Add(Record.LocalId);
		}
	}

	TArray<FGuid> LiveIds;
	LiveActorsById.GetKeys(LiveIds);
	for (const FGuid& LiveId : LiveIds)
	{
		if (!DesiredIds.Contains(LiveId))
		{
			bRestoreHadFailure |= !DestroyLiveActor(
				LiveId,
				EEMSSpawnerDestructionContext::Internal,
				false);
		}
	}

	// Indexed and copied for the same reason as the loop above: Load Actor State
	// fires the actor's own EMS Actor Loaded, which can spawn through this spawner
	// and move the manifest's elements. Consuming the eligibility set instead of
	// testing it also keeps an element shift from loading one record twice.
	for (int32 Index = 0; Index < SpawnedActorManifest.Num(); ++Index)
	{
		const FEMSSpawnedActorRecord Record = SpawnedActorManifest[Index];
		if (StateLoadEligibleIds.Remove(Record.LocalId) == 0)
		{
			continue;
		}
		const bool bDeferredRestore = DeferredRestoreIds.Remove(Record.LocalId) > 0;
		AActor* Actor = GetSpawnedActor(FEMSSpawnedActorId(Record.LocalId));
		if (!Actor)
		{
			bRestoreHadFailure |= HasSpawnedActorRecord(FEMSSpawnedActorId(Record.LocalId));
			continue;
		}
		const bool bLoaded = LoadActorState(Record, Actor, bDeferredRestore);
		const bool bStillBound = IsValid(Actor)
			&& GetSpawnedActor(FEMSSpawnedActorId(Record.LocalId)) == Actor;
		if (bLoaded && bStillBound)
		{
			RestoredActorIds.Add(Record.LocalId);
		}
		else
		{
			bRestoreHadFailure = true;
			if (bDeferredRestore)
			{
				// A newly spawned actor has already run Actor Pre Load. Leaving it
				// live without Actor Loaded is a half-restored actor, so it is
				// discarded and its record kept for the next restore.
				DestroyLiveActor(Record.LocalId, EEMSSpawnerDestructionContext::Internal, false);
				continue;
			}
		}

		// After the binary state, which may have moved the actor.
		ApplyRecordAttachment(Actor, Record);
	}

	// Same case for a deferred spawn whose record moved out from under the loop
	// above: it never reached its state load, so it never reached Actor Loaded.
	for (const FGuid& LocalId : DeferredRestoreIds)
	{
		bRestoreHadFailure = true;
		DestroyLiveActor(LocalId, EEMSSpawnerDestructionContext::Internal, false);
	}

	FinishRestore();
}

void AEMSActorSpawner::FinishRestore()
{
	for (const FGuid& ActorId : RestoredActorIds)
	{
		if (AActor* Actor = GetSpawnedActor(FEMSSpawnedActorId(ActorId)))
		{
			OnActorRestored.Broadcast(FEMSSpawnedActorId(ActorId), Actor);
		}
	}
	bRestoreFinished = true;
	LastRestoreResult = bRestoreHadFailure
		? FEMSAddonResult(EEMSAddonResultCode::RestoreFailed, LOCTEXT("SpawnerRestorePartialFailure", "Actor spawner restoration completed with one or more failures."))
		: bRestoreHadSkippedRecord
			? FEMSAddonResult(EEMSAddonResultCode::Skipped, LOCTEXT("SpawnerRestoreSkipped", "Actor spawner restoration completed with one or more skipped records."))
			: FEMSAddonResult(EEMSAddonResultCode::Success, LOCTEXT("SpawnerRestoreSuccess", "Actor spawner restoration completed."));
	OnRestoreFinished.Broadcast(LastRestoreResult);
}

bool AEMSActorSpawner::HasFinishedRestoring() const
{
	return bRestoreFinished;
}

FEMSAddonResult AEMSActorSpawner::GetLastRestoreResult() const
{
	return LastRestoreResult;
}

UEMSObject* AEMSActorSpawner::ResolveEMSObject() const
{
	return UEMSObject::Get(this);
}

#undef LOCTEXT_NAMESPACE