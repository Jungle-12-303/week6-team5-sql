#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sqlproc.h"

/*
 * executor.c는 파서가 만든 AST를 실제 파일 입출력 동작으로 수행하는 모듈입니다.
 * - INSERT: CSV 파일에 행을 추가하고 필요하면 인덱스도 갱신
 * - SELECT: CSV를 읽어 WHERE를 검사하고 결과 출력
 * - CREATE INDEX: 인덱스 모듈에 생성 작업 위임
 */

#define EXECUTOR_MAX_PATH_LEN 512
#define EXECUTOR_MAX_ROW_LEN 1024

static int parse_csv_line(const char *line,
                          char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                          int *value_count);
static int parse_int_text(const char *text, long *value);

static void set_runtime_error(ErrorInfo *error,
                              const char *message,
                              SourceLocation location)
{
    /* SQL 문장 자체와 관련된 오류는 line/column 위치를 함께 저장합니다. */
    snprintf(error->message, sizeof(error->message), "%s", message);
    error->line = location.line;
    error->column = location.column;
}

static void set_file_error(ErrorInfo *error, const char *message)
{
    /* 파일 시스템/CSV 형식 오류는 SQL 위치 없이 메시지만 기록합니다. */
    snprintf(error->message, sizeof(error->message), "%s", message);
    error->line = 0;
    error->column = 0;
}

static void build_table_path(char *dest,
                             size_t dest_size,
                             const char *base_dir,
                             const char *table_name,
                             const char *extension)
{
    /* data_dir/users.csv 같은 실제 파일 경로를 조립합니다. */
    snprintf(dest, dest_size, "%s/%s%s", base_dir, table_name, extension);
}

static int find_schema_column(const TableSchema *schema, const char *name)
{
    int i;

    /* 컬럼 이름을 스키마 순서 인덱스로 바꿉니다. 찾지 못하면 -1입니다. */
    for (i = 0; i < schema->column_count; i++) {
        if (strcmp(schema->columns[i].name, name) == 0) {
            return i;
        }
    }

    return -1;
}

static int validate_literal_type(DataType data_type, const LiteralValue *value)
{
    /* INSERT/WHERE에서 리터럴 타입이 컬럼 타입과 맞는지 확인합니다. */
    if (data_type == DATA_TYPE_INT && value->type == LITERAL_INT) {
        return 1;
    }

    if (data_type == DATA_TYPE_STRING && value->type == LITERAL_STRING) {
        return 1;
    }

    return 0;
}

static int write_csv_field(FILE *file, const char *text)
{
    int needs_quote;
    const char *cursor;

    /*
     * CSV 필드 1개를 안전하게 저장합니다.
     * 쉼표/따옴표/개행이 있으면 큰따옴표로 감싸고 내부 따옴표는 이스케이프합니다.
     */
    needs_quote = 0;
    for (cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor == ',' || *cursor == '"' || *cursor == '\n') {
            needs_quote = 1;
            break;
        }
    }

    if (!needs_quote) {
        return fputs(text, file) != EOF;
    }

    if (fputc('"', file) == EOF) {
        return 0;
    }

    for (cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor == '"') {
            if (fputc('"', file) == EOF) {
                return 0;
            }
        }

        if (fputc(*cursor, file) == EOF) {
            return 0;
        }
    }

    return fputc('"', file) != EOF;
}

static int write_csv_row(FILE *file,
                         char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                         int value_count)
{
    int i;

    /* 컬럼 배열 1개를 CSV 한 줄로 저장합니다. */
    for (i = 0; i < value_count; i++) {
        if (i > 0) {
            if (fputc(',', file) == EOF) {
                return 0;
            }
        }

        if (!write_csv_field(file, values[i])) {
            return 0;
        }
    }

    return fputc('\n', file) != EOF;
}

