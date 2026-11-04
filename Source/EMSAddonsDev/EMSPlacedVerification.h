//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once
#include "CoreMinimal.h"
#include "EMSActorSaveInterface.h"
#include "GameFramework/Actor.h"
#include "UObject/StrongObjectPtr.h"
#include "WorldPartition/WorldPartitionStreamingSource.h"
#include "EMSPlacedVerification.generated.h"

class UBoxComponent;
class UEMSWorldPartitionPlacedComponent;
class UEMSPhysicsStreamingGuardComponent;
class UEMSObject;
class UWorldPartitionSubsystem;
class UWorldPartitionRuntimeCell;

/** Dev-project fixture; never distributed as part of the addon plugin. */
UCLASS(NotPlaceable, HideDropdown)
class AEMSVerificationStateActor : public AActor, public IEMSActorSaveInterface
{
	GENERATED_BODY()
public:
	AEMSVerificationStateActor();
	UPROPERTY() TObjectPtr<UBoxComponent> Body;
	UPROPERTY(SaveGame) int32 Health = 17;
	UPROPERTY(SaveGame) bool bSavedGravity = true;
	bool bGameplayDeath = false;
	virtual void ActorPreSave_Implementation() override;
	virtual void ActorLoaded_Implementation() override;
	virtual void EndPlay(EEndPlayReason::Type Reason) override;
};

UCLASS(NotPlaceable, HideDropdown)
class AEMSPlacedVerificationActor : public AEMSVerificationStateActor
{
	GENERATED_BODY()
public:
	AEMSPlacedVerificationActor();
	UPROPERTY() TObjectPtr<UEMSWorldPartitionPlacedComponent> Marker;
	UPROPERTY() TObjectPtr<UEMSPhysicsStreamingGuardComponent> Guard;
};

/** Runs only with -EMSPlacedAcceptance and a generated verification map. */
UCLASS(NotPlaceable, HideDropdown)
class AEMSPlacedVerificationController : public AActor, public IWorldPartitionStreamingSourceProvider
{
	GENERATED_BODY()
public:
	AEMSPlacedVerificationController();
	UPROPERTY() FGuid ExpectedPlacementId;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(EEndPlayReason::Type Reason) override;
	virtual bool GetStreamingSources(TArray<FWorldPartitionStreamingSource>& Sources) const override;
private:
	UPROPERTY() TObjectPtr<UObject> Operation;
	UFUNCTION() void Completed();
	UFUNCTION() void Failed();
	bool BeginOperation(bool bSave, const TCHAR* Slot);
	AEMSVerificationStateActor* FindPlaced() const;
	bool VerifyAlive();
	bool VerifyRuntime();
	void Finish(bool bSuccess, const FString& Reason);
	TWeakObjectPtr<UWorldPartitionSubsystem> Streaming;
	TWeakObjectPtr<const UWorldPartitionRuntimeCell> CellA, CellB;
	TStrongObjectPtr<AEMSVerificationStateActor> Initial;
	FVector SourceA = FVector(0, 0, 200);
	FVector SourceB = FVector(30000, 0, 200);
	FName OriginalName;
	FString Case;
	FString OldUser, OldSlot;
	int32 Step = 0;
	double Start = 0;
	bool bEnabled = false;
	bool bDone = false;
	bool bSuccess = false;
	bool bConfigured = false;
};