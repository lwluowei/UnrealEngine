// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	MaterialShared.cpp: Shared material implementation.
=============================================================================*/

#include "EnginePrivate.h"
#include "PixelFormat.h"
#include "ShaderCompiler.h"
#include "MaterialCompiler.h"
#include "MaterialShader.h"
#include "MeshMaterialShader.h"
#include "HLSLMaterialTranslator.h"
#include "MaterialUniformExpressions.h"
#include "Developer/TargetPlatform/Public/TargetPlatform.h"

DEFINE_LOG_CATEGORY(LogMaterial);

FName MaterialQualityLevelNames[] = 
{
	FName(TEXT("Low")),
	FName(TEXT("High")),
	FName(TEXT("Num"))
};

checkAtCompileTime(ARRAY_COUNT(MaterialQualityLevelNames) == EMaterialQualityLevel::Num + 1, MissingEntryFromMaterialQualityLevelNames);

void GetMaterialQualityLevelName(EMaterialQualityLevel::Type InQualityLevel, FString& OutName)
{
	check(InQualityLevel < ARRAY_COUNT(MaterialQualityLevelNames));
	MaterialQualityLevelNames[(int32)InQualityLevel].ToString(OutName);
}

int32 FMaterialCompiler::Errorf(const TCHAR* Format,...)
{
	TCHAR	ErrorText[2048];
	GET_VARARGS( ErrorText, ARRAY_COUNT(ErrorText), ARRAY_COUNT(ErrorText)-1, Format, Format );
	return Error(ErrorText);
}

//
//	FExpressionInput::Compile
//

int32 FExpressionInput::Compile(class FMaterialCompiler* Compiler, int32 MultiplexIndex)
{
	if(Expression)
	{
		Expression->ValidateState();

		if(Mask)
		{
			int32 ExpressionResult = Compiler->CallExpression(FMaterialExpressionKey(Expression, OutputIndex, MultiplexIndex),Compiler);
			if(ExpressionResult != INDEX_NONE)
			{
				return Compiler->ComponentMask(
					ExpressionResult,
					!!MaskR,!!MaskG,!!MaskB,!!MaskA
					);
			}
			else
			{
				return INDEX_NONE;
			}
		}
		else
		{
			return Compiler->CallExpression(FMaterialExpressionKey(Expression, OutputIndex, MultiplexIndex),Compiler);
		}
	}
	else
		return INDEX_NONE;
}

void FExpressionInput::Connect( int32 InOutputIndex, class UMaterialExpression* InExpression )
{
	OutputIndex = InOutputIndex;
	Expression = InExpression;

	TArray<FExpressionOutput> Outputs;
	Outputs = Expression->GetOutputs();
	FExpressionOutput* Output = &Outputs[ OutputIndex ];
	Mask = Output->Mask;
	MaskR = Output->MaskR;
	MaskG = Output->MaskG;
	MaskB = Output->MaskB;
	MaskA = Output->MaskA;
}

//
//	FColorMaterialInput::Compile
//

int32 FColorMaterialInput::Compile(class FMaterialCompiler* Compiler,const FColor& Default)
{
	if(UseConstant)
	{
		FLinearColor	LinearColor(Constant);
		return Compiler->Constant3(LinearColor.R,LinearColor.G,LinearColor.B);
	}
	else if(Expression)
	{
		int32 ResultIndex = FExpressionInput::Compile(Compiler);
		if (ResultIndex == INDEX_NONE)
		{
			FLinearColor	LinearColor(Default);
			return Compiler->Constant3(LinearColor.R,LinearColor.G,LinearColor.B);
		}
		else
		{
			return ResultIndex;
		}
	}
	else
	{
		FLinearColor	LinearColor(Default);
		return Compiler->Constant3(LinearColor.R,LinearColor.G,LinearColor.B);
	}
}

//
//	FScalarMaterialInput::Compile
//

int32 FScalarMaterialInput::Compile(class FMaterialCompiler* Compiler,float Default)
{
	if(UseConstant)
	{
		return Compiler->Constant(Constant);
	}
	else if(Expression)
	{
		int32 ResultIndex = FExpressionInput::Compile(Compiler);
		if (ResultIndex == INDEX_NONE)
		{
			return Compiler->Constant(Default);
		}
		else
		{
			return ResultIndex;
		}
	}
	else
	{
		return Compiler->Constant(Default);
	}
}

//
//	FVectorMaterialInput::Compile
//

int32 FVectorMaterialInput::Compile(class FMaterialCompiler* Compiler,const FVector& Default)
{
	if(UseConstant)
	{
		return Compiler->Constant3(Constant.X,Constant.Y,Constant.Z);
	}
	else if(Expression)
	{
		int32 ResultIndex = FExpressionInput::Compile(Compiler);
		if (ResultIndex == INDEX_NONE)
		{
			return Compiler->Constant3(Default.X,Default.Y,Default.Z);
		}
		else
		{
			return ResultIndex;
		}
	}
	else
	{
		return Compiler->Constant3(Default.X,Default.Y,Default.Z);
	}
}

//
//	FVector2MaterialInput::Compile
//

int32 FVector2MaterialInput::Compile(class FMaterialCompiler* Compiler,const FVector2D& Default)
{
	if(UseConstant)
	{
		return Compiler->Constant2(Constant.X,Constant.Y);
	}
	else if(Expression)
	{
		int32 ResultIndex = FExpressionInput::Compile(Compiler);
		if (ResultIndex == INDEX_NONE)
		{
			return Compiler->Constant2(Default.X,Default.Y);
		}
		else
		{
			return ResultIndex;
		}
	}
	else
	{
		return Compiler->Constant2(Default.X,Default.Y);
	}
}

//
//	FMaterialAttributesInput::Compile
//

int32 FMaterialAttributesInput::Compile(class FMaterialCompiler* Compiler, EMaterialProperty Property, float DefaultFloat, const FColor& DefaultColor, const FVector& DefaultVector)
{
	int32 Ret = INDEX_NONE;
	if(Expression)
	{
		int32 MultiplexIndex = GetInputOutputIndexFromMaterialProperty(Property);

		if( MultiplexIndex >= 0 )
		{
			Ret = FExpressionInput::Compile(Compiler,MultiplexIndex);

			if( Ret != INDEX_NONE && !Expression->IsResultMaterialAttributes(OutputIndex) )
			{
				Compiler->Error(TEXT("Cannot connect a non MaterialAttributes node to a MaterialAttributes pin."));
			}
		}
	}

	if( Ret == INDEX_NONE )
	{
		//Fallback to defaults
		switch(Property)
		{
		case MP_EmissiveColor: Ret = Compiler->Constant3(DefaultColor.R,DefaultColor.G,DefaultColor.B); break;
		case MP_Opacity: Ret = Compiler->Constant(DefaultFloat); break;
		case MP_OpacityMask: Ret = Compiler->Constant(DefaultFloat); break;
		case MP_DiffuseColor: Ret = Compiler->Constant3(DefaultColor.R,DefaultColor.G,DefaultColor.B); break;
		case MP_SpecularColor: Ret = Compiler->Constant3(DefaultColor.R,DefaultColor.G,DefaultColor.B); break;
		case MP_BaseColor: Ret = Compiler->Constant3(DefaultColor.R,DefaultColor.G,DefaultColor.B); break;
		case MP_Metallic: Ret = Compiler->Constant(DefaultFloat); break;
		case MP_Specular: Ret = Compiler->Constant(DefaultFloat); break;
		case MP_Roughness: Ret = Compiler->Constant(DefaultFloat); break;
		case MP_Normal: Ret = Compiler->Constant3(DefaultVector.X,DefaultVector.Y,DefaultVector.Z); break;
		case MP_WorldPositionOffset: Ret = Compiler->Constant3(DefaultVector.X,DefaultVector.Y,DefaultVector.Z); break;
		case MP_WorldDisplacement : Ret = Compiler->Constant3(DefaultVector.X,DefaultVector.Y,DefaultVector.Z); break;
		case MP_TessellationMultiplier: Ret = Compiler->Constant(DefaultFloat); break;
		case MP_SubsurfaceColor: Ret = Compiler->Constant3(DefaultColor.R,DefaultColor.G,DefaultColor.B); break;
		case MP_AmbientOcclusion: Ret = Compiler->Constant(DefaultFloat); break;
		case MP_Refraction: Ret = Compiler->Constant2(DefaultVector.X,DefaultVector.Y); break;
		};

		if (Property >= MP_CustomizedUVs0 && Property <= MP_CustomizedUVs7)
		{
			const int32 TextureCoordinateIndex = Property - MP_CustomizedUVs0;
			// The user did not customize this UV, pass through the vertex texture coordinates
			Ret = Compiler->TextureCoordinate(TextureCoordinateIndex, false, false);
		}
	}

	return Ret;
}

EMaterialValueType GetMaterialPropertyType(EMaterialProperty Property)
{
	switch(Property)
	{
	case MP_EmissiveColor: return MCT_Float3;
	case MP_Opacity: return MCT_Float;
	case MP_OpacityMask: return MCT_Float;
	case MP_DiffuseColor: return MCT_Float3;
	case MP_SpecularColor: return MCT_Float3;
	case MP_BaseColor: return MCT_Float3;
	case MP_Metallic: return MCT_Float;
	case MP_Specular: return MCT_Float;
	case MP_Roughness: return MCT_Float;
	case MP_Normal: return MCT_Float3;
	case MP_WorldPositionOffset: return MCT_Float3;
	case MP_WorldDisplacement : return MCT_Float3;
	case MP_TessellationMultiplier: return MCT_Float;
	case MP_SubsurfaceColor: return MCT_Float3;
	case MP_AmbientOcclusion: return MCT_Float;
	case MP_Refraction: return MCT_Float;
	case MP_MaterialAttributes: return MCT_MaterialAttributes;
	};

	if (Property >= MP_CustomizedUVs0 && Property <= MP_CustomizedUVs7)
	{
		return MCT_Float2;
	}

	return MCT_Unknown;
}

/** Returns the shader frequency corresponding to the given material input. */
EShaderFrequency GetMaterialPropertyShaderFrequency(EMaterialProperty Property)
{
	if (Property == MP_WorldPositionOffset
		|| (Property >= MP_CustomizedUVs0 && Property <= MP_CustomizedUVs7))
	{
		return SF_Vertex;
	}
	else if(Property == MP_WorldDisplacement)
	{
		return SF_Domain;
	}
	else if(Property == MP_TessellationMultiplier)
	{
		return SF_Hull;
	}
	return SF_Pixel;
}

void FMaterialCompilationOutput::Serialize(FArchive& Ar)
{
	UniformExpressionSet.Serialize(Ar);

	if (Ar.UE4Ver() >= VER_UE4_PURGED_FMATERIAL_COMPILE_OUTPUTS)
	{
		Ar << bUsesSceneColor;
	}

	Ar << bNeedsSceneTextures;

	Ar << bUsesEyeAdaptation;

	Ar << bModifiesMeshPosition;
}

void FMaterial::GetShaderMapId(EShaderPlatform Platform, FMaterialShaderMapId& OutId) const
{ 
	TArray<FShaderType*> ShaderTypes;
	TArray<FVertexFactoryType*> VFTypes;

	GetDependentShaderAndVFTypes(Platform, ShaderTypes, VFTypes);

	OutId.Usage = GetShaderMapUsage();
	OutId.BaseMaterialId = GetMaterialId();
	OutId.QualityLevel = GetQualityLevelForShaderMapId();
	OutId.FeatureLevel = GetFeatureLevel();
	OutId.SetShaderDependencies(ShaderTypes, VFTypes);
	GetReferencedTexturesHash(OutId.TextureReferencesHash);
}