static int ensure_data_file(const AppConfig *config,
                            const TableSchema *schema,
                            ErrorInfo *error)
{
    char path[EXECUTOR_MAX_PATH_LEN];
    char line[EXECUTOR_MAX_ROW_LEN];
    char header_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
    int header_count;
    FILE *file;
    int i;

    /*
     * 데이터 파일이 이미 있으면 헤더가 현재 스키마와 일치하는지 검증하고,
     * 없으면 새 CSV 파일을 만들고 헤더를 기록합니다.
     */
    build_table_path(path, sizeof(path), config->data_dir, schema->table_name, ".csv");
    file = fopen(path, "rb");
    if (file != NULL) {
        if (fgets(line, sizeof(line), file) == NULL) {
            fclose(file);
            set_file_error(error, "기존 데이터 파일 헤더를 읽을 수 없습니다.");
            return 0;
        }

        if (!parse_csv_line(line, header_values, &header_count)) {
            fclose(file);
            set_file_error(error, "기존 데이터 파일 헤더 형식이 잘못되었습니다.");
            return 0;
        }

        if (header_count != schema->column_count) {
            fclose(file);
            set_file_error(error, "기존 데이터 파일 헤더가 스키마와 다릅니다.");
            return 0;
        }

        for (i = 0; i < schema->column_count; i++) {
            if (strcmp(header_values[i], schema->columns[i].name) != 0) {
                fclose(file);
                set_file_error(error, "기존 데이터 파일 헤더 순서가 스키마와 다릅니다.");
                return 0;
            }
        }

        fclose(file);
        return 1;
    }

    file = fopen(path, "wb");
    if (file == NULL) {
        set_file_error(error, "데이터 파일을 만들 수 없습니다.");
        return 0;
    }

    for (i = 0; i < schema->column_count; i++) {
        if (i > 0) {
            if (fputc(',', file) == EOF) {
                fclose(file);
                set_file_error(error, "데이터 파일 헤더를 쓸 수 없습니다.");
                return 0;
            }
        }

        if (fputs(schema->columns[i].name, file) == EOF) {
            fclose(file);
            set_file_error(error, "데이터 파일 헤더를 쓸 수 없습니다.");
            return 0;
        }
    }

    if (fputc('\n', file) == EOF) {
        fclose(file);
        set_file_error(error, "데이터 파일 헤더를 쓸 수 없습니다.");
        return 0;
    }

    fclose(file);
    return 1;
}

static int parse_csv_line(const char *line,
                          char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                          int *value_count)
{
    int in_quotes;
    int row_index;
    int text_index;
    int i;

    /* CSV 한 줄을 컬럼 문자열 배열로 분해합니다. 큰따옴표 이스케이프도 처리합니다. */
    in_quotes = 0;
    row_index = 0;
    text_index = 0;

    for (i = 0; line[i] != '\0' && line[i] != '\n' && line[i] != '\r'; i++) {
        if (row_index >= SQLPROC_MAX_COLUMNS) {
            return 0;
        }

        if (line[i] == '"') {
            if (in_quotes && line[i + 1] == '"') {
                if (text_index >= SQLPROC_MAX_VALUE_LEN - 1) {
                    return 0;
                }

                values[row_index][text_index] = '"';
                text_index += 1;
                i += 1;
                continue;
            }

            in_quotes = !in_quotes;
            continue;
        }

        if (!in_quotes && line[i] == ',') {
            values[row_index][text_index] = '\0';
            row_index += 1;
            text_index = 0;
            continue;
        }

        if (text_index >= SQLPROC_MAX_VALUE_LEN - 1) {
            return 0;
        }

        values[row_index][text_index] = line[i];
        text_index += 1;
    }

    if (row_index >= SQLPROC_MAX_COLUMNS) {
        return 0;
    }

    values[row_index][text_index] = '\0';
    *value_count = row_index + 1;
    return 1;
}

static int parse_int_text(const char *text, long *value)
{
    char *end_pointer;

    /* CSV 문자열이 정수로 완전히 해석되는지 검사하면서 long으로 변환합니다. */
    if (text[0] == '\0') {
        return 0;
    }

    *value = strtol(text, &end_pointer, 10);
    return *end_pointer == '\0';
}

/*
 * 무엇을 하는가:
 * - CSV에서 읽은 문자열 값과 WHERE 절의 리터럴을 스키마 타입에 맞춰
 *   비교합니다.
 *
 * 왜 필요한가:
 * - 같은 비교 연산자라도 int와 string은 해석 방식이 달라서, 실행기가
 *   타입별 규칙을 한곳에서 일관되게 적용해야 하기 때문입니다.
 *
 * 입력과 출력:
 * - 입력: 스키마 타입, CSV 문자열 값, 비교 연산자, 비교 대상 리터럴
 * - 출력: 조건을 만족하면 1, 만족하지 않으면 0
 *
 * 핵심 흐름:
 * - int는 정수로 바꿔 숫자 비교를 하고, string은 strcmp 결과로 비교합니다.
 */
