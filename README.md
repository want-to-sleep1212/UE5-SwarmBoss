# UE5-SwarmBoss

Unreal Engine 5와 C++로 개발하는 군체 보스 프로토타입입니다. 고정 크기 구체들을 기본 도형 안에 배치하고, 피격으로 줄어든 생존 수에 맞춰 재분배·축소합니다. 현재 몸통과 왼팔의 생성·피격·양방향 재분배까지 구현했으며, 거인 전체와 검 N자루 및 완전 변신은 후속 작업입니다.

## 작업을 이어받을 때

**현재 구현, 설계 결정, 다음 작업, 빌드·테스트 절차는 [설계 및 작업 인수인계](SwarmBossDesign.md)를 먼저 읽어 주세요.** 기준일은 2026-11-14이며, 로컬 작업 트리 기준입니다. GitHub에 모든 변경이 올라갔다는 뜻은 아닙니다.

- [포트폴리오 후보 메모](Docs/PortfolioNotes.md): 고민한 대안과 선택 이유, 확인한 내용, 나중에 실측할 항목. 완성된 포트폴리오 원고가 아닙니다.
- [단일 도형 미리보기](Docs/Stage1README.md): Preview 사용법과 초기 샘플러 검증 기록.
- [데모 에셋 추적 범위](Docs/DemoAssets.md): 에셋 참조 조사와 Git LFS 추적 정책.

## 현재 구현

- 구·박스 내부 3D Poisson Disk Sampling, 최소 간격과 배치 계수에 따른 크기 계산.
- 중앙 `FSwarmParticleStore`와 부위별 ISM, 안정적인 `ParticleId`와 인스턴스 번호의 양방향 연결.
- 경로의 구체를 모두 지우며 관통하는 구체 투사체(엔진 Sweep 조회)와 교환 삭제, 생존 수 관리, 위치를 유지하는 부위 간 소속 이전.
- `USwarmFormDefinition` Data Asset(`DA_GiantForm`)에서 부위 도형·비율·배분 가중치를 읽어 생성하고, 데이터 유효성을 검사. 부위 ISM도 데이터의 부위 목록대로 Play 시작 시 생성.
- 부위 위치는 부모·자식 연결점(크기 비율)과 관절 간격 `Gap`으로 계산하고, 자식 ISM을 부모 ISM에 붙여 관절 계층을 구성. 재분배로 크기가 바뀌면 관절 위치를 다시 계산. 가장 낮은 부위의 구체 표면이 Manager 위치에 닿도록 몸 전체 높이를 자동으로 맞춤(Manager 위치 = 발바닥).
- 데이터 가중치(현재 시험값, 미확정)에 따른 모든 부위 재분배와 생존 수에 맞춘 목표 재생성. 구체를 잃으면 자동으로 재분배하며(관통 투사체 한 발이 지나가는 동안의 손실을 한 번으로 묶음), 새 목표는 가까운 것끼리 짝지어(격자 후보 + 그리디) 맞은 곳 주변만 주로 움직이고, 남은 먼 이동은 주변 구체가 한 칸씩 밀리는 사슬(도미노)로 바꿈. 관절로 들어온 구체는 받는 부위 도형의 관절 쪽 면으로 들어감. 부위 간 이동은 부위 트리의 이웃끼리만, 모든 관절에서 동시에 일어나며 관절에 가까운 구체부터 보냄. 중간 부위가 모자라면 일부 구체가 그 부위를 건너뛰어 다음 부위로 감.
- 기본 0.8초의 이차 감속 이동. 에디터에서 `Reconstruction Duration` 수정 가능.
- 표시 전용 `SwarmShapePreview`. 실제 피격·이동은 `SwarmBossManager`가 담당.

재분배 때의 몸 전체 일렁임 제거(이완 방식, 보류), 관절 근처 공간 검색, 거인 전체 구성·행동, 검 구성·행동, 형태 변신과 변신 중 무적은 아직 구현하지 않았습니다.

## 개발 환경과 실행

- Unreal Engine 5.7 (기존 빌드 검증 5.7.4), Windows, Visual Studio 2022 C++ 개발 도구.
- Git / Git LFS. `.uasset`과 `.umap`은 LFS로 관리하며 솔루션·빌드 결과·캐시·로그는 제외합니다.

