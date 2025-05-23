/*
 * APPARATIST
 * Created: 2023-02-02 16:26:26
 * Author: Vladislav Dmitrievich Turbanov (vladislav@turbanov.ru)
 * Community forums: https://talk.turbanov.ru
 * Copyright 2019 - 2023, SP Vladislav Dmitrievich Turbanov
 * Made in Russia, Moscow City, Chekhov City
 */

 /*
  * BattleFrame
  * Refactor: 2025
  * Author: Leroy Works
  */

#include "NeighborGridComponent.h"
#include "Runtime/Core/Public/Async/ParallelFor.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "BitMask.h"
#include "Traits/RegisterMultiple.h"
#include "Traits/Collider.h"
#include "Traits/Located.h"
#include "Traits/BoxObstacle.h"
#include "Traits/Moving.h"
#include "Traits/SphereObstacle.h"
#include "Traits/Appearing.h"
#include "Traits/Move.h"
#include "Traits/Trace.h"
#include "Traits/Avoiding.h"
#include "Traits/Corpse.h"
#include "Traits/Dying.h"
#include "Traits/Activated.h"
#include "Traits/Agent.h"
#include "Traits/Patrol.h"
#include "Traits/Collider.h"
#include "Traits/Trace.h"
#include "Traits/Located.h"
#include "BattleFrameFunctionLibraryRT.h"
#include "Kismet/BlueprintAsyncActionBase.h"

UNeighborGridComponent::UNeighborGridComponent()
{
	bWantsInitializeComponent = true;
}

void UNeighborGridComponent::BeginPlay()
{
	Super::BeginPlay();

	DefineFilters();
}

void UNeighborGridComponent::InitializeComponent()
{
	DoInitializeCells();
	GetBounds();
}

//--------------------------------------------Tracing----------------------------------------------------------------

// Multi Trace For Subjects
void UNeighborGridComponent::SphereTraceForSubjects
(
	int32 KeepCount,
	const FVector& Origin,
	const float Radius,
	const bool bCheckVisibility,
	const FVector& CheckOrigin,
	const float CheckRadius,
	ESortMode SortMode,
	const FVector& SortOrigin,
	const FSubjectArray& IgnoreSubjects,
	const FFilter& Filter,
	bool& Hit,
	TArray<FTraceResult>& Results
) const
{
	//TRACE_CPUPROFILER_EVENT_SCOPE_STR("SphereTraceForSubjects");
	Results.Reset();

	// 特殊处理标志
	const bool bSingleResult = (KeepCount == 1);
	const bool bNoCountLimit = (KeepCount == -1);

	// 将忽略列表转换为集合以便快速查找
	const TSet<FSubjectHandle> IgnoreSet(IgnoreSubjects.Subjects);

	// 扩展搜索范围 - 使用各轴独立的CellSize
	const FVector CellRadius = CellSize * 0.5f;
	const float MaxCellRadius = FMath::Max3(CellRadius.X, CellRadius.Y, CellRadius.Z);
	const float ExpandedRadius = Radius + MaxCellRadius * FMath::Sqrt(2.0f);
	const FVector Range(ExpandedRadius);

	const FIntVector CagePosMin = WorldToCage(Origin - Range);
	const FIntVector CagePosMax = WorldToCage(Origin + Range);

	// 跟踪最佳结果（用于KeepCount=1的情况）
	FTraceResult BestResult;
	float BestDistSq = (SortMode == ESortMode::NearToFar) ? FLT_MAX : -FLT_MAX;

	// 临时存储所有结果（用于需要排序或随机的情况）
	TArray<FTraceResult> TempResults;

	// 预收集候选格子并按距离排序
	TArray<FIntVector> CandidateCells;

	for (int32 z = CagePosMin.Z; z <= CagePosMax.Z; ++z)
	{
		for (int32 y = CagePosMin.Y; y <= CagePosMax.Y; ++y)
		{
			for (int32 x = CagePosMin.X; x <= CagePosMax.X; ++x)
			{
				const FIntVector CellPos(x, y, z);
				if (!IsInside(CellPos)) continue;

				const FVector CellCenter = CageToWorld(CellPos);
				const float DistSq = FVector::DistSquared(CellCenter, Origin);

				if (DistSq > FMath::Square(ExpandedRadius)) continue;

				CandidateCells.Add(CellPos);
			}
		}
	}

	// 根据SortMode对格子进行排序
	if (SortMode != ESortMode::None)
	{
		CandidateCells.Sort([this, SortOrigin, SortMode](const FIntVector& A, const FIntVector& B) {
			const float DistSqA = FVector::DistSquared(CageToWorld(A), SortOrigin);
			const float DistSqB = FVector::DistSquared(CageToWorld(B), SortOrigin);
			return SortMode == ESortMode::NearToFar ? DistSqA < DistSqB : DistSqA > DistSqB;
			});
	}

	// 计算提前终止阈值
	float ThresholdDistanceSq = FLT_MAX;
	if (!bNoCountLimit && KeepCount > 0 && SortMode != ESortMode::None)
	{
		// 计算阈值：当前最远结果的距离 + 2倍格子对角线距离（使用最大轴尺寸）
		const float MaxCellSize = CellSize.GetMax();
		const float ThresholdDistance = FMath::Sqrt(BestDistSq) + 2.0f * MaxCellSize * FMath::Sqrt(2.0f);
		ThresholdDistanceSq = FMath::Square(ThresholdDistance);
	}

	// 遍历检测
	for (const FIntVector& CellPos : CandidateCells)
	{
		// 提前终止检查
		if (!bNoCountLimit && KeepCount > 0 && SortMode != ESortMode::None)
		{
			const float CellDistSq = FVector::DistSquared(CageToWorld(CellPos), SortOrigin);
			if ((SortMode == ESortMode::NearToFar && CellDistSq > ThresholdDistanceSq) ||
				(SortMode == ESortMode::FarToNear && CellDistSq < ThresholdDistanceSq))
			{
				break;
			}
		}

		const auto& CellData = At(CellPos);

		for (const FAvoiding& SubjectData : CellData.Subjects)
		{
			const FSubjectHandle Subject = SubjectData.SubjectHandle;
			if (IgnoreSet.Contains(Subject)) continue;
			if (!Subject.Matches(Filter)) continue;

			const FVector SubjectPos = SubjectData.Location;
			const float SubjectRadius = SubjectData.Radius;

			// 距离检查
			const FVector Delta = SubjectPos - Origin;
			const float DistSq = Delta.SizeSquared();
			const float CombinedRadius = Radius + SubjectRadius;
			if (DistSq > FMath::Square(CombinedRadius)) continue;

			if (bCheckVisibility)
			{
				bool bVisibilityHit = false;
				FTraceResult VisibilityResult;

				const FVector ToSubjectDir = (SubjectPos - CheckOrigin).GetSafeNormal();
				const FVector SubjectSurfacePoint = SubjectPos - (ToSubjectDir * SubjectRadius);

				SphereSweepForObstacle(CheckOrigin, SubjectSurfacePoint, CheckRadius, bVisibilityHit, VisibilityResult);

				if (bVisibilityHit) continue;
			}

			const float CurrentDistSq = FVector::DistSquared(SortOrigin, SubjectPos);

			if (bSingleResult)
			{
				bool bIsBetter = false;
				if (SortMode == ESortMode::NearToFar)
				{
					bIsBetter = (CurrentDistSq < BestDistSq);
				}
				else if (SortMode == ESortMode::FarToNear)
				{
					bIsBetter = (CurrentDistSq > BestDistSq);
				}
				else // ESortMode::None
				{
					bIsBetter = true;
				}

				if (bIsBetter)
				{
					BestResult.Subject = Subject;
					BestResult.Location = SubjectPos;
					BestResult.CachedDistSq = CurrentDistSq;
					BestDistSq = CurrentDistSq;

					// 更新阈值
					if (!bNoCountLimit && KeepCount > 0)
					{
						const float MaxCellSize = CellSize.GetMax();
						const float ThresholdDistance = FMath::Sqrt(BestDistSq) + 2.0f * MaxCellSize * FMath::Sqrt(2.0f);
						ThresholdDistanceSq = FMath::Square(ThresholdDistance);
					}
				}
			}
			else
			{
				FTraceResult Result;
				Result.Subject = Subject;
				Result.Location = SubjectPos;
				Result.CachedDistSq = CurrentDistSq;
				TempResults.Add(Result);

				// 当收集到足够结果时更新阈值
				if (!bNoCountLimit && KeepCount > 0 && SortMode != ESortMode::None && TempResults.Num() >= KeepCount)
				{
					// 找到当前第KeepCount个最佳结果的距离
					float CurrentThresholdDistSq = 0.0f;
					if (SortMode == ESortMode::NearToFar)
					{
						CurrentThresholdDistSq = TempResults[KeepCount - 1].CachedDistSq;
					}
					else // FarToNear
					{
						CurrentThresholdDistSq = TempResults.Last().CachedDistSq;
					}

					// 更新阈值
					const float MaxCellSize = CellSize.GetMax();
					const float ThresholdDistance = FMath::Sqrt(CurrentThresholdDistSq) + 2.0f * MaxCellSize * FMath::Sqrt(2.0f);
					ThresholdDistanceSq = FMath::Square(ThresholdDistance);
				}
			}
		}
	}

	// 处理结果
	if (bSingleResult)
	{
		if (BestDistSq != FLT_MAX && BestDistSq != -FLT_MAX)
		{
			Results.Add(BestResult);
		}
	}
	else if (TempResults.Num() > 0)
	{
		// 处理排序或随机化
		if (SortMode != ESortMode::None)
		{
			// 应用排序（无论KeepCount是否为-1）
			TempResults.Sort([SortMode](const FTraceResult& A, const FTraceResult& B) {
				return SortMode == ESortMode::NearToFar ?
					A.CachedDistSq < B.CachedDistSq :
					A.CachedDistSq > B.CachedDistSq;
				});
		}
		else
		{
			// 随机打乱结果
			if (TempResults.Num() > 1)
			{
				for (int32 i = TempResults.Num() - 1; i > 0; --i)
				{
					const int32 j = FMath::Rand() % (i + 1);
					TempResults.Swap(i, j);
				}
			}
		}

		// 应用数量限制（只有当KeepCount>0时）
		if (!bNoCountLimit && KeepCount > 0 && TempResults.Num() > KeepCount)
		{
			TempResults.SetNum(KeepCount);
		}

		Results.Append(TempResults);
	}

	Hit = !Results.IsEmpty();
}

