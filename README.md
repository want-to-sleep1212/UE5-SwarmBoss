# UE5-SwarmBoss

Unreal Engine 5와 C++로 개발하는 군체 보스 프로토타입입니다. Poisson Disk Sampling 기반 형태 생성을 구현했으며, 개체 재분배와 거인·검 형태 전환을 개발할 예정입니다.

## 현재 구현

- 구·박스 영역 내부에 3D Poisson Disk Sampling으로 목표 좌표 생성
- 구체 최소 간격과 배치 계수에 따른 도형 부피·크기 계산
- 고정 크기 구체의 Instanced Static Mesh 미리보기
- 생성 횟수·확장량 제한 및 실패 시 기존 미리보기 보존
- 에디터 생성·초기화·경계 표시 버튼과 진단 로그

현재 단계는 정적인 단일 도형 배치입니다. 개체 이동, 피격, 재분배, 거인·검 변신은 아직 구현하지 않았습니다.

## 개발 환경

- Unreal Engine 5.7 (빌드 검증: 5.7.4)
- Windows / Visual Studio 2022 C++ 개발 도구
- Git 및 Git LFS

## 실행 방법

```powershell
git lfs install
git clone https://github.com/want-to-sleep1212/UE5-SwarmBoss.git
cd UE5-SwarmBoss
git lfs pull
```

1. `SwarmBoss.uproject`에서 Visual Studio 프로젝트 파일을 생성합니다.
2. 생성된 솔루션을 열어 `Development Editor / Win64`로 빌드합니다.
3. Unreal Editor에서 프로젝트를 엽니다.
4. 레벨에 `SwarmShapePreview` Actor를 배치합니다.
5. Details에서 `Particle Count` 등 설정을 변경하고 **Generate Preview**를 누릅니다.
6. **Show Bounds**와 `Last Report`로 결과를 확인합니다.

솔루션, 빌드 결과, 캐시, 로그는 저장소에 포함하지 않으며 각 개발 환경에서 생성합니다. `.uasset`과 `.umap`은 Git LFS로 관리합니다.

## 코드 구성

| 경로 | 역할 |
|---|---|
| `Source/SwarmBoss/Swarm/SwarmShapeSampler.h/.cpp` | 생성 설정, 크기 계산, 샘플링, 결과 검증 |
| `Source/SwarmBoss/Swarm/SwarmShapePreview.h/.cpp` | ISM 기반 미리보기 Actor |
| `Source/SwarmBoss/Swarm/SwarmShapeSamplerTests.cpp` | Unreal Automation 테스트 |

그 외 First Person·Shooter·Horror 관련 C++ 소스는 프로젝트 템플릿 기반 구성입니다. 콘텐츠는 현재 FirstPerson 데모에서 재귀 참조하는 86개 에셋만 포함하며, 미사용 Shooter·Horror 레벨은 포함하지 않습니다. 군체 생성 구현과 템플릿 코드를 구분하여 유지합니다.

## 검증

2026-11-09 기준:

- UE 5.7.4 Editor 타깃 빌드 성공
- `SwarmBoss.Stage1` 자동 테스트 4개 통과
- 개수·최소 간격·도형 경계·Seed 재현성·크기 비례·실패 처리·미리보기 인스턴스 검증
- 18개 배치 조합 중 17개는 확장 없이 생성, 1개는 5% 확장 후 생성

이후 에디터에서 직접 배치·실행하여 표시 동작도 확인했습니다. 배치 계수와 시각적 분포는 계속 조정할 예정이며, 현재 수치를 최종 성능·품질 보장으로 보지 않습니다.

## 예정 기능

- 도형 중심 기준 상대 좌표 이동 및 독립적인 불규칙 운동
- 엔진 충돌 기반 피격과 개체 제거
- 공간 격자와 연결부를 이용한 인접 도형 간 재분배
- 복합 도형 거인과 검 N자루 구성 및 완전 변신

## 문서

- [1단계 사용 방법 및 검증 기록](Docs/Stage1README.md)
- [전체 설계안](SwarmBossDesign.md)
- [데모 에셋 추적 범위와 검증](Docs/DemoAssets.md)

Android File Server의 로컬 인증 토큰은 공개 설정에 포함하지 않습니다. 해당 기능을 사용할 경우 개발 환경에서 별도로 설정해야 합니다.
