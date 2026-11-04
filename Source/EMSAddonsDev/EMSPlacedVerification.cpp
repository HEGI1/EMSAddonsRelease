//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSPlacedVerification.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "EMSAsyncLoadGame.h"
#include "EMSAsyncSaveGame.h"
#include "EMSObject.h"
#include "EMSPluginSettings.h"
#include "EMSPhysicsStreamingGuardComponent.h"
#include "EMSWorldPartitionPlacedComponent.h"
#include "EMSWorldPartitionPlacedSubsystem.h"
#include "EMSWorldPartitionRuntimeLibrary.h"
#include "EMSWorldPartitionRuntimeManager.h"
#include "EMSWorldPartitionRuntimeSubsystem.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionRuntimeHash.h"
#include "WorldPartition/WorldPartitionSubsystem.h"

AEMSVerificationStateActor::AEMSVerificationStateActor()
{
	PrimaryActorTick.bCanEverTick = false;
	Body = CreateDefaultSubobject<UBoxComponent>(TEXT("Body"));
	Body->SetBoxExtent(FVector(40));
	Body->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Body->SetCollisionObjectType(ECC_PhysicsBody);
	Body->SetCollisionResponseToAllChannels(ECR_Block);
	Body->SetSimulatePhysics(true);
	SetRootComponent(Body);
}
void AEMSVerificationStateActor::ActorPreSave_Implementation() { bSavedGravity = Body->BodyInstance.bEnableGravity; }
void AEMSVerificationStateActor::ActorLoaded_Implementation() { Body->SetEnableGravity(bSavedGravity); }
void AEMSVerificationStateActor::EndPlay(EEndPlayReason::Type Reason)
{
	bGameplayDeath = Reason == EEndPlayReason::Destroyed && !UEMSWorldPartitionRuntimeLibrary::IsWorldPartitionRuntimeRemovalInProgress(this);
	Super::EndPlay(Reason);
}
AEMSPlacedVerificationActor::AEMSPlacedVerificationActor()
{
	Marker = CreateDefaultSubobject<UEMSWorldPartitionPlacedComponent>(TEXT("PlacedMarker"));
	Guard = CreateDefaultSubobject<UEMSPhysicsStreamingGuardComponent>(TEXT("Guard"));
}
AEMSPlacedVerificationController::AEMSPlacedVerificationController()
{
	PrimaryActorTick.bCanEverTick = true;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
#if WITH_EDITOR
	SetIsSpatiallyLoaded(false);
#endif
}
void AEMSPlacedVerificationController::BeginPlay()
{
	Super::BeginPlay();
	bEnabled = FParse::Param(FCommandLine::Get(), TEXT("EMSPlacedAcceptance"));
	if (!bEnabled) { SetActorTickEnabled(false); return; }
	Start = FPlatformTime::Seconds();
	FParse::Value(FCommandLine::Get(), TEXT("EMSPlacedCase="), Case);
	GetMutableDefault<UEMSPluginSettings>()->WorldPartitionInit = EWorldPartitionInit::Skip;
	Streaming = GetWorld()->GetSubsystem<UWorldPartitionSubsystem>();
	if (Streaming.IsValid()) Streaming->RegisterStreamingSourceProvider(this);
}
bool AEMSPlacedVerificationController::GetStreamingSources(TArray<FWorldPartitionStreamingSource>& Sources) const
{
	if (!bEnabled) return false;
	for (int32 Index = 0; Index < 2; ++Index)
	{
		FWorldPartitionStreamingSource Source;
		Source.Name = Index ? TEXT("EMSVerificationB") : TEXT("EMSVerificationA");
		Source.Location = Index ? SourceB : SourceA;
		FStreamingSourceShape Shape;
		Shape.bUseGridLoadingRange = false;
		Shape.Radius = 2000;
		Source.Shapes.Add(Shape);
		Sources.Add(Source);
	}
	return true;
}
void AEMSPlacedVerificationController::EndPlay(EEndPlayReason::Type Reason)
{
	if (Streaming.IsValid()) Streaming->UnregisterStreamingSourceProvider(this);
	Super::EndPlay(Reason);
}
AEMSVerificationStateActor* AEMSPlacedVerificationController::FindPlaced() const
{
	for (TActorIterator<AEMSVerificationStateActor> It(GetWorld()); It; ++It)
		if ((It->FindComponentByClass<UEMSWorldPartitionPlacedComponent>() && It->FindComponentByClass<UEMSWorldPartitionPlacedComponent>()->GetPlacementId() == ExpectedPlacementId)) return *It;
	return nullptr;
}
void AEMSPlacedVerificationController::Completed() { bDone = true; bSuccess = true; }
void AEMSPlacedVerificationController::Failed() { bDone = true; bSuccess = false; }
bool AEMSPlacedVerificationController::BeginOperation(bool bSave, const TCHAR* Slot)
{
	UEMSObject* EMSObject = UEMSObject::Get(this);
	EMSObject->SetCurrentSaveGameName(Slot);
	bDone = false;
	bSuccess = false;
	if (bSave)
	{
		UEMSAsyncSaveGame* Save = UEMSAsyncSaveGame::AsyncSaveActors(this, TOFLAG(ESaveTypeFlags::SF_Level));
		if (!Save) return false;
		Operation = Save;
		Save->OnCompleted.AddDynamic(this, &AEMSPlacedVerificationController::Completed);
		Save->OnFailed.AddDynamic(this, &AEMSPlacedVerificationController::Failed);
		Save->RegisterWithGameInstance(EMSObject);
		Save->Activate();
	}
	else
	{
		UEMSAsyncLoadGame* Load = UEMSAsyncLoadGame::AsyncLoadActors(this, TOFLAG(ESaveTypeFlags::SF_Level), true);
		if (!Load) return false;
		Operation = Load;
		Load->OnCompleted.AddDynamic(this, &AEMSPlacedVerificationController::Completed);
		Load->OnFailed.AddDynamic(this, &AEMSPlacedVerificationController::Failed);
		Load->RegisterWithGameInstance(EMSObject);
		Load->Activate();
	}
	return true;
}
bool AEMSPlacedVerificationController::VerifyRuntime()
{
	auto* Runtime = GetWorld()->GetSubsystem<UEMSWorldPartitionRuntimeSubsystem>();
	TArray<AActor*> Actors; Runtime->GetManagedActors(Actors);
	return Actors.Num() == 1 && Cast<AEMSVerificationStateActor>(Actors[0]) && CastChecked<AEMSVerificationStateActor>(Actors[0])->Health == 303;
}
bool AEMSPlacedVerificationController::VerifyAlive()
{
	AEMSVerificationStateActor* Actor = FindPlaced();
	if (!Actor || Actor->Health != 101 || Actor->GetFName() != OriginalName
		|| Actor->GetLevel() != GetWorld()->PersistentLevel || !Actor->GetActorLocation().Equals(FVector(30000, 0, 40), 5)
		|| !Actor->Body->IsSimulatingPhysics() || !Actor->Body->BodyInstance.bEnableGravity) return false;
	int32 Count = 0;
	for (TActorIterator<AEMSVerificationStateActor> It(GetWorld()); It; ++It) if ((It->FindComponentByClass<UEMSWorldPartitionPlacedComponent>() && It->FindComponentByClass<UEMSWorldPartitionPlacedComponent>()->GetPlacementId() == ExpectedPlacementId)) ++Count;
	auto* Placed = GetWorld()->GetSubsystem<UEMSWorldPartitionPlacedSubsystem>();
	auto* Runtime = GetWorld()->GetSubsystem<UEMSWorldPartitionRuntimeSubsystem>();
	return Count == 1 && Placed->IsPlacedActorManaged(Actor) && !Runtime->IsManagedActor(Actor) && VerifyRuntime();
}
void AEMSPlacedVerificationController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bEnabled) return;
	if (FPlatformTime::Seconds() - Start > 90) { Finish(false, FString::Printf(TEXT("Timed out at step %d"), Step)); return; }
	UEMSObject* EMSObject = UEMSObject::Get(this);
	auto* Runtime = GetWorld()->GetSubsystem<UEMSWorldPartitionRuntimeSubsystem>();
	auto* Placed = GetWorld()->GetSubsystem<UEMSWorldPartitionPlacedSubsystem>();
	if (!EMSObject || !Runtime || !Placed || !Streaming.IsValid()) return;
	if (EMSObject->IsAsyncTaskActive(false) || EMSObject->IsLevelStreaming()) return;
	Placed->Reconcile();
	if (Step == 0)
	{
		APlayerController* PC = GetWorld()->GetFirstPlayerController();
		if (!PC) return;
		PC->bEnableStreamingSource = false;
		if (!bConfigured)
		{
			FString User;
			if (!FParse::Value(FCommandLine::Get(), TEXT("EMSPlacedUser="), User) || !User.StartsWith(TEXT("PlacedPackaged_"))) { Finish(false, TEXT("Isolated verification user is required")); return; }
			OldUser = EMSObject->GetCurrentSaveUserName(); OldSlot = EMSObject->GetCurrentSaveGameName();
			EMSObject->SetCurrentSaveUserName(User);
			GetWorld()->GetWorldPartition()->RuntimeHash->ForEachStreamingCells([&](const UWorldPartitionRuntimeCell* Cell)
			{
				if (Cell->IsSpatiallyLoaded() && !Cell->GetIsHLOD() && !Cell->HasDataLayers())
				{
					if (Cell->GetCellBounds().IsInsideOrOn(FVector(0, 0, 200))) CellA = Cell;
					if (Cell->GetCellBounds().IsInsideOrOn(FVector(30000, 0, 200))) CellB = Cell;
				}
				return true;
			});
			bConfigured = true;
		}
		if (!CellA.IsValid() || !CellB.IsValid() || !CellA->IsVisible() || !CellB->IsVisible()) return;
		AEMSVerificationStateActor* Actor = FindPlaced();
		if (!Actor || !Placed->IsPlacedActorManaged(Actor)) return;
		Initial.Reset(Actor); OriginalName = Actor->GetFName();
		if (Actor->GetLevel() != GetWorld()->PersistentLevel) { Finish(false, TEXT("Cooked placement is spatial")); return; }
		if (Case == TEXT("SaveAlive"))
		{
			Actor->Health = 101;
			Actor->SetActorLocation(FVector(30000, 0, 40), false, nullptr, ETeleportType::TeleportPhysics);
			auto* Other = Cast<AEMSVerificationStateActor>(Runtime->SpawnManagedActor(AEMSVerificationStateActor::StaticClass(), FTransform(FVector(30100, 0, 40)), ESpawnActorCollisionHandlingMethod::AlwaysSpawn));
			if (!Other) { Finish(false, TEXT("Runtime coexistence spawn failed")); return; }
			Other->Health = 303;
			SourceA = FVector(1e8, 0, 200);
			Step = 1; return;
		}
		const TCHAR* Slot = Case == TEXT("LoadDestroyed") || Case == TEXT("LoadEarlier") ? TEXT("Destroyed") : (Case == TEXT("LoadDormant") ? TEXT("Dormant") : TEXT("Alive"));
		if (!BeginOperation(false, Slot)) { Finish(false, TEXT("Initial load refused")); return; }
		Step = 10; return;
	}
	if (Step == 1)
	{
		if (CellA->IsVisible()) return;
		if (FindPlaced() != Initial.Get() || !VerifyAlive()) { Finish(false, TEXT("Old-cell departure replaced or lost the moving original")); return; }
		if (!BeginOperation(true, TEXT("Alive"))) { Finish(false, TEXT("Alive save refused")); return; }
		Step = 2; return;
	}
	if (Step == 13)
	{
		if (CellA->IsVisible() || CellB->IsVisible() || FindPlaced() || Runtime->GetManagedActorCount() != 0) return;
		if (Initial->bGameplayDeath) { Finish(false, TEXT("Dormancy caused gameplay death")); return; }
		if (!BeginOperation(true, TEXT("Dormant"))) { Finish(false, TEXT("Dormant save refused")); return; }
		Step = 14; return;
	}
	if (!bDone) return;
	if (!bSuccess) { Finish(false, TEXT("EMS operation failed")); return; }
	// EMS completes before the runtime registry's idle reconciliation. Let its
	// real timer settle the loaded records; the overall timeout still fails a
	// record that never returns. Do not drive reconciliation from the fixture.
	if ((Step == 10 || Step == 11) && Runtime->GetManagedActorCount() == 0
		&& Runtime->GetDormantActorCount() > 0) return;
	if (Step == 2) { Finish(VerifyAlive(), TEXT("SaveAlive movement and full disk save")); return; }
	if (Step == 10)
	{
		if (Case == TEXT("LoadDestroyed") || Case == TEXT("LoadEarlier"))
		{
			if (FindPlaced() || !VerifyRuntime()) { Finish(false, TEXT("Tombstone did not suppress original placement")); return; }
			if (Case == TEXT("LoadDestroyed")) { Finish(true, TEXT("Fresh-map tombstone")); return; }
			if (!BeginOperation(false, TEXT("Alive"))) { Finish(false, TEXT("Earlier alive load refused")); return; }
			Step = 11; return;
		}
		if (!VerifyAlive()) { Finish(false, TEXT("Fresh-map alive state, identity, physics or ownership mismatch")); return; }
		if (Case == TEXT("LoadAlive") || Case == TEXT("LoadDormant")) { Finish(true, TEXT("Fresh-map position/state without duplicates")); return; }
		if (Case == TEXT("SaveDestroyed"))
		{
			Initial.Reset(FindPlaced());
			Initial->Destroy();
			if (!Initial->bGameplayDeath) { Finish(false, TEXT("Gameplay destruction was misclassified as dormancy")); return; }
			if (!BeginOperation(true, TEXT("Destroyed"))) { Finish(false, TEXT("Destroyed save refused")); return; }
			Step = 12; return;
		}
		if (Case == TEXT("SaveDormant")) { Initial.Reset(FindPlaced()); SourceA = FVector(1e8, 0, 200); SourceB = FVector(1e8, 30000, 200); Step = 13; return; }
		Finish(false, TEXT("Unknown acceptance case")); return;
	}
	if (Step == 11) { Finish(VerifyAlive() && !IsValid(Initial.Get()) && !Initial->bGameplayDeath, TEXT("Earlier alive save after tombstone, predecessor retained before GC")); return; }
	if (Step == 12)
	{
		auto* Manager = Runtime->GetPrimaryManager();
		const auto* Record = Manager->PlacedActorRecords.FindByPredicate([&](const auto& Item) { return Item.State.LocalId == ExpectedPlacementId; });
		Finish(!FindPlaced() && Record && Record->bDestroyed && VerifyRuntime(), TEXT("Gameplay destruction saved as tombstone")); return;
	}
	if (Step == 14)
	{
		auto* Manager = Runtime->GetPrimaryManager();
		const auto* Record = Manager->PlacedActorRecords.FindByPredicate([&](const auto& Item) { return Item.State.LocalId == ExpectedPlacementId; });
		Finish(Record && !Record->bDestroyed && Record->State.SavedTransform.GetLocation().Equals(FVector(30000, 0, 40), 5), TEXT("Dormant disk save retains pre-removal position"));
	}
}
void AEMSPlacedVerificationController::Finish(bool bPassed, const FString& Reason)
{
	bEnabled = false; SetActorTickEnabled(false);
	if (bConfigured)
		if (UEMSObject* EMSObject = UEMSObject::Get(this)) { EMSObject->SetCurrentSaveUserName(OldUser); EMSObject->SetCurrentSaveGameName(OldSlot); }
	UE_LOG(LogTemp, Display, TEXT("EMS_PLACED_ACCEPTANCE %s Case=%s Reason=%s"), bPassed ? TEXT("PASS") : TEXT("FAIL"), *Case, *Reason);
	FPlatformMisc::RequestExitWithStatus(false, bPassed ? 0 : 1);
}
