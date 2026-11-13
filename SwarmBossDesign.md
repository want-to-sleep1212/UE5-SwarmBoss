# 군체 보스 설계 및 작업 인수인계

- 최초 작성: 2026-11-09 / 최신화: 2026-11-14.
- 실제 프로젝트: `D:\UnrealProjects\SwarmBoss`, C++: `Source/SwarmBoss`, 엔진: `D:\Epic Games\UE_5.7`.
- 이 문서는 현재 소스와 대화에서 정한 방향을 구분한다. 앞으로 추가할 클래스·함수 이름은 제안이며 이미 존재하는 API가 아니다.
- 별도 인수인계 파일을 만들지 않고 이 문서에 작업 재개 정보를 유지한다. 선택 이유와 실측 후보는 [PortfolioNotes](Docs/PortfolioNotes.md)에 기록한다.

## 1. 작업 재개 지점

**Manager는 `USwarmFormDefinition` Data Asset(`/Game/Data/DA_GiantForm`)을 읽어 몸통·왼팔을 생성하고, T 재분배도 같은 데이터의 가중치를 사용한다. 다음 작은 작업은 거인 전체 부위 추가다.** 거인 전체·검·변신을 한 번에 구현하지 않는다. 최근 대화에서 변신 구조를 먼저 검토한 이유는 이 데이터 구조를 나중에 크게 바꾸지 않기 위해서다.

읽을 순서:

1. 이 문서의 현재 구현과 향후 형태 관리 설계.
2. `Source/SwarmBoss/Swarm/SwarmParticleStore.h`: 구체 상태와 ID.
3. `Source/SwarmBoss/Boss/SwarmFormDefinition.h`: 부위 구성 데이터 형식.
4. `Source/SwarmBoss/Boss/SwarmBossManager.h`: 소유 데이터와 공개 기능.
5. 같은 `.cpp`의 `GenerateInitialParts`, `ValidateFormDefinition`, `RemoveInstance`, `TransferParticle`, `RebalanceParts`, `RebalancePartsFromDefinition`, `UpdateParticleMovement`.
6. `Source/SwarmBoss/SwarmBossPlayerController.cpp`: 실제 클릭/T 입력.
7. `Source/SwarmBoss/Swarm/SwarmShapeSamplerTests.cpp`: 기존 동작의 검증 조건.

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
- 현재 기본 반지름 10cm, 여유 간격 2cm, `PackingCoefficient` 0.35. 기본값은 `SwarmShapeSampler.h`의 `FSwarmSamplingSettings`에 있다. Manager는 이를 사용하며 k를 에디터 속성으로 노출하지 않았다.
- k가 작을수록 넉넉한 영역을 확보한다. 확장을 드문 예외로 만들도록 테스트로 튜닝한다. 생성 시도·후보 검사 예산·확장량에 상한을 둔다.
- 목표 수만 채우고 중단하지 않고 영역에 후보를 분포시킨 후 필요한 수를 선택한다. 개수·간격 검사만으로 시각적 균질성까지 보장되지는 않는다.
- 경계는 구체 중심의 허용 영역이며 구체 표면은 반지름만큼 돌출될 수 있다.
- 목표 간격은 각 도형 내부 기준이다. 서로 다른 도형의 연결부까지 비겹침을 보장하지 않는다. 이동 중 겹침은 허용한다.

### 생성 후 전수 검사의 사용 단계 — 2026-11-11 결정

현재 `GenerateTargets()`의 일반 생성 성공 경로에는 독립적인 O(N²) `ValidateTargets()` 호출이 남아 있다. 초기 검증용이며 최종 실행 경로에서는 제외할 예정이다. 생성 중 공간 격자로 검사하는 거리·경계와 입력 검증·목표 개수·실패 처리는 계속 유지한다. 검증 함수와 자동 테스트는 삭제하지 않는다.

- [ ] 최종 데모 전에 최초 생성·재구성·변신의 일반 실행 경로에서 중복 전수 검사 제외.
- [ ] 자동 테스트의 독립 간격·경계 검사 유지 및 재검증.
- [ ] 검사 포함/제외를 명시해 생성 시간 비교.

## 3. 현재 구현과 실행 흐름

### 소유 구조

```text
ASwarmBossManager
 ├─ FormDefinition: USwarmFormDefinition Data Asset 참조(DA_GiantForm)
 ├─ FSwarmParticleStore
 │   ├─ Particles: 중앙 구체 배열, 슬롯 인덱스가 ParticleId
 │   └─ ActiveParticleCount: 전체 생존 수
 └─ Parts: TMap<ESwarmPartId, FSwarmPart>
     ├─ Instances: 부위별 ISM
     ├─ InstanceToParticle: 해당 ISM 번호 → 중앙 ParticleId
     └─ SamplingSettings: 마지막 초기 생성에 성공한 설정
```

`USwarmRuntimeComponent`의 생성·제거·위치 갱신·Tick은 Manager에 통합했다. `ASwarmShapePreview`는 독립적인 샘플링 표시 Actor로 충돌·중앙 저장소·게임플레이 입력을 담당하지 않는다.

