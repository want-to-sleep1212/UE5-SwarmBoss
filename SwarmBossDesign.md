# 군체 보스 설계 및 작업 인수인계

- 최초 작성: 2026-11-09 / 최신화: 2026-11-14.
- 실제 프로젝트: `D:\UnrealProjects\SwarmBoss`, C++: `Source/SwarmBoss`, 엔진: `D:\Epic Games\UE_5.7`.
- 이 문서는 현재 소스와 대화에서 정한 방향을 구분한다. 앞으로 추가할 클래스·함수 이름은 제안이며 이미 존재하는 API가 아니다.
- 별도 인수인계 파일을 만들지 않고 이 문서에 작업 재개 정보를 유지한다. 선택 이유와 실측 후보는 [PortfolioNotes](Docs/PortfolioNotes.md)에 기록한다.

## 1. 작업 재개 지점

**Manager는 `USwarmFormDefinition` Data Asset(`/Game/Data/DA_GiantForm`)의 부위 목록대로 ISM을 만들어 구체를 생성하고, T 재분배도 같은 데이터의 가중치를 사용한다. 부위 위치는 데이터의 연결점·Gap으로 부모 ISM 기준에서 계산하고, 가장 낮은 부위가 Manager 위치(발바닥)에 닿도록 몸통 높이를 자동으로 맞춘다. T 재분배는 모든 부위를 대상으로 부위 트리의 이웃끼리만 동시에 전달하며, 길목이 모자라면 일부 구체가 길목을 건너뛴다. 다음 작은 작업은 관절 피벗과 부위 자세 적용으로 부위를 회전시키는 것이다(7장 3번).** 거인 전체·검·변신을 한 번에 구현하지 않는다. 최근 대화에서 변신 구조를 먼저 검토한 이유는 이 데이터 구조를 나중에 크게 바꾸지 않기 위해서다.

읽을 순서:

1. 이 문서의 현재 구현과 향후 형태 관리 설계.
2. `Source/SwarmBoss/Swarm/SwarmParticleStore.h`: 구체 상태와 ID.
3. `Source/SwarmBoss/Boss/SwarmFormDefinition.h`: 부위 구성 데이터 형식.
4. `Source/SwarmBoss/Boss/SwarmBossManager.h`: 소유 데이터와 공개 기능.
5. 같은 `.cpp`의 `GenerateInitialParts`, `ValidateFormDefinition`, `SyncPartInstances`, `RemoveInstance`, `TransferParticle`, `RebalanceParts`, `RebalanceToCounts`, `RebalancePartsFromDefinition`, `UpdateParticleMovement`.
6. `Source/SwarmBoss/Boss/SwarmTransferPlanner.h/.cpp`: 이웃 전달 계획(관절별 흐름과 길목 건너뛰기).
7. `Source/SwarmBoss/SwarmBossPlayerController.cpp`: 관통 투사체 발사(왼쪽 버튼 단발)/T 입력. 투사체는 `Source/SwarmBoss/Projectiles/SwarmProjectile.h/.cpp`.
8. `Source/SwarmBoss/Swarm/SwarmShapeSamplerTests.cpp`: 기존 동작의 검증 조건.

Preview는 표시 실험용으로 정리되어 있다. 게임플레이를 Preview에 다시 넣거나 삭제한 `USwarmRuntimeComponent`를 복원하지 않는다.

### 작업 방식

- 한 번에 승인받은 작은 기능만 구현한다. 설계·설명 요청은 파일 수정 요청으로 해석하지 않는다.
- 한국어로 설명하며 식별자는 원문을 유지한다. 완료 후 변경 파일·함수, 이유, 실행 흐름·상태 변화, 검증 결과를 설명한다.
- 함수 정의 위에 독립적으로 이해되는 한국어 목적 주석을 단다. 좌표 기준 공간, ID가 가리키는 배열, 입력과 변경 상태·반환 의미를 필요한 만큼 적는다. 단순 setter는 생략 가능하다.
- 서로 다른 논리 단계 사이에 빈 줄을 두고, 과도한 추상화·의미 없는 클래스 분리를 피한다. setter 정리는 현재 보류하고 기능 구현을 우선한다.
- 코드 변경 전 실제 구현과 의존 관계를 확인한다. 문서의 제안을 구현 완료로 간주하지 않는다.
- 커밋과 푸시는 각각 요청받았을 때 수행한다. 커밋 메시지는 한국어. 관련 없는 사용자 변경은 되돌리지 않는다.

### 로컬 작업 트리 주의

Runtime→Manager 통합은 `e5a54f0`, 부위 구성 데이터 형식은 `b5f2cbb`로 커밋했다. 다음 작업자는 `git status --short`부터 다시 확인한다.

`Config/DefaultEngine.ini`와 FirstPerson 외부 Actor 에셋에는 커밋하지 않은 별도 수정이 있다. 설정 변경은 로컬 인증 토큰이므로 커밋하지 않으며 값은 문서·로그에 복사하지 않는다. 에셋은 사용자의 에디터 변경을 임의로 삭제하지 않는다.

레벨에 배치한 `SwarmBossManager` 외부 Actor 파일은 `.gitignore` 허용 목록에 없어 Git이 추적하지 않는다. 새로 받은 저장소에서는 Manager를 배치하고 `Form Definition`에 `DA_GiantForm`을 지정해야 한다.

## 2. 크기와 배치 밀도

구체 반지름은 고정하고, 생존 개체 수에 맞춰 도형의 배치 영역을 균등 축소한다. 도형이나 Manager의 Scale을 바꿔 구체 자체까지 줄이지 않는다.

```text
d = 2 × ParticleRadius + SpacingMargin
밀도 = k / d³
필요 부피 V = N × d³ / k
선형 크기 배율 = (V / 기준 도형 부피)^(1/3)
```

- 최소 중심 간격으로부터 밀도를 정한다. 밀도를 별도 독립 설정으로 두지 않는다.
- 현재 기본 반지름 10cm, 여유 간격 2cm. 샘플러 기본 `PackingCoefficient`는 0.35(`FSwarmSamplingSettings`, Preview가 사용)이고, Manager는 자체 에디터 속성 `Swarm|Generation > Packing Coefficient`(기본 0.5, 0.05~0.7)를 생성 설정에 넣는다. 2026-11-15 사용자 요청으로 0.35에서 0.5로 올렸다(같은 개수에서 각 축 길이 약 0.89배). 구·박스 4종 비율 × 2~2000개에서 확장 없이 생성됨을 `Stage2.ManagerDensity`로 확인했다. 값은 시험값이며 확정하지 않았다.
- 부위별 시드(2026-11-15): 모든 부위가 샘플러 기본 시드를 같이 쓰면 도형·비율·개수가 같은 좌우 부위가 똑같은 무늬로 나와 대칭이 눈에 띈다. `ASwarmBossManager::GetPartSamplingSeed`가 기본 시드에 PartId × 7919를 더한다. 부위끼리 결과를 재사용하지 않으므로 연산량은 같다. 같은 부위는 같은 시드라 같은 개수면 같은 배치를 재현한다.
- k가 작을수록 넉넉한 영역을 확보한다. 확장을 드문 예외로 만들도록 테스트로 튜닝한다. 생성 시도·후보 검사 예산·확장량에 상한을 둔다.
- 목표 수만 채우고 중단하지 않고 영역에 후보를 분포시킨 후 필요한 수를 선택한다. 개수·간격 검사만으로 시각적 균질성까지 보장되지는 않는다.
- 경계는 구체 중심의 허용 영역이며 구체 표면은 반지름만큼 돌출될 수 있다.
- 목표 간격은 각 도형 내부 기준이다. 서로 다른 도형의 연결부까지 비겹침을 보장하지 않는다. 이동 중 겹침은 허용한다.

