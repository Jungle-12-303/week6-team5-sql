#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlproc.h"

/*
 * storage.c는 CSV 파일 저장과 조회를 담당하는 얇은 스토리지 계층입니다.
 * 실행기는 "무슨 데이터를 읽고 쓸지"만 결정하고,
 * 실제 파일 경로/헤더/행 입출력은 이 모듈에 맡깁니다.
 */

#define STORAGE_MAX_PATH_LEN 512
#define STORAGE_MAX_ROW_LEN 1024

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

static int validate_line_length(const char *line,
                                size_t line_size,
                                FILE *file,
                                ErrorInfo *error,
                                const char *message)
{
    size_t line_len;

    /*
     * fgets가 버퍼 끝까지 읽었는데 개행을 못 만났다면
     * 아직 다음 조각이 남아 있다는 뜻이므로 "한 행이 너무 길다"고 봅니다.
     */
    line_len = strlen(line);
    if (line_len == line_size - 1 && line[line_len - 1] != '\n' && !feof(file)) {
        set_file_error(error, message);
        return 0;
    }

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

static int validate_header_values(const TableSchema *schema,
                                  char header_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                                  int header_count,
                                  ErrorInfo *error,
                                  const char *count_message,
                                  const char *order_message)
{
    int i;

    if (header_count != schema->column_count) {
        set_file_error(error, count_message);
        return 0;
    }

    for (i = 0; i < schema->column_count; i++) {
        if (strcmp(header_values[i], schema->columns[i].name) != 0) {
            set_file_error(error, order_message);
            return 0;
        }
    }

    return 1;
}

static int ensure_data_file(const AppConfig *config,
                            const TableSchema *schema,
                            ErrorInfo *error)
{
    char path[STORAGE_MAX_PATH_LEN];
    char line[STORAGE_MAX_ROW_LEN];
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

        if (!validate_line_length(line,
                                  sizeof(line),
                                  file,
                                  error,
                                  "기존 데이터 파일 헤더 행이 너무 깁니다.")) {
            fclose(file);
            return 0;
        }

        if (!parse_csv_line(line, header_values, &header_count)) {
            fclose(file);
            set_file_error(error, "기존 데이터 파일 헤더 형식이 잘못되었습니다.");
            return 0;
        }

        if (!validate_header_values(schema,
                                    header_values,
                                    header_count,
                                    error,
                                    "기존 데이터 파일 헤더가 스키마와 다릅니다.",
                                    "기존 데이터 파일 헤더 순서가 스키마와 다릅니다.")) {
            fclose(file);
            return 0;
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

static void print_selected_header(const TableSchema *schema,
                                  const int selected_indices[SQLPROC_MAX_COLUMNS],
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

static int parse_int_text(const char *text, long *value)
{
    char *end_pointer;

    if (text[0] == '\0') {
        return 0;
    }

    errno = 0;
    *value = strtol(text, &end_pointer, 10);
    if (errno == ERANGE || *end_pointer != '\0') {
        return 0;
    }

    return *value >= INT_MIN && *value <= INT_MAX;
}

static int compare_values(DataType data_type,
                          const char *row_value,
                          CompareOperator operator_type,
                          const LiteralValue *literal)
{
    int compare_result;

    /* int는 숫자 비교, string은 문자열 비교로 WHERE 판정을 통일합니다. */
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
            set_file_error(error, "WHERE 절 컬럼을 스키마에서 찾을 수 없습니다.");
            return -1;
        }

        if (schema->columns[column_index].type == DATA_TYPE_INT) {
            if (values[column_index][0] == '\0') {
                return 0;
            }

            if (!parse_int_text(values[column_index], &unused_value)) {
                /*
                 * 스키마가 int인 컬럼은 비교 전에 형식과 범위를 모두 검증해
                 * 손상된 CSV가 있더라도 조용히 잘못된 비교를 하지 않게 합니다.
                 */
                snprintf(error->message,
                         sizeof(error->message),
                         "%s",
                         "정수 컬럼에 유효한 int 값이 아닌 데이터가 저장되어 있습니다.");
                error->line = where_clause->items[i].column_location.line;
                error->column = where_clause->items[i].column_location.column;
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

int storage_append_row(const AppConfig *config,
                       const TableSchema *schema,
                       char row_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                       ErrorInfo *error)
{
    char path[STORAGE_MAX_PATH_LEN];
    FILE *file;

    /* INSERT용 한 행을 파일에 추가하기 전에 헤더 상태를 먼저 맞춥니다. */
    if (!ensure_data_file(config, schema, error)) {
        return 0;
    }

    build_table_path(path, sizeof(path), config->data_dir, schema->table_name, ".csv");
    file = fopen(path, "ab");
    if (file == NULL) {
        set_file_error(error, "데이터 파일을 열 수 없습니다.");
        return 0;
    }

    if (!write_csv_row(file, row_values, schema->column_count)) {
        fclose(file);
        set_file_error(error, "데이터 행을 파일에 쓸 수 없습니다.");
        return 0;
    }

    fclose(file);
    return 1;
}

int storage_print_rows(const AppConfig *config,
                       const TableSchema *schema,
                       const int selected_indices[SQLPROC_MAX_COLUMNS],
                       int selected_count,
                       const WhereClause *where_clause,
                       ErrorInfo *error)
{
    char path[STORAGE_MAX_PATH_LEN];
    char line[STORAGE_MAX_ROW_LEN];
    char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
    FILE *file;

    /*
     * SELECT는 파일이 없으면 헤더만 출력하고,
     * 파일이 있으면 헤더를 검증한 뒤 전체 행을 순차 출력합니다.
     */
    build_table_path(path, sizeof(path), config->data_dir, schema->table_name, ".csv");
    file = fopen(path, "rb");
    if (file == NULL) {
        if (errno == ENOENT) {
            print_selected_header(schema, selected_indices, selected_count);
            return 1;
        }

        set_file_error(error, "데이터 파일을 열 수 없습니다.");
        return 0;
    }

    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        /*
         * 파일이 존재하는데 첫 줄을 읽지 못했다면 빈 CSV 또는 손상된 파일입니다.
         * 헤더 없이 성공 처리하면 SELECT가 조용히 데이터를 놓치게 됩니다.
         */
        set_file_error(error, "CSV 헤더를 읽을 수 없습니다.");
        return 0;
    }

    if (!validate_line_length(line,
                              sizeof(line),
                              file,
                              error,
                              "CSV 헤더 행이 너무 깁니다.")) {
        fclose(file);
        return 0;
    }

    {
        char header_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
        int header_count;

        if (!parse_csv_line(line, header_values, &header_count)) {
            fclose(file);
            set_file_error(error, "CSV 헤더 형식이 잘못되었습니다.");
            return 0;
        }

        if (!validate_header_values(schema,
                                    header_values,
                                    header_count,
                                    error,
                                    "CSV 헤더가 스키마와 다릅니다.",
                                    "CSV 헤더 순서가 스키마와 다릅니다.")) {
            fclose(file);
            return 0;
        }
    }

    print_selected_header(schema, selected_indices, selected_count);

    while (fgets(line, sizeof(line), file) != NULL) {
        int value_count;
        int i;
        int match_result;

        if (!validate_line_length(line, sizeof(line), file, error, "CSV 행이 너무 깁니다.")) {
            fclose(file);
            return 0;
        }

        memset(values, 0, sizeof(values));

        if (!parse_csv_line(line, values, &value_count)) {
            fclose(file);
            set_file_error(error, "CSV 행을 읽는 중 오류가 발생했습니다.");
            return 0;
        }

        if (value_count != schema->column_count) {
            fclose(file);
            set_file_error(error, "CSV 컬럼 수가 스키마와 맞지 않습니다.");
            return 0;
        }

        match_result = row_matches_where(schema, values, where_clause, error);
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