// Multi Sweep Trace For Subjects
void UNeighborGridComponent::SphereSweepForSubjects
(
	int32 KeepCount,
	const FVector& Start,
	const FVector& End,
	const float Radius,
	const bool bCheckVisibility,
	const FVector& CheckOrigin,
	const float CheckRadius,
	ESortMode SortMode,
	const FVector& SortOrigin,
	const FSubjectArray& IgnoreSubjects,
	const FFilter& Filter,
	bool& Hit,
	TArray<FTraceResult>& Results
) const
{
	//TRACE_CPUPROFILER_EVENT_SCOPE_STR("SphereSweepForSubjects");
	Results.Reset(); // Clear result array

	// Convert ignore list to set for fast lookup
	const TSet<FSubjectHandle> IgnoreSet(IgnoreSubjects.Subjects);

	// Get cells along the sweep path (already handles FVector CellSize)
	TArray<FIntVector> GridCells = SphereSweepForCells(Start, End, Radius);

	const FVector TraceDir = (End - Start).GetSafeNormal();
	const float TraceLength = FVector::Distance(Start, End);

	// Temporary array to store unsorted results
	TArray<FTraceResult> TempResults;

	// Check subjects in each cell
	for (const FIntVector& CellIndex : GridCells)
	{
		if (!IsInside(CellIndex)) continue;

		const auto& CageCell = At(CellIndex.X, CellIndex.Y, CellIndex.Z);

		for (const FAvoiding& Data : CageCell.Subjects)
		{
			const FSubjectHandle Subject = Data.SubjectHandle;

			// Check if in ignore list
			if (IgnoreSet.Contains(Subject)) continue;

			// Validity checks
			if (!Subject.IsValid() || !Subject.Matches(Filter)) continue;

			const FVector SubjectPos = Data.Location;
			float SubjectRadius = Data.Radius;

			// Distance calculations
			const FVector ToSubject = SubjectPos - Start;
			const float ProjOnTrace = FVector::DotProduct(ToSubject, TraceDir);

			// Initial filtering
			const float ProjThreshold = SubjectRadius + Radius;
			if (ProjOnTrace < -ProjThreshold || ProjOnTrace > TraceLength + ProjThreshold) continue;

			// Precise distance check (still spherical)
			const float ClampedProj = FMath::Clamp(ProjOnTrace, 0.0f, TraceLength);
			const FVector NearestPoint = Start + ClampedProj * TraceDir;
			const float CombinedRadSq = FMath::Square(Radius + SubjectRadius);

			if (FVector::DistSquared(NearestPoint, SubjectPos) < CombinedRadSq)
			{
				if (bCheckVisibility)
				{
					// Perform visibility check
					bool bHit = false;
					FTraceResult VisibilityResult;

					// Calculate the surface point on the subject's sphere
					const FVector ToSubjectDir = (SubjectPos - CheckOrigin).GetSafeNormal();
					const FVector SubjectSurfacePoint = SubjectPos - (ToSubjectDir * SubjectRadius);

					SphereSweepForObstacle(CheckOrigin, SubjectSurfacePoint, CheckRadius, bHit, VisibilityResult);

					if (bHit) continue; // Path is blocked, skip this subject
				}

				// Create FTraceResult and add to temp results array
				FTraceResult Result;
				Result.Subject = Subject;
				Result.Location = SubjectPos;
				Result.CachedDistSq = FVector::DistSquared(SortOrigin, SubjectPos);
				TempResults.Add(Result);
			}
		}
	}

	// Sorting logic
	if (SortMode != ESortMode::None)
	{
		TempResults.Sort([SortMode](const FTraceResult& A, const FTraceResult& B)
			{
				if (SortMode == ESortMode::NearToFar)
				{
					return A.CachedDistSq < B.CachedDistSq;
				}
				else // FarToNear
				{
					return A.CachedDistSq > B.CachedDistSq;
				}
			});
	}

	// Apply KeepCount limit
	const bool bHasLimit = (KeepCount > 0);
	int32 Count = 0;

	for (const FTraceResult& Result : TempResults)
	{
		Results.Add(Result);
		Count++;

		if (bHasLimit && Count >= KeepCount)
		{
			break;
		}
	}

	Hit = !Results.IsEmpty();
}