`FSwarmParticle`은 `LocalPosition`, `TargetLocalPosition`, `RemainingMoveTime`, `bActive`, `InstanceIndex`, `PartId`를 가진다. 위치·목표는 소속 부위 ISM 기준이다. enum은 `SwarmPartTypes.h`로 분리했고 거인의 머리·오른팔·양손·양다리·양발 및 None을 추가했다. 현재 Manager가 실제로 생성하는 부위는 여전히 몸통·왼팔뿐이다.

### 생성

`BeginPlay → GenerateInitialParts → ValidateFormDefinition → AllocatePartCounts → GenerateTargets(각 부위) → InitializeParticles → ResetParticles → AddPartParticles → Store.AddParticles`.

- Manager Details `Swarm|Generation`의 `Form Definition`에 지정한 Data Asset을 읽는다. 비어 있으면 `FormDefinition is not assigned` 경고를 남기고 생성하지 않는다. 코드 기본값으로 대체 생성하지 않는다.
- `InitialParticleCount` 기본 90, 에디터 범위 0~10000. 각 부위는 `전체 × AllocationWeight / 가중치 합`을 버림한 수를 받고, 버림 나머지는 `RemainderPartId`(몸통)가 받는다.
- 부위의 도형·Box 비율은 데이터의 `Shape`, `BoxHalfExtentsRatio`를 쓴다. `DA_GiantForm`의 비율은 몸통 (1, 0.75, 1.5), 왼팔 (0.45, 0.45, 1)이다. 가중치는 이전 코드 상수 2:1로 시작했고 2026-11-14 현재 시험 중인 몸통:왼팔 10:1로 저장되어 있다. 확정값이 아니다.
- 생성 전에 `ValidateFormDefinition`이 ID 중복·None/Preview, Manager ISM이 없는 부위·정의 누락, 음수 가중치·가중치 합 0, 비율·자세 값, 자기 부모·미정의 부모, 몸통 외 최상위 부위, 순환, 잔여 수신 부위를 검사한다. 실패 시 원인을 경고로 남기고 기존 구체를 유지한다. Clamp 메타데이터는 런타임 검증으로 간주하지 않는다.
- Manager는 아직 몸통·왼팔 ISM만 만들므로 데이터에는 이 두 부위가 정확히 있어야 한다. 부위가 늘어나면 ISM 생성도 데이터 기반으로 바꿔야 한다.
- 모든 부위의 샘플링 결과가 성공한 뒤 중앙 저장소를 한 번 초기화하고 각 부위를 추가한다. `AddParticles`는 첫 중앙 ID를 반환하므로 두 번째 부위가 첫 번째를 덮어쓰지 않는다.
- 실제 `AddInstances` 반환 인덱스로 양방향 연결을 채운다. 중앙 ID와 ISM 인덱스가 같다고 가정하지 않는다.
- `PartGap` 기본 150cm. 초기 도형의 Y 반크기와 구체 반지름으로 표면 사이 여유를 확보하고 왼팔 위치를 정한다. 재분배마다 부위 중심을 재배치하지는 않는다.
- 데이터의 `RelativeLocation`·`RelativeRotation`은 유효성만 검사하고 아직 적용하지 않는다. 고정 cm 위치는 축소 후 관절 간격이 벌어지거나 겹칠 수 있어, 연결 위치 계산 방식을 거인 전체 부위 추가 때 함께 정한다(2026-11-14 결정).
- 생성 때만 초기 개수 설정을 사용한다. 이후 전체 생존 수의 기준은 Store다.

### 피격과 삭제

`왼쪽 클릭 → PlayerController.TraceSwarmParticle → 엔진 LineTraceSingleByObjectType → Manager.RemoveInstance → Store.DeactivateParticle`.

- 카메라 정면 10000cm, `ECC_GameTraceChannel3 = SwarmSphere` Object Type만 조회한다. `ECC_GameTraceChannel2 = PlayerProjectile`, ISM 프로필은 `SwarmSpherePreset`.
- 현재는 디버그 클릭 Trace이며 실제 투사체 발사/관통 구현이 아니다. 다른 Object Type의 벽 등은 조회에서 제외한다.
- 피격 컴포넌트로 부위를 먼저 찾고 `Hit.Item`을 그 부위의 `InstanceToParticle`로 변환한다.
- ISM 교환 삭제가 성공하면 중앙 개체를 비활성화하고 생존 수를 줄인다. 역방향 배열도 교환 삭제하고 이동한 마지막 개체의 `InstanceIndex`를 고친다.
- 중앙 슬롯은 삭제·압축·재사용하지 않는다. ID는 한 번의 초기화 이후 생존 기간 동안 유지된다. 슬롯 풀, Generation Handle, 제거 대기 큐는 현재 구현하지 않는다.
- 제거 후 자동 재구성은 없다. T 입력으로 요청한다. 실패 시 다음 프레임 자동 재시도도 없다.

### 부위 이전