EMaterialTessellationMode FMaterial::GetTessellationMode() const 
{ 
	return MTM_NoTessellation; 
};

void FMaterial::FinishCompilation()
{
	TArray<int32> ShaderMapIdsToFinish;

	// Build an array of the shader map Id's that we need to be compiled
	if (GameThreadShaderMap && !GameThreadShaderMap->IsCompilationFinalized())
	{
		ShaderMapIdsToFinish.Add(GameThreadShaderMap->GetCompilingId());
	}
	else if (OutstandingCompileShaderMapId != INDEX_NONE)
	{
		ShaderMapIdsToFinish.Add(OutstandingCompileShaderMapId);
	}

	if (ShaderMapIdsToFinish.Num() > 0)
	{
		// Block until the shader maps that we will save have finished being compiled
		GShaderCompilingManager->FinishCompilation(*GetFriendlyName(), ShaderMapIdsToFinish);
	}
}

const TArray<TRefCountPtr<FMaterialUniformExpressionTexture> >& FMaterial::GetUniform2DTextureExpressions() const 
{ 
	const FMaterialShaderMap* ShaderMapToUse = NULL;

	if (IsInGameThread())
	{
		// If we are accessing uniform texture expressions on the game thread, use results from a shader map whose compile is in flight that matches this material
		// This allows querying what textures a material uses even when it is being asynchronously compiled
		ShaderMapToUse = GetGameThreadShaderMap() ? GetGameThreadShaderMap() : FMaterialShaderMap::GetShaderMapBeingCompiled(this);
	}
	else 
	{
		checkSlow(IsInRenderingThread());
		ShaderMapToUse = GetRenderingThreadShaderMap();
	}

	if (ShaderMapToUse)
	{
		return ShaderMapToUse->GetUniformExpressionSet().Uniform2DTextureExpressions; 
	}
	
	static const TArray<TRefCountPtr<FMaterialUniformExpressionTexture> > EmptyExpressions;
	return EmptyExpressions;
}

const TArray<TRefCountPtr<FMaterialUniformExpressionTexture> >& FMaterial::GetUniformCubeTextureExpressions() const 
{ 
	const FMaterialShaderMap* ShaderMapToUse = NULL;

	if (IsInGameThread())
	{
		// If we are accessing uniform texture expressions on the game thread, use results from a shader map whose compile is in flight that matches this material
		// This allows querying what textures a material uses even when it is being asynchronously compiled
		ShaderMapToUse = GetGameThreadShaderMap() ? GetGameThreadShaderMap() : FMaterialShaderMap::GetShaderMapBeingCompiled(this);
	}
	else 
	{
		checkSlow(IsInRenderingThread());
		ShaderMapToUse = GetRenderingThreadShaderMap();
}

	if (ShaderMapToUse)
	{
		return ShaderMapToUse->GetUniformExpressionSet().UniformCubeTextureExpressions; 
	}

	static const TArray<TRefCountPtr<FMaterialUniformExpressionTexture> > EmptyExpressions;
	return EmptyExpressions;
}

bool FMaterial::UsesSceneColor() const 
{
	if (IsInGameThread() && GameThreadShaderMap)
	{
		return GameThreadShaderMap->UsesSceneColor();
	}
	else if (IsInRenderingThread() && RenderingThreadShaderMap)
	{
		return RenderingThreadShaderMap->UsesSceneColor();
	}

	return false;
}

bool FMaterial::NeedsSceneTextures() const 
{
	checkSlow(IsInRenderingThread());

	if (RenderingThreadShaderMap)
	{
		return RenderingThreadShaderMap->NeedsSceneTextures();
	}
	
	return false;
}

bool FMaterial::UsesEyeAdaptation() const 
{
	checkSlow(IsInRenderingThread());

	if (RenderingThreadShaderMap)
	{
		return RenderingThreadShaderMap->UsesEyeAdaptation();
	}

	return false;
}

bool FMaterial::MaterialModifiesMeshPosition() const 
{ 
	FMaterialShaderMap* ShaderMap = IsInRenderingThread() ? RenderingThreadShaderMap : GameThreadShaderMap.GetReference();
	bool bUsesWPO = ShaderMap ? ShaderMap->ModifiesMeshPosition() : false;

	return bUsesWPO || GetTessellationMode() != MTM_NoTessellation;
}

bool FMaterial::MaterialMayModifyMeshPosition() const
{
	// Conservative estimate when called before material translation has occurred. 
    // This function is only intended for use in deciding whether or not shader permutations are required.
	return HasVertexPositionOffsetConnected() || HasMaterialAttributesConnected() || GetTessellationMode() != MTM_NoTessellation;
}

FMaterialShaderMap* FMaterial::GetRenderingThreadShaderMap() const 
{ 
	checkSlow(IsInRenderingThread());
	return RenderingThreadShaderMap; 
}

void FMaterial::SetRenderingThreadShaderMap(FMaterialShaderMap* InMaterialShaderMap)
{
	checkSlow(IsInRenderingThread());
	RenderingThreadShaderMap = InMaterialShaderMap;
}

void FMaterial::AddReferencedObjects(FReferenceCollector& Collector)
{
	for (int32 ExpressionIndex = 0; ExpressionIndex < ErrorExpressions.Num(); ExpressionIndex++)
	{
		Collector.AddReferencedObject(ErrorExpressions[ExpressionIndex]);
	}
}

struct FLegacyTextureLookup
{
	void Serialize(FArchive& Ar)
	{
		Ar << TexCoordIndex;
		Ar << TextureIndex;
		Ar << UScale;
		Ar << VScale;
	}

	int32 TexCoordIndex;
	int32 TextureIndex;	

	float UScale;
	float VScale;
};

FArchive& operator<<(FArchive& Ar, FLegacyTextureLookup& Ref)
{
	Ref.Serialize( Ar );
	return Ar;
}

void FMaterial::LegacySerialize(FArchive& Ar)
{
	if (Ar.UE4Ver() < VER_UE4_PURGED_FMATERIAL_COMPILE_OUTPUTS)
	{
		TArray<FString> LegacyStrings;
		Ar << LegacyStrings;

		TMap<UMaterialExpression*,int32> LegacyMap;
		Ar << LegacyMap;
		int32 LegacyInt;
		Ar << LegacyInt;

		FeatureLevel = ERHIFeatureLevel::SM4;
		QualityLevel = EMaterialQualityLevel::High;
		Ar << Id_DEPRECATED;

		if (Ar.UE4Ver() < VER_UE4_REMOVED_FMATERIAL_COMPILE_OUTPUTS)
		{
			uint32 Temp = 0;
			Ar << Temp;
		}

		TArray<UTexture*> LegacyTextures;
		Ar << LegacyTextures;

		bool bTemp2;
		Ar << bTemp2;

		if (Ar.UE4Ver() < VER_UE4_REMOVED_FMATERIAL_COMPILE_OUTPUTS)
		{
			bool bUsesLightmapUVsTemp = false;
			Ar << bUsesLightmapUVsTemp;

			bool bUsesMaterialVertexPositionOffsetTemp = false;
			Ar << bUsesMaterialVertexPositionOffsetTemp;

			bool bUsesSphericalParticleOpacityTemp = false;
			Ar << bUsesSphericalParticleOpacityTemp;
		}

		bool bTemp;
		Ar << bTemp;

		TArray<FLegacyTextureLookup> LegacyLookups;
		Ar << LegacyLookups;

		uint32 DummyDroppedFallbackComponents = 0;
		Ar << DummyDroppedFallbackComponents;
	}

	SerializeInlineShaderMap(Ar);
}

void FMaterial::SerializeInlineShaderMap(FArchive& Ar)
{
	bool bCooked = false;

	if (Ar.UE4Ver() >= VER_UE4_INLINE_SHADERS)
	{
		bCooked = Ar.IsCooking();
		Ar << bCooked;
	}

	if (FPlatformProperties::RequiresCookedData() && !bCooked && Ar.IsLoading())
	{
		UE_LOG(LogShaders, Fatal, TEXT("This platform requires cooked packages, and shaders were not cooked into this material %s."), *GetFriendlyName());
	}

	if (bCooked)
	{
		if (Ar.IsCooking())
		{
			FinishCompilation();

			bool bValid = GameThreadShaderMap != NULL && GameThreadShaderMap->CompiledSuccessfully();

			Ar << bValid;

			if (bValid)
			{
				GameThreadShaderMap->Serialize(Ar);
			}
		}
		else
		{
			bool bValid = false;
			Ar << bValid;

			if (bValid)
			{
				TRefCountPtr<FMaterialShaderMap> LoadedShaderMap = new FMaterialShaderMap();
				LoadedShaderMap->Serialize(Ar);

				// Toss the loaded shader data if this is a server only instance
				//@todo - don't cook it in the first place
				if (FApp::CanEverRender())
				{
				GameThreadShaderMap = RenderingThreadShaderMap = LoadedShaderMap;
			}
		}
	}
}
}

void FMaterialResource::LegacySerialize(FArchive& Ar)
{
	FMaterial::LegacySerialize(Ar);

	if (Ar.UE4Ver() < VER_UE4_PURGED_FMATERIAL_COMPILE_OUTPUTS
		&& Ar.UE4Ver() >= VER_UE4_MATERIAL_BLEND_OVERRIDE)
	{
		int32 BlendModeOverrideValueTemp = 0;
		Ar << BlendModeOverrideValueTemp;
		bool bDummyBool = false;
		Ar << bDummyBool;
		Ar << bDummyBool;
	}
}

const TArray<UTexture*>& FMaterialResource::GetReferencedTextures() const
{
	return Material->ExpressionTextureReferences;
}

bool FMaterialResource::GetAllowDevelopmentShaderCompile()const
{
	return Material->bAllowDevelopmentShaderCompile;
}

void FMaterial::ReleaseShaderMap()
{
	if (GameThreadShaderMap)
	{
		GameThreadShaderMap = NULL;
		
		ENQUEUE_UNIQUE_RENDER_COMMAND_ONEPARAMETER(
			ReleaseShaderMap,
			FMaterial*,Material,this,
		{
			Material->SetRenderingThreadShaderMap(NULL);
		});
	}
}

bool IsTranslucentBlendMode(EBlendMode BlendMode)
{
	return BlendMode != BLEND_Opaque && BlendMode != BLEND_Masked;
}

int32 FMaterialResource::GetMaterialDomain() const { return Material->MaterialDomain; }
bool FMaterialResource::IsTangentSpaceNormal() const { return Material->bTangentSpaceNormal || Material->Normal.Expression == NULL; }
bool FMaterialResource::ShouldInjectEmissiveIntoLPV() const { return Material->bUseEmissiveForDynamicAreaLighting; }
bool FMaterialResource::ShouldGenerateSphericalParticleNormals() const { return Material->bGenerateSphericalParticleNormals; }
bool FMaterialResource::ShouldDisableDepthTest() const { return Material->bDisableDepthTest; }
bool FMaterialResource::ShouldEnableResponsiveAA() const { return Material->bEnableResponsiveAA; }
bool FMaterialResource::IsWireframe() const { return Material->Wireframe; }
bool FMaterialResource::IsLightFunction() const { return Material->MaterialDomain == MD_LightFunction; }
bool FMaterialResource::IsUsedWithEditorCompositing() const { return Material->bUsedWithEditorCompositing; }
bool FMaterialResource::IsUsedWithDeferredDecal() const { return Material->MaterialDomain == MD_DeferredDecal; }
bool FMaterialResource::IsSpecialEngineMaterial() const { return Material->bUsedAsSpecialEngineMaterial; }
bool FMaterialResource::HasVertexPositionOffsetConnected() const { return !Material->bUseMaterialAttributes && Material->WorldPositionOffset.Expression != NULL; }
bool FMaterialResource::HasMaterialAttributesConnected() const { return Material->bUseMaterialAttributes && Material->MaterialAttributes.Expression != NULL; }
FString FMaterialResource::GetBaseMaterialPathName() const { return Material->GetPathName(); }

