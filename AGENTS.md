# AGENTS.md

이 문서는 이 저장소에서 작업하는 AI 에이전트를 위한 공용 컨텍스트입니다.
다른 Codex 에이전트가 이 파일 하나만 읽고도 현재 프로젝트 상태, 작업 규칙,
검증 방식, GitHub 워크플로를 바로 이해할 수 있도록 정리했습니다.

## 1. 프로젝트 목적

- 프로젝트 이름: `week6-team5-sql`
- 목표: 초심자가 읽기 쉬운 `C99` 기반 파일형 SQL 처리기 구현
- 현재 저장 방식: `CSV`
- 현재 인덱스 방식: 디스크 영속형 `B+ 트리` `.idx`
- 현재 실행 방식:
  - SQL 파일 실행 모드
  - 입력 파일이 없을 때 REPL 모드

## 2. 현재 구현 상태

현재 `main`/`dev` 기준으로 아래 기능이 구현되어 있습니다.

- `INSERT`
- `SELECT`
- `CREATE INDEX`
- `WHERE`와 최대 2개 조건의 `AND`
- `INSERT INTO table VALUES (...)` 문법
- `INSERT INTO table (col1, col2) VALUES (...)` 문법
- CSV 헤더 자동 생성
- 단일 컬럼 B+ 트리 인덱스 생성과 조회
- REPL 모드에서 `quit`, `exit` 종료
- 스키마의 `:pk` 표기를 통한 단일 PRIMARY KEY 선언
- PRIMARY KEY 중복 INSERT 차단

## 3. 현재 지원 SQL

```sql
INSERT INTO users VALUES (1, 'kim', 20);
INSERT INTO users (id, name, age) VALUES (2, 'lee', 30);
SELECT * FROM users;
SELECT name, age FROM users WHERE age >= 20;
SELECT * FROM users WHERE age >= 20 AND id = 1;
CREATE INDEX idx_users_age ON users(age);
```

## 4. 현재 지원 범위와 제한

### 지원 범위

- 단일 테이블
- `WHERE` 조건 1개 또는 2개
- 결합자 `AND`
- 비교 연산자 `=`, `<`, `<=`, `>`, `>=`
- 타입 `int`, `string`
- 문자열 최대 길이 `63`
- 스키마의 단일 PRIMARY KEY

### 현재 제한

- `OR`
- `JOIN`
- `ORDER BY`
- `UPDATE`
- `DELETE`
- `CREATE TABLE`
- 복합 인덱스
- 통계 기반 옵티마이저

### 구현 선택에 따른 비차단 제한

- 인덱스 스캔은 성능보다 가독성을 우선해 모든 leaf 노드를 읽고 후보를
  모읍니다.
- 인덱스 후보 수가 `SQLPROC_MAX_INDEX_RESULTS`를 넘으면 인덱스 사용을
  포기하고 full scan으로 되돌아갑니다.
- 저장소는 append-only로 가정합니다.

## 5. 스키마와 데이터 형식

### 스키마 파일

- 경로: `schemas/<table>.schema`
- 예시:

```text
id:int:pk,name:string,age:int
```

규칙:

- 컬럼 순서가 CSV 헤더 순서가 됩니다.
- 타입은 `int`, `string`만 허용합니다.
- `:pk`가 붙은 컬럼은 PRIMARY KEY입니다.
- PRIMARY KEY는 하나만 허용합니다.

### 데이터 파일

- 경로: `data/<table>.csv`
- 첫 줄은 항상 헤더입니다.
- 첫 `INSERT` 시 파일이 없으면 헤더를 자동 생성합니다.

### 인덱스 파일

- 경로: `indexes/<index_name>.idx`
- 헤더 1개와 고정 크기 노드 배열로 저장합니다.
- leaf 노드:
  - `key`
  - `row_offset`
  - `next_leaf_id`
- internal 노드:
  - `key`
  - `child_id`

## 6. 주요 파일과 책임

- [include/sqlproc.h](/Users/donghyunkim/Downloads/test_sql/week6-team5-sql/include/sqlproc.h)
  공개 상수, AST 구조체, 스키마 구조체, 함수 선언
- [src/app.c](/Users/donghyunkim/Downloads/test_sql/week6-team5-sql/src/app.c)
  인자 파싱, SQL 파일 실행, REPL 모드
