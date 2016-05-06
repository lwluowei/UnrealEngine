// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	ScriptCore.cpp: Kismet VM execution and support code.
=============================================================================*/

#include "CoreUObjectPrivate.h"
#include "MallocProfiler.h"
#include "HotReloadInterface.h"
#include "UObject/UObjectThreadContext.h"

DEFINE_LOG_CATEGORY(LogScriptFrame);
DEFINE_LOG_CATEGORY_STATIC(LogScriptCore, Log, All);

DECLARE_CYCLE_STAT(TEXT("Blueprint Time"),STAT_BlueprintTime,STATGROUP_Game);

#define LOCTEXT_NAMESPACE "ScriptCore"

#if TOTAL_OVERHEAD_SCRIPT_STATS
COREUOBJECT_API FBlueprintEventTimer::FScopedVMTimer* FBlueprintEventTimer::ActiveVMTimer = nullptr;
COREUOBJECT_API FBlueprintEventTimer::FPausableScopeTimer* FBlueprintEventTimer::ActiveTimer = nullptr;

DEFINE_STAT(STAT_ScriptVmTime_Total);
DEFINE_STAT(STAT_ScriptNativeTime_Total);
#endif //TOTAL_OVERHEAD_SCRIPT_STATS

/*-----------------------------------------------------------------------------
	Globals.
-----------------------------------------------------------------------------*/

//
// Native function table.
//
COREUOBJECT_API Native GNatives[EX_Max];
COREUOBJECT_API int32 GNativeDuplicate=0;

COREUOBJECT_API void (UObject::*GCasts[CST_Max])( FFrame &Stack, RESULT_DECL );
COREUOBJECT_API int32 GCastDuplicate=0;

COREUOBJECT_API int32 GMaximumScriptLoopIterations = 1000000;

#if !PLATFORM_DESKTOP
	#define RECURSE_LIMIT 120
#else
	#define RECURSE_LIMIT 250
#endif

#if DO_BLUEPRINT_GUARD
#define CHECK_RUNAWAY { ++FBlueprintExceptionTracker::Get().Runaway; }
COREUOBJECT_API void GInitRunaway() 
{
	FBlueprintExceptionTracker::Get().ResetRunaway();
}
#else
#define CHECK_RUNAWAY
COREUOBJECT_API void GInitRunaway() {}
#endif

#define IMPLEMENT_FUNCTION(cls,func) \
	static FNativeFunctionRegistrar cls##func##Registar(cls::StaticClass(),#func,(Native)&cls::func);

#define IMPLEMENT_CAST_FUNCTION(cls, CastIndex, func) \
	IMPLEMENT_FUNCTION(cls, func); \
	static uint8 cls##func##CastTemp = GRegisterCast( CastIndex, (Native)&cls::func );

#define IMPLEMENT_VM_FUNCTION(BytecodeIndex, func) \
	IMPLEMENT_FUNCTION(UObject, func) \
	static uint8 UObject##func##BytecodeTemp = GRegisterNative( BytecodeIndex, (Native)&UObject::func );

//////////////////////////////////////////////////////////////////////////
// FBlueprintCoreDelegates

FBlueprintCoreDelegates::FOnScriptDebuggingEvent FBlueprintCoreDelegates::OnScriptException;
FBlueprintCoreDelegates::FOnScriptExecutionEnd FBlueprintCoreDelegates::OnScriptExecutionEnd;
FBlueprintCoreDelegates::FOnScriptInstrumentEvent FBlueprintCoreDelegates::OnScriptProfilingEvent;
FBlueprintCoreDelegates::FOnToggleScriptProfiler FBlueprintCoreDelegates::OnToggleScriptProfiler;

void FBlueprintCoreDelegates::ThrowScriptException(const UObject* ActiveObject, const FFrame& StackFrame, const FBlueprintExceptionInfo& Info)
{
	bool bShouldLogWarning = true;

	switch (Info.GetType())
	{
	case EBlueprintExceptionType::Breakpoint:
	case EBlueprintExceptionType::Tracepoint:
	case EBlueprintExceptionType::WireTracepoint:
		// These shouldn't warn (they're just to pass the exception into the editor via the delegate below)
		bShouldLogWarning = false;
		break;
#if WITH_EDITOR && DO_BLUEPRINT_GUARD
	case EBlueprintExceptionType::AccessViolation:
		{
			// Determine if the access none should warn or not (we suppress warnings beyond a certain count for each object to avoid per-frame spaminess)
			struct FIntConfigValueHelper
			{
				int32 Value;

				FIntConfigValueHelper() : Value(0)
				{
					GConfig->GetInt(TEXT("ScriptErrorLog"), TEXT("MaxNumOfAccessViolation"), Value, GEditorIni);
				}
			};

			static const FIntConfigValueHelper MaxNumOfAccessViolation;
			if (MaxNumOfAccessViolation.Value > 0)
			{
				const FName ActiveObjectName = ActiveObject ? ActiveObject->GetFName() : FName();
				int32& Num = FBlueprintExceptionTracker::Get().DisplayedWarningsMap.FindOrAdd(ActiveObjectName);
				if (Num > MaxNumOfAccessViolation.Value)
				{
					// Skip the generic warning, we've hit this one too many times
					bShouldLogWarning = false;
				}
				Num++;
			}
		}
		break;
#endif // WITH_EDITOR && DO_BLUEPRINT_GUARD
	default:
		// Other unhandled cases should always emit a warning
		break;
	}

	if (bShouldLogWarning)
	{
		UE_SUPPRESS(LogScript, Warning, const_cast<FFrame*>(&StackFrame)->Logf(TEXT("%s"), *(Info.GetDescription().ToString())));
	}

	// cant fire arbitrary delegates here off the game thread
	if (IsInGameThread())
	{
		// If nothing is bound, show warnings so something is left in the log.
		if (OnScriptException.IsBound() == false)
		{
			UE_LOG(LogScript, Warning, TEXT("%s"), *StackFrame.GetStackTrace());
		}

		OnScriptException.Broadcast(ActiveObject, StackFrame, Info);
	}

	if (Info.GetType() == EBlueprintExceptionType::FatalError)
	{
		// Crash maybe?
	}
}

void FBlueprintCoreDelegates::InstrumentScriptEvent(const EScriptInstrumentationEvent& Info)
{
	OnScriptProfilingEvent.Broadcast(Info);
}

void FBlueprintCoreDelegates::SetScriptMaximumLoopIterations( const int32 MaximumLoopIterations )
{
	if (ensure(MaximumLoopIterations > 0))
	{
		GMaximumScriptLoopIterations = MaximumLoopIterations;
	}
}

//////////////////////////////////////////////////////////////////////////
// FEditorScriptExecutionGuard
FEditorScriptExecutionGuard::FEditorScriptExecutionGuard()
	: bOldGAllowScriptExecutionInEditor(GAllowActorScriptExecutionInEditor)
{
	GAllowActorScriptExecutionInEditor = true;

	if( GIsEditor && !FApp::IsGame() )
	{
		GInitRunaway();
	}
}

FEditorScriptExecutionGuard::~FEditorScriptExecutionGuard()
{
	GAllowActorScriptExecutionInEditor = bOldGAllowScriptExecutionInEditor;
}

bool IsValidCPPIdentifierChar(TCHAR Char)
{
	return Char == TCHAR('_')
		|| (Char >= TCHAR('a') && Char <= TCHAR('z'))
		|| (Char >= TCHAR('A') && Char <= TCHAR('Z'))
		|| (Char >= TCHAR('0') && Char <= TCHAR('9'));
}

FString ToValidCPPIdentifierChars(TCHAR Char)
{
	FString Ret;
	int32 RawValue = Char;
	int32 Counter = 0;
	while (RawValue != 0)
	{
		int32 Digit = RawValue % 63;
		RawValue = (RawValue - Digit) / 63;

		TCHAR SafeChar;
		if (Digit <= 25)
		{
			SafeChar = TCHAR(TCHAR('a') + (25 - Digit));
		}
		else if (Digit <= 51)
		{
			SafeChar = TCHAR(TCHAR('A') + (51 - Digit));
		}
		else if (Digit <= 61)
		{
			SafeChar = TCHAR(TCHAR('0') + (61 - Digit));
		}
		else
		{
			check(Digit == 62);
			SafeChar = TCHAR('_');
		}

		Ret.AppendChar(SafeChar);
	}
	return Ret;
}

FString UnicodeToCPPIdentifier(const FString& InName, bool bDeprecated, const TCHAR* Prefix)
{
	// FName's can contain unicode characters or collide with other CPP identifiers or keywords. This function 
	// returns a string that will have a prefix which is unlikely to collide with existing identifiers and
	// converts unicode characters in place to valid ascii characters. Strictly speaking a C++ compiler *could*
	// support unicode identifiers in source files, but I am not comfortable relying on this behavior.


	FString Ret = InName;
	// Initialize postfix with a unique identifier. This prevents potential collisions between names that have unicode
	// characters and those that do not. The drawback is that it is not safe to put '__pf' in a blueprint name.
	FString Postfix = TEXT("__pf");
	for (auto& Char : Ret)
	{
		// if the character is not a valid character for a c++ identifier, then we need to encode it using valid characters:
		if (!IsValidCPPIdentifierChar(Char))
		{
			// deterministically map char to a valid ascii character, we have 63 characters available (aA-zZ, 0-9, and _)
			// so the optimal encoding would be base 63:
			Postfix.Append(ToValidCPPIdentifierChars(Char));
			Char = TCHAR('x');
		}
	}

	FString PrefixStr(Prefix);
	//fix for error C2059: syntax error : 'bad suffix on number'
	if (!PrefixStr.Len() && Ret.Len() && FChar::IsDigit(Ret[0]))
	{
		Ret.InsertAt(0, TCHAR('_'));
	}
	Ret = PrefixStr + Ret + Postfix;

	// Workaround for a strange compiler error
	if (InName == TEXT("Replicate to server"))
	{
		Ret = TEXT("MagicNameWorkaround");
	}

	return bDeprecated ? Ret + TEXT("_DEPRECATED") : Ret;
}

/*-----------------------------------------------------------------------------
	FFrame implementation.
-----------------------------------------------------------------------------*/

void FFrame::Step(UObject *Context, RESULT_DECL)
{
	int32 B = *Code++;
	(Context->*GNatives[B])(*this,RESULT_PARAM);
}

void FFrame::StepExplicitProperty(void*const Result, UProperty* Property)
{
	checkSlow(Result != NULL);

	if (Property->PropertyFlags & CPF_OutParm)
	{
		// look through the out parameter infos and find the one that has the address of this property
		FOutParmRec* Out = OutParms;
		checkSlow(Out);
		while (Out->Property != Property)
		{
			Out = Out->NextOutParm;
			checkSlow(Out);
		}
		MostRecentPropertyAddress = Out->PropAddr;
		// no need to copy property value, since the caller is just looking for MostRecentPropertyAddress
	}
	else
	{
		MostRecentPropertyAddress = Property->ContainerPtrToValuePtr<uint8>(Locals);
		Property->CopyCompleteValueToScriptVM(Result, MostRecentPropertyAddress);
	}
}

//
// Helper function that checks commandline and Engine ini to see whether
// script stack should be shown on warnings
static bool ShowKismetScriptStackOnWarnings()
{
	static bool ShowScriptStackForScriptWarning = false;
	static bool CheckScriptWarningOptions = false;

	if (!CheckScriptWarningOptions)
	{
		GConfig->GetBool(TEXT("Kismet"), TEXT("ScriptStackOnWarnings"), ShowScriptStackForScriptWarning, GEngineIni);

		if (FParse::Param(FCommandLine::Get(), TEXT("SCRIPTSTACKONWARNINGS")))
		{
			ShowScriptStackForScriptWarning = true;
		}

		CheckScriptWarningOptions = true;
	}

	return ShowScriptStackForScriptWarning;
}

FString FFrame::GetScriptCallstack()
{
	FString ScriptStack;

#if DO_BLUEPRINT_GUARD
	FBlueprintExceptionTracker& BlueprintExceptionTracker = FBlueprintExceptionTracker::Get();
	if (BlueprintExceptionTracker.ScriptStack.Num() > 0)
	{
		for (int32 i = BlueprintExceptionTracker.ScriptStack.Num() - 1; i >= 0; --i)
		{
			ScriptStack += TEXT("\t") + BlueprintExceptionTracker.ScriptStack[i].GetStackDescription() + TEXT("\n");
		}
	}
#else
	ScriptStack = TEXT("Unable to display Script Callstack. Compile with DO_BLUEPRINT_GUARD=1");
#endif

	return ScriptStack;
}

//
// Error or warning handler.
//
//@TODO: This function should take more information in, or be able to gather it from the callstack!
void FFrame::KismetExecutionMessage(const TCHAR* Message, ELogVerbosity::Type Verbosity, FName WarningId)
{
#if !UE_BUILD_SHIPPING
	// Optionally always treat errors/warnings as bad
	if (Verbosity <= ELogVerbosity::Warning && FParse::Param(FCommandLine::Get(), TEXT("FATALSCRIPTWARNINGS")))
	{
		Verbosity = ELogVerbosity::Fatal;
	}
	else if(Verbosity == ELogVerbosity::Warning && WarningId != FName())
	{
		// check to see if this specific warning has been elevated to an error:
		if( FBlueprintSupport::ShouldTreatWarningAsError(WarningId) )
		{
			Verbosity = ELogVerbosity::Error;
		}
		else if(FBlueprintSupport::ShouldSuppressWarning(WarningId))
		{
			return;
		}
	}
#endif

	FString ScriptStack;

	// Show the stackfor fatal/error, and on warning if that option is enabled
	if (Verbosity <= ELogVerbosity::Error || (ShowKismetScriptStackOnWarnings() && Verbosity == ELogVerbosity::Warning))
	{
		ScriptStack = TEXT("Script call stack:\n");
		ScriptStack += GetScriptCallstack();
	}

	if (Verbosity == ELogVerbosity::Fatal)
	{
		UE_LOG(LogScriptCore, Fatal, TEXT("%s\n%s"), Message, *ScriptStack);
	}
#if !NO_LOGGING
	else if (!LogScriptCore.IsSuppressed(Verbosity))
	{
		// Call directly so we can pass verbosity through
		FMsg::Logf_Internal(__FILE__, __LINE__, LogScriptCore.GetCategoryName(), Verbosity, TEXT("%s\n%s"), Message, *ScriptStack);
	}	
#endif
}