### 생성 후 전수 검사의 사용 단계 — 2026-11-11 결정

2026-11-15 적용: `GenerateTargets()`의 생성 성공 경로에서 O(N²) `ValidateTargets()` 호출을 뺐다. 결과의 `MeasuredMinimumDistance`는 이제 채우지 않으며(0), 진단용 Preview만 생성 후 따로 `ValidateTargets()`로 잰다(생성 시간에는 포함하지 않음). 생성 중 공간 격자로 검사하는 거리·경계와 입력 검증·목표 개수·실패 처리는 계속 유지한다. 검증 함수와 자동 테스트는 삭제하지 않는다.

- [x] 최초 생성·재구성의 일반 실행 경로에서 중복 전수 검사 제외(2026-11-15). 변신도 같은 `GenerateTargets()`를 쓰므로 따로 할 일이 없다.
- [x] 자동 테스트의 독립 간격·경계 검사 유지 및 재검증(`SamplingInvariants`, `EdgeCasesAndFailure`가 `ValidateTargets()`를 직접 호출).
- [ ] 검사 포함/제외를 명시해 생성 시간 비교.

## 3. 현재 구현과 실행 흐름

### 소유 구조

```text
ASwarmBossManager
 ├─ FormDefinition: USwarmFormDefinition Data Asset 참조(DA_GiantForm)
 ├─ SphereMesh: 모든 부위 ISM의 구체 메시(/Engine/BasicShapes/Sphere)
 ├─ FSwarmParticleStore
 │   ├─ Particles: 중앙 구체 배열, 슬롯 인덱스가 ParticleId
 │   └─ ActiveParticleCount: 전체 생존 수
 └─ Parts: TMap<ESwarmPartId, FSwarmPart>   마지막 생성 데이터의 부위 목록과 일치
     ├─ Instances: 부위별 ISM(생성 시 동적 생성, 이름 "부위명Instances", 부모 부위 ISM에 부착)
     ├─ Definition: 마지막 생성에 쓴 부위 정의 복사본(부모·연결점·Gap·회전)
     ├─ HalfExtents: 현재 목표 배치의 도형 반크기, 위치 계산에 사용
     ├─ InstanceToParticle: 해당 ISM 번호 → 중앙 ParticleId
     └─ SamplingSettings: 마지막 초기 생성에 성공한 설정
```

`USwarmRuntimeComponent`의 생성·제거·위치 갱신·Tick은 Manager에 통합했다. `ASwarmShapePreview`는 독립적인 샘플링 표시 Actor로 충돌·중앙 저장소·게임플레이 입력을 담당하지 않는다.

`FSwarmParticle`은 `LocalPosition`, `TargetLocalPosition`, `RemainingMoveTime`, `bActive`, `InstanceIndex`, `PartId`를 가진다. 위치·목표는 소속 부위 ISM 기준이다. enum은 `SwarmPartTypes.h`로 분리했고 거인의 머리·오른팔·양손·양다리·양발 및 None을 추가했다. Manager는 데이터에 있는 부위를 모두 생성할 수 있지만, `DA_GiantForm`에는 아직 몸통·왼팔만 있다.

### 생성

`BeginPlay → GenerateInitialParts → ValidateFormDefinition → AllocatePartCounts → GenerateTargets(각 부위) → SyncPartInstances → InitializeParticles → ResetParticles → AddPartParticles → Store.AddParticles → ApplyPartPlacement`.

- Manager Details `Swarm|Generation`의 `Form Definition`에 지정한 Data Asset을 읽는다. 비어 있으면 `FormDefinition is not assigned` 경고를 남기고 생성하지 않는다. 코드 기본값으로 대체 생성하지 않는다.
- `InitialParticleCount` 기본 90, 에디터 범위 0~10000. 각 부위는 `전체 × AllocationWeight / 가중치 합`을 버림한 수를 받고, 버림 나머지는 `RemainderPartId`(몸통)가 받는다.
- 부위의 도형·Box 비율은 데이터의 `Shape`, `BoxHalfExtentsRatio`를 쓴다. `DA_GiantForm`의 비율은 몸통 (1, 0.75, 1.5), 왼팔 (0.45, 0.45, 1)이다. 가중치는 이전 코드 상수 2:1로 시작했고 이후 10:1, 5:1 등을 시험했다. 가중치와 부위 구성은 확정값이 아니며, 현재 값은 에셋을 열어 확인한다.
- 생성 전에 `ValidateFormDefinition`이 ID 중복·None/Preview, 음수 가중치·가중치 합 0, 비율, 연결점 범위(-1~1), 음수 Gap, 회전 값, 자기 부모·미정의 부모, 몸통 외 최상위 부위, 순환, 잔여 수신 부위를 검사한다. 실패 시 원인을 경고로 남기고 기존 구체를 유지한다. Clamp 메타데이터는 런타임 검증으로 간주하지 않는다.
- 부위 ISM은 생성자에서 만들지 않는다(2026-11-14 변경). 모든 부위의 샘플링이 성공한 뒤 `SyncPartInstances`가 데이터 목록에 없는 부위의 ISM을 파괴하고, 새 부위의 ISM을 만들어 등록한 뒤 부모 부위 ISM(최상위는 루트)에 붙인다. 이미 있는 부위의 ISM은 재사용한다. 그래서 Play 전 에디터 Details에는 부위 ISM이 보이지 않는다. 부위 ISM은 `GetPartInstances(PartId)`로 조회한다.
- ISM 이름은 `TorsoInstances`처럼 "부위명Instances"이다. 파괴 직후 같은 이름이 남아 있으면 번호가 붙을 수 있으므로 이름으로 부위를 찾지 않는다.
- 모든 부위의 샘플링 결과가 성공한 뒤 중앙 저장소를 한 번 초기화하고 각 부위를 추가한다. `AddParticles`는 첫 중앙 ID를 반환하므로 두 번째 부위가 첫 번째를 덮어쓰지 않는다.
- 실제 `AddInstances` 반환 인덱스로 양방향 연결을 채운다. 중앙 ID와 ISM 인덱스가 같다고 가정하지 않는다.
- 생성 때만 초기 개수 설정을 사용한다. 이후 전체 생존 수의 기준은 Store다.

