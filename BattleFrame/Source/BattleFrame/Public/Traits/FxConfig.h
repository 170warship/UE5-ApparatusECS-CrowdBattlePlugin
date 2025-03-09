#pragma once

#include "CoreMinimal.h"
#include "NiagaraSystem.h"

#include "FxConfig.generated.h" 

USTRUCT(BlueprintType)
struct BATTLEFRAME_API FFxConfig
{
	GENERATED_BODY()

public:

	UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (Tooltip = "������Ч��������", DisplayName = "SubType_Batched"))
	ESubType SubType = ESubType::None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (Tooltip = "�Ǻ�����ЧNiagara�ʲ�", DisplayName = "NiagaraAsset_UnBatched"))
	UNiagaraSystem* NiagaraAsset;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (Tooltip = "�Ǻ�����ЧCascade�ʲ�", DisplayName = "CascadeAsset_UnBatched"))
	UParticleSystem* CascadeAsset;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, meta = (Tooltip = "ƫ�����λ���볯��ȫ������"))
	FTransform Transform = FTransform::Identity;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (Tooltip = "�Ƿ�̶�"))
	bool bAttached = false;

};
