# `storage.c` / `executor.c` 구조 설명

> C언어 초심자를 위한 두 모듈의 역할 분리와 함수 흐름 안내

---

## 1. 두 파일의 역할 분리

SQL 처리기는 "무엇을 실행할지"와 "어떻게 파일에 저장할지"를 서로 다른 파일에 나눠 구현합니다.

```mermaid
flowchart LR
    subgraph executor["executor.c — SQL 로직"]
        E1["INSERT 검증<br/>(컬럼 이름·타입)"]
        E2["SELECT 검증<br/>(컬럼 이름·범위)"]
        E3["값 배치<br/>(스키마 순서 맞추기)"]
    end

    subgraph storage["storage.c — CSV 파일 입출력"]
        S1["파일 경로 조립"]
        S2["헤더 쓰기/검증"]
        S3["행 읽기/쓰기"]
    end

    executor -->|"storage_append_row()"| storage
    executor -->|"storage_print_rows()"| storage
```

- **`executor.c`**: SQL 문장 구조체를 받아 타입·이름 검증 후 스토리지에 요청
- **`storage.c`**: 파일 경로, 헤더, CSV 행 입출력만 담당

이렇게 나누면 저장 방식이 바뀌어도 executor.c를 고칠 필요가 없습니다.

---

## 2. 핵심 구조체 관계

```mermaid
flowchart TD
    AC["AppConfig<br/>schema_dir · data_dir<br/>input_path"]
    TS["TableSchema<br/>table_name<br/>columns[] · column_count"]
    CS["ColumnSchema<br/>name · type"]
    IS["InsertStatement<br/>table_name<br/>column_names[] · values[]<br/>has_column_list"]
    SS["SelectStatement<br/>table_name<br/>column_names[]<br/>select_all"]
    LV["LiteralValue<br/>type · text · location"]
    EI["ErrorInfo<br/>message · line · column"]

    AC -->|"경로 정보 제공"| TS
    TS -->|"columns[]"| CS
    IS -->|"values[]"| LV
    IS -->|"타입·이름 검증"| CS
    SS -->|"이름 검증"| CS
    EI -. "오류 발생 시 채워짐" .-> IS
    EI -. "오류 발생 시 채워짐" .-> SS
```

| 구조체 | 역할 |
|---|---|
| `AppConfig` | 실행에 필요한 디렉터리 경로 묶음 |
| `TableSchema` | 스키마 파일에서 읽은 컬럼 순서/타입 정보 |
| `ColumnSchema` | 컬럼 하나의 이름과 타입 (`int` / `string`) |
| `InsertStatement` | 파서가 만든 INSERT 문장 구조체 |
| `SelectStatement` | 파서가 만든 SELECT 문장 구조체 |
| `LiteralValue` | 숫자/문자열 리터럴과 그 위치 |
| `ErrorInfo` | 오류 메시지와 줄·열 위치 |

---

## 3. executor.c 함수 흐름

### 3-1. INSERT 전체 흐름

```mermaid
flowchart TD
    EP["execute_program()"]
    EI["execute_insert()"]
    LTS["load_table_schema()"]
    BIRV["build_insert_row_values()"]
    FSC["find_schema_column()"]
    VLT["validate_literal_type()"]
    SAR["storage_append_row()"]

    EP -->|"STATEMENT_INSERT"| EI
    EI --> LTS
    EI --> BIRV
    BIRV --> FSC
    BIRV --> VLT
    EI --> SAR
```

### 3-2. SELECT 전체 흐름

```mermaid
flowchart TD
    EP["execute_program()"]
    ES["execute_select()"]
    LTS["load_table_schema()"]
    RSC["resolve_selected_columns()"]
    FSC["find_schema_column()"]
    SPR["storage_print_rows()"]

    EP -->|"STATEMENT_SELECT"| ES
    ES --> LTS
    ES --> RSC
    RSC --> FSC
    ES --> SPR
```

---

