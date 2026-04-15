#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "sqlproc.h"

/*
 * test_runner.c는 프로젝트의 통합 테스트와 단위 성격 테스트를 함께 담습니다.
 * 초심자가 흐름을 따라가기 쉽도록 한 파일에서
 * - 인자 파싱
 * - 토크나이저/파서
 * - CSV 기반 스토리지/실행기
 * 를 순서대로 검증합니다.
 */

static int ensure_directory(const char *path);
static int write_text_file(const char *path, const char *text);
static int file_contains_text(const char *path, const char *needle);
static int capture_run_program_streams(const AppConfig *config,
                                       const char *output_path,
                                       const char *error_path,
                                       int *result);
static int capture_run_program(const AppConfig *config, const char *output_path);
static int capture_storage_print_rows(const AppConfig *config,
                                      const TableSchema *schema,
                                      const int selected_indices[SQLPROC_MAX_COLUMNS],
                                      int selected_count,
                                      const WhereClause *where_clause,
                                      const char *output_path,
                                      ErrorInfo *error);
static int create_temp_workspace(char *base_path,
                                 size_t base_size,
                                 char *schema_dir,
                                 size_t schema_size,
                                 char *data_dir,
                                 size_t data_size,
                                 const char *prefix);

static int test_parse_arguments_success(void)
{
    AppConfig config;
    char *argv[] = {
        "sqlproc",
        "--schema-dir", "schemas",
        "--data-dir", "data",
        "input.sql"
    };

    if (!parse_arguments(6, argv, &config)) {
        return 0;
    }

    if (strcmp(config.schema_dir, "schemas") != 0) {
        return 0;
    }

    if (strcmp(config.data_dir, "data") != 0) {
        return 0;
    }

    if (strcmp(config.input_path, "input.sql") != 0) {
        return 0;
    }

    return 1;
}

static int test_parse_arguments_fail(void)
{
    AppConfig config;
    char *argv[] = {
        "sqlproc",
        "--schema-dir", "schemas",
        "--data-dir", "data",
        "input.sql"
    };

    return !parse_arguments(5, argv, &config);
}

static int test_tokenize_select(void)
{
    TokenList tokens;
    ErrorInfo error;

    if (!tokenize_sql("SELECT name FROM users;", &tokens, &error)) {
        return 0;
    }

    if (tokens.count != 6) {
        return 0;
    }

    if (tokens.items[0].type != TOKEN_KEYWORD_SELECT) {
        return 0;
    }

    if (tokens.items[1].type != TOKEN_IDENTIFIER ||
        strcmp(tokens.items[1].text, "name") != 0) {
        return 0;
    }

    if (tokens.items[3].type != TOKEN_IDENTIFIER ||
        strcmp(tokens.items[3].text, "users") != 0) {
        return 0;
    }

    if (tokens.items[4].type != TOKEN_SEMICOLON) {
        return 0;
    }

    return tokens.items[5].type == TOKEN_EOF;
}

static int test_tokenize_where_select(void)
{
    TokenList tokens;
    ErrorInfo error;

    if (!tokenize_sql("SELECT * FROM users WHERE age >= 20 AND name = 'kim';",
                      &tokens,
                      &error)) {
        return 0;
    }

    if (tokens.items[4].type != TOKEN_KEYWORD_WHERE) {
        return 0;
    }

    if (tokens.items[6].type != TOKEN_GREATER_EQUAL) {
        return 0;
    }

    if (tokens.items[8].type != TOKEN_KEYWORD_AND) {
        return 0;
    }

    if (tokens.items[10].type != TOKEN_EQUAL) {
        return 0;
    }

    return tokens.items[11].type == TOKEN_STRING;
}

static int test_parse_insert_statement(void)
{
    TokenList tokens;
    SqlProgram program;
    ErrorInfo error;

    if (!tokenize_sql("INSERT INTO users (id, name) VALUES (1, 'kim');", &tokens, &error)) {
        return 0;
    }

    if (!parse_program(&tokens, &program, &error)) {
        return 0;
    }

    if (program.count != 1) {
        return 0;
    }

    if (program.items[0].type != STATEMENT_INSERT) {
        return 0;
    }

    if (strcmp(program.items[0].insert_statement.table_name, "users") != 0) {
        return 0;
    }

    if (program.items[0].insert_statement.column_count != 2) {
        return 0;
    }

    if (strcmp(program.items[0].insert_statement.column_names[1], "name") != 0) {
        return 0;
    }

    if (program.items[0].insert_statement.values[0].type != LITERAL_INT) {
        return 0;
    }

    if (program.items[0].insert_statement.values[1].location.line != 1) {
        return 0;
    }

    if (program.items[0].insert_statement.values[1].location.column <= 0) {
        return 0;
    }

    return strcmp(program.items[0].insert_statement.values[1].text, "kim") == 0;
}