bool FMaterialResource::IsUsedWithSkeletalMesh() const
{
	return Material->bUsedWithSkeletalMesh;
}

bool FMaterialResource::IsUsedWithLandscape() const
{
	return Material->bUsedWithLandscape;
}

bool FMaterialResource::IsUsedWithParticleSystem() const
{
	return Material->bUsedWithParticleSprites || Material->bUsedWithBeamTrails;
}

bool FMaterialResource::IsUsedWithParticleSprites() const
{
	return Material->bUsedWithParticleSprites;
}

bool FMaterialResource::IsUsedWithBeamTrails() const
{
	return Material->bUsedWithBeamTrails;
}

bool FMaterialResource::IsUsedWithMeshParticles() const
{
	return Material->bUsedWithMeshParticles;
}

bool FMaterialResource::IsUsedWithStaticLighting() const
{
	return Material->bUsedWithStaticLighting;
}

bool FMaterialResource::IsUsedWithMorphTargets() const
{
	return Material->bUsedWithMorphTargets;
}

bool FMaterialResource::IsUsedWithSplineMeshes() const
{
	return Material->bUsedWithSplineMeshes;
}

bool FMaterialResource::IsUsedWithInstancedStaticMeshes() const
{
	return Material->bUsedWithInstancedStaticMeshes;
}

bool FMaterialResource::IsUsedWithAPEXCloth() const
{
	return Material->bUsedWithClothing;
}

EMaterialTessellationMode FMaterialResource::GetTessellationMode() const 
{ 
	return (EMaterialTessellationMode)Material->D3D11TessellationMode; 
}

bool FMaterialResource::IsCrackFreeDisplacementEnabled() const 
{ 
	return Material->bEnableCrackFreeDisplacement;
}

bool FMaterialResource::IsSeparateTranslucencyEnabled() const 
{ 
	return Material->bEnableSeparateTranslucency;
}

bool FMaterialResource::IsAdaptiveTessellationEnabled() const
{
	return Material->bEnableAdaptiveTessellation;
}

bool FMaterialResource::IsFullyRough() const
{
	return Material->bFullyRough;
}

bool FMaterialResource::UseLmDirectionality() const
{
	return Material->bUseLightmapDirectionality;
}

/**
 * Should shaders compiled for this material be saved to disk?
 */
bool FMaterialResource::IsPersistent() const { return true; }

FGuid FMaterialResource::GetMaterialId() const
{
	return Material->StateId;
}

ETranslucencyLightingMode FMaterialResource::GetTranslucencyLightingMode() const { return (ETranslucencyLightingMode)Material->TranslucencyLightingMode; }

float FMaterialResource::GetOpacityMaskClipValue() const 
{
	float Ret;
	if( !(MaterialInstance && MaterialInstance->GetOpacityMaskClipValueOverride(Ret)) )
	{
		Ret = Material->OpacityMaskClipValue;
	}
	return Ret;
}

EBlendMode FMaterialResource::GetBlendMode() const 
{
	EBlendMode Ret;
	if( !(MaterialInstance && MaterialInstance->GetBlendModeOverride(Ret)) )
	{
		Ret = Material->BlendMode;
	}
	return Ret;
}

EMaterialLightingModel FMaterialResource::GetLightingModel() const 
{ 
	EMaterialLightingModel Ret;
	if( !(MaterialInstance && MaterialInstance->GetLightingModelOverride(Ret)) )
	{
		Ret = Material->GetLightingModel_Internal();
	}
	return Ret;
}

bool FMaterialResource::IsTwoSided() const 
{
	bool Ret;
	if( !(MaterialInstance && MaterialInstance->IsTwoSidedOverride(Ret)) )
	{
		Ret = Material->TwoSided != 0;
	}
	return Ret;
}

bool FMaterialResource::IsDistorted() const { return Material->bUsesDistortion; }
float FMaterialResource::GetTranslucencyDirectionalLightingIntensity() const { return Material->TranslucencyDirectionalLightingIntensity; }
float FMaterialResource::GetTranslucentShadowDensityScale() const { return Material->TranslucentShadowDensityScale; }
float FMaterialResource::GetTranslucentSelfShadowDensityScale() const { return Material->TranslucentSelfShadowDensityScale; }
float FMaterialResource::GetTranslucentSelfShadowSecondDensityScale() const { return Material->TranslucentSelfShadowSecondDensityScale; }
float FMaterialResource::GetTranslucentSelfShadowSecondOpacity() const { return Material->TranslucentSelfShadowSecondOpacity; }
float FMaterialResource::GetTranslucentBackscatteringExponent() const { return Material->TranslucentBackscatteringExponent; }
FLinearColor FMaterialResource::GetTranslucentMultipleScatteringExtinction() const { return Material->TranslucentMultipleScatteringExtinction; }
float FMaterialResource::GetTranslucentShadowStartOffset() const { return Material->TranslucentShadowStartOffset; }
float FMaterialResource::GetRefractionDepthBiasValue() const { return Material->RefractionDepthBias; }
bool FMaterialResource::UseTranslucencyVertexFog() const {return Material->bUseTranslucencyVertexFog;}
/**
 * Check if the material is masked and uses an expression or a constant that's not 1.0f for opacity.
 * @return true if the material uses opacity
 */
bool FMaterialResource::IsMasked() const { return Material->bIsMasked; }

FString FMaterialResource::GetFriendlyName() const { return *GetNameSafe(Material); }

uint32 FMaterialResource::GetDecalBlendMode() const
{
	return Material->GetDecalBlendMode();
}

uint32 FMaterialResource::GetMaterialDecalResponse() const
{
	return Material->GetMaterialDecalResponse();
}

bool FMaterialResource::HasNormalConnected() const
{
	return Material->HasNormalConnected();
}

bool FMaterialResource::RequiresSynchronousCompilation() const
{
	return Material->IsDefaultMaterial();
}

bool FMaterialResource::IsDefaultMaterial() const
{
	return Material->IsDefaultMaterial();
}

void FMaterialResource::NotifyCompilationFinished()
{
	Material->NotifyCompilationFinished(this);
}

/**
 * Gets instruction counts that best represent the likely usage of this material based on lighting model and other factors.
 * @param Descriptions - an array of descriptions to be populated
 * @param InstructionCounts - an array of instruction counts matching the Descriptions.  
 *		The dimensions of these arrays are guaranteed to be identical and all values are valid.
 */
void FMaterialResource::GetRepresentativeInstructionCounts(TArray<FString> &Descriptions, TArray<int32> &InstructionCounts) const
{
	TArray<FString> ShaderTypeNames;
	TArray<FString> ShaderTypeDescriptions;

	//when adding a shader type here be sure to update FPreviewMaterial::ShouldCache()
	//so the shader type will get compiled with preview materials
	const FMaterialShaderMap* MaterialShaderMap = GetGameThreadShaderMap();
	if (MaterialShaderMap && MaterialShaderMap->IsCompilationFinalized())
	{
		GetRepresentativeShaderTypesAndDescriptions(ShaderTypeNames, ShaderTypeDescriptions);

		const FMeshMaterialShaderMap* MeshShaderMap = MaterialShaderMap->GetMeshShaderMap(&FLocalVertexFactory::StaticType);
		if (MeshShaderMap)
		{
			Descriptions.Empty();
			InstructionCounts.Empty();

			for (int32 InstructionIndex = 0; InstructionIndex < ShaderTypeNames.Num(); InstructionIndex++)
			{
				FShaderType* ShaderType = FindShaderTypeByName(*ShaderTypeNames[InstructionIndex]);
				if (ShaderType)
				{
					const FShader* Shader = MeshShaderMap->GetShader(ShaderType);
					if (Shader && Shader->GetNumInstructions() > 0)
					{
						//if the shader was found, add it to the output arrays
						InstructionCounts.Push(Shader->GetNumInstructions());
						Descriptions.Push(ShaderTypeDescriptions[InstructionIndex]);
					}
				}
			}
		}
	}

	check(Descriptions.Num() == InstructionCounts.Num());
}

void FMaterialResource::GetRepresentativeShaderTypesAndDescriptions(TArray<FString> &ShaderTypeNames, TArray<FString> &ShaderTypeDescriptions) const
{
	static auto* MobileHDR = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.MobileHDR"));
	bool bMobileHDR = MobileHDR && MobileHDR->GetValueOnAnyThread() == 1;

	if (GetFeatureLevel() > ERHIFeatureLevel::ES2)
	{
		if (GetLightingModel() == MLM_Unlit)
		{
			//unlit materials are never lightmapped
			new (ShaderTypeNames)FString(TEXT("TBasePassPSFNoLightMapPolicy"));
			new (ShaderTypeDescriptions)FString(TEXT("Base pass shader without light map"));
		}
		else
		{
			if (IsUsedWithStaticLighting())
			{
				//lit materials are usually lightmapped
				new (ShaderTypeNames)FString(TEXT("TBasePassPSTDistanceFieldShadowsAndLightMapPolicyHQ"));
				new (ShaderTypeDescriptions)FString(TEXT("Base pass shader with static lighting"));
			}

			//also show a dynamically lit shader
			new (ShaderTypeNames)FString(TEXT("TBasePassPSFNoLightMapPolicy"));
			new (ShaderTypeDescriptions)FString(TEXT("Base pass shader with only dynamic lighting"));

			if (IsTranslucentBlendMode(GetBlendMode()))
			{
				new (ShaderTypeNames)FString(TEXT("TBasePassPSFSelfShadowedTranslucencyPolicy"));
				new (ShaderTypeDescriptions)FString(TEXT("Base pass shader for self shadowed translucency"));
			}
		}

		new (ShaderTypeNames)FString(TEXT("TBasePassVSFNoLightMapPolicy"));
		new (ShaderTypeDescriptions)FString(TEXT("Vertex shader"));
	}
	else
	{
		const TCHAR* ShaderSuffix = bMobileHDR ? TEXT("HDRLinear64") : TEXT("LDRGamma32");
		const TCHAR* DescSuffix = bMobileHDR ? TEXT(" (HDR)") : TEXT(" (LDR)");

		if (GetLightingModel() == MLM_Unlit)
		{
			//unlit materials are never lightmapped
			new (ShaderTypeNames)FString(FString::Printf(TEXT("TBasePassForForwardShadingPSFNoLightMapPolicy%s"), ShaderSuffix));
			new (ShaderTypeDescriptions)FString(FString::Printf(TEXT("Mobile base pass shader without light map%s"), DescSuffix));
		}
		else
		{
			if (IsUsedWithStaticLighting())
			{
				//lit materials are usually lightmapped
				new (ShaderTypeNames)FString(FString::Printf(TEXT("TBasePassForForwardShadingPSTDistanceFieldShadowsAndLightMapPolicyLQ%s"), ShaderSuffix));
				new (ShaderTypeDescriptions)FString(FString::Printf(TEXT("Mobile base pass shader with static lighting%s"), DescSuffix));
			}

			//also show a dynamically lit shader
			new (ShaderTypeNames)FString(FString::Printf(TEXT("TBasePassForForwardShadingPSFSimpleDirectionalLightAndSHIndirectPolicy%s"), ShaderSuffix));
			new (ShaderTypeDescriptions)FString(FString::Printf(TEXT("Mobile base pass shader with only dynamic lighting%s"), DescSuffix));
		}

		new (ShaderTypeNames)FString(FString::Printf(TEXT("TBasePassForForwardShadingVSFNoLightMapPolicy%s"), ShaderSuffix));
		new (ShaderTypeDescriptions)FString(FString::Printf(TEXT("Mobile base pass vertex shader%s"), DescSuffix));
	}
}