## 4. executor.c 함수별 설명

### `execute_program()`

```mermaid
flowchart LR
    SP["SqlProgram<br/>(문장 목록)"]
    L{"문장 순회"}
    EI["execute_insert()"]
    ES["execute_select()"]
    OK["성공 반환"]
    NG["오류 반환"]

    SP --> L
    L -->|"INSERT"| EI
    L -->|"SELECT"| ES
    EI -->|"실패"| NG
    ES -->|"실패"| NG
    L -->|"모두 완료"| OK
```

- SqlProgram 안에 있는 모든 SQL 문장을 앞에서부터 순서대로 실행합니다.
- 하나라도 실패하면 즉시 0을 반환합니다.

---

### `build_insert_row_values()`

INSERT 구조체의 값들을 스키마 순서에 맞는 배열로 재배치합니다.

```mermaid
flowchart TD
    START["시작"]
    CHECK{"has_column_list?"}

    subgraph no_list["컬럼 목록 없음 (VALUES만)"]
        N1{"값 개수 == 스키마 컬럼 수?"}
        N2["타입 검증"]
        N3["순서 그대로 배치"]
    end

    subgraph with_list["컬럼 목록 있음"]
        W1["컬럼 이름 → 스키마 인덱스"]
        W2{"중복 컬럼?"}
        W3["타입 검증"]
        W4["해당 위치에 배치"]
    end

    END["row_values[] 완성"]
    ERR["오류 반환"]

    START --> CHECK
    CHECK -->|"없음"| N1
    N1 -->|"불일치"| ERR
    N1 -->|"일치"| N2
    N2 -->|"실패"| ERR
    N2 -->|"통과"| N3
    N3 --> END

    CHECK -->|"있음"| W1
    W1 -->|"없는 이름"| ERR
    W1 --> W2
    W2 -->|"있음"| ERR
    W2 -->|"없음"| W3
    W3 -->|"실패"| ERR
    W3 -->|"통과"| W4
    W4 --> END
```

**왜 재배치가 필요한가?**

```sql
-- 스키마: id:int, name:string, age:int
INSERT INTO users (age, id, name) VALUES (20, 1, 'kim');
```

파서는 `(age, id, name)` 순서 그대로 저장합니다. 하지만 CSV에는 스키마 순서(`id,name,age`)로 써야 합니다. `build_insert_row_values()`가 이름으로 스키마 위치를 찾아 재배치합니다.

---

### `resolve_selected_columns()`

`SELECT *` 또는 `SELECT col1, col2`를 스키마 인덱스 배열로 변환합니다.

```mermaid
flowchart TD
    START["시작"]
    SA{"select_all?"}
    ALL["0, 1, 2, ... 전체 인덱스 배열"]
    LOOP["컬럼 이름 → find_schema_column()"]
    FOUND{"인덱스 찾음?"}
    ERR["오류 반환"]
    END["selected_indices[] 완성"]

    START --> SA
    SA -->|"true (SELECT *)"| ALL --> END
    SA -->|"false"| LOOP
    LOOP --> FOUND
    FOUND -->|"없음 (-1)"| ERR
    FOUND -->|"찾음"| LOOP
    LOOP -->|"모두 완료"| END
```

결과로 만들어진 `selected_indices[]`는 storage.c에 전달되어 CSV에서 해당 열만 출력하는 데 사용됩니다.

---

## 5. storage.c 함수 흐름

### 5-1. `storage_append_row()` (INSERT)

```mermaid
flowchart TD
    SAR["storage_append_row()"]
    EDF["ensure_data_file()"]
    BTP["build_table_path()"]
    OPEN["fopen(ab) — 추가 모드"]
    WCR["write_csv_row()"]
    WCF["write_csv_field()"]
    OK["성공"]
    ERR["오류"]

    SAR --> EDF
    EDF -->|"실패"| ERR
    EDF -->|"성공"| BTP
    BTP --> OPEN
    OPEN -->|"실패"| ERR
    OPEN --> WCR
    WCR --> WCF
    WCF -->|"실패"| ERR
    WCR -->|"성공"| OK
```