### 부위 위치 규칙 — 2026-11-14

부위마다 붙는 방식은 다르지만 PartId별 코드 분기는 두지 않는다. 모든 부위가 "부모 경계의 한 점에 자식 경계의 한 점을 맞춘다"는 같은 계산을 쓰고, 차이는 데이터 값으로 표현한다. PartId 분기는 반복되는 검 부위를 구분할 수 없고 값을 다시 코드에 넣게 된다.

- 데이터 필드: `ParentAnchor`, `ChildAnchor`(각 축 -1~1, 현재 반크기에 대한 비율), `Gap`(cm, 0 이상), `RelativeRotation`. 이전 `RelativeLocation`(고정 cm)과 Manager의 `PartGap` 속성은 제거했다.
- 계산(부모 ISM 기준): `자식 중심 = 부모 연결점 + 면 바깥 방향 × (Gap + 부모 구체 반지름 + 자식 구체 반지름) − 회전된 자식 연결점`. 도형 경계는 구체 중심 허용 영역이므로 반지름을 더해야 `Gap`이 화면의 관절 구체 표면 사이 거리가 된다. Gap 0이면 관절 구체가 맞닿는다.
- 면 바깥 방향: 박스는 비율이 ±1인 축들(모서리면 합 방향), 면 위가 아니거나 구형이면 중심에서 연결점 방향, 연결점이 0이면 띄우지 않는다. 구형 도형에서 비율 점이 구 밖이면 구 표면으로 당긴다.
- 연결점이 비율이므로 생존 수에 따라 도형이 줄어도 관절이 표면을 따라간다. 회전 여유를 위해 관절을 일부러 띄우는 용도로 `Gap`을 쓰고, 이동·회전 중 부위 간 일부 겹침은 현재 허용한다(사용자 결정).
- ISM 계층: 최상위 부위는 Manager 루트, 나머지는 부모 부위 ISM에 붙인다. 부모를 움직이거나 회전하면 자식이 따라가므로 3단계 행동에서 관절 계층으로 쓴다. 부모가 바뀐 데이터로 재생성하면 순환 부착을 피하려고 모두 루트에 붙였다가 다시 붙인다.
- 0개 부위는 반크기 0인 점으로 다룬다. 자식은 그 점에 붙으며 별도 처리는 하지 않는다.
- 바닥 맞춤(2026-11-15): Manager 위치는 거인의 발바닥이다. 모든 부위를 놓은 뒤, 구체가 있는 부위 중 가장 낮은 구체 표면(도형 경계의 가장 낮은 점 − 구체 반지름, 박스는 회전을 반영한 8개 꼭짓점 기준)이 Manager 높이(루트 Z=0)에 오도록 최상위 부위의 Z만 올린다. 자식 ISM은 붙어 있어 함께 올라가며 X/Y는 바꾸지 않는다. 다리가 있으면 다리 끝, 없으면 몸통 바닥이 기준이 된다. 0개 부위는 화면에 없으므로 제외한다.
- 바닥은 실제 남은 구체가 아니라 현재 도형 범위로 잡는다. 실제 구체 기준이면 아래쪽 구체를 맞힐 때마다 몸이 들썩이기 때문이다. 따라서 높이는 생성과 재분배 때만 바뀌고, 재분배 때는 다른 관절과 같이 구체가 서서히 내려앉거나 올라간다. 레벨의 Manager는 바닥 높이에 놓는다.
- 초기 생성 후와 재분배로 부위 크기가 바뀔 때 `ApplyPartPlacement`로 모든 부위 위치를 다시 계산한다. 재분배 때는 위치가 바뀐 부위의 구체를 같은 월드 위치에 남기도록 로컬 좌표를 바꾸고, 기존 로컬 목표로 `ReconstructionDuration` 동안 다시 이동시킨다. 따라서 부위가 순간 이동하지 않고 관절이 서서히 맞춰진다.
- 위치 계산은 생성 당시 복사한 부위 정의(`FSwarmPart::Definition`)를 쓰므로 Play 중 에셋을 바꿔도 다시 생성하기 전까지는 반영되지 않는다.

### 피격과 삭제

`왼쪽 버튼 → PlayerController.TryFireProjectile(쿨타임 확인) → ASwarmProjectile.Advance(매 Tick 이동 구간을 구로 SweepMultiByObjectType) → Manager.RemoveHitInstances → Manager.RemoveInstance → Store.DeactivateParticle`.

- 2026-11-15: 구체를 하나씩 클릭해 지우기가 번거로워(사용자 요청) 디버그 클릭 Trace를 투사체로 바꿨다. 이전 `TraceSwarmParticle`은 삭제했다. 처음 구현한 "선 충돌 + 맞은 지점 반경 40cm 제거 + 연사"는 사용자 의도(관통 구체, 겉모습 = 충돌 크기)와 달라 같은 날 아래 방식으로 대체했다.
- 발사(사용자 결정): 왼쪽 버튼을 누를 때마다 한 발(단발), 쿨타임 `FireCooldown` 2초. 카메라 위치에서 `MuzzleOffset`(50cm) 앞에 생성해 카메라 정면으로 `ProjectileSpeed`(1500cm/s, 날아가는 모습이 보이는 속도)로 날린다. 3초 수명이라 사거리는 약 45m다. 값은 PlayerController의 `Swarm|Attack`에서 바꾼다.
- 관통 구체(사용자 결정): 반지름 `ProjectileRadius` 15cm, 겉모습과 충돌 크기가 같다. 관통 한도 없이 경로에 닿는 구체를 모두 지우며 계속 날아가므로 지름 2 × (15 + 10) = 50cm 터널이 생긴다. 벽(WorldStatic·WorldDynamic)에 닿으면 그 앞까지만 지우고 사라진다. 발사자와 그 Pawn은 무시한다.
- 판정은 엔진 조회 방식(가)이다. 투사체에는 충돌 컴포넌트가 없고, 매 Tick 이번 이동 구간을 같은 반지름의 구로 `SweepMultiByObjectType`(WorldStatic·WorldDynamic·`ECC_GameTraceChannel3 = SwarmSphere`) 조회한다. ISM 인스턴스마다 있는 충돌 바디를 엔진 물리 장면의 공간 자료구조가 찾아 주므로 직접 만든 O(N) 순회나 격자가 필요 없다. 충돌체 + `ProjectileMovementComponent` + Overlap 이벤트(나)와 비교해, 구체 쪽 충돌 프로필(`SwarmSpherePreset`)을 바꾸지 않아도 되고 `Advance()`로 테스트할 수 있어 골랐다. 두 방식 모두 매 프레임 갱신이 있다(Actor Tick 또는 컴포넌트 Tick).
- `RemoveHitInstances`는 맞은 (부위 ISM, 인스턴스 번호)를 ParticleId로 바꿔 먼저 모은 뒤 하나씩 제거한다. 교환 삭제로 다른 구체의 인스턴스 번호가 바뀌므로 제거마다 ParticleId로 현재 번호를 다시 읽는다. 같은 구체의 중복 결과와 다른 Actor 결과는 건너뛴다.
- 단일 제거 `RemoveInstance`는 피격 컴포넌트로 부위를 찾고 인스턴스 번호를 그 부위의 `InstanceToParticle`로 변환한다.
- ISM 교환 삭제가 성공하면 중앙 개체를 비활성화하고 생존 수를 줄인다. 역방향 배열도 교환 삭제하고 이동한 마지막 개체의 `InstanceIndex`를 고친다.
- 중앙 슬롯은 삭제·압축·재사용하지 않는다. ID는 한 번의 초기화 이후 생존 기간 동안 유지된다. 슬롯 풀, Generation Handle, 제거 대기 큐는 현재 구현하지 않는다.
- 제거 후 자동 재구성은 없다. T 입력으로 요청한다. 실패 시 다음 프레임 자동 재시도도 없다.

