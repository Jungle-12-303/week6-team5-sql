#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sqlproc.h"

#define EXECUTOR_MAX_PATH_LEN 512
#define EXECUTOR_MAX_ROW_LEN 1024

static int parse_csv_line(const char *line,
                          char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                          int *value_count);
static int parse_int_text(const char *text, long *value);

/* SQL 소스 위치 정보를 포함한 오류 메시지를 ErrorInfo에 저장한다.
 *
 * @param error     오류 정보를 저장할 포인터
 * @param message   오류 메시지 문자열
 * @param location  오류 발생 소스 위치 (줄, 열)
 */
static void set_runtime_error(ErrorInfo *error,
                              const char *message,
                              SourceLocation location)
{
    snprintf(error->message, sizeof(error->message), "%s", message);
    error->line = location.line;
    error->column = location.column;
}

/* 파일/CSV 오류 메시지를 ErrorInfo에 저장한다. 위치 정보는 기록하지 않는다.
 *
 * @param error    오류 정보를 저장할 포인터
 * @param message  오류 메시지 문자열
 */
static void set_file_error(ErrorInfo *error, const char *message)
{
    snprintf(error->message, sizeof(error->message), "%s", message);
    error->line = 0;
    error->column = 0;
}

/* "base_dir/table_name.ext" 형태의 파일 경로를 조립하여 dest에 저장한다.
 *
 * @param dest       결과 경로를 저장할 버퍼
 * @param dest_size  dest 버퍼 크기
 * @param base_dir   기본 디렉토리 경로
 * @param table_name 테이블 이름
 * @param extension  파일 확장자 (예: ".csv")
 */
static void build_table_path(char *dest,
                             size_t dest_size,
                             const char *base_dir,
                             const char *table_name,
                             const char *extension)
{
    snprintf(dest, dest_size, "%s/%s%s", base_dir, table_name, extension);
}

/* 컬럼 이름으로 스키마 내 인덱스를 찾아 반환한다.
 *
 * @param schema  테이블 스키마 포인터
 * @param name    찾을 컬럼 이름
 * @return        해당 컬럼의 인덱스, 없으면 -1
 */
