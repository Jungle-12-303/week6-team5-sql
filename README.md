# 초심자 친화형 C99 SQL 처리기

SQL 파일을 실행하면 CSV 파일에 데이터를 읽고 쓰는 작은 SQL 처리기입니다.
초심자가 읽기 쉽도록 `C99`의 기본 문법만 사용했고, 어려운 흐름에는
한국어 주석을 붙였습니다.

## 한눈에 보기

- 지원 문장: `INSERT`, `SELECT`
- 데이터 저장: `CSV`
- 빌드 기준: `-std=c99 -Wall -Wextra -Werror`

## 지원 SQL

```sql
INSERT INTO users VALUES (1, 'kim', 20);
INSERT INTO users (id, name, age) VALUES (2, 'lee', 30);
SELECT * FROM users;
SELECT name, age FROM users;
```

## 지원 범위

- 단일 테이블만 처리합니다.
- 지원 타입은 `int`, `string` 두 가지입니다.
- 문자열 최대 길이는 63자입니다.

## 지원하지 않는 기능

- `WHERE`, `AND`, `OR`
- `JOIN`
- `ORDER BY`
- `UPDATE`
- `DELETE`
- `CREATE TABLE`, `CREATE INDEX`
- 복합 인덱스, Primary KEY

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

```bash
mkdir -p ./demo-data
./build/sqlproc \
  --schema-dir ./examples/schemas \
  --data-dir ./demo-data \
  ./examples/demo.sql
```

실행이 끝나면 `demo-data/users.csv`가 만들어집니다.

## 파일 형식

### 1. 스키마 파일

경로 규칙은 `schemas/<table>.schema`입니다.

```text
id:int,name:string,age:int
```

- 형식: `컬럼명:타입`
- 타입은 `int` 또는 `string`만 사용합니다.
- 컬럼 순서가 CSV 헤더 순서가 됩니다.

### 2. 데이터 파일

경로 규칙은 `data/<table>.csv`입니다.

```text
id,name,age
1,kim,20
2,lee,30
```

- 첫 줄은 항상 헤더입니다.
- 첫 `INSERT` 때 파일이 없으면 헤더를 자동으로 만듭니다.

## 실행 흐름

프로그램은 아래 순서로 동작합니다.

1. SQL 파일에서 문장을 읽습니다.
2. 토큰으로 나눕니다.
3. 수동 파서로 AST를 만듭니다.
4. 실행기가 문장 종류에 따라 CSV 파일 입출력을 수행합니다.

## 초심자에게 중요한 코드 읽기 포인트

- [src/parser.c](src/parser.c)
  `INSERT`, `SELECT`를 수동 파싱합니다.
- [src/executor.c](src/executor.c)
  CSV 저장과 읽기를 담당합니다.
- [tests/test_runner.c](tests/test_runner.c)
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

- 인자 파싱 (성공·실패)
- 토크나이저 (SELECT 문장)
- `INSERT` 파서 (컬럼 목록 있음·없음)
- 빈 SQL 파일 오류
- CSV 기반 `INSERT`와 `SELECT` 통합 실행
