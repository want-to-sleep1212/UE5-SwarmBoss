# 군체 보스

Unreal Engine 5.7과 C++로 개발 중인 군체 보스 기술 데모입니다. 개별 구체들이 모여 거인 하나를 이룹니다. 투사체가 팔을 관통하면 경로에 있던 구체들이 사라집니다. 남은 구체들은 부위 사이를 이동해 빈자리를 채우고, 거인은 원래의 비율을 유지하며 작아집니다.

향후에는 거인을 구성하는 구체들이 재배치되어 여러 자루의 검으로 변하는 기능을 추가할 계획입니다.

## 시연 영상

[피격 후 군체 재구성](https://youtu.be/WgvwN_u2ToU)

## 개발 환경 및 기술

| 구분 | 사용 기술 |
| --- | --- |
| 엔진·언어 | Unreal Engine 5.7, C++ |
| 렌더링 | Instanced Static Mesh (ISM) |
| 형상 정의 | Data Asset |
| 구체 배치 | 3D Poisson Disk Sampling |

## 핵심 시스템

| 시스템 | 구현 요약 | 상세 문서 |
| --- | --- | --- |
| 피격과 재구성 | 맞은 구체는 즉시 제거합니다. 한 발이 여러 프레임에 걸쳐 보스를 관통할 수 있으므로, 관통이 끝나면 그동안의 손실을 모아 부위별로 필요한 구체 수를 다시 계산합니다. 구체가 부족한 부위에는 관절 가까이에 있는 구체부터 보내고, 각 부위 안에서는 가까운 목표 위치를 우선 배정해 긴 이동을 줄입니다. | [피격과 재구성](Docs/HitAndReconstruction.md) |
| 군체 형상 생성 | 머리, 몸통, 팔다리는 기본 도형으로 정의합니다. 보스 관리자가 부위별 가중치에 따라 구체 수를 배분하고, 중앙 저장소(`FSwarmParticleStore`)는 각 구체의 ID와 소속 부위를 추적합니다. 각 도형 내부에는 3D Poisson Disk Sampling으로 구체 사이의 최소 간격을 확보하면서 불규칙하게 배치합니다. 피격 후에도 개별 구체의 크기는 유지하고, 남은 수에 맞춰 부위의 크기와 관절 위치를 다시 계산합니다. | [군체 형상 생성](Docs/SwarmGeneration.md) |
| 애니메이션 연동 · 개발 중 | 마네킹의 본 회전을 군체의 각 부위에 적용하는 기능을 구현했으며, 현재 일부 동작을 보정하고 있습니다. | [자세와 애니메이션](Docs/PoseAnimation.md) |
| 프로파일링 |  | [프로파일링](Docs/Profiling.md) |

## 대표 소스 위치

| 영역 | 주요 소스 |
| --- | --- |
| 피격 처리 | [SwarmProjectile.cpp](Source/SwarmBoss/Projectiles/SwarmProjectile.cpp), [SwarmParticleStore.cpp](Source/SwarmBoss/Swarm/SwarmParticleStore.cpp) |
| 재구성 및 목표 배정 | [SwarmBossManager.cpp](Source/SwarmBoss/Boss/SwarmBossManager.cpp), [SwarmTransferPlanner.cpp](Source/SwarmBoss/Boss/SwarmTransferPlanner.cpp), [SwarmTargetAssigner.cpp](Source/SwarmBoss/Swarm/SwarmTargetAssigner.cpp) |
| 형상 정의와 배치 | [SwarmFormDefinition.h](Source/SwarmBoss/Boss/SwarmFormDefinition.h), [SwarmShapeSampler.cpp](Source/SwarmBoss/Swarm/SwarmShapeSampler.cpp) |