void FFrame::Serialize( const TCHAR* V, ELogVerbosity::Type Verbosity, const class FName& Category )
{
	// Treat errors/warnings as bad
	if (Verbosity == ELogVerbosity::Warning)
	{
#if !UE_BUILD_SHIPPING
		static bool GTreatScriptWarningsFatal = FParse::Param(FCommandLine::Get(),TEXT("FATALSCRIPTWARNINGS"));
		if (GTreatScriptWarningsFatal)
		{
			Verbosity = ELogVerbosity::Error;
		}
#endif
	}
	if( Verbosity==ELogVerbosity::Error )
	{
		UE_LOG(LogScriptCore, Fatal,
			TEXT("%s\r\n\t%s\r\n\t%s:%04X\r\n\t%s"),
			V,
			*Object->GetFullName(),
			*Node->GetFullName(),
			Code - Node->Script.GetData(),
			*GetStackTrace()
		);
	}
	else
	{
#if DO_BLUEPRINT_GUARD
		UE_LOG(LogScript, Warning,
			TEXT("%s\r\n\t%s\r\n\t%s:%04X%s"),
			V,
			*Object->GetFullName(),
			*Node->GetFullName(),
			Code - Node->Script.GetData(),
			ShowKismetScriptStackOnWarnings() ? *(FString(TEXT("\r\n")) + GetStackTrace()) : TEXT("")
		);
#endif
	}
}

FString FFrame::GetStackTrace() const
{
	FString Result;

	// travel down the stack recording the frames
	TArray<const FFrame*> FrameStack;
	const FFrame* CurrFrame = this;
	while (CurrFrame != NULL)
	{
		FrameStack.Add(CurrFrame);
		CurrFrame = CurrFrame->PreviousFrame;
	}
	
	// and then dump them to a string
	Result += FString( TEXT("Script call stack:\n") );
	for (int32 Index = FrameStack.Num() - 1; Index >= 0; Index--)
	{
		Result += FString::Printf(TEXT("\t%s\n"), *FrameStack[Index]->Node->GetFullName());
	}

	return Result;
}


/*-----------------------------------------------------------------------------
	Global script execution functions.
-----------------------------------------------------------------------------*/

/*-----------------------------------------------------------------------------
	Native registry.
-----------------------------------------------------------------------------*/

//
// Register a native function.
// Warning: Called at startup time, before engine initialization.
//
COREUOBJECT_API uint8 GRegisterNative( int32 NativeBytecodeIndex, const Native& Func )
{
	static bool bInitialized = false;
	if (!bInitialized)
	{
		bInitialized = true;
		for (uint32 i = 0; i < ARRAY_COUNT(GNatives); i++)
		{
			GNatives[i] = &UObject::execUndefined;
		}
	}

	if( NativeBytecodeIndex != INDEX_NONE )
	{
		if( NativeBytecodeIndex<0 || (uint32)NativeBytecodeIndex>ARRAY_COUNT(GNatives) || GNatives[NativeBytecodeIndex]!=&UObject::execUndefined) 
		{
#if WITH_HOT_RELOAD
			if (GIsHotReload)
			{
				IHotReloadInterface& HotReloadSupport = FModuleManager::LoadModuleChecked<IHotReloadInterface>("HotReload");
				CA_SUPPRESS(6385)				
				HotReloadSupport.AddHotReloadFunctionRemap(Func, GNatives[NativeBytecodeIndex]);
			}
			else
#endif
			{
			GNativeDuplicate = NativeBytecodeIndex;
			}
		}
		CA_SUPPRESS(6386)
		GNatives[NativeBytecodeIndex] = Func;
	}

	return 0;
}

COREUOBJECT_API uint8 GRegisterCast( int32 CastCode, const Native& Func )
{
	static int32 bInitialized = false;
	if (!bInitialized)
	{
		bInitialized = true;
		for (uint32 i = 0; i < ARRAY_COUNT(GCasts); i++)
		{
			GCasts[i] = &UObject::execUndefined;
		}
	}

	//@TODO: UCREMOVAL: Remove rest of cast machinery
	check((CastCode == CST_ObjectToBool) || (CastCode == CST_ObjectToInterface) || (CastCode == CST_InterfaceToBool));

	if (CastCode != INDEX_NONE)
	{
		if(  
#if WITH_HOT_RELOAD
			!GIsHotReload && 
#endif
			(CastCode<0 || (uint32)CastCode>ARRAY_COUNT(GCasts) || GCasts[CastCode]!=&UObject::execUndefined) ) 
		{
			GCastDuplicate = CastCode;
		}
		GCasts[CastCode] = Func;
	}
	return 0;
}

void UObject::SkipFunction(FFrame& Stack, RESULT_DECL, UFunction* Function)
{
	// allocate temporary memory on the stack for evaluating parameters
	uint8* Frame = (uint8*)FMemory_Alloca(Function->PropertiesSize);
	FMemory::Memzero(Frame, Function->PropertiesSize);
	for (UProperty* Property = (UProperty*)Function->Children; *Stack.Code != EX_EndFunctionParms; Property = (UProperty*)Property->Next)
	{
		Stack.MostRecentPropertyAddress = NULL;
		// evaluate the expression into our temporary memory space
		// it'd be nice to be able to skip the copy, but most native functions assume a non-NULL Result pointer
		// so we can only do that if we know the expression is an l-value (out parameter)
		Stack.Step(Stack.Object, (Property->PropertyFlags & CPF_OutParm) ? NULL : Property->ContainerPtrToValuePtr<uint8>(Frame));
	}

	// advance the code past EX_EndFunctionParms
	Stack.Code++;

	// destruct properties requiring it for which we had to use our temporary memory 
	// @warning: conditions for skipping DestroyValue() here must match conditions for passing NULL to Stack.Step() above
	for (UProperty* Destruct = Function->DestructorLink; Destruct; Destruct = Destruct->DestructorLinkNext)
	{
		if (!Destruct->HasAnyPropertyFlags(CPF_OutParm))
		{
			Destruct->DestroyValue_InContainer(Frame);
		}
	}

	UProperty* ReturnProp = Function->GetReturnProperty();
	if (ReturnProp != NULL)
	{
		// destroy old value if necessary
		ReturnProp->DestroyValue(RESULT_PARAM);
		// copy zero value for return property into Result
		FMemory::Memzero(RESULT_PARAM, ReturnProp->ArrayDim * ReturnProp->ElementSize);
	}
}

#ifdef _MSC_VER
#pragma warning (push)
#pragma warning (disable : 4750) // warning C4750: function with _alloca() inlined into a loop
#endif

void UObject::execCallMathFunction(FFrame& Stack, RESULT_DECL)
{
	UFunction* Function = (UFunction*)Stack.ReadObject();
	checkSlow(Function);
	checkSlow(Function->FunctionFlags & FUNC_Native);
	UObject* NewContext = Function->GetOuterUClass()->GetDefaultObject(false);
	checkSlow(NewContext);
	{
		FScopeCycleCounterUObject ContextScope(Stack.Object);
		FScopeCycleCounterUObject FunctionScope(Function);

		// CurrentNativeFunction is used so far only by FLuaContext::InvokeScriptFunction
		// TGuardValue<UFunction*> NativeFuncGuard(Stack.CurrentNativeFunction, Function);
		
		Native Func = Function->GetNativeFunc();
		checkSlow(Func);
		(NewContext->*Func)(Stack, RESULT_PARAM);
	}
}
IMPLEMENT_VM_FUNCTION(EX_CallMath, execCallMathFunction);

void UObject::CallFunction( FFrame& Stack, RESULT_DECL, UFunction* Function )
{
#if PER_FUNCTION_SCRIPT_STATS
	const bool bShouldTrackFunction = FThreadStats::IsCollectingData();
	FScopeCycleCounterUObject FunctionScope(bShouldTrackFunction ? Function : nullptr);
#endif // PER_FUNCTION_SCRIPT_STATS

#if STATS
	const bool bShouldTrackObject = FThreadStats::IsCollectingData();
	FScopeCycleCounterUObject ContextScope(bShouldTrackObject ? this : nullptr);
#endif

	checkSlow(Function);

	if (Function->FunctionFlags & FUNC_Native)
	{
		uint8* Buffer = (uint8*)FMemory_Alloca(Function->ParmsSize);
		int32 FunctionCallspace = GetFunctionCallspace( Function, Buffer, &Stack );
		uint8* SavedCode = NULL;
		if (FunctionCallspace & FunctionCallspace::Remote)
		{
			// Call native networkable function.

			SavedCode = Stack.Code; // Since this is native, we need to rollback the stack if we are calling both remotely and locally

			FMemory::Memzero( Buffer, Function->ParmsSize );

			// Form the RPC parameters.
			for( TFieldIterator<UProperty> It(Function); It && (It->PropertyFlags & (CPF_Parm|CPF_ReturnParm))==CPF_Parm; ++It )
			{
				uint8* CurrentPropAddr = It->ContainerPtrToValuePtr<uint8>(Buffer);
				if ( Cast<UBoolProperty>(*It) && It->ArrayDim == 1 )
				{
					// we're going to get '1' returned for bools that are set, so we need to manually mask it in to the proper place
					bool bValue = false;
					Stack.Step(Stack.Object, &bValue);
					if (bValue)
					{
						((UBoolProperty*)*It)->SetPropertyValue( CurrentPropAddr, true );
					}
				}
				else
				{
					Stack.Step(Stack.Object, CurrentPropAddr);
				}
			}
			checkSlow(*Stack.Code==EX_EndFunctionParms);

			CallRemoteFunction(Function, Buffer, Stack.OutParms, &Stack);
		}

		if (FunctionCallspace & FunctionCallspace::Local)
		{
			if (SavedCode)
			{
				Stack.Code = SavedCode;
			}

			// Call regular native function.
			FScopeCycleCounterUObject NativeContextScope(Stack.Object);
			FScopeCycleCounterUObject NativeFunctionScope(Function);

			Function->Invoke(this, Stack, RESULT_PARAM);
		}
		else
		{
			// Eat up the remaining parameters in the stream.
			SkipFunction(Stack, RESULT_PARAM, Function);
		}
	}
	else
	{
		uint8* Frame = NULL;
#if USE_UBER_GRAPH_PERSISTENT_FRAME
		Frame = GetClass()->GetPersistentUberGraphFrame(this, Function);
#endif
		const bool bUsePersistentFrame = (NULL != Frame);
		if (!bUsePersistentFrame)
		{
			Frame = (uint8*)FMemory_Alloca(Function->PropertiesSize);
			FMemory::Memzero(Frame, Function->PropertiesSize);
		}
		FFrame NewStack( this, Function, Frame, &Stack, Function->Children );
		FOutParmRec** LastOut = &NewStack.OutParms;
		UProperty* Property;

  		// Check to see if we need to handle a return value for this function.  We need to handle this first, because order of return parameters isn't always first.
 		if( Function->HasAnyFunctionFlags(FUNC_HasOutParms) )
 		{
 			// Iterate over the function parameters, searching for the ReturnValue
 			for( TFieldIterator<UProperty> ParmIt(Function); ParmIt; ++ParmIt )
 			{
 				Property = *ParmIt;
 				if( Property->HasAnyPropertyFlags(CPF_ReturnParm) )
 				{
					CA_SUPPRESS(6263)
 					FOutParmRec* RetVal = (FOutParmRec*)FMemory_Alloca(sizeof(FOutParmRec));
 
 					// Our context should be that we're in a variable assignment to the return value, so ensure that we have a valid property to return to
 					check(RESULT_PARAM != NULL);
 					RetVal->PropAddr = (uint8*)RESULT_PARAM;
 					RetVal->Property = Property;
					NewStack.OutParms = RetVal;
 
 					// A function can only have one return value, so we can stop searching
 					break;
 				}
 			}
 		}
		
		for (Property = (UProperty*)Function->Children; *Stack.Code != EX_EndFunctionParms; Property = (UProperty*)Property->Next)
		{
			checkfSlow(Property, TEXT("NULL Property in Function %s"), *Function->GetPathName()); 

			Stack.MostRecentPropertyAddress = NULL;
			
			// Skip the return parameter case, as we've already handled it above
			const bool bIsReturnParam = ((Property->PropertyFlags & CPF_ReturnParm) != 0);
			if( bIsReturnParam )
			{
				continue;
			}

			if (Property->PropertyFlags & CPF_OutParm)
			{
				// evaluate the expression for this parameter, which sets Stack.MostRecentPropertyAddress to the address of the property accessed
				Stack.Step(Stack.Object, NULL);

				CA_SUPPRESS(6263)
				FOutParmRec* Out = (FOutParmRec*)FMemory_Alloca(sizeof(FOutParmRec));
				// set the address and property in the out param info
				// warning: Stack.MostRecentPropertyAddress could be NULL for optional out parameters
				// if that's the case, we use the extra memory allocated for the out param in the function's locals
				// so there's always a valid address
				ensure(Stack.MostRecentPropertyAddress); // possible problem - output param values on local stack are neither initialized nor cleaned.
				Out->PropAddr = (Stack.MostRecentPropertyAddress != NULL) ? Stack.MostRecentPropertyAddress : Property->ContainerPtrToValuePtr<uint8>(NewStack.Locals);
				Out->Property = Property;

				// add the new out param info to the stack frame's linked list
				if (*LastOut)
				{
					(*LastOut)->NextOutParm = Out;
					LastOut = &(*LastOut)->NextOutParm;
				}
				else
				{
					*LastOut = Out;
				}
			}
			else
			{
				// copy the result of the expression for this parameter into the appropriate part of the local variable space
				uint8* Param = Property->ContainerPtrToValuePtr<uint8>(NewStack.Locals);
				checkSlow(Param);

				Property->InitializeValue_InContainer(NewStack.Locals);

				Stack.Step(Stack.Object, Param);
			}
		}
		Stack.Code++;
#if UE_BUILD_DEBUG
		// set the next pointer of the last item to NULL so we'll properly assert if something goes wrong
		if (*LastOut)
		{
			(*LastOut)->NextOutParm = NULL;
		}
#endif

		if (!bUsePersistentFrame)
		{
			// Initialize any local struct properties with defaults
			for (UProperty* LocalProp = Function->FirstPropertyToInit; LocalProp != NULL; LocalProp = (UProperty*)LocalProp->Next)
			{
				LocalProp->InitializeValue_InContainer(NewStack.Locals);
			}
		}

		const bool bIsValidFunction = (Function->FunctionFlags & FUNC_Native) || (Function->Script.Num() > 0);

		// Execute the code.
		if( bIsValidFunction )
		{
			ProcessInternal( NewStack, RESULT_PARAM );
		}

		if (!bUsePersistentFrame)
		{
			// destruct properties on the stack, except for out params since we know we didn't use that memory
			for (UProperty* Destruct = Function->DestructorLink; Destruct; Destruct = Destruct->DestructorLinkNext)
			{
				if (!Destruct->HasAnyPropertyFlags(CPF_OutParm))
				{
					Destruct->DestroyValue_InContainer(NewStack.Locals);
				}
			}
		}
	}
}