SIZE_T FMaterialResource::GetResourceSizeInclusive()
{
	SIZE_T ResourceSize = 0;
	TSet<const FMaterialShaderMap*> UniqueShaderMaps;
	TMap<FShaderId,FShader*> UniqueShaders;
	TSet<FShaderResourceId> UniqueShaderResourceIds;

	ResourceSize += sizeof(FMaterialResource);
	UniqueShaderMaps.Add(GetGameThreadShaderMap());

	for (TSet<const FMaterialShaderMap*>::TConstIterator It(UniqueShaderMaps); It; ++It)
	{
		const FMaterialShaderMap* MaterialShaderMap = *It;
		if (MaterialShaderMap)
		{
			ResourceSize += MaterialShaderMap->GetSizeBytes();
			MaterialShaderMap->GetShaderList(UniqueShaders);
		}
	}

	for (TMap<FShaderId,FShader*>::TConstIterator It(UniqueShaders); It; ++It)
	{
		FShader* Shader = It.Value();
		if (Shader)
		{
			FShaderResourceId ResourceId = Shader->GetResourceId();
			bool bCountedResource = false;
			UniqueShaderResourceIds.Add(ResourceId,&bCountedResource);
			if (!bCountedResource)
			{
				ResourceSize += Shader->GetResourceSizeBytes();
			}
			ResourceSize += Shader->GetSizeBytes();
		}
	}

	return ResourceSize;
}

/**
 * Destructor
 */
FMaterial::~FMaterial()
{
	if (GIsEditor)
	{
		const FSetElementId FoundId = EditorLoadedMaterialResources.FindId(this);
		if (FoundId.IsValidId())
		{
			// Remove the material from EditorLoadedMaterialResources if found
			EditorLoadedMaterialResources.Remove(FoundId);
		}
	}

	FMaterialShaderMap::RemovePendingMaterial(this);
}

/** Populates OutEnvironment with defines needed to compile shaders for this material. */
void FMaterial::SetupMaterialEnvironment(
	EShaderPlatform Platform,
	const FUniformExpressionSet& InUniformExpressionSet,
	FShaderCompilerEnvironment& OutEnvironment
	) const
{
	// Add the material uniform buffer definition.
	FShaderUniformBufferParameter::ModifyCompilationEnvironment(TEXT("Material"),InUniformExpressionSet.GetUniformBufferStruct(),Platform,OutEnvironment);

	if ((RHISupportsTessellation(Platform) == false) || (GetTessellationMode() == MTM_NoTessellation))
	{
		OutEnvironment.SetDefine(TEXT("USING_TESSELLATION"),TEXT("0"));
	}
	else
	{
		OutEnvironment.SetDefine(TEXT("USING_TESSELLATION"),TEXT("1"));
		if (GetTessellationMode() == MTM_FlatTessellation)
		{
			OutEnvironment.SetDefine(TEXT("TESSELLATION_TYPE_FLAT"),TEXT("1"));
		}
		else if (GetTessellationMode() == MTM_PNTriangles)
		{
			OutEnvironment.SetDefine(TEXT("TESSELLATION_TYPE_PNTRIANGLES"),TEXT("1"));
		}

		// This is dominant vertex/edge information.  Note, mesh must have preprocessed neighbors IB of material will fallback to default.
		//  PN triangles needs preprocessed buffers regardless of c
		if (IsCrackFreeDisplacementEnabled())
		{
			OutEnvironment.SetDefine(TEXT("DISPLACEMENT_ANTICRACK"),TEXT("1"));
		}
		else
		{
			OutEnvironment.SetDefine(TEXT("DISPLACEMENT_ANTICRACK"),TEXT("0"));
		}

		// Set whether to enable the adaptive tessellation, which tries to maintain a uniform number of pixels per triangle.
		if (IsAdaptiveTessellationEnabled())
		{
			OutEnvironment.SetDefine(TEXT("USE_ADAPTIVE_TESSELLATION_FACTOR"),TEXT("1"));
		}
		else
		{
			OutEnvironment.SetDefine(TEXT("USE_ADAPTIVE_TESSELLATION_FACTOR"),TEXT("0"));
		}
		
	}

	switch(GetBlendMode())
	{
	case BLEND_Opaque: OutEnvironment.SetDefine(TEXT("MATERIALBLENDING_SOLID"),TEXT("1")); break;
	case BLEND_Masked:
		{
			// Only set MATERIALBLENDING_MASKED if the material is truly masked
			//@todo - this may cause mismatches with what the shader compiles and what the renderer thinks the shader needs
			// For example IsTranslucentBlendMode doesn't check IsMasked
			if(IsMasked())
			{
				OutEnvironment.SetDefine(TEXT("MATERIALBLENDING_MASKED"),TEXT("1"));
			}
			else
			{
				OutEnvironment.SetDefine(TEXT("MATERIALBLENDING_SOLID"),TEXT("1"));
			}
			break;
		}
	case BLEND_Translucent: OutEnvironment.SetDefine(TEXT("MATERIALBLENDING_TRANSLUCENT"),TEXT("1")); break;
	case BLEND_Additive: OutEnvironment.SetDefine(TEXT("MATERIALBLENDING_ADDITIVE"),TEXT("1")); break;
	case BLEND_Modulate: OutEnvironment.SetDefine(TEXT("MATERIALBLENDING_MODULATE"),TEXT("1")); break;
	default: 
		UE_LOG(LogMaterial, Warning, TEXT("Unknown material blend mode: %u  Setting to BLEND_Opaque"),(int32)GetBlendMode());
		OutEnvironment.SetDefine(TEXT("MATERIALBLENDING_SOLID"),TEXT("1"));
	}

	{
		EMaterialDecalResponse MaterialDecalResponse = (EMaterialDecalResponse)GetMaterialDecalResponse();

		// bit 0:color/1:normal/2:roughness to enable/disable parts of the DBuffer decal effect
		int32 MaterialDecalResponseMask = 0;

		switch(MaterialDecalResponse)
		{
			case MDR_None:					MaterialDecalResponseMask = 0; break;
			case MDR_ColorNormalRoughness:	MaterialDecalResponseMask = 1 + 2 + 4; break;
			case MDR_Color:					MaterialDecalResponseMask = 1; break;
			case MDR_ColorNormal:			MaterialDecalResponseMask = 1 + 2; break;
			case MDR_ColorRoughness:		MaterialDecalResponseMask = 1 + 4; break;
			case MDR_Normal:				MaterialDecalResponseMask = 2; break;
			case MDR_NormalRoughness:		MaterialDecalResponseMask = 2 + 4; break;
			case MDR_Roughness:				MaterialDecalResponseMask = 4; break;
			default:
				check(0);
		}

		OutEnvironment.SetDefine(TEXT("MATERIALDECALRESPONSEMASK"), MaterialDecalResponseMask);
	}

	OutEnvironment.SetDefine(TEXT("MATERIAL_TWOSIDED"), IsTwoSided() ? TEXT("1") : TEXT("0"));
	OutEnvironment.SetDefine(TEXT("MATERIAL_TANGENTSPACENORMAL"), IsTangentSpaceNormal() ? TEXT("1") : TEXT("0"));
	OutEnvironment.SetDefine(TEXT("GENERATE_SPHERICAL_PARTICLE_NORMALS"),ShouldGenerateSphericalParticleNormals() ? TEXT("1") : TEXT("0"));
	OutEnvironment.SetDefine(TEXT("MATERIAL_USES_SCENE_COLOR"), UsesSceneColor() ? TEXT("1") : TEXT("0"));
	OutEnvironment.SetDefine(TEXT("MATERIAL_FULLY_ROUGH"), IsFullyRough() ? TEXT("1") : TEXT("0"));
	OutEnvironment.SetDefine(TEXT("MATERIAL_USE_LM_DIRECTIONALITY"), UseLmDirectionality() ? TEXT("1") : TEXT("0"));
	OutEnvironment.SetDefine(TEXT("MATERIAL_INJECT_EMISSIVE_INTO_LPV"), ShouldInjectEmissiveIntoLPV() ? TEXT("1") : TEXT("0"));

	switch(GetLightingModel())
	{
	case MLM_DefaultLit: OutEnvironment.SetDefine(TEXT("MATERIAL_LIGHTINGMODEL_DEFAULT_LIT"),TEXT("1")); break;
	case MLM_Subsurface: OutEnvironment.SetDefine(TEXT("MATERIAL_LIGHTINGMODEL_SUBSURFACE"),TEXT("1")); break;
	case MLM_PreintegratedSkin: OutEnvironment.SetDefine(TEXT("MATERIAL_LIGHTINGMODEL_PREINTEGRATED_SKIN"),TEXT("1")); break;
	case MLM_Unlit: OutEnvironment.SetDefine(TEXT("MATERIAL_LIGHTINGMODEL_UNLIT"),TEXT("1")); break;
	default: 
		UE_LOG(LogMaterial, Warning, TEXT("Unknown material lighting model: %u  Setting to MLM_DefaultLit"),(int32)GetLightingModel());
		OutEnvironment.SetDefine(TEXT("MATERIAL_LIGHTINGMODEL_DEFAULT_LIT"),TEXT("1"));
	
	};

	if (IsTranslucentBlendMode(GetBlendMode()))
	{
		switch(GetTranslucencyLightingMode())
		{
		case TLM_VolumetricNonDirectional: OutEnvironment.SetDefine(TEXT("TRANSLUCENCY_LIGHTING_VOLUMETRIC_NONDIRECTIONAL"),TEXT("1")); break;
		case TLM_VolumetricDirectional: OutEnvironment.SetDefine(TEXT("TRANSLUCENCY_LIGHTING_VOLUMETRIC_DIRECTIONAL"),TEXT("1")); break;
		case TLM_Surface: OutEnvironment.SetDefine(TEXT("TRANSLUCENCY_LIGHTING_SURFACE"),TEXT("1")); break;
		default: 
			UE_LOG(LogMaterial, Warning, TEXT("Unknown lighting mode: %u"),(int32)GetTranslucencyLightingMode());
			OutEnvironment.SetDefine(TEXT("TRANSLUCENCY_LIGHTING_VOLUMETRIC_NONDIRECTIONAL"),TEXT("1")); break;
		};
	}

	if( IsUsedWithEditorCompositing() )
	{
		OutEnvironment.SetDefine(TEXT("EDITOR_PRIMITIVE_MATERIAL"),TEXT("1"));
	}
}

/**
 * Caches the material shaders for this material with no static parameters on the given platform.
 * This is used by material resources of UMaterials.
 */
bool FMaterial::CacheShaders(EShaderPlatform Platform, bool bApplyCompletedShaderMapForRendering)
{
	FMaterialShaderMapId NoStaticParametersId;
	GetShaderMapId(Platform, NoStaticParametersId);
	return CacheShaders(NoStaticParametersId, Platform, bApplyCompletedShaderMapForRendering);
}

/**
 * Caches the material shaders for the given static parameter set and platform.
 * This is used by material resources of UMaterialInstances.
 */
