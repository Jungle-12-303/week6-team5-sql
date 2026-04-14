#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlproc.h"

/*
 * executor.c는 SQL 문장 구조체를 실행 단계로 연결하는 모듈입니다.
 * - INSERT: 스키마/값 유효성을 확인한 뒤 스토리지에 행 추가를 요청합니다.
 * - SELECT: 선택 컬럼을 계산한 뒤 스토리지에 조회 출력을 요청합니다.
 */

static void set_runtime_error(ErrorInfo *error,
                              const char *message,
                              SourceLocation location)
{
    /* SQL 문장 자체와 관련된 오류는 line/column 위치를 함께 저장합니다. */
    snprintf(error->message, sizeof(error->message), "%s", message);
    error->line = location.line;
    error->column = location.column;
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
    /* INSERT에서 리터럴 타입이 컬럼 타입과 맞는지 확인합니다. */
    if (data_type == DATA_TYPE_INT && value->type == LITERAL_INT) {
        return 1;
    }

    if (data_type == DATA_TYPE_STRING && value->type == LITERAL_STRING) {
        return 1;
    }

    return 0;
}

static int validate_int_literal_range(const LiteralValue *value)
{
    char *end_pointer;
    long parsed_value;

    /*
     * 토크나이저는 "숫자처럼 보이는 문자열"까지만 보장하므로
     * 실제 int 범위 검사는 실행 직전에 한 번 더 확인합니다.
     */
    errno = 0;
    parsed_value = strtol(value->text, &end_pointer, 10);
    if (errno == ERANGE || *end_pointer != '\0') {
        return 0;
    }

    return parsed_value >= INT_MIN && parsed_value <= INT_MAX;
}

static int validate_typed_literal(DataType data_type, const LiteralValue *value)
{
    if (!validate_literal_type(data_type, value)) {
        return 0;
    }

    if (data_type == DATA_TYPE_INT) {
        return validate_int_literal_range(value);
    }

    return 1;
}

static int build_insert_row_values(const TableSchema *schema,
                                   const InsertStatement *statement,
                                   char row_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                                   ErrorInfo *error)
{
    int used_columns[SQLPROC_MAX_COLUMNS];
    int i;

    /*
     * INSERT 구조체를 실제 "스키마 순서의 한 행 데이터"로 정렬합니다.
     * - 컬럼 목록 생략 시: 스키마 순서 그대로 값 매핑
     * - 컬럼 목록 명시 시: 이름을 찾아 해당 스키마 위치로 값 배치
     */
    memset(used_columns, 0, sizeof(used_columns));
    memset(row_values, 0, sizeof(char[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN]));

    if (!statement->has_column_list) {
        if (statement->value_count != schema->column_count) {
            set_runtime_error(error,
                              "VALUES 값 수가 스키마 컬럼 수와 일치하지 않습니다.",
                              statement->table_location);
            return 0;
        }

        for (i = 0; i < schema->column_count; i++) {
            if (!validate_typed_literal(schema->columns[i].type, &statement->values[i])) {
                set_runtime_error(error,
                                  "INSERT 값 타입이 스키마와 맞지 않습니다.",
                                  statement->values[i].location);
                return 0;
            }

            snprintf(row_values[i], sizeof(row_values[i]), "%s", statement->values[i].text);
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

        if (!validate_typed_literal(schema->columns[schema_index].type,
                                    &statement->values[i])) {
            set_runtime_error(error,
                              "INSERT 값 타입이 스키마와 맞지 않습니다.",
                              statement->values[i].location);
            return 0;
        }

        snprintf(row_values[schema_index], sizeof(row_values[schema_index]), "%s",
                 statement->values[i].text);
        used_columns[schema_index] = 1;
    }

    return 1;
}

static int execute_insert(const AppConfig *config,
                          const InsertStatement *statement,
                          ErrorInfo *error)
{
    TableSchema schema;
    char row_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];

    /*
     * INSERT 실행 흐름:
     * 1. 스키마 로드
     * 2. 구조체 값을 스키마 순서 행 데이터로 정리
     * 3. 스토리지에 append 요청
     */
    if (!load_table_schema(config->schema_dir, statement->table_name, &schema, error)) {
        return 0;
    }

    if (!build_insert_row_values(&schema, statement, row_values, error)) {
        return 0;
    }

    return storage_append_row(config, &schema, row_values, error);
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

static int validate_where_clause(const TableSchema *schema,
                                 const WhereClause *where_clause,
                                 ErrorInfo *error)
{
    int i;

    /*
     * full scan 전에 WHERE 컬럼 존재 여부와 리터럴 타입을 미리 확인합니다.
     * 실제 행 단위 비교는 storage.c가 맡습니다.
     */
    for (i = 0; i < where_clause->count; i++) {
        int column_index;

        column_index = find_schema_column(schema, where_clause->items[i].column_name);
        if (column_index < 0) {
            set_runtime_error(error,
                              "WHERE 절의 컬럼이 스키마에 없습니다.",
                              where_clause->items[i].column_location);
            return 0;
        }

        if (!validate_typed_literal(schema->columns[column_index].type,
                                    &where_clause->items[i].value)) {
            set_runtime_error(error,
                              "WHERE 절 리터럴 타입이 스키마와 맞지 않습니다.",
                              where_clause->items[i].value.location);
            return 0;
        }
    }

    return 1;
}

static int execute_select(const AppConfig *config,
                          const SelectStatement *statement,
                          ErrorInfo *error)
{
    TableSchema schema;
    int selected_indices[SQLPROC_MAX_COLUMNS];
    int selected_count;

    /*
     * SELECT 실행 흐름:
     * 1. 스키마/선택 컬럼/WHERE 유효성 확인
     * 2. 스토리지에 full scan 조회 출력 요청
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

    return storage_print_rows(config,
                              &schema,
                              selected_indices,
                              selected_count,
                              &statement->where_clause,
                              error);
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

        if (!execute_select(config, &program->items[i].select_statement, error)) {
            return 0;
        }
    }

    return 1;
}