`TransferParticle(ParticleId, DestinationPartId)`는 현재 월드 Transform과 기존 목표 월드 위치를 목적지 부위 로컬로 변환한다. 목적지 ISM 추가 후 출발지 제거를 수행하며 출발지 제거 실패 시 방금 추가한 목적지 인스턴스를 되돌린다. 성공 시 양쪽 대응표와 중앙 PartId/InstanceIndex/로컬 좌표를 바꾼다. ID·생존 수·남은 이동 시간은 유지하고, 자체적으로 새 목표를 정하거나 Tick을 켜지는 않는다.

`SelectPartParticleIds`는 중앙 배열 순서로 활성 개체를 선택한다. 관절 검색 TODO가 있지만 아직 공간 자료구조를 사용하지 않는다. `TransferParticlesToTargets`는 선택한 개체를 이전하고 전달 성공 개체에 목적지 로컬 목표를 순서대로 준다. 반환값은 실제 소속 이전 성공 수이며 실패 개체만 건너뛴다.

### T 키 재분배

`T → RebalanceSwarmPartsForTest → 각 Manager.RebalancePartsFromDefinition → RebalanceParts(RemainderPart, OtherPart, 가중치, 가중치)`.

`RebalancePartsFromDefinition`은 T 입력 시점의 `FormDefinition`을 다시 검증하고 부위 가중치만 읽는다. 잔여 수신 부위를 첫 번째 부위로 넘기므로 초기 생성과 같은 배분 규칙을 따른다. 도형 종류·비율은 마지막 초기 생성 때 보관한 설정을 사용한다. 데이터가 없거나 유효하지 않거나 부위가 둘이 아니면 경고 후 상태를 바꾸지 않는다. 가중치 0인 부위는 구체를 모두 다른 부위로 보낸다.

1. 두 부위의 현재 생존 수 합계로 목표 개수를 계산한다. 두 번째 부위는 버림, 첫 번째는 나머지를 받는다. 가중치는 0 이상, 합은 양수여야 한다.
2. 이미 목표 개수와 같으면 성공 반환하며 샘플링·이동 재시작을 하지 않는다. 비율이 맞으면 결손이 있어도 새 배치를 만들지 않는 현재 제한이 있다.
3. 양쪽 새 목표를 먼저 생성한다. 생성 실패까지는 기존 상태를 유지한다.
4. 초과 부위에서 부족 부위로 필요한 수를 보낸다. 방향과 개수는 고정되어 있지 않다.
5. 전달 성공 후 양쪽의 모든 활성 구체에 목표를 순서대로 배정한다. 최근접 목표 탐색은 하지 않는다.

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
- 부위별 필드: `PartId`, `ParentPartId`, `Shape`, `BoxHalfExtentsRatio`, `AllocationWeight`, `RelativeLocation`, `RelativeRotation`. 위치·회전은 부모 중심 기준이며 부모 없음은 Manager 기준이다. Scale은 없다.
- 형태 데이터는 `Parts` 목록과 잔여 수신 부위 `RemainderPartId`(몸통)를 보관한다. 실제 구체 수·샘플러 설정 전체·ISM은 넣지 않는다.
- 부모 상대 위치는 고정 cm 정의다. 축소 후 관절 연결 위치 자동 계산·정규화 앵커는 후속 작업이며 현재 데이터만으로 해결됐다고 간주하지 않는다.
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
2. 거인 전체 부위 추가. 부위 ISM을 데이터 기반으로 생성, 부모 상대 연결 위치 계산 방식 결정, 비율·잔여·0개 부위·연결 관계 확인.
3. 거인 부위/그룹 이동·회전과 행동. 구체 고정 크기와 상대 좌표 유지 확인.
4. 현재 두 부위 재분배를 전체 연결 관계로 확장. 관절 공간 검색 도입은 필요성을 따로 판단.
5. 고정 N자루 복합 검 구성과 행동.
6. 계획 준비 → 무적 전환 → 직접 이동 → 도착 완료의 변신 흐름 연결.
7. 극소수/실패 처리, 전수 검사 실행 분리, 성능 측정과 데모 마무리.

이 목록 전체를 다음 한 번의 변경으로 구현하지 않는다. 몸통·왼팔 생성과 T 재분배의 데이터 연결은 끝났으며 바로 다음 작업은 거인 전체 부위 추가다.

## 8. 검증과 작업 명령

형태 데이터 생성 연결: Editor 타깃 빌드 성공, 자동 테스트 11개 성공/실패0(`Saved/Automation/FormDefinitionLink/index.json`). 사용자가 `DA_GiantForm`을 지정한 Editor Play에서 생성·제거·재분배가 이전과 같이 동작함을 확인했다.

T 재분배의 데이터 가중치 사용: Editor 타깃 빌드 성공, 자동 테스트 11개 성공/실패0(`Saved/Automation/DefinitionRebalance/index.json`, `reportCreatedOn` `2026.11.13-17.18.36` 보고서 표기 그대로). 사용자가 에셋 가중치를 10:1로 바꾼 Editor Play에서 생성과 T 재분배가 그 비율을 따르는 것을 확인했다.

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
| Stage2.FormDefinition | 데이터 누락, 가중치 배분과 잔여, 가중치 0, 잘못된 데이터 12종 거부, 데이터 가중치 재분배·변경 반영 |

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