static int compare_values(DataType data_type,
                          const char *row_value,
                          CompareOperator operator_type,
                          const LiteralValue *literal)
{
    int compare_result;

    compare_result = 0;

    if (data_type == DATA_TYPE_INT) {
        long left_value;
        long right_value;

        parse_int_text(row_value, &left_value);
        parse_int_text(literal->text, &right_value);

        if (left_value < right_value) {
            compare_result = -1;
        } else if (left_value > right_value) {
            compare_result = 1;
        }
    } else {
        compare_result = strcmp(row_value, literal->text);
    }

    if (operator_type == COMPARE_EQUAL) {
        return compare_result == 0;
    }

    if (operator_type == COMPARE_LESS) {
        return compare_result < 0;
    }

    if (operator_type == COMPARE_LESS_EQUAL) {
        return compare_result <= 0;
    }

    if (operator_type == COMPARE_GREATER) {
        return compare_result > 0;
    }

    return compare_result >= 0;
}

static int row_matches_where(const TableSchema *schema,
                             char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                             const WhereClause *where_clause,
                             ErrorInfo *error)
{
    int i;

    /* CSV 한 행이 WHERE 조건들을 모두 만족하는지 검사합니다. */
    for (i = 0; i < where_clause->count; i++) {
        int column_index;
        long unused_value;

        column_index = find_schema_column(schema, where_clause->items[i].column_name);
        if (column_index < 0) {
            set_runtime_error(error,
                              "WHERE 절의 컬럼이 스키마에 없습니다.",
                              where_clause->items[i].column_location);
            return -1;
        }

        if (!validate_literal_type(schema->columns[column_index].type,
                                   &where_clause->items[i].value)) {
            set_runtime_error(error,
                              "WHERE 절 리터럴 타입이 스키마와 맞지 않습니다.",
                              where_clause->items[i].value.location);
            return -1;
        }

        if (schema->columns[column_index].type == DATA_TYPE_INT) {
            if (values[column_index][0] == '\0') {
                return 0;
            }

            if (!parse_int_text(values[column_index], &unused_value)) {
                set_runtime_error(error,
                                  "정수 컬럼에 숫자가 아닌 값이 저장되어 있습니다.",
                                  where_clause->items[i].column_location);
                return -1;
            }
        }

        if (!compare_values(schema->columns[column_index].type,
                            values[column_index],
                            where_clause->items[i].operator_type,
                            &where_clause->items[i].value)) {
            return 0;
        }
    }

    return 1;
}

static int validate_where_clause(const TableSchema *schema,
                                 const WhereClause *where_clause,
                                 ErrorInfo *error)
{
    int i;

    /* SELECT를 실행하기 전에 WHERE 절 자체가 스키마와 맞는지 미리 검증합니다. */
    for (i = 0; i < where_clause->count; i++) {
        int column_index;

        column_index = find_schema_column(schema, where_clause->items[i].column_name);
        if (column_index < 0) {
            set_runtime_error(error,
                              "WHERE 절의 컬럼이 스키마에 없습니다.",
                              where_clause->items[i].column_location);
            return 0;
        }

        if (!validate_literal_type(schema->columns[column_index].type,
                                   &where_clause->items[i].value)) {
            set_runtime_error(error,
                              "WHERE 절 리터럴 타입이 스키마와 맞지 않습니다.",
                              where_clause->items[i].value.location);
            return 0;
        }
    }

    return 1;
}