void UNeighborGridComponent::SectorTraceForSubjects
(
	int32 KeepCount,
	const FVector& Origin,
	const float Radius,
	const float Height,
	const FVector& Direction,
	const float Angle,
	const bool bCheckVisibility,
	const FVector& CheckOrigin,
	const float CheckRadius,
	ESortMode SortMode,
	const FVector& SortOrigin,
	const FSubjectArray& IgnoreSubjects,
	const FFilter& Filter,
	bool& Hit,
	TArray<FTraceResult>& Results
) const
{
	//TRACE_CPUPROFILER_EVENT_SCOPE_STR("SectorTraceForSubjects");
	Results.Reset();

	// 特殊处理标志
	const bool bFullCircle = FMath::IsNearlyEqual(Angle, 360.0f, KINDA_SMALL_NUMBER);
	const bool bSingleResult = (KeepCount == 1);
	const bool bNoCountLimit = (KeepCount == -1);

	// 将忽略列表转换为集合以便快速查找
	const TSet<FSubjectHandle> IgnoreSet(IgnoreSubjects.Subjects);

	const FVector NormalizedDir = Direction.GetSafeNormal2D();
	const float HalfAngleRad = FMath::DegreesToRadians(Angle * 0.5f);
	const float CosHalfAngle = FMath::Cos(HalfAngleRad);

	// 计算扇形的两个边界方向（如果不是全圆）
	FVector LeftBoundDir, RightBoundDir;
	if (!bFullCircle)
	{
		LeftBoundDir = NormalizedDir.RotateAngleAxis(Angle * 0.5f, FVector::UpVector);
		RightBoundDir = NormalizedDir.RotateAngleAxis(-Angle * 0.5f, FVector::UpVector);
	}

	// 扩展搜索范围 - 使用各轴独立的CellSize
	const FVector CellRadius = CellSize * 0.5f;
	const float ExpandedRadiusXY = Radius + FMath::Max(CellRadius.X, CellRadius.Y) * FMath::Sqrt(2.0f);
	const float ExpandedHeight = Height / 2.0f + CellRadius.Z;
	const FVector Range(ExpandedRadiusXY, ExpandedRadiusXY, ExpandedHeight);

	const FIntVector CagePosMin = WorldToCage(Origin - Range);
	const FIntVector CagePosMax = WorldToCage(Origin + Range);

	// 跟踪最佳结果（用于KeepCount=1的情况）
	FTraceResult BestResult;
	float BestDistSq = (SortMode == ESortMode::NearToFar) ? FLT_MAX : -FLT_MAX;

	// 临时存储所有结果（用于需要排序或随机的情况）
	TArray<FTraceResult> TempResults;

	// 预收集候选格子并按距离排序
	TArray<FIntVector> CandidateCells;

	for (int32 z = CagePosMin.Z; z <= CagePosMax.Z; ++z)
	{
		for (int32 x = CagePosMin.X; x <= CagePosMax.X; ++x)
		{
			for (int32 y = CagePosMin.Y; y <= CagePosMax.Y; ++y)
			{
				const FIntVector CellPos(x, y, z);
				if (!IsInside(CellPos)) continue;

				const FVector CellCenter = CageToWorld(CellPos);
				const FVector DeltaXY = (CellCenter - Origin) * FVector(1, 1, 0);
				const float DistSqXY = DeltaXY.SizeSquared();

				if (DistSqXY > FMath::Square(ExpandedRadiusXY)) continue;

				const float VerticalDist = FMath::Abs(CellCenter.Z - Origin.Z);
				if (VerticalDist > ExpandedHeight) continue;

				if (!bFullCircle && DistSqXY > SMALL_NUMBER)
				{
					const FVector ToCellDirXY = DeltaXY.GetSafeNormal();
					const float DotProduct = FVector::DotProduct(NormalizedDir, ToCellDirXY);

					if (DotProduct < CosHalfAngle)
					{
						FVector ClosestPoint;
						float DistToLeftBound = FMath::PointDistToLine(CellCenter, Origin, LeftBoundDir, ClosestPoint);
						if (DistToLeftBound >= FMath::Max(CellRadius.X, CellRadius.Y))
						{
							float DistToRightBound = FMath::PointDistToLine(CellCenter, Origin, RightBoundDir, ClosestPoint);
							if (DistToRightBound >= FMath::Max(CellRadius.X, CellRadius.Y))
							{
								const FVector CellMin = CageToWorld(CellPos) - CellRadius;
								const FVector CellMax = CageToWorld(CellPos) + CellRadius;
								if (!(CellMin.X <= Origin.X && Origin.X <= CellMax.X &&
									CellMin.Y <= Origin.Y && Origin.Y <= CellMax.Y))
								{
									continue;
								}
							}
						}
					}
				}
				CandidateCells.Add(CellPos);
			}
		}
	}

	// 根据SortMode对格子进行排序
	if (SortMode != ESortMode::None)
	{
		CandidateCells.Sort([this, SortOrigin, SortMode](const FIntVector& A, const FIntVector& B) {
			const float DistSqA = FVector::DistSquared(CageToWorld(A), SortOrigin);
			const float DistSqB = FVector::DistSquared(CageToWorld(B), SortOrigin);
			return SortMode == ESortMode::NearToFar ? DistSqA < DistSqB : DistSqA > DistSqB;
			});
	}

	// 计算提前终止阈值
	float ThresholdDistanceSq = FLT_MAX;
	if (!bNoCountLimit && KeepCount > 0 && SortMode != ESortMode::None)
	{
		// 计算阈值：当前最远结果的距离 + 2倍格子对角线距离（使用最大轴尺寸）
		const float MaxCellSize = CellSize.GetMax();
		const float ThresholdDistance = FMath::Sqrt(BestDistSq) + 2.0f * MaxCellSize * FMath::Sqrt(2.0f);
		ThresholdDistanceSq = FMath::Square(ThresholdDistance);
	}

	// 遍历检测
	for (const FIntVector& CellPos : CandidateCells)
	{
		// 提前终止检查
		if (!bNoCountLimit && KeepCount > 0 && SortMode != ESortMode::None)
		{
			const float CellDistSq = FVector::DistSquared(CageToWorld(CellPos), SortOrigin);
			if ((SortMode == ESortMode::NearToFar && CellDistSq > ThresholdDistanceSq) ||
				(SortMode == ESortMode::FarToNear && CellDistSq < ThresholdDistanceSq))
			{
				break;
			}
		}

		const auto& CellData = At(CellPos);

		for (const FAvoiding& SubjectData : CellData.Subjects)
		{
			const FSubjectHandle Subject = SubjectData.SubjectHandle;
			if (IgnoreSet.Contains(Subject)) continue;
			if (!Subject.Matches(Filter)) continue;

			const FVector SubjectPos = SubjectData.Location;
			const float SubjectRadius = SubjectData.Radius;

			// 高度检查
			const float VerticalDist = FMath::Abs(SubjectPos.Z - Origin.Z);
			if (VerticalDist > (Height / 2.0f + SubjectRadius)) continue;

			// 距离检查
			const FVector DeltaXY = (SubjectPos - Origin) * FVector(1, 1, 0);
			const float DistSqXY = DeltaXY.SizeSquared();
			const float CombinedRadius = Radius + SubjectRadius;
			if (DistSqXY > FMath::Square(CombinedRadius)) continue;

			// 角度检查
			if (!bFullCircle && DistSqXY > SMALL_NUMBER)
			{
				const FVector ToSubjectDirXY = DeltaXY.GetSafeNormal();
				const float DotProduct = FVector::DotProduct(NormalizedDir, ToSubjectDirXY);
				if (DotProduct < CosHalfAngle) continue;
			}

			if (bCheckVisibility)
			{
				bool bVisibilityHit = false;
				FTraceResult VisibilityResult;

				const FVector ToSubjectDir = (SubjectPos - CheckOrigin).GetSafeNormal();
				const FVector SubjectSurfacePoint = SubjectPos - (ToSubjectDir * SubjectRadius);

				SphereSweepForObstacle(CheckOrigin, SubjectSurfacePoint, CheckRadius, bVisibilityHit, VisibilityResult);

				if (bVisibilityHit) continue;
			}

			const float CurrentDistSq = FVector::DistSquared(SortOrigin, SubjectPos);

			if (bSingleResult)
			{
				bool bIsBetter = false;
				if (SortMode == ESortMode::NearToFar)
				{
					bIsBetter = (CurrentDistSq < BestDistSq);
				}
				else if (SortMode == ESortMode::FarToNear)
				{
					bIsBetter = (CurrentDistSq > BestDistSq);
				}
				else // ESortMode::None
				{
					bIsBetter = true;
				}

				if (bIsBetter)
				{
					BestResult.Subject = Subject;
					BestResult.Location = SubjectPos;
					BestResult.CachedDistSq = CurrentDistSq;
					BestDistSq = CurrentDistSq;

					// 更新阈值
					if (!bNoCountLimit && KeepCount > 0)
					{
						const float MaxCellSize = CellSize.GetMax();
						const float ThresholdDistance = FMath::Sqrt(BestDistSq) + 2.0f * MaxCellSize * FMath::Sqrt(2.0f);
						ThresholdDistanceSq = FMath::Square(ThresholdDistance);
					}
				}
			}
			else
			{
				FTraceResult Result;
				Result.Subject = Subject;
				Result.Location = SubjectPos;
				Result.CachedDistSq = CurrentDistSq;
				TempResults.Add(Result);

				// 当收集到足够结果时更新阈值
				if (!bNoCountLimit && KeepCount > 0 && SortMode != ESortMode::None && TempResults.Num() >= KeepCount)
				{
					// 找到当前第KeepCount个最佳结果的距离
					float CurrentThresholdDistSq = 0.0f;
					if (SortMode == ESortMode::NearToFar)
					{
						CurrentThresholdDistSq = TempResults[KeepCount - 1].CachedDistSq;
					}
					else // FarToNear
					{
						CurrentThresholdDistSq = TempResults.Last().CachedDistSq;
					}

					// 更新阈值
					const float MaxCellSize = CellSize.GetMax();
					const float ThresholdDistance = FMath::Sqrt(CurrentThresholdDistSq) + 2.0f * MaxCellSize * FMath::Sqrt(2.0f);
					ThresholdDistanceSq = FMath::Square(ThresholdDistance);
				}
			}
		}
	}

	// 处理结果
	if (bSingleResult)
	{
		if (BestDistSq != FLT_MAX && BestDistSq != -FLT_MAX)
		{
			Results.Add(BestResult);
		}
	}
	else if (TempResults.Num() > 0)
	{
		// 处理排序或随机化
		if (SortMode != ESortMode::None)
		{
			// 应用排序（无论KeepCount是否为-1）
			TempResults.Sort([SortMode](const FTraceResult& A, const FTraceResult& B) {
				return SortMode == ESortMode::NearToFar ?
					A.CachedDistSq < B.CachedDistSq :
					A.CachedDistSq > B.CachedDistSq;
				});
		}
		else
		{
			// 随机打乱结果
			if (TempResults.Num() > 1)
			{
				for (int32 i = TempResults.Num() - 1; i > 0; --i)
				{
					const int32 j = FMath::Rand() % (i + 1);
					TempResults.Swap(i, j);
				}
			}
		}

		// 应用数量限制（只有当KeepCount>0时）
		if (!bNoCountLimit && KeepCount > 0 && TempResults.Num() > KeepCount)
		{
			TempResults.SetNum(KeepCount);
		}

		Results.Append(TempResults);
	}

	Hit = !Results.IsEmpty();
}

