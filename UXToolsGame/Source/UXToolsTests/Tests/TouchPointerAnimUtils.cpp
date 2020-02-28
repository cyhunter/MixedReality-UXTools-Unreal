// Fill out your copyright notice in the Description page of Project Settings.

#include "TouchPointerAnimUtils.h"

#include "Input/UxtTouchPointer.h"
#include "UXToolsTestUtils.h"

void UTestTouchPointerTarget::BeginPlay()
{
	Super::BeginPlay();

	HoverStartedCount = 0;
	HoverEndedCount = 0;
}

void UTestTouchPointerTarget::HoverStarted_Implementation(UUxtTouchPointer* Pointer)
{
	++HoverStartedCount;
}

void UTestTouchPointerTarget::HoverEnded_Implementation(UUxtTouchPointer* Pointer)
{
	++HoverEndedCount;
}

void UTestTouchPointerTarget::GraspStarted_Implementation(UUxtTouchPointer* Pointer)
{
	++GraspStartedCount;
}

void UTestTouchPointerTarget::GraspEnded_Implementation(UUxtTouchPointer* Pointer)
{
	++GraspEndedCount;
}

bool UTestTouchPointerTarget::GetClosestPointOnSurface_Implementation(const FVector& Point, FVector& OutPointOnSurface)
{
	if (GetOwner()->GetRootComponent() == nullptr)
	{
		return false;
	}

	OutPointOnSurface = GetOwner()->GetActorLocation();
	return true;
}

namespace TouchPointerAnimUtils
{

	/**
	 * Latent command that interpolates the pointers over multiple frames.
	 * Tests for correctness at each keyframe position.
	 */
	class FAnimatePointersCommand : public IAutomationLatentCommand
	{
	public:
		FAnimatePointersCommand(
			FAutomationTestBase* Test,
			const TouchAnimSequence& Sequence,
			float Duration = 1.0f)
			: Test(Test)
			, Sequence(Sequence)
			, Duration(Duration)
			, bIsFinished(false)
			, Section(-1)
			, TestKeyframeOnNextUpdate(false)
		{
			EventCountSequence = Sequence.ComputeTargetEventCounts();
		}

		virtual ~FAnimatePointersCommand()
		{}

		virtual bool Update() override
		{
			if (Sequence.GetPointers().empty())
			{
				return true;
			}

			// Test the current keyframe if flagged in the previous update.
			if (TestKeyframeOnNextUpdate)
			{
				Sequence.TestKeyframe(Test, EventCountSequence[Section], Section);
				TestKeyframeOnNextUpdate = false;
			}
			// One update for testing keyframe may be run after animation is finished.
			// In that case exit here.
			if (bIsFinished)
			{
				return true;
			}

			const int NumSections = (int)(Sequence.GetKeyframes().size() - 1);
			const float Elapsed = FPlatformTime::Seconds() - StartTime;
			bIsFinished = (Elapsed >= Duration * NumSections);
			const float AnimTime = Elapsed / Duration;

			{
				int NewSection = FMath::Clamp((int)AnimTime, 0, NumSections);
				if (NewSection != Section)
				{
					Section = NewSection;
					TestKeyframeOnNextUpdate = true;
				}
			}

			if (Section < Sequence.GetKeyframes().size() - 1)
			{
				float Alpha = AnimTime - (float)Section;
				const PointerKeyframe& start = Sequence.GetKeyframes()[Section];
				const PointerKeyframe& end = Sequence.GetKeyframes()[Section + 1];
				FVector location = FMath::Lerp(start.Location, end.Location, Alpha);

				for (UUxtTouchPointer* pointer : Sequence.GetPointers())
				{
					pointer->GetOwner()->SetActorLocation(location);
				}
			}

			if (Section < Sequence.GetKeyframes().size())
			{
				const PointerKeyframe& start = Sequence.GetKeyframes()[Section];
				for (UUxtTouchPointer* pointer : Sequence.GetPointers())
				{
					pointer->SetGrasped(start.bIsGrasped);
				}
			}

			return bIsFinished && !TestKeyframeOnNextUpdate;
		}

