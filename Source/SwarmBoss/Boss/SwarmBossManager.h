#pragma once

#include "GameFramework/Actor.h"
#include "Boss/SwarmFormDefinition.h"
#include "Swarm/SwarmParticleStore.h"
#include "Swarm/SwarmShapeSampler.h"
#include "SwarmBossManager.generated.h"

class UAnimSequence;
class UInstancedStaticMeshComponent;
class UPrimitiveComponent;
class USkeletalMesh;
class USkeletalMeshComponent;
class UStaticMesh;
class USwarmFormDefinition;

// 애니메이션 자세 전달에 쓰는 부위 하나의 기준 데이터. CapturePoseCalibration()이 기준 애니메이션 0초 포즈에서 만든다.
struct FSwarmPartPoseCalibration
{
	ESwarmPartId PartId = ESwarmPartId::None;

	// 부위 뼈 B와 기준 뼈 P의 PoseSource 뼈 번호.
	int32 BoneIndex = INDEX_NONE;
	int32 ReferenceBoneIndex = INDEX_NONE;

	// 기준 자세의 상대 회전의 역 L0⁻¹ = (P0⁻¹ × B0)⁻¹.
	FQuat InverseRestRelative = FQuat::Identity;

	// 기준 뼈 좌표계 → 거인 부모 부위 좌표계 보정 C = G0⁻¹ × M × P0. 기준 자세에서 한 번 계산하고 매 프레임 다시 계산하지 않는다.
	FQuat FrameCorrection = FQuat::Identity;
};

// 초기 생성할 부위와 해당 ISM 컴포넌트 기준의 구체 중심 좌표 목록.
struct FSwarmPartPlacement
{
	ESwarmPartId PartId;
	TConstArrayView<FVector> LocalPositions;
};

// 부위 하나의 ISM과 인스턴스 번호를 중앙 ParticleId로 바꾸는 배열.
USTRUCT()
struct FSwarmPart
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> Instances;

	TArray<int32> InstanceToParticle;

	// 마지막 초기 생성에 사용한 도형·간격 설정. 재분배 시 개수만 바꿔 목표 좌표를 생성한다.
	FSwarmSamplingSettings SamplingSettings;

	// 마지막 초기 생성에 사용한 부위 정의 복사본. 부모·연결점·Gap·회전으로 이 ISM의 위치를 계산한다.
	// 생성 후 에셋이 바뀌어도 재분배 위치 계산이 생성 당시 구성과 어긋나지 않도록 복사해 둔다.
	FSwarmPartDefinition Definition;

	// 현재 목표 배치의 도형 반크기(cm, 구 중심 허용 영역). 구는 세 축 모두 반지름이며, 구체가 0개면 0이다.
	FVector HalfExtents = FVector::ZeroVector;

	// 현재 자세. 기본 회전(Definition.RelativeRotation)에 더하는 회전이며 축은 부모 부위 로컬 기준이다.
	// 관절 피벗(이 부위의 (0, 0, ChildAnchor.Z) 점)을 중심으로 돈다. 최상위 부위는 항상 회전 없음이다.
	// 재분배에서는 유지하고, GenerateInitialParts()로 전체를 다시 만들 때 초기화한다.
	FQuat Pose = FQuat::Identity;
};

/** 중앙 구체 저장소와 부위별 ISM을 소유하고, 구체 생성·제거·목표 배정·이동을 처리한다. */
UCLASS()
class SWARMBOSS_API ASwarmBossManager : public AActor
{
	GENERATED_BODY()

public:
	ASwarmBossManager();

	bool GenerateInitialParts();

	// 초기 생성에 사용할 형태 데이터를 바꾼다. 이미 생성된 구체는 그대로 두고 다음 GenerateInitialParts()부터 적용한다.
	void SetFormDefinition(USwarmFormDefinition* NewFormDefinition);

	bool RemoveInstance(const UPrimitiveComponent* HitComponent, int32 InstanceIndex);

	// 여러 피격 결과(Hit.GetActor()가 이 Manager, Hit.Item이 인스턴스 번호)에 해당하는 구체를 모두 제거하고 제거한 수를 반환한다.
	int32 RemoveHitInstances(TConstArrayView<FHitResult> Hits);
	bool TransferParticle(int32 ParticleId, ESwarmPartId DestinationPartId);