```powershell
git lfs install
git clone https://github.com/want-to-sleep1212/UE5-SwarmBoss.git
cd UE5-SwarmBoss
git lfs pull
```

1. `SwarmBoss.uproject`의 Visual Studio 프로젝트 파일을 생성하고 `Development Editor / Win64`로 빌드합니다.
2. FirstPerson 데모 레벨에서 `SwarmBossManager` Actor를 배치합니다. 이미 있으면 중복 배치하지 않습니다. Manager 위치가 거인의 발바닥이므로 바닥 높이에 놓습니다. Manager Actor는 Git으로 추적하지 않으므로 새로 받은 저장소에서는 직접 배치합니다.
3. Details의 `Swarm|Generation`에서 `Form Definition`에 `/Game/Data/DA_GiantForm`을 지정합니다. 비어 있으면 Play 시 구체를 생성하지 않고 Output Log에 경고만 남깁니다.
4. `Initial Particle Count`, `Reconstruction Duration`을 설정하고 Play를 누릅니다. Manager는 Play 시작 시 생성합니다. 부위 간 위치와 간격은 `DA_GiantForm`의 부위별 `Parent Anchor`, `Child Anchor`, `Gap`으로 정합니다.
5. `ASwarmBossPlayerController`를 사용하는 플레이어로 중앙 조준선을 맞추고 왼쪽 버튼을 누르면 반지름 15cm 관통 구체가 날아가 경로의 구체를 모두 지웁니다. 쿨타임은 2초입니다. 쿨타임·속도·반지름은 PlayerController의 `Swarm|Attack`에서 바꿉니다.
6. 구체를 맞히면 투사체가 보스를 지나간 뒤 모든 부위의 생존 합계를 `DA_GiantForm`의 가중치 비율로 자동 재분배합니다. Manager의 `Swarm|Rebalance`에서 `Auto Rebalance On Loss`로 끌 수 있습니다. `T`를 누르면 같은 재분배를 수동으로 실행합니다. 이미 개수 비율이 맞으면 재생성하지 않습니다. Output Log의 `Assign` 줄에서 부위별 최대·평균 이동과 먼 이동 수를 볼 수 있습니다.

7. `1`~`4` 키로 오른팔을 어깨 관절 기준으로 돌립니다. `1` 옆으로 들기, `2` 내리기, `3` 앞으로, `4` 뒤로이며 한 번에 15도입니다(PlayerController `Swarm|Pose`의 `Pose Step Degrees`). 팔을 돌려도 몸 높이는 바뀌지 않고, 재분배 뒤에도 자세가 유지됩니다.
8. `5` 키로 마네킹 애니메이션을 거인에 옮깁니다. 누를 때마다 끔 → 서 있기 → 걷기 → 공격(한 번 재생 후 서 있기) → 끔 순서로 바뀌고, 끄면 기본 자세로 돌아갑니다. 재생 중에는 `1`~`4`가 무시됩니다. 팔·다리가 한 덩어리라 팔꿈치·무릎 굽힘에 따른 손·발 위치는 재현되지 않습니다.

`R` 재구성과 `T` 1,000cm 이동은 과거 테스트 입력입니다. 현재 T는 재분배이며 R은 연결되어 있지 않습니다. Preview만 배치하면 현재 게임플레이 기능을 시험할 수 없습니다.

## 검증

2026-11-15 `Saved/Automation/Grounding/index.json` 보고서에서 자동 테스트 **12개 성공, 실패 0개**를 확인했습니다. 사용자 Play 확인으로 `DA_GiantForm` 기반 생성과 제거·이동·부위 전달·재분배·축소, 연결점·Gap 위치, 거인 10부위의 모든 부위 재분배, 바닥 맞춤이 동작했고, 이동 시간은 0.8초가 적절하다는 피드백이 있었습니다. 자동 테스트와 수동 화면 확인의 범위 및 재실행 명령은 설계 문서에 있습니다.

Android File Server의 로컬 인증 토큰은 공개 설정에 포함하지 않습니다. 에셋은 참조 관계에 따라 필요한 것만 추적하며, 신규 에셋 추가 시 `.gitignore` 허용 목록도 확인합니다.
