# 1단계: 단일 도형 배치

## 사용 방법

1. `SwarmBossEditor`를 빌드한 뒤 프로젝트를 연다.
2. C++ Classes에서 `SwarmShapePreview`를 레벨에 배치한다. 기존 레벨과 GameMode는 코드에서 변경하지 않는다.
3. Actor Details의 `Swarm > Settings`에서 값을 조정한다.
4. `Generate Preview`를 누른다. Play 시작 시에도 현재 설정으로 생성한다.
5. `Show Bounds`를 누르면 적용된 도형 경계가 10초 동안 표시된다.
6. `Result`의 `Last Report`, `Generated Count`, `Applied Half Extents`와 Output Log의 `LogSwarmShape`를 확인한다.

기본값은 구체 300개, 개체 반지름 10cm, 여유 간격 2cm, 배치 계수 0.35다. 배치 계수는 최종 튜닝값이 아니다. `Box`의 `Box Half Extents`는 기준 종횡비이며 실제 크기는 개체 수와 최소 간격으로 결정한다. 구형 영역에서는 이 비율을 사용하지 않는다.

설정 변경만으로 재생성하지 않는다. 에디터 드래그·속성 변경마다 비싼 샘플링이 반복되는 것을 막기 위한 선택이다. 생성 실패 시 이전 성공 미리보기를 유지하며, 결과 메시지는 마지막 시도의 결과다. 비우려면 `Clear Preview`를 누른다.

## 구현 범위

- 구형/박스형 영역 하나의 내부 중심 좌표 생성.
- 최소 중심 간격 `d = 2r + margin`, 필요 부피 `N*d³/k`.
- Bridson 방식의 활성점·구각 후보·공간 격자 이웃 검사.
- 영역 전체 샘플링 후 임의 부분 선택: 목표 수에서 조기 종료하여 한쪽만 채워지는 현상을 피한다. 분포의 균질성은 시각적 튜닝이 추가로 필요하다.
- 총 후보 검사 예산, 시도 횟수, 선형 확장 상한.
- 같은 Seed와 같은 설정의 재현 가능한 결과.
- ISM 미리보기, 실패 시 기존 결과 보존, 수치 진단.

도형 경계는 구체 **중심**의 허용 영역이다. 구체 표면은 해당 경계에서 반지름만큼 돌출될 수 있다. 확장량은 중심 영역 각 축의 증가 비율이다. 구체 자체 크기는 그대로다.

Actor Scale은 1로 유지한다. 위치·회전은 변경할 수 있다. 현재는 이동·Noise·피격·재분배·전환·개체 풀을 구현하지 않았다. 충돌도 비활성 상태다.

`ValidateTargets()`는 생성 완료 시 실행되는 독립적인 O(N²) 전수 검사다. 검증용 비용을 포함한 시간이 표시된다. 대규모 런타임 생성 최적화는 후속 단계에서 분리한다. 목표 수와 간격만으로 공간의 시각적 균질성이 보장되지는 않는다.

## 파일 역할

| 파일 | 역할 |
|---|---|
| SwarmShapeSampler.h / .cpp | 설정·결과 구조 및 엔진 오브젝트와 분리된 샘플러 |
| SwarmShapePreview.h / .cpp | 레벨 배치 가능한 ISM 실험 Actor |
| SwarmShapeSamplerTests.cpp | 개수·간격·경계·재현성·스케일·실패 경로 자동 테스트 |

## 테스트 실행

Unreal Session Frontend의 Automation에서 `SwarmBoss.Stage1`을 실행한다. 명령줄에서는 다음과 같이 실행할 수 있다.

```powershell
& 'D:\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' `
  'D:\UnrealProjects\SwarmBoss\SwarmBoss.uproject' `
  '-ExecCmds=Automation RunTests SwarmBoss.Stage1' `
  '-TestExit=Automation Test Queue Empty' -unattended -nop4 -NullRHI `
  '-ReportExportPath=D:\UnrealProjects\SwarmBoss\Saved\Automation\Stage1'
```

자동 테스트의 성공은 실제 화면 배치·머티리얼·실루엣 검증을 대신하지 않는다.

## 검증 기록 — 2026-11-09

- UE 5.7.4 `SwarmBossEditor Win64 Development` 빌드 성공.
- `SwarmBoss.Stage1` 자동 테스트 4개 통과: SamplingInvariants, DeterminismAndScale, EdgeCasesAndFailure, PreviewInstances.
- 구·박스 × Seed 1/42/12345 × 개체 수 2/50/300의 18개 조합에서 모두 목표 개수·간격·경계 검사 통과.
- 17개 조합은 확장 없음. 구형 영역, Seed 1, 개체 2개 조합은 5% 선형 확장 후 성공.
- 테스트 표본에서 300개 생성은 약 5.2~5.5ms. 전수 검증을 포함한 단일 실행 측정이며 성능 보장값이 아니다.
- 미리보기 Actor의 메시 로딩, 인스턴스 수, 구체 Scale, 실패 시 기존 배치 보존, Clear 동작 확인.
- NullRHI 실행이므로 실제 에디터 화면과 실루엣은 미검증.
- 로그: `D:\UnrealProjects\SwarmBoss\Saved\Logs\Stage1Tests.log`
- 보고서: `D:\UnrealProjects\SwarmBoss\Saved\Automation\Stage1`