	int32 TransferParticlesToTargets(ESwarmPartId SourcePartId, ESwarmPartId DestinationPartId,
		TConstArrayView<FVector> TargetLocalPositions);

	bool RebalanceParts(ESwarmPartId FirstPartId, ESwarmPartId SecondPartId, int32 FirstWeight, int32 SecondWeight);

	// 현재 FormDefinition의 AllocationWeight로 모든 부위의 생존 구체를 재분배한다. 나머지는 RemainderPartId 부위가 받는다.
	bool RebalancePartsFromDefinition();

	bool AssignParticleTarget(int32 ParticleId, const FVector& TargetLocalPosition, float MoveDuration = 0.0f);
	bool AssignReconstructionTargets(const TArray<FVector>& Targets);

	int32 GetActiveParticleCount() const;
	const TArray<FSwarmParticle>& GetParticles() const;
	UInstancedStaticMeshComponent* GetPartInstances(ESwarmPartId PartId) const;

	// 부위 생성·재분배에 쓰는 샘플링 시드. 부위마다 달라 좌우 같은 도형도 다른 배치가 나온다.
	static int32 GetPartSamplingSeed(ESwarmPartId PartId);

	// 중앙 Particles 배열 순서대로 지정 부위의 활성 구체를 최대 MaxCount개 선택해 ParticleId 목록을 반환한다.
	TArray<int32> SelectPartParticleIds(ESwarmPartId PartId, int32 MaxCount) const;

	// 여러 프레임에 걸쳐 구체를 지우는 피해(관통 투사체 등)가 시작·끝날 때 호출한다. 열린 구간이 하나라도 있으면
	// 손실 자동 재분배를 미루고, 모두 닫힌 다음 Tick에 모인 손실을 한 번에 재분배한다. Begin마다 End를 정확히 한 번 호출해야 한다.
	void BeginDamageWindow();
	void EndDamageWindow();
	int32 GetOpenDamageWindowCount() const { return OpenDamageWindowCount; }

	// 손실로 자동 재분배를 기다리는 중이면 true.
	bool IsRebalancePending() const { return bRebalancePending; }

	// 손실 자동 재분배를 켜거나 끈다. 끄면 이미 기다리던 재분배도 취소한다.
	void SetAutoRebalanceOnLoss(bool bEnabled)
	{
		bAutoRebalanceOnLoss = bEnabled;
		bRebalancePending &= bEnabled;
	}

	// PartId 부위의 자세를 Rotation으로 바꾼다(절대 지정, 부모 부위 로컬 축 기준, 기본 회전에 더함).
	// 없는 부위, 최상위 부위, NaN·무한대 회전은 거부하고 기존 자세를 유지한 채 false를 반환한다.
	bool SetPartPose(ESwarmPartId PartId, const FRotator& Rotation);

	// PartId 부위의 현재 자세에 부모 부위 로컬 축 기준 증분 Delta를 더한다. NewPose = (Delta * CurrentPose).GetNormalized().
	// 키 입력처럼 축을 섞어 여러 번 돌릴 때 쓴다. FRotator 각도를 더하는 방식은 축을 섞으면 의미가 달라지므로 쓰지 않는다.
	// 거부 조건과 실패 시 동작은 SetPartPose()와 같다.
	bool AddPartPoseDelta(ESwarmPartId PartId, const FQuat& Delta);

	// 쿼터니언 절대 자세 지정. 애니메이션 경로처럼 매 프레임 절대 자세를 줄 때 쓴다. 거부 조건은 SetPartPose(FRotator)와 같다.
	bool SetPartPose(ESwarmPartId PartId, const FQuat& Rotation);

	// PartId 부위의 현재 자세. 없는 부위는 회전 없음을 반환한다.
	FQuat GetPartPose(ESwarmPartId PartId) const;