bool FMaterial::CacheShaders(const FMaterialShaderMapId& ShaderMapId, EShaderPlatform Platform, bool bApplyCompletedShaderMapForRendering)
{
	bool bSucceeded = false;
	OutstandingCompileShaderMapId = INDEX_NONE;

	check(ShaderMapId.BaseMaterialId.IsValid());

	// If we loaded this material with inline shaders, use what was loaded (GameThreadShaderMap) instead of looking in the DDC
	if (bContainsInlineShaders)
	{
		FMaterialShaderMap* ExistingShaderMap = NULL;
		
		if (GameThreadShaderMap)
		{
			// Note: in the case of an inlined shader map, the shadermap Id will not be valid because we stripped some editor-only data needed to create it
			// Get the shadermap Id from the shadermap that was inlined into the package, if it exists
			ExistingShaderMap = FMaterialShaderMap::FindId(GameThreadShaderMap->GetShaderMapId(), Platform);
		}

		// Re-use an identical shader map in memory if possible, removing the reference to the inlined shader map
		if (ExistingShaderMap)
		{
			GameThreadShaderMap = ExistingShaderMap;
		}
		else if (GameThreadShaderMap)
		{
			// We are going to use the inlined shader map, register it so it can be re-used by other materials
			GameThreadShaderMap->Register();
		}
	}
	else
	{
		// Find the material's cached shader map.
		GameThreadShaderMap = FMaterialShaderMap::FindId(ShaderMapId, Platform);

		// Attempt to load from the derived data cache if we are uncooked
		if ((!GameThreadShaderMap || !GameThreadShaderMap->IsComplete(this, true)) && !FPlatformProperties::RequiresCookedData())
		{
			FMaterialShaderMap::LoadFromDerivedDataCache(this, ShaderMapId, Platform, GameThreadShaderMap);
		}
	}

	bool bRequiredRecompile = false;
	if (!GameThreadShaderMap || !GameThreadShaderMap->IsComplete(this, true))
	{
		if (bContainsInlineShaders || FPlatformProperties::RequiresCookedData())
		{
			if (IsSpecialEngineMaterial())
			{
				//assert if the default material's shader map was not found, since it will cause problems later
				UE_LOG(LogMaterial, Fatal,TEXT("Failed to find shader map for default material %s!  Please make sure cooking was successful."), *GetFriendlyName());
			}
			else
			{
				UE_LOG(LogMaterial, Log, TEXT("Can't compile %s with cooked content, will use default material instead"), *GetFriendlyName());
			}

			// Reset the shader map so the default material will be used.
			GameThreadShaderMap = NULL;
		}
		else
		{
			const TCHAR* ShaderMapCondition;
			if (GameThreadShaderMap)
			{
				ShaderMapCondition = TEXT("Incomplete");
			}
			else
			{
				ShaderMapCondition = TEXT("Missing");
			}
			UE_LOG(LogMaterial, Log, TEXT("%s cached shader map for material %s, compiling."),ShaderMapCondition,*GetFriendlyName());

			// If there's no cached shader map for this material, compile a new one.
			// This is just kicking off the async compile, GameThreadShaderMap will not be complete yet
			bSucceeded = BeginCompileShaderMap(ShaderMapId, Platform, GameThreadShaderMap, bApplyCompletedShaderMapForRendering);
			bRequiredRecompile = true;

			if (!bSucceeded)
			{
				// If it failed to compile the material, reset the shader map so the material isn't used.
				GameThreadShaderMap = NULL;

				if (IsDefaultMaterial())
				{
					for (int32 ErrorIndex = 0; ErrorIndex < CompileErrors.Num(); ErrorIndex++)
					{
						// Always log material errors in an unsuppressed category
						UE_LOG(LogMaterial, Warning, TEXT("	%s"), *CompileErrors[ErrorIndex]);
					}

					// Assert if the default material could not be compiled, since there will be nothing for other failed materials to fall back on.
					UE_LOG(LogMaterial, Fatal,TEXT("Failed to compile default material %s!"), *GetFriendlyName());
				}
			}
		}
	}
	else
	{
		bSucceeded = true;
	}

	RenderingThreadShaderMap = GameThreadShaderMap;

	return bSucceeded;
}

/**
* Compiles this material for Platform, storing the result in OutShaderMap
*
* @param ShaderMapId - the set of static parameters to compile
* @param Platform - the platform to compile for
* @param OutShaderMap - the shader map to compile
* @return - true if compile succeeded or was not necessary (shader map for ShaderMapId was found and was complete)
*/
bool FMaterial::BeginCompileShaderMap(
	const FMaterialShaderMapId& ShaderMapId, 
	EShaderPlatform Platform, 
	TRefCountPtr<FMaterialShaderMap>& OutShaderMap, 
	bool bApplyCompletedShaderMapForRendering)
{
#if WITH_EDITORONLY_DATA
	bool bSuccess = false;

	STAT(double MaterialCompileTime = 0);

	TRefCountPtr<FMaterialShaderMap> NewShaderMap = new FMaterialShaderMap();

	SCOPE_SECONDS_COUNTER(MaterialCompileTime);

	// Generate the material shader code.
	FMaterialCompilationOutput NewCompilationOutput;
	FHLSLMaterialTranslator MaterialTranslator(this,NewCompilationOutput,ShaderMapId.ParameterSet,Platform,GetQualityLevel(),ShaderMapId.FeatureLevel);
	bSuccess = MaterialTranslator.Translate();

	if(bSuccess)
	{
		// Create a shader compiler environment for the material that will be shared by all jobs from this material
		TRefCountPtr<FShaderCompilerEnvironment> MaterialEnvironment = new FShaderCompilerEnvironment();

		MaterialTranslator.GetMaterialEnvironment(Platform, *MaterialEnvironment);
		const FString MaterialShaderCode = MaterialTranslator.GetMaterialShaderCode();
		const bool bSynchronousCompile = RequiresSynchronousCompilation() || !GShaderCompilingManager->AllowAsynchronousShaderCompiling();

		MaterialEnvironment->IncludeFileNameToContentsMap.Add(TEXT("Material.usf"), MaterialShaderCode);

		// Compile the shaders for the material.
		NewShaderMap->Compile(this, ShaderMapId, MaterialEnvironment, NewCompilationOutput, Platform, bSynchronousCompile, bApplyCompletedShaderMapForRendering);

		if (bSynchronousCompile)
		{
			// If this is a synchronous compile, assign the compile result to the output
			OutShaderMap = NewShaderMap->CompiledSuccessfully() ? NewShaderMap : NULL;
		}
		else
		{
			OutstandingCompileShaderMapId = NewShaderMap->GetCompilingId();
			// Async compile, use NULL so that rendering will fall back to the default material.
			OutShaderMap = NULL;
		}
	}

	INC_FLOAT_STAT_BY(STAT_ShaderCompiling_MaterialCompiling,(float)MaterialCompileTime);
	INC_FLOAT_STAT_BY(STAT_ShaderCompiling_MaterialShaders,(float)MaterialCompileTime);

	return bSuccess;
#else
	UE_LOG(LogMaterial, Fatal,TEXT("Not supported."));
	return false;
#endif
}

/**
 * Should the shader for this material with the given platform, shader type and vertex 
 * factory type combination be compiled
 *
 * @param Platform		The platform currently being compiled for
 * @param ShaderType	Which shader is being compiled
 * @param VertexFactory	Which vertex factory is being compiled (can be NULL)
 *
 * @return true if the shader should be compiled
 */
bool FMaterial::ShouldCache(EShaderPlatform Platform, const FShaderType* ShaderType, const FVertexFactoryType* VertexFactoryType) const
{
	return true;
}

//
// FColoredMaterialRenderProxy implementation.
//

const FMaterial* FColoredMaterialRenderProxy::GetMaterial(ERHIFeatureLevel::Type InFeatureLevel) const
{
	return Parent->GetMaterial(InFeatureLevel);
}

/**
 * Finds the shader matching the template type and the passed in vertex factory, asserts if not found.
 */
FShader* FMaterial::GetShader(FMeshMaterialShaderType* ShaderType, FVertexFactoryType* VertexFactoryType) const
{
	const FMeshMaterialShaderMap* MeshShaderMap = RenderingThreadShaderMap->GetMeshShaderMap(VertexFactoryType);
	FShader* Shader = MeshShaderMap ? MeshShaderMap->GetShader(ShaderType) : NULL;
	if (!Shader)
	{
		// Get the ShouldCache results that determine whether the shader should be compiled
		bool bMaterialShouldCache = ShouldCache(GRHIShaderPlatform, ShaderType, VertexFactoryType);
		bool bVFShouldCache = VertexFactoryType->ShouldCache(GRHIShaderPlatform, this, ShaderType);
		bool bShaderShouldCache = ShaderType->ShouldCache(GRHIShaderPlatform, this, VertexFactoryType);
		FString MaterialUsage = GetMaterialUsageDescription();

		int BreakPoint = 0;

		// Assert with detailed information if the shader wasn't found for rendering.  
		// This is usually the result of an incorrect ShouldCache function.
		UE_LOG(LogMaterial, Fatal,
			TEXT("Couldn't find Shader %s for Material Resource %s!\n")
			TEXT("		With VF=%s, Platform=%s \n")
			TEXT("		ShouldCache: Mat=%u, VF=%u, Shader=%u \n")
			TEXT("		Material Usage = %s"),
			ShaderType->GetName(), 
			*GetFriendlyName(),
			VertexFactoryType->GetName(),
			*LegacyShaderPlatformToShaderFormat(GRHIShaderPlatform).ToString(),
			bMaterialShouldCache,
			bVFShouldCache,
			bShaderShouldCache,
			*MaterialUsage
			);
	}

	return Shader;
}

/** Returns the index to the Expression in the Expressions array, or -1 if not found. */
int32 FMaterial::FindExpression( const TArray<TRefCountPtr<FMaterialUniformExpressionTexture> >&Expressions, const FMaterialUniformExpressionTexture &Expression )
{
	for (int32 ExpressionIndex = 0; ExpressionIndex < Expressions.Num(); ++ExpressionIndex)
	{
		if ( Expressions[ExpressionIndex]->IsIdentical(&Expression) )
		{
			return ExpressionIndex;
		}
	}
	return -1;
}

TSet<FMaterial*> FMaterial::EditorLoadedMaterialResources;

/*-----------------------------------------------------------------------------
	FMaterialRenderContext
-----------------------------------------------------------------------------*/

/** 
 * Constructor
 */
FMaterialRenderContext::FMaterialRenderContext(
	const FMaterialRenderProxy* InMaterialRenderProxy,
	const FMaterial& InMaterial,
	const FSceneView* InView,
	bool bInWorkAroundDeferredMipArtifacts)
		: MaterialRenderProxy(InMaterialRenderProxy)
		, Material(InMaterial)
		, bWorkAroundDeferredMipArtifacts(bInWorkAroundDeferredMipArtifacts)
{
	bShowSelection = GIsEditor && InView && InView->Family->EngineShowFlags.Selection;
}

/*-----------------------------------------------------------------------------
	FMaterialRenderProxy
-----------------------------------------------------------------------------*/