// Single Sweep Trace For Nearest Obstacle
void UNeighborGridComponent::SphereSweepForObstacle
(
	const FVector& Start,
	const FVector& End,
	float Radius,
	bool& Hit,
	FTraceResult& Result
) const
{
	Hit = false;
	Result = FTraceResult();
	float ClosestHitDistSq = FLT_MAX;

	TArray<FIntVector> PathCells = SphereSweepForCells(Start, End, Radius);
	const FVector CellExtent = CellSize * 0.5f;
	const float CellMaxRadius = CellExtent.GetMax(); // 最长半轴作为单元包围球半径

	auto ProcessObstacle = [&](const FAvoiding& Obstacle, float DistSqr)
		{
			if (DistSqr < ClosestHitDistSq)
			{
				ClosestHitDistSq = DistSqr;
				Hit = true;
				Result.Subject = Obstacle.SubjectHandle;
				Result.Location = Obstacle.Location;
				Result.CachedDistSq = DistSqr;
			}
		};

	for (const FIntVector& CellPos : PathCells)
	{
		const auto& Cell = At(CellPos);

		// 检查球形障碍物
		auto CheckSphereCollision = [&](const TArray<FAvoiding, TInlineAllocator<8>>& Obstacles)
			{
				for (const FAvoiding& Avoiding : Obstacles)
				{
					if (!Avoiding.SubjectHandle.IsValid()) continue;

					const FSphereObstacle* CurrentObstacle = Avoiding.SubjectHandle.GetTraitPtr<FSphereObstacle, EParadigm::Unsafe>();
					if (!CurrentObstacle) continue;
					if (CurrentObstacle->bExcluded) continue;

					// 计算球体到线段的最短距离平方
					const float DistSqr = FMath::PointDistToSegmentSquared(Avoiding.Location, Start, End);
					const float CombinedRadius = Radius + Avoiding.Radius;

					// 如果距离小于合并半径，则发生碰撞
					if (DistSqr <= FMath::Square(CombinedRadius))
					{
						ProcessObstacle(Avoiding, DistSqr);
					}
				}
			};

		// 检查静态/动态球形障碍物
		CheckSphereCollision(Cell.SphereObstacles);
		CheckSphereCollision(Cell.SphereObstaclesStatic);

		// 检查长方体障碍物碰撞
		auto CheckBoxCollision = [&](const TArray<FAvoiding, TInlineAllocator<8>>& Obstacles)
			{
				// 预计算常用向量
				const FVector Up = FVector::UpVector;
				const FVector SphereDir = (End - Start).GetSafeNormal();
				const float SphereRadiusSq = Radius * Radius;

				for (const FAvoiding& Avoiding : Obstacles)
				{
					if (!Avoiding.SubjectHandle.IsValid()) continue;

					const FBoxObstacle* CurrentObstacle = Avoiding.SubjectHandle.GetTraitPtr<FBoxObstacle, EParadigm::Unsafe>();
					if (!CurrentObstacle || CurrentObstacle->bExcluded) continue;

					// 检查并获取前一个障碍物
					const FBoxObstacle* PrevObstacle = CurrentObstacle->prevObstacle_.IsValid() ?
						CurrentObstacle->prevObstacle_.GetTraitPtr<FBoxObstacle, EParadigm::Unsafe>() : nullptr;
					if (!PrevObstacle || PrevObstacle->bExcluded) continue;

					// 检查并获取下一个障碍物
					const FBoxObstacle* NextObstacle = CurrentObstacle->nextObstacle_.IsValid() ?
						CurrentObstacle->nextObstacle_.GetTraitPtr<FBoxObstacle, EParadigm::Unsafe>() : nullptr;
					if (!NextObstacle || NextObstacle->bExcluded) continue;

					// 检查并获取下下个障碍物
					const FBoxObstacle* NextNextObstacle = NextObstacle->nextObstacle_.IsValid() ?
						NextObstacle->nextObstacle_.GetTraitPtr<FBoxObstacle, EParadigm::Unsafe>() : nullptr;
					if (!NextNextObstacle || NextNextObstacle->bExcluded) continue;

					// 获取所有位置并计算高度相关向量
					const FVector& CurrentPos = CurrentObstacle->point3d_;
					const FVector& PrevPos = PrevObstacle->point3d_;
					const FVector& NextPos = NextObstacle->point3d_;
					const FVector& NextNextPos = NextNextObstacle->point3d_;

					const float HalfHeight = CurrentObstacle->height_ * 0.5f;
					const FVector HalfHeightVec = Up * HalfHeight;

					// 预计算所有顶点
					const FVector BottomVertices[4] =
					{
						PrevPos - HalfHeightVec,
						CurrentPos - HalfHeightVec,
						NextPos - HalfHeightVec,
						NextNextPos - HalfHeightVec
					};

					const FVector TopVertices[4] =
					{
						PrevPos + HalfHeightVec,
						CurrentPos + HalfHeightVec,
						NextPos + HalfHeightVec,
						NextNextPos + HalfHeightVec
					};

					// 优化后的球体与长方体相交检测
					auto SphereIntersectsBox = [&](const FVector& SphereStart, const FVector& SphereEnd, float SphereRadius,
						const FVector BottomVerts[4], const FVector TopVerts[4]) -> bool
						{
							// 1. 快速AABB测试
							FVector BoxMin = BottomVerts[0];
							FVector BoxMax = TopVerts[0];
							for (int i = 1; i < 4; ++i)
							{
								BoxMin = BoxMin.ComponentMin(BottomVerts[i]);
								BoxMin = BoxMin.ComponentMin(TopVerts[i]);
								BoxMax = BoxMax.ComponentMax(BottomVerts[i]);
								BoxMax = BoxMax.ComponentMax(TopVerts[i]);
							}

							// 扩展AABB以包含球体半径
							BoxMin -= FVector(SphereRadius);
							BoxMax += FVector(SphereRadius);

							// 快速拒绝测试
							if (SphereStart.X > BoxMax.X && SphereEnd.X > BoxMax.X) return false;
							if (SphereStart.X < BoxMin.X && SphereEnd.X < BoxMin.X) return false;
							if (SphereStart.Y > BoxMax.Y && SphereEnd.Y > BoxMax.Y) return false;
							if (SphereStart.Y < BoxMin.Y && SphereEnd.Y < BoxMin.Y) return false;
							if (SphereStart.Z > BoxMax.Z && SphereEnd.Z > BoxMax.Z) return false;
							if (SphereStart.Z < BoxMin.Z && SphereEnd.Z < BoxMin.Z) return false;

							// 2. 分离轴定理(SAT)测试
							// 定义长方体的边
							const FVector BottomEdges[4] =
							{
								BottomVerts[1] - BottomVerts[0],
								BottomVerts[2] - BottomVerts[1],
								BottomVerts[3] - BottomVerts[2],
								BottomVerts[0] - BottomVerts[3]
							};

							const FVector SideEdges[4] =
							{
								TopVerts[0] - BottomVerts[0],
								TopVerts[1] - BottomVerts[1],
								TopVerts[2] - BottomVerts[2],
								TopVerts[3] - BottomVerts[3]
							};

							// 测试方向包括: 3个坐标轴、4个底面边与胶囊方向的叉积、4个侧边与胶囊方向的叉积
							const FVector TestDirs[11] =
							{
								FVector(1, 0, 0),  // X轴
								FVector(0, 1, 0),  // Y轴
								FVector(0, 0, 1),  // Z轴
								FVector::CrossProduct(BottomEdges[0], SphereDir).GetSafeNormal(),
								FVector::CrossProduct(BottomEdges[1], SphereDir).GetSafeNormal(),
								FVector::CrossProduct(BottomEdges[2], SphereDir).GetSafeNormal(),
								FVector::CrossProduct(BottomEdges[3], SphereDir).GetSafeNormal(),
								FVector::CrossProduct(SideEdges[0], SphereDir).GetSafeNormal(),
								FVector::CrossProduct(SideEdges[1], SphereDir).GetSafeNormal(),
								FVector::CrossProduct(SideEdges[2], SphereDir).GetSafeNormal(),
								FVector::CrossProduct(SideEdges[3], SphereDir).GetSafeNormal()
							};

							for (const FVector& Axis : TestDirs)
							{
								if (Axis.IsNearlyZero()) continue;

								// 计算长方体在轴上的投影范围
								float BoxMinProj = FLT_MAX, BoxMaxProj = -FLT_MAX;
								for (int i = 0; i < 4; ++i)
								{
									float BottomProj = FVector::DotProduct(BottomVerts[i], Axis);
									float TopProj = FVector::DotProduct(TopVerts[i], Axis);
									BoxMinProj = FMath::Min(BoxMinProj, FMath::Min(BottomProj, TopProj));
									BoxMaxProj = FMath::Max(BoxMaxProj, FMath::Max(BottomProj, TopProj));
								}

								// 计算胶囊在轴上的投影范围
								float CapsuleProj1 = FVector::DotProduct(SphereStart, Axis);
								float CapsuleProj2 = FVector::DotProduct(SphereEnd, Axis);
								float CapsuleMin = FMath::Min(CapsuleProj1, CapsuleProj2) - SphereRadius;
								float CapsuleMax = FMath::Max(CapsuleProj1, CapsuleProj2) + SphereRadius;

								// 检查是否分离
								if (CapsuleMax < BoxMinProj || CapsuleMin > BoxMaxProj)
								{
									return false;
								}
							}

							return true;
						};

					if (SphereIntersectsBox(Start, End, Radius, BottomVertices, TopVertices))
					{
						// 使用距离平方避免开方计算
						const float DistSqr = FVector::DistSquared(Start, Avoiding.Location);
						ProcessObstacle(Avoiding, DistSqr);
					}
				}
			};

		// 检查静态/动态盒体障碍物
		CheckBoxCollision(Cell.BoxObstacles);
		CheckBoxCollision(Cell.BoxObstaclesStatic);

		// ============== 新增提前退出判断 ==============
		if (Hit) // 只有发现过障碍物才需要判断
		{
			const FVector CellCenter = CageToWorld(CellPos);
			const float DistToSegmentSq = FMath::PointDistToSegmentSquared(CellCenter, Start, End);
			const float MinPossibleDist = FMath::Sqrt(DistToSegmentSq) - CellMaxRadius;

			if (MinPossibleDist > 0 && (MinPossibleDist * MinPossibleDist) > ClosestHitDistSq)
			{
				break; // 后续单元不可能更近，提前退出循环
			}
		}
	}
}


// To Do : 1.Sphere Trace For Subjects(can filter by direction angle)  2.Sphere Sweep For Subjects  3.Sphere Sweep For Subjects Async  4.Sector Trace For Subjects  5.Sector Trace For Subjects Async 
// 
// 1. IgnoreList  2.VisibilityCheck  3.AngleCheck  4.KeepCount  5.SortByDist  6.Async


//--------------------------------------------Avoidance---------------------------------------------------------------

