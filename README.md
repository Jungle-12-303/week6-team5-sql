# 초심자 친화형 C99 SQL 처리기

CLI에서 SQL을 계속 입력하며 실행할 수 있는 작은 SQL 처리기입니다.
초심자가 읽기 쉽도록 `C99`의 기본 문법만 사용했고, 어려운 흐름에는
한국어 주석을 붙였습니다.

## 한눈에 보기

- 지원 문장: `INSERT`, `SELECT`, `CREATE INDEX`
- `WHERE`는 최대 2개 조건과 `AND`만 지원
- 데이터 저장: `CSV`
- 인덱스 저장: 디스크 영속형 `B+ 트리` `.idx`
- 빌드 기준: `-std=c99 -Wall -Wextra -Werror`

## 지원 SQL

```sql
INSERT INTO users VALUES (1, 'kim', 20);
INSERT INTO users (id, name, age) VALUES (2, 'lee', 30);
SELECT * FROM users;
SELECT name, age FROM users WHERE age >= 20;
SELECT * FROM users WHERE age >= 20 AND id = 1;
CREATE INDEX idx_users_age ON users(age);
```

## 지원 범위

- 단일 테이블만 처리합니다.
- `WHERE`는 조건 1개 또는 2개만 허용합니다.
- 비교 연산자는 `=`, `<`, `<=`, `>`, `>=`만 지원합니다.
- 지원 타입은 `int`, `string` 두 가지입니다.
- 문자열 최대 길이는 63자입니다.

## 지원하지 않는 기능

- `OR`
- `JOIN`
- `ORDER BY`
- `UPDATE`
- `DELETE`
- `CREATE TABLE`
- 복합 인덱스

## 디렉터리 구조

```text
include/
  sqlproc.h
src/
  app.c
  tokenizer.c
  parser.c
  schema.c
  executor.c
  btree_index.c
tests/
  test_runner.c
examples/
  demo.sql
  schemas/users.schema
docs/session-logs/
```

## 빌드와 테스트

```bash
make
make test
```

- 실행 파일은 `build/sqlproc`에 생성됩니다.
- 테스트는 `build/test_runner`를 실행합니다.

## 빠른 실행 예시

먼저 예제용 데이터 디렉터리와 인덱스 디렉터리를 만듭니다.

```bash
mkdir -p ./demo-data ./demo-indexes
./build/sqlproc \
  --schema-dir ./examples/schemas \
  --data-dir ./demo-data \
  --index-dir ./demo-indexes
```

프롬프트가 뜨면 아래처럼 문장을 계속 입력합니다.

```sql
INSERT INTO users VALUES (1, 'kim', 20);
INSERT INTO users VALUES (2, 'lee', 30);
CREATE INDEX idx_users_age ON users(age);
SELECT * FROM users;
quit
```

배치 실행이 필요하면 SQL 파일 경로를 마지막 인자로 넘길 수도 있습니다.

```bash
./build/sqlproc \
  --schema-dir ./examples/schemas \
  --data-dir ./demo-data \
  --index-dir ./demo-indexes \
  ./examples/demo.sql
```

예제 실행이 끝나면 아래 파일이 만들어집니다.

- `demo-data/users.csv`
- `demo-indexes/idx_users_age.idx`

## 파일 형식

### 1. 스키마 파일

경로 규칙은 `schemas/<table>.schema`입니다.

```text
id:int:pk,name:string,age:int
```

- 컬럼 순서가 CSV 헤더 순서가 됩니다.
- 타입은 `int` 또는 `string`만 사용합니다.
- `:pk`를 붙인 컬럼은 PRIMARY KEY로 동작하며 중복 `INSERT`를 막습니다.

### 2. 데이터 파일

경로 규칙은 `data/<table>.csv`입니다.

```text
id,name,age
1,kim,20
2,lee,30
```

- 첫 줄은 항상 헤더입니다.
- 첫 `INSERT` 때 파일이 없으면 헤더를 자동으로 만듭니다.

