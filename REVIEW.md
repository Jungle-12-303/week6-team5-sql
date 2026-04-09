# 코드 리뷰 — simplify 브랜치

## 우선순위 기준
- **P1 (HIGH)**: 데이터 손상 또는 잘못된 동작
- **P2 (MEDIUM)**: 미정의 동작(UB) 가능성, 잠재적 버그
- **P3 (LOW)**: 방어적 코딩, 명확성 개선

---

## P1 — HIGH

### [1] CSV 행 무음 잘림 — `executor.c`

**위치**: `execute_select` (line 530), `ensure_data_file` (line 157)

**문제**: `EXECUTOR_MAX_ROW_LEN`은 1024바이트다. `fgets`는 버퍼가 가득 차면 나머지를 자른 뒤 **에러 없이 리턴**한다. 이후 코드는 잘린 반쪽 줄을 정상 CSV 행으로 처리하여 컬럼 수 불일치 오류를 내거나, 잘못된 데이터를 읽는다.

**재현 조건**: 하나의 CSV 행이 1023바이트를 초과하는 경우.

**수정**: `fgets` 직후 `strlen(line) == sizeof(line) - 1 && line[마지막] != '\n'` 조건으로 잘림 여부를 감지하고 오류 반환.

---

## P2 — MEDIUM

### [2] `previous_token()` 경계 검사 없음 — `parser.c:35`

**위치**: `parser.c`, line 35

**문제**: `state->position - 1`을 검사 없이 인덱스로 사용한다. `position == 0`일 때 `items[-1]`을 참조하므로 **정의되지 않은 동작(UB)**이다. 현재 호출 경로에서는 항상 `position >= 5`이므로 문제가 발생하지 않지만, 함수 자체가 호출 맥락에 의존하는 구조라 취약하다.

**수정**: 함수 내부에 `position == 0` 가드 추가.

### [3] `fread` 두 번째 호출이 버퍼를 덮어씀 — `app.c:96`

**위치**: `app.c`, line 96

**문제**: 파일 크기 초과 여부를 확인하는 두 번째 `fread`가 `buffer` 자체를 목적지로 사용한다. 파일이 실제로 크면 `buffer[0]`(SQL 첫 문자)를 읽은 바이트로 덮어쓴다. 직후 에러를 반환하므로 덮어쓴 버퍼는 사용되지 않지만, 코드 의도와 동작이 불일치한다.

**수정**: 1바이트 로컬 임시 변수 `probe`에 읽도록 변경.

### [4] 음수 리터럴 lookahead에 명시적 null 검사 없음 — `tokenizer.c:277`

**위치**: `tokenizer.c`, line 277

**문제**: `sql_text[index] == '-'`인 경우 `sql_text[index + 1]`을 읽는다. `'-'`가 문자열 맨 끝(`'\0'` 바로 앞)에 있으면 `index + 1`은 `'\0'`을 가리킨다. `isdigit('\0')` 자체는 0을 반환해 분기하지 않으므로 현재는 안전하지만, null termination에 암묵적으로 의존하는 구조다.

**수정**: `sql_text[index + 1] != '\0'` 조건을 명시적으로 추가.

---

## P3 — LOW

### [5] 스키마 파일에서 빈 컬럼 이름 미검증 — `schema.c`

**위치**: `schema.c`, line 121 부근

**문제**: `:int` 처럼 콜론 앞이 비어 있어도 `lower_name`이 빈 문자열인 채로 스키마에 저장된다. 이후 `find_schema_column`에서 빈 이름을 가진 컬럼에 의도치 않게 매칭될 수 있다.

**수정**: `lower_name[0] == '\0'` 검사 후 에러 반환.

### [6] CSV 파서 인용부호 lookahead 암묵적 의존 — `executor.c:239`

**위치**: `executor.c`, line 239

**문제**: 루프 조건은 `line[i] != '\0'`이지만, `line[i] == '"'`일 때 `line[i + 1]`을 즉시 참조한다. `'"'`가 null 직전 마지막 문자이면 `line[i + 1]`은 `'\0'`이다. `'\0' == '"'`은 false라 분기하지 않으므로 현재 안전하지만, `fgets` 기반이라는 가정에 암묵적으로 의존한다.

**수정**: `line[i + 1] != '\0'` 조건을 명시적으로 추가.

---

## 불필요한 코드 제거

| 파일 | 항목 | 이유 |
|------|------|------|
| `executor.c` | `#include <stdlib.h>` | malloc/free/exit 등 stdlib 함수 미사용 |
| `executor.c` | `#include <stddef.h>` | 불필요하게 추가됨. `size_t`는 `string.h`에서 이미 제공 |
| `app.c` | `#include <stdlib.h>` | 동일하게 stdlib 함수 미사용 |
| `parser.c` + `sqlproc.h` | `statement_type_name()` 함수 및 선언 | 프로젝트 전체에서 호출 없음 |
| `tokenizer.c` + `sqlproc.h` | `token_type_name()` 함수 및 선언 | 프로젝트 전체에서 호출 없음 |
| `sqlproc.h` | `SelectStatement.table_location` 필드 | 파서에서 저장만 하고 실행기에서 읽히지 않음 |
| `parser.c` | `&select_statement->table_location` 인자 | 위 필드 제거에 따라 `NULL`로 교체 |

---

## 설계 참고 (수정하지 않음)

- **컬럼 리스트 INSERT에서 누락 컬럼**: `INSERT INTO t (id) VALUES (1)` 처럼 일부 컬럼만 지정하면 나머지 컬럼은 빈 문자열로 CSV에 삽입된다. 스키마에 DEFAULT나 NOT NULL 개념이 없으므로 현재 구조상 허용된 동작이지만, 의도적인 설계 결정임을 인지해야 한다.
- **고정 경로 버퍼 (256/512바이트)**: 매우 긴 경로에서 `snprintf`에 의해 조용히 잘릴 수 있다. 현재 프로젝트 범위에서는 문제 없으나, 이식성을 높이려면 동적 할당을 고려할 수 있다.