static int test_parse_insert_without_column_list(void)
{
    TokenList tokens;
    SqlProgram program;
    ErrorInfo error;

    if (!tokenize_sql("INSERT INTO users VALUES (1, 'park', 40);", &tokens, &error)) {
        return 0;
    }

    if (!parse_program(&tokens, &program, &error)) {
        return 0;
    }

    if (program.items[0].insert_statement.has_column_list) {
        return 0;
    }

    if (program.items[0].insert_statement.column_count != 0) {
        return 0;
    }

    if (program.items[0].insert_statement.value_count != 3) {
        return 0;
    }

    return strcmp(program.items[0].insert_statement.values[1].text, "park") == 0;
}

static int test_parse_select_where_statement(void)
{
    TokenList tokens;
    SqlProgram program;
    ErrorInfo error;

    if (!tokenize_sql("SELECT name FROM users WHERE age >= 20 AND id = 1;",
                      &tokens,
                      &error)) {
        return 0;
    }

    if (!parse_program(&tokens, &program, &error)) {
        return 0;
    }

    if (program.items[0].type != STATEMENT_SELECT) {
        return 0;
    }

    if (program.items[0].select_statement.where_clause.count != 2) {
        return 0;
    }

    if (program.items[0].select_statement.where_clause.items[0].operator_type !=
        COMPARE_GREATER_EQUAL) {
        return 0;
    }

    if (program.items[0].select_statement.where_clause.items[1].operator_type !=
        COMPARE_EQUAL) {
        return 0;
    }

    return strcmp(program.items[0].select_statement.where_clause.items[1].column_name, "id") == 0;
}

static int test_parse_where_limit_fail(void)
{
    TokenList tokens;
    SqlProgram program;
    ErrorInfo error;

    if (!tokenize_sql("SELECT * FROM users WHERE age >= 20 AND id = 1 AND name = 'kim';",
                      &tokens,
                      &error)) {
        return 0;
    }

    if (parse_program(&tokens, &program, &error)) {
        return 0;
    }

    return strstr(error.message, "최대 2개") != NULL;
}

static int test_parse_where_or_fail(void)
{
    TokenList tokens;
    SqlProgram program;
    ErrorInfo error;

    if (!tokenize_sql("SELECT * FROM users WHERE age >= 20 OR id = 1;",
                      &tokens,
                      &error)) {
        return 0;
    }

    if (parse_program(&tokens, &program, &error)) {
        return 0;
    }

    return strstr(error.message, "OR 조건은 지원하지 않습니다.") != NULL;
}

static int test_run_program_success(void)
{
    AppConfig config;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char path[256];
    char output_path[256];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_run_program_success_")) {
        return 0;
    }

    snprintf(path, sizeof(path), "%s/input.sql", base_dir);
    snprintf(output_path, sizeof(output_path), "%s/output.txt", base_dir);

    {
        char schema_path[256];
        char data_path[256];

        snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
        snprintf(data_path, sizeof(data_path), "%s/users.csv", data_dir);

        if (!write_text_file(schema_path, "id:int,name:string\n")) {
            return 0;
        }

        if (!write_text_file(data_path, "id,name\n1,kim\n")) {
            return 0;
        }
    }

    if (!write_text_file(path, "SELECT * FROM users;")) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);
    snprintf(config.input_path, sizeof(config.input_path), "%s", path);

    if (!capture_run_program(&config, output_path)) {
        return 0;
    }

    if (!file_contains_text(output_path, "id\tname\n1\tkim\n")) {
        return 0;
    }

    return 1;
}

static int ensure_directory(const char *path)
{
    /* 테스트용 임시 워크스페이스 디렉터리를 보장합니다. */
    if (mkdir(path, 0777) == 0) {
        return 1;
    }

    return access(path, F_OK) == 0;
}