	// 숨긴 PoseSource에서 Animation을 단일 애니메이션으로 재생하고, 매 Tick 평가된 뼈 회전을 부위 자세로 옮긴다.
	// 처음이거나 메시·기준 애니메이션·부위 구성이 바뀌었으면 기준 포즈를 먼저 캡처하며, 실패하면 재생하지 않고 false를 반환한다.
	// 비가산 일반 UAnimSequence만 받으며 PoseSource 메시와 스켈레톤이 같아야 한다. bLoop가 false면 끝난 뒤 ReturnPoseAnimation을 반복 재생한다.
	bool PlayPoseAnimation(UAnimSequence* Animation, bool bLoop);

	// 애니메이션 자세 전달을 멈추고 PoseSource 평가도 멈춘 뒤 모든 부위 자세를 기본 자세(회전 없음)로 되돌린다.
	void StopPoseAnimation();

	// 애니메이션 자세 전달 중이면 true. 이때 1~4 키 같은 수동 자세는 매 프레임 덮어써진다.
	bool IsPoseAnimationActive() const { return bAnimatingPose; }

	// 기준 뼈 회전 P·부위 뼈 회전 B(같은 PoseSource 컴포넌트 공간)를 부모 부위 로컬 자세로 바꾼다. 엔진 상태와 무관한 순수 계산이다.
	// D = (P⁻¹ × B) × L0⁻¹(기준 자세 대비 변화, 기준 뼈 좌표계), Q = (C × D × C⁻¹).GetNormalized().
	static FQuat ComputePartPoseFromBones(const FQuat& ReferenceBone, const FQuat& Bone, const FQuat& InverseRestRelative,
		const FQuat& FrameCorrection);

	// 모든 부위의 현재 도형(반크기 + 구체 반지름)을 감싸는 월드 AABB. 구체가 없으면 유효하지 않은 상자를 반환한다.
	// 피해 구간을 연 투사체가 보스를 벗어났는지 판단할 때 쓴다. 구체를 하나씩 보지 않으므로 부위 수만큼만 계산한다.
	FBox GetSwarmBounds() const;

	virtual void Tick(float DeltaTime) override;

protected:
	virtual void BeginPlay() override;

private:
	friend class FSwarmManagerBehaviorTest;
	friend class FSwarmNeighborTransferTest;
	friend class FSwarmAutoRebalanceTest;
	friend class FSwarmJointPoseTest;
	friend class FSwarmPoseAnimationTest;

	// 재구성 목표를 배정한 시점부터 도착까지의 게임 시간(초). 변경한 값은 다음 목표 배정부터 적용한다.
	UPROPERTY(EditAnywhere, Category="Swarm|Movement", meta=(ClampMin="0.01", UIMin="0.01", Units="s"))
	float ReconstructionDuration = 0.8f;

	bool InitializeParticles(TConstArrayView<FSwarmPartPlacement> Placements, double ParticleRadius);
	void AddPartParticles(const FSwarmPartPlacement& Placement, double ParticleRadius);
	void ResetParticles(int32 ExpectedParticleCount);

	bool ValidateFormDefinition(const USwarmFormDefinition& Definition) const;
	void SyncPartInstances(const USwarmFormDefinition& Definition);
	void ApplyPartPlacement(bool bPreserveParticleWorldPositions);
	bool RebalanceToCounts(TConstArrayView<TPair<ESwarmPartId, int32>> PartTargetCounts);

	FVector GetJointWorldLocation(ESwarmPartId FirstPartId, ESwarmPartId SecondPartId) const;

	// Part의 자세 검사를 마친 Pose를 저장하고, 그 부위 ISM의 상대 위치·회전 하나만 다시 설정한다. 자식 부위는 부착 계층으로 따라 돈다.
	// bLog가 false면 호출별 로그를 남기지 않는다(애니메이션처럼 매 프레임 부르는 경로는 1초 집계 로그를 쓴다).
	bool ApplyPartPose(FSwarmPart& Part, const FQuat& Pose, bool bLog = true);

	// 부모 ISM 기준 상대 변환(부위 정의·현재 도형 크기, bWithPose면 저장된 자세 포함).
	FTransform ComputePartRelative(const FSwarmPart& Part, bool bWithPose) const;

