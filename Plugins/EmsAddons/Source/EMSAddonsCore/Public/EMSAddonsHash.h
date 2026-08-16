//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"

/**
 * FNV-1a primitives shared by the addon save formats.
 *
 * These values are persisted in save data, so the algorithm must stay fixed.
 * Unreal's GetTypeHash is deliberately not used here because it is not stable
 * across builds or platforms.
 */
namespace EMSAddons::Hash
{
	constexpr uint64 OffsetBasis = 14695981039346656037ull;
	constexpr uint64 Prime = 1099511628211ull;

	inline void HashBytes(uint64& InOutHash, const void* Data, const SIZE_T Size)
	{
		const uint8* Bytes = static_cast<const uint8*>(Data);
		for (SIZE_T Index = 0; Index < Size; ++Index)
		{
			InOutHash ^= Bytes[Index];
			InOutHash *= Prime;
		}
	}

	template <typename ValueType>
	void HashValue(uint64& InOutHash, const ValueType& Value)
	{
		HashBytes(InOutHash, &Value, sizeof(ValueType));
	}
}