void UNeighborGridComponent::Update()
{
	TRACE_CPUPROFILER_EVENT_SCOPE_STR("RVO2 Update");

	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("ResetCells");

		ParallelFor(OccupiedCellsQueues.Num(), [&](int32 Index)
		{
			int32 CellIndex;

			while (OccupiedCellsQueues[Index].Dequeue(CellIndex))
			{
				auto& Cell = Cells[CellIndex];
				Cell.Lock();
				Cell.Reset();
				Cell.Unlock();
			}			
		});
	}

	AMechanism* Mechanism = GetMechanism();

	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("RegisterNeighborGrid_Trace");

		auto Chain = Mechanism->EnchainSolid(RegisterNeighborGrid_Trace_Filter);
		UBattleFrameFunctionLibraryRT::CalculateThreadsCountAndBatchSize(Chain->IterableNum(), MaxThreadsAllowed, 1000, ThreadsCount, BatchSize);

		Chain->OperateConcurrently([&](FLocated& Located, FTrace& Trace)
		{
			const auto Location = Located.Location;

			if (UNLIKELY(!IsInside(Location))) return;

			Trace.Lock();
			Trace.NeighborGrid = this;// when agent traces, it uses this neighbor grid instance.
			Trace.Unlock();

		}, ThreadsCount, BatchSize);
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("RegisterNeighborGrid_SphereObstacle");

		auto Chain = Mechanism->EnchainSolid(RegisterNeighborGrid_SphereObstacle_Filter);
		UBattleFrameFunctionLibraryRT::CalculateThreadsCountAndBatchSize(Chain->IterableNum(), MaxThreadsAllowed, 1000, ThreadsCount, BatchSize);

		Chain->OperateConcurrently([&](FLocated& Located, FSphereObstacle& SphereObstacle)
		{
			if (UNLIKELY(!IsInside(Located.Location))) return;

			SphereObstacle.Lock();
			SphereObstacle.NeighborGrid = this;// when sphere obstacle override speed limit, it uses this neighbor grid instance.
			SphereObstacle.Unlock();

		}, ThreadsCount, BatchSize);
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("RegisterSubjectSingle");// agents are allowed to register themselves only in the cell where their origins are in, this helps to improve performance

		auto Chain = Mechanism->EnchainSolid(RegisterSubjectSingleFilter);
		UBattleFrameFunctionLibraryRT::CalculateThreadsCountAndBatchSize(Chain->IterableNum(), MaxThreadsAllowed, MinBatchSizeAllowed, ThreadsCount, BatchSize);

		Chain->OperateConcurrently([&](FSolidSubjectHandle Subject, FLocated& Located, FCollider& Collider, FAvoiding& Avoiding)
		{
			const auto Location = Located.Location;

			if (UNLIKELY(!IsInside(Location))) return;

			Avoiding.Location = Location;
			Avoiding.Radius = Collider.Radius;

			bool bShouldRegister = false;

			auto& Cell = At(Location);

			Cell.Lock();

			if (!Cell.Registered)
			{
				bShouldRegister = true;
				Cell.Registered = true;
			}
			
			Cell.Subjects.Add(Avoiding);

			Cell.Unlock();

			if (bShouldRegister)
			{
				int32 CellIndex = GetIndexAt(WorldToCage(Location));
				OccupiedCellsQueues[CellIndex % MaxThreadsAllowed].Enqueue(CellIndex);
			}

		}, ThreadsCount, BatchSize);
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("RegisterSubjectMultiple");// agents can also register themselves into all overlapping cells, thus improve avoidance precision

		auto Chain = Mechanism->EnchainSolid(RegisterSubjectMultipleFilter);
		UBattleFrameFunctionLibraryRT::CalculateThreadsCountAndBatchSize(Chain->IterableNum(), MaxThreadsAllowed, MinBatchSizeAllowed, ThreadsCount, BatchSize);

		Chain->OperateConcurrently([&](FSolidSubjectHandle Subject, FLocated& Located, FCollider& Collider, FAvoiding& Avoiding)
		{
			const auto Location = Located.Location;

			if (UNLIKELY(!IsInside(Location))) return;

			Avoiding.Location = Location;
			Avoiding.Radius = Collider.Radius;

			const FVector Range = FVector(Collider.Radius);

			// Compute the range of involved grid cells
			const FIntVector CagePosMin = WorldToCage(Location - Range);
			const FIntVector CagePosMax = WorldToCage(Location + Range);

			for (int32 i = CagePosMin.Z; i <= CagePosMax.Z; ++i)
			{
				for (int32 j = CagePosMin.Y; j <= CagePosMax.Y; ++j)
				{
					for (int32 k = CagePosMin.X; k <= CagePosMax.X; ++k)
					{
						const auto CurrentCellPos = FIntVector(k, j, i);

						if (!IsInside(CurrentCellPos)) continue;

						bool bShouldRegister = false;

						auto& Cell = At(CurrentCellPos);

						Cell.Lock();

						if (!Cell.Registered)
						{
							bShouldRegister = true;
							Cell.Registered = true;
						}

						Cell.Subjects.Add(Avoiding);

						Cell.Unlock();

						if (bShouldRegister)
						{
							int32 CellIndex = GetIndexAt(CurrentCellPos);
							OccupiedCellsQueues[CellIndex % MaxThreadsAllowed].Enqueue(CellIndex);
						}
					}
				}
			}

		}, ThreadsCount, BatchSize);
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("RegisterSphereObstacles");

		auto Chain = Mechanism->EnchainSolid(RegisterSphereObstaclesFilter);
		UBattleFrameFunctionLibraryRT::CalculateThreadsCountAndBatchSize(Chain->IterableNum(), MaxThreadsAllowed, MinBatchSizeAllowed, ThreadsCount, BatchSize);

		Chain->OperateConcurrently([&](FSolidSubjectHandle Subject, FSphereObstacle& SphereObstacle, FLocated& Located, FCollider& Collider, FAvoiding& Avoiding)
		{
			if (SphereObstacle.bStatic && SphereObstacle.bRegistered) return; // if static, we only register once

			const auto Location = Located.Location;

			if (UNLIKELY(!IsInside(Location))) return;

			Avoiding.Location = Location;
			Avoiding.Radius = Collider.Radius;

			const FVector Range = FVector(Collider.Radius);

			// Compute the range of involved grid cells
			const FIntVector CagePosMin = WorldToCage(Location - Range);
			const FIntVector CagePosMax = WorldToCage(Location + Range);

			for (int32 i = CagePosMin.Z; i <= CagePosMax.Z; ++i)
			{
				for (int32 j = CagePosMin.Y; j <= CagePosMax.Y; ++j)
				{
					for (int32 k = CagePosMin.X; k <= CagePosMax.X; ++k)
					{
						const auto CurrentCellPos = FIntVector(k, j, i);

						if (!IsInside(CurrentCellPos)) continue;

						bool bShouldRegister = false;

						auto& Cell = At(CurrentCellPos);

						Cell.Lock();

						if (!Cell.Registered)
						{
							bShouldRegister = true;
							Cell.Registered = true;
						}

						if (SphereObstacle.bStatic)
						{
							Cell.SphereObstaclesStatic.Add(Avoiding);
						}
						else
						{
							Cell.SphereObstacles.Add(Avoiding);
						}

						Cell.Unlock();

						if (bShouldRegister)
						{
							int32 CellIndex = GetIndexAt(CurrentCellPos);
							OccupiedCellsQueues[CellIndex % MaxThreadsAllowed].Enqueue(CellIndex);
						}
					}
				}
			}

			SphereObstacle.bRegistered = true;

		}, ThreadsCount, BatchSize);
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("RegisterBoxObstacles");

		auto Chain = Mechanism->EnchainSolid(RegisterBoxObstaclesFilter);
		UBattleFrameFunctionLibraryRT::CalculateThreadsCountAndBatchSize(Chain->IterableNum(), MaxThreadsAllowed, 1, ThreadsCount, BatchSize);

		Chain->OperateConcurrently([&](FSolidSubjectHandle Subject, FBoxObstacle& BoxObstacle, FAvoiding& Avoiding)
		{
			if (BoxObstacle.bStatic && BoxObstacle.bRegistered) return; // if static, we only register once

			const auto& Location = BoxObstacle.point3d_;
			Avoiding.Location = Location;

			const FBoxObstacle* PreObstaclePtr = BoxObstacle.prevObstacle_.GetTraitPtr<FBoxObstacle, EParadigm::Unsafe>();
			const FBoxObstacle* NextObstaclePtr = BoxObstacle.nextObstacle_.GetTraitPtr<FBoxObstacle, EParadigm::Unsafe>();

			if (NextObstaclePtr == nullptr || PreObstaclePtr == nullptr) return;

			const FVector& NextLocation = NextObstaclePtr->point3d_;
			const float ObstacleHeight = BoxObstacle.height_;

			const float StartZ = Location.Z;
			const float EndZ = StartZ + ObstacleHeight;
			TSet<FIntVector> AllGridCells;
			TArray<FIntVector> AllGridCellsArray;

			float CurrentLayerZ = StartZ;

			// 使用CellSize.Z作为Z轴步长
			while (CurrentLayerZ < EndZ)
			{
				const FVector LayerCurrentPoint = FVector(Location.X, Location.Y, CurrentLayerZ);
				const FVector LayerNextPoint = FVector(NextLocation.X, NextLocation.Y, CurrentLayerZ);

				// 使用最大轴尺寸的2倍作为扫描半径
				const float SweepRadius = CellSize.GetMax() * 2.0f;
				auto LayerCells = SphereSweepForCells(LayerCurrentPoint, LayerNextPoint, SweepRadius);

				for (const auto& CellPos : LayerCells)
				{
					AllGridCells.Add(CellPos);
				}

				CurrentLayerZ += CellSize.Z; // 使用Z轴尺寸作为步长
			}

			AllGridCellsArray = AllGridCells.Array();

			ParallelFor(AllGridCellsArray.Num(), [&](int32 Index)
				{
					const FIntVector& CellPos = AllGridCellsArray[Index];

					//if (!LIKELY(IsInside(CellPos))) return;

					bool bShouldRegister = false;

					auto& Cell = At(CellPos);

					Cell.Lock();

					if (!Cell.Registered)
					{
						bShouldRegister = true;
						Cell.Registered = true;
					}

					if (BoxObstacle.bStatic)
					{
						Cell.BoxObstaclesStatic.Add(Avoiding);
					}
					else
					{
						Cell.BoxObstacles.Add(Avoiding);
					}

					Cell.Unlock();

					if (bShouldRegister)
					{
						int32 CellIndex = GetIndexAt(CellPos);
						OccupiedCellsQueues[CellIndex % MaxThreadsAllowed].Enqueue(CellIndex);
					}
				});

			BoxObstacle.bRegistered = true;

		}, ThreadsCount, BatchSize);
	}

}