	// 자세를 모두 뺀 기본 자세에서 부위 로컬 → Manager 루트 변환. 부모를 따라 재귀로 누적한다(자식상대 * 부모→루트).
	FTransform ComputeRestToRoot(ESwarmPartId PartId) const;

	// 기준 애니메이션 0초 포즈를 PoseSource에서 동기로 평가한 뒤 부위마다 기준 데이터를 만든다. 필요한 뼈가 없거나
	// 메시·애니메이션이 맞지 않으면 경고를 남기고 false를 반환한다(기준 데이터 없이는 자세 전달을 시작하지 않는다).
	bool CapturePoseCalibration();

	// 이번 프레임 평가가 끝난 PoseSource 뼈 회전을 읽어 부위마다 절대 자세로 적용한다. 1회 재생이 끝났으면 복귀 애니메이션으로 바꾼다.
	void UpdatePoseFromAnimation();
	TArray<int32> SelectParticlesNearest(ESwarmPartId PartId, const FVector& WorldPoint, int32 MaxCount,
		const TSet<int32>& ExcludedParticleIds) const;

	FSwarmPart* FindPartByComponent(const UPrimitiveComponent* HitComponent);
	bool UpdateParticleMovement(float DeltaTime);

	// 초기 생성할 부위의 도형·종횡비·배분 가중치를 담은 형태 데이터. 비어 있으면 생성하지 않는다.
	// 부위별 ISM은 생성 시점에 이 데이터의 부위 목록대로 만들어진다.
	UPROPERTY(EditAnywhere, Category="Swarm|Generation")
	TObjectPtr<USwarmFormDefinition> FormDefinition;

	// Play 시작 시 생성할 전체 구체 수. FormDefinition의 가중치 비율로 나누고 정수 배분의 나머지는 잔여 수신 부위에 배정한다.
	UPROPERTY(EditAnywhere, Category="Swarm|Generation", meta=(ClampMin="0", ClampMax="10000"))
	int32 InitialParticleCount = 90;

	// 부위 도형 크기를 정하는 배치 계수 k(필요 부피 = 구체 수 × 간격³ / k). 클수록 도형이 작아져 구체가 촘촘해진다.
	// 너무 크면 샘플링이 실패하거나 도형이 확장된다. 다음 GenerateInitialParts()부터 적용하며, 재분배는 생성 때 값을 유지한다.
	UPROPERTY(EditAnywhere, Category="Swarm|Generation", meta=(ClampMin="0.05", ClampMax="0.7"))
	float PackingCoefficient = 0.5f;

	// 구체를 잃으면(RemoveInstance 성공) 다음 Tick에 RebalancePartsFromDefinition()을 자동으로 호출한다.
	// 피해 구간이 열려 있으면 모두 닫힐 때까지 미룬다. 끄면 T 입력 등으로 직접 호출할 때만 재분배한다.
	UPROPERTY(EditAnywhere, Category="Swarm|Rebalance")
	bool bAutoRebalanceOnLoss = true;

	// 재분배 목표 배정에서 구체마다 살펴볼 가까운 목표 후보 수 k.
	UPROPERTY(EditAnywhere, Category="Swarm|Rebalance", meta=(ClampMin="1", ClampMax="64"))
	int32 AssignCandidateCount = 12;

	// 먼 이동 기준 = 구체 중심 최소 간격 d × 이 값. 이보다 먼 배정은 도미노로 고치며, 고친 사슬 안 이동은 모두 이 기준 이내다.
	UPROPERTY(EditAnywhere, Category="Swarm|Rebalance", meta=(ClampMin="0.5", ClampMax="10"))
	float LongMoveThresholdScale = 2.0f;

	// 재분배 때 부위마다 도미노 고치기를 시도할 최대 수. 시간 상한용이며 넘는 먼 이동은 그리디 결과 그대로 둔다. 0이면 고치지 않는다.
	UPROPERTY(EditAnywhere, Category="Swarm|Rebalance", meta=(ClampMin="0", ClampMax="1000"))
	int32 MaxLongMoveRepairs = 200;

