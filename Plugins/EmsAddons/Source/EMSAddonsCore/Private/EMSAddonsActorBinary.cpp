//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#include "EMSAddonsActorBinary.h"

#include "EMSData.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

namespace
{
	constexpr uint32 ActorBinaryMagic = 0x454D5341; // EMSA
	constexpr uint32 ActorBinaryVersion = 1;
}

bool EMSAddons::PackActorSaveData(const FGameObjectSaveData& SaveData, TArray<uint8>& OutBinary)
{
	OutBinary.Reset();
	FMemoryWriter Writer(OutBinary, true);
	uint32 Magic = ActorBinaryMagic;
	uint32 Version = ActorBinaryVersion;
	FGameObjectSaveData MutableSaveData = SaveData;
	Writer << Magic;
	Writer << Version;
	Writer << MutableSaveData;
	if (Writer.IsError())
	{
		OutBinary.Reset();
		return false;
	}
	return true;
}

bool EMSAddons::UnpackActorSaveData(const TArray<uint8>& Binary, FGameObjectSaveData& OutSaveData)
{
	if (Binary.IsEmpty())
	{
		return false;
	}

	FMemoryReader Reader(Binary, true);
	uint32 Magic = 0;
	uint32 Version = 0;
	Reader << Magic;
	Reader << Version;
	if (Reader.IsError() || Magic != ActorBinaryMagic || Version != ActorBinaryVersion)
	{
		return false;
	}

	Reader << OutSaveData;
	return !Reader.IsError();
}