/** Helper function to zero the return value in case of a fatal (runaway / infinite recursion) error */
void ClearReturnValue(UProperty* ReturnProp, RESULT_DECL)
{
	if (ReturnProp != NULL)
	{
		// destroy old value if necessary
		if (!ReturnProp->HasAllPropertyFlags(CPF_NoDestructor))
		{
			ReturnProp->DestroyValue(RESULT_PARAM);
		}
		// copy zero value for return property into Result
		FMemory::Memzero(RESULT_PARAM, ReturnProp->ArrayDim * ReturnProp->ElementSize);
	}
}

void UObject::ProcessInternal( FFrame& Stack, RESULT_DECL )
{
	// remove later when stable
	if (GetClass()->HasAnyClassFlags(CLASS_NewerVersionExists))
	{
		if (!GIsReinstancing)
		{
			static int32 num = 0;
			num++;
			if (num < 5)
			{
				ensureMsgf(!GetClass()->HasAnyClassFlags(CLASS_NewerVersionExists), TEXT("Object '%s' is being used for execution, but its class is out of date and has been replaced with a recompiled class!"), *GetFullName());
			}
		}
		return;
	}

	UFunction* Function = (UFunction*)Stack.Node;

#if PER_FUNCTION_SCRIPT_STATS
	const bool bShouldTrackFunction = FThreadStats::IsCollectingData();
	FScopeCycleCounterUObject FunctionScope(bShouldTrackFunction ? Function : nullptr);
#endif // PER_FUNCTION_SCRIPT_STATS

#if STATS
	const bool bShouldTrackObject = FThreadStats::IsCollectingData();
	FScopeCycleCounterUObject ContextScope(bShouldTrackObject ? this : nullptr);
#endif

	int32 FunctionCallspace = GetFunctionCallspace(Function, Stack.Locals, NULL);
	if (FunctionCallspace & FunctionCallspace::Remote)
	{
		CallRemoteFunction(Function, Stack.Locals, Stack.OutParms, NULL);
	}

	if (FunctionCallspace & FunctionCallspace::Local)
	{
		// No POD struct can ever be stored in this buffer. 
		MS_ALIGN(16) uint8 Buffer[MAX_SIMPLE_RETURN_VALUE_SIZE] GCC_ALIGN(16);

#if DO_BLUEPRINT_GUARD
		if(FBlueprintExceptionTracker::Get().bRanaway)
		{
			// If we have a return property, return a zeroed value in it, to try and save execution as much as possible
			UProperty* ReturnProp = (Function)->GetReturnProperty();
			ClearReturnValue(ReturnProp, RESULT_PARAM);
			return;
		}
		else if (++FBlueprintExceptionTracker::Get().Recurse == RECURSE_LIMIT)
		{
			// If we have a return property, return a zeroed value in it, to try and save execution as much as possible
			UProperty* ReturnProp = (Function)->GetReturnProperty();
			ClearReturnValue(ReturnProp, RESULT_PARAM);

			// Notify anyone who cares that we've had a fatal error, so we can shut down PIE, etc
			FBlueprintExceptionInfo InfiniteRecursionExceptionInfo(
				EBlueprintExceptionType::InfiniteLoop, 
				FText::Format(
					LOCTEXT("InfiniteLoop", "Infinite script recursion ({0} calls) detected - see log for stack trace"), 
					FText::AsNumber(RECURSE_LIMIT)
				)
			);
			FBlueprintCoreDelegates::ThrowScriptException(this, Stack, InfiniteRecursionExceptionInfo);

			// This flag prevents repeated warnings of infinite loop, script exception handler 
			// is expected to have terminated execution appropriately:
			FBlueprintExceptionTracker::Get().bRanaway = true;

			return;
		}
#endif
		// Execute the bytecode
		while (*Stack.Code != EX_Return)
		{
#if DO_BLUEPRINT_GUARD
			if( FBlueprintExceptionTracker::Get().Runaway > GMaximumScriptLoopIterations )
			{
				// If we have a return property, return a zeroed value in it, to try and save execution as much as possible
				UProperty* ReturnProp = (Function)->GetReturnProperty();
				ClearReturnValue(ReturnProp, RESULT_PARAM);

				// Notify anyone who cares that we've had a fatal error, so we can shut down PIE, etc
				FBlueprintExceptionInfo RunawayLoopExceptionInfo(
					EBlueprintExceptionType::InfiniteLoop, 
					FText::Format(
						LOCTEXT("RunawayLoop", "Runaway loop detected (over {0} iterations) - see log for stack trace"),
						FText::AsNumber(GMaximumScriptLoopIterations)
					)
				);

				// Need to reset Runaway counter BEFORE throwing script exception, because the exception causes a modal dialog,
				// and other scripts running will then erroneously think they are also "runaway".
				FBlueprintExceptionTracker::Get().Runaway = 0;

				FBlueprintCoreDelegates::ThrowScriptException(this, Stack, RunawayLoopExceptionInfo);
				return;
			}
#endif

			Stack.Step(Stack.Object, Buffer);
		}

		// Step over the return statement and evaluate the result expression
		Stack.Code++;

		if (*Stack.Code != EX_Nothing)
		{
			Stack.Step(Stack.Object, RESULT_PARAM);
		}
		else
		{
			Stack.Code++;
		}

#if DO_BLUEPRINT_GUARD
		--FBlueprintExceptionTracker::Get().Recurse;
#endif
	}
	else
	{
		UProperty* ReturnProp = (Function)->GetReturnProperty();
		if (ReturnProp != NULL)
		{
			// destroy old value if necessary
			ReturnProp->DestroyValue(RESULT_PARAM);
			// copy zero value for return property into Result
			FMemory::Memzero(RESULT_PARAM, ReturnProp->ArrayDim * ReturnProp->ElementSize);
		}
	}
}

bool UObject::CallFunctionByNameWithArguments(const TCHAR* Str, FOutputDevice& Ar, UObject* Executor, bool bForceCallWithNonExec/*=false*/)
{
	// Find an exec function.
	FString MsgStr;
	if(!FParse::Token(Str,MsgStr,true))
	{
		UE_LOG(LogScriptCore, Verbose, TEXT("CallFunctionByNameWithArguments: Not Parsed '%s'"), Str);
		return false;
	}
	const FName Message = FName(*MsgStr,FNAME_Find);
	if(Message == NAME_None)
	{
		UE_LOG(LogScriptCore, Verbose, TEXT("CallFunctionByNameWithArguments: Name not found '%s'"), Str);
		return false;
	}
	UFunction* Function = FindFunction(Message);
	if(NULL == Function)
	{
		UE_LOG(LogScriptCore, Verbose, TEXT("CallFunctionByNameWithArguments: Function not found '%s'"), Str);
		return false;
	}
	if(0 == (Function->FunctionFlags & FUNC_Exec) && !bForceCallWithNonExec)
	{
		UE_LOG(LogScriptCore, Verbose, TEXT("CallFunctionByNameWithArguments: Function not executable '%s'"), Str);
		return false;
	}

	UProperty* LastParameter=NULL;

	// find the last parameter
	for ( TFieldIterator<UProperty> It(Function); It && (It->PropertyFlags&(CPF_Parm|CPF_ReturnParm)) == CPF_Parm; ++It )
	{
		LastParameter = *It;
	}

	UStrProperty* LastStringParameter = dynamic_cast<UStrProperty*>(LastParameter);


	// Parse all function parameters.
	uint8* Parms = (uint8*)FMemory_Alloca(Function->ParmsSize);
	FMemory::Memzero( Parms, Function->ParmsSize );

	bool Failed = 0;
	int32 NumParamsEvaluated = 0;
	for( TFieldIterator<UProperty> It(Function); It && (It->PropertyFlags & (CPF_Parm|CPF_ReturnParm))==CPF_Parm; ++It, NumParamsEvaluated++ )
	{
		UProperty* propertyParam = *It;
		if (NumParamsEvaluated == 0 && Executor)
		{
			UObjectPropertyBase* Op = dynamic_cast<UObjectPropertyBase*>(*It);
			if( Op && Executor->IsA(Op->PropertyClass) )
			{
				// First parameter is implicit reference to object executing the command.
				Op->SetObjectPropertyValue(Op->ContainerPtrToValuePtr<uint8>(Parms), Executor);
				continue;
			}
		}

		FParse::Next( &Str );

		// if Str is empty but we have more params to read parse the function to see if these have defaults, if so set them
		bool bFoundDefault = false;
		bool bFailedImport = true;
		if (!FCString::Strcmp(Str, TEXT("")))
		{
			const FName DefaultPropertyKey(*(FString(TEXT("CPP_Default_")) + propertyParam->GetName()));
#if WITH_EDITOR
			const FString PropertyDefaultValue = Function->GetMetaData(DefaultPropertyKey);
#else
			const FString PropertyDefaultValue = TEXT("");
#endif
			if (!PropertyDefaultValue.IsEmpty()) 
			{
				bFoundDefault = true;
				uint32 ExportFlags = PPF_Localized;

				// if this is the last parameter of the exec function and it's a string, make sure that it accepts the remainder of the passed in value
				if ( LastStringParameter != *It )
				{
					ExportFlags |= PPF_Delimited;
				}
				const TCHAR* Result = It->ImportText( *PropertyDefaultValue, It->ContainerPtrToValuePtr<uint8>(Parms), ExportFlags, NULL );
				bFailedImport = Result == NULL;
			}
		}

		if (!bFoundDefault)
		{
			uint32 ExportFlags = PPF_Localized;

			// if this is the last parameter of the exec function and it's a string, make sure that it accepts the remainder of the passed in value
			if ( LastStringParameter != *It )
			{
				ExportFlags |= PPF_Delimited;
			}
			const TCHAR* PreviousStr = Str;
			const TCHAR* Result = It->ImportText( Str, It->ContainerPtrToValuePtr<uint8>(Parms), ExportFlags, NULL );
			bFailedImport = (Result == NULL || Result == PreviousStr);
			
			// move to the next parameter
			Str = Result;
		}
		
		if( bFailedImport )
		{
			FFormatNamedArguments Arguments;
			Arguments.Add(TEXT("Message"), FText::FromName( Message ));
			Arguments.Add(TEXT("PropertyName"), FText::FromString( It->GetName() ));
			Ar.Logf( *FText::Format( NSLOCTEXT( "Core", "BadProperty", "'{Message}': Bad or missing property '{PropertyName}'" ), Arguments ).ToString() );
			Failed = true;

			break;
		}

	}

	if( !Failed )
	{
		ProcessEvent( Function, Parms );
	}

	//!!destructframe see also UObject::ProcessEvent
	for( TFieldIterator<UProperty> It(Function); It && (It->PropertyFlags & (CPF_Parm|CPF_ReturnParm))==CPF_Parm; ++It )
	{
		It->DestroyValue_InContainer(Parms);
	}

	// Success.
	return true;
}

UFunction* UObject::FindFunction( FName InName ) const
{
	return GetClass()->FindFunctionByName(InName);
}

UFunction* UObject::FindFunctionChecked( FName InName ) const
{
	UFunction* Result = FindFunction(InName);
	if (Result == NULL)
	{
		UE_LOG(LogScriptCore, Fatal, TEXT("Failed to find function %s in %s"), *InName.ToString(), *GetFullName());
	}
	return Result;
}