	// 재분배 샘플링에서 활성점 하나마다 시험할 후보 수(Bridson의 k). 초기 생성은 샘플링 설정 값(30)을 그대로 쓴다.
	// 줄이면 샘플링이 빨라지는 대신 배치가 약간 덜 촘촘해질 수 있다(재분배마다 몸 전체를 다시 샘플링하는 비용 절감용).
	UPROPERTY(EditAnywhere, Category="Swarm|Rebalance", meta=(ClampMin="1", ClampMax="100"))
	int32 RebalanceCandidatesPerPoint = 15;

	// 측정용: 구체가 움직이는 동안의 이동 갱신 프레임 수, 합계·최대 시간(ms). 이동이 끝나면 한 줄로 남기고 비운다.
	int32 MovementFrames = 0;
	double MovementTotalMs = 0.0;
	double MovementMaxMs = 0.0;

	// 손실 후 아직 재분배하지 않았으면 true. 열린 피해 구간 수가 0이 된 뒤 Tick에서 처리하고 지운다.
	bool bRebalancePending = false;
	int32 OpenDamageWindowCount = 0;

	FSwarmParticleStore ParticleStore;

	// 마지막으로 생성한 부위별 ISM과 대응표. GenerateInitialParts()가 FormDefinition의 부위 목록과 일치시킨다.
	UPROPERTY()
	TMap<ESwarmPartId, FSwarmPart> Parts;

	// 모든 부위 ISM에 지정할 구체 메시(/Engine/BasicShapes/Sphere).
	UPROPERTY()
	TObjectPtr<UStaticMesh> SphereMesh;

	// 애니메이션을 평가만 하는 숨긴 스켈레탈 메시. 화면에 보이지 않아도 뼈를 갱신하고, 재생 중에만 Tick한다.
	UPROPERTY(VisibleAnywhere, Category="Swarm|Animation")
	TObjectPtr<USkeletalMeshComponent> PoseSource;

	// PoseSource에 쓸 마네킹 메시. 바꾸면 다음 재생 때 기준 포즈를 다시 캡처한다.
	UPROPERTY(EditAnywhere, Category="Swarm|Animation")
	TObjectPtr<USkeletalMesh> PoseSourceMesh;

	// 기준 자세 애니메이션. 0초 포즈를 거인 기본 자세에 대응시킨다(서 있는 자세 = 거인 기본 자세).
	UPROPERTY(EditAnywhere, Category="Swarm|Animation")
	TObjectPtr<UAnimSequence> CalibrationAnimation;

	// 1회 재생 애니메이션(공격 등)이 끝난 뒤 반복 재생할 애니메이션.
	UPROPERTY(EditAnywhere, Category="Swarm|Animation")
	TObjectPtr<UAnimSequence> ReturnPoseAnimation;

	// PoseSource 컴포넌트의 Yaw(도). 마네킹 메시는 컴포넌트 공간 +Y가 앞이라 -90도로 Manager 앞(+X)에 맞춘다(PoseAnimation 테스트에서 확인).
	UPROPERTY(EditAnywhere, Category="Swarm|Animation")
	float PoseSourceYaw = -90.0f;

	bool bAnimatingPose = false;
	bool bPoseAnimationLoops = true;

	// 지금 재생 중인 애니메이션(1회 재생이 끝났는지 길이로 판단할 때 쓴다).
	TWeakObjectPtr<UAnimSequence> CurrentPoseAnimation;
	bool bPoseCalibrated = false;
	TArray<FSwarmPartPoseCalibration> PoseCalibration;

	// 기준 데이터를 만든 조건. 바뀌면 다시 캡처한다(비교용이라 약한 참조).
	TWeakObjectPtr<USkeletalMesh> CalibratedMesh;
	TWeakObjectPtr<UAnimSequence> CalibratedAnimation;

	// 측정용 1초 집계: 한 프레임 전체 부위 기준 뼈 읽기·변환 시간과 ISM 자세 적용 시간(ms). PoseSource 평가 비용은 포함하지 않는다.
	int32 PoseFrames = 0;
	double PoseComputeTotalMs = 0.0;
	double PoseComputeMaxMs = 0.0;
	double PoseApplyTotalMs = 0.0;
	double PoseApplyMaxMs = 0.0;
	double PoseLogStartTime = -1.0;
};
