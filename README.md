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
- 클릭한 구체의 교환 삭제, 생존 수 관리, 위치를 유지하는 부위 간 소속 이전.
- `USwarmFormDefinition` Data Asset(`DA_GiantForm`)에서 부위 도형·비율·배분 가중치를 읽어 생성하고, 데이터 유효성을 검사.
- 데이터 가중치(현재 시험값 몸통:왼팔 10:1, 미확정)에 따른 재분배와 생존 수에 맞춘 목표 재생성·순차 배정.
- 기본 0.8초의 이차 감속 이동. 에디터에서 `Reconstruction Duration` 수정 가능.
- 표시 전용 `SwarmShapePreview`. 실제 피격·이동은 `SwarmBossManager`가 담당.

관절 근처 공간 검색, 거인 전체 구성·행동, 검 구성·행동, 형태 변신과 변신 중 무적은 아직 구현하지 않았습니다.

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
2. FirstPerson 데모 레벨에서 `SwarmBossManager` Actor를 배치합니다. 이미 있으면 중복 배치하지 않습니다. Manager Actor는 Git으로 추적하지 않으므로 새로 받은 저장소에서는 직접 배치합니다.
3. Details의 `Swarm|Generation`에서 `Form Definition`에 `/Game/Data/DA_GiantForm`을 지정합니다. 비어 있으면 Play 시 구체를 생성하지 않고 Output Log에 경고만 남깁니다.
4. `Initial Particle Count`, `Part Gap`, `Reconstruction Duration`을 설정하고 Play를 누릅니다. Manager는 Play 시작 시 생성합니다.
5. `ASwarmBossPlayerController`를 사용하는 플레이어로 중앙 조준선을 맞추고 왼쪽 클릭하면 구체 하나가 제거됩니다.
6. `T`를 누르면 몸통·왼팔의 생존 합계를 `DA_GiantForm`의 가중치 비율로 맞추고 새 목표로 이동합니다. 이미 개수 비율이 맞으면 재생성하지 않습니다.

`R` 재구성과 `T` 1,000cm 이동은 과거 테스트 입력입니다. 현재 T는 재분배이며 R은 연결되어 있지 않습니다. Preview만 배치하면 현재 게임플레이 기능을 시험할 수 없습니다.

## 검증

2026-11-14 `Saved/Automation/DefinitionRebalance/index.json` 보고서에서 자동 테스트 **11개 성공, 실패 0개**를 확인했습니다. 사용자 Play 확인으로 `DA_GiantForm` 기반 생성과 제거·이동·부위 전달·재분배·축소가 동작했고, 이동 시간은 0.8초가 적절하다는 피드백이 있었습니다. 자동 테스트와 수동 화면 확인의 범위 및 재실행 명령은 설계 문서에 있습니다.

Android File Server의 로컬 인증 토큰은 공개 설정에 포함하지 않습니다. 에셋은 참조 관계에 따라 필요한 것만 추적하며, 신규 에셋 추가 시 `.gitignore` 허용 목록도 확인합니다.