void UNeighborGridComponent::Decouple()
{
	TRACE_CPUPROFILER_EVENT_SCOPE_STR("RVO2 Decouple");

	const float DeltaTime = FMath::Clamp(GetWorld()->GetDeltaSeconds(),0,0.0333f);

	AMechanism* Mechanism = GetMechanism();
	auto Chain = Mechanism->EnchainSolid(DecoupleFilter);
	UBattleFrameFunctionLibraryRT::CalculateThreadsCountAndBatchSize(Chain->IterableNum(), MaxThreadsAllowed, MinBatchSizeAllowed, ThreadsCount, BatchSize);

	Chain->OperateConcurrently([&](FSolidSubjectHandle Subject, FMove& Move, FLocated& Located, FCollider& Collider, FMoving& Moving, FAvoidance& Avoidance, FAvoiding& Avoiding)
	{
		if (LIKELY(Avoidance.bEnable))
		{
			const auto& SelfLocation = Located.Location;
			Avoidance.Position = RVO::Vector2(SelfLocation.X, SelfLocation.Y);

			const auto SelfRadius = Collider.Radius;
			Avoidance.Radius = SelfRadius;

			const auto NeighborDist = Avoidance.NeighborDist;
			const float TotalRangeSqr = FMath::Square(SelfRadius + NeighborDist);
			const int32 MaxNeighbors = Avoidance.MaxNeighbors;

			//--------------------------Collect Subject Neighbors--------------------------------

			FFilter SubjectFilter = SubjectFilterBase;

			// 碰撞组
			if (!Avoidance.IgnoreGroups.IsEmpty())
			{
				const int32 ClampedGroups = FMath::Clamp(Avoidance.IgnoreGroups.Num(), 0, 9);

				for (int32 i = 0; i < ClampedGroups; ++i)
				{
					UBattleFrameFunctionLibraryRT::ExcludeAvoGroupTraitByIndex(Avoidance.IgnoreGroups[i], SubjectFilter);
				}
			}

			if (UNLIKELY(Subject.HasTrait<FDying>()))
			{
				SubjectFilter.Include<FDying>();// dying subject only collide with dying subjects
			}

			const FVector SubjectRange3D(NeighborDist + SelfRadius, NeighborDist + SelfRadius, SelfRadius);
			TArray<FIntVector> NeighbourCellCoords = GetNeighborCells(SelfLocation, SubjectRange3D);

			// 使用最大堆收集最近的SubjectNeighbors
			auto SubjectCompare = [&](const FAvoiding& A, const FAvoiding& B)
			{
				return FVector::DistSquared(SelfLocation, A.Location) > FVector::DistSquared(SelfLocation, B.Location);
			};

			TArray<FAvoiding> SubjectNeighbors;
			SubjectNeighbors.Reserve(MaxNeighbors);

			TSet<uint32> SeenHashes;
			SeenHashes.Reserve(MaxNeighbors);

			// this for loop is the most expensive code of all
			for (const auto& Coord : NeighbourCellCoords)
			{
				const auto& Subjects = At(Coord).Subjects;

				for (const auto& AvoData : Subjects)
				{
					// these check are arranged so for cache optimization
					// 距离检查
					const float DistSqr = FVector::DistSquared(SelfLocation, AvoData.Location);
					const float RadiusSqr = FMath::Square(AvoData.Radius) + TotalRangeSqr;

					if (DistSqr > RadiusSqr) continue;

					// 排除自身
					if (UNLIKELY(AvoData.SubjectHash == Avoiding.SubjectHash)) continue;

					// 去重
					if (UNLIKELY(SeenHashes.Contains(AvoData.SubjectHash))) continue;

					// Filter By Traits
					if (UNLIKELY(!AvoData.SubjectHandle.Matches(SubjectFilter))) continue;

					SeenHashes.Add(AvoData.SubjectHash);

					// we limit the amount of subjects. we keep the nearest MaxNeighbors amount of neighbors
					// 动态维护堆
					if (LIKELY(SubjectNeighbors.Num() < MaxNeighbors))
					{
						SubjectNeighbors.HeapPush(AvoData, SubjectCompare);
					}
					else
					{
						const float HeapTopDist = FVector::DistSquared(SelfLocation, SubjectNeighbors.HeapTop().Location);

						if (UNLIKELY(DistSqr < HeapTopDist))
						{
							// 弹出时同步移除哈希记录
							SeenHashes.Remove(SubjectNeighbors.HeapTop().SubjectHash);
							SubjectNeighbors.HeapPopDiscard(SubjectCompare);
							SubjectNeighbors.HeapPush(AvoData, SubjectCompare);
						}
					}
				}
			}

			//----------------------------Try Avoid SubjectNeighbors---------------------------------

			Avoidance.MaxSpeed = FMath::Clamp(Move.MoveSpeed * Moving.PassiveSpeedMult, Avoidance.RVO_MinAvoidSpeed, FLT_MAX);
			Avoidance.DesiredVelocity = RVO::Vector2(Moving.DesiredVelocity.X, Moving.DesiredVelocity.Y);//copy into rvo trait
			Avoidance.CurrentVelocity = RVO::Vector2(Moving.CurrentVelocity.X, Moving.CurrentVelocity.Y);//copy into rvo trait

			TArray<FAvoiding> EmptyArray;

			ComputeNewVelocity(Avoidance, SubjectNeighbors, EmptyArray, DeltaTime);

			if (!Moving.bFalling && !(Moving.LaunchTimer > 0))
			{
				FVector AvoidingVelocity(Avoidance.AvoidingVelocity.x(), Avoidance.AvoidingVelocity.y(), 0);
				FVector CurrentVelocity(Avoidance.CurrentVelocity.x(), Avoidance.CurrentVelocity.y(), 0);
				FVector InterpedVelocity = FMath::VInterpTo(CurrentVelocity, AvoidingVelocity, DeltaTime, FMath::Clamp(Move.Acceleration / 100, 0.0001, FLT_MAX));
				Moving.CurrentVelocity = FVector(InterpedVelocity.X, InterpedVelocity.Y, Moving.CurrentVelocity.Z); // velocity can only change so much because of inertia
			}

			//---------------------------Collect Obstacle Neighbors--------------------------------

			const float ObstacleRange = Avoidance.RVO_TimeHorizon_Obstacle * Avoidance.MaxSpeed + Avoidance.Radius;
			const FVector ObstacleRange3D(ObstacleRange, ObstacleRange, Avoidance.Radius);
			TArray<FIntVector> ObstacleCellCoords = GetNeighborCells(SelfLocation, ObstacleRange3D);

			TSet<FAvoiding> ValidSphereObstacleNeighbors;
			TSet<FAvoiding> ValidBoxObstacleNeighbors;
			ValidSphereObstacleNeighbors.Reserve(MaxNeighbors);
			ValidBoxObstacleNeighbors.Reserve(MaxNeighbors);

			for (const FIntVector& Coord : ObstacleCellCoords)
			{
				const auto& Cell = At(Coord);

				// 定义处理 SphereObstacles 的 Lambda 函数
				auto ProcessSphereObstacles = [&](const TArray<FAvoiding, TInlineAllocator<8>>& Obstacles)
				{
					ValidSphereObstacleNeighbors.Append(Obstacles);
				};

				ProcessSphereObstacles(Cell.SphereObstacles);
				ProcessSphereObstacles(Cell.SphereObstaclesStatic);

				// 定义处理 BoxObstacles 的 Lambda 函数
				auto ProcessBoxObstacles = [&](const TArray<FAvoiding, TInlineAllocator<8>>& Obstacles)
				{
					for (const FAvoiding& AvoData : Obstacles)
						{
							const FSubjectHandle ObstacleHandle = AvoData.SubjectHandle;
							if (!ObstacleHandle.IsValid()) continue;
							const auto ObstacleData = ObstacleHandle.GetTraitPtr<FBoxObstacle, EParadigm::Unsafe>();
							if (!ObstacleData) continue;
							const FSubjectHandle NextObstacleHandle = ObstacleData->nextObstacle_;
							if (!NextObstacleHandle.IsValid()) continue;
							const auto NextObstacleData = NextObstacleHandle.GetTraitPtr<FBoxObstacle, EParadigm::Unsafe>();
							if (!NextObstacleData) continue;

							const FVector ObstaclePoint = ObstacleData->point3d_;
							const float ObstacleHalfHeight = ObstacleData->height_ * 0.5f;
							const FVector NextPoint = NextObstacleData->point3d_;

							// Z 轴范围检查
							const float ObstacleZMin = ObstaclePoint.Z - ObstacleHalfHeight;
							const float ObstacleZMax = ObstaclePoint.Z + ObstacleHalfHeight;
							const float SubjectZMin = SelfLocation.Z - SelfRadius;
							const float SubjectZMax = SelfLocation.Z + SelfRadius;

							if (SubjectZMax < ObstacleZMin || SubjectZMin > ObstacleZMax) continue;

							// 2D 碰撞检测（RVO）
							RVO::Vector2 currentPos(Located.Location.X, Located.Location.Y);
							RVO::Vector2 obstacleStart(ObstaclePoint.X, ObstaclePoint.Y);
							RVO::Vector2 obstacleEnd(NextPoint.X, NextPoint.Y);

							float leftOfValue = RVO::leftOf(obstacleStart, obstacleEnd, currentPos);

							if (leftOfValue < 0.0f)
							{
								ValidBoxObstacleNeighbors.Add(AvoData);
							}
						}
				};

				ProcessBoxObstacles(Cell.BoxObstacles);
				ProcessBoxObstacles(Cell.BoxObstaclesStatic);
			}

			TArray<FAvoiding> SphereObstacleNeighbors = ValidSphereObstacleNeighbors.Array();
			TArray<FAvoiding> BoxObstacleNeighbors = ValidBoxObstacleNeighbors.Array();

			//-------------------------------Blocked By Obstacles------------------------------------

			Avoidance.MaxSpeed = Moving.bPushedBack ? FMath::Max(Moving.CurrentVelocity.Size2D(), Moving.PushBackSpeedOverride) : Moving.CurrentVelocity.Size2D();
			Avoidance.DesiredVelocity = RVO::Vector2(Moving.CurrentVelocity.X, Moving.CurrentVelocity.Y);//copy into rvo trait
			Avoidance.CurrentVelocity = RVO::Vector2(Moving.CurrentVelocity.X, Moving.CurrentVelocity.Y);//copy into rvo trait

			ComputeNewVelocity(Avoidance, SphereObstacleNeighbors, BoxObstacleNeighbors, DeltaTime);

			Moving.CurrentVelocity = FVector(Avoidance.AvoidingVelocity.x(), Avoidance.AvoidingVelocity.y(), Moving.CurrentVelocity.Z);// since obstacles are hard, we set velocity directly without any interpolation
		}

		Located.PreLocation = Located.Location;
		Located.Location += Moving.CurrentVelocity * DeltaTime;

	}, ThreadsCount, BatchSize);
}