	private:

		FAutomationTestBase* Test;

		const TouchAnimSequence Sequence;

		/** Time in seconds between each pair of keyframes. */
		float Duration;

		bool bIsFinished;
		/** Current section of the keyframe list that is being interpolated. */
		int Section;
		/** Flag to enable testing of the keyframe in the next update.
		 *  Pointer components need to tick once to update overlaps and focused targets,
		 *  so delaying the keyframe test for one frame ensures the state is correctly updated.
		 */
		bool TestKeyframeOnNextUpdate;

		/** Per-keyframe event counts for each target. */
		std::vector<TargetEventCountMap> EventCountSequence;
	};

	void TouchAnimSequence::CreatePointers(UWorld* world, int Count)
	{
		Pointers.resize(Count);
		for (int i = 0; i < Count; ++i)
		{
			Pointers[i] = UXToolsTestUtils::CreateTouchPointer(world, FVector::ZeroVector);
		}
	}

	void TouchAnimSequence::AddTarget(UWorld* world, const FVector& pos)
	{
		const FString& targetFilename = TEXT("/Engine/BasicShapes/Cube.Cube");
		const float targetScale = 0.3f;
		Targets.emplace_back(UXToolsTestUtils::CreateTouchPointerTarget(world, pos, targetFilename, targetScale));
	}

	// Enter/Exit events must be incremented separately based on expected behavior.
	void TouchAnimSequence::AddMovementKeyframe(const FVector& pos)
	{
		PointerKeyframe& keyframe = CreateKeyframe();
		keyframe.Location = pos;
	};

	void TouchAnimSequence::AddGraspKeyframe(bool bEnableGrasp)
	{
		PointerKeyframe& keyframe = CreateKeyframe();
		keyframe.bIsGrasped = bEnableGrasp;
	}

	PointerKeyframe& TouchAnimSequence::CreateKeyframe()
	{
		PointerKeyframe keyframe;
		if (Keyframes.empty())
		{
			keyframe.Location = FVector::ZeroVector;
			keyframe.bIsGrasped = false;
			keyframe.ExpectedHoverTargetIndex = -1;
		}
		else
		{
			keyframe.Location = Keyframes.back().Location;
			keyframe.bIsGrasped = Keyframes.back().bIsGrasped;
			keyframe.ExpectedHoverTargetIndex = Keyframes.back().ExpectedHoverTargetIndex;
		}

		Keyframes.emplace_back(std::move(keyframe));

		return Keyframes.back();
	}

	void TouchAnimSequence::ExpectHoverTargetIndex(int TargetIndex, bool bExpectEvents)
	{
		Keyframes.back().ExpectedHoverTargetIndex = TargetIndex;
		Keyframes.back().bExpectEvents = bExpectEvents;
	}

	void TouchAnimSequence::ExpectHoverTargetNone(bool bExpectEvents)
	{
		ExpectHoverTargetIndex(-1, bExpectEvents);
	}