### 부위 이전

`TransferParticle(ParticleId, DestinationPartId)`는 현재 월드 Transform과 기존 목표 월드 위치를 목적지 부위 로컬로 변환한다. 목적지 ISM 추가 후 출발지 제거를 수행하며 출발지 제거 실패 시 방금 추가한 목적지 인스턴스를 되돌린다. 성공 시 양쪽 대응표와 중앙 PartId/InstanceIndex/로컬 좌표를 바꾼다. ID·생존 수·남은 이동 시간은 유지하고, 자체적으로 새 목표를 정하거나 Tick을 켜지는 않는다.

`SelectPartParticleIds`는 중앙 배열 순서로 활성 개체를 선택한다. 재분배의 부위 간 전달은 관절에 가까운 순서로 고르는 `SelectParticlesNearest`를 쓰며, 아직 공간 자료구조 없이 부위 전체를 정렬한다. `TransferParticlesToTargets`는 선택한 개체를 이전하고 전달 성공 개체에 목적지 로컬 목표를 순서대로 준다. 반환값은 실제 소속 이전 성공 수이며 실패 개체만 건너뛴다.

### T 키 재분배

`T → RebalanceSwarmPartsForTest → 각 Manager.RebalancePartsFromDefinition → AllocatePartCounts → RebalanceToCounts`.

`RebalancePartsFromDefinition`은 T 입력 시점의 `FormDefinition`을 다시 검증하고 부위 가중치만 읽는다. 모든 부위의 현재 생존 수 합계를 초기 생성과 같은 규칙(가중치 비율 버림, 나머지는 `RemainderPartId`)으로 나눠 부위별 목표 개수를 정한다. 도형 종류·비율은 마지막 초기 생성 때 보관한 설정을 사용한다. 데이터가 없거나 유효하지 않거나, 생성 후 데이터의 부위 목록이 바뀌어 생성된 ISM과 다르면 경고 후 상태를 바꾸지 않는다. 부위 목록이 바뀌었으면 다시 생성해야 한다. 가중치 0인 부위는 구체를 모두 다른 부위로 보낸다.

2026-11-14 결정: 부위가 셋 이상일 때는 연결 관계를 따르지 않고 초과 부위에서 부족 부위로 직접 보내는 방식(B)을 먼저 구현했다. 거인 10부위의 크기·비율·관절을 시험하려면 T가 필요하고, 직접 이동 코드는 변신에서도 재사용할 수 있다. 손에서 머리로 몸을 가로지르는 이동이 눈에 띄면 이웃 부위끼리만 전달하는 방식(C, 7장 4단계)으로 바꾼다.

2026-11-15 결정: 최종 목표인 이웃 전달(C)로 바꿨다(사용자 결정). 직접 전달은 더 이상 쓰지 않는다.

- 흐름 계산: 부위 트리에서 관절마다 자식 쪽 전체(자식과 그 아래 부위들)의 (현재 − 목표) 합만큼 구체가 지나간다. 양수면 자식 → 부모, 음수면 부모 → 자식. 트리에는 순환이 없어 관절마다 흐름이 하나로 정해진다.
- 동시 출발: 말단부터 계산하므로 단계를 기다리지 않고 모든 관절의 전달을 한 번에 시작한다(사용자 제안). 각 부위는 지금 가진 구체로 내보내고 동시에 받는다. 예: 머리만 −6이면 손→팔 3, 팔→몸통 4, 몸통→머리 6이 함께 출발한다.
- 길목 건너뛰기: 한 부위만 맞으면 생기지 않지만, 중간 부위와 그 아래 부위가 함께 크게 맞으면(예: 왼팔 2개 남음, 왼손도 부족) 중간 부위가 넘겨야 할 수가 가진 수보다 많다. 이때 그 부위로 들어오는 구체 중 모자란 수만큼이 경로를 연장해 길목을 건너뛰고 다음 부위로 바로 간다(예: 몸통→왼팔→왼손). 내보낼 수 − 받을 수 = 현재 − 목표 ≤ 현재이므로 항상 채울 수 있다. 가진 만큼만 보내기, 도착 후 2차 전달과 비교해 한 번에 정확히 맞는 이 방식을 골랐다.
- 구체 선택: 묶음마다 출발 부위에서 첫 관절(부모 연결점과 자식 연결점의 가운데)에 가장 가까운 구체부터 고른다. 이번 재분배에서 이미 옮긴 구체는 다시 고르지 않는다. 공간 자료구조 없이 부위 구체 전체를 거리순 정렬한다.
- 이동: 현재는 출발 위치에서 최종 목표까지 직선이다. 다음 단계에서 길목을 건너뛰는 구체만 관절 단면을 지나게 한다. 단면은 관절 위치를 지나고 연결 방향에 수직인 원판이며, 구체마다 출발 위치를 그 면에 내린 점을 통과해 한 점으로 몰리지 않는다. 관절 하나만 건너는 구체는 관절 근처에서 출발하므로 직선으로 둔다(사용자 결정). 경유 지점은 좌표가 아닌 관절로 저장해 나중에 부위가 움직여도 따라가게 한다.

`RebalanceToCounts(부위별 목표 개수)`가 공통 처리를 맡는다. 두 부위 API인 `RebalanceParts(First, Second, 가중치, 가중치)`도 두 부위 목표 개수(두 번째 버림, 첫 번째 나머지)를 계산해 같은 함수를 호출한다.

1. 목록 부위의 목표 개수 합이 현재 개수 합과 같은지, 부위 중복·누락이 없는지 확인한다. 가중치는 0 이상, 합은 양수여야 한다.
2. 모든 부위가 이미 목표 개수와 같으면 성공 반환하며 샘플링·이동 재시작을 하지 않는다. 비율이 맞으면 결손이 있어도 새 배치를 만들지 않는 현재 제한이 있다.
3. 모든 부위의 새 목표를 먼저 생성한다. 생성 실패까지는 기존 상태를 유지한다. 생성 성공 후 새 도형 크기로 `ApplyPartPlacement`를 호출해 모든 부위 관절 위치를 다시 계산한다(구체 화면 위치 유지).
4. `FSwarmTransferPlanner::Plan`으로 이웃 전달 계획을 만들고, 묶음마다 관절에 가까운 구체를 골라 최종 부위로 소속을 바꾼다. 목록에 없는 부위는 개수를 바꾸지 않고 길목 역할만 한다. 예: 몸통 +4, 왼팔 +1, 머리 −5이면 왼팔→몸통 1개, 몸통→머리 5개.
5. 전달 성공 후 모든 대상 부위의 활성 구체에 목표를 순서대로 배정한다. 최근접 목표 탐색은 하지 않는다.

