#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlproc.h"

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
    snprintf(error->message, sizeof(error->message), "%s", message);
    error->line = location.line;
    error->column = location.column;
}

static void set_file_error(ErrorInfo *error, const char *message)
{
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
    snprintf(dest, dest_size, "%s/%s%s", base_dir, table_name, extension);
}

static int find_schema_column(const TableSchema *schema, const char *name)
{
    int i;

    for (i = 0; i < schema->column_count; i++) {
        if (strcmp(schema->columns[i].name, name) == 0) {
            return i;
        }
    }

    return -1;
}

static int validate_literal_type(DataType data_type, const LiteralValue *value)
{
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

    needs_quote = 0;
    for (cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor == ',' || *cursor == '"' || *cursor == '\n') {
            needs_quote = 1;
            break;
        }
    }

    if (!needs_quote) {
        fputs(text, file);
        return 1;
    }

    fputc('"', file);

    for (cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor == '"') {
            fputc('"', file);
        }

        fputc(*cursor, file);
    }

    fputc('"', file);
    return 1;
}

static int write_csv_row(FILE *file,
                         char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                         int value_count)
{
    int i;

    for (i = 0; i < value_count; i++) {
        if (i > 0) {
            fputc(',', file);
        }

        write_csv_field(file, values[i]);
    }

    fputc('\n', file);
    return 1;
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
            fputc(',', file);
        }

        fputs(schema->columns[i].name, file);
    }

    fputc('\n', file);
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

static int execute_insert(const AppConfig *config,
                          const InsertStatement *statement,
                          ErrorInfo *error)
{
    TableSchema schema;
    char row_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
    char path[EXECUTOR_MAX_PATH_LEN];
    int used_columns[SQLPROC_MAX_COLUMNS];
    FILE *file;
    long row_offset;
    int i;

    if (!load_table_schema(config->schema_dir, statement->table_name, &schema, error)) {
        return 0;
    }

    memset(row_values, 0, sizeof(row_values));
    memset(used_columns, 0, sizeof(used_columns));

    for (i = 0; i < statement->column_count; i++) {
        int schema_index;

        schema_index = find_schema_column(&schema, statement->column_names[i]);
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

        if (!validate_literal_type(schema.columns[schema_index].type,
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

    if (!ensure_data_file(config, &schema, error)) {
        return 0;
    }

    build_table_path(path, sizeof(path), config->data_dir, schema.table_name, ".csv");
    file = fopen(path, "ab+");
    if (file == NULL) {
        set_file_error(error, "데이터 파일을 열 수 없습니다.");
        return 0;
    }

    fseek(file, 0, SEEK_END);
    row_offset = ftell(file);
    write_csv_row(file, row_values, schema.column_count);
    fclose(file);

    return update_all_indexes_for_row(config, &schema, row_values, row_offset, error);
}

static int resolve_selected_columns(const TableSchema *schema,
                                    const SelectStatement *statement,
                                    int selected_indices[SQLPROC_MAX_COLUMNS],
                                    int *selected_count,
                                    ErrorInfo *error)
{
    int i;

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
    return create_index_from_statement(config, statement, error);
}

int execute_program(const AppConfig *config, const SqlProgram *program, ErrorInfo *error)
{
    int i;

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
