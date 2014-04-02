// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

#include "EnginePrivate.h"


UFloatingPawnMovement::UFloatingPawnMovement(const class FPostConstructInitializeProperties& PCIP)
	: Super(PCIP)
{
	MaxSpeed = 1200.f;
	Acceleration = 4000.f;
	Deceleration = 8000.f;
	bPositionCorrected = false;

	ResetMoveState();
}


void UFloatingPawnMovement::TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!PawnOwner || !UpdatedComponent)
	{
		return;
	}

	const AController* Controller = PawnOwner->GetController();
	if (Controller && Controller->IsLocalController())
	{
		ApplyControlInputToVelocity(DeltaTime);
		LimitWorldBounds();
		bPositionCorrected = false;

		// Move actor
		FVector Delta = Velocity * DeltaTime;

		if (!Delta.IsNearlyZero(1e-6f))
		{
			const FVector OldLocation = UpdatedComponent->GetComponentLocation();
			const FRotator Rotation = UpdatedComponent->GetComponentRotation();
			FVector TraceStart = OldLocation;

			FHitResult Hit(1.f);
			SafeMoveUpdatedComponent(Delta, Rotation, true, Hit);

			if (Hit.IsValidBlockingHit())
			{
				HandleImpact(Hit);
				// Try to slide the remaining distance along the surface.
				SlideAlongSurface(Delta, 1.f-Hit.Time, Hit.Normal, Hit, true);
			}

			// Update velocity
			// We don't want position changes to vastly reverse our direction (which can happen due to penetration fixups etc)
			if (!bPositionCorrected)
			{
				const FVector NewLocation = UpdatedComponent->GetComponentLocation();
				Velocity = ((NewLocation - OldLocation) / DeltaTime);
			}
		}
		
		// Finalize
		UpdateComponentVelocity();
	}
};


bool UFloatingPawnMovement::LimitWorldBounds()
{
	AWorldSettings* WorldSettings = PawnOwner ? PawnOwner->GetWorldSettings() : NULL;
	if (!WorldSettings || !WorldSettings->bEnableWorldBoundsChecks || !UpdatedComponent)
	{
		return false;
	}

	const FVector CurrentLocation = UpdatedComponent->GetComponentLocation();
	if ( CurrentLocation.Z < WorldSettings->KillZ )
	{
		Velocity.Z = FMath::Min(GetMaxSpeed(), WorldSettings->KillZ - CurrentLocation.Z + 2.0f);
		return true;
	}

	return false;
}


void UFloatingPawnMovement::ApplyControlInputToVelocity(float DeltaTime)
{
	const FVector ControlAcceleration = GetInputVector().ClampMaxSize(1.f);
	float AnalogInputModifier = 0.f;
	
	if (ControlAcceleration.SizeSquared() > 0.f)
	{
		// Apply change in velocity direction
		AnalogInputModifier = ControlAcceleration.Size();
		if (Velocity.SizeSquared() > 0.f)
		{
			Velocity -= (Velocity - ControlAcceleration * Velocity.Size()) * FMath::Min(DeltaTime * 8.f, 1.f);
		}
	}
	else
	{
		// Dampen velocity magnitude based on deceleration.
		if (Velocity.SizeSquared() > 0.f)
		{
			const float VelSize = FMath::Max(Velocity.Size() - Deceleration * DeltaTime, 0.f);
			Velocity = Velocity.SafeNormal() * VelSize;
		}
	}

	// Apply acceleration and clamp velocity magnitude.
	const float MaxSpeed = GetModifiedMaxSpeed() * AnalogInputModifier;
	const float NewMaxSpeed = (IsExceedingMaxSpeed(MaxSpeed)) ? Velocity.Size() : MaxSpeed;
	Velocity += ControlAcceleration * Acceleration * DeltaTime;
	Velocity = Velocity.ClampMaxSize(NewMaxSpeed);

	ConsumeInputVector();
}


bool UFloatingPawnMovement::ResolvePenetration(const FVector& Adjustment, const FHitResult& Hit, const FRotator& NewRotation)
{
	bPositionCorrected |= Super::ResolvePenetration(Adjustment, Hit, NewRotation);
	return bPositionCorrected;
}