void UObject::ProcessEvent( UFunction* Function, void* Parms )
{
	checkf(!IsUnreachable(),TEXT("%s  Function: '%s'"), *GetFullName(), *Function->GetPathName());
	checkf(!FUObjectThreadContext::Get().IsRoutingPostLoad, TEXT("Cannot call UnrealScript (%s - %s) while PostLoading objects"), *GetFullName(), *Function->GetFullName());

	// Reject.
	if (IsPendingKill())
	{
		return;
	}
	
#if WITH_EDITORONLY_DATA
	// Cannot invoke script events when the game thread is paused for debugging.
	if(GIntraFrameDebuggingGameThread)
	{
		if(GFirstFrameIntraFrameDebugging)
		{
			UE_LOG(LogScriptCore, Warning, TEXT("Cannot call UnrealScript (%s - %s) while stopped at a breakpoint."), *GetFullName(), *Function->GetFullName());
		}

		return;
	}
#endif	// WITH_EDITORONLY_DATA

	if ((Function->FunctionFlags & FUNC_Native) != 0)
	{
		int32 FunctionCallspace = GetFunctionCallspace(Function, Parms, NULL);
		if (FunctionCallspace & FunctionCallspace::Remote)
		{
			CallRemoteFunction(Function, Parms, NULL, NULL);
		}

		if ((FunctionCallspace & FunctionCallspace::Local) == 0)
		{
			return;
		}
	}
	else if (Function->Script.Num() == 0)
	{
		return;
	}
	checkSlow((Function->ParmsSize == 0) || (Parms != NULL));

#if TOTAL_OVERHEAD_SCRIPT_STATS
	FBlueprintEventTimer::FScopedVMTimer VMTime;
#endif // TOTAL_OVERHEAD_SCRIPT_STATS

#if PER_FUNCTION_SCRIPT_STATS
	const bool bShouldTrackFunction = FThreadStats::IsCollectingData();
	FScopeCycleCounterUObject FunctionScope(bShouldTrackFunction ? Function : nullptr);
#endif // PER_FUNCTION_SCRIPT_STATS

#if STATS
	const bool bShouldTrackObject = FThreadStats::IsCollectingData();
	FScopeCycleCounterUObject ContextScope(bShouldTrackObject ? this : nullptr);
#endif

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	if (GetClass()->HasInstrumentation())
	{
		const EScriptInstrumentation::Type EventType = Function->HasAnyFunctionFlags(FUNC_Event|FUNC_BlueprintEvent) ? EScriptInstrumentation::Event : EScriptInstrumentation::ResumeEvent;
		EScriptInstrumentationEvent EventInstrumentationInfo(EventType, this, Function->GetFName());
		FBlueprintCoreDelegates::InstrumentScriptEvent(EventInstrumentationInfo);
	}
#endif

#if DO_BLUEPRINT_GUARD
	FBlueprintExceptionTracker& BlueprintExceptionTracker = FBlueprintExceptionTracker::Get();
	BlueprintExceptionTracker.ScriptEntryTag++;

	CONDITIONAL_SCOPE_CYCLE_COUNTER(STAT_BlueprintTime, BlueprintExceptionTracker.ScriptEntryTag == 1);
#endif

#if UE_BLUEPRINT_EVENTGRAPH_FASTCALLS
	// Fast path for ubergraph calls
	int32 EventGraphParams;
	if (Function->EventGraphFunction != nullptr)
	{
		// Call directly into the event graph, skipping the stub thunk function
		EventGraphParams = Function->EventGraphCallOffset;
		Parms = &EventGraphParams;
		Function = Function->EventGraphFunction;

		// Validate assumptions required for this optimized path (EventGraphFunction should have only been filled out if these held)
		checkSlow(Function->ParmsSize == sizeof(EventGraphParams));
		checkSlow(Function->FirstPropertyToInit == nullptr);
		checkSlow(Function->PostConstructLink == nullptr);
	}
#endif

	// Scope required for scoped script stats.
	{
		uint8* Frame = NULL;
#if USE_UBER_GRAPH_PERSISTENT_FRAME
		Frame = GetClass()->GetPersistentUberGraphFrame(this, Function);
#endif
		const bool bUsePersistentFrame = (NULL != Frame);
		if (!bUsePersistentFrame)
		{
			Frame = (uint8*)FMemory_Alloca(Function->PropertiesSize);
			// zero the local property memory
			FMemory::Memzero(Frame + Function->ParmsSize, Function->PropertiesSize - Function->ParmsSize);
		}

		// initialize the parameter properties
		FMemory::Memcpy(Frame, Parms, Function->ParmsSize);

		// Create a new local execution stack.
		FFrame NewStack(this, Function, Frame, NULL, Function->Children);

		checkSlow(NewStack.Locals || Function->ParmsSize == 0);



		// if the function has out parameters, fill the stack frame's out parameter info with the info for those params 
		if ( Function->HasAnyFunctionFlags(FUNC_HasOutParms) )
		{
			FOutParmRec** LastOut = &NewStack.OutParms;
			for ( UProperty* Property = (UProperty*)Function->Children; Property && (Property->PropertyFlags&(CPF_Parm)) == CPF_Parm; Property = (UProperty*)Property->Next )
			{
				// this is used for optional parameters - the destination address for out parameter values is the address of the calling function
				// so we'll need to know which address to use if we need to evaluate the default parm value expression located in the new function's
				// bytecode
				if ( Property->HasAnyPropertyFlags(CPF_OutParm) )
				{
					CA_SUPPRESS(6263)
					FOutParmRec* Out = (FOutParmRec*)FMemory_Alloca(sizeof(FOutParmRec));
					// set the address and property in the out param info
					// note that since C++ doesn't support "optional out" we can ignore that here
					Out->PropAddr = Property->ContainerPtrToValuePtr<uint8>(Parms);
					Out->Property = Property;

					// add the new out param info to the stack frame's linked list
					if (*LastOut)
					{
						(*LastOut)->NextOutParm = Out;
						LastOut = &(*LastOut)->NextOutParm;
					}
					else
					{
						*LastOut = Out;
					}
				}
			}

#if UE_BUILD_DEBUG
			// set the next pointer of the last item to NULL so we'll properly assert if something goes wrong
			if (*LastOut)
			{
				(*LastOut)->NextOutParm = NULL;
			}
#endif
		}

		if (!bUsePersistentFrame)
		{
			for (UProperty* LocalProp = Function->FirstPropertyToInit; LocalProp != NULL; LocalProp = (UProperty*)LocalProp->Next)
			{
				LocalProp->InitializeValue_InContainer(NewStack.Locals);
			}
		}

		// Call native function or UObject::ProcessInternal.
		const bool bHasReturnParam = Function->ReturnValueOffset != MAX_uint16;
		uint8* ReturnValueAdress = bHasReturnParam ? ((uint8*)Parms + Function->ReturnValueOffset) : nullptr;
		if (Function->FunctionFlags & FUNC_Native)
		{
			Function->Invoke(this, NewStack, ReturnValueAdress);
		}
		else
		{
			Function->Invoke(this, NewStack, ReturnValueAdress);
		}

		if (!bUsePersistentFrame)
		{
			// Destroy local variables except function parameters.!! see also UObject::CallFunctionByNameWithArguments
			// also copy back constructed value parms here so the correct copy is destroyed when the event function returns
			for (UProperty* P = Function->DestructorLink; P; P = P->DestructorLinkNext)
			{
				if (!P->IsInContainer(Function->ParmsSize))
				{
					P->DestroyValue_InContainer(NewStack.Locals);
				}
				else if (!(P->PropertyFlags & CPF_OutParm))
				{
					FMemory::Memcpy(P->ContainerPtrToValuePtr<uint8>(Parms), P->ContainerPtrToValuePtr<uint8>(NewStack.Locals), P->ArrayDim * P->ElementSize);
				}
			}
		}
	}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	if (GetClass()->HasInstrumentation())
	{
		EScriptInstrumentationEvent EventInstrumentationInfo(EScriptInstrumentation::Stop, this, NAME_None);
		FBlueprintCoreDelegates::InstrumentScriptEvent(EventInstrumentationInfo);
	}
#if WITH_EDITORONLY_DATA
	FBlueprintCoreDelegates::OnScriptExecutionEnd.Broadcast();
#endif
#endif

#if DO_BLUEPRINT_GUARD
	--BlueprintExceptionTracker.ScriptEntryTag;
#endif
}

#ifdef _MSC_VER
#pragma warning (pop)
#endif

void UObject::execUndefined(FFrame& Stack, RESULT_DECL)
{
	Stack.Logf(ELogVerbosity::Error, TEXT("Unknown code token %02X"), Stack.Code[-1] );
}

void UObject::execLocalVariable(FFrame& Stack, RESULT_DECL)
{
	checkSlow(Stack.Object == this);
	checkSlow(Stack.Locals != NULL);

	UProperty* VarProperty = Stack.ReadProperty();
	if (VarProperty == nullptr)
	{
		FBlueprintExceptionInfo ExceptionInfo(EBlueprintExceptionType::AccessViolation, LOCTEXT("MissingLocalVariable", "Attempted to access missing local variable. If this is a packaged/cooked build, are you attempting to use an editor-only property?"));
		FBlueprintCoreDelegates::ThrowScriptException(this, Stack, ExceptionInfo);

		Stack.MostRecentPropertyAddress = nullptr;
	}
	else
	{
		Stack.MostRecentPropertyAddress = VarProperty->ContainerPtrToValuePtr<uint8>(Stack.Locals);

		if (RESULT_PARAM)
		{
			VarProperty->CopyCompleteValueToScriptVM(RESULT_PARAM, Stack.MostRecentPropertyAddress);
		}
	}
}
IMPLEMENT_VM_FUNCTION( EX_LocalVariable, execLocalVariable );

void UObject::execInstanceVariable(FFrame& Stack, RESULT_DECL)
{
	UProperty* VarProperty = (UProperty*)Stack.ReadObject();
	Stack.MostRecentProperty = VarProperty;

	if (VarProperty == nullptr || !IsA((UClass*)VarProperty->GetOuter()))
	{
		FBlueprintExceptionInfo ExceptionInfo(EBlueprintExceptionType::AccessViolation, LOCTEXT("MissingProperty", "Attempted to access missing property. If this is a packaged/cooked build, are you attempting to use an editor-only property?"));
		FBlueprintCoreDelegates::ThrowScriptException(this, Stack, ExceptionInfo);

		Stack.MostRecentPropertyAddress = nullptr;
	}
	else
	{
		Stack.MostRecentPropertyAddress = VarProperty->ContainerPtrToValuePtr<uint8>(this);

		if (RESULT_PARAM)
		{
			VarProperty->CopyCompleteValueToScriptVM(RESULT_PARAM, Stack.MostRecentPropertyAddress);
		}
	}

	
}
IMPLEMENT_VM_FUNCTION( EX_InstanceVariable, execInstanceVariable );

void UObject::execDefaultVariable(FFrame& Stack, RESULT_DECL)
{
	UProperty* VarProperty = (UProperty*)Stack.ReadObject();
	Stack.MostRecentProperty = VarProperty;
	Stack.MostRecentPropertyAddress = nullptr;

	UObject* DefaultObject = nullptr;
	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		DefaultObject = this;
	}
	else
	{
		// @todo - allow access to archetype properties through object references?
	}

	if (VarProperty == nullptr || (DefaultObject && !DefaultObject->IsA((UClass*)VarProperty->GetOuter())))
	{
		FBlueprintExceptionInfo ExceptionInfo(EBlueprintExceptionType::AccessViolation, LOCTEXT("MissingPropertyDefaultObject", "Attempted to access a missing property on a CDO. If this is a packaged/cooked build, are you attempting to use an editor-only property?"));
		FBlueprintCoreDelegates::ThrowScriptException(this, Stack, ExceptionInfo);
	}
	else
	{
		if(DefaultObject != nullptr)
		{
			Stack.MostRecentPropertyAddress = VarProperty->ContainerPtrToValuePtr<uint8>(DefaultObject);
			if(RESULT_PARAM)
			{
				VarProperty->CopyCompleteValueToScriptVM(RESULT_PARAM, Stack.MostRecentPropertyAddress);
			}
		}
		else
		{
			FBlueprintExceptionInfo ExceptionInfo(EBlueprintExceptionType::AccessViolation, LOCTEXT("AccessNoneDefaultObject", "Accessed None attempting to read a default property"));
			FBlueprintCoreDelegates::ThrowScriptException(this, Stack, ExceptionInfo);
		}
	}
}
IMPLEMENT_VM_FUNCTION( EX_DefaultVariable, execDefaultVariable );

void UObject::execLocalOutVariable(FFrame& Stack, RESULT_DECL)
{
	checkSlow(Stack.Object == this);

	// get the property we need to find
	UProperty* VarProperty = Stack.ReadProperty();
	
	// look through the out parameter infos and find the one that has the address of this property
	FOutParmRec* Out = Stack.OutParms;
	checkSlow(Out);
	while (Out->Property != VarProperty)
	{
		Out = Out->NextOutParm;
		checkSlow(Out);
	}
	Stack.MostRecentPropertyAddress = Out->PropAddr;

	// if desired, copy the value in that address to Result
	if (RESULT_PARAM && RESULT_PARAM != Stack.MostRecentPropertyAddress)
	{
		VarProperty->CopyCompleteValueToScriptVM(RESULT_PARAM, Stack.MostRecentPropertyAddress);
	}
}
IMPLEMENT_VM_FUNCTION(EX_LocalOutVariable, execLocalOutVariable);

void UObject::execInterfaceContext(FFrame& Stack, RESULT_DECL)
{
	// get the value of the interface variable
	FScriptInterface InterfaceValue;
	Stack.Step(this, &InterfaceValue);

	if (RESULT_PARAM != NULL)
	{
		// copy the UObject pointer to Result
		*(UObject**)RESULT_PARAM = InterfaceValue.GetObject();
	}
}
IMPLEMENT_VM_FUNCTION( EX_InterfaceContext, execInterfaceContext );

