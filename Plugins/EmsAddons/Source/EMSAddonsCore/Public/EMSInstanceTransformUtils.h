//Easy Multi Save Addons - Copyright (C) 2026 by Michael Hegemann.
#pragma once

#include "CoreMinimal.h"
#include "EMSAddonsHash.h"

namespace EMSAddons::Instances
{
	struct FCanonicalTransform
	{
		int64 LocationX = 0;
		int64 LocationY = 0;
		int64 LocationZ = 0;
		int32 RotationX = 0;
		int32 RotationY = 0;
		int32 RotationZ = 0;
		int32 RotationW = 0;
		int32 ScaleX = 0;
		int32 ScaleY = 0;
		int32 ScaleZ = 0;

		bool operator==(const FCanonicalTransform& Other) const
		{
			return LocationX == Other.LocationX
				&& LocationY == Other.LocationY
				&& LocationZ == Other.LocationZ
				&& RotationX == Other.RotationX
				&& RotationY == Other.RotationY
				&& RotationZ == Other.RotationZ
				&& RotationW == Other.RotationW
				&& ScaleX == Other.ScaleX
				&& ScaleY == Other.ScaleY
				&& ScaleZ == Other.ScaleZ;
		}
	};

	inline FCanonicalTransform CanonicalizeTransform(const FTransform& Transform)
	{
		FQuat Rotation = Transform.GetRotation().GetNormalized();
		if (Rotation.W < 0.0
			|| (FMath::IsNearlyZero(Rotation.W)
				&& (Rotation.X < 0.0
					|| (FMath::IsNearlyZero(Rotation.X)
						&& (Rotation.Y < 0.0
							|| (FMath::IsNearlyZero(Rotation.Y)
								&& Rotation.Z < 0.0))))))
		{
			Rotation.X *= -1.0;
			Rotation.Y *= -1.0;
			Rotation.Z *= -1.0;
			Rotation.W *= -1.0;
		}

		const FVector Location = Transform.GetLocation();
		const FVector Scale = Transform.GetScale3D();
		FCanonicalTransform Result;
		Result.LocationX = FMath::RoundToInt64(Location.X * 10.0);
		Result.LocationY = FMath::RoundToInt64(Location.Y * 10.0);
		Result.LocationZ = FMath::RoundToInt64(Location.Z * 10.0);
		Result.RotationX = FMath::RoundToInt(Rotation.X * 1000000.0);
		Result.RotationY = FMath::RoundToInt(Rotation.Y * 1000000.0);
		Result.RotationZ = FMath::RoundToInt(Rotation.Z * 1000000.0);
		Result.RotationW = FMath::RoundToInt(Rotation.W * 1000000.0);
		Result.ScaleX = FMath::RoundToInt(Scale.X * 10000.0);
		Result.ScaleY = FMath::RoundToInt(Scale.Y * 10000.0);
		Result.ScaleZ = FMath::RoundToInt(Scale.Z * 10000.0);
		return Result;
	}

	inline bool CanonicalTransformLess(
		const FCanonicalTransform& Left,
		const FCanonicalTransform& Right)
	{
		if (Left.LocationX != Right.LocationX) return Left.LocationX < Right.LocationX;
		if (Left.LocationY != Right.LocationY) return Left.LocationY < Right.LocationY;
		if (Left.LocationZ != Right.LocationZ) return Left.LocationZ < Right.LocationZ;
		if (Left.RotationX != Right.RotationX) return Left.RotationX < Right.RotationX;
		if (Left.RotationY != Right.RotationY) return Left.RotationY < Right.RotationY;
		if (Left.RotationZ != Right.RotationZ) return Left.RotationZ < Right.RotationZ;
		if (Left.RotationW != Right.RotationW) return Left.RotationW < Right.RotationW;
		if (Left.ScaleX != Right.ScaleX) return Left.ScaleX < Right.ScaleX;
		if (Left.ScaleY != Right.ScaleY) return Left.ScaleY < Right.ScaleY;
		return Left.ScaleZ < Right.ScaleZ;
	}

	/** Hashes bytes in explicit little-endian order so saves stay portable. */
	inline void HashCanonicalInt64(uint64& InOutHash, const int64 Value)
	{
		uint64 Bits = static_cast<uint64>(Value);
		for (int32 ByteIndex = 0; ByteIndex < 8; ++ByteIndex)
		{
			InOutHash ^= static_cast<uint8>(Bits & 0xff);
			InOutHash *= EMSAddons::Hash::Prime;
			Bits >>= 8;
		}
	}

	inline void HashCanonicalTransformSet(
		uint64& InOutHash,
		const FCanonicalTransform& Value)
	{
		HashCanonicalInt64(InOutHash, Value.LocationX);
		HashCanonicalInt64(InOutHash, Value.LocationY);
		HashCanonicalInt64(InOutHash, Value.LocationZ);
		HashCanonicalInt64(InOutHash, Value.RotationX);
		HashCanonicalInt64(InOutHash, Value.RotationY);
		HashCanonicalInt64(InOutHash, Value.RotationZ);
		HashCanonicalInt64(InOutHash, Value.RotationW);
		HashCanonicalInt64(InOutHash, Value.ScaleX);
		HashCanonicalInt64(InOutHash, Value.ScaleY);
		HashCanonicalInt64(InOutHash, Value.ScaleZ);
	}

	inline uint64 HashCanonicalTransform(const FCanonicalTransform& Value)
	{
		uint64 Hash = EMSAddons::Hash::OffsetBasis;
		HashCanonicalTransformSet(Hash, Value);
		return Hash;
	}
}