관찰(2026-11-15, 버그 아님): 몇 개만 제거하고 T를 누르면 일부 부위만 바뀐다. 버림 배분이라 작은 손실로는 대부분 부위의 목표 개수가 그대로이고 남는 수를 받는 몸통이 손실을 떠안는다. 개수가 같은 부위는 샘플러 시드가 고정이라 새 목표도 같아서 움직이지 않는다. 어느 부위가 함께 바뀌는지는 비율의 소수점 경계에 달려 있어 무작위처럼 보인다. 사용자 판단으로 지금은 유지한다. 이웃 전달로 바꾼 뒤에도 배분 규칙은 그대로이며, 필요하면 따로 다시 정한다.

예: 초기 2000개는 몸통1334/왼팔666. 몸통12개 제거 후1322/666, 합계1988의 목표는1326/662이므로 왼팔에서 몸통으로4개 이동한다.

부분 전달 실패 시 성공한 이전은 유지하고 false를 반환한다. 현재 Rebalance는 전체 원상복구를 보장하지 않는다. 향후 변신의 준비/적용 실패 정책을 이 함수가 이미 해결했다고 가정하지 않는다.

### 이동

`AssignParticleTarget → Store.AssignTarget → 필요 시 Manager Tick 활성화 → UpdateParticleMovement`.

- `ReconstructionDuration`은 에디터 `Swarm > Movement`에서 수정, 기본 0.8초. 다음 목표 배정부터 적용되며 이미 이동 중인 개체의 남은 시간은 소급 변경하지 않는다.
- 양수 시간: 남은 거리가 남은 시간의 제곱에 비례하는 이차 감속. 0 시간: 기존 200cm/s 일정 속도 이동. 부위 재분배와 `AssignReconstructionTargets`는 설정된 양수 시간을 쓴다.
- 전체 중앙 배열을 순회하고 비활성/도착 개체를 건너뛴다. ISM 위치 갱신이 성공한 경우에만 Store 위치와 남은 시간을 갱신한다.
- 변경된 ISM별로 프레임당 한 번 `MarkRenderStateDirty`를 호출한다. 이동이 없으면 Manager Tick을 끈다.
- 현재 이동 갱신의 bool은 성공적으로 갱신한 개체 중 미도착 개체 유무다. 잘못된 부위/인덱스나 갱신 실패를 건너뛰므로, 향후 변신에서 이것을 그대로 ‘모두 정상 도착’ 판정으로 사용하면 안 된다.
- `AssignReconstructionTargets`는 전체 활성 배열 순서로 목표를 준다. 각 좌표는 개체의 현재 부위 로컬로 해석하며, 형태·부위 배분을 수행하지 않는다. 현재 T 경로는 이 함수 대신 Rebalance에서 부위별로 배정한다.

## 4. 확정한 방향과 보류한 범위

| 항목 | 결정 / 현재 상태 |
|---|---|
| 목표 형태 | 거인 ↔ 고정 N자루 검의 완전 변신. 한 번에 한 형태로 활동 |
| 도형 구성 | 거인과 검 모두 여러 기본 도형으로 구성. 검날·가드·손잡이 연결도 관절부 |
| 크기 | 구체 크기 고정, 생존 수에 맞춰 각 도형의 비율을 유지하며 축소 |
| 배분 | 비율에 따라 정수 버림, 1 미만 도형은 0개. 거인 잔여는 몸통 |
| 공간/이동 | 부위 로컬 좌표, 이동 중 겹침 허용 |
| 일반 재분배 | 인접 부위 연결을 통한 이동이 목표. 연결 그래프·관절 후보 검색은 미구현 |
| 변신 | 인접 부위 제한 없이 목적지로 직접 이동, 변신 중 무적 |
| 매칭 | 현재 순차 배정 유지. 최적해 보장·최근접 검색은 필수 요구가 아님 |
| 재구성 연출 | 0.2초 → 0.4초 감속 → 에디터 조절 후 0.8초 선택. 최종 고정값은 아님 |
| 저장소 | Manager 소유. 비활성 슬롯 재사용은 데모에서 보류 |
| 최적화 | 이동 ID 전용 목록·관절 격자는 필요성 확인 후. 포트폴리오용이라는 이유만으로 추가하지 않음 |

좌우 반전 목표 재사용, 개체별 Noise는 초기 설계 방향이지만 미구현이다. 최근 최소 구현 범위에 자동으로 포함하지 않는다.

## 5. 향후 형태 관리 구조

아래는 최근 검토한 구현 방향이다. 클래스 이름, 데이터 스키마, 실패 처리의 세부 구현은 다음 단계에서 확정한다. 현재 소스에는 형태 전환 상태나 무적이 없다.

```text
ASwarmBossManager                         기존, 계속 중앙 소유
 ├─ FSwarmParticleStore                   기존, 변신 때 초기화하지 않음
 ├─ 부위별 ISM/대응표                     기존 구조를 데이터 기반으로 확장
 ├─ USwarmFormDefinition                  거인용 데이터 형식 추가, Manager 연결 전
 ├─ USwarmGiantBehaviorComponent          제안: 거인 동작
 └─ USwarmSwordBehaviorComponent          제안: 검 N자루의 동작
```

거인 Manager/검 Manager로 상속 분리하지 않고 하나의 Manager가 공통 구체 처리를 맡는다. 형태별 행동은 부위·그룹의 위치와 회전을 결정하고 Manager에 적용을 요청한다. 행동 컴포넌트가 중앙 배열이나 인스턴스를 직접 변경하지 않는다. 공통 행동 기반 클래스나 별도 변신 클래스는 실제 중복이 생기기 전에는 만들지 않는다.

형태 데이터 후보: 형태 종류, 부위 도형·종횡비·개체 비율·상대 자세, 그룹(거인/몇 번째 검), 부모/연결 관계, 연결부 기준점, 잔여를 받을 대표 부위, 대칭 관계. 검 전체를 움직일 그룹 기준 Transform과 도형 연결 관계는 구분한다. 부위 연결은 우선 그룹별 트리를 고려한다.

현재 enum만으로는 여러 검의 같은 ‘검날’을 구분할 수 없다. 사용자가 원하는 의미 있는 enum 식별을 존중하되 ‘부위 종류 + 검 그룹 번호’와 실행 중 고유 부위 ID의 관계를 데이터 작업에서 정해야 한다. `Blade1`, `Blade2`를 무작정 추가하는 설계로 굳히지 않는다.