void UNeighborGridComponent::Evaluate()
{
	Update();
	Decouple();
}

void UNeighborGridComponent::DefineFilters()
{
	RegisterNeighborGrid_Trace_Filter = FFilter::Make<FLocated, FTrace, FActivated>();
	RegisterNeighborGrid_SphereObstacle_Filter = FFilter::Make<FLocated, FSphereObstacle>();
	RegisterSubjectSingleFilter = FFilter::Make<FLocated, FCollider, FAvoiding, FActivated>().Exclude<FRegisterMultiple>();
	RegisterSubjectMultipleFilter = FFilter::Make<FLocated, FCollider, FAvoiding, FRegisterMultiple, FActivated>().Exclude<FSphereObstacle>();
	RegisterSphereObstaclesFilter = FFilter::Make<FLocated, FCollider, FAvoiding, FSphereObstacle>();
	RegisterBoxObstaclesFilter = FFilter::Make<FBoxObstacle, FLocated, FAvoiding>();
	SubjectFilterBase = FFilter::Make<FLocated, FCollider, FAvoidance, FAvoiding, FActivated>().Exclude<FSphereObstacle, FBoxObstacle, FCorpse>();
	SphereObstacleFilter = FFilter::Make<FSphereObstacle, FAvoiding, FAvoidance, FLocated, FCollider>();
	BoxObstacleFilter = FFilter::Make<FBoxObstacle, FAvoiding, FLocated>();
	DecoupleFilter = FFilter::Make<FAgent, FLocated, FCollider, FMove, FMoving, FAvoidance, FAvoiding, FActivated>().Exclude<FAppearing>();
}


//-------------------------------RVO2D Copyright 2023, EastFoxStudio. All Rights Reserved-------------------------------

