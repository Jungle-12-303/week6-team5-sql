# sqlproc 코드베이스 입문 가이드

C를 막 배운 사람이 이 프로젝트를 처음 읽을 때 참고하는 문서입니다.  
코드 구조, 데이터 흐름, 핵심 자료구조를 단계별로 설명합니다.

---

## 목차

1. [프로젝트 한 줄 요약](#1-프로젝트-한-줄-요약)
2. [디렉토리 구조](#2-디렉토리-구조)
3. [전체 실행 흐름](#3-전체-실행-흐름)
4. [모듈별 역할](#4-모듈별-역할)
5. [중앙 헤더 sqlproc.h 읽는 법](#5-중앙-헤더-sqlproch-읽는-법)
6. [SQL 처리 3단계: 토큰화 → 파싱 → 실행](#6-sql-처리-3단계-토큰화--파싱--실행)
7. [데이터 저장 방식 (CSV)](#7-데이터-저장-방식-csv)
8. [오류 처리 패턴](#8-오류-처리-패턴)
9. [빌드 및 실행 방법](#9-빌드-및-실행-방법)

> executor.c · storage.c의 상세 함수 흐름은 [docs/storage-executor.md](docs/storage-executor.md)에서 다룹니다.

---

## 1. 프로젝트 한 줄 요약

> **SQL 파일을 실행하면, CSV 파일에 데이터를 읽고 쓰는 미니 데이터베이스**

지원하는 SQL:

```sql
-- 테이블에 행 삽입 (컬럼 순서 지정)
INSERT INTO users (id, name, age) VALUES (1, 'kim', 20);

-- 테이블에 행 삽입 (스키마 순서 그대로)
INSERT INTO users VALUES (2, 'lee', 30);

-- 전체 조회
SELECT * FROM users;

-- 특정 컬럼만 조회
SELECT name, age FROM users;

-- WHERE 조건 조회 (full scan)
SELECT name, age FROM users WHERE age >= 25;
SELECT * FROM users WHERE age >= 25 AND name = 'lee';
```

지원하지 않는 SQL (의도적으로 제외):

```sql
-- OR, JOIN, ORDER BY, UPDATE, DELETE 등은 미구현
SELECT * FROM users WHERE age >= 20 OR name = 'kim';   -- ✗
```

---

## 2. 디렉토리 구조

```
week6-team5-sql/
├── include/
│   └── sqlproc.h       ← 모든 .c 파일이 공유하는 "공용 계약"
├── src/
│   ├── main.c          ← 프로그램 시작점 (진입점)
│   ├── app.c           ← 명령줄 인자 파싱, SQL 파일 실행
│   ├── tokenizer.c     ← SQL 문자열 → 토큰 조각
│   ├── parser.c        ← 토큰 → SQL 문장 구조체
│   ├── schema.c        ← 테이블 스키마 파일 읽기
│   ├── executor.c      ← SQL 검증 및 실행 흐름 제어
│   └── storage.c       ← CSV 파일 경로·헤더·행 입출력
├── tests/
│   └── test_runner.c   ← 통합 테스트
├── examples/
│   ├── schemas/
│   │   └── users.schema
│   ├── demo.sql
│   ├── where_demo.sql
│   └── user_input.sql
└── Makefile
```

### C에서 헤더(.h)와 소스(.c)의 관계

```
sqlproc.h               (설계도 — 구조체, 함수 선언)
    ↑ #include
tokenizer.c  parser.c  executor.c  storage.c ...  (실제 구현체)
```

> **비유**: `.h`는 레스토랑 메뉴판(무슨 요리가 있는지), `.c`는 주방(요리를 실제로 만드는 곳).

---

## 3. 전체 실행 흐름

```mermaid
flowchart TD
    A(["input.sql 파일"]) --> B["app.c\nSQL 파일 읽기"]
    B --> C["tokenizer.c\n문자열 → 토큰 배열"]
    C --> D["parser.c\n토큰 → SQL 문장 구조체"]
    D --> E["executor.c\nSQL 검증 및 실행 흐름 제어"]
    E --> F["schema.c\n스키마 파일 읽기"]
    E -->|"storage_append_row()\nstorage_print_rows()"| H["storage.c\nCSV 파일 입출력"]
    H --> G[("CSV 파일\n데이터 저장/읽기")]

    style A fill:#e8f5e9
    style G fill:#fff9c4
```

### 단계별 변환 예시

SQL 한 줄 `SELECT name, age FROM users WHERE age >= 25;`이 어떻게 처리되는지 따라가 봅니다.

**① 토큰화** (문자열 → 조각)

```
"SELECT name, age FROM users WHERE age >= 25;"

[SELECT] [name] [,] [age] [FROM] [users] [WHERE] [age] [>=] [25] [;] [EOF]
```

**② 파싱** (조각 → 구조체)

```c
SelectStatement {
    table_name   = "users"
    select_all   = 0
    column_count = 2
    column_names = ["name", "age"]
    where_clause = [age >= 25]
}
```

**③ 실행** (구조체 → 파일 I/O)

```
1. users.schema 로드 → 컬럼 순서 파악
2. WHERE 컬럼과 리터럴 타입이 스키마와 맞는지 확인
3. users.csv 열기
4. 헤더 행 읽어 스키마와 일치 확인
5. 나머지 행을 한 줄씩 읽어 조건에 맞는 name, age만 출력
```

---

## 4. 모듈별 역할

```mermaid
graph LR
    subgraph 입력 처리
        A["main.c\n진입점"]
        B["app.c\n파일 읽기·파이프라인 조율"]
    end
    subgraph SQL 컴파일
        C["tokenizer.c\n어휘 분석"]
        D["parser.c\n구문 분석"]
    end
    subgraph 실행
        E["executor.c\nSQL 검증·실행 제어"]
        F["schema.c\n스키마 로더"]
        G["storage.c\nCSV 파일 입출력"]
    end

    A --> B --> C --> D --> E
    E --> F
    E --> G
```

| 파일 | 주요 함수 | 한 줄 설명 |
|------|-----------|------------|
| `main.c` | `main()` | 인자 파싱 후 `run_program()` 호출 |
| `app.c` | `parse_arguments()`, `run_program()` | 인자 검증, SQL 파일 읽기, 파이프라인 실행 |
| `tokenizer.c` | `tokenize_sql()` | 문자열을 `TokenList`로 변환 |
| `parser.c` | `parse_program()` | `TokenList`를 `SqlProgram`으로 변환 |
| `schema.c` | `load_table_schema()` | `.schema` 파일 읽어 `TableSchema` 반환 |
| `executor.c` | `execute_program()` | SQL 타입·이름 검증, storage.c 호출 |
| `storage.c` | `storage_append_row()`, `storage_print_rows()` | CSV 경로·헤더·행 읽기/쓰기 |

---

## 5. 중앙 헤더 sqlproc.h 읽는 법

모든 모듈이 공유하는 타입과 상수가 `include/sqlproc.h` 하나에 모여 있습니다.

### 상수 (크기 제한)

```c
#define SQLPROC_MAX_NAME_LEN  64   // 컬럼명·테이블명 최대 63자
#define SQLPROC_MAX_VALUE_LEN 64   // 값 문자열 최대 63자
#define SQLPROC_MAX_COLUMNS   16   // 한 테이블 최대 16개 컬럼
#define SQLPROC_MAX_PREDICATES 2   // WHERE 조건 최대 2개
#define SQLPROC_MAX_TOKENS   512   // SQL 한 문자열당 최대 토큰 수
#define SQLPROC_MAX_STATEMENTS 32  // SQL 파일 최대 문장 수
#define SQLPROC_MAX_SQL_SIZE 8192  // SQL 파일 최대 크기
```

> C에서는 배열 크기를 미리 정해야 합니다.  
> 이 값을 넘으면 프로그램이 오류를 반환합니다.

### 핵심 열거형(enum)

```c
/* 토크나이저가 만드는 토큰 종류 */
typedef enum {
    TOKEN_EOF,              // 입력 끝
    TOKEN_IDENTIFIER,       // 테이블명, 컬럼명 등
    TOKEN_NUMBER,           // 숫자 리터럴: 1, 20, -5
    TOKEN_STRING,           // 문자열 리터럴: 'kim'
    TOKEN_COMMA,            // ,
    TOKEN_SEMICOLON,        // ;
    TOKEN_LPAREN,           // (
    TOKEN_RPAREN,           // )
    TOKEN_STAR,             // *
    TOKEN_EQUAL,            // =
    TOKEN_LESS,             // <
    TOKEN_LESS_EQUAL,       // <=
    TOKEN_GREATER,          // >
    TOKEN_GREATER_EQUAL,    // >=
    TOKEN_KEYWORD_INSERT,   // INSERT
    TOKEN_KEYWORD_INTO,     // INTO
    TOKEN_KEYWORD_VALUES,   // VALUES
    TOKEN_KEYWORD_SELECT,   // SELECT
    TOKEN_KEYWORD_FROM,     // FROM
    TOKEN_KEYWORD_WHERE,    // WHERE
    TOKEN_KEYWORD_AND,      // AND
    TOKEN_KEYWORD_OR        // OR
} TokenType;

/* 파서가 구분하는 SQL 문장 종류 */
typedef enum {
    STATEMENT_INSERT,
    STATEMENT_SELECT
} StatementType;

/* WHERE 절 비교 연산자 */
typedef enum {
    COMPARE_EQUAL,
    COMPARE_LESS,
    COMPARE_LESS_EQUAL,
    COMPARE_GREATER,
    COMPARE_GREATER_EQUAL
} CompareOperator;
```

### 핵심 구조체 관계도

```mermaid
classDiagram
    class SqlProgram {
        Statement items[32]
        int count
    }
    class Statement {
        StatementType type
        SourceLocation location
        InsertStatement insert_statement
        SelectStatement select_statement
    }
    class InsertStatement {
        char table_name[64]
        int has_column_list
        int column_count
        int value_count
        char column_names[16][64]
        LiteralValue values[16]
    }
    class SelectStatement {
        char table_name[64]
        int select_all
        int column_count
        char column_names[16][64]
        WhereClause where_clause
    }
    class WhereClause {
        int count
        Predicate items[2]
    }
    class Predicate {
        char column_name[64]
        CompareOperator operator_type
        LiteralValue value
    }
    class TableSchema {
        char table_name[64]
        int column_count
        ColumnSchema columns[16]
    }
    class ColumnSchema {
        char name[64]
        DataType type
    }

    SqlProgram "1" --> "*" Statement
    Statement --> InsertStatement
    Statement --> SelectStatement
    SelectStatement --> WhereClause
    WhereClause --> Predicate
    TableSchema "1" --> "*" ColumnSchema
```

### AppConfig — 실행 설정

```c
/* parse_arguments()가 채워 주는 구조체 */
typedef struct {
    char schema_dir[256];  // --schema-dir 값
    char data_dir[256];    // --data-dir 값
    char input_path[256];  // SQL 파일 경로
} AppConfig;
```

---

## 6. SQL 처리 3단계: 토큰화 → 파싱 → 실행

### 6-1. 토큰화 (tokenizer.c)

SQL 문자열을 의미 있는 조각(토큰)으로 자릅니다.

```c
/* tokenize_sql() 호출 예시 */
TokenList tokens;
ErrorInfo error;
tokenize_sql("SELECT * FROM users;", &tokens, &error);

/* 결과: tokens.items[] */
/* [0] type=TOKEN_KEYWORD_SELECT, text="SELECT" */
/* [1] type=TOKEN_STAR,           text="*"      */
/* [2] type=TOKEN_KEYWORD_FROM,   text="FROM"   */
/* [3] type=TOKEN_IDENTIFIER,     text="users"  */
/* [4] type=TOKEN_SEMICOLON,      text=";"      */
/* [5] type=TOKEN_EOF,            text=""       */
```

**토큰화 흐름:**

```mermaid
flowchart LR
    A["문자 'S'"] --> B{알파벳?}
    B -- 예 --> C["read_word()\n단어 전체 읽기"]
    C --> D{"예약어?\nkeyword_type()"}
    D -- SELECT --> E[TOKEN_KEYWORD_SELECT]
    D -- 아니면 --> F[TOKEN_IDENTIFIER]

    G["문자 '2'"] --> H{숫자?}
    H -- 예 --> I["read_number()\n정수 읽기"]
    I --> J[TOKEN_NUMBER]

    K["문자 '''"] --> L{작은따옴표?}
    L -- 예 --> M["read_string()\n닫힘 따옴표까지 읽기"]
    M --> N[TOKEN_STRING]
```

**keyword_type() 동작 원리:**

```c
/* 소문자로 변환한 단어를 예약어 목록과 비교합니다 */
static TokenType keyword_type(const char *text)
{
    if (strcmp(text, "select") == 0) return TOKEN_KEYWORD_SELECT;
    if (strcmp(text, "from")   == 0) return TOKEN_KEYWORD_FROM;
    if (strcmp(text, "insert") == 0) return TOKEN_KEYWORD_INSERT;
    if (strcmp(text, "where")  == 0) return TOKEN_KEYWORD_WHERE;
    if (strcmp(text, "and")    == 0) return TOKEN_KEYWORD_AND;
    if (strcmp(text, "or")     == 0) return TOKEN_KEYWORD_OR;
    /* ... */
    return TOKEN_IDENTIFIER;  /* 예약어가 아니면 식별자 */
}
```

### 6-2. 파싱 (parser.c)

토큰 배열을 받아 SQL 문장 구조체(`SqlProgram`)를 만듭니다.  
**재귀 하강 파서(Recursive Descent Parser)** 패턴을 사용합니다.

> **재귀 하강 파서란?**  
> 문법 규칙마다 함수를 하나씩 만들고, 함수들이 서로 호출해 가며 토큰을 소비하는 방식입니다.  
> `parse_program` → `parse_statement` → `parse_select_statement` 순서로 호출됩니다.

**파서 핵심 패턴:**

```c
/* consume_token: 기대한 토큰이면 소비하고, 아니면 오류 */
static int consume_token(ParserState *state,
                         TokenType expected,
                         ErrorInfo *error,
                         const char *message)
{
    if (current_token(state)->type != expected) {
        set_error(error, current_token(state), message);
        return 0;  /* 실패 */
    }
    advance_token(state);  /* 다음 토큰으로 이동 */
    return 1;              /* 성공 */
}
```

**SELECT 파싱 흐름:**

```mermaid
flowchart TD
    A[parse_program] --> B[parse_statement]
    B -- SELECT 토큰 발견 --> C[parse_select_statement]
    C --> D["consume(SELECT)"]
    D --> E{다음 토큰?}
    E -- "*" --> F["select_all = 1\nadvance()"]
    E -- 컬럼명 --> G["컬럼 목록 수집\nparse_identifier() 반복"]
    F --> H["consume(FROM)"]
    G --> H
    H --> I["테이블명 읽기\nparse_identifier()"]
    I --> J{"WHERE 키워드?"}
    J -- 없음 --> K[완료]
    J -- 있음 --> L["parse_where_clause()\n비교 조건 1~2개 + AND"]
    L --> K
```

현재 `parse_where_clause()`는 비교 조건 1개 또는 2개만 허용하고, 조건 연결은 `AND`만 지원합니다. `OR`가 나오면 파서 단계에서 바로 실패합니다.

**INSERT 파싱 흐름:**

```mermaid
flowchart TD
    A[parse_insert_statement] --> B["consume(INSERT INTO)"]
    B --> C["테이블명 읽기"]
    C --> D{"다음 토큰이 '('?"}
    D -- 예 --> E["컬럼 목록 읽기\nhas_column_list = 1"]
    D -- 아니오 --> F["has_column_list = 0"]
    E --> G["consume(VALUES)"]
    F --> G
    G --> H["consume('(')"]
    H --> I["parse_value_list()\n값 목록 읽기"]
    I --> J["consume(')')"]
    J --> K[완료]
```

### 6-3. 실행 (executor.c + storage.c)

executor.c는 SQL 검증과 흐름 제어만 담당하고, 파일 읽기/쓰기는 storage.c에 완전히 위임합니다.

```mermaid
flowchart LR
    subgraph executor["executor.c — SQL 로직"]
        E1["타입·이름 검증"]
        E2["WHERE 유효성 확인"]
        E3["컬럼 순서 재배치"]
    end
    subgraph storage["storage.c — 파일 입출력"]
        S1["CSV 헤더 생성·검증"]
        S2["행 읽기·쓰기"]
        S3["full scan + WHERE 비교"]
    end
    executor -->|"storage_append_row()\nstorage_print_rows()"| storage
```

**INSERT 실행 흐름:**

```mermaid
flowchart TD
    A["execute_insert()"] --> B["load_table_schema()\n스키마 파일 읽기"]
    B --> C["build_insert_row_values()\n구조체 값을 스키마 순서로 정렬"]
    C --> D["storage_append_row()"]

    subgraph storage_detail["storage.c 내부"]
        D --> E["ensure_data_file()\nCSV 없으면 헤더 생성, 있으면 헤더 검증"]
        E --> F["fopen(path, 'ab')\n추가 모드로 열기"]
        F --> G["write_csv_row()\nCSV 끝에 행 추가"]
    end
```

> **"ab" 모드란?**  
> `"a"` = append(추가), `"b"` = binary.  
> 파일 끝에 이어 쓰기 때문에 기존 데이터를 덮어쓰지 않습니다.

**build_insert_row_values() 가 하는 일:**

```c
/* 컬럼 목록 없을 때: 스키마 순서 그대로 */
INSERT INTO users VALUES (1, 'kim', 20);
→ row_values[0]="1", row_values[1]="kim", row_values[2]="20"

/* 컬럼 목록 있을 때: 이름으로 위치 찾아 배치 */
INSERT INTO users (age, name, id) VALUES (20, 'kim', 1);
→ row_values[0]="1"(id), row_values[1]="kim"(name), row_values[2]="20"(age)
/* 스키마 순서(id, name, age)에 맞게 재정렬됩니다 */
```

**SELECT 실행 흐름:**

```mermaid
flowchart TD
    A["execute_select()"] --> B["load_table_schema()\n스키마 파일 읽기"]
    B --> C["resolve_selected_columns()\nSELECT * 또는 컬럼명 → 스키마 인덱스 배열"]
    C --> D["validate_where_clause()\nWHERE 컬럼/타입/정수 범위 확인"]
    D --> E["storage_print_rows()"]

    subgraph storage_detail["storage.c 내부"]
        E --> F{"CSV 파일 없음?"}
        F -- ENOENT --> G["헤더만 출력하고 종료\n빈 테이블 처리"]
        F -- 정상 --> H["헤더 행 읽기\n스키마와 일치 확인"]
        H --> I["print_selected_header()\n선택 컬럼명 출력"]
        I --> J["행 반복: fgets()"]
        J --> K["parse_csv_line()\nCSV 행 파싱"]
        K --> L["row_matches_where()\n조건 비교"]
        L --> M{"조건 만족?"}
        M -- 예 --> N["선택 컬럼만 탭 구분 출력"]
        M -- 아니오 --> J
        N --> J
        J -- EOF --> O[완료]
    end
```

**validate_where_clause()가 하는 일:**

- `WHERE`에 적은 컬럼이 스키마에 실제로 있는지 확인합니다.
- 리터럴 타입이 스키마 타입과 맞는지 확인합니다.
- `int` 컬럼 비교일 때는 `strtol()`로 실제 `int` 범위 안에 들어오는지도 같이 확인합니다.

**parse_csv_line() 가 처리하는 CSV 형식:**

```c
/* 일반 값 */
1,kim,20         → ["1", "kim", "20"]

/* 쉼표를 포함한 값은 큰따옴표로 감쌈 */
1,"lee, junior",30  → ["1", "lee, junior", "30"]

/* 내부 큰따옴표는 두 번 씀 */
1,"say ""hi""",20   → ["1", "say \"hi\"", "20"]
```

---

## 7. 데이터 저장 방식 (CSV)

### 스키마 파일 (.schema)

```
examples/schemas/users.schema:

id:int,name:string,age:int
```

- 형식: `컬럼명:타입`
- 타입: `int` 또는 `string` 만 지원
- 컬럼 순서가 CSV 헤더 순서가 됩니다

```c
/* load_table_schema()가 반환하는 구조체 */
TableSchema {
    table_name   = "users"
    column_count = 3
    columns[0]   = { name="id",   type=DATA_TYPE_INT    }
    columns[1]   = { name="name", type=DATA_TYPE_STRING }
    columns[2]   = { name="age",  type=DATA_TYPE_INT    }
}
```

### 데이터 파일 (.csv)

```csv
id,name,age
1,kim,20
2,"lee, junior",30
3,"quote ""test""",25
```

- 첫 줄: 헤더 (스키마 순서와 반드시 일치해야 함)
- 쉼표나 큰따옴표를 포함한 값은 `"..."` 로 감쌈
- 내부 큰따옴표는 `""` 로 이스케이프

### CSV 파일이 자동으로 생성되는 시점

```mermaid
flowchart TD
    A["INSERT 실행"] --> B{"CSV 파일\n존재하는가?"}
    B -- 아니오 --> C["fopen('wb') 로 생성\n헤더 행 먼저 기록\n예: id,name,age"]
    B -- 예 --> D["fopen('rb') 로 열어\n헤더가 스키마와 일치하는지 검증"]
    C --> E["fopen('ab') 로 열어\n데이터 행 추가"]
    D --> E
```

---

## 8. 오류 처리 패턴

이 프로젝트의 모든 함수는 같은 방식으로 오류를 반환합니다.

```c
/* 함수 시그니처 패턴 */
int some_function(..., ErrorInfo *error);
/*  ↑ 반환값: 1=성공, 0=실패          */
/*                       ↑ 오류 상세를 여기에 기록 */
```

```c
/* ErrorInfo 구조체 */
typedef struct {
    char message[256];  /* 오류 메시지                    */
    int  line;          /* SQL 내 줄 번호 (파일 오류면 0) */
    int  column;        /* SQL 내 열 번호 (파일 오류면 0) */
} ErrorInfo;
```

**사용 예시:**

```c
/* 호출하는 쪽 */
TableSchema schema;
ErrorInfo error;

if (!load_table_schema(config->schema_dir, "users", &schema, &error)) {
    /* 실패: error.message에 이유가 담겨 있음 */
    print_error(&error);
    return 0;
}
/* 성공: schema를 안심하고 사용 가능 */
```

> **C 초보자 팁**: C에는 `try/catch`가 없습니다.  
> 대신 함수가 `int` (1=성공, 0=실패)를 반환하고,  
> 오류 상세는 포인터(`ErrorInfo *error`)로 전달받는 관례를 씁니다.

**오류 전파 흐름:**

```mermaid
flowchart LR
    A["tokenizer.c\n어휘 오류"] --> E["app.c\nprint_error() 출력"]
    B["parser.c\n구문 오류"] --> E
    C["executor.c\n런타임 오류"] --> E
    D["schema.c\n파일 오류"] --> E
    S["storage.c\nCSV 파일 오류"] --> E

    style E fill:#ffcdd2
```

---

## 9. 빌드 및 실행 방법

### 빌드

```bash
make          # build/sqlproc 생성
make test     # 테스트 실행
make clean    # build/ 삭제
```

### 실행 (파일 모드)

```bash
./build/sqlproc \
  --schema-dir examples/schemas \
  --data-dir   /tmp/data \
  examples/demo.sql
```

`WHERE` 예시를 바로 실행해 보고 싶다면 아래 파일을 사용하면 됩니다.

```bash
./build/sqlproc \
  --schema-dir examples/schemas \
  --data-dir   /tmp/data \
  examples/where_demo.sql
```

인자 설명:

| 인자 | 설명 |
|------|------|
| `--schema-dir <dir>` | `.schema` 파일들이 있는 디렉터리 |
| `--data-dir <dir>` | `.csv` 파일들이 저장되는 디렉터리 |
| `<input.sql>` | 실행할 SQL 파일 경로 |

### 스키마 파일 예시

```
# examples/schemas/users.schema
id:int,name:string,age:int
```

### SQL 파일 예시

```sql
-- examples/demo.sql
INSERT INTO users VALUES (1, 'kim', 20);
INSERT INTO users VALUES (2, 'lee', 30);
INSERT INTO users VALUES (3, 'park', 40);
SELECT * FROM users;
SELECT id, name FROM users;
```

```sql
-- examples/where_demo.sql
SELECT name, age FROM users WHERE age >= 25;
SELECT id, name FROM users WHERE age >= 25 AND name = 'lee';
```

### 실행 결과

```
id	name	age
1	kim	20
2	lee	30
3	park	40
id	name
1	kim
2	lee
3	park
```

실행이 끝나면 결과 표는 `stdout`에, 총 실행 시간은 `stderr`에 각각 출력됩니다.

```text
총 실행 시간: 0.284 ms
```

---

## 부록: 코드 읽기 순서 추천

처음 코드를 읽을 때는 이 순서를 따르면 전체 맥락이 잡힙니다.

```
1. include/sqlproc.h       ← 전체 타입 훑어보기 (20분)
2. src/main.c              ← 진입점 확인 (5분)
3. src/app.c               ← 파일 읽기와 파이프라인 연결 (15분)
4. src/tokenizer.c         ← tokenize_sql() 함수 (20분)
5. src/parser.c            ← parse_insert_statement(),
                              parse_select_statement() (25분)
6. src/schema.c            ← load_table_schema() 함수 (15분)
7. src/executor.c          ← execute_insert(), execute_select() (20분)
8. src/storage.c           ← storage_append_row(), storage_print_rows() (20분)
```

> executor.c와 storage.c는 함께 읽는 것을 권장합니다.  
> executor.c가 "무엇을"만 결정하고, storage.c가 "어떻게 파일에" 쓰는지 경계를 확인하세요.  
> 자세한 함수 흐름은 [docs/storage-executor.md](docs/storage-executor.md)를 참고합니다.

> 각 파일은 독립적으로 읽을 수 있습니다.  
> 막히면 `sqlproc.h`에서 관련 구조체를 먼저 확인하세요.