void UObject::execClassContext(FFrame& Stack, RESULT_DECL)
{
	// Get class expression.
	UClass* ClassContext = NULL;
	Stack.Step(this, &ClassContext);

	// Execute expression in class context.
	if(IsValid(ClassContext))
	{
		UObject* DefaultObject = ClassContext->GetDefaultObject();
		check(DefaultObject != NULL);

		Stack.Code += sizeof(CodeSkipSizeType)	// Code offset for NULL expressions.
			+ sizeof(ScriptPointerType);		// Property corresponding to the r-value data, in case the l-value needs to be cleared
		Stack.Step(DefaultObject, RESULT_PARAM);
	}
	else
	{
		if (Stack.MostRecentProperty != NULL)
		{
			FBlueprintExceptionInfo ExceptionInfo(
				EBlueprintExceptionType::AccessViolation, 
				FText::Format(
					LOCTEXT("AccessedNoneClass", "Accessed None trying to read Class from property {0}"), 
					FText::FromString(Stack.MostRecentProperty->GetName())
				)
			);
			FBlueprintCoreDelegates::ThrowScriptException(this, Stack, ExceptionInfo);
		}
		else
		{
			FBlueprintExceptionInfo ExceptionInfo(EBlueprintExceptionType::AccessViolation, LOCTEXT("AccessedNoneClassUnknownProperty", "Accessed None reading a Class"));
			FBlueprintCoreDelegates::ThrowScriptException(this, Stack, ExceptionInfo);
		}

		const CodeSkipSizeType wSkip = Stack.ReadCodeSkipCount(); // Code offset for NULL expressions. Code += sizeof(CodeSkipSizeType)
		UProperty* RValueProperty = nullptr;
		const VariableSizeType bSize = Stack.ReadVariableSize(&RValueProperty); // Code += sizeof(ScriptPointerType) + sizeof(uint8)
		Stack.Code += wSkip;
		Stack.MostRecentPropertyAddress = NULL;
		Stack.MostRecentProperty = NULL;

		if (RESULT_PARAM && RValueProperty)
		{
			RValueProperty->ClearValue(RESULT_PARAM);
		}
	}
}
IMPLEMENT_VM_FUNCTION( EX_ClassContext, execClassContext );

void UObject::execEndOfScript( FFrame& Stack, RESULT_DECL )
{
#if WITH_EDITOR
	if (GIsEditor)
	{
		UE_LOG(LogScriptCore, Warning, TEXT("--- Dumping bytecode for %s on %s ---"), *Stack.Node->GetFullName(), *Stack.Object->GetFullName());
		const UFunction* Func = Stack.Node;
		for(int32 i = 0; i < Func->Script.Num(); ++i)
		{
			UE_LOG(LogScriptCore, Log, TEXT("0x%x"), Func->Script[i]);
		}
	}
#endif //WITH_EDITOR

	UE_LOG(LogScriptCore, Fatal,TEXT("Execution beyond end of script in %s on %s"), *Stack.Node->GetFullName(), *Stack.Object->GetFullName());
}
IMPLEMENT_VM_FUNCTION( EX_EndOfScript, execEndOfScript );

void UObject::execNothing( FFrame& Stack, RESULT_DECL )
{
	// Do nothing.
}
IMPLEMENT_VM_FUNCTION( EX_Nothing, execNothing );

void UObject::execNothingOp4a( FFrame& Stack, RESULT_DECL )
{
	// Do nothing.
}
IMPLEMENT_VM_FUNCTION( EX_DeprecatedOp4A, execNothingOp4a );

void UObject::execBreakpoint( FFrame& Stack, RESULT_DECL )
{
#if WITH_EDITORONLY_DATA
	if (GIsEditor)
	{
		FBlueprintExceptionInfo BreakpointExceptionInfo(EBlueprintExceptionType::Breakpoint);
		FBlueprintCoreDelegates::ThrowScriptException(this, Stack, BreakpointExceptionInfo);
	}
#endif
}
IMPLEMENT_VM_FUNCTION( EX_Breakpoint, execBreakpoint );

void UObject::execTracepoint( FFrame& Stack, RESULT_DECL )
{
#if WITH_EDITORONLY_DATA
	if (GIsEditor)
	{
		FBlueprintExceptionInfo TracepointExceptionInfo(EBlueprintExceptionType::Tracepoint);
		FBlueprintCoreDelegates::ThrowScriptException(this, Stack, TracepointExceptionInfo);
	}
#endif
}
IMPLEMENT_VM_FUNCTION( EX_Tracepoint, execTracepoint );

void UObject::execWireTracepoint( FFrame& Stack, RESULT_DECL )
{
#if WITH_EDITORONLY_DATA
	if (GIsEditor)
	{
		FBlueprintExceptionInfo TracepointExceptionInfo(EBlueprintExceptionType::WireTracepoint);
		FBlueprintCoreDelegates::ThrowScriptException(this, Stack, TracepointExceptionInfo);
	}
#endif
}
IMPLEMENT_VM_FUNCTION( EX_WireTracepoint, execWireTracepoint );

void UObject::execInstrumentation( FFrame& Stack, RESULT_DECL )
{
#if !UE_BUILD_SHIPPING
	const EScriptInstrumentation::Type EventType = static_cast<EScriptInstrumentation::Type>(Stack.PeekCode());
#if WITH_EDITORONLY_DATA
	if (GIsEditor)
	{
		if (EventType == EScriptInstrumentation::NodeEntry)
		{
			FBlueprintExceptionInfo TracepointExceptionInfo(EBlueprintExceptionType::Tracepoint);
			FBlueprintCoreDelegates::ThrowScriptException(this, Stack, TracepointExceptionInfo);
		}
		else if (EventType == EScriptInstrumentation::NodeExit)
		{
			FBlueprintExceptionInfo WiretraceExceptionInfo(EBlueprintExceptionType::WireTracepoint);
			FBlueprintCoreDelegates::ThrowScriptException(this, Stack, WiretraceExceptionInfo);
		}
		else if (EventType == EScriptInstrumentation::NodeDebugSite)
		{
			FBlueprintExceptionInfo TracepointExceptionInfo(EBlueprintExceptionType::Breakpoint);
			FBlueprintCoreDelegates::ThrowScriptException(this, Stack, TracepointExceptionInfo);
		}
	}
#endif
	EScriptInstrumentationEvent InstrumentationEventInfo(EventType, this, Stack);
	FBlueprintCoreDelegates::InstrumentScriptEvent(InstrumentationEventInfo);
	Stack.SkipCode(1);
#endif
}
IMPLEMENT_VM_FUNCTION( EX_InstrumentationEvent, execInstrumentation );

void UObject::execEndFunctionParms( FFrame& Stack, RESULT_DECL )
{
	// For skipping over optional function parms without values specified.
	Stack.Code--;
}
IMPLEMENT_VM_FUNCTION( EX_EndFunctionParms, execEndFunctionParms );


void UObject::execJump( FFrame& Stack, RESULT_DECL )
{
	CHECK_RUNAWAY;

	// Jump immediate.
	CodeSkipSizeType Offset = Stack.ReadCodeSkipCount();
	Stack.Code = &Stack.Node->Script[Offset];
}
IMPLEMENT_VM_FUNCTION( EX_Jump, execJump );

void UObject::execComputedJump( FFrame& Stack, RESULT_DECL )
{
	CHECK_RUNAWAY;

	// Get the jump offset expression
	int32 ComputedOffset = 0;
	Stack.Step( Stack.Object, &ComputedOffset );
	check((ComputedOffset < Stack.Node->Script.Num()) && (ComputedOffset >= 0));

	// Jump to the new offset
	Stack.Code = &Stack.Node->Script[ComputedOffset];
}
IMPLEMENT_VM_FUNCTION( EX_ComputedJump, execComputedJump );
	
void UObject::execJumpIfNot( FFrame& Stack, RESULT_DECL )
{
	CHECK_RUNAWAY;

	// Get code offset.
	CodeSkipSizeType Offset = Stack.ReadCodeSkipCount();

	// Get boolean test value.
	bool Value=0;
	Stack.Step( Stack.Object, &Value );

	// Jump if false.
	if( !Value )
	{
		Stack.Code = &Stack.Node->Script[ Offset ];
	}
}
IMPLEMENT_VM_FUNCTION( EX_JumpIfNot, execJumpIfNot );

void UObject::execAssert( FFrame& Stack, RESULT_DECL )
{
	// Get line number.
	int32 wLine = Stack.ReadWord();

	// find out whether we are in debug mode and therefore should crash on failure
	uint8 bDebug = *(uint8*)Stack.Code++;

	// Get boolean assert value.
	uint32 Value=0;
	Stack.Step( Stack.Object, &Value );

	// Check it.
	if( !Value )
	{
		Stack.Logf(TEXT("%s"), *Stack.GetStackTrace());
		if (bDebug)
		{
			Stack.Logf(ELogVerbosity::Error, TEXT("Assertion failed, line %i"), wLine);
		}
		else
		{
			UE_SUPPRESS(LogScript, Warning, Stack.Logf(TEXT("Assertion failed, line %i"), wLine));
		}
	}
}
IMPLEMENT_VM_FUNCTION( EX_Assert, execAssert );

void UObject::execPushExecutionFlow( FFrame& Stack, RESULT_DECL )
{
	// Read a code offset and push it onto the flow stack
	CodeSkipSizeType Offset = Stack.ReadCodeSkipCount();
	Stack.FlowStack.Push(Offset);
}
IMPLEMENT_VM_FUNCTION( EX_PushExecutionFlow, execPushExecutionFlow );

void UObject::execPopExecutionFlow( FFrame& Stack, RESULT_DECL )
{
	// Since this is a branch function, check for runaway script execution
	CHECK_RUNAWAY;

	// Try to pop an entry off the stack and go there
	if (Stack.FlowStack.Num())
	{
		CodeSkipSizeType Offset = Stack.FlowStack.Pop(/*bAllowShrinking=*/ false);
		Stack.Code = &Stack.Node->Script[ Offset ];
	}
	else
	{
		UE_LOG(LogScriptCore, Log, TEXT("%s"), *Stack.GetStackTrace());
		Stack.Logf(ELogVerbosity::Error, TEXT("Tried to pop from an empty flow stack"));
	}
}
IMPLEMENT_VM_FUNCTION( EX_PopExecutionFlow, execPopExecutionFlow );

void UObject::execPopExecutionFlowIfNot( FFrame& Stack, RESULT_DECL )
{
	// Since this is a branch function, check for runaway script execution
	CHECK_RUNAWAY;

	// Get boolean test value.
	bool Value=0;
	Stack.Step( Stack.Object, &Value );

	if (!Value)
	{
		// Try to pop an entry off the stack and go there
		if (Stack.FlowStack.Num())
		{
			CodeSkipSizeType Offset = Stack.FlowStack.Pop(/*bAllowShrinking=*/ false);
			Stack.Code = &Stack.Node->Script[ Offset ];
		}
		else
		{
			UE_LOG(LogScriptCore, Log, TEXT("%s"), *Stack.GetStackTrace());
			Stack.Logf(ELogVerbosity::Error, TEXT("Tried to pop from an empty flow stack"));
		}
	}
}
IMPLEMENT_VM_FUNCTION( EX_PopExecutionFlowIfNot, execPopExecutionFlowIfNot );

void UObject::execLetValueOnPersistentFrame(FFrame& Stack, RESULT_DECL)
{
#if USE_UBER_GRAPH_PERSISTENT_FRAME
	Stack.MostRecentProperty = NULL;
	Stack.MostRecentPropertyAddress = NULL;

	auto DestProperty = Stack.ReadProperty();
	checkSlow(DestProperty);
	auto UberGraphFunction = CastChecked<UFunction>(DestProperty->GetOwnerStruct());
	auto FrameBase = Stack.Object->GetClass()->GetPersistentUberGraphFrame(Stack.Object, UberGraphFunction);
	checkSlow(FrameBase);
	auto DestAddress = DestProperty->ContainerPtrToValuePtr<uint8>(FrameBase);

	Stack.Step(Stack.Object, DestAddress);
#else
	checkf(false, TEXT("execLetValueOnPersistentFrame: UberGraphPersistentFrame is not supported by current build!"));
#endif
}
IMPLEMENT_VM_FUNCTION(EX_LetValueOnPersistentFrame, execLetValueOnPersistentFrame);