static int write_text_file(const char *path, const char *text)
{
    FILE *file;

    /* 테스트 입력 SQL, 스키마, CSV 파일을 간단히 만들기 위한 헬퍼입니다. */
    file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }

    fputs(text, file);
    fclose(file);
    return 1;
}

static int file_contains_text(const char *path, const char *needle)
{
    char buffer[4096];
    FILE *file;
    size_t size;

    /* 출력 파일에 특정 문자열이 포함됐는지 확인합니다. */
    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }

    size = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[size] = '\0';
    return strstr(buffer, needle) != NULL;
}

static int capture_run_program_streams(const AppConfig *config,
                                       const char *output_path,
                                       const char *error_path,
                                       int *result)
{
    FILE *output_file;
    FILE *error_file;
    int saved_stdout;
    int saved_stderr;
    int run_result;

    /*
     * run_program의 stdout/stderr를 각각 파일로 받아
     * 결과 표와 오류/실행 시간 메시지를 분리 검증할 때 사용합니다.
     */
    output_file = fopen(output_path, "wb");
    if (output_file == NULL) {
        return 0;
    }

    error_file = NULL;
    if (error_path != NULL) {
        error_file = fopen(error_path, "wb");
        if (error_file == NULL) {
            fclose(output_file);
            return 0;
        }
    }

    fflush(stdout);
    fflush(stderr);
    saved_stdout = dup(STDOUT_FILENO);
    saved_stderr = dup(STDERR_FILENO);
    if (saved_stdout < 0) {
        if (error_file != NULL) {
            fclose(error_file);
        }
        fclose(output_file);
        return 0;
    }

    if (saved_stderr < 0) {
        close(saved_stdout);
        if (error_file != NULL) {
            fclose(error_file);
        }
        fclose(output_file);
        return 0;
    }

    if (dup2(fileno(output_file), STDOUT_FILENO) < 0) {
        close(saved_stdout);
        close(saved_stderr);
        if (error_file != NULL) {
            fclose(error_file);
        }
        fclose(output_file);
        return 0;
    }

    if (error_file != NULL && dup2(fileno(error_file), STDERR_FILENO) < 0) {
        dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stdout);
        close(saved_stderr);
        fclose(error_file);
        fclose(output_file);
        return 0;
    }

    run_result = run_program(config);
    fflush(stdout);
    fflush(stderr);
    dup2(saved_stdout, STDOUT_FILENO);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stdout);
    close(saved_stderr);
    if (error_file != NULL) {
        fclose(error_file);
    }
    fclose(output_file);

    if (result != NULL) {
        *result = run_result;
    }

    return 1;
}

static int capture_run_program(const AppConfig *config, const char *output_path)
{
    int result;

    if (!capture_run_program_streams(config, output_path, "/dev/null", &result)) {
        return 0;
    }

    return result == 0;
}

static int capture_storage_print_rows(const AppConfig *config,
                                      const TableSchema *schema,
                                      const int selected_indices[SQLPROC_MAX_COLUMNS],
                                      int selected_count,
                                      const WhereClause *where_clause,
                                      const char *output_path,
                                      ErrorInfo *error)
{
    FILE *file;
    int saved_stdout;
    int result;

    /* storage_print_rows의 stdout을 파일로 받아 헤더/조회 출력을 검증합니다. */
    file = fopen(output_path, "wb");
    if (file == NULL) {
        return 0;
    }

    fflush(stdout);
    saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdout < 0) {
        fclose(file);
        return 0;
    }

    if (dup2(fileno(file), STDOUT_FILENO) < 0) {
        close(saved_stdout);
        fclose(file);
        return 0;
    }

    result = storage_print_rows(config,
                                schema,
                                selected_indices,
                                selected_count,
                                where_clause,
                                error);
    fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    fclose(file);
    return result;
}

static int create_temp_workspace(char *base_path,
                                 size_t base_size,
                                 char *schema_dir,
                                 size_t schema_size,
                                 char *data_dir,
                                 size_t data_size,
                                 const char *prefix)
{
    /* 각 테스트가 서로 간섭하지 않도록 /tmp 아래에 독립 워크스페이스를 만듭니다. */
    snprintf(base_path, base_size, "/tmp/%sXXXXXX", prefix);
    if (mkdtemp(base_path) == NULL) {
        return 0;
    }

    snprintf(schema_dir, schema_size, "%s/schemas", base_path);
    snprintf(data_dir, data_size, "%s/data", base_path);

    return ensure_directory(schema_dir) && ensure_directory(data_dir);
}