static int build_insert_row_values(const TableSchema *schema,
                                   const InsertStatement *statement,
                                   char row_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                                   SourceLocation value_locations[SQLPROC_MAX_COLUMNS],
                                   ErrorInfo *error)
{
    int used_columns[SQLPROC_MAX_COLUMNS];
    int i;

    /*
     * INSERT AST를 실제 "스키마 순서의 한 행 데이터"로 정렬합니다.
     * - 컬럼 목록 생략 시: 스키마 순서 그대로 값 매핑
     * - 컬럼 목록 명시 시: 이름을 찾아 해당 스키마 위치로 값 배치
     */
    memset(used_columns, 0, sizeof(used_columns));
    memset(row_values, 0, sizeof(char[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN]));
    memset(value_locations, 0, sizeof(SourceLocation) * SQLPROC_MAX_COLUMNS);

    if (!statement->has_column_list) {
        if (statement->value_count != schema->column_count) {
            set_runtime_error(error,
                              "VALUES 값 수가 스키마 컬럼 수와 일치하지 않습니다.",
                              statement->table_location);
            return 0;
        }

        for (i = 0; i < schema->column_count; i++) {
            if (!validate_literal_type(schema->columns[i].type, &statement->values[i])) {
                set_runtime_error(error,
                                  "INSERT 값 타입이 스키마와 맞지 않습니다.",
                                  statement->values[i].location);
                return 0;
            }

            snprintf(row_values[i], sizeof(row_values[i]), "%s", statement->values[i].text);
            value_locations[i] = statement->values[i].location;
        }

        return 1;
    }

    for (i = 0; i < statement->column_count; i++) {
        int schema_index;

        schema_index = find_schema_column(schema, statement->column_names[i]);
        if (schema_index < 0) {
            set_runtime_error(error,
                              "INSERT 대상 컬럼이 스키마에 없습니다.",
                              statement->column_locations[i]);
            return 0;
        }

        if (used_columns[schema_index]) {
            set_runtime_error(error,
                              "같은 컬럼이 INSERT 문에 두 번 들어왔습니다.",
                              statement->column_locations[i]);
            return 0;
        }

        if (!validate_literal_type(schema->columns[schema_index].type,
                                   &statement->values[i])) {
            set_runtime_error(error,
                              "INSERT 값 타입이 스키마와 맞지 않습니다.",
                              statement->values[i].location);
            return 0;
        }

        snprintf(row_values[schema_index], sizeof(row_values[schema_index]), "%s",
                 statement->values[i].text);
        value_locations[schema_index] = statement->values[i].location;
        used_columns[schema_index] = 1;
    }

    return 1;
}

static int validate_primary_key_insert(FILE *file,
                                       const TableSchema *schema,
                                       char row_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                                       SourceLocation value_locations[SQLPROC_MAX_COLUMNS],
                                       ErrorInfo *error)
{
    char line[EXECUTOR_MAX_ROW_LEN];
    char existing_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
    int value_count;
    int pk_index;

    /*
     * PRIMARY KEY가 있는 테이블이면 기존 CSV 전체를 읽어
     * 같은 PK 값이 이미 있는지 확인합니다.
     */
    pk_index = schema->primary_key_column_index;
    if (pk_index < 0) {
        return 1;
    }

    if (row_values[pk_index][0] == '\0') {
        set_runtime_error(error,
                          "PRIMARY KEY 컬럼 값이 필요합니다.",
                          value_locations[pk_index]);
        return 0;
    }

    rewind(file);
    if (fgets(line, sizeof(line), file) == NULL) {
        set_file_error(error, "데이터 파일 헤더를 읽을 수 없습니다.");
        return 0;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        if (!parse_csv_line(line, existing_values, &value_count)) {
            set_file_error(error, "기존 데이터 행 형식이 잘못되었습니다.");
            return 0;
        }

        if (value_count != schema->column_count) {
            set_file_error(error, "기존 데이터 행 컬럼 수가 스키마와 다릅니다.");
            return 0;
        }

        if (strcmp(existing_values[pk_index], row_values[pk_index]) == 0) {
            set_runtime_error(error,
                              "PRIMARY KEY 값이 이미 존재합니다.",
                              value_locations[pk_index]);
            return 0;
        }
    }

    return 1;
}

/*
 * 무엇을 하는가:
 * - INSERT AST 1개를 받아 실제 CSV 파일 끝에 새 행을 추가합니다.
 *
 * 왜 필요한가:
 * - parser.c는 "무슨 SQL인지"까지만 이해하고 끝나므로,
 *   실제 저장은 executor.c가 맡아야 합니다.
 *
 * 초심자용 큰 그림:
 * - parser.c가 만든 설계도(InsertStatement)를 받아
 *   "진짜 파일에 저장 가능한 한 줄"로 바꾸는 단계입니다.
 *
 * 실제 예시:
 * - SQL: INSERT INTO users VALUES (1, 'kim', 20);
 * - 스키마: id:int:pk,name:string,age:int
 * - 최종 CSV 행: 1,kim,20\n
 *
 * 실패하면 어떻게 되는가:
 * - PK가 중복이면 아예 쓰지 않습니다.
 * - CSV 저장 뒤 인덱스 갱신이 실패하면 방금 쓴 CSV 행을 잘라내고,
 *   인덱스를 다시 만들어 정합성을 맞춥니다.
 */
