// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.
#pragma once

#include "OnlineStoreInterface.h"
#include "InAppPurchaseRestoreCallbackProxy.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FInAppPurchaseRestoreResult, EInAppPurchaseState::Type, CompletionStatus, const TArray<FInAppPurchaseRestoreInfo>&, InAppRestorePurchaseInformation);

UCLASS(MinimalAPI)
class UInAppPurchaseRestoreCallbackProxy : public UObject
{
	GENERATED_UCLASS_BODY()

	// Called when there is a successful In-App Purchase transaction
	UPROPERTY(BlueprintAssignable)
	FInAppPurchaseRestoreResult OnSuccess;

	// Called when there is an unsuccessful In-App Purchase transaction
	UPROPERTY(BlueprintAssignable)
	FInAppPurchaseRestoreResult OnFailure;

	// Kicks off a transaction for the provided product identifier
	UFUNCTION(BlueprintCallable, meta = (DisplayName="Restore In-App Purchases"), Category="Online|InAppPurchase")
	static UInAppPurchaseRestoreCallbackProxy* CreateProxyObjectForInAppPurchaseRestore(class APlayerController* PlayerController);

public:

	// Begin UObject interface
	virtual void BeginDestroy() override;
	// End UObject interface

private:

	/** Called by the InAppPurchase system when the transaction has finished */
	void OnInAppPurchaseRestoreComplete_Delayed();
	void OnInAppPurchaseRestoreComplete(EInAppPurchaseState::Type CompletionState);

	/** Unregisters our delegate from the In-App Purchase system */
	void RemoveDelegate();

	/** Triggers the In-App Purchase Restore Transaction for the specifed user */
	void Trigger(class APlayerController* PlayerController);

private:

	/** Delegate called when a InAppPurchase has been successfully restored */
	FOnInAppPurchaseRestoreCompleteDelegate InAppPurchaseRestoreCompleteDelegate;

	/** Handle to the registered InAppPurchaseCompleteDelegate */
	FDelegateHandle InAppPurchaseRestoreCompleteDelegateHandle;

	/** The InAppPurchaseRestore read request */
	FOnlineInAppPurchaseRestoreReadPtr ReadObject;

	/** Did we fail immediately? */
	bool bFailedToEvenSubmit;

	/** Pointer to the world, needed to delay the results slightly */
	TWeakObjectPtr<UWorld> WorldPtr;

	/** Did the purchase succeed? */
	EInAppPurchaseState::Type SavedPurchaseState;
	TArray<FInAppPurchaseRestoreInfo> SavedProductInformation;

	FTimerHandle OnInAppPurchaseRestoreComplete_DelayedTimerHandle;
};