void FMaterialRenderProxy::EvaluateUniformExpressions(FUniformExpressionCache& OutUniformExpressionCache, const FMaterialRenderContext& Context) const
{
	check(IsInRenderingThread());

	SCOPE_CYCLE_COUNTER(STAT_CacheUniformExpressions);
	
	// Retrieve the material's uniform expression set.
	const FUniformExpressionSet& UniformExpressionSet = Context.Material.GetRenderingThreadShaderMap()->GetUniformExpressionSet();

	OutUniformExpressionCache.CachedUniformExpressionShaderMap = Context.Material.GetRenderingThreadShaderMap();

	// Create and cache the material's uniform buffer.
	OutUniformExpressionCache.UniformBuffer = UniformExpressionSet.CreateUniformBuffer(Context);

	// Cache 2D texture uniform expressions.
	OutUniformExpressionCache.Textures.Empty(UniformExpressionSet.Uniform2DTextureExpressions.Num());
	for(int32 ExpressionIndex = 0;ExpressionIndex < UniformExpressionSet.Uniform2DTextureExpressions.Num();ExpressionIndex++)
	{
		const UTexture* Value;
		UniformExpressionSet.Uniform2DTextureExpressions[ExpressionIndex]->GetTextureValue(Context,Context.Material,Value);
		OutUniformExpressionCache.Textures.Add(Value);
	}

	// Cache cube texture uniform expressions.
	OutUniformExpressionCache.CubeTextures.Empty(UniformExpressionSet.UniformCubeTextureExpressions.Num());
	for(int32 ExpressionIndex = 0;ExpressionIndex < UniformExpressionSet.UniformCubeTextureExpressions.Num();ExpressionIndex++)
	{
		const UTexture* Value;
		UniformExpressionSet.UniformCubeTextureExpressions[ExpressionIndex]->GetTextureValue(Context,Context.Material,Value);
		OutUniformExpressionCache.CubeTextures.Add(Value);
	}

	OutUniformExpressionCache.ParameterCollections = UniformExpressionSet.ParameterCollections;

	OutUniformExpressionCache.bUpToDate = true;
}

void FMaterialRenderProxy::CacheUniformExpressions()
{
	// Register the render proxy's as a render resource so it can receive notifications to free the uniform buffer.
	InitResource();

	check(UMaterial::GetDefaultMaterial(MD_Surface));
	const bool bES2Preview = false;
	ERHIFeatureLevel::Type FeatureLevelsToCache[2] = { GRHIFeatureLevel, ERHIFeatureLevel::ES2 };
	int32 NumFeatureLevelsToCache = bES2Preview ? 2 : 1;

	for (int32 i = 0; i < NumFeatureLevelsToCache; ++i)
	{
		ERHIFeatureLevel::Type FeatureLevel = FeatureLevelsToCache[i];
		const FMaterial* MaterialNoFallback = GetMaterialNoFallback(FeatureLevel);

		if (MaterialNoFallback && MaterialNoFallback->GetRenderingThreadShaderMap())
		{
			const FMaterial* Material = GetMaterial(FeatureLevel);

			// Do not cache uniform expressions for fallback materials. This step could
			// be skipped where we don't allow for asynchronous shader compiling.
			bool bIsFallbackMaterial = (Material != GetMaterialNoFallback(FeatureLevel));

			if (!bIsFallbackMaterial)
			{
				FMaterialRenderContext MaterialRenderContext(this, *Material, NULL);
				MaterialRenderContext.bShowSelection = GIsEditor;
				EvaluateUniformExpressions(UniformExpressionCache[(int32)FeatureLevel], MaterialRenderContext);
			}
			else
			{
				InvalidateUniformExpressionCache();
				return;
			}
		}
		else
		{
			InvalidateUniformExpressionCache();
			return;
		}
	}
}

void FMaterialRenderProxy::CacheUniformExpressions_GameThread()
{
	if (FApp::CanEverRender())
	{
		ENQUEUE_UNIQUE_RENDER_COMMAND_ONEPARAMETER(
			FCacheUniformExpressionsCommand,
			FMaterialRenderProxy*,RenderProxy,this,
		{
			RenderProxy->CacheUniformExpressions();
		});
	}
}

void FMaterialRenderProxy::InvalidateUniformExpressionCache()
{
	checkSlow(IsInRenderingThread());
	for (int32 i = 0; i < ERHIFeatureLevel::Num; ++i)
	{
		UniformExpressionCache[i].bUpToDate = false;
		UniformExpressionCache[i].UniformBuffer.SafeRelease();
		UniformExpressionCache[i].CachedUniformExpressionShaderMap = NULL;
	}
}

FMaterialRenderProxy::FMaterialRenderProxy()
	: bSelected(false)
	, bHovered(false)
{
}

FMaterialRenderProxy::FMaterialRenderProxy(bool bInSelected, bool bInHovered)
	: bSelected(bInSelected)
	, bHovered(bInHovered)
{
}

FMaterialRenderProxy::~FMaterialRenderProxy()
{
	if(IsInitialized())
	{
		check(IsInRenderingThread());
		ReleaseResource();
	}
}

void FMaterialRenderProxy::InitDynamicRHI()
{
	FMaterialRenderProxy::MaterialRenderProxyMap.Add(this);
}

void FMaterialRenderProxy::ReleaseDynamicRHI()
{
	FMaterialRenderProxy::MaterialRenderProxyMap.Remove(this);
	InvalidateUniformExpressionCache();
}

TSet<FMaterialRenderProxy*> FMaterialRenderProxy::MaterialRenderProxyMap;

/*-----------------------------------------------------------------------------
	FColoredMaterialRenderProxy
-----------------------------------------------------------------------------*/

bool FColoredMaterialRenderProxy::GetVectorValue(const FName ParameterName, FLinearColor* OutValue, const FMaterialRenderContext& Context) const
{
	if(ParameterName == ColorParamName)
	{
		*OutValue = Color;
		return true;
	}
	else
	{
		return Parent->GetVectorValue(ParameterName, OutValue, Context);
	}
}

bool FColoredMaterialRenderProxy::GetScalarValue(const FName ParameterName, float* OutValue, const FMaterialRenderContext& Context) const
{
	return Parent->GetScalarValue(ParameterName, OutValue, Context);
}

bool FColoredMaterialRenderProxy::GetTextureValue(const FName ParameterName,const UTexture** OutValue, const FMaterialRenderContext& Context) const
{
	return Parent->GetTextureValue(ParameterName,OutValue,Context);
}

/*-----------------------------------------------------------------------------
	FLightingDensityMaterialRenderProxy
-----------------------------------------------------------------------------*/
static FName NAME_LightmapRes = FName(TEXT("LightmapRes"));

bool FLightingDensityMaterialRenderProxy::GetVectorValue(const FName ParameterName, FLinearColor* OutValue, const FMaterialRenderContext& Context) const
{
	if (ParameterName == NAME_LightmapRes)
	{
		*OutValue = FLinearColor(LightmapResolution.X, LightmapResolution.Y, 0.0f, 0.0f);
		return true;
	}
	return FColoredMaterialRenderProxy::GetVectorValue(ParameterName, OutValue, Context);
}

/*-----------------------------------------------------------------------------
	FFontMaterialRenderProxy
-----------------------------------------------------------------------------*/

const class FMaterial* FFontMaterialRenderProxy::GetMaterial(ERHIFeatureLevel::Type InFeatureLevel) const
{
	return Parent->GetMaterial(InFeatureLevel);
}

bool FFontMaterialRenderProxy::GetVectorValue(const FName ParameterName, FLinearColor* OutValue, const FMaterialRenderContext& Context) const
{
	return Parent->GetVectorValue(ParameterName, OutValue, Context);
}

bool FFontMaterialRenderProxy::GetScalarValue(const FName ParameterName, float* OutValue, const FMaterialRenderContext& Context) const
{
	return Parent->GetScalarValue(ParameterName, OutValue, Context);
}

bool FFontMaterialRenderProxy::GetTextureValue(const FName ParameterName,const UTexture** OutValue, const FMaterialRenderContext& Context) const
{
	// find the matching font parameter
	if( ParameterName == FontParamName &&
		Font->Textures.IsValidIndex(FontPage) )
	{
		// use the texture page from the font specified for the parameter
		UTexture2D* Texture = Font->Textures[FontPage];
		if( Texture && Texture->Resource )
		{
			*OutValue = Texture;
			return true;
		}		
	}
	// try parent if not valid parameter
	return Parent->GetTextureValue(ParameterName,OutValue,Context);
}

/*-----------------------------------------------------------------------------
	FOverrideSelectionColorMaterialRenderProxy
-----------------------------------------------------------------------------*/

const FMaterial* FOverrideSelectionColorMaterialRenderProxy::GetMaterial(ERHIFeatureLevel::Type InFeatureLevel) const
{
	return Parent->GetMaterial(InFeatureLevel);
}

bool FOverrideSelectionColorMaterialRenderProxy::GetVectorValue(const FName ParameterName, FLinearColor* OutValue, const FMaterialRenderContext& Context) const
{
	if( ParameterName == NAME_SelectionColor )
	{
		*OutValue = SelectionColor;
		return true;
	}
	else
	{
		return Parent->GetVectorValue(ParameterName, OutValue, Context);
	}
}

bool FOverrideSelectionColorMaterialRenderProxy::GetScalarValue(const FName ParameterName, float* OutValue, const FMaterialRenderContext& Context) const
{
	return Parent->GetScalarValue(ParameterName,OutValue,Context);
}

bool FOverrideSelectionColorMaterialRenderProxy::GetTextureValue(const FName ParameterName,const UTexture** OutValue, const FMaterialRenderContext& Context) const
{
	return Parent->GetTextureValue(ParameterName,OutValue,Context);
}

/** Returns the number of samplers used in this material, or -1 if the material does not have a valid shader map (compile error or still compiling). */
int32 FMaterialResource::GetSamplerUsage() const
{
	if (GetGameThreadShaderMap())
	{
		return GetGameThreadShaderMap()->GetMaxTextureSamplers();
	}

	return -1;
}

/** Returns a string that describes the material's usage for debugging purposes. */
FString FMaterialResource::GetMaterialUsageDescription() const
{
	check(Material);
	FString BaseDescription = GetLightingModelString(GetLightingModel()) + TEXT(", ") + GetBlendModeString(GetBlendMode());

	if (IsSpecialEngineMaterial())
	{
		BaseDescription += TEXT(", SpecialEngine");
	}
	if (IsTwoSided())
	{
		BaseDescription += TEXT(", TwoSided");
	}
	if (IsTangentSpaceNormal())
	{
		BaseDescription += TEXT(", TSNormal");
	}
	if (ShouldInjectEmissiveIntoLPV())
	{
		BaseDescription += TEXT(", InjectEmissiveIntoLPV");
	}
	if (IsMasked())
	{
		BaseDescription += TEXT(", Masked");
	}
	if (IsDistorted())
	{
		BaseDescription += TEXT(", Distorted");
	}

	for (int32 MaterialUsageIndex = 0; MaterialUsageIndex < MATUSAGE_MAX; MaterialUsageIndex++)
	{
		if (Material->GetUsageByFlag((EMaterialUsage)MaterialUsageIndex))
		{
			BaseDescription += FString(TEXT(", ")) + Material->GetUsageName((EMaterialUsage)MaterialUsageIndex);
		}
	}

	return BaseDescription;
}