### 거인 구성 데이터 형식 추가 — 2026-11-14

- `Source/SwarmBoss/Swarm/SwarmPartTypes.h`: 기존 enum을 Store에서 분리. 기존 숫자 값은 보존하고 거인의 10부위와 부모 없음/미지정용 None을 표현한다. enum 추가만으로 해당 부위를 생성하지는 않는다.
- `Source/SwarmBoss/Boss/SwarmFormDefinition.h`: `FSwarmPartDefinition`과 `USwarmFormDefinition` 선언. 별도 실행 함수나 Tick은 없다.
- 부위별 필드(최초): `PartId`, `ParentPartId`, `Shape`, `BoxHalfExtentsRatio`, `AllocationWeight`, `RelativeLocation`, `RelativeRotation`. Scale은 없다. 이후 `RelativeLocation`은 `ParentAnchor`·`ChildAnchor`·`Gap`으로 교체했다(3장 부위 위치 규칙).
- 형태 데이터는 `Parts` 목록과 잔여 수신 부위 `RemainderPartId`(몸통)를 보관한다. 실제 구체 수·샘플러 설정 전체·ISM은 넣지 않는다.
- 검 그룹/중복 부위 식별, 형태 상태, 행동 컴포넌트, 관절 검색은 이번 범위에서 제외했다.
- 검증: UE Editor 타깃 빌드와 UHT 성공, 기존 자동 테스트 10개 성공/실패0(`Saved/Automation/GiantDefinition/index.json`).

### Manager의 형태 데이터 연결 — 2026-11-14

- 값을 둘 곳으로 Data Asset과 C++ 기본값을 비교했다. 거인/검 형태를 데이터 하나씩 갈아 끼우는 변신 구조와 빌드 없는 값 수정을 위해 Data Asset을 선택했다. 값이 diff로 보이지 않고 에셋을 Git 허용 목록에 추가해야 하는 비용이 있다.
- 사용자가 에디터에서 `/Game/Data/DA_GiantForm`(`Content/Data/DA_GiantForm.uasset`)을 만들고 Manager에 지정했다. 처음에는 이전 코드 상수와 같은 값(몸통 루트·(1, 0.75, 1.5)·가중치 2, 왼팔 부모 몸통·(0.45, 0.45, 1)·가중치 1)으로 만들었다. 이후 사용자가 가중치를 몸통:왼팔 10:1로 바꿔 시험했으며 이 값으로 커밋했다. 가중치는 아직 확정하지 않은 튜닝값이다.
- `GenerateInitialParts`와 T 재분배가 이 데이터를 읽는다. 세부 흐름·검증 규칙은 3장 생성과 T 키 재분배에 있다.
- 에셋이 비어 있으면 생성 실패로 처리한다. 코드 기본값 대체 경로를 두면 같은 값이 두 곳에 남는다.
- 자동 테스트는 에셋 대신 `NewObject`로 같은 값의 데이터를 만들어 사용한다. 실제 에셋 값은 사용자 Play로 확인했다.
- 순환 연결 검사는 구현했지만 현재 두 부위로는 다른 검사보다 먼저 걸리는 순환을 만들 수 없어 자동 테스트가 없다. 부위 추가 시 테스트를 추가한다.

### 상태와 공통 기능

제안 상태는 `CurrentForm`, `TargetForm`(Giant/Swords), `FormState`(Stable/Transforming)이다. CurrentForm은 마지막으로 완성한 형태다. 거인→검 전환 중에는 Current=Giant, Target=Swords, State=Transforming이고 완료 후 Current=Swords, State=Stable이 된다.

Manager 공통 기능: 생존 수 관리, 배분, 샘플러 호출, 목표 배정, ISM 이전·삭제·갱신, 변신 상태, 피격 허용 판정. 형태 데이터는 ‘모양’, 행동 컴포넌트는 ‘완성된 형태의 동작’을 맡는다.

무적은 사용자 결정이다. 수천 구체 충돌을 켰다 끄는 대신 Manager의 피격 제거 진입점에서 Transforming이면 거절하는 방식을 추천했다. 발사 금지/투사체 충돌 끄기는 대안이며 최종 연결 방식은 미구현이다. 중앙 거절 방식은 충돌 비용을 없애는 것이 아니며, 충돌한 투사체 소멸 여부는 별도 정책이다.

### 변신 실행 제안

1. `RequestFormChange(NewForm)`: 같은 형태/변신 중 요청, 형태 데이터와 생존 수를 확인. 변신 중 재요청은 우선 거절하는 제안이며 큐나 중간 취소를 만들지 않는다.
2. `BuildFormChangePlan`: 생존 수로 목적지 부위별 수·크기·목표·ParticleId별 목적지를 임시 계산한다. 준비 실패 시 현재 형태와 소속을 유지한다.
3. `BeginFormChange`: Transforming 진입, 기존 행동 중지, 목적지 ISM 준비, 현재 위치를 유지하며 목적지 부위에 소속 이전, 이동 목표 배정. 동일한 중앙 ID를 사용한다.
4. 이동: 기존 시간 기반 감속을 활용. 인접 제한 없이 직접 이동하며 겹침 허용. 목적지 부위 자세는 변신 동안 고정하고 완료 후 행동 시작하는 안을 우선한다.
5. `FinishFormChange`: 실제 대상 개체의 도착과 갱신 실패 여부를 확인하고 현재 형태 확정, Stable 복귀, 새 행동 시작, 피격 허용.

별도 공통 좌표 저장 상태를 필수로 도입하지 않는다. 출발 부위 로컬 → 월드 → 목적지 부위 로컬로 현재 위치와 목표를 바꾸면 기존 이전 방식을 재사용할 수 있다. 월드는 변환 중간 기준이며 구체를 항상 월드 좌표로 보관한다는 뜻이 아니다. 기존 형태 ISM은 이전 완료 후 정리하며 구체를 중복 생성하지 않는다.

적용 도중 일부 ISM 이전 실패에 대한 복구/중단 처리, 모든 구체 제거 시 정책, 재구성 이동 중 변신 요청 정책은 구현 전에 정할 사항이다. 준비 성공만으로 적용 전체의 성공이 보장되는 것은 아니다. 현재 Tick은 구체 이동만 없으면 꺼지므로 향후 형태 행동의 지속 갱신과 변신 상태 확인까지 고려해 Tick 종료 조건을 수정해야 한다.

## 6. 일반 재분배와 공간 검색의 후속 설계

형태 변신과 구분한다. 일반 재분배의 초기 목표는 연결된 부위끼리 필요한 수를 전달하는 것이다. 말단부터 ‘현재 수 - 목표 수 + 자식들의 여유’를 계산해 각 연결의 방향·양을 정하고, 실제 공급 가능한 개체를 확인하며 실행하는 방안을 검토했다. 0개 부위도 연결·자세 정의는 유지하고, 중간 부위를 통과하는 공급과 중복 예약 방지를 검증해야 한다.

