#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "sqlproc.h"

/*
 * executor.c는 파서가 만든 SQL 문장 구조체를 실제 파일 입출력 동작으로 수행하는 모듈입니다.
 * - INSERT: CSV 파일에 행을 추가
 * - SELECT: CSV를 읽어 결과 출력
 */

#define EXECUTOR_MAX_PATH_LEN 512
#define EXECUTOR_MAX_ROW_LEN 1024

static int parse_csv_line(const char *line,
                          char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                          int *value_count);

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
    /* INSERT에서 리터럴 타입이 컬럼 타입과 맞는지 확인합니다. */
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

        {
            size_t line_len = strlen(line);
            if (line_len == sizeof(line) - 1 && line[line_len - 1] != '\n' && !feof(file)) {
                fclose(file);
                set_file_error(error, "기존 데이터 파일 헤더 행이 너무 깁니다.");
                return 0;
            }
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
            if (in_quotes && line[i + 1] != '\0' && line[i + 1] == '"') {
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

static int build_insert_row_values(const TableSchema *schema,
                                   const InsertStatement *statement,
                                   char row_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                                   SourceLocation value_locations[SQLPROC_MAX_COLUMNS],
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

static int execute_insert(const AppConfig *config,
                          const InsertStatement *statement,
                          ErrorInfo *error)
{
    TableSchema schema;
    char row_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
    SourceLocation value_locations[SQLPROC_MAX_COLUMNS];
    char path[EXECUTOR_MAX_PATH_LEN];
    FILE *file;

    /*
     * INSERT 실행 흐름:
     * 1. 스키마 로드
     * 2. 구조체 값을 스키마 순서 행 데이터로 정리
     * 3. CSV 헤더 준비
     * 4. CSV append
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
    file = fopen(path, "ab");
    if (file == NULL) {
        set_file_error(error, "데이터 파일을 열 수 없습니다.");
        return 0;
    }

    if (!write_csv_row(file, row_values, schema.column_count)) {
        fclose(file);
        set_file_error(error, "데이터 행을 파일에 쓸 수 없습니다.");
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
    FILE *file;

    /*
     * SELECT 실행 흐름:
     * 1. 스키마/선택 컬럼 유효성 확인
     * 2. CSV 헤더 검증
     * 3. 전체 행 순차 출력
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
        size_t line_len = strlen(line);
        if (line_len == sizeof(line) - 1 && line[line_len - 1] != '\n' && !feof(file)) {
            fclose(file);
            set_file_error(error, "CSV 헤더 행이 너무 깁니다.");
            return 0;
        }
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

    while (fgets(line, sizeof(line), file) != NULL) {
        int value_count;
        int i;

        {
            size_t line_len = strlen(line);
            if (line_len == sizeof(line) - 1 && line[line_len - 1] != '\n' && !feof(file)) {
                fclose(file);
                set_file_error(error, "CSV 행이 너무 깁니다.");
                return 0;
            }
        }

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