### 5-2. `storage_print_rows()` (SELECT)

```mermaid
flowchart TD
    SPR["storage_print_rows()"]
    OPEN["fopen(rb) — 읽기 모드"]
    NOENT{"파일 없음?"}
    HEADER["헤더만 출력 후 종료"]
    VLL["validate_line_length()"]
    PCL["parse_csv_line()"]
    VHV["validate_header_values()"]
    PSH["print_selected_header()"]
    LOOP["행 순회"]
    PRINT["선택 컬럼만 출력"]
    OK["성공"]
    ERR["오류"]

    SPR --> OPEN
    OPEN --> NOENT
    NOENT -->|"예"| HEADER --> OK
    NOENT -->|"아니오"| VLL
    VLL -->|"실패"| ERR
    VLL -->|"통과"| PCL
    PCL -->|"실패"| ERR
    PCL -->|"통과"| VHV
    VHV -->|"실패"| ERR
    VHV -->|"통과"| PSH
    PSH --> LOOP
    LOOP -->|"다음 행"| VLL
    LOOP -->|"끝"| OK
    VLL -->|"행 통과"| PRINT --> LOOP
```

---

## 6. storage.c 함수별 설명

### `ensure_data_file()`

INSERT 전에 항상 호출됩니다. CSV 파일이 있는지 없는지에 따라 다르게 동작합니다.

```mermaid
flowchart TD
    START["ensure_data_file() 시작"]
    EXIST{"파일 존재?"}

    subgraph new_file["파일 없음 → 새로 생성"]
        NF1["fopen(wb) 로 새 파일 생성"]
        NF2["스키마 컬럼 이름을 쉼표로 연결해 헤더 행 기록"]
        NF3["파일 닫기"]
    end

    subgraph old_file["파일 있음 → 헤더 검증"]
        OF1["fgets()로 첫 줄 읽기"]
        OF2["validate_line_length()"]
        OF3["parse_csv_line()"]
        OF4["validate_header_values()"]
        OF5["파일 닫기"]
    end

    OK["성공"]
    ERR["오류"]

    START --> EXIST
    EXIST -->|"없음"| NF1 --> NF2 --> NF3 --> OK
    EXIST -->|"있음"| OF1
    OF1 -->|"읽기 실패"| ERR
    OF1 --> OF2
    OF2 -->|"너무 긴 줄"| ERR
    OF2 --> OF3
    OF3 -->|"CSV 형식 오류"| ERR
    OF3 --> OF4
    OF4 -->|"헤더 불일치"| ERR
    OF4 -->|"일치"| OF5 --> OK
```

**왜 헤더를 검증하는가?**

스키마 파일을 바꾼 뒤 기존 CSV 파일을 그대로 사용하면 컬럼 구조가 달라집니다. 이를 미리 감지해 잘못된 데이터 입력을 막습니다.

---

### `validate_line_length()`

`fgets()`는 버퍼 크기만큼만 읽습니다. 줄이 버퍼보다 길면 뒷부분이 잘립니다. 이 함수는 그 상황을 감지합니다.

```mermaid
flowchart LR
    A["fgets() 호출 후"]
    B{"버퍼 꽉 참<br/>AND 마지막 문자 != '\\n'<br/>AND EOF 아님?"}
    C["오류: 줄이 너무 깁니다."]
    D["정상 — 계속 진행"]

    A --> B
    B -->|"예"| C
    B -->|"아니오"| D
```

세 조건이 모두 참일 때만 오류입니다. EOF 직전 마지막 줄은 `\n`이 없어도 정상입니다.

---

### `parse_csv_line()`

CSV 한 줄을 컬럼 배열로 분해합니다. 큰따옴표 안의 쉼표는 구분자로 취급하지 않습니다.