void UNeighborGridComponent::ComputeNewVelocity(FAvoidance& Avoidance, const TArray<FAvoiding>& SubjectNeighbors, const TArray<FAvoiding>& ObstacleNeighbors, float TimeStep_)
{
	Avoidance.OrcaLines.clear();

	/* Create obstacle ORCA lines. */
	if (!ObstacleNeighbors.IsEmpty())
	{
		const float invTimeHorizonObst = 1.0f / Avoidance.RVO_TimeHorizon_Obstacle;

		for (const auto& Data : ObstacleNeighbors) 
		{
			FBoxObstacle* obstacle1 = Data.SubjectHandle.GetTraitPtr<FBoxObstacle, EParadigm::Unsafe>();
			FBoxObstacle* obstacle2 = obstacle1->nextObstacle_.GetTraitPtr<FBoxObstacle, EParadigm::Unsafe>();

			const RVO::Vector2 relativePosition1 = obstacle1->point_ - Avoidance.Position;
			const RVO::Vector2 relativePosition2 = obstacle2->point_ - Avoidance.Position;

			/*
			 * Check if velocity obstacle of obstacle is already taken care of by
			 * previously constructed obstacle ORCA lines.
			 */
			bool alreadyCovered = false;

			for (size_t j = 0; j < Avoidance.OrcaLines.size(); ++j) {
				if (RVO::det(invTimeHorizonObst * relativePosition1 - Avoidance.OrcaLines[j].point, Avoidance.OrcaLines[j].direction) - invTimeHorizonObst * Avoidance.Radius >= -RVO_EPSILON && det(invTimeHorizonObst * relativePosition2 - Avoidance.OrcaLines[j].point, Avoidance.OrcaLines[j].direction) - invTimeHorizonObst * Avoidance.Radius >= -RVO_EPSILON) {
					alreadyCovered = true;
					break;
				}
			}

			if (alreadyCovered) {
				continue;
			}

			/* Not yet covered. Check for collisions. */

			const float distSq1 = RVO::absSq(relativePosition1);
			const float distSq2 = RVO::absSq(relativePosition2);

			const float radiusSq = RVO::sqr(Avoidance.Radius);

			const RVO::Vector2 obstacleVector = obstacle2->point_ - obstacle1->point_;
			const float s = (-relativePosition1 * obstacleVector) / absSq(obstacleVector);
			const float distSqLine = absSq(-relativePosition1 - s * obstacleVector);

			RVO::Line line;

			if (s < 0.0f && distSq1 <= radiusSq) {
				/* Collision with left vertex. Ignore if non-convex. */
				if (obstacle1->isConvex_) {
					line.point = RVO::Vector2(0.0f, 0.0f);
					line.direction = normalize(RVO::Vector2(-relativePosition1.y(), relativePosition1.x()));
					Avoidance.OrcaLines.push_back(line);
				}
				continue;
			}
			else if (s > 1.0f && distSq2 <= radiusSq) {
				/* Collision with right vertex. Ignore if non-convex
				 * or if it will be taken care of by neighoring obstace */
				if (obstacle2->isConvex_ && det(relativePosition2, obstacle2->unitDir_) >= 0.0f) {
					line.point = RVO::Vector2(0.0f, 0.0f);
					line.direction = normalize(RVO::Vector2(-relativePosition2.y(), relativePosition2.x()));
					Avoidance.OrcaLines.push_back(line);
				}
				continue;
			}
			else if (s >= 0.0f && s < 1.0f && distSqLine <= radiusSq) {
				/* Collision with obstacle segment. */
				line.point = RVO::Vector2(0.0f, 0.0f);
				line.direction = -obstacle1->unitDir_;
				Avoidance.OrcaLines.push_back(line);
				continue;
			}

			/*
			 * No collision.
			 * Compute legs. When obliquely viewed, both legs can come from a single
			 * vertex. Legs extend cut-off line when nonconvex vertex.
			 */

			RVO::Vector2 leftLegDirection, rightLegDirection;

			if (s < 0.0f && distSqLine <= radiusSq) {
				/*
				 * Obstacle viewed obliquely so that left vertex
				 * defines velocity obstacle.
				 */
				if (!obstacle1->isConvex_) {
					/* Ignore obstacle. */
					continue;
				}

				obstacle2 = obstacle1;

				const float leg1 = std::sqrt(distSq1 - radiusSq);
				leftLegDirection = RVO::Vector2(relativePosition1.x() * leg1 - relativePosition1.y() * Avoidance.Radius, relativePosition1.x() * Avoidance.Radius + relativePosition1.y() * leg1) / distSq1;
				rightLegDirection = RVO::Vector2(relativePosition1.x() * leg1 + relativePosition1.y() * Avoidance.Radius, -relativePosition1.x() * Avoidance.Radius + relativePosition1.y() * leg1) / distSq1;
			}
			else if (s > 1.0f && distSqLine <= radiusSq) {
				/*
				 * Obstacle viewed obliquely so that
				 * right vertex defines velocity obstacle.
				 */
				if (!obstacle2->isConvex_) {
					/* Ignore obstacle. */
					continue;
				}

				obstacle1 = obstacle2;

				const float leg2 = std::sqrt(distSq2 - radiusSq);
				leftLegDirection = RVO::Vector2(relativePosition2.x() * leg2 - relativePosition2.y() * Avoidance.Radius, relativePosition2.x() * Avoidance.Radius + relativePosition2.y() * leg2) / distSq2;
				rightLegDirection = RVO::Vector2(relativePosition2.x() * leg2 + relativePosition2.y() * Avoidance.Radius, -relativePosition2.x() * Avoidance.Radius + relativePosition2.y() * leg2) / distSq2;
			}
			else {
				/* Usual situation. */
				if (obstacle1->isConvex_) {
					const float leg1 = std::sqrt(distSq1 - radiusSq);
					leftLegDirection = RVO::Vector2(relativePosition1.x() * leg1 - relativePosition1.y() * Avoidance.Radius, relativePosition1.x() * Avoidance.Radius + relativePosition1.y() * leg1) / distSq1;
				}
				else {
					/* Left vertex non-convex; left leg extends cut-off line. */
					leftLegDirection = -obstacle1->unitDir_;
				}

				if (obstacle2->isConvex_) {
					const float leg2 = std::sqrt(distSq2 - radiusSq);
					rightLegDirection = RVO::Vector2(relativePosition2.x() * leg2 + relativePosition2.y() * Avoidance.Radius, -relativePosition2.x() * Avoidance.Radius + relativePosition2.y() * leg2) / distSq2;
				}
				else {
					/* Right vertex non-convex; right leg extends cut-off line. */
					rightLegDirection = obstacle1->unitDir_;
				}
			}

			/*
			 * Legs can never point into neighboring edge when convex vertex,
			 * take cutoff-line of neighboring edge instead. If velocity projected on
			 * "foreign" leg, no constraint is added.
			 */

			const FSubjectHandle leftNeighbor = obstacle1->prevObstacle_;

			bool isLeftLegForeign = false;
			bool isRightLegForeign = false;

			if (obstacle1->isConvex_ && det(leftLegDirection, -obstacle1->unitDir_) >= 0.0f) {
				/* Left leg points into obstacle. */
				leftLegDirection = -obstacle1->unitDir_;
				isLeftLegForeign = true;
			}

			if (obstacle2->isConvex_ && det(rightLegDirection, obstacle2->unitDir_) <= 0.0f) {
				/* Right leg points into obstacle. */
				rightLegDirection = obstacle2->unitDir_;
				isRightLegForeign = true;
			}

			/* Compute cut-off centers. */
			const RVO::Vector2 leftCutoff = invTimeHorizonObst * (obstacle1->point_ - Avoidance.Position);
			const RVO::Vector2 rightCutoff = invTimeHorizonObst * (obstacle2->point_ - Avoidance.Position);
			const RVO::Vector2 cutoffVec = rightCutoff - leftCutoff;

			/* Project current velocity on velocity obstacle. */

			/* Check if current velocity is projected on cutoff circles. */
			const float t = (obstacle1 == obstacle2 ? 0.5f : ((Avoidance.CurrentVelocity - leftCutoff) * cutoffVec) / absSq(cutoffVec));
			const float tLeft = ((Avoidance.CurrentVelocity - leftCutoff) * leftLegDirection);
			const float tRight = ((Avoidance.CurrentVelocity - rightCutoff) * rightLegDirection);

			if ((t < 0.0f && tLeft < 0.0f) || (obstacle1 == obstacle2 && tLeft < 0.0f && tRight < 0.0f)) {
				/* Project on left cut-off circle. */
				const RVO::Vector2 unitW = normalize(Avoidance.CurrentVelocity - leftCutoff);

				line.direction = RVO::Vector2(unitW.y(), -unitW.x());
				line.point = leftCutoff + Avoidance.Radius * invTimeHorizonObst * unitW;
				Avoidance.OrcaLines.push_back(line);
				continue;
			}
			else if (t > 1.0f && tRight < 0.0f) {
				/* Project on right cut-off circle. */
				const RVO::Vector2 unitW = normalize(Avoidance.CurrentVelocity - rightCutoff);

				line.direction = RVO::Vector2(unitW.y(), -unitW.x());
				line.point = rightCutoff + Avoidance.Radius * invTimeHorizonObst * unitW;
				Avoidance.OrcaLines.push_back(line);
				continue;
			}

			/*
			 * Project on left leg, right leg, or cut-off line, whichever is closest
			 * to velocity.
			 */
			const float distSqCutoff = ((t < 0.0f || t > 1.0f || obstacle1 == obstacle2) ? std::numeric_limits<float>::infinity() : absSq(Avoidance.CurrentVelocity - (leftCutoff + t * cutoffVec)));
			const float distSqLeft = ((tLeft < 0.0f) ? std::numeric_limits<float>::infinity() : absSq(Avoidance.CurrentVelocity - (leftCutoff + tLeft * leftLegDirection)));
			const float distSqRight = ((tRight < 0.0f) ? std::numeric_limits<float>::infinity() : absSq(Avoidance.CurrentVelocity - (rightCutoff + tRight * rightLegDirection)));

			if (distSqCutoff <= distSqLeft && distSqCutoff <= distSqRight) {
				/* Project on cut-off line. */
				line.direction = -obstacle1->unitDir_;
				line.point = leftCutoff + Avoidance.Radius * invTimeHorizonObst * RVO::Vector2(-line.direction.y(), line.direction.x());
				Avoidance.OrcaLines.push_back(line);
				continue;
			}
			else if (distSqLeft <= distSqRight) {
				/* Project on left leg. */
				if (isLeftLegForeign) {
					continue;
				}

				line.direction = leftLegDirection;
				line.point = leftCutoff + Avoidance.Radius * invTimeHorizonObst * RVO::Vector2(-line.direction.y(), line.direction.x());
				Avoidance.OrcaLines.push_back(line);
				continue;
			}
			else {
				/* Project on right leg. */
				if (isRightLegForeign) {
					continue;
				}

				line.direction = -rightLegDirection;
				line.point = rightCutoff + Avoidance.Radius * invTimeHorizonObst * RVO::Vector2(-line.direction.y(), line.direction.x());
				Avoidance.OrcaLines.push_back(line);
				continue;
			}
		}
	}

	const size_t numObstLines = Avoidance.OrcaLines.size();

	/* Create agent ORCA lines. */
	if (!SubjectNeighbors.IsEmpty())
	{
		const float invTimeHorizon = 1.0f / Avoidance.RVO_TimeHorizon_Agent;

		for (const auto& Data : SubjectNeighbors) 
		{
			const auto& other = Data.SubjectHandle.GetTraitRef<FAvoidance, EParadigm::Unsafe>();
			const RVO::Vector2 relativePosition = other.Position - Avoidance.Position;
			const RVO::Vector2 relativeVelocity = Avoidance.CurrentVelocity - other.CurrentVelocity;
			const float distSq = absSq(relativePosition);
			const float combinedRadius = Avoidance.Radius + other.Radius;
			const float combinedRadiusSq = RVO::sqr(combinedRadius);

			RVO::Line line;
			RVO::Vector2 u;

			if (distSq > combinedRadiusSq) {
				/* No collision. */
				const RVO::Vector2 w = relativeVelocity - invTimeHorizon * relativePosition;
				/* Vector from cutoff center to relative velocity. */
				const float wLengthSq = RVO::absSq(w);

				const float dotProduct1 = w * relativePosition;

				if (dotProduct1 < 0.0f && RVO::sqr(dotProduct1) > combinedRadiusSq * wLengthSq) {
					/* Project on cut-off circle. */
					const float wLength = std::sqrt(wLengthSq);
					const RVO::Vector2 unitW = w / wLength;

					line.direction = RVO::Vector2(unitW.y(), -unitW.x());
					u = (combinedRadius * invTimeHorizon - wLength) * unitW;
				}
				else {
					/* Project on legs. */
					const float leg = std::sqrt(distSq - combinedRadiusSq);

					if (det(relativePosition, w) > 0.0f) {
						/* Project on left leg. */
						line.direction = RVO::Vector2(relativePosition.x() * leg - relativePosition.y() * combinedRadius, relativePosition.x() * combinedRadius + relativePosition.y() * leg) / distSq;
					}
					else {
						/* Project on right leg. */
						line.direction = -RVO::Vector2(relativePosition.x() * leg + relativePosition.y() * combinedRadius, -relativePosition.x() * combinedRadius + relativePosition.y() * leg) / distSq;
					}

					const float dotProduct2 = relativeVelocity * line.direction;

					u = dotProduct2 * line.direction - relativeVelocity;
				}
			}
			else {
				/* Collision. Project on cut-off circle of time timeStep. */
				const float invTimeStep = 1.0f / TimeStep_;

				/* Vector from cutoff center to relative velocity. */
				const RVO::Vector2 w = relativeVelocity - invTimeStep * relativePosition;

				const float wLength = abs(w);
				const RVO::Vector2 unitW = w / wLength;

				line.direction = RVO::Vector2(unitW.y(), -unitW.x());
				u = (combinedRadius * invTimeStep - wLength) * unitW;
			}

			line.point = Avoidance.CurrentVelocity + 0.5f * u;
			Avoidance.OrcaLines.push_back(line);
		}
	}

	size_t lineFail = LinearProgram2(Avoidance.OrcaLines, Avoidance.MaxSpeed, Avoidance.DesiredVelocity, false, Avoidance.AvoidingVelocity);

	if (lineFail < Avoidance.OrcaLines.size()) {
		LinearProgram3(Avoidance.OrcaLines, numObstLines, lineFail, Avoidance.MaxSpeed, Avoidance.AvoidingVelocity);
	}
}