static int test_parse_empty_sql_fail(void)
{
    TokenList tokens;
    SqlProgram program;
    ErrorInfo error;

    if (!tokenize_sql("", &tokens, &error)) {
        return 0;
    }

    if (parse_program(&tokens, &program, &error)) {
        return 0;
    }

    return strstr(error.message, "비어") != NULL;
}

static int test_insert_and_select_execution(void)
{
    AppConfig config;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char data_path[256];
    char sql_path[256];
    char output_path[256];
    char schema_path[256];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_insert_select_test_")) {
        return 0;
    }

    snprintf(sql_path, sizeof(sql_path), "%s/input.sql", base_dir);
    snprintf(output_path, sizeof(output_path), "%s/output.txt", base_dir);
    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(data_path, sizeof(data_path), "%s/users.csv", data_dir);

    if (!write_text_file(schema_path, "id:int,name:string,age:int\n")) {
        return 0;
    }

    if (!write_text_file(sql_path,
                         "INSERT INTO users (id, name, age) VALUES (1, 'kim', 20);"
                         "INSERT INTO users (id, name, age) VALUES (2, 'lee', 30);"
                         "SELECT name, age FROM users;\n")) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);
    snprintf(config.input_path, sizeof(config.input_path), "%s", sql_path);

    if (!capture_run_program(&config, output_path)) {
        return 0;
    }

    if (!file_contains_text(data_path, "id,name,age\n1,kim,20\n2,lee,30\n")) {
        return 0;
    }

    return file_contains_text(output_path, "name\tage\nkim\t20\nlee\t30\n");
}

static int test_insert_and_select_where_execution(void)
{
    AppConfig config;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char data_path[256];
    char sql_path[256];
    char output_path[256];
    char schema_path[256];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_insert_select_where_test_")) {
        return 0;
    }

    snprintf(sql_path, sizeof(sql_path), "%s/input.sql", base_dir);
    snprintf(output_path, sizeof(output_path), "%s/output.txt", base_dir);
    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(data_path, sizeof(data_path), "%s/users.csv", data_dir);

    if (!write_text_file(schema_path, "id:int,name:string,age:int\n")) {
        return 0;
    }

    if (!write_text_file(sql_path,
                         "INSERT INTO users VALUES (1, 'kim', 20);"
                         "INSERT INTO users VALUES (2, 'lee', 30);"
                         "INSERT INTO users VALUES (3, 'lee', 24);"
                         "SELECT name, age FROM users WHERE age >= 25 AND name = 'lee';\n")) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);
    snprintf(config.input_path, sizeof(config.input_path), "%s", sql_path);

    if (!capture_run_program(&config, output_path)) {
        return 0;
    }

    if (!file_contains_text(data_path, "id,name,age\n1,kim,20\n2,lee,30\n3,lee,24\n")) {
        return 0;
    }

    if (!file_contains_text(output_path, "name\tage\nlee\t30\n")) {
        return 0;
    }

    return !file_contains_text(output_path, "lee\t24\n");
}

static int test_storage_print_rows_where_without_data_file(void)
{
    AppConfig config;
    TableSchema schema;
    ErrorInfo error;
    WhereClause where_clause;
    int selected_indices[SQLPROC_MAX_COLUMNS];
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char schema_path[256];
    char output_path[256];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_storage_where_header_only_")) {
        return 0;
    }

    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(output_path, sizeof(output_path), "%s/output.txt", base_dir);

    if (!write_text_file(schema_path, "id:int,name:string,age:int\n")) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    memset(&schema, 0, sizeof(schema));
    memset(&error, 0, sizeof(error));
    memset(&where_clause, 0, sizeof(where_clause));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);

    if (!load_table_schema(config.schema_dir, "users", &schema, &error)) {
        return 0;
    }

    where_clause.count = 1;
    snprintf(where_clause.items[0].column_name,
             sizeof(where_clause.items[0].column_name),
             "%s",
             "age");
    where_clause.items[0].operator_type = COMPARE_GREATER_EQUAL;
    where_clause.items[0].column_location.line = 1;
    where_clause.items[0].column_location.column = 27;
    where_clause.items[0].value.type = LITERAL_INT;
    snprintf(where_clause.items[0].value.text,
             sizeof(where_clause.items[0].value.text),
             "%s",
             "20");

    selected_indices[0] = 0;
    selected_indices[1] = 2;

    if (!capture_storage_print_rows(&config,
                                    &schema,
                                    selected_indices,
                                    2,
                                    &where_clause,
                                    output_path,
                                    &error)) {
        return 0;
    }

    return file_contains_text(output_path, "id\tage\n");
}

