#pragma once
 
#include "CoreMinimal.h"
#include "Math/Vector.h"
#include "Shoot.generated.h"


/**
 * The attack command for the player.
 */
USTRUCT(BlueprintType)
struct BATTLEFRAME_API FShoot
{
	GENERATED_BODY()
 
  public:

	/* ����ģʽ*/
	UPROPERTY(BlueprintReadWrite, EditAnywhere)
	int Mode = 0;
	
	/* ���Ƶ��*/
	UPROPERTY(BlueprintReadWrite, EditAnywhere)
	float Frequency = 3.f; 

	/* ��������ֲ�����*/
	UPROPERTY(BlueprintReadWrite, EditAnywhere)
	int AdditionalStreamCount = 0; 

	/* �������ÿ���ֲ�ļн�*/
	UPROPERTY(BlueprintReadWrite, EditAnywhere)
	float AdditionalStreamDistance = 700.f; 

	/* �����뾶�����*/
	UPROPERTY(BlueprintReadWrite, EditAnywhere)
	float Range = 6500.0f; 

	/* Ԥ�е�������*/
	UPROPERTY(BlueprintReadWrite, EditAnywhere)
	int PredictPrecision = 3; 

	/* ��������*/
	UPROPERTY(BlueprintReadWrite, EditAnywhere)
	float MaxPitchAngle = 45.0f; 

	/* ��������*/
	UPROPERTY(BlueprintReadWrite, EditAnywhere)
	float MinPitchAngle = 15.0f; 

	/* ����Ƕ����*/
	//UPROPERTY(BlueprintReadWrite, EditAnywhere)
	float AngleRandom = 0.f; 

	/* �����ʼλ�����*/
	//UPROPERTY(BlueprintReadWrite, EditAnywhere)
	FVector2D FromLocationRandom = FVector2D(0.f, 0.f);

	UPROPERTY(BlueprintReadWrite, EditAnywhere)
	float Gravity = 8000.f; 

	/* ���������ô�������ѡ��һ��������*/
	UPROPERTY(BlueprintReadWrite, EditAnywhere)
	int32 pickOneAmongst = 3;

	/* ������ʱ���Ŀ�껹�����½���һ������*/
	UPROPERTY(BlueprintReadWrite, EditAnywhere)
	float retargettingTime = 1.f;

	UPROPERTY(BlueprintReadWrite, EditAnywhere)
	float targettingAgainTime = 0.5f;

	UPROPERTY(BlueprintReadWrite, EditAnywhere)
	float traceCompensationMult = 1.f;

	UPROPERTY(BlueprintReadWrite, EditAnywhere)
	int32 Priority = 0;

};