- [src/main.c](/Users/donghyunkim/Downloads/test_sql/week6-team5-sql/src/main.c)
  CLI 진입점
- [src/tokenizer.c](/Users/donghyunkim/Downloads/test_sql/week6-team5-sql/src/tokenizer.c)
  SQL 토큰화
- [src/parser.c](/Users/donghyunkim/Downloads/test_sql/week6-team5-sql/src/parser.c)
  수동 파서, AST 생성
- [src/schema.c](/Users/donghyunkim/Downloads/test_sql/week6-team5-sql/src/schema.c)
  `.schema` 로딩, 타입과 `:pk` 해석
- [src/executor.c](/Users/donghyunkim/Downloads/test_sql/week6-team5-sql/src/executor.c)
  CSV 저장/조회, WHERE 평가, PK 중복 검사, 롤백과 인덱스 재빌드 연동
- [src/btree_index.c](/Users/donghyunkim/Downloads/test_sql/week6-team5-sql/src/btree_index.c)
  B+ 트리 인덱스 생성, split, 조회, 재빌드
- [tests/test_runner.c](/Users/donghyunkim/Downloads/test_sql/week6-team5-sql/tests/test_runner.c)
  전체 기능 테스트
- [examples/demo.sql](/Users/donghyunkim/Downloads/test_sql/week6-team5-sql/examples/demo.sql)
  배치 실행 예시
- [examples/user_input.sql](/Users/donghyunkim/Downloads/test_sql/week6-team5-sql/examples/user_input.sql)
  사용자 입력 예시

## 7. 코드 스타일 규칙

이 저장소는 "초심자 친화형 C99"가 최우선입니다.

### 반드시 지킬 것

- `-std=c99 -Wall -Wextra -Werror` 기준을 유지합니다.
- `struct`, `enum`, 배열, `if/else`, `for`, `while`, 파일 입출력 중심으로
  작성합니다.
- 함수는 짧게 유지하고, 한 함수가 한 책임만 갖도록 나눕니다.
- 어렵거나 꼭 필요한 복잡한 흐름에는 한국어 주석을 붙입니다.
- 테스트 코드도 초심자가 따라가기 쉽게 작성합니다.

### 피할 것

- `void *`
- 함수 포인터
- 복잡한 매크로
- `union`
- 비트필드
- 가변 길이 배열
- compound literal
- designated initializer
- 과한 포인터 연산
- 읽기 어려운 삼항 연산자 남용

### 주석 기준

특히 아래 성격의 함수는 함수 시작부에 한국어 설명을 붙이는 편이 좋습니다.

- 파서의 복잡한 분기
- 타입 비교 함수
- B+ 트리 split / root 갱신
- rollback / rebuild 같은 실패 복구 경로

## 8. 작업 전 확인할 것

새 작업을 시작할 때는 보통 아래 순서로 확인합니다.

1. 현재 브랜치와 작업 트리 상태
2. 최신 `README.md`
3. 관련 `docs/session-logs/*.md`
4. 관련 코어 파일
5. 현재 테스트 범위

추천 명령:

```bash
git status --short --branch
git log --oneline --decorate -5
make test
```

## 9. 구현 후 기본 검증

기본 검증 순서는 아래를 권장합니다.

1. `make`
2. `make test`
3. 변경된 README 예시나 CLI 예시를 실제로 한 번 실행

REPL 예시:

```bash
mkdir -p ./demo-data ./demo-indexes
./build/sqlproc \
  --schema-dir ./examples/schemas \
  --data-dir ./demo-data \
  --index-dir ./demo-indexes
```

배치 예시:

```bash
./build/sqlproc \
  --schema-dir ./examples/schemas \
  --data-dir ./demo-data \
  --index-dir ./demo-indexes \
  ./examples/demo.sql
```

## 10. Git 브랜치 전략

이 저장소는 아래 순서를 반드시 지킵니다.

1. `main`
2. `dev`
3. `feature/<기능명>`

규칙:

- 직접 `main`에서 작업하지 않습니다.
- 일반적인 개발 작업은 항상 최신 `dev`에서 기능 브랜치를 따서 진행합니다.
- 기능 완료 후 `feature/* -> dev`로 병합합니다.
- 최종 통합은 `dev -> main`으로 병합합니다.
- merge는 모두 `--no-ff` 기준으로 구분 가능한 이력을 남깁니다.

## 11. merge 전 필수 GitHub 절차