static int test_run_program_reports_elapsed_time(void)
{
    AppConfig config;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char sql_path[256];
    char output_path[256];
    char error_path[256];
    char schema_path[256];
    int result;

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_elapsed_time_test_")) {
        return 0;
    }

    snprintf(sql_path, sizeof(sql_path), "%s/input.sql", base_dir);
    snprintf(output_path, sizeof(output_path), "%s/output.txt", base_dir);
    snprintf(error_path, sizeof(error_path), "%s/error.txt", base_dir);
    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);

    if (!write_text_file(schema_path, "id:int,name:string\n")) {
        return 0;
    }

    if (!write_text_file(sql_path,
                         "INSERT INTO users VALUES (1, 'kim');"
                         "SELECT * FROM users WHERE id >= 1;\n")) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);
    snprintf(config.input_path, sizeof(config.input_path), "%s", sql_path);

    if (!capture_run_program_streams(&config, output_path, error_path, &result)) {
        return 0;
    }

    if (result != 0) {
        return 0;
    }

    if (!file_contains_text(output_path, "id\tname\n1\tkim\n")) {
        return 0;
    }

    return file_contains_text(error_path, "총 실행 시간: ");
}

static int test_where_int_overflow_literal_fail(void)
{
    AppConfig config;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char sql_path[256];
    char output_path[256];
    char error_path[256];
    char schema_path[256];
    char data_path[256];
    int result;

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_where_int_overflow_literal_")) {
        return 0;
    }

    snprintf(sql_path, sizeof(sql_path), "%s/input.sql", base_dir);
    snprintf(output_path, sizeof(output_path), "%s/output.txt", base_dir);
    snprintf(error_path, sizeof(error_path), "%s/error.txt", base_dir);
    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(data_path, sizeof(data_path), "%s/users.csv", data_dir);

    if (!write_text_file(schema_path, "id:int,name:string,age:int\n")) {
        return 0;
    }

    if (!write_text_file(data_path, "id,name,age\n1,kim,20\n")) {
        return 0;
    }

    if (!write_text_file(sql_path,
                         "SELECT * FROM users WHERE age >= 999999999999999999999999999999;\n")) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);
    snprintf(config.input_path, sizeof(config.input_path), "%s", sql_path);

    if (!capture_run_program_streams(&config, output_path, error_path, &result)) {
        return 0;
    }

    if (result == 0) {
        return 0;
    }

    return file_contains_text(error_path, "WHERE 절 리터럴 타입이 스키마와 맞지 않습니다.");
}

static int test_where_type_mismatch_fail(void)
{
    AppConfig config;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char sql_path[256];
    char output_path[256];
    char error_path[256];
    char schema_path[256];
    char data_path[256];
    int result;

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_where_type_fail_")) {
        return 0;
    }

    snprintf(sql_path, sizeof(sql_path), "%s/input.sql", base_dir);
    snprintf(output_path, sizeof(output_path), "%s/output.txt", base_dir);
    snprintf(error_path, sizeof(error_path), "%s/error.txt", base_dir);
    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(data_path, sizeof(data_path), "%s/users.csv", data_dir);

    if (!write_text_file(schema_path, "id:int,name:string,age:int\n")) {
        return 0;
    }

    if (!write_text_file(data_path, "id,name,age\n1,kim,20\n")) {
        return 0;
    }

    if (!write_text_file(sql_path, "SELECT * FROM users WHERE age = 'kim';\n")) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);
    snprintf(config.input_path, sizeof(config.input_path), "%s", sql_path);

    if (!capture_run_program_streams(&config, output_path, error_path, &result)) {
        return 0;
    }

    if (result == 0) {
        return 0;
    }

    return file_contains_text(error_path, "WHERE 절 리터럴 타입이 스키마와 맞지 않습니다.");
}