static int find_schema_column(const TableSchema *schema, const char *name)
{
    for (int i = 0; i < schema->column_count; i++) {
        if (strcmp(schema->columns[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* 리터럴 타입이 스키마 컬럼 타입과 일치하는지 확인한다.
 *
 * @param data_type  스키마 컬럼의 데이터 타입
 * @param value      비교할 리터럴 값 포인터
 * @return           타입이 일치하면 1, 아니면 0
 */
static int validate_literal_type(DataType data_type, const LiteralValue *value)
{
    if (data_type == DATA_TYPE_INT    && value->type == LITERAL_INT)    return 1;
    if (data_type == DATA_TYPE_STRING && value->type == LITERAL_STRING) return 1;
    return 0;
}

/* CSV 필드 1개를 파일에 쓴다.
 * 쉼표·따옴표·개행이 포함된 경우 큰따옴표로 감싸고 내부 따옴표를 이스케이프한다.
 *
 * @param file  쓸 파일 포인터
 * @param text  출력할 필드 문자열
 * @return      성공 시 1, 파일 쓰기 오류 시 0
 */
static int write_csv_field(FILE *file, const char *text)
{
    int needs_quote = 0;
    for (const char *cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor == ',' || *cursor == '"' || *cursor == '\n') {
            needs_quote = 1;
            break;
        }
    }

    if (!needs_quote) {
        return fputs(text, file) != EOF;
    }

    if (fputc('"', file) == EOF) return 0;

    for (const char *cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor == '"') {
            if (fputc('"', file) == EOF) return 0;
        }
        if (fputc(*cursor, file) == EOF) return 0;
    }

    return fputc('"', file) != EOF;
}

/* 컬럼 값 배열을 CSV 한 줄로 파일에 쓴다.
 *
 * @param file         쓸 파일 포인터
 * @param values       컬럼 값 배열
 * @param value_count  출력할 컬럼 수
 * @return             성공 시 1, 파일 쓰기 오류 시 0
 */
static int write_csv_row(FILE *file,
                         char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                         int value_count)
{
    for (int i = 0; i < value_count; i++) {
        if (i > 0 && fputc(',', file) == EOF) return 0;
        if (!write_csv_field(file, values[i])) return 0;
    }
    return fputc('\n', file) != EOF;
}

/* CSV 데이터 파일이 없으면 헤더를 포함하여 새로 생성하고,
 * 이미 있으면 헤더가 스키마와 일치하는지 검증한다.
 *
 * @param config  실행 설정 구조체 포인터
 * @param schema  테이블 스키마 포인터
 * @param error   오류 정보 저장 포인터
 * @return        성공 시 1, 실패 시 0
 */
static int ensure_data_file(const AppConfig *config,
                            const TableSchema *schema,
                            ErrorInfo *error)
{
    char path[EXECUTOR_MAX_PATH_LEN];
    build_table_path(path, sizeof(path), config->data_dir, schema->table_name, ".csv");

    FILE *file = fopen(path, "rb");
    if (file != NULL) {
        char line[EXECUTOR_MAX_ROW_LEN];
        if (fgets(line, sizeof(line), file) == NULL) {
            fclose(file);
            set_file_error(error, "기존 데이터 파일 헤더를 읽을 수 없습니다.");
            return 0;
        }

        char header_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
        int header_count;
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

        for (int i = 0; i < schema->column_count; i++) {
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

    for (int i = 0; i < schema->column_count; i++) {
        if (i > 0 && fputc(',', file) == EOF) {
            fclose(file);
            set_file_error(error, "데이터 파일 헤더를 쓸 수 없습니다.");
            return 0;
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

/* CSV 한 줄을 컬럼 값 배열로 파싱한다. 큰따옴표 이스케이프도 처리한다.
 *
 * @param line         파싱할 CSV 줄 문자열
 * @param values       결과 컬럼 값 배열
 * @param value_count  파싱된 컬럼 수를 저장할 포인터
 * @return             성공 시 1, 형식 오류 시 0
 */
static int parse_csv_line(const char *line,
                          char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                          int *value_count)
{
    int in_quotes = 0;
    int row_index = 0;
    int text_index = 0;

    for (int i = 0; line[i] != '\0' && line[i] != '\n' && line[i] != '\r'; i++) {
        if (row_index >= SQLPROC_MAX_COLUMNS) return 0;

        if (line[i] == '"') {
            if (in_quotes && line[i + 1] == '"') {
                if (text_index >= SQLPROC_MAX_VALUE_LEN - 1) return 0;
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

        if (text_index >= SQLPROC_MAX_VALUE_LEN - 1) return 0;
        values[row_index][text_index] = line[i];
        text_index += 1;
    }

    if (row_index >= SQLPROC_MAX_COLUMNS) return 0;
    values[row_index][text_index] = '\0';
    *value_count = row_index + 1;
    return 1;
}

/* CSV 문자열을 long 정수로 변환한다. 완전히 파싱되지 않으면 실패를 반환한다.
 *
 * @param text   변환할 문자열
 * @param value  결과를 저장할 long 포인터
 * @return       성공 시 1, 실패 시 0
 */
static int parse_int_text(const char *text, long *value)
{
    if (text[0] == '\0') return 0;
    char *end_pointer;
    *value = strtol(text, &end_pointer, 10);
    return *end_pointer == '\0';
}

/* CSV 행 값과 WHERE 조건의 리터럴을 스키마 타입에 따라 비교한다.
 * int는 숫자 비교, string은 사전순 비교를 수행한다.
 *
 * @param data_type     컬럼 데이터 타입
 * @param row_value     CSV에서 읽은 문자열 값
 * @param operator_type 비교 연산자
 * @param literal       비교 대상 리터럴 포인터
 * @return              조건 만족 시 1, 불만족 시 0
 */
static int compare_values(DataType data_type,
                          const char *row_value,
                          CompareOperator operator_type,
                          const LiteralValue *literal)
{
    int compare_result = 0;

    if (data_type == DATA_TYPE_INT) {
        long left_value;
        long right_value;
        parse_int_text(row_value, &left_value);
        parse_int_text(literal->text, &right_value);
        if (left_value < right_value)      compare_result = -1;
        else if (left_value > right_value) compare_result = 1;
    } else {
        compare_result = strcmp(row_value, literal->text);
    }

    if (operator_type == COMPARE_EQUAL)         return compare_result == 0;
    if (operator_type == COMPARE_LESS)          return compare_result <  0;
    if (operator_type == COMPARE_LESS_EQUAL)    return compare_result <= 0;
    if (operator_type == COMPARE_GREATER)       return compare_result >  0;
    return compare_result >= 0;
}

/* CSV 한 행이 WHERE 조건들을 모두 만족하는지 검사한다.
 *
 * @param schema       테이블 스키마 포인터
 * @param values       CSV 행의 컬럼 값 배열
 * @param where_clause WHERE 조건 목록 포인터
 * @param error        오류 정보 저장 포인터
 * @return             모두 만족하면 1, 불만족하면 0, 오류이면 -1
 */
static int row_matches_where(const TableSchema *schema,
                             char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                             const WhereClause *where_clause,
                             ErrorInfo *error)
{
    for (int i = 0; i < where_clause->count; i++) {
        int column_index = find_schema_column(schema, where_clause->items[i].column_name);
        if (column_index < 0) {
            set_runtime_error(error, "WHERE 절의 컬럼이 스키마에 없습니다.",
                              where_clause->items[i].column_location);
            return -1;
        }

        if (!validate_literal_type(schema->columns[column_index].type,
                                   &where_clause->items[i].value)) {
            set_runtime_error(error, "WHERE 절 리터럴 타입이 스키마와 맞지 않습니다.",
                              where_clause->items[i].value.location);
            return -1;
        }

        if (schema->columns[column_index].type == DATA_TYPE_INT) {
            if (values[column_index][0] == '\0') return 0;
            long unused_value;
            if (!parse_int_text(values[column_index], &unused_value)) {
                set_runtime_error(error, "정수 컬럼에 숫자가 아닌 값이 저장되어 있습니다.",
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

/* SELECT 실행 전에 WHERE 절의 컬럼명과 리터럴 타입이 스키마와 맞는지 검증한다.
 *
 * @param schema       테이블 스키마 포인터
 * @param where_clause WHERE 조건 목록 포인터
 * @param error        오류 정보 저장 포인터
 * @return             유효하면 1, 아니면 0
 */
static int validate_where_clause(const TableSchema *schema,
                                 const WhereClause *where_clause,
                                 ErrorInfo *error)
{
    for (int i = 0; i < where_clause->count; i++) {
        int column_index = find_schema_column(schema, where_clause->items[i].column_name);
        if (column_index < 0) {
            set_runtime_error(error, "WHERE 절의 컬럼이 스키마에 없습니다.",
                              where_clause->items[i].column_location);
            return 0;
        }
        if (!validate_literal_type(schema->columns[column_index].type,
                                   &where_clause->items[i].value)) {
            set_runtime_error(error, "WHERE 절 리터럴 타입이 스키마와 맞지 않습니다.",
                              where_clause->items[i].value.location);
            return 0;
        }
    }
    return 1;
}

/* INSERT AST의 값을 스키마 컬럼 순서에 맞게 row_values 배열에 배치한다.
 * 컬럼 목록이 생략된 경우 스키마 순서대로, 명시된 경우 이름으로 매핑한다.
 *
 * @param schema           테이블 스키마 포인터
 * @param statement        INSERT AST 포인터
 * @param row_values       결과 행 값 배열
 * @param value_locations  각 값의 소스 위치 배열
 * @param error            오류 정보 저장 포인터
 * @return                 성공 시 1, 실패 시 0
 */
static int build_insert_row_values(const TableSchema *schema,
                                   const InsertStatement *statement,
                                   char row_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                                   SourceLocation value_locations[SQLPROC_MAX_COLUMNS],
                                   ErrorInfo *error)
{
    int used_columns[SQLPROC_MAX_COLUMNS];
    memset(used_columns, 0, sizeof(used_columns));
    memset(row_values, 0, sizeof(char[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN]));
    memset(value_locations, 0, sizeof(SourceLocation) * SQLPROC_MAX_COLUMNS);

    if (!statement->has_column_list) {
        if (statement->value_count != schema->column_count) {
            set_runtime_error(error, "VALUES 값 수가 스키마 컬럼 수와 일치하지 않습니다.",
                              statement->table_location);
            return 0;
        }
        for (int i = 0; i < schema->column_count; i++) {
            if (!validate_literal_type(schema->columns[i].type, &statement->values[i])) {
                set_runtime_error(error, "INSERT 값 타입이 스키마와 맞지 않습니다.",
                                  statement->values[i].location);
                return 0;
            }
            snprintf(row_values[i], sizeof(row_values[i]), "%s", statement->values[i].text);
            value_locations[i] = statement->values[i].location;
        }
        return 1;
    }

    for (int i = 0; i < statement->column_count; i++) {
        int schema_index = find_schema_column(schema, statement->column_names[i]);
        if (schema_index < 0) {
            set_runtime_error(error, "INSERT 대상 컬럼이 스키마에 없습니다.",
                              statement->column_locations[i]);
            return 0;
        }
        if (used_columns[schema_index]) {
            set_runtime_error(error, "같은 컬럼이 INSERT 문에 두 번 들어왔습니다.",
                              statement->column_locations[i]);
            return 0;
        }
        if (!validate_literal_type(schema->columns[schema_index].type, &statement->values[i])) {
            set_runtime_error(error, "INSERT 값 타입이 스키마와 맞지 않습니다.",
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

/* CSV 파일 전체를 순회하여 삽입할 행의 PRIMARY KEY 중복 여부를 검사한다.
 *
 * @param file             열린 CSV 파일 포인터
 * @param schema           테이블 스키마 포인터
 * @param row_values       삽입할 행의 값 배열
 * @param value_locations  각 값의 소스 위치 배열
 * @param error            오류 정보 저장 포인터
 * @return                 중복 없으면 1, 중복이면 0
 */
static int validate_primary_key_insert(FILE *file,
                                       const TableSchema *schema,
                                       char row_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                                       SourceLocation value_locations[SQLPROC_MAX_COLUMNS],
                                       ErrorInfo *error)
{
    int pk_index = schema->primary_key_column_index;
    if (pk_index < 0) return 1;

    if (row_values[pk_index][0] == '\0') {
        set_runtime_error(error, "PRIMARY KEY 컬럼 값이 필요합니다.", value_locations[pk_index]);
        return 0;
    }

    char line[EXECUTOR_MAX_ROW_LEN];
    rewind(file);
    if (fgets(line, sizeof(line), file) == NULL) {
        set_file_error(error, "데이터 파일 헤더를 읽을 수 없습니다.");
        return 0;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char existing_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
        int value_count;
        if (!parse_csv_line(line, existing_values, &value_count)) {
            set_file_error(error, "기존 데이터 행 형식이 잘못되었습니다.");
            return 0;
        }
        if (value_count != schema->column_count) {
            set_file_error(error, "기존 데이터 행 컬럼 수가 스키마와 다릅니다.");
            return 0;
        }
        if (strcmp(existing_values[pk_index], row_values[pk_index]) == 0) {
            set_runtime_error(error, "PRIMARY KEY 값이 이미 존재합니다.", value_locations[pk_index]);
            return 0;
        }
    }

    return 1;
}

/* INSERT 문을 실행한다. CSV에 행을 추가하고 관련 인덱스를 갱신한다.
 * 인덱스 갱신 실패 시 CSV를 원래대로 되돌리고 인덱스를 재빌드한다.
 *
 * @param config     실행 설정 구조체 포인터
 * @param statement  INSERT AST 포인터
 * @param error      오류 정보 저장 포인터
 * @return           성공 시 1, 실패 시 0
 */
static int execute_insert(const AppConfig *config,
                          const InsertStatement *statement,
                          ErrorInfo *error)
{
    TableSchema schema;
    if (!load_table_schema(config->schema_dir, statement->table_name, &schema, error)) return 0;

    char row_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
    SourceLocation value_locations[SQLPROC_MAX_COLUMNS];
    if (!build_insert_row_values(&schema, statement, row_values, value_locations, error)) return 0;
    if (!ensure_data_file(config, &schema, error)) return 0;

    char path[EXECUTOR_MAX_PATH_LEN];
    build_table_path(path, sizeof(path), config->data_dir, schema.table_name, ".csv");
    FILE *file = fopen(path, "rb+");
    if (file == NULL) {
        set_file_error(error, "데이터 파일을 열 수 없습니다.");
        return 0;
    }

    if (!validate_primary_key_insert(file, &schema, row_values, value_locations, error)) {
        fclose(file);
        return 0;
    }

    fseek(file, 0, SEEK_END);
    long row_offset = ftell(file);
    if (!write_csv_row(file, row_values, schema.column_count)) {
        fclose(file);
        set_file_error(error, "데이터 행을 파일에 쓸 수 없습니다.");
        return 0;
    }

    int changed_index;
    if (!update_all_indexes_for_row(config, &schema, row_values, row_offset, &changed_index, error)) {
        ErrorInfo update_error = *error;
        fflush(file);
        ftruncate(fileno(file), row_offset);
        fclose(file);

        if (changed_index) {
            ErrorInfo rebuild_error;
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

/* SELECT 컬럼 목록을 스키마 인덱스 배열로 변환한다. SELECT *이면 전체 컬럼을 반환한다.
 *
 * @param schema           테이블 스키마 포인터
 * @param statement        SELECT AST 포인터
 * @param selected_indices 결과 인덱스 배열
 * @param selected_count   결과 인덱스 개수를 저장할 포인터
 * @param error            오류 정보 저장 포인터
 * @return                 성공 시 1, 실패 시 0
 */
static int resolve_selected_columns(const TableSchema *schema,
                                    const SelectStatement *statement,
                                    int selected_indices[SQLPROC_MAX_COLUMNS],
                                    int *selected_count,
                                    ErrorInfo *error)
{
    *selected_count = 0;

    if (statement->select_all) {
        for (int i = 0; i < schema->column_count; i++) {
            selected_indices[(*selected_count)++] = i;
        }
        return 1;
    }

    for (int i = 0; i < statement->column_count; i++) {
        int column_index = find_schema_column(schema, statement->column_names[i]);
        if (column_index < 0) {
            set_runtime_error(error, "SELECT 대상 컬럼이 스키마에 없습니다.",
                              statement->column_locations[i]);
            *selected_count = 0;
            return 0;
        }
        selected_indices[(*selected_count)++] = column_index;
    }

    return 1;
}

/* SELECT 결과의 헤더 줄을 탭 구분으로 stdout에 출력한다.
 *
 * @param schema           테이블 스키마 포인터
 * @param selected_indices 출력할 컬럼 인덱스 배열
 * @param selected_count   출력할 컬럼 수
 */
static void print_selected_header(const TableSchema *schema,
                                  int selected_indices[SQLPROC_MAX_COLUMNS],
                                  int selected_count)
{
    for (int i = 0; i < selected_count; i++) {
        if (i > 0) fputc('\t', stdout);
        fputs(schema->columns[selected_indices[i]].name, stdout);
    }
    fputc('\n', stdout);
}

/* CSV 파일에서 지정한 바이트 오프셋 위치의 행을 읽어 파싱한다.
 *
 * @param file         열린 CSV 파일 포인터
 * @param row_offset   읽을 행의 파일 내 바이트 오프셋
 * @param values       결과 컬럼 값 배열
 * @param value_count  파싱된 컬럼 수를 저장할 포인터
 * @param error        오류 정보 저장 포인터
 * @return             성공 시 1, 실패 시 0
 */
static int read_row_at_offset(FILE *file,
                              long row_offset,
                              char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN],
                              int *value_count,
                              ErrorInfo *error)
{
    if (fseek(file, row_offset, SEEK_SET) != 0) {
        set_file_error(error, "CSV 행 위치로 이동할 수 없습니다.");
        return 0;
    }

    char line[EXECUTOR_MAX_ROW_LEN];
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

/* SELECT 문을 실행한다. 인덱스가 있으면 인덱스 경로로, 없으면 전체 스캔으로 결과를 출력한다.
 *
 * @param config     실행 설정 구조체 포인터
 * @param statement  SELECT AST 포인터
 * @param error      오류 정보 저장 포인터
 * @return           성공 시 1, 실패 시 0
 */
static int execute_select(const AppConfig *config,
                          const SelectStatement *statement,
                          ErrorInfo *error)
{
    TableSchema schema;
    if (!load_table_schema(config->schema_dir, statement->table_name, &schema, error)) return 0;

    int selected_indices[SQLPROC_MAX_COLUMNS];
    int selected_count;
    if (!resolve_selected_columns(&schema, statement, selected_indices, &selected_count, error)) return 0;
    if (!validate_where_clause(&schema, &statement->where_clause, error)) return 0;

    long candidate_offsets[SQLPROC_MAX_INDEX_RESULTS];
    int candidate_count;
    int used_index;
    if (!try_collect_offsets_from_indexes(config, &schema, statement,
                                          candidate_offsets, &candidate_count, &used_index, error)) {
        return 0;
    }

    char path[EXECUTOR_MAX_PATH_LEN];
    build_table_path(path, sizeof(path), config->data_dir, schema.table_name, ".csv");
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        if (errno == ENOENT) {
            print_selected_header(&schema, selected_indices, selected_count);
            return 1;
        }
        set_file_error(error, "데이터 파일을 열 수 없습니다.");
        return 0;
    }

    char line[EXECUTOR_MAX_ROW_LEN];
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return 1;
    }

    {
        char header_values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
        int header_count;
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
        for (int i = 0; i < schema.column_count; i++) {
            if (strcmp(header_values[i], schema.columns[i].name) != 0) {
                fclose(file);
                set_file_error(error, "CSV 헤더 순서가 스키마와 다릅니다.");
                return 0;
            }
        }
    }

    print_selected_header(&schema, selected_indices, selected_count);

    if (used_index) {
        char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
        for (int offset_index = 0; offset_index < candidate_count; offset_index++) {
            int value_count;
            memset(values, 0, sizeof(values));
            if (!read_row_at_offset(file, candidate_offsets[offset_index], values, &value_count, error)) {
                fclose(file);
                return 0;
            }
            if (value_count != schema.column_count) {
                fclose(file);
                set_file_error(error, "CSV 컬럼 수가 스키마와 맞지 않습니다.");
                return 0;
            }
            int match_result = row_matches_where(&schema, values, &statement->where_clause, error);
            if (match_result < 0) { fclose(file); return 0; }
            if (!match_result) continue;

            for (int i = 0; i < selected_count; i++) {
                if (i > 0) fputc('\t', stdout);
                fputs(values[selected_indices[i]], stdout);
            }
            fputc('\n', stdout);
        }
        fclose(file);
        return 1;
    }

    char values[SQLPROC_MAX_COLUMNS][SQLPROC_MAX_VALUE_LEN];
    while (fgets(line, sizeof(line), file) != NULL) {
        int value_count;
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
        int match_result = row_matches_where(&schema, values, &statement->where_clause, error);
        if (match_result < 0) { fclose(file); return 0; }
        if (!match_result) continue;

        for (int i = 0; i < selected_count; i++) {
            if (i > 0) fputc('\t', stdout);
            fputs(values[selected_indices[i]], stdout);
        }
        fputc('\n', stdout);
    }

    fclose(file);
    return 1;
}

/* CREATE INDEX 문 실행을 btree_index 모듈에 위임한다.
 *
 * @param config     실행 설정 구조체 포인터
 * @param statement  CREATE INDEX AST 포인터
 * @param error      오류 정보 저장 포인터
 * @return           성공 시 1, 실패 시 0
 */
static int execute_create_index(const AppConfig *config,
                                const CreateIndexStatement *statement,
                                ErrorInfo *error)
{
    return create_index_from_statement(config, statement, error);
}

/* SQL 프로그램의 모든 문장을 순서대로 실행한다.
 *
 * @param config   실행 설정 구조체 포인터
 * @param program  실행할 SQL 프로그램 AST 포인터
 * @param error    오류 정보 저장 포인터
 * @return         성공 시 1, 실패 시 0
 */
int execute_program(const AppConfig *config, const SqlProgram *program, ErrorInfo *error)
{
    memset(error, 0, sizeof(*error));

    for (int i = 0; i < program->count; i++) {
        if (program->items[i].type == STATEMENT_INSERT) {
            if (!execute_insert(config, &program->items[i].insert_statement, error)) return 0;
            continue;
        }
        if (program->items[i].type == STATEMENT_SELECT) {
            if (!execute_select(config, &program->items[i].select_statement, error)) return 0;
            continue;
        }
        if (!execute_create_index(config, &program->items[i].create_index_statement, error)) return 0;
    }

    return 1;
}