	std::vector<TargetEventCountMap> TouchAnimSequence::ComputeTargetEventCounts() const
	{
		std::vector<TargetEventCountMap> result;

		result.reserve(Keyframes.size());

		int PrevHoverTarget = -1;
		bool bPrevIsGrasped = false;
		for (const auto& Keyframe : Keyframes)
		{
			TargetEventCountMap KeyframeEventCounts;
			if (result.empty())
			{
				KeyframeEventCounts.resize(Targets.size(), TargetEventCount());
			}
			else
			{
				KeyframeEventCounts = result.back();
			}

			const int HoverTarget = Keyframe.ExpectedHoverTargetIndex;
			const bool bIsGrasped = Keyframe.bIsGrasped;

			// Increment expected event counts if the keyframe is expected to trigger events
			if (Keyframe.bExpectEvents)
			{
				if (HoverTarget != PrevHoverTarget)
				{
					// Hovered target changed: Increment the HoverEndCount of the previous target and the HoverStartCount of the new target.

					for (int Target = 0; Target < Targets.size(); ++Target)
					{
						if (Target == PrevHoverTarget)
						{
							KeyframeEventCounts[Target].HoverEndCount += Pointers.size();
						}
						if (Target == HoverTarget)
						{
							KeyframeEventCounts[Target].HoverStartCount += Pointers.size();
						}
					}
				}

				if (bIsGrasped != bPrevIsGrasped)
				{
					// Grasp changed: Increment GraspEndCount of the previous target when released, or the GraspStartCount of the current target when grasped.

					for (int Target = 0; Target < Targets.size(); ++Target)
					{
						if (!bIsGrasped && Target == PrevHoverTarget)
						{
							KeyframeEventCounts[Target].GraspEndCount += Pointers.size();
						}
						if (bIsGrasped && Target == HoverTarget)
						{
							KeyframeEventCounts[Target].GraspStartCount += Pointers.size();
						}
					}
				}
			}

			result.emplace_back(std::move(KeyframeEventCounts));
			PrevHoverTarget = HoverTarget;
			bPrevIsGrasped = bIsGrasped;
		}

		return result;
	}

	/** Test enter/exit events for all targets at the given keyframe position. */
	void TouchAnimSequence::TestKeyframe(FAutomationTestBase* Test, const TargetEventCountMap& EventCounts, int KeyframeIndex) const
	{
		for (int TargetIndex = 0; TargetIndex < Targets.size(); ++TargetIndex)
		{
			const UTestTouchPointerTarget* Target = Targets[TargetIndex];

			const TargetEventCount& ExpectedEventCounts = EventCounts[TargetIndex];

			FString whatHoverStarted; whatHoverStarted.Appendf(TEXT("Keyframe %d: Target %d HoverStarted count"), KeyframeIndex, TargetIndex);
			FString whatHoverEnded; whatHoverEnded.Appendf(TEXT("Keyframe %d: Target %d HoverEnded count"), KeyframeIndex, TargetIndex);
			FString whatGraspStarted; whatGraspStarted.Appendf(TEXT("Keyframe %d: Target %d GraspStarted count"), KeyframeIndex, TargetIndex);
			FString whatGraspEnded; whatGraspEnded.Appendf(TEXT("Keyframe %d: Target %d GraspEnded count"), KeyframeIndex, TargetIndex);
			Test->TestEqual(whatHoverStarted, Target->HoverStartedCount, ExpectedEventCounts.HoverStartCount);
			Test->TestEqual(whatHoverEnded, Target->HoverEndedCount, ExpectedEventCounts.HoverEndCount);
			Test->TestEqual(whatGraspStarted, Target->GraspStartedCount, ExpectedEventCounts.GraspStartCount);
			Test->TestEqual(whatGraspEnded, Target->GraspEndedCount, ExpectedEventCounts.GraspEndCount);
		}
	}

	void TouchAnimSequence::RunInterpolatedPointersTest(FAutomationTestBase* Test, float KeyframeDuration) const
	{
		ADD_LATENT_AUTOMATION_COMMAND(FAnimatePointersCommand(Test, *this, KeyframeDuration));
	}

	void TouchAnimSequence::RunTeleportingPointersTest(FAutomationTestBase* Test) const
	{
		auto EventCountSequence = ComputeTargetEventCounts();

		for (int Section = 0; Section < Keyframes.size(); ++Section)
		{
			const PointerKeyframe& keyframe = Keyframes[Section];

			for (UUxtTouchPointer* pointer : Pointers)
			{
				pointer->GetOwner()->SetActorLocation(keyframe.Location);
			}

			TestKeyframe(Test, EventCountSequence[Section], Section);
		}
	}

}