관절 주변만 별도 저장하면 안쪽 개체 보충이 어려우므로 도형 전체를 성긴 로컬 균일 격자로 나누는 방향을 고려했다. 관절 근처에서 범위를 넓혀 충분히 가까운 후보를 고르며 최적해를 보장할 필요는 없다. 차등 셀 크기는 대안이며 초기 구현 필수가 아니다.

샘플러의 격자는 목표 생성용으로 이미 존재하지만, 실제 살아 있는 개체 검색 격자는 아직 없다. 두 용도를 섞지 않는다. 실제 격자는 내부 이동·제거·소속 변경 때 갱신하고 도형 전체 이동·회전만으로 다시 만들지는 않는다.

최근 감속 재구성의 시각 결과가 수용 가능했으므로 최근접 매칭·격자는 바로 구현하지 않는다. 필요하면 이동 거리/화면 결과와 유지 비용을 측정한 뒤 추가한다. 검별 배분 잔여와 검 내부 대표 부위, 서로 떨어진 검 사이 일반 재분배, 극소수에서 N자루를 표시할 수 없는 경우는 아직 세부 정책이 없다.

## 7. 다음 구현 순서

1. ~~부위 구성 데이터 설계 → 기존 몸통·왼팔을 그 데이터로 생성. 현재 클릭·전달·재분배 동작 유지.~~ 완료(2026-11-14).
2. 거인 전체 부위 추가. 하위 단계로 나눠 진행한다.
   1. ~~부위 ISM을 데이터 기반으로 생성.~~ 완료(2026-11-14).
   2. ~~부모 기준 연결 위치 계산 방식 결정과 적용.~~ 완료(2026-11-14, 연결점·Gap·ISM 계층·재분배 시 재계산).
   3. ~~T 재분배를 모든 부위로 확장(직접 전달).~~ 완료(2026-11-14).
   - ~~가장 낮은 부위 기준 바닥 맞춤.~~ 완료(2026-11-15, 요청으로 2-4 전에 추가).
   4. ~~`DA_GiantForm`에 거인 10부위 입력, 비율·잔여·0개 부위·연결 관계와 직접 전달 이동의 화면 결과 확인.~~ 완료(2026-11-15).
3. 거인 부위/그룹 이동·회전과 행동. 구체 고정 크기와 상대 좌표 유지 확인.
4. 이웃 부위 전달. 3번보다 먼저 진행한다(사용자 결정, 2026-11-15).
   1. ~~관절별 흐름 계산, 동시 출발, 길목 건너뛰기, 관절에 가까운 구체 선택(이동은 직선).~~ 완료(2026-11-15).
   2. 길목을 건너뛰는 구체만 관절 단면을 지나 이동. **보류(2026-11-15, 사용자 결정).** 건너뛰기 자체가 드물고, 건너뛰는 구체도 첫 관절 근처에서 출발하며 기본 자세에서는 직선 경로가 거의 비어 있는 중간 부위 근처를 지나 눈에 띄지 않는다. 3번에서 관절이 회전한 뒤 직선 이동이 몸 밖으로 벗어나 보이면 진행한다. 설계는 3장 T 키 재분배의 "이동" 항목을 그대로 쓴다.
   - 관절 공간 검색(공간 자료구조) 도입은 성능 필요성을 따로 판단.
5. 고정 N자루 복합 검 구성과 행동.
6. 계획 준비 → 무적 전환 → 직접 이동 → 도착 완료의 변신 흐름 연결.
7. 극소수/실패 처리, 전수 검사 실행 분리, 성능 측정과 데모 마무리.

이 목록 전체를 다음 한 번의 변경으로 구현하지 않는다. 데이터 기반 생성·T 재분배·부위 ISM 생성·부위 위치 규칙·모든 부위 재분배는 끝났으며 2-4 거인 10부위 입력과 Play 확인, 바닥 맞춤, 4-1 이웃 전달 계산도 끝났다. 4-1은 Play로 확인했고 4-2는 보류했다. 바로 다음 작업은 3번 부위 이동·회전(관절 피벗 → 자세 적용 → 키 입력으로 회전 확인)이다.

## 8. 검증과 작업 명령

부위 위치 규칙: Editor 타깃 빌드 성공, 자동 테스트 12개 성공/실패0(`Saved/Automation/PartPlacement/index.json`, `reportCreatedOn` `2026.11.13-18.54.23` 보고서 표기 그대로). 사용자가 `DA_GiantForm`에 왼팔 연결점·Gap을 입력한 Editor Play에서 위치와 크기 반영을 확인했다. 연결점 Z를 0으로 두면 두 면의 위아래 가운데끼리 맞으므로 5:1처럼 팔이 작으면 팔 윗부분이 몸통보다 낮아 보인다. 어깨 높이는 연결점 Z(예: 둘 다 1)로 맞춘다.

모든 부위 재분배: Editor 타깃 빌드 성공, 자동 테스트 12개 성공/실패0(`Saved/Automation/AllPartRebalance/index.json`, `reportCreatedOn` `2026.11.14-13.46.41` 보고서 표기 그대로). 세 부위 테스트 로그에서 몸통→머리 4개, 왼팔→머리 1개 직접 전달을 확인했다. 사용자가 `DA_GiantForm`에 거인 10부위를 넣은 Editor Play에서 T 재분배가 동작함을 확인했다(에디터 로그상 요청 수와 전달 수 일치).

이웃 전달: Editor 타깃 빌드 성공, 자동 테스트 14개 성공/실패0(`Saved/Automation/NeighborTransfer/index.json`, `reportCreatedOn` `2026.11.14-16.32.22` 보고서 표기 그대로). 로그에서 `Torso -> LeftArm -> LeftHand Requested=1` 길목 건너뛰기를 확인했다. 사용자 Editor Play에서 T 재분배가 동작함을 확인했다. 흐름이 육안으로 구분하기 어려워 `Reconstruction Duration`을 늘리거나 디버그 표시가 필요할 수 있다.

배치 계수 0.5와 부위별 시드: Editor 타깃 빌드 성공, 자동 테스트 15개 성공/실패0(`Saved/Automation/PartSeed/index.json`, `reportCreatedOn` `2026.11.14-17.33.11` 보고서 표기 그대로). 사용자 Editor Play에서 밀도와 좌우 배치 차이를 확인했다.

관통 구체 투사체와 생성 경로 전수 검사 제외: Editor 타깃 빌드 성공, 자동 테스트 16개 성공/경고0/실패0(`Saved/Automation/PiercingProjectile/index.json`, `reportCreatedOn` `2026.11.14-18.20.19` 보고서 표기 그대로). 테스트 로그 `Projectile passed through ... Removed=8`(구체 90개 몸통·왼팔). Editor Play는 아직 확인하지 않았다.

바닥 맞춤: Editor 타깃 빌드 성공, 자동 테스트 12개 성공/실패0(`Saved/Automation/Grounding/index.json`, `reportCreatedOn` `2026.11.14-15.26.29` 보고서 표기 그대로). PartPlacement에서 몸통 상승량(손이 가장 낮은 4부위), 재분배 후 재계산, 활성 구체 목표가 Manager 높이 아래로 내려가지 않음, 빈 왼팔 제외를 확인했다. 사용자가 Manager를 바닥 높이에 놓은 Editor Play에서 바닥 맞춤을 확인했다.