static int execute_insert(const AppConfig *config,
                          const InsertStatement *statement,
                          ErrorInfo *error)
{
    TableSchema schema;
    char row_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
    SourceLocation value_locations[SQLPROC_MAX_COLUMNS];
    char path[EXECUTOR_MAX_PATH_LEN];
    FILE *file;
    long row_offset;
    int changed_index;
    ErrorInfo update_error;
    ErrorInfo rebuild_error;

    /*
     * INSERT 실행 흐름:
     * 1. 스키마 로드
     * 2. AST 값을 스키마 순서 행 데이터로 정리
     * 3. CSV 헤더 준비
     * 4. PK 중복 검사
     * 5. CSV append
     * 6. 관련 인덱스 갱신
     */
    if (!load_table_schema(config->schema_dir, statement->table_name, &schema, error)) {
        return 0;
    }

    if (!build_insert_row_values(&schema, statement, row_values, value_locations, error)) {
        return 0;
    }

    if (!ensure_data_file(config, &schema, error)) {
        return 0;
    }

    build_table_path(path, sizeof(path), config->data_dir, schema.table_name, ".csv");
    file = fopen(path, "rb+");
    if (file == NULL) {
        set_file_error(error, "데이터 파일을 열 수 없습니다.");
        return 0;
    }

    if (!validate_primary_key_insert(file, &schema, row_values, value_locations, error)) {
        fclose(file);
        return 0;
    }

    fseek(file, 0, SEEK_END);
    row_offset = ftell(file);
    if (!write_csv_row(file, row_values, schema.column_count)) {
        fclose(file);
        set_file_error(error, "데이터 행을 파일에 쓸 수 없습니다.");
        return 0;
    }

    if (!update_all_indexes_for_row(config,
                                    &schema,
                                    row_values,
                                    row_offset,
                                    &changed_index,
                                    error)) {
        update_error = *error;
        fflush(file);
        /*
         * 방금 append한 위치(row_offset)까지 파일을 되돌립니다.
         * 예:
         * - row_offset이 12였다면, 헤더 뒤에 막 추가한 새 행은 잘리고
         *   파일 길이는 다시 12바이트로 돌아갑니다.
         */
        ftruncate(fileno(file), row_offset);
        fclose(file);

        /*
         * 1. CSV에 막 추가한 행을 먼저 되돌립니다.
         * 2. 그다음 이미 손댄 인덱스가 있을 수 있으면 같은 테이블 인덱스를
         *    현재 CSV 내용 기준으로 다시 만들어 정합성을 맞춥니다.
         */
        if (changed_index) {
            memset(&rebuild_error, 0, sizeof(rebuild_error));
            if (!rebuild_indexes_for_table(config, &schema, &rebuild_error)) {
                *error = rebuild_error;
                return 0;
            }
        }

        *error = update_error;
        return 0;
    }

    fclose(file);
    return 1;
}

static int resolve_selected_columns(const TableSchema *schema,
                                    const SelectStatement *statement,
                                    int selected_indices[SQLPROC_MAX_COLUMNS],
                                    int *selected_count,
                                    ErrorInfo *error)
{
    int i;

    /* SELECT * 또는 SELECT col1, col2 를 실제 스키마 인덱스 배열로 바꿉니다. */
    *selected_count = 0;

    if (statement->select_all) {
        for (i = 0; i < schema->column_count; i++) {
            selected_indices[*selected_count] = i;
            *selected_count += 1;
        }
    } else {
        for (i = 0; i < statement->column_count; i++) {
            int column_index;

            column_index = find_schema_column(schema, statement->column_names[i]);
            if (column_index < 0) {
                set_runtime_error(error,
                                  "SELECT 대상 컬럼이 스키마에 없습니다.",
                                  statement->column_locations[i]);
                *selected_count = 0;
                return 0;
            }

            selected_indices[*selected_count] = column_index;
            *selected_count += 1;
        }
    }

    return 1;
}