void UObject::execSwitchValue(FFrame& Stack, RESULT_DECL)
{
	const int32 NumCases = Stack.ReadWord();
	const CodeSkipSizeType OffsetToEnd = Stack.ReadCodeSkipCount();

	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.Step(Stack.Object, nullptr);

	UProperty* IndexProperty = Stack.MostRecentProperty;
	checkSlow(IndexProperty);

	uint8* IndexAdress = Stack.MostRecentPropertyAddress;
	if (!ensure(IndexAdress))
	{
		FBlueprintExceptionInfo ExceptionInfo(
			EBlueprintExceptionType::NonFatalError, 
			FText::Format(
				LOCTEXT("SwitchValueIndex", "Switch statement failed to read property for index value for index property {0}"),
				FText::FromString(IndexProperty->GetName())
			)
		);
		FBlueprintCoreDelegates::ThrowScriptException(this, Stack, ExceptionInfo);
	}

	bool bProperCaseUsed = false;
	{
		auto LocalTempIndexMem = (uint8*)FMemory_Alloca(IndexProperty->GetSize());
		IndexProperty->InitializeValue(LocalTempIndexMem);
		for (int32 CaseIndex = 0; CaseIndex < NumCases; ++CaseIndex)
		{
			Stack.Step(Stack.Object, LocalTempIndexMem); // case index value
			const CodeSkipSizeType OffsetToNextCase = Stack.ReadCodeSkipCount();

			if (IndexAdress && IndexProperty->Identical(IndexAdress, LocalTempIndexMem))
			{
				Stack.Step(Stack.Object, RESULT_PARAM);
				bProperCaseUsed = true;
				break;
			}

			// skip to the next case
			Stack.Code = &Stack.Node->Script[OffsetToNextCase];
		}
		IndexProperty->DestroyValue(LocalTempIndexMem);
	}

	if (bProperCaseUsed)
	{
		Stack.Code = &Stack.Node->Script[OffsetToEnd];
	}
	else
	{
		FBlueprintExceptionInfo ExceptionInfo(
			EBlueprintExceptionType::NonFatalError, 
			FText::Format(
				LOCTEXT("SwitchValueOutOfBounds", "Switch statement failed to match case for index property {0}"),
				FText::FromString(IndexProperty->GetName())
			)
		);
		FBlueprintCoreDelegates::ThrowScriptException(this, Stack, ExceptionInfo);

		// get default value
		Stack.Step(Stack.Object, RESULT_PARAM);
	}
}
IMPLEMENT_VM_FUNCTION(EX_SwitchValue, execSwitchValue);

void UObject::execArrayGetByRef(FFrame& Stack, RESULT_DECL)
{
	// Get variable address.
	Stack.MostRecentPropertyAddress = NULL;
	Stack.Step( Stack.Object, NULL ); // Evaluate variable.

	if (Stack.MostRecentPropertyAddress == NULL)
	{
		static FBlueprintExceptionInfo ExceptionInfo(EBlueprintExceptionType::AccessViolation, LOCTEXT("ArrayGetRefException", "Attempt to assign variable through None"));
		FBlueprintCoreDelegates::ThrowScriptException(this, Stack, ExceptionInfo);
	}

	void* ArrayAddr = Stack.MostRecentPropertyAddress;
	UArrayProperty* ArrayProperty = ExactCast<UArrayProperty>(Stack.MostRecentProperty);

 	int32 ArrayIndex;
 	Stack.Step( Stack.Object, &ArrayIndex);

	FScriptArrayHelper ArrayHelper(ArrayProperty, ArrayAddr);
	Stack.MostRecentProperty = ArrayProperty->Inner;
	// Add a little safety for Blueprints to not hard crash
	if (ArrayHelper.IsValidIndex(ArrayIndex))
	{
		Stack.MostRecentPropertyAddress = ArrayHelper.GetRawPtr(ArrayIndex);

		if (RESULT_PARAM)
		{
			ArrayProperty->Inner->CopyCompleteValueToScriptVM(RESULT_PARAM, ArrayHelper.GetRawPtr(ArrayIndex));
		}
	}
	else
	{
		FBlueprintExceptionInfo ExceptionInfo(
			EBlueprintExceptionType::AccessViolation,
			FText::Format(
			LOCTEXT("ArrayGetOutofBounds", "Attempted to access index {0} from array {1} of length {2}!"),
			FText::AsNumber(ArrayIndex),
			FText::FromString(*ArrayProperty->GetName()),
			FText::AsNumber(ArrayHelper.Num())
			)
		);
		FBlueprintCoreDelegates::ThrowScriptException(this, Stack, ExceptionInfo);
	}
}
IMPLEMENT_VM_FUNCTION(EX_ArrayGetByRef, execArrayGetByRef);

void UObject::execLet(FFrame& Stack, RESULT_DECL)
{
	Stack.MostRecentProperty = nullptr;
	UProperty* LocallyKnownProperty = Stack.ReadPropertyUnchecked();

	// Get variable address.
	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.Step(Stack.Object, nullptr); // Evaluate variable.

	uint8* LocalTempResult = nullptr;
	if (Stack.MostRecentPropertyAddress == nullptr)
	{
		FBlueprintExceptionInfo ExceptionInfo(
			EBlueprintExceptionType::AccessViolation, 
			LOCTEXT("LetAccessNone", "Attempted to assign to None"));
		FBlueprintCoreDelegates::ThrowScriptException(this, Stack, ExceptionInfo);

		if (LocallyKnownProperty)
		{
			LocalTempResult = (uint8*)FMemory_Alloca(LocallyKnownProperty->GetSize());
			LocallyKnownProperty->InitializeValue(LocalTempResult);
			Stack.MostRecentPropertyAddress = LocalTempResult;
		}
		else
		{
			Stack.MostRecentPropertyAddress = (uint8*)FMemory_Alloca(1024);
			FMemory::Memzero(Stack.MostRecentPropertyAddress, sizeof(FString));
		}
	}

	// Evaluate expression into variable.
	Stack.Step(Stack.Object, Stack.MostRecentPropertyAddress);

	if (LocalTempResult && LocallyKnownProperty)
	{
		LocallyKnownProperty->DestroyValue(LocalTempResult);
	}
}
IMPLEMENT_VM_FUNCTION( EX_Let, execLet );

void UObject::execLetObj( FFrame& Stack, RESULT_DECL )
{
	// Get variable address.
	Stack.MostRecentPropertyAddress = NULL;
	Stack.Step( Stack.Object, NULL ); // Evaluate variable.

	if (Stack.MostRecentPropertyAddress == NULL)
	{
		FBlueprintExceptionInfo ExceptionInfo(
			EBlueprintExceptionType::AccessViolation, 
			LOCTEXT("LetObjAccessNone", "Accessed None attempting to assign variable on an object"));
		FBlueprintCoreDelegates::ThrowScriptException(this, Stack, ExceptionInfo);
	}

	void* ObjAddr = Stack.MostRecentPropertyAddress;
	UObjectPropertyBase* ObjectProperty = dynamic_cast<UObjectPropertyBase*>(Stack.MostRecentProperty);
	if (ObjectProperty == NULL)
	{
		UArrayProperty* ArrayProp = ExactCast<UArrayProperty>(Stack.MostRecentProperty);
		if (ArrayProp != NULL)
		{
			ObjectProperty = dynamic_cast<UObjectPropertyBase*>(ArrayProp->Inner);
		}
	}

	UObject* NewValue = NULL;
	// evaluate the r-value for this expression into Value
	Stack.Step( Stack.Object, &NewValue );

	if (ObjAddr)
	{
		checkSlow(ObjectProperty);
		ObjectProperty->SetObjectPropertyValue(ObjAddr, NewValue);
	}
}
IMPLEMENT_VM_FUNCTION( EX_LetObj, execLetObj );

void UObject::execLetWeakObjPtr( FFrame& Stack, RESULT_DECL )
{
	// Get variable address.
	Stack.MostRecentPropertyAddress = NULL;
	Stack.Step( Stack.Object, NULL ); // Evaluate variable.

	if (Stack.MostRecentPropertyAddress == NULL)
	{
		FBlueprintExceptionInfo ExceptionInfo(
			EBlueprintExceptionType::AccessViolation,
			LOCTEXT("LetWeakObjAccessNone", "Accessed None attempting to assign variable on a weakly referenced object"));
		FBlueprintCoreDelegates::ThrowScriptException(this, Stack, ExceptionInfo);
	}

	void* ObjAddr = Stack.MostRecentPropertyAddress;
	UObjectPropertyBase* ObjectProperty = dynamic_cast<UObjectPropertyBase*>(Stack.MostRecentProperty);
	if (ObjectProperty == NULL)
	{
		UArrayProperty* ArrayProp = ExactCast<UArrayProperty>(Stack.MostRecentProperty);
		if (ArrayProp != NULL)
		{
			ObjectProperty = dynamic_cast<UObjectPropertyBase*>(ArrayProp->Inner);
		}
	}
	
	UObject* NewValue = NULL;
	// evaluate the r-value for this expression into Value
	Stack.Step( Stack.Object, &NewValue );

	if (ObjAddr)
	{
		checkSlow(ObjectProperty);
		ObjectProperty->SetObjectPropertyValue(ObjAddr, NewValue);
	}
}
IMPLEMENT_VM_FUNCTION( EX_LetWeakObjPtr, execLetWeakObjPtr );

void UObject::execLetBool( FFrame& Stack, RESULT_DECL )
{
	Stack.MostRecentPropertyAddress = NULL;
	Stack.MostRecentProperty = NULL;

	// Get the variable and address to place the data.
	Stack.Step( Stack.Object, NULL );

	/*
		Class bool properties are packed together as bitfields, so in order 
		to set the value on the correct bool, we need to mask it against
		the bool property's BitMask.

		Local bool properties (declared inside functions) are not packed, thus
		their bitmask is always 1.

		Bool properties inside dynamic arrays and tmaps are also not packed together.
		If the bool property we're accessing is an element in a dynamic array, Stack.MostRecentProperty
		will be pointing to the dynamic array that has a UBoolProperty as its inner, so
		we'll need to check for that.
	*/
	uint8* BoolAddr = (uint8*)Stack.MostRecentPropertyAddress;
	UBoolProperty* BoolProperty = ExactCast<UBoolProperty>(Stack.MostRecentProperty);
	if (BoolProperty == NULL)
	{
		UArrayProperty* ArrayProp = ExactCast<UArrayProperty>(Stack.MostRecentProperty);
		if (ArrayProp != NULL)
		{
			BoolProperty = ExactCast<UBoolProperty>(ArrayProp->Inner);
		}
	}

	bool NewValue = false;

	// evaluate the r-value for this expression into Value
	Stack.Step( Stack.Object, &NewValue );
	if( BoolAddr )
	{
		checkSlow(dynamic_cast<UBoolProperty*>(BoolProperty));
		BoolProperty->SetPropertyValue( BoolAddr, NewValue );
	}
}
IMPLEMENT_VM_FUNCTION( EX_LetBool, execLetBool );


void UObject::execLetDelegate( FFrame& Stack, RESULT_DECL )
{
	// Get variable address.
	Stack.MostRecentPropertyAddress = NULL;
	Stack.MostRecentProperty = NULL;
	Stack.Step( Stack.Object, NULL ); // Variable.

	FScriptDelegate* DelegateAddr = (FScriptDelegate*)Stack.MostRecentPropertyAddress;
	FScriptDelegate Delegate;
	Stack.Step( Stack.Object, &Delegate );

	if (DelegateAddr != NULL)
	{
		DelegateAddr->BindUFunction( Delegate.GetUObject(), Delegate.GetFunctionName() );
	}
}
IMPLEMENT_VM_FUNCTION( EX_LetDelegate, execLetDelegate );


void UObject::execLetMulticastDelegate( FFrame& Stack, RESULT_DECL )
{
	// Get variable address.
	Stack.MostRecentPropertyAddress = NULL;
	Stack.MostRecentProperty = NULL;
	Stack.Step( Stack.Object, NULL ); // Variable.

	FMulticastScriptDelegate* DelegateAddr = (FMulticastScriptDelegate*)Stack.MostRecentPropertyAddress;
	FMulticastScriptDelegate Delegate;
	Stack.Step( Stack.Object, &Delegate );

	if (DelegateAddr != NULL)
	{
		*DelegateAddr = Delegate;
	}
}
IMPLEMENT_VM_FUNCTION( EX_LetMulticastDelegate, execLetMulticastDelegate );


void UObject::execSelf( FFrame& Stack, RESULT_DECL )
{
	// Get Self actor for this context.
	*(UObject**)RESULT_PARAM = this;
}
IMPLEMENT_VM_FUNCTION( EX_Self, execSelf );

void UObject::execContext( FFrame& Stack, RESULT_DECL )
{
	ProcessContextOpcode(Stack, RESULT_PARAM, /*bCanFailSilently=*/ false);
}
IMPLEMENT_VM_FUNCTION( EX_Context, execContext );

void UObject::execContext_FailSilent( FFrame& Stack, RESULT_DECL )
{
	ProcessContextOpcode(Stack, RESULT_PARAM, /*bCanFailSilently=*/ true);
}
IMPLEMENT_VM_FUNCTION( EX_Context_FailSilent, execContext_FailSilent );

