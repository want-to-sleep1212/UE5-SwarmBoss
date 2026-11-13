#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Swarm/SwarmPartTypes.h"
#include "Swarm/SwarmShapeSampler.h"
#include "SwarmFormDefinition.generated.h"

/** 거인을 구성하는 도형 하나의 생성 설정. 실행 중인 구체·ISM·생존 수는 저장하지 않는다. */
USTRUCT(BlueprintType)
struct SWARMBOSS_API FSwarmPartDefinition
{
	GENERATED_BODY()

	// 이 도형의 부위 식별자. 같은 형태의 Parts 안에서 중복되지 않아야 하며 None/Preview는 사용할 수 없다.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Part")
	ESwarmPartId PartId = ESwarmPartId::Torso;

	// 연결된 부모 부위의 PartId. 최상위 몸통은 None이며, 나머지는 같은 Parts 안의 부위를 가리킨다.
	// 배열 순서가 부모·자식 관계를 정하지 않으며, 자기 자신을 부모로 지정하거나 순환 연결을 만들면 안 된다.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Part")
	ESwarmPartId ParentPartId = ESwarmPartId::None;

	// 구체 중심 좌표를 생성할 영역 종류. 구체 메시 자체의 모양을 바꾸는 설정이 아니다.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Shape")
	ESwarmPrimitive Shape = ESwarmPrimitive::Box;

	// 박스의 X/Y/Z 반길이 비율. 실제 크기는 배정 개수·구체 간격·배치 계수로 계산하며 구형 영역에서는 무시한다.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Shape", meta=(ClampMin="0.01"))
	FVector BoxHalfExtentsRatio = FVector::OneVector;

	// 전체 부위 가중치 합에 대한 이 부위의 구체 배분 비중. 2와 1이면 2:1이며 구체 개수 자체가 아니다.
	// 0이면 비율 배분은 받지 않는다. 정수 나머지는 형태의 RemainderPartId 부위에 별도로 배정한다.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Distribution", meta=(ClampMin="0"))
	int32 AllocationWeight = 1;

	// 부모 도형 중심 기준의 초기 중심 위치(cm). ParentPartId가 None이면 Manager 기준 위치이다.
	// 크기 변화에 따른 연결부 자동 배치는 아직 없으므로 이 값만으로 손실 후 관절 연결을 보장하지 않는다.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Pose", meta=(Units="cm"))
	FVector RelativeLocation = FVector::ZeroVector;

	// 부모 도형 기준의 초기 회전. 최상위 부위는 Manager 기준이다.
	// 구체 크기가 함께 바뀌지 않도록 이 정의에는 Scale을 두지 않는다.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Pose")
	FRotator RelativeRotation = FRotator::ZeroRotator;
};

/** 형태를 구성할 부위 정의를 보관하는 데이터 에셋. 현재는 거인만 정의하며 생성·이동 기능은 Manager가 담당한다. */
UCLASS(BlueprintType)
class SWARMBOSS_API USwarmFormDefinition : public UDataAsset
{
	GENERATED_BODY()

public:
	// 거인을 구성할 도형 목록. 이번 단계에서는 형식만 정의하며 실제 부위 목록과 배치 수치는 에셋에서 작성한다.
	// 소비하는 Manager가 ID 중복·부모 참조·순환·가중치·좌표 유효성을 확인한 뒤 사용해야 한다.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Swarm|Parts", meta=(TitleProperty="PartId"))
	TArray<FSwarmPartDefinition> Parts;

	// 비율 배분에서 정수로 버린 뒤 남은 구체를 받을 부위. 거인은 몸통이며 Parts 안에 존재해야 한다.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Swarm|Distribution")
	ESwarmPartId RemainderPartId = ESwarmPartId::Torso;
};