모든 merge 전에 아래 순서를 반드시 따릅니다.

1. 세션 로그를 Markdown으로 기록
2. 멀티 페르소나 코드 리뷰 수행
3. GitHub Issue 생성
4. Issue를 GitHub Project에 추가
5. Issue 코멘트에 한국어 검증 결과 작성
6. PR 생성
7. 테스트 통과 후 merge

### GitHub 정보

- repo: `Jungle-12-303/week6-team5-sql`
- Project URL:
  [6주차 5조 미니 SQL 처리기 보드](https://github.com/orgs/Jungle-12-303/projects/1/views/1)

### Issue 코멘트 필수 항목

- `검증 범위`
- `발견된 문제`
- `수정 여부`
- `남은 리스크`
- `merge 가능 여부`

### 멀티 페르소나 리뷰 역할

- 정확성/버그
- 자료구조 무결성/B+트리 불변식
- 초심자 가독성/C99 난이도

## 12. 세션 로그와 컨텍스트 압축 규칙

세션 로그는 아래 경로에 남깁니다.

- `docs/session-logs/YYYY-MM-DD_HHMM-feature-name.md`

권장 섹션:

- `요청 요약`
- `결정 사항`
- `현재 브랜치 상태`
- `완료한 작업`
- `리뷰 결과`
- `다음 작업`
- `남은 리스크`

중요:

- 자동 컨텍스트 압축 전에 반드시 지금까지의 대화를 Markdown으로
  정리한 뒤 압축합니다.
- merge 직전에는 리뷰 결과와 테스트 결과까지 로그에 반영합니다.

## 13. 커밋 메시지 규칙

커밋 메시지는 한국어 본문을 사용하고, 아래 7가지 규칙을 지킵니다.

1. 제목과 본문을 빈 줄로 구분
2. 제목은 50자 이내
3. 제목 첫 글자는 대문자
4. 제목 끝에 마침표를 쓰지 않음
5. 제목은 명령문 형식
6. 본문은 72자 안팎으로 줄바꿈
7. 본문은 무엇과 왜를 설명

권장 제목 형식:

- `Add 파서 보강`
- `Fix B+ 트리 복구 경로`
- `Document README 정리`

## 14. 새 기능을 넣을 때 어디를 고치면 되는가

### SQL 문법 추가

- 토큰이 필요하면 `src/tokenizer.c`
- AST가 바뀌면 `include/sqlproc.h`
- 문법 파싱은 `src/parser.c`
- 실행은 `src/executor.c`
- 관련 테스트는 `tests/test_runner.c`

### 스키마 규칙 추가

- `src/schema.c`
- 필요하면 `include/sqlproc.h`
- 예제 스키마와 README도 함께 갱신

### 인덱스 동작 변경

- `src/btree_index.c`
- 인덱스 선택이나 SELECT 경로와 연결되면 `src/executor.c`도 확인
- 중복 키, split, 재실행, rollback/rebuild 테스트를 꼭 같이 확인

### CLI / REPL 변경

- `src/app.c`
- `src/main.c`
- README의 실행 예시와 테스트도 같이 업데이트

## 15. 현재까지 merge된 주요 작업

아래 흐름이 이미 `dev`와 `main`에 반영되어 있습니다.

- 프로젝트 스캐폴딩
- SQL 파서
- WHERE 실행기와 CSV 저장
- B+ 트리 인덱스
- README / examples / 최종 검증 정리
- REPL 입력 모드와 PRIMARY KEY 지원

## 16. 남아 있는 대표 리스크

- 파일 쓰기 실패를 강제로 주입하는 rollback/rebuild 전용 테스트는 아직
  없습니다.
- 인덱스 스캔은 단순성과 가독성을 우선한 구조라 대규모 데이터에 최적화되어
  있지 않습니다.

## 17. 에이전트 행동 원칙

- 저장소 규칙과 사용자 워크플로를 우선합니다.
- "간단한 수정"이어도 브랜치, 로그, 리뷰, GitHub 절차를 가능한 한
  유지합니다.
- 초심자 친화성을 해치는 구현보다, 조금 길더라도 읽기 쉬운 구현을 택합니다.
- 문서를 바꾸면 예제와 실제 실행 경로를 함께 확인합니다.
- B+ 트리나 복구 로직을 건드리면 테스트만 믿지 말고 흐름을 한 번 더
  눈으로 검토합니다.