void UObject::ProcessContextOpcode( FFrame& Stack, RESULT_DECL, bool bCanFailSilently )
{
	Stack.MostRecentProperty = NULL;
	
	// Get object variable.
	UObject* NewContext = NULL;
	Stack.Step( this, &NewContext );

	uint8* const OriginalCode = Stack.Code;
	const bool bValidContext = IsValid(NewContext);
	// Execute or skip the following expression in the object's context.
	if (bValidContext)
	{
		Stack.Code += sizeof(CodeSkipSizeType)	// Code offset for NULL expressions.
			+ sizeof(ScriptPointerType);		// Property corresponding to the r-value data, in case the l-value needs to be cleared
		Stack.Step( NewContext, RESULT_PARAM );
	}

	if (!bValidContext || Stack.bArrayContextFailed)
	{
		if (Stack.bArrayContextFailed)
		{
			Stack.bArrayContextFailed = false;
			Stack.Code = OriginalCode;
		}

		if (!bCanFailSilently)
		{
			if (NewContext && NewContext->IsPendingKill())
			{
				FBlueprintExceptionInfo ExceptionInfo(
					EBlueprintExceptionType::AccessViolation, 
					FText::Format(
						LOCTEXT("AccessPendingKill", "Attempted to access {0} via property {1}, but {0} is pending kill"),
						FText::FromString( GetNameSafe(NewContext) ), 
						FText::FromString( GetNameSafe(Stack.MostRecentProperty) )
					)
				);
				FBlueprintCoreDelegates::ThrowScriptException(this, Stack, ExceptionInfo);
			}
			else if (Stack.MostRecentProperty != NULL)
			{
				FBlueprintExceptionInfo ExceptionInfo(
					EBlueprintExceptionType::AccessViolation, 
					FText::Format( 
						LOCTEXT("AccessNoneContext", "Accessed None trying to read property {0}"), 
						FText::FromString( Stack.MostRecentProperty->GetName() )
					)
				);
				FBlueprintCoreDelegates::ThrowScriptException(this, Stack, ExceptionInfo);
			}
			else
			{
				// Stack.MostRecentProperty will be NULL under the following conditions:
				//   1. the context expression was a function call which returned an object
				//   2. the context expression was a literal object reference
				//   3. the context expression was an instance variable that no longer exists (it was editor-only, etc.)
				FBlueprintExceptionInfo ExceptionInfo(
					EBlueprintExceptionType::AccessViolation, 
					LOCTEXT("AccessNoneNoContext", "Accessed None")
				);
				FBlueprintCoreDelegates::ThrowScriptException(this, Stack, ExceptionInfo);
			}
		}

		const CodeSkipSizeType wSkip = Stack.ReadCodeSkipCount(); // Code offset for NULL expressions. Code += sizeof(CodeSkipSizeType)
		UProperty* RValueProperty = nullptr;
		const VariableSizeType bSize = Stack.ReadVariableSize(&RValueProperty); // Code += sizeof(ScriptPointerType) + sizeof(uint8)
		Stack.Code += wSkip;
		Stack.MostRecentPropertyAddress = NULL;
		Stack.MostRecentProperty = NULL;

		if (RESULT_PARAM && RValueProperty)
		{
			RValueProperty->ClearValue(RESULT_PARAM);
		}
	}
}

void UObject::execStructMemberContext(FFrame& Stack, RESULT_DECL)
{
	// Get the structure element we care about
	UProperty* StructProperty = Stack.ReadProperty();
	checkSlow(StructProperty);

	// Evaluate an expression leading to the struct.
	Stack.MostRecentProperty = NULL;
	Stack.MostRecentPropertyAddress = NULL;
	Stack.Step(Stack.Object, NULL);

	if (Stack.MostRecentProperty != NULL)
	{
		// Offset into the specific member
		Stack.MostRecentPropertyAddress = StructProperty->ContainerPtrToValuePtr<uint8>(Stack.MostRecentPropertyAddress);
		Stack.MostRecentProperty = StructProperty;

		// Handle variable reads
		if (RESULT_PARAM)
		{
			StructProperty->CopyCompleteValueToScriptVM(RESULT_PARAM, Stack.MostRecentPropertyAddress);
		}
	}
	else
	{
		// Access none
		FBlueprintExceptionInfo ExceptionInfo(
			EBlueprintExceptionType::AccessViolation, 
			FText::Format(
				LOCTEXT("AccessNoneStructure", "Accessed None reading structure {0}"),
				FText::FromString(StructProperty->GetName())
			)
		);
		FBlueprintCoreDelegates::ThrowScriptException(this, Stack, ExceptionInfo);

		Stack.MostRecentPropertyAddress = NULL;
		Stack.MostRecentProperty = NULL;
	}
}
IMPLEMENT_VM_FUNCTION( EX_StructMemberContext, execStructMemberContext );

void UObject::execVirtualFunction( FFrame& Stack, RESULT_DECL )
{
	// Call the virtual function.
	CallFunction( Stack, RESULT_PARAM, FindFunctionChecked(Stack.ReadName()) );
}
IMPLEMENT_VM_FUNCTION( EX_VirtualFunction, execVirtualFunction );

void UObject::execFinalFunction( FFrame& Stack, RESULT_DECL )
{
	// Call the final function.
	CallFunction( Stack, RESULT_PARAM, (UFunction*)Stack.ReadObject() );
}
IMPLEMENT_VM_FUNCTION( EX_FinalFunction, execFinalFunction );

class FCallDelegateHelper
{
public:
	static void CallMulticastDelegate(FFrame& Stack)
	{
		//Get delegate
		UFunction* SignatureFunction = CastChecked<UFunction>(Stack.ReadObject());
		Stack.MostRecentPropertyAddress = NULL;
		Stack.MostRecentProperty = NULL;
		Stack.Step( Stack.Object, NULL );
		const FMulticastScriptDelegate* DelegateAddr = (FMulticastScriptDelegate*)Stack.MostRecentPropertyAddress;

		//Fill parameters
		uint8* Parameters = (uint8*)FMemory_Alloca(SignatureFunction->ParmsSize);
		FMemory::Memzero(Parameters, SignatureFunction->ParmsSize);
		for (UProperty* Property = (UProperty*)SignatureFunction->Children; *Stack.Code != EX_EndFunctionParms; Property = (UProperty*)Property->Next)
		{
			Stack.MostRecentPropertyAddress = NULL;
			if (Property->PropertyFlags & CPF_OutParm)
			{
				Stack.Step(Stack.Object, NULL);
				if(NULL != Stack.MostRecentPropertyAddress)
				{
					check(Property->IsInContainer(SignatureFunction->ParmsSize));
					uint8* ConstRefCopyParamAdress = Property->ContainerPtrToValuePtr<uint8>(Parameters);
					Property->CopyCompleteValueToScriptVM(ConstRefCopyParamAdress, Stack.MostRecentPropertyAddress);
				}
			}
			else
			{
				uint8* Param = Property->ContainerPtrToValuePtr<uint8>(Parameters);
				checkSlow(Param);
				Property->InitializeValue_InContainer(Parameters);
				Stack.Step(Stack.Object, Param);
			}
		}
		Stack.Code++;

		//Process delegate
		if (DelegateAddr)
		{
			DelegateAddr->ProcessMulticastDelegate<UObject>(Parameters);
		}
		
		//Clean parameters
		for (UProperty* Destruct = SignatureFunction->DestructorLink; Destruct; Destruct = Destruct->DestructorLinkNext)
		{
			Destruct->DestroyValue_InContainer(Parameters);
		}
	}
};

void UObject::execCallMulticastDelegate( FFrame& Stack, RESULT_DECL )
{
	FCallDelegateHelper::CallMulticastDelegate(Stack);
}
IMPLEMENT_VM_FUNCTION( EX_CallMulticastDelegate, execCallMulticastDelegate );

void UObject::execAddMulticastDelegate( FFrame& Stack, RESULT_DECL )
{
	// Get variable address.
	Stack.MostRecentPropertyAddress = NULL;
	Stack.MostRecentProperty = NULL;
	Stack.Step( Stack.Object, NULL ); // Variable.

	FMulticastScriptDelegate* DelegateAddr = (FMulticastScriptDelegate*)Stack.MostRecentPropertyAddress;
	FScriptDelegate Delegate;
	Stack.Step( Stack.Object, &Delegate );

	if (DelegateAddr != NULL)
	{
		DelegateAddr->AddUnique(Delegate);
	}
}
IMPLEMENT_VM_FUNCTION( EX_AddMulticastDelegate, execAddMulticastDelegate );

void UObject::execRemoveMulticastDelegate( FFrame& Stack, RESULT_DECL )
{
	// Get variable address.
	Stack.MostRecentPropertyAddress = NULL;
	Stack.MostRecentProperty = NULL;
	Stack.Step( Stack.Object, NULL ); // Variable.

	FMulticastScriptDelegate* DelegateAddr = (FMulticastScriptDelegate*)Stack.MostRecentPropertyAddress;
	FScriptDelegate Delegate;
	Stack.Step( Stack.Object, &Delegate );

	if (DelegateAddr != NULL)
	{
		DelegateAddr->Remove(Delegate);
	}
}
IMPLEMENT_VM_FUNCTION( EX_RemoveMulticastDelegate, execRemoveMulticastDelegate );

void UObject::execClearMulticastDelegate( FFrame& Stack, RESULT_DECL )
{
	// Get the delegate address
	Stack.MostRecentPropertyAddress = NULL;
	Stack.MostRecentProperty = NULL;
	Stack.Step( Stack.Object, NULL );

	FMulticastScriptDelegate* DelegateAddr = (FMulticastScriptDelegate*)Stack.MostRecentPropertyAddress;
	if (DelegateAddr != NULL)
	{
		DelegateAddr->Clear();
	}
}
IMPLEMENT_VM_FUNCTION( EX_ClearMulticastDelegate, execClearMulticastDelegate );

void UObject::execIntConst( FFrame& Stack, RESULT_DECL )
{
	*(int32*)RESULT_PARAM = Stack.ReadInt<int32>();
}
IMPLEMENT_VM_FUNCTION( EX_IntConst, execIntConst );

void UObject::execInt64Const(FFrame& Stack, RESULT_DECL)
{
	*(int64*)RESULT_PARAM = Stack.ReadInt<int64>();
}
IMPLEMENT_VM_FUNCTION(EX_Int64Const, execInt64Const);

void UObject::execUInt64Const(FFrame& Stack, RESULT_DECL)
{
	*(uint64*)RESULT_PARAM = Stack.ReadInt<uint64>();
}
IMPLEMENT_VM_FUNCTION(EX_UInt64Const, execUInt64Const);

void UObject::execSkipOffsetConst( FFrame& Stack, RESULT_DECL )
{
	CodeSkipSizeType Literal = Stack.ReadCodeSkipCount();
	*(int32*)RESULT_PARAM = Literal;
}
IMPLEMENT_VM_FUNCTION( EX_SkipOffsetConst, execSkipOffsetConst );

void UObject::execFloatConst( FFrame& Stack, RESULT_DECL )
{
	*(float*)RESULT_PARAM = Stack.ReadFloat();
}
IMPLEMENT_VM_FUNCTION( EX_FloatConst, execFloatConst );

void UObject::execStringConst( FFrame& Stack, RESULT_DECL )
{
	*(FString*)RESULT_PARAM = (ANSICHAR*)Stack.Code;
	while( *Stack.Code )
		Stack.Code++;
	Stack.Code++;
}
IMPLEMENT_VM_FUNCTION( EX_StringConst, execStringConst );

void UObject::execUnicodeStringConst( FFrame& Stack, RESULT_DECL )
{
 	*(FString*)RESULT_PARAM = FString((UCS2CHAR*)Stack.Code);

	while( *(uint16*)Stack.Code )
	{
		Stack.Code+=sizeof(uint16);
	}
	Stack.Code+=sizeof(uint16);
}
IMPLEMENT_VM_FUNCTION( EX_UnicodeStringConst, execUnicodeStringConst );

void UObject::execTextConst( FFrame& Stack, RESULT_DECL )
{
	// What kind of text are we dealing with?
	const EBlueprintTextLiteralType TextLiteralType = (EBlueprintTextLiteralType)*Stack.Code++;

	switch (TextLiteralType)
	{
	case EBlueprintTextLiteralType::Empty:
		{
			*(FText*)RESULT_PARAM = FText::GetEmpty();
		}
		break;

	case EBlueprintTextLiteralType::LocalizedText:
		{
			FString SourceString;
			Stack.Step(Stack.Object, &SourceString);

			FString KeyString;
			Stack.Step(Stack.Object, &KeyString);
			
			FString Namespace;
			Stack.Step(Stack.Object, &Namespace);

			*(FText*)RESULT_PARAM = FInternationalization::ForUseOnlyByLocMacroAndGraphNodeTextLiterals_CreateText(*SourceString, *Namespace, *KeyString);
		}
		break;

	case EBlueprintTextLiteralType::InvariantText:
		{
			FString SourceString;
			Stack.Step(Stack.Object, &SourceString);

			*(FText*)RESULT_PARAM = FText::AsCultureInvariant(MoveTemp(SourceString));
		}
		break;

	case EBlueprintTextLiteralType::LiteralString:
		{
			FString SourceString;
			Stack.Step(Stack.Object, &SourceString);

			*(FText*)RESULT_PARAM = FText::FromString(MoveTemp(SourceString));
		}
		break;

	default:
		checkf(false, TEXT("Unknown EBlueprintTextLiteralType! Please update UObject::execTextConst to handle this type of text."));
		break;
	}
}
IMPLEMENT_VM_FUNCTION( EX_TextConst, execTextConst );

void UObject::execObjectConst( FFrame& Stack, RESULT_DECL )
{
	*(UObject**)RESULT_PARAM = (UObject*)Stack.ReadObject();
}
IMPLEMENT_VM_FUNCTION( EX_ObjectConst, execObjectConst );

void UObject::execAssetConst(FFrame& Stack, RESULT_DECL)
{
	FString LongPath;
	Stack.Step(Stack.Object, &LongPath);
	*(FAssetPtr*)RESULT_PARAM = FStringAssetReference(LongPath);
}
IMPLEMENT_VM_FUNCTION(EX_AssetConst, execAssetConst);

void UObject::execInstanceDelegate( FFrame& Stack, RESULT_DECL )
{
	FName FunctionName = Stack.ReadName();
	((FScriptDelegate*)RESULT_PARAM)->BindUFunction( (FunctionName == NAME_None) ? NULL : this, FunctionName );
}
IMPLEMENT_VM_FUNCTION( EX_InstanceDelegate, execInstanceDelegate );

void UObject::execBindDelegate( FFrame& Stack, RESULT_DECL )
{
	FName FunctionName = Stack.ReadName();

	// Get delegate address.
	Stack.MostRecentPropertyAddress = NULL;
	Stack.MostRecentProperty = NULL;
	Stack.Step( Stack.Object, NULL ); // Variable.

	FScriptDelegate* DelegateAddr = (FScriptDelegate*)Stack.MostRecentPropertyAddress;

	UObject* ObjectForDelegate = NULL;
	Stack.Step(Stack.Object, &ObjectForDelegate);

	if (DelegateAddr)
	{
		DelegateAddr->BindUFunction(ObjectForDelegate, FunctionName);
	}
}
IMPLEMENT_VM_FUNCTION( EX_BindDelegate, execBindDelegate );