```mermaid
flowchart TD
    START["문자 하나씩 순회"]
    QC{"큰따옴표?"}
    TQ["연속 두 따옴표<br/>(&quot;&quot;&quot;)"]
    ESCAPE["따옴표 문자 저장"]
    TOGGLE["in_quotes 반전"]
    CM{"쉼표<br/>AND not in_quotes?"}
    NEXT["다음 컬럼으로"]
    STORE["현재 컬럼에 문자 추가"]
    END["values[] 배열 완성"]

    START --> QC
    QC -->|"예, in_quotes"| TQ
    TQ -->|"예"| ESCAPE --> START
    TQ -->|"아니오"| TOGGLE --> START
    QC -->|"아니오"| CM
    CM -->|"예"| NEXT --> START
    CM -->|"아니오"| STORE --> START
    START -->|"줄 끝"| END
```

예시:

```
kim,"lee,park",30
```

→ `["kim", "lee,park", "30"]` (쉼표가 따옴표 안에 있으면 구분자 아님)

---

### `write_csv_field()`

CSV 필드 하나를 안전하게 파일에 씁니다. 쉼표·따옴표·개행이 포함된 경우 자동으로 큰따옴표로 감쌉니다.

```mermaid
flowchart TD
    START["텍스트 스캔"]
    NQ{"쉼표/따옴표/개행<br/>포함?"}
    PLAIN["그대로 fputs()"]
    OPEN["'&quot;' 출력"]
    LOOP["문자 순회"]
    DQ{"'&quot;'?"}
    ESC["'&quot;' 한 번 더 출력"]
    CHAR["문자 출력"]
    CLOSE["'&quot;' 출력"]
    END["완료"]

    START --> NQ
    NQ -->|"없음"| PLAIN --> END
    NQ -->|"있음"| OPEN --> LOOP
    LOOP --> DQ
    DQ -->|"예"| ESC --> CHAR
    DQ -->|"아니오"| CHAR
    CHAR --> LOOP
    LOOP -->|"끝"| CLOSE --> END
```

예시: `she said "hi"` → `"she said ""hi"""` (내부 따옴표는 두 번 씁니다)

---

### `validate_header_values()`

CSV 파일의 헤더 행이 현재 스키마와 일치하는지 두 단계로 확인합니다.

```mermaid
flowchart TD
    A["header_count 확인"]
    B{"== schema->column_count?"}
    C["오류: 컬럼 수 불일치"]
    D["이름 순서 확인 (0 ~ N-1)"]
    E{"header[i] == schema.columns[i].name?"}
    F["오류: 헤더 순서 불일치"]
    G["검증 통과"]

    A --> B
    B -->|"아니오"| C
    B -->|"예"| D
    D --> E
    E -->|"아니오"| F
    E -->|"예, 다음"| D
    D -->|"모두 일치"| G
```

---

## 7. 두 모듈의 협력 요약

```mermaid
sequenceDiagram
    participant EP as execute_program()
    participant EI as execute_insert()
    participant ES as execute_select()
    participant ST as storage.c

    EP->>EI: INSERT 문장 전달
    EI->>EI: 스키마 로드
    EI->>EI: build_insert_row_values()
    EI->>ST: storage_append_row()
    ST->>ST: ensure_data_file()
    ST->>ST: write_csv_row()
    ST-->>EI: 성공/실패

    EP->>ES: SELECT 문장 전달
    ES->>ES: 스키마 로드
    ES->>ES: resolve_selected_columns()
    ES->>ST: storage_print_rows()
    ST->>ST: 헤더 검증
    ST->>ST: 행 순회 + 출력
    ST-->>ES: 성공/실패
```

executor.c는 "무엇을"만 결정하고, storage.c는 "어떻게 파일에"만 집중합니다. 두 모듈이 `storage_append_row()` / `storage_print_rows()` 두 함수로만 연결되기 때문에, 한 쪽을 수정해도 다른 쪽에 영향이 최소화됩니다.