void FMaterial::GetDependentShaderAndVFTypes(EShaderPlatform Platform, TArray<FShaderType*>& OutShaderTypes, TArray<FVertexFactoryType*>& OutVFTypes) const
{
	// Iterate over all vertex factory types.
	for (TLinkedList<FVertexFactoryType*>::TIterator VertexFactoryTypeIt(FVertexFactoryType::GetTypeList()); VertexFactoryTypeIt; VertexFactoryTypeIt.Next())
	{
		FVertexFactoryType* VertexFactoryType = *VertexFactoryTypeIt;
		check(VertexFactoryType);

		if (VertexFactoryType->IsUsedWithMaterials())
		{
			bool bAddedTypeFromThisVF = false;

			// Iterate over all mesh material shader types.
			for(TLinkedList<FShaderType*>::TIterator ShaderTypeIt(FShaderType::GetTypeList());ShaderTypeIt;ShaderTypeIt.Next())
			{
				FMeshMaterialShaderType* ShaderType = ShaderTypeIt->GetMeshMaterialShaderType();

				if (ShaderType && 
					VertexFactoryType && 
					ShaderType->ShouldCache(Platform, this, VertexFactoryType) && 
					ShouldCache(Platform, ShaderType, VertexFactoryType) &&
					VertexFactoryType->ShouldCache(Platform, this, ShaderType)
					)
				{
					bAddedTypeFromThisVF = true;
					OutShaderTypes.AddUnique(ShaderType);
				}
			}

			if (bAddedTypeFromThisVF)
			{
				OutVFTypes.Add(VertexFactoryType);
			}
		}
	}

	// Iterate over all material shader types.
	for (TLinkedList<FShaderType*>::TIterator ShaderTypeIt(FShaderType::GetTypeList()); ShaderTypeIt; ShaderTypeIt.Next())
	{
		FMaterialShaderType* ShaderType = ShaderTypeIt->GetMaterialShaderType();

		if (ShaderType && 
			ShaderType->ShouldCache(Platform, this) && 
			ShouldCache(Platform, ShaderType, NULL)
			)
		{
			OutShaderTypes.Add(ShaderType);
		}
	}

	// Sort by name so that we get deterministic keys
	OutShaderTypes.Sort(FCompareShaderTypes());
	OutVFTypes.Sort(FCompareVertexFactoryTypes());
}

void FMaterial::GetReferencedTexturesHash(FSHAHash& OutHash) const
{
	FSHA1 HashState;

	const TArray<UTexture*>& ReferencedTextures = GetReferencedTextures();
	// Hash the names of the uniform expression textures to capture changes in their order or values resulting from material compiler code changes
	for (int32 TextureIndex = 0; TextureIndex < ReferencedTextures.Num(); TextureIndex++)
	{
		FString TextureName;

		if (ReferencedTextures[TextureIndex])
		{
			TextureName = ReferencedTextures[TextureIndex]->GetName();
		}

		HashState.UpdateWithString(*TextureName, TextureName.Len());
	}

	HashState.Final();
	HashState.GetHash(&OutHash.Hash[0]);
}

/**
 * Get user source code for the material, with a list of code snippets to highlight representing the code for each MaterialExpression
 * @param OutSource - generated source code
 * @param OutHighlightMap - source code highlight list
 * @return - true on Success
 */
bool FMaterial::GetMaterialExpressionSource( FString& OutSource, TMap<FMaterialExpressionKey,int32> OutExpressionCodeMap[][SF_NumFrequencies] )
{
#if WITH_EDITORONLY_DATA
	class FViewSourceMaterialTranslator : public FHLSLMaterialTranslator
	{
	public:
		FViewSourceMaterialTranslator(FMaterial* InMaterial,FMaterialCompilationOutput& InMaterialCompilationOutput,const FStaticParameterSet& StaticParameters,EShaderPlatform InPlatform,EMaterialQualityLevel::Type InQualityLevel,ERHIFeatureLevel::Type InFeatureLevel)
		:	FHLSLMaterialTranslator(InMaterial,InMaterialCompilationOutput,StaticParameters,InPlatform,InQualityLevel,InFeatureLevel)
		{}

		void GetExpressionCodeMap(TMap<FMaterialExpressionKey,int32> OutExpressionCodeMap[][SF_NumFrequencies])
		{
			for (int32 PropertyIndex = 0; PropertyIndex < MP_MAX; PropertyIndex++)
			{
				OutExpressionCodeMap[PropertyIndex][GetMaterialPropertyShaderFrequency((EMaterialProperty)PropertyIndex)] = FunctionStack.Last().ExpressionCodeMap[PropertyIndex][GetMaterialPropertyShaderFrequency((EMaterialProperty)PropertyIndex)];
			}
		}
	};

	FMaterialCompilationOutput TempOutput;
	FViewSourceMaterialTranslator MaterialTranslator(this, TempOutput, FStaticParameterSet(), GRHIShaderPlatform, GetQualityLevel(), GetFeatureLevel());
	bool bSuccess = MaterialTranslator.Translate();

	if( bSuccess )
	{
		// Generate the HLSL
		OutSource = MaterialTranslator.GetMaterialShaderCode();

		// Save the Expression Code map.
		MaterialTranslator.GetExpressionCodeMap(OutExpressionCodeMap);
	}
	return bSuccess;
#else
	UE_LOG(LogMaterial, Fatal,TEXT("Not supported."));
	return false;
#endif
}

/** Recompiles any materials in the EditorLoadedMaterialResources list if they are not complete. */
void FMaterial::UpdateEditorLoadedMaterialResources()
{
	for (TSet<FMaterial*>::TIterator It(EditorLoadedMaterialResources); It; ++It)
	{
		FMaterial* CurrentMaterial = *It;
		if (!CurrentMaterial->GetGameThreadShaderMap() || !CurrentMaterial->GetGameThreadShaderMap()->IsComplete(CurrentMaterial, true))
		{
			CurrentMaterial->CacheShaders(GRHIShaderPlatform, true);
		}
	}
}

void FMaterial::BackupEditorLoadedMaterialShadersToMemory(TMap<FMaterialShaderMap*, TScopedPointer<TArray<uint8> > >& ShaderMapToSerializedShaderData)
{
	for (TSet<FMaterial*>::TIterator It(EditorLoadedMaterialResources); It; ++It)
	{
		FMaterial* CurrentMaterial = *It;
		FMaterialShaderMap* ShaderMap = CurrentMaterial->GetGameThreadShaderMap();

		if (ShaderMap && !ShaderMapToSerializedShaderData.Contains(ShaderMap))
		{
			TArray<uint8>* ShaderData = ShaderMap->BackupShadersToMemory();
			ShaderMapToSerializedShaderData.Emplace(ShaderMap, ShaderData);
		}
	}
}

void FMaterial::RestoreEditorLoadedMaterialShadersFromMemory(const TMap<FMaterialShaderMap*, TScopedPointer<TArray<uint8> > >& ShaderMapToSerializedShaderData)
{
	for (TSet<FMaterial*>::TIterator It(EditorLoadedMaterialResources); It; ++It)
	{
		FMaterial* CurrentMaterial = *It;
		FMaterialShaderMap* ShaderMap = CurrentMaterial->GetGameThreadShaderMap();

		if (ShaderMap)
		{
			const TScopedPointer<TArray<uint8> >* ShaderData = ShaderMapToSerializedShaderData.Find(ShaderMap);

			if (ShaderData)
			{
				ShaderMap->RestoreShadersFromMemory(**ShaderData);
			}
		}
	}
}

FMaterialUpdateContext::FMaterialUpdateContext(uint32 Options, EShaderPlatform InShaderPlatform)
{
	bool bReregisterComponents = (Options & EOptions::ReregisterComponents) != 0;
	bSyncWithRenderingThread = (Options & EOptions::SyncWithRenderingThread) != 0;
	if (bReregisterComponents)
	{
		ComponentReregisterContext = new FGlobalComponentReregisterContext();
	}
	if (bSyncWithRenderingThread)
	{
		FlushRenderingCommands();
	}
	ShaderPlatform = InShaderPlatform;
}

FMaterialUpdateContext::~FMaterialUpdateContext()
{
	double StartTime = FPlatformTime::Seconds();

	// if the shader platform that was processed is not the currently rendering shader platform, 
	// there's no reason to update all of the runtime components
	if (ShaderPlatform != GRHIShaderPlatform)
	{
		return;
	}

	// Flush rendering commands even though we already did so in the constructor.
	// Anything may have happened since the constructor has run. The flush is
	// done once here to avoid calling it once per static permutation we update.
	if (bSyncWithRenderingThread)
	{
		FlushRenderingCommands();
	}

	TArray<const FMaterial*> MaterialResourcesToUpdate;
	TArray<UMaterialInstance*> InstancesToUpdate;

	bool bUpdateStaticDrawLists = ComponentReregisterContext == NULL;

	// If static draw lists must be updated, gather material resources from all updated materials.
	if (bUpdateStaticDrawLists)
	{
		for (TSet<UMaterial*>::TConstIterator It(UpdatedMaterials); It; ++It)
		{
			UMaterial* Material = *It;

			for (int32 QualityLevelIndex = 0; QualityLevelIndex < EMaterialQualityLevel::Num; QualityLevelIndex++)
			{
				for (int32 FeatureLevelIndex = 0; FeatureLevelIndex < ERHIFeatureLevel::Num; FeatureLevelIndex++)
				{
					FMaterialResource* CurrentResource = Material->MaterialResources[QualityLevelIndex][FeatureLevelIndex];
					MaterialResourcesToUpdate.Add(CurrentResource);
				}
			}
		}
	}

	// Go through all loaded material instances and recompile their static permutation resources if needed
	// This is necessary since the parent UMaterial stores information about how it should be rendered, (eg bUsesDistortion)
	// but the child can have its own shader map which may not contain all the shaders that the parent's settings indicate that it should.
	for (TObjectIterator<UMaterialInstance> It; It; ++It)
	{
		UMaterialInstance* CurrentMaterialInstance = *It;
		UMaterial* BaseMaterial = CurrentMaterialInstance->GetMaterial();

		if (UpdatedMaterials.Contains(BaseMaterial))
		{
			InstancesToUpdate.Add(CurrentMaterialInstance);
		}
	}

	// Material instances that use this base material must have their uniform expressions recached 
	// However, some material instances that use this base material may also depend on another MI with static parameters
	// So we must update the MI's with static parameters first, and do other MI's in a second pass
	int32 NumInstancesWithStaticPermutations = 0;
	for (int32 MIIndex = 0; MIIndex < InstancesToUpdate.Num(); MIIndex++)
	{
		UMaterialInstance* CurrentMaterialInstance = InstancesToUpdate[MIIndex];

		if (CurrentMaterialInstance->bHasStaticPermutationResource)
		{
			NumInstancesWithStaticPermutations++;
			CurrentMaterialInstance->InitStaticPermutation();

			// Collect FMaterial's that have been recompiled
			if (bUpdateStaticDrawLists)
			{
				for (int32 QualityLevelIndex = 0; QualityLevelIndex < EMaterialQualityLevel::Num; QualityLevelIndex++)
				{
					for (int32 FeatureLevelIndex = 0; FeatureLevelIndex < ERHIFeatureLevel::Num; FeatureLevelIndex++)
					{
						FMaterialResource* CurrentResource = CurrentMaterialInstance->StaticPermutationMaterialResources[QualityLevelIndex][FeatureLevelIndex];
						MaterialResourcesToUpdate.Add(CurrentResource);
					}
				}
			}
		}
	}

	// Recache uniform expressions on dependent MI's without static parameters
	for (int32 MIIndex = 0; MIIndex < InstancesToUpdate.Num(); MIIndex++)
	{
		UMaterialInstance* CurrentMaterialInstance = InstancesToUpdate[MIIndex];

		if (!CurrentMaterialInstance->bHasStaticPermutationResource)
		{
			CurrentMaterialInstance->InitStaticPermutation();
		}
	}

	if (bUpdateStaticDrawLists)
	{
		// Update static draw lists affected by any FMaterials that were recompiled
		// This is only needed if we aren't reregistering components which is not always
		// safe, e.g. while a component is being registered.
		GetRendererModule().UpdateStaticDrawListsForMaterials(MaterialResourcesToUpdate);
	}
	else
	{
		// We must be reregistering components in which case static draw lists will be recreated.
		check(ComponentReregisterContext != NULL);
		ComponentReregisterContext.Reset();
	}

	double EndTime = FPlatformTime::Seconds();
	UE_LOG(LogMaterial,Log,
		TEXT("%f seconds spent updating %d materials, %d instances, %d with static permutations."),
		(float)(EndTime-StartTime),
		UpdatedMaterials.Num(),
		InstancesToUpdate.Num(),
		NumInstancesWithStaticPermutations
		);
}