static void print_selected_header(const TableSchema *schema,
                                  int selected_indices[SQLPROC_MAX_COLUMNS],
                                  int selected_count)
{
    int i;

    /* 출력 결과 첫 줄에 선택된 컬럼 이름들을 탭 구분으로 출력합니다. */
    for (i = 0; i < selected_count; i++) {
        if (i > 0) {
            fputc('\t', stdout);
        }

        fputs(schema->columns[selected_indices[i]].name, stdout);
    }

    fputc('\n', stdout);
}

static int read_row_at_offset(FILE *file,
                              long row_offset,
                              char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                              int *value_count,
                              ErrorInfo *error)
{
    char line[EXECUTOR_MAX_ROW_LEN];

    /* 인덱스가 돌려준 row_offset 위치로 이동해 CSV 행 하나를 읽습니다. */
    if (fseek(file, row_offset, SEEK_SET) != 0) {
        set_file_error(error, "CSV 행 위치로 이동할 수 없습니다.");
        return 0;
    }

    if (fgets(line, sizeof(line), file) == NULL) {
        set_file_error(error, "CSV 행을 읽을 수 없습니다.");
        return 0;
    }

    if (!parse_csv_line(line, values, value_count)) {
        set_file_error(error, "CSV 행 형식이 잘못되었습니다.");
        return 0;
    }

    return 1;
}

/*
 * 무엇을 하는가:
 * - SELECT AST 1개를 받아 결과 표를 stdout으로 출력합니다.
 *
 * 왜 필요한가:
 * - SELECT는 단순히 CSV를 전부 읽는 것이 아니라,
 *   스키마 확인, WHERE 검증, 인덱스 사용 여부 판단, 출력 컬럼 선택까지
 *   여러 단계를 거쳐야 하기 때문입니다.
 *
 * 초심자용 큰 그림:
 * - "어떤 줄이 조건에 맞는지 찾고"
 * - "그 줄에서 어떤 칸만 보여줄지 정한 뒤"
 * - "화면에 표처럼 출력하는 함수"입니다.
 *
 * 두 가지 길:
 * - 인덱스가 있으면: 후보 행 위치(row_offset)만 먼저 모읍니다.
 * - 인덱스가 없으면: CSV를 처음부터 끝까지 다 읽습니다.
 */
