# 데모 에셋 추적 범위

2026-11-10 기준, 현재 데모의 참조 관계를 따라 필요한 에셋만 Git에서 추적한다.

- 시작점: `/Game/FirstPerson/Lvl_FirstPerson`, `/Game/FirstPerson/Blueprints/BP_FirstPersonGameMode`
- World Partition 외부 Actor·Object: 해당 레벨의 `__ExternalActors__`, `__ExternalObjects__` 패키지를 추가 시작점으로 포함.
- Unreal Asset Registry로 하드·소프트·게임·에디터 전용·관리 참조를 재귀 조회.
- C++의 직접 콘텐츠 로딩도 조사. 군체 미리보기는 `/Engine/BasicShapes/Sphere`를 사용하므로 별도 프로젝트 메시가 필요하지 않음.
- 결과: 495개 중 86개 유지(약 90.39MiB), 참조되지 않는 409개(약 57.62MiB)는 Git 추적에서만 제외.

## 유지된 파일

기본 레벨, 플레이어/GameMode/Controller Blueprint, 입력·터치 UI, 프로토타입 지형, 레벨 외부 Actor/Object 및 재귀 참조된 캐릭터 애니메이션·메시·머티리얼을 포함한다. 화면에 캐릭터가 보이지 않더라도 저장된 Blueprint·애니메이션이 참조하는 파일은 임의로 제거하지 않았다.

구체적인 파일 목록과 패키지별 의존 관계는 [DemoAssetDependencies.json](DemoAssetDependencies.json)에 있다. `/Engine`, `/Script`, 플러그인 경로는 엔진·코드·플러그인이 공급하며 Content에 복사하지 않는다.

## 로컬 파일 보존

제외한 파일은 `git rm --cached`로 인덱스에서만 제거했다. 원본 Content 495개 파일의 SHA-256을 변경 전후 비교하여 내용이 유지되었는지 확인했다. 로컬 Shooter·Horror 레벨과 에셋은 남아 있지만 새로 받은 저장소에는 포함되지 않는다. 기존 템플릿 C++ 소스는 유지한다.

`.gitignore`의 Content 허용 목록으로 불필요한 에셋의 재추가를 방지한다. 새 에셋을 데모에서 사용하면 참조 관계를 다시 확인하고 허용 목록과 의존성 문서를 함께 갱신해야 한다. 단순 `git add .`만으로 신규 Content가 자동 포함되지 않는다.

## 검증과 한계

원본 프로젝트의 에셋을 삭제하지 않고, 필요한 86개만 복사한 독립 검증 프로젝트를 만들었다. 해당 프로젝트에는 기존 Intermediate·Asset Registry 캐시를 복사하지 않았다. UE 5.7.4 Python commandlet으로 데모 맵과 플레이어 Blueprint 3개를 로드했고, 오류·경고 0개로 완료했다. 로드한 레벨에서 Actor 19개와 SwarmShapePreview 1개를 확인했다.

NullRHI 로딩 검증이며 실제 화면·플레이·패키징 재검증은 아니다. 앞으로 문자열로 동적 로딩할 에셋이 추가되면 Asset Registry 참조 외에 명시적인 루트 등록이 필요하다.

`Config/DefaultEditor.ini`의 SimpleMapName은 기존 템플릿의 존재하지 않는 FirstPersonExampleMap을 가리킨다. 현재 데모의 EditorStartupMap·GameDefaultMap은 정상이며, 이번 에셋 추적 정리에서는 이 기존 설정을 수정하지 않았다.

과거 커밋과 이미 업로드된 Git LFS 객체는 유지한다. 이번 작업은 최신 브랜치의 파일 목록 정리이며, 이력 재작성이나 원격 LFS 용량 회수는 수행하지 않는다.