형태 데이터 생성 연결: Editor 타깃 빌드 성공, 자동 테스트 11개 성공/실패0(`Saved/Automation/FormDefinitionLink/index.json`). 사용자가 `DA_GiantForm`을 지정한 Editor Play에서 생성·제거·재분배가 이전과 같이 동작함을 확인했다.

T 재분배의 데이터 가중치 사용: Editor 타깃 빌드 성공, 자동 테스트 11개 성공/실패0(`Saved/Automation/DefinitionRebalance/index.json`, `reportCreatedOn` `2026.11.13-17.18.36` 보고서 표기 그대로). 사용자가 에셋 가중치를 10:1로 바꾼 Editor Play에서 생성과 T 재분배가 그 비율을 따르는 것을 확인했다.

부위 ISM 데이터 기반 생성: Editor 타깃 빌드 성공, 자동 테스트 11개 성공/실패0(`Saved/Automation/DataDrivenInstances/index.json`, `reportCreatedOn` `2026.11.13-17.31.36` 보고서 표기 그대로). 사용자가 에디터에서 레벨에 저장된 기존 Manager가 경고 없이 로드되고, Play에서 생성·클릭 제거·T 재분배가 이전과 같이 동작함을 확인했다.

실행 로그와 JSON은 로컬 Saved에 있고 Git에는 포함되지 않는다.

| 테스트 | 확인 범위 |
|---|---|
| Stage1.SamplingInvariants | 생성 개수·간격·경계 |
| Stage1.DeterminismAndScale | Seed 재현성과 크기 관계 |
| Stage1.EdgeCasesAndFailure | 0/1개, 잘못된 설정·생성 실패 |
| Stage1.PreviewInstances | 표시 전용 Preview 생성·초기화 |
| Stage2.ManagerBehavior | 생성·대응표·삭제·목표 배정·이동 |
| Stage2.TwoPartRemoval | 두 ISM의 동일 번호 구분과 제거 |
| Stage2.PartTransfer | 소속 이전·위치/목표 유지·대응표 |
| Stage2.TransferBatch | 여러 개 전달과 실패 입력 |
| Stage2.RebalanceParts | 양방향 전달·정수 잔여·반복 요청·가중치 입력 거부 |
| Stage2.TimedReconstruction | 설정 시간·감속·재배정·잘못된 시간 |
| Stage2.PartPlacement | 연결점·Gap 위치 계산(박스·구형·회전), ISM 부착 계층, 재분배 시 관절 재계산과 구체 화면 위치 유지·재이동, 0개 부위 점 처리, 가장 낮은 부위 기준 바닥 맞춤 |
| Stage2.TransferPlanner | 관절별 흐름 계산, 이웃끼리만 전달, 길목 부족·0개 시 경로 연장, 균형 시 빈 계획, 합 불일치·최상위 둘·순환 거부 |
| Stage2.NeighborTransfer | Manager 재분배의 이웃 전달 소속 변화(몸통→팔 15, 팔→손 2, 몸통→팔→손 1), 관절에 가까운 구체 선택 |
| Stage2.ProjectileAttack | 관통 구체가 경로에서 (투사체 반지름 + 구체 반지름) 안의 구체를 모두 지우고 밖은 남기며 멈추지 않음, 대응표 유지, 벽에서 정지·파괴, 빈 공간 비행 |
| Stage2.ManagerDensity | Manager 기본 배치 계수(0.5)로 구·박스 4종 비율 × 2~2000개 생성 성공, 부위별 시드로 좌우 배치가 다르고 같은 부위는 재현 |
| Stage2.FormDefinition | 데이터 누락, 가중치 배분과 잔여, 가중치 0, 잘못된 데이터 13종 거부, 데이터 목록대로 부위 ISM 생성·제거, 세 부위 데이터 가중치 재분배·이미 균형 시 유지, 목록 변경 시 재분배 거절, 데이터 가중치 재분배·변경 반영 |

사용자는 Play에서 이동 중 피격 제거, 부위 전달, 양방향 재분배와 축소를 확인했고 0.8초 이동을 선택했다. 이는 성능 비교나 미구현 거인/검/변신 검증이 아니다. NullRHI 자동 테스트는 실제 화면·패키징 검증을 대신하지 않는다.

```powershell
# Editor가 DLL을 사용 중이면 사용자에게 저장 후 종료를 요청하고 프로세스 종료를 확인한다.
& 'D:/Epic Games/UE_5.7/Engine/Build/BatchFiles/Build.bat' SwarmBossEditor Win64 Development '-Project=D:/UnrealProjects/SwarmBoss/SwarmBoss.uproject' -WaitMutex -NoHotReloadFromIDE

# 빌드 성공 후 전체 자동 테스트. 새 작업 이름의 보고서 경로를 사용한다.
& 'D:/Epic Games/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' 'D:/UnrealProjects/SwarmBoss/SwarmBoss.uproject' '-ExecCmds=Automation RunTests SwarmBoss.' '-TestExit=Automation Test Queue Empty' '-ReportExportPath=D:/UnrealProjects/SwarmBoss/Saved/Automation/NextValidation' -unattended -nop4 -NullRHI -nosound '-abslog=D:/UnrealProjects/SwarmBoss/Saved/Logs/NextValidation.log'

# 새 파일이 VS 솔루션 탐색기에 보이지 않을 때 프로젝트 파일 재생성
& 'D:/Epic Games/UE_5.7/Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe' -ProjectFiles '-Project=D:/UnrealProjects/SwarmBoss/SwarmBoss.uproject' -Game -Rocket
```

프로세스 종료 코드만 보지 말고 보고서 JSON의 succeeded/failed/notRun을 확인한다. 에디터를 임의 종료하지 않는다. 새 컴퓨터에서는 프로젝트·엔진 경로를 해당 환경으로 바꾼다.

## 9. 지켜야 할 불변 조건

- 정상 처리 완료 시 활성 개체는 소속 부위 하나와 인스턴스 하나를 가진다.
- `Parts[PartId].InstanceToParticle[InstanceIndex]`와 중앙 `Particles[ParticleId]`의 소속·인덱스가 서로 일치한다.
- 중앙 `ActiveParticleCount`는 활성 수이며 `Particles.Num()`과 구분한다.
- 소속 이전은 생존 수·ID·화면 위치를 유지한다. 생성 초기화만 ID의 수명을 다시 시작한다.
- 각 부위에 새로 생성한 목표는 그 부위의 개체에 일대일 배정한다.
- 샘플링 실패와 적용 도중 부분 실패를 구분한다. 현재 코드에 없는 원상복구나 재시도를 있다고 설명하지 않는다.
- 구체 크기는 생존 수와 무관하게 유지하며, 목표 간격과 이동 중 겹침 정책을 구분한다.