static int execute_select(const AppConfig *config,
                          const SelectStatement *statement,
                          ErrorInfo *error)
{
    TableSchema schema;
    char path[EXECUTOR_MAX_PATH_LEN];
    char line[EXECUTOR_MAX_ROW_LEN];
    char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
    int selected_indices[SQLPROC_MAX_COLUMNS];
    int selected_count;
    long candidate_offsets[SQLPROC_MAX_INDEX_RESULTS];
    int candidate_count;
    int used_index;
    FILE *file;

    /*
     * SELECT 실행 흐름:
     * 1. 스키마/선택 컬럼/WHERE 유효성 확인
     * 2. 인덱스 사용 가능하면 후보 row offset 수집
     * 3. CSV 헤더 검증
     * 4. 인덱스 경로 또는 full scan 경로로 실제 행 출력
     */
    if (!load_table_schema(config->schema_dir, statement->table_name, &schema, error)) {
        return 0;
    }

    if (!resolve_selected_columns(&schema,
                                  statement,
                                  selected_indices,
                                  &selected_count,
                                  error)) {
        return 0;
    }

    if (!validate_where_clause(&schema, &statement->where_clause, error)) {
        return 0;
    }

    if (!try_collect_offsets_from_indexes(config,
                                          &schema,
                                          statement,
                                          candidate_offsets,
                                          &candidate_count,
                                          &used_index,
                                          error)) {
        return 0;
    }

    build_table_path(path, sizeof(path), config->data_dir, schema.table_name, ".csv");
    file = fopen(path, "rb");
    if (file == NULL) {
        if (errno == ENOENT) {
            print_selected_header(&schema, selected_indices, selected_count);
            return 1;
        }

        set_file_error(error, "데이터 파일을 열 수 없습니다.");
        return 0;
    }

    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return 1;
    }

    {
        char header_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
        int header_count;
        int i;

        if (!parse_csv_line(line, header_values, &header_count)) {
            fclose(file);
            set_file_error(error, "CSV 헤더 형식이 잘못되었습니다.");
            return 0;
        }

        if (header_count != schema.column_count) {
            fclose(file);
            set_file_error(error, "CSV 헤더가 스키마와 다릅니다.");
            return 0;
        }

        for (i = 0; i < schema.column_count; i++) {
            if (strcmp(header_values[i], schema.columns[i].name) != 0) {
                fclose(file);
                set_file_error(error, "CSV 헤더 순서가 스키마와 다릅니다.");
                return 0;
            }
        }
    }

    print_selected_header(&schema, selected_indices, selected_count);

    if (used_index) {
        int offset_index;

        /*
         * 인덱스를 썼더라도 WHERE를 한 번 더 검사합니다.
         * 이유:
         * - 인덱스가 age 하나만 있어도
         *   WHERE age >= 20 AND id = 1 처럼 다른 조건이 함께 있을 수 있기
         *   때문입니다.
         */
        for (offset_index = 0; offset_index < candidate_count; offset_index++) {
            int value_count;
            int match_result;
            int i;

            memset(values, 0, sizeof(values));

            if (!read_row_at_offset(file,
                                    candidate_offsets[offset_index],
                                    values,
                                    &value_count,
                                    error)) {
                fclose(file);
                return 0;
            }

            if (value_count != schema.column_count) {
                fclose(file);
                set_file_error(error, "CSV 컬럼 수가 스키마와 맞지 않습니다.");
                return 0;
            }

            match_result = row_matches_where(&schema, values, &statement->where_clause, error);
            if (match_result < 0) {
                fclose(file);
                return 0;
            }

            if (!match_result) {
                continue;
            }

            for (i = 0; i < selected_count; i++) {
                if (i > 0) {
                    fputc('\t', stdout);
                }

                fputs(values[selected_indices[i]], stdout);
            }

            fputc('\n', stdout);
        }

        fclose(file);
        return 1;
    }

    /*
     * full scan 경로입니다.
     * 헤더 다음 줄부터 한 줄씩 읽으면서 조건을 검사합니다.
     * 초심자 관점에서는 "가장 단순한 SELECT의 원형"이라고 보면 됩니다.
     */
    while (fgets(line, sizeof(line), file) != NULL) {
        int value_count;
        int match_result;
        int i;

        memset(values, 0, sizeof(values));

        if (!parse_csv_line(line, values, &value_count)) {
            fclose(file);
            set_file_error(error, "CSV 행을 읽는 중 오류가 발생했습니다.");
            return 0;
        }

        if (value_count != schema.column_count) {
            fclose(file);
            set_file_error(error, "CSV 컬럼 수가 스키마와 맞지 않습니다.");
            return 0;
        }

        match_result = row_matches_where(&schema, values, &statement->where_clause, error);
        if (match_result < 0) {
            fclose(file);
            return 0;
        }

        if (!match_result) {
            continue;
        }

        for (i = 0; i < selected_count; i++) {
            if (i > 0) {
                fputc('\t', stdout);
            }

            fputs(values[selected_indices[i]], stdout);
        }

        fputc('\n', stdout);
    }

    fclose(file);
    return 1;
}

static int execute_create_index(const AppConfig *config,
                                const CreateIndexStatement *statement,
                                ErrorInfo *error)
{
    /* CREATE INDEX 실제 구현은 btree_index.c로 위임합니다. */
    return create_index_from_statement(config, statement, error);
}

int execute_program(const AppConfig *config, const SqlProgram *program, ErrorInfo *error)
{
    int i;

    /* SqlProgram에 담긴 문장들을 앞에서부터 순서대로 실행합니다. */
    memset(error, 0, sizeof(*error));

    for (i = 0; i < program->count; i++) {
        if (program->items[i].type == STATEMENT_INSERT) {
            if (!execute_insert(config, &program->items[i].insert_statement, error)) {
                return 0;
            }
            continue;
        }

        if (program->items[i].type == STATEMENT_SELECT) {
            if (!execute_select(config, &program->items[i].select_statement, error)) {
                return 0;
            }
            continue;
        }

        if (!execute_create_index(config, &program->items[i].create_index_statement, error)) {
            return 0;
        }
    }

    return 1;
}
