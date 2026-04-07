# 세션 로그

## 요청 요약

- SQL 입력 방식을 파일 실행 중심에서 CLI 반복 입력 방식으로 확장한다.
- 스키마에서 `id`를 PRIMARY KEY로 선언하고 중복 INSERT를 막는다.

## 결정 사항

- 기존 SQL 파일 실행 방식은 유지하고, 입력 파일이 없을 때만 REPL 모드로 동작한다.
- `INSERT INTO users VALUES (...)` 문법을 추가해 CLI 입력을 짧게 유지한다.
- 스키마는 `id:int:pk,name:string,age:int`처럼 `:pk` 표기를 받아 단일 PRIMARY KEY를 해석한다.
- PRIMARY KEY 중복 검사는 CSV append 전에 기존 데이터 파일을 읽어 확인한다.

## 현재 브랜치 상태

- 현재 작업 중이던 변경을 보존한 채 `main`에서 `feature/interactive-cli-pk` 브랜치로 분리했다.

## 완료한 작업

- 입력 파일이 없으면 SQL 문장을 계속 받아 실행하는 REPL 루프를 추가했다.
- `quit`, `exit` 명령과 세미콜론 기준 문장 실행 흐름을 연결했다.
- `INSERT` 파서가 컬럼 목록 생략 문법을 읽도록 확장했다.
- 스키마 로더가 `:pk` 제약을 해석하도록 바꾸고 단일 PRIMARY KEY만 허용하게 했다.
- INSERT 실행 전에 PRIMARY KEY 중복 값을 검사하도록 실행기를 보강했다.
- 예제 스키마, 예제 SQL, README를 새 입력 방식과 PK 형식에 맞게 갱신했다.
- REPL 실행, 컬럼 생략 INSERT, PRIMARY KEY 중복 실패를 포함한 테스트를 추가했다.
- `make`, `make test`, 실제 REPL 입력 예시 실행을 확인했다.

## 리뷰 결과

- 정확성 관점에서 헤더만 있는 CSV, 컬럼 생략 INSERT, PRIMARY KEY 중복 INSERT 경로를 확인했고 현재 기준 차단 이슈는 없다.
- 사용성 관점에서 CLI 반복 입력과 기존 SQL 파일 실행을 함께 지원하도록 정리해 기존 예제와 새 사용 방식이 모두 유지된다.
- 테스트 관점에서 REPL 입력 리다이렉션, 중복 키 실패, 인덱스 기존 경로 회귀 여부를 확인했고 현재 기준 차단 이슈는 없다.

## 다음 작업

- GitHub Issue, Project, PR 절차를 진행한다.

## 남은 리스크

- 기존에 잘못 생성된 `demo-data/users.csv`나 예전 인덱스 파일이 남아 있으면 새 동작 확인 시 혼동이 생길 수 있으므로 실행 전 정리가 필요하다.
