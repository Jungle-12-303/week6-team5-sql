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
static int capture_run_program(const AppConfig *config, const char *output_path);
static int capture_storage_print_rows(const AppConfig *config,
                                      const TableSchema *schema,
                                      const int selected_indices[SQLPROC_MAX_COLUMNS],
                                      int selected_count,
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
    char buffer[2048];
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

static int capture_run_program(const AppConfig *config, const char *output_path)
{
    FILE *file;
    int saved_stdout;
    int result;

    /* run_program의 stdout을 파일로 받아 SELECT 결과를 검증할 때 사용합니다. */
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

    result = run_program(config);
    fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    fclose(file);
    return result == 0;
}

static int capture_storage_print_rows(const AppConfig *config,
                                      const TableSchema *schema,
                                      const int selected_indices[SQLPROC_MAX_COLUMNS],
                                      int selected_count,
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

    result = storage_print_rows(config, schema, selected_indices, selected_count, error);
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

static int test_storage_print_rows_without_data_file(void)
{
    AppConfig config;
    TableSchema schema;
    ErrorInfo error;
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
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);

    if (!load_table_schema(config.schema_dir, "users", &schema, &error)) {
        return 0;
    }

    selected_indices[0] = 0;
    selected_indices[1] = 1;

    if (storage_print_rows(&config, &schema, selected_indices, 2, &error)) {
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

    if (!test_parse_insert_statement()) {
        fprintf(stderr, "test_parse_insert_statement failed\n");
        return 1;
    }

    if (!test_parse_insert_without_column_list()) {
        fprintf(stderr, "test_parse_insert_without_column_list failed\n");
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

    if (!test_storage_print_rows_without_data_file()) {
        fprintf(stderr, "test_storage_print_rows_without_data_file failed\n");
        return 1;
    }

    if (!test_storage_print_rows_header_mismatch()) {
        fprintf(stderr, "test_storage_print_rows_header_mismatch failed\n");
        return 1;
    }

    printf("All tests passed.\n");
    return 0;
}