EMaterialProperty GetMaterialPropertyFromInputOutputIndex(int32 Index)
{
	switch(Index)
	{
	case 0: return MP_BaseColor;
	case 1: return MP_Metallic;
	case 2: return MP_Specular;
	case 3: return MP_Roughness;
	case 4: return MP_EmissiveColor;
	case 5: return MP_Opacity;
	case 6: return MP_OpacityMask;
	case 7: return MP_Normal;
	case 8: return MP_WorldPositionOffset;
	case 9: return MP_WorldDisplacement;
	case 10: return MP_TessellationMultiplier;
	case 11: return MP_SubsurfaceColor;
	case 12: return MP_AmbientOcclusion;
	case 13: return MP_Refraction;
	};

	const int32 UVStart = 14;
	const int32 UVEnd = UVStart + MP_CustomizedUVs7 - MP_CustomizedUVs0;

	if (Index >= UVStart && Index <= UVEnd)
	{
		return (EMaterialProperty)(MP_CustomizedUVs0 + Index - UVStart);
	}

	if (Index == UVEnd + 1)
	{
		return MP_MaterialAttributes;
	}

	return MP_MAX;
}

int32 GetInputOutputIndexFromMaterialProperty(EMaterialProperty Property)
{
	switch(Property)
	{
	case MP_BaseColor: return 0;
	case MP_Metallic: return 1;
	case MP_Specular: return 2;
	case MP_Roughness: return 3;
	case MP_EmissiveColor: return 4;
	case MP_Opacity: return 5;
	case MP_OpacityMask: return 6;
	case MP_Normal: return 7;
	case MP_WorldPositionOffset: return 8;
	case MP_WorldDisplacement: return 9;
	case MP_TessellationMultiplier: return 10;
	case MP_SubsurfaceColor: return 11;
	case MP_AmbientOcclusion: return 12;
	case MP_Refraction: return 13;
	case MP_MaterialAttributes: UE_LOG(LogMaterial, Fatal, TEXT("We should never need the IO index of the MaterialAttriubtes property."));
	};

	if (Property >= MP_CustomizedUVs0 && Property <= MP_CustomizedUVs7)
	{
		return 14 + Property - MP_CustomizedUVs0;
	}

	return -1;
}

void GetDefaultForMaterialProperty(EMaterialProperty Property, float& OutDefaultFloat, FColor& OutDefaultColor, FVector& OutDefaultVector )
{
	OutDefaultFloat = 0;
	OutDefaultColor = FColor::Black;
	OutDefaultVector = FVector::ZeroVector;

	switch(Property)
	{
	case MP_Opacity:				OutDefaultFloat = 1.0f; break;
	case MP_OpacityMask:			OutDefaultFloat = 1.0f; break;
	case MP_Metallic:				OutDefaultFloat = 0.0f; break;
	case MP_Specular:				OutDefaultFloat = 0.5f; break;
	case MP_Roughness:				OutDefaultFloat = 0.5f; break;
	case MP_TessellationMultiplier: OutDefaultFloat = 1.0f; break;
	case MP_AmbientOcclusion:		OutDefaultFloat = 1.0f; break;
	case MP_EmissiveColor:			OutDefaultColor = FColor(0,0,0); break;
	case MP_DiffuseColor:			OutDefaultColor = FColor(0,0,0); break;
	case MP_SpecularColor:			OutDefaultColor = FColor(0,0,0); break;
	case MP_BaseColor:				OutDefaultColor = FColor(0,0,0); break;
	case MP_SubsurfaceColor:		OutDefaultColor = FColor(1,1,1); break;
	case MP_Normal:					OutDefaultVector = FVector(0,0,1); break;
	case MP_WorldPositionOffset:	OutDefaultVector = FVector::ZeroVector; break;
	case MP_WorldDisplacement:		OutDefaultVector = FVector::ZeroVector; break;
	case MP_Refraction:				OutDefaultVector = FVector(1,0,0); break;
	};
}

FString GetNameOfMaterialProperty(EMaterialProperty Property)
{
	switch(Property)
	{
	case MP_BaseColor:				return TEXT("BaseColor");
	case MP_Metallic:				return TEXT("Metallic");
	case MP_Specular:				return TEXT("Specular");
	case MP_Roughness:				return TEXT("Roughness");
	case MP_Normal:					return TEXT("Normal");
	case MP_EmissiveColor:			return TEXT("EmissiveColor");
	case MP_Opacity:				return TEXT("Opacity");
	case MP_OpacityMask:			return TEXT("OpacityMask"); 
	case MP_WorldPositionOffset:	return TEXT("WorldPositionOffset");
	case MP_WorldDisplacement:		return TEXT("WorldDisplacement");
	case MP_TessellationMultiplier: return TEXT("TessellationMultiplier");
	case MP_SubsurfaceColor:		return TEXT("SubsurfaceColor");
	case MP_AmbientOcclusion:		return TEXT("AmbientOcclusion");
	case MP_Refraction:				return TEXT("Refraction");
	};

	if (Property >= MP_CustomizedUVs0 && Property <= MP_CustomizedUVs7)
	{
		return FString::Printf(TEXT("CustomizedUVs%u"), Property - MP_CustomizedUVs0);
	}

	return TEXT("");
}


bool UMaterialInterface::IsPropertyActive(EMaterialProperty InProperty)const
{
	//TODO: Disable properties in instances based on the currently set overrides and other material settings?
	//For now just allow all properties in instances. 
	//This had to be refactored into the instance as some override properties alter the properties that are active.
	return false;
}

int32 UMaterialInterface::CompileProperty( class FMaterialCompiler* Compiler, EMaterialProperty Property, float DefaultFloat, FLinearColor DefaultColor, const FVector4& DefaultVector )
{
	return INDEX_NONE;
}

int32 UMaterialInterface::CompileProperty( FMaterialCompiler* Compiler, EMaterialProperty Property )
{
	int32 Ret = INDEX_NONE;

	float DefaultFloat;
	FColor DefaultColor;
	FVector DefaultVector;
	GetDefaultForMaterialProperty(Property, DefaultFloat, DefaultColor, DefaultVector);

	if (IsPropertyActive(Property))
	{
		return CompileProperty(Compiler, Property, DefaultFloat, DefaultColor, DefaultVector);
	}
	else
	{
		FLinearColor DefaultLinearColor(DefaultColor.ReinterpretAsLinear());

		switch(Property)
		{
		case MP_Opacity:
		case MP_OpacityMask:
		case MP_Metallic:
		case MP_Specular:
		case MP_Roughness:
		case MP_TessellationMultiplier:
		case MP_AmbientOcclusion:
			{
				return Compiler->Constant(DefaultFloat);
			}
		case MP_EmissiveColor:
		case MP_DiffuseColor:
		case MP_SpecularColor:
		case MP_BaseColor:
		case MP_SubsurfaceColor:
			{
				return Compiler->Constant3(DefaultLinearColor.R, DefaultLinearColor.G, DefaultLinearColor.B);
			}

		case MP_Normal:
		case MP_WorldPositionOffset:
		case MP_WorldDisplacement:
		case MP_Refraction:
			{
				return Compiler->Constant3(DefaultVector.X, DefaultVector.Y, DefaultVector.Z);
			}

		default:
			{
				if (Property >= MP_CustomizedUVs0 && Property <= MP_CustomizedUVs7)
				{
					return Compiler->Constant2(DefaultVector.X, DefaultVector.Y);
				}
				else
				{
					check(false);
					break;
				}
			}
		};
	}
	return Ret;
}

/** TODO - This can be removed whenever VER_UE4_MATERIAL_ATTRIBUTES_REORDERING is no longer relevant. */
void DoMaterialAttributeReorder(FExpressionInput* Input)
{
	if( Input && Input->Expression && Input->Expression->IsA(UMaterialExpressionBreakMaterialAttributes::StaticClass()) )
	{
		switch(Input->OutputIndex)
		{
		case 4: Input->OutputIndex = 7; break;
		case 5: Input->OutputIndex = 4; break;
		case 6: Input->OutputIndex = 5; break;
		case 7: Input->OutputIndex = 6; break;
		}
	}
}
FMaterialInstanceBasePropertyOverrides::FMaterialInstanceBasePropertyOverrides()
	:bOverride_OpacityMaskClipValue(false)
	,bOverride_BlendMode(false)
	,bOverride_LightingModel(false)
	,bOverride_TwoSided(false)
	,OpacityMaskClipValue(0.0f)
	,BlendMode(BLEND_Opaque)
	,LightingModel(MLM_DefaultLit)
	,TwoSided(0)
{

}

void FMaterialInstanceBasePropertyOverrides::Init(const UMaterialInstance& Instance)
{
	OpacityMaskClipValue = Instance.GetOpacityMaskClipValue();
	BlendMode = Instance.GetBlendMode();
	LightingModel = Instance.GetLightingModel();
	TwoSided = (uint32)Instance.IsTwoSided();
}

void FMaterialInstanceBasePropertyOverrides::UpdateHash(FSHA1& HashState) const
{
	if(bOverride_OpacityMaskClipValue)
	{
		const FString HashString = TEXT("bOverride_OpacityMaskClipValue");
		HashState.UpdateWithString(*HashString, HashString.Len());
		HashState.Update((const uint8*)&OpacityMaskClipValue, sizeof(OpacityMaskClipValue));
	}

 	if(bOverride_BlendMode)
	{
		const FString HashString = TEXT("bOverride_BlendMode");
 		HashState.UpdateWithString(*HashString, HashString.Len());
 		HashState.Update((const uint8*)&BlendMode, sizeof(BlendMode));
	}

	if(bOverride_LightingModel)
	{
		const FString HashString = TEXT("bOverride_LightingModel");
		HashState.UpdateWithString(*HashString, HashString.Len());
		HashState.Update((const uint8*)&LightingModel, sizeof(LightingModel));
	}

	//This does seem like it needs to be in the hash but due to some shaders being added to when two sided is enabled
	//it causes a recompile anyway.
// 	if(bOverride_TwoSided)
// 	{
// 		HashString = TEXT("bOverride_TwoSided");
// 		HashState.UpdateWithString(*HashString, HashString.Len());
// 		bool bIsTwoSided = TwoSided;
// 		HashState.Update((uint8*)&bIsTwoSided, sizeof(bIsTwoSided));
// 	}

	//Some properties may not need to be in the shader map ID so don't add them unnecessarily.
}

bool FMaterialInstanceBasePropertyOverrides::Update(const FMaterialInstanceBasePropertyOverrides& Updated)
{
	bool bRet = false;
	//Work out if we need a recompile.
	bRet |= (Updated.bOverride_OpacityMaskClipValue != bOverride_OpacityMaskClipValue);
	bRet |= Updated.bOverride_OpacityMaskClipValue && (OpacityMaskClipValue != Updated.OpacityMaskClipValue);

	bRet |= (Updated.bOverride_BlendMode != bOverride_BlendMode);
	bRet |= Updated.bOverride_BlendMode && (BlendMode != Updated.BlendMode);

	bRet |= (Updated.bOverride_LightingModel != bOverride_LightingModel);
	bRet |= Updated.bOverride_LightingModel && (LightingModel != Updated.LightingModel);

	bRet |= (Updated.bOverride_TwoSided != bOverride_TwoSided);
	bRet |= Updated.bOverride_TwoSided && (TwoSided != Updated.TwoSided);

	//Update the data.
	*this = Updated;

	return bRet;
}