void UObject::execNameConst( FFrame& Stack, RESULT_DECL )
{
	*(FName*)RESULT_PARAM = Stack.ReadName();
}
IMPLEMENT_VM_FUNCTION( EX_NameConst, execNameConst );

void UObject::execByteConst( FFrame& Stack, RESULT_DECL )
{
	*(uint8*)RESULT_PARAM = *Stack.Code++;
}
IMPLEMENT_VM_FUNCTION( EX_ByteConst, execByteConst );

void UObject::execRotationConst( FFrame& Stack, RESULT_DECL )
{
	((FRotator*)RESULT_PARAM)->Pitch = Stack.ReadFloat();
	((FRotator*)RESULT_PARAM)->Yaw   = Stack.ReadFloat();
	((FRotator*)RESULT_PARAM)->Roll  = Stack.ReadFloat();
}
IMPLEMENT_VM_FUNCTION( EX_RotationConst, execRotationConst );

void UObject::execVectorConst( FFrame& Stack, RESULT_DECL )
{
	((FVector*)RESULT_PARAM)->X = Stack.ReadFloat();
	((FVector*)RESULT_PARAM)->Y = Stack.ReadFloat();
	((FVector*)RESULT_PARAM)->Z = Stack.ReadFloat();
}
IMPLEMENT_VM_FUNCTION( EX_VectorConst, execVectorConst );

void UObject::execTransformConst( FFrame& Stack, RESULT_DECL )
{
	// Rotation
	FQuat TmpRotation;
	TmpRotation.X = Stack.ReadFloat();
	TmpRotation.Y = Stack.ReadFloat();
	TmpRotation.Z = Stack.ReadFloat();
	TmpRotation.W = Stack.ReadFloat();

	// Translation
	FVector TmpTranslation;
	TmpTranslation.X = Stack.ReadFloat();
	TmpTranslation.Y = Stack.ReadFloat();
	TmpTranslation.Z = Stack.ReadFloat();

	// Scale
	FVector TmpScale;
	TmpScale.X = Stack.ReadFloat();
	TmpScale.Y = Stack.ReadFloat();
	TmpScale.Z = Stack.ReadFloat();

	((FTransform*)RESULT_PARAM)->SetComponents(TmpRotation, TmpTranslation, TmpScale);
}
IMPLEMENT_VM_FUNCTION( EX_TransformConst, execTransformConst );

void UObject::execStructConst( FFrame& Stack, RESULT_DECL )
{
	UScriptStruct* ScriptStruct = CastChecked<UScriptStruct>(Stack.ReadObject());
	int32 SerializedSize = Stack.ReadInt<int32>();

	// Temporarily disabling this check because we can't assume the serialized size
	// will match the struct size on all platforms (like win64 vs win32 cooked)
	//check( ScriptStruct->GetStructureSize() == SerializedSize );
	
	for( UProperty* StructProp = ScriptStruct->PropertyLink; StructProp; StructProp = StructProp->PropertyLinkNext )
	{
		for (int32 ArrayIter = 0; ArrayIter < StructProp->ArrayDim; ++ArrayIter)
		{
			Stack.Step(Stack.Object, StructProp->ContainerPtrToValuePtr<uint8>(RESULT_PARAM, ArrayIter));
		}
	}

	P_FINISH;	// EX_EndStructConst
}
IMPLEMENT_VM_FUNCTION( EX_StructConst, execStructConst );

void UObject::execSetArray( FFrame& Stack, RESULT_DECL )
{
	// Get the array address
	Stack.MostRecentPropertyAddress = NULL;
	Stack.MostRecentProperty = NULL;
	Stack.Step( Stack.Object, NULL ); // Array to set
	
	UArrayProperty* ArrayProperty = CastChecked<UArrayProperty>(Stack.MostRecentProperty);
 	FScriptArrayHelper ArrayHelper(ArrayProperty, Stack.MostRecentPropertyAddress);
 	ArrayHelper.EmptyValues();
 
 	// Read in the parameters one at a time
 	int32 i = 0;
 	while(*Stack.Code != EX_EndArray)
 	{
 		ArrayHelper.AddValues(1);
 		Stack.Step(Stack.Object, ArrayHelper.GetRawPtr(i++));
 	}
 
 	P_FINISH;
}
IMPLEMENT_VM_FUNCTION( EX_SetArray, execSetArray );

void UObject::execArrayConst(FFrame& Stack, RESULT_DECL)
{
	UProperty* InnerProperty = CastChecked<UProperty>(Stack.ReadObject());
	int32 Num = Stack.ReadInt<int32>();
	check(RESULT_PARAM);
	FScriptArrayHelper ArrayHelper = FScriptArrayHelper::CreateHelperFormInnerProperty(InnerProperty, RESULT_PARAM);
	ArrayHelper.EmptyValues(Num);

	int32 i = 0;
	while (*Stack.Code != EX_EndArrayConst)
	{
		ArrayHelper.AddValues(1);
		Stack.Step(Stack.Object, ArrayHelper.GetRawPtr(i++));
	}
	ensure(i == Num);

	P_FINISH;	// EX_EndArrayConst
}
IMPLEMENT_VM_FUNCTION(EX_ArrayConst, execArrayConst);

void UObject::execIntZero( FFrame& Stack, RESULT_DECL )
{
	*(int32*)RESULT_PARAM = 0;
}
IMPLEMENT_VM_FUNCTION( EX_IntZero, execIntZero );

void UObject::execIntOne( FFrame& Stack, RESULT_DECL )
{
	*(int32*)RESULT_PARAM = 1;
}
IMPLEMENT_VM_FUNCTION( EX_IntOne, execIntOne );

void UObject::execTrue( FFrame& Stack, RESULT_DECL )
{
	*(bool*)RESULT_PARAM = true;
}
IMPLEMENT_VM_FUNCTION( EX_True, execTrue );

void UObject::execFalse( FFrame& Stack, RESULT_DECL )
{
	*(bool*)RESULT_PARAM = false;
}
IMPLEMENT_VM_FUNCTION( EX_False, execFalse );

void UObject::execNoObject( FFrame& Stack, RESULT_DECL )
{
	*(UObject**)RESULT_PARAM = NULL;
}
IMPLEMENT_VM_FUNCTION( EX_NoObject, execNoObject );

void UObject::execNullInterface(FFrame& Stack, RESULT_DECL)
{
	FScriptInterface& InterfaceValue = *(FScriptInterface*)RESULT_PARAM;
	InterfaceValue.SetObject(nullptr);
}
IMPLEMENT_VM_FUNCTION( EX_NoInterface, execNullInterface );

void UObject::execIntConstByte( FFrame& Stack, RESULT_DECL )
{
	*(int32*)RESULT_PARAM = *Stack.Code++;
}
IMPLEMENT_VM_FUNCTION( EX_IntConstByte, execIntConstByte );


void UObject::execDynamicCast( FFrame& Stack, RESULT_DECL )
{
	// Get "to cast to" class for the dynamic actor class
	UClass* ClassPtr = (UClass *)Stack.ReadObject();

	// Compile object expression.
	UObject* Castee = NULL;
	Stack.Step( Stack.Object, &Castee );
	//*(UObject**)RESULT_PARAM = (Castee && Castee->IsA(Class)) ? Castee : NULL;
	*(UObject**)RESULT_PARAM = NULL; // default value

	if (ClassPtr)
	{
		// if we were passed in a null value
		if( Castee == NULL )
		{
			if(ClassPtr->HasAnyClassFlags(CLASS_Interface) )
			{
				((FScriptInterface*)RESULT_PARAM)->SetObject(NULL);
			}
			else
			{
				*(UObject**)RESULT_PARAM = NULL;
			}
			return;
		}

		// check to see if the Castee is an implemented interface by looking up the
		// class hierarchy and seeing if any class in said hierarchy implements the interface
		if(ClassPtr->HasAnyClassFlags(CLASS_Interface) )
		{
			if ( Castee->GetClass()->ImplementsInterface(ClassPtr) )
			{
				// interface property type - convert to FScriptInterface
				((FScriptInterface*)RESULT_PARAM)->SetObject(Castee);
				((FScriptInterface*)RESULT_PARAM)->SetInterface(Castee->GetInterfaceAddress(ClassPtr));
			}
		}
		// check to see if the Castee is a castable class
		else if( Castee->IsA(ClassPtr) )
		{
			*(UObject**)RESULT_PARAM = Castee;
		}
	}
}
IMPLEMENT_VM_FUNCTION( EX_DynamicCast, execDynamicCast );

void UObject::execMetaCast( FFrame& Stack, RESULT_DECL )
{
	UClass* MetaClass = (UClass*)Stack.ReadObject();

	// Compile actor expression.
	UObject* Castee = nullptr;
	Stack.Step( Stack.Object, &Castee );
	UClass* CasteeClass = dynamic_cast<UClass*>(Castee);
	*(UObject**)RESULT_PARAM = (CasteeClass && CasteeClass->IsChildOf(MetaClass)) ? Castee : nullptr;
}
IMPLEMENT_VM_FUNCTION( EX_MetaCast, execMetaCast );

void UObject::execPrimitiveCast( FFrame& Stack, RESULT_DECL )
{
	int32 B = *(Stack.Code)++;
	(Stack.Object->*GCasts[B])( Stack, RESULT_PARAM );
}
IMPLEMENT_VM_FUNCTION( EX_PrimitiveCast, execPrimitiveCast );

void UObject::execInterfaceCast( FFrame& Stack, RESULT_DECL )
{
	(Stack.Object->*GCasts[CST_ObjectToInterface])(Stack, RESULT_PARAM);
}
IMPLEMENT_VM_FUNCTION( EX_ObjToInterfaceCast, execInterfaceCast );

void UObject::execObjectToBool( FFrame& Stack, RESULT_DECL )
{
	UObject* Obj=NULL;
	Stack.Step( Stack.Object, &Obj );
	*(bool*)RESULT_PARAM = Obj != NULL;
}
IMPLEMENT_CAST_FUNCTION( UObject, CST_ObjectToBool, execObjectToBool );

void UObject::execInterfaceToBool( FFrame& Stack, RESULT_DECL )
{
	FScriptInterface Interface;
	Stack.Step( Stack.Object, &Interface);
	*(bool*)RESULT_PARAM = (Interface.GetObject() != NULL);
}
IMPLEMENT_CAST_FUNCTION( UObject, CST_InterfaceToBool, execInterfaceToBool );

void UObject::execObjectToInterface( FFrame& Stack, RESULT_DECL )
{
	FScriptInterface& InterfaceValue = *(FScriptInterface*)RESULT_PARAM;

	// read the interface class off the stack
	UClass* InterfaceClass = dynamic_cast<UClass*>(Stack.ReadObject());
	checkSlow(InterfaceClass != NULL);

	// read the object off the stack
	UObject* ObjectValue = NULL;
	Stack.Step( Stack.Object, &ObjectValue );

	if ( ObjectValue && ObjectValue->GetClass()->ImplementsInterface(InterfaceClass) )
	{
		InterfaceValue.SetObject(ObjectValue);

		void* IAddress = ObjectValue->GetInterfaceAddress(InterfaceClass);
		InterfaceValue.SetInterface(IAddress);
	}
	else
	{
		InterfaceValue.SetObject(NULL);
	}
}
IMPLEMENT_CAST_FUNCTION( UObject, CST_ObjectToInterface, execObjectToInterface );

void UObject::execInterfaceToInterface( FFrame& Stack, RESULT_DECL )
{
	FScriptInterface& CastResult = *(FScriptInterface*)RESULT_PARAM;

	// read the interface class off the stack
	UClass* ClassToCastTo = dynamic_cast<UClass*>(Stack.ReadObject());
	checkSlow(ClassToCastTo != NULL);
	checkSlow(ClassToCastTo->HasAnyClassFlags(CLASS_Interface));

	// read the input interface-object off the stack
	FScriptInterface InterfaceInput;
	Stack.Step(Stack.Object, &InterfaceInput);

	UObject* ObjectWithInterface = InterfaceInput.GetObjectRef();
	if ((ObjectWithInterface != NULL) && ObjectWithInterface->GetClass()->ImplementsInterface(ClassToCastTo))
	{
		CastResult.SetObject(ObjectWithInterface);

		void* IAddress = ObjectWithInterface->GetInterfaceAddress(ClassToCastTo);
		CastResult.SetInterface(IAddress);
	}
	else
	{
 		CastResult.SetObject(NULL);
 	}
}
IMPLEMENT_VM_FUNCTION( EX_CrossInterfaceCast, execInterfaceToInterface );

void UObject::execInterfaceToObject(FFrame& Stack, RESULT_DECL)
{
	// read the interface class off the stack
	UClass* ObjClassToCastTo = dynamic_cast<UClass*>(Stack.ReadObject());
	checkSlow(ObjClassToCastTo != nullptr);

	// read the input interface-object off the stack
	FScriptInterface InterfaceInput;
	Stack.Step(Stack.Object, &InterfaceInput);

	UObject* InputObjWithInterface = InterfaceInput.GetObjectRef();
	if (InputObjWithInterface && InputObjWithInterface->IsA(ObjClassToCastTo))
	{
		*(UObject**)RESULT_PARAM = InputObjWithInterface;
	}
	else
	{
		*(UObject**)RESULT_PARAM = nullptr;
	}
}
IMPLEMENT_VM_FUNCTION( EX_InterfaceToObjCast, execInterfaceToObject );

#undef LOCTEXT_NAMESPACE