static int test_insert_int_overflow_literal_fail(void)
{
    AppConfig config;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char sql_path[256];
    char output_path[256];
    char error_path[256];
    char schema_path[256];
    int result;

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_insert_int_overflow_literal_")) {
        return 0;
    }

    snprintf(sql_path, sizeof(sql_path), "%s/input.sql", base_dir);
    snprintf(output_path, sizeof(output_path), "%s/output.txt", base_dir);
    snprintf(error_path, sizeof(error_path), "%s/error.txt", base_dir);
    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);

    if (!write_text_file(schema_path, "age:int\n")) {
        return 0;
    }

    if (!write_text_file(sql_path,
                         "INSERT INTO users VALUES (999999999999999999999999999999);\n")) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);
    snprintf(config.input_path, sizeof(config.input_path), "%s", sql_path);

    if (!capture_run_program_streams(&config, output_path, error_path, &result)) {
        return 0;
    }

    if (result == 0) {
        return 0;
    }

    return file_contains_text(error_path, "INSERT 값 타입이 스키마와 맞지 않습니다.");
}

static int test_where_invalid_int_data_fail(void)
{
    AppConfig config;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char sql_path[256];
    char output_path[256];
    char error_path[256];
    char schema_path[256];
    char data_path[256];
    int result;

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_where_bad_int_data_")) {
        return 0;
    }

    snprintf(sql_path, sizeof(sql_path), "%s/input.sql", base_dir);
    snprintf(output_path, sizeof(output_path), "%s/output.txt", base_dir);
    snprintf(error_path, sizeof(error_path), "%s/error.txt", base_dir);
    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(data_path, sizeof(data_path), "%s/users.csv", data_dir);

    if (!write_text_file(schema_path, "id:int,name:string,age:int\n")) {
        return 0;
    }

    if (!write_text_file(data_path, "id,name,age\n1,kim,twenty\n")) {
        return 0;
    }

    if (!write_text_file(sql_path, "SELECT * FROM users WHERE age >= 20;\n")) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);
    snprintf(config.input_path, sizeof(config.input_path), "%s", sql_path);

    if (!capture_run_program_streams(&config, output_path, error_path, &result)) {
        return 0;
    }

    if (result == 0) {
        return 0;
    }

    return file_contains_text(error_path, "정수 컬럼에 유효한 int 값이 아닌 데이터가 저장되어 있습니다.");
}

static int test_storage_print_rows_empty_file_fail(void)
{
    AppConfig config;
    TableSchema schema;
    ErrorInfo error;
    WhereClause where_clause;
    int selected_indices[SQLPROC_MAX_COLUMNS];
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char schema_path[256];
    char data_path[256];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_storage_empty_csv_")) {
        return 0;
    }

    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(data_path, sizeof(data_path), "%s/users.csv", data_dir);

    if (!write_text_file(schema_path, "id:int,name:string\n")) {
        return 0;
    }

    if (!write_text_file(data_path, "")) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    memset(&schema, 0, sizeof(schema));
    memset(&error, 0, sizeof(error));
    memset(&where_clause, 0, sizeof(where_clause));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);

    if (!load_table_schema(config.schema_dir, "users", &schema, &error)) {
        return 0;
    }

    selected_indices[0] = 0;
    selected_indices[1] = 1;

    if (storage_print_rows(&config, &schema, selected_indices, 2, &where_clause, &error)) {
        return 0;
    }

    return strstr(error.message, "CSV 헤더를 읽을 수 없습니다.") != NULL;
}

static int test_storage_print_rows_without_data_file(void)
{
    AppConfig config;
    TableSchema schema;
    ErrorInfo error;
    WhereClause where_clause;
    int selected_indices[SQLPROC_MAX_COLUMNS];
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char schema_path[256];
    char output_path[256];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_storage_header_only_")) {
        return 0;
    }

    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(output_path, sizeof(output_path), "%s/output.txt", base_dir);

    if (!write_text_file(schema_path, "id:int,name:string\n")) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    memset(&schema, 0, sizeof(schema));
    memset(&error, 0, sizeof(error));
    memset(&where_clause, 0, sizeof(where_clause));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);

    if (!load_table_schema(config.schema_dir, "users", &schema, &error)) {
        return 0;
    }

    selected_indices[0] = 0;
    selected_indices[1] = 1;

    if (!capture_storage_print_rows(&config,
                                    &schema,
                                    selected_indices,
                                    2,
                                    &where_clause,
                                    output_path,
                                    &error)) {
        return 0;
    }

    return file_contains_text(output_path, "id\tname\n");
}