### 3. 인덱스 파일

경로 규칙은 `indexes/<index_name>.idx`입니다.

- 헤더 1개와 고정 크기 노드 배열로 저장합니다.
- leaf 노드는 `key`, `row_offset`, `next_leaf_id`를 저장합니다.
- internal 노드는 `key`, `child_id`를 저장합니다.

## B+ 트리 동작 설명

이 프로젝트의 B+ 트리는 성능보다 이해 가능성을 우선합니다.

- 한 노드가 가질 수 있는 최대 키 수는 `4`입니다.
- 키가 넘치면 leaf 또는 internal 노드를 둘로 나눕니다.
- 가운데 기준 키 하나를 부모로 올립니다.
- 부모도 가득 차 있으면 같은 과정을 위로 반복합니다.
- 맨 위 부모까지 올라갔는데 더 올라갈 곳이 없으면 새 루트를 만듭니다.

초심자가 흐름을 따라가기 쉽도록 재귀보다 반복문과 경로 배열을 더 많이
사용했습니다.

## 실행 흐름

프로그램은 아래 순서로 동작합니다.

1. CLI 입력 또는 SQL 파일에서 문장을 읽습니다.
2. 토큰으로 나눕니다.
3. 수동 파서로 AST를 만듭니다.
4. 실행기가 문장 종류에 따라 파일 입출력을 수행합니다.
5. PK가 있으면 `INSERT` 전에 중복 키를 검사합니다.
6. 인덱스가 있으면 row offset 후보를 먼저 모으고, 나머지 조건은 다시 검사합니다.

## 초심자에게 중요한 코드 읽기 포인트

- [src/parser.c](/Users/donghyunkim/Downloads/test_sql/week6-team5-sql/src/parser.c)
  `INSERT`, `SELECT`, `CREATE INDEX`를 수동 파싱합니다.
- [src/executor.c](/Users/donghyunkim/Downloads/test_sql/week6-team5-sql/src/executor.c)
  CSV 저장과 `WHERE` 비교, 실패 시 롤백을 담당합니다.
- [src/btree_index.c](/Users/donghyunkim/Downloads/test_sql/week6-team5-sql/src/btree_index.c)
  B+ 트리 삽입, split, 인덱스 조회를 담당합니다.
- [tests/test_runner.c](/Users/donghyunkim/Downloads/test_sql/week6-team5-sql/tests/test_runner.c)
  기능별 테스트 흐름을 한 파일에서 따라갈 수 있습니다.

## Git 브랜치와 GitHub 절차

이 저장소는 아래 규칙으로 개발합니다.

- 기본 브랜치: `main`
- 개발 통합 브랜치: `dev`
- 기능 브랜치: `feature/<기능명>`

모든 merge 전에는 아래 순서를 지킵니다.

1. 세션 로그를 Markdown 파일로 기록합니다.
2. 멀티 페르소나 코드 리뷰를 진행합니다.
3. GitHub Issue를 새로 만듭니다.
4. 이슈를 `6주차 5조 미니 SQL 처리기 보드`에 추가합니다.
5. 이슈 코멘트에 한국어 검증 결과를 남깁니다.
6. PR을 생성합니다.
7. 테스트 통과 후 merge합니다.

## 현재 테스트 범위

- 인자 파싱
- 토크나이저
- `INSERT`, `SELECT`, `CREATE INDEX` 파서
- 빈 SQL 파일 오류
- CSV 기반 `INSERT`와 `SELECT WHERE`
- 인덱스 생성과 재실행 후 조회
- 중복 키와 split 뒤 equality 조회

## 제한 사항

- 인덱스 스캔은 가독성을 위해 모든 leaf 노드를 읽고 후보를 모읍니다.
- 통계 기반 옵티마이저는 없습니다.
- 인덱스는 단일 컬럼만 지원합니다.
- 저장소는 append-only이며 `UPDATE`, `DELETE`는 구현하지 않았습니다.