static int test_storage_print_rows_header_mismatch(void)
{
    AppConfig config;
    TableSchema schema;
    ErrorInfo error;
    WhereClause where_clause;
    int selected_indices[SQLPROC_MAX_COLUMNS];
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char schema_path[256];
    char data_path[256];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               "sqlproc_storage_bad_header_")) {
        return 0;
    }

    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(data_path, sizeof(data_path), "%s/users.csv", data_dir);

    if (!write_text_file(schema_path, "id:int,name:string\n")) {
        return 0;
    }

    if (!write_text_file(data_path, "name,id\nkim,1\n")) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    memset(&schema, 0, sizeof(schema));
    memset(&error, 0, sizeof(error));
    memset(&where_clause, 0, sizeof(where_clause));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);

    if (!load_table_schema(config.schema_dir, "users", &schema, &error)) {
        return 0;
    }

    selected_indices[0] = 0;
    selected_indices[1] = 1;

    if (storage_print_rows(&config, &schema, selected_indices, 2, &where_clause, &error)) {
        return 0;
    }

    return strstr(error.message, "CSV 헤더 순서가 스키마와 다릅니다.") != NULL;
}

int main(void)
{
    if (!test_parse_arguments_success()) {
        fprintf(stderr, "test_parse_arguments_success failed\n");
        return 1;
    }

    if (!test_parse_arguments_fail()) {
        fprintf(stderr, "test_parse_arguments_fail failed\n");
        return 1;
    }

    if (!test_tokenize_select()) {
        fprintf(stderr, "test_tokenize_select failed\n");
        return 1;
    }

    if (!test_tokenize_where_select()) {
        fprintf(stderr, "test_tokenize_where_select failed\n");
        return 1;
    }

    if (!test_parse_insert_statement()) {
        fprintf(stderr, "test_parse_insert_statement failed\n");
        return 1;
    }

    if (!test_parse_insert_without_column_list()) {
        fprintf(stderr, "test_parse_insert_without_column_list failed\n");
        return 1;
    }

    if (!test_parse_select_where_statement()) {
        fprintf(stderr, "test_parse_select_where_statement failed\n");
        return 1;
    }

    if (!test_parse_where_limit_fail()) {
        fprintf(stderr, "test_parse_where_limit_fail failed\n");
        return 1;
    }

    if (!test_parse_where_or_fail()) {
        fprintf(stderr, "test_parse_where_or_fail failed\n");
        return 1;
    }

    if (!test_run_program_success()) {
        fprintf(stderr, "test_run_program_success failed\n");
        return 1;
    }

    if (!test_parse_empty_sql_fail()) {
        fprintf(stderr, "test_parse_empty_sql_fail failed\n");
        return 1;
    }

    if (!test_insert_and_select_execution()) {
        fprintf(stderr, "test_insert_and_select_execution failed\n");
        return 1;
    }

    if (!test_insert_and_select_where_execution()) {
        fprintf(stderr, "test_insert_and_select_where_execution failed\n");
        return 1;
    }

    if (!test_storage_print_rows_without_data_file()) {
        fprintf(stderr, "test_storage_print_rows_without_data_file failed\n");
        return 1;
    }

    if (!test_storage_print_rows_where_without_data_file()) {
        fprintf(stderr, "test_storage_print_rows_where_without_data_file failed\n");
        return 1;
    }

    if (!test_storage_print_rows_header_mismatch()) {
        fprintf(stderr, "test_storage_print_rows_header_mismatch failed\n");
        return 1;
    }

    if (!test_storage_print_rows_empty_file_fail()) {
        fprintf(stderr, "test_storage_print_rows_empty_file_fail failed\n");
        return 1;
    }

    if (!test_run_program_reports_elapsed_time()) {
        fprintf(stderr, "test_run_program_reports_elapsed_time failed\n");
        return 1;
    }

    if (!test_where_int_overflow_literal_fail()) {
        fprintf(stderr, "test_where_int_overflow_literal_fail failed\n");
        return 1;
    }

    if (!test_where_type_mismatch_fail()) {
        fprintf(stderr, "test_where_type_mismatch_fail failed\n");
        return 1;
    }

    if (!test_insert_int_overflow_literal_fail()) {
        fprintf(stderr, "test_insert_int_overflow_literal_fail failed\n");
        return 1;
    }

    if (!test_where_invalid_int_data_fail()) {
        fprintf(stderr, "test_where_invalid_int_data_fail failed\n");
        return 1;
    }

    printf("All tests passed.\n");
    return 0;
}
