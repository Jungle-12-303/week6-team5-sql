#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "sqlproc.h"

static int ensure_directory(const char *path);
static int write_text_file(const char *path, const char *text);
static int file_contains_text(const char *path, const char *needle);
static int file_equals_text(const char *path, const char *expected_text);
static int capture_run_program(const AppConfig *config, const char *output_path);
static int capture_run_program_with_input(const AppConfig *config,
                                          const char *input_text,
                                          const char *output_path);
static int create_temp_workspace(char *base_path,
                                 size_t base_size,
                                 char *schema_dir,
                                 size_t schema_size,
                                 char *data_dir,
                                 size_t data_size,
                                 char *index_dir,
                                 size_t index_size,
                                 const char *prefix);
static int append_text(char *dest, size_t dest_size, const char *text);

/* --schema-dir, --data-dir, --index-dir, input.sql 인수 파싱이 성공하는지 검증한다. */
static int test_parse_arguments_success(void)
{
    AppConfig config;
    char *argv[] = {
        "sqlproc",
        "--schema-dir", "schemas",
        "--data-dir", "data",
        "--index-dir", "indexes",
        "input.sql"
    };

    if (!parse_arguments(8, argv, &config)) return 0;
    if (strcmp(config.schema_dir, "schemas") != 0) return 0;
    if (strcmp(config.data_dir, "data") != 0) return 0;
    if (strcmp(config.index_dir, "indexes") != 0) return 0;
    if (strcmp(config.input_path, "input.sql") != 0) return 0;
    if (!config.has_input_path) return 0;
    return 1;
}

/* dest 문자열 끝에 text를 이어 붙인다. 버퍼가 부족하면 0을 반환한다.
 *
 * @param dest       대상 문자열 버퍼
 * @param dest_size  버퍼 크기
 * @param text       이어 붙일 문자열
 * @return           성공 시 1, 버퍼 부족 시 0
 */
static int append_text(char *dest, size_t dest_size, const char *text)
{
    size_t current_length = strlen(dest);
    size_t text_length = strlen(text);
    if (current_length + text_length >= dest_size) return 0;
    memcpy(dest + current_length, text, text_length + 1);
    return 1;
}

/* 필수 인수 누락 시 parse_arguments가 실패하는지 검증한다. */
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

/* input.sql 없이 REPL 모드 인수 파싱이 성공하는지 검증한다. */
static int test_parse_arguments_repl_success(void)
{
    AppConfig config;
    char *argv[] = {
        "sqlproc",
        "--schema-dir", "schemas",
        "--data-dir", "data",
        "--index-dir", "indexes"
    };
    if (!parse_arguments(7, argv, &config)) return 0;
    if (config.has_input_path) return 0;
    return config.input_path[0] == '\0';
}

/* SELECT 문이 올바르게 토크나이징되는지 검증한다. */
static int test_tokenize_select(void)
{
    TokenList tokens;
    ErrorInfo error;

    if (!tokenize_sql("SELECT name FROM users;", &tokens, &error)) return 0;
    if (tokens.count != 6) return 0;
    if (tokens.items[0].type != TOKEN_KEYWORD_SELECT) return 0;
    if (tokens.items[1].type != TOKEN_IDENTIFIER || strcmp(tokens.items[1].text, "name") != 0) return 0;
    if (tokens.items[3].type != TOKEN_IDENTIFIER || strcmp(tokens.items[3].text, "users") != 0) return 0;
    if (tokens.items[4].type != TOKEN_SEMICOLON) return 0;
    return tokens.items[5].type == TOKEN_EOF;
}

/* 컬럼 목록이 있는 INSERT 문이 올바르게 파싱되는지 검증한다. */
static int test_parse_insert_statement(void)
{
    TokenList tokens;
    SqlProgram program;
    ErrorInfo error;

    if (!tokenize_sql("INSERT INTO users (id, name) VALUES (1, 'kim');", &tokens, &error)) return 0;
    if (!parse_program(&tokens, &program, &error)) return 0;
    if (program.count != 1) return 0;
    if (program.items[0].type != STATEMENT_INSERT) return 0;
    if (strcmp(program.items[0].insert_statement.table_name, "users") != 0) return 0;
    if (program.items[0].insert_statement.column_count != 2) return 0;
    if (strcmp(program.items[0].insert_statement.column_names[1], "name") != 0) return 0;
    if (program.items[0].insert_statement.values[0].type != LITERAL_INT) return 0;
    if (program.items[0].insert_statement.values[1].location.line != 1) return 0;
    if (program.items[0].insert_statement.values[1].location.column <= 0) return 0;
    return strcmp(program.items[0].insert_statement.values[1].text, "kim") == 0;
}

/* 컬럼 목록이 없는 INSERT 문이 올바르게 파싱되는지 검증한다. */
static int test_parse_insert_without_column_list(void)
{
    TokenList tokens;
    SqlProgram program;
    ErrorInfo error;

    if (!tokenize_sql("INSERT INTO users VALUES (1, 'park', 40);", &tokens, &error)) return 0;
    if (!parse_program(&tokens, &program, &error)) return 0;
    if (program.items[0].insert_statement.has_column_list) return 0;
    if (program.items[0].insert_statement.column_count != 0) return 0;
    if (program.items[0].insert_statement.value_count != 3) return 0;
    return strcmp(program.items[0].insert_statement.values[1].text, "park") == 0;
}

/* WHERE AND 조건이 있는 SELECT와 CREATE INDEX가 올바르게 파싱되는지 검증한다. */
static int test_parse_select_where_and_create_index(void)
{
    TokenList tokens;
    SqlProgram program;
    ErrorInfo error;
    const char *sql = "SELECT * FROM users WHERE age >= 20 AND id = 1;"
                      "CREATE INDEX idx_users_age ON users(age);";

    if (!tokenize_sql(sql, &tokens, &error)) return 0;
    if (!parse_program(&tokens, &program, &error)) return 0;
    if (program.count != 2) return 0;
    if (program.items[0].type != STATEMENT_SELECT) return 0;
    if (!program.items[0].select_statement.select_all) return 0;
    if (program.items[0].select_statement.where_clause.count != 2) return 0;
    if (program.items[0].select_statement.where_clause.items[0].operator_type != COMPARE_GREATER_EQUAL) return 0;
    if (program.items[0].select_statement.where_clause.items[1].column_location.column <= 0) return 0;
    if (strcmp(program.items[1].create_index_statement.index_name, "idx_users_age") != 0) return 0;
    return strcmp(program.items[1].create_index_statement.column_name, "age") == 0;
}

/* WHERE 조건이 3개 이상일 때 파서가 오류를 반환하는지 검증한다. */
static int test_parse_where_limit_fail(void)
{
    TokenList tokens;
    SqlProgram program;
    ErrorInfo error;
    const char *sql = "SELECT * FROM users WHERE age >= 20 AND id = 1 AND name = 'kim';";

    if (!tokenize_sql(sql, &tokens, &error)) return 0;
    if (parse_program(&tokens, &program, &error)) return 0;
    return strstr(error.message, "최대 2개") != NULL;
}

/* SELECT * 실행이 CSV 데이터를 올바르게 출력하는지 검증한다. */
static int test_run_program_success(void)
{
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char index_dir[256];

    if (!create_temp_workspace(base_dir, sizeof(base_dir),
                               schema_dir, sizeof(schema_dir),
                               data_dir, sizeof(data_dir),
                               index_dir, sizeof(index_dir),
                               "sqlproc_run_program_success_")) {
        return 0;
    }

    char schema_path[256];
    char data_path[256];
    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(data_path, sizeof(data_path), "%s/users.csv", data_dir);
    if (!write_text_file(schema_path, "id:int:pk,name:string\n")) return 0;
    if (!write_text_file(data_path, "id,name\n1,kim\n")) return 0;

    char path[256];
    char output_path[256];
    snprintf(path, sizeof(path), "%s/input.sql", base_dir);
    snprintf(output_path, sizeof(output_path), "%s/output.txt", base_dir);
    if (!write_text_file(path, "SELECT * FROM users;")) return 0;

    AppConfig config;
    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);
    snprintf(config.index_dir, sizeof(config.index_dir), "%s", index_dir);
    config.has_input_path = 1;
    snprintf(config.input_path, sizeof(config.input_path), "%s", path);

    if (!capture_run_program(&config, output_path)) return 0;
    return file_contains_text(output_path, "id\tname\n1\tkim\n");
}

/* 디렉토리가 없으면 생성하고, 이미 있으면 그대로 성공을 반환한다.
 *
 * @param path  확인/생성할 디렉토리 경로
 * @return      성공 시 1, 실패 시 0
 */
static int ensure_directory(const char *path)
{
    if (mkdir(path, 0777) == 0) return 1;
    return access(path, F_OK) == 0;
}

/* 지정한 경로에 텍스트 파일을 생성하고 내용을 쓴다.
 *
 * @param path  생성할 파일 경로
 * @param text  파일에 쓸 내용
 * @return      성공 시 1, 실패 시 0
 */
static int write_text_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) return 0;
    fputs(text, file);
    fclose(file);
    return 1;
}

/* 파일에 특정 부분 문자열이 포함되어 있는지 확인한다.
 *
 * @param path    확인할 파일 경로
 * @param needle  찾을 부분 문자열
 * @return        포함되면 1, 아니면 0
 */
static int file_contains_text(const char *path, const char *needle)
{
    char buffer[2048];
    FILE *file = fopen(path, "rb");
    if (file == NULL) return 0;
    size_t size = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[size] = '\0';
    return strstr(buffer, needle) != NULL;
}

/* 파일 전체 내용이 기대 문자열과 정확히 일치하는지 확인한다.
 *
 * @param path           확인할 파일 경로
 * @param expected_text  기대하는 전체 내용 문자열
 * @return               일치하면 1, 아니면 0
 */
static int file_equals_text(const char *path, const char *expected_text)
{
    char buffer[2048];
    FILE *file = fopen(path, "rb");
    if (file == NULL) return 0;
    size_t size = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[size] = '\0';
    return strcmp(buffer, expected_text) == 0;
}

/* run_program의 stdout을 파일로 리다이렉트하여 출력 결과를 캡처한다.
 *
 * @param config       실행 설정 구조체 포인터
 * @param output_path  캡처 결과를 저장할 파일 경로
 * @return             run_program이 0을 반환하면 1, 아니면 0
 */
static int capture_run_program(const AppConfig *config, const char *output_path)
{
    FILE *file = fopen(output_path, "wb");
    if (file == NULL) return 0;

    fflush(stdout);
    int saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdout < 0) { fclose(file); return 0; }

    if (dup2(fileno(file), STDOUT_FILENO) < 0) {
        close(saved_stdout);
        fclose(file);
        return 0;
    }

    int result = run_program(config);
    fflush(stdout);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    fclose(file);
    return result == 0;
}

/* REPL 모드에서 input_text를 stdin으로 주입하고 stdout 결과를 파일로 캡처한다.
 *
 * @param config       실행 설정 구조체 포인터
 * @param input_text   stdin에 주입할 SQL 텍스트
 * @param output_path  캡처 결과를 저장할 파일 경로
 * @return             run_program이 0을 반환하면 1, 아니면 0
 */
static int capture_run_program_with_input(const AppConfig *config,
                                          const char *input_text,
                                          const char *output_path)
{
    FILE *input_file = tmpfile();
    if (input_file == NULL) return 0;

    FILE *output_file = fopen(output_path, "wb");
    if (output_file == NULL) { fclose(input_file); return 0; }

    fputs(input_text, input_file);
    rewind(input_file);

    fflush(stdout);
    int saved_stdin = dup(STDIN_FILENO);
    int saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdin < 0 || saved_stdout < 0) {
        if (saved_stdin >= 0) close(saved_stdin);
        if (saved_stdout >= 0) close(saved_stdout);
        fclose(input_file);
        fclose(output_file);
        return 0;
    }

    if (dup2(fileno(input_file), STDIN_FILENO) < 0 ||
        dup2(fileno(output_file), STDOUT_FILENO) < 0) {
        dup2(saved_stdin, STDIN_FILENO);
        dup2(saved_stdout, STDOUT_FILENO);
        close(saved_stdin);
        close(saved_stdout);
        fclose(input_file);
        fclose(output_file);
        return 0;
    }

    int result = run_program(config);
    fflush(stdout);
    dup2(saved_stdin, STDIN_FILENO);
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdin);
    close(saved_stdout);
    fclose(input_file);
    fclose(output_file);
    return result == 0;
}

/* /tmp 아래에 고유한 테스트 워크스페이스 디렉토리와 하위 디렉토리를 생성한다.
 *
 * @param base_path   생성된 기본 디렉토리 경로를 저장할 버퍼
 * @param base_size   base_path 버퍼 크기
 * @param schema_dir  스키마 디렉토리 경로를 저장할 버퍼
 * @param schema_size schema_dir 버퍼 크기
 * @param data_dir    데이터 디렉토리 경로를 저장할 버퍼
 * @param data_size   data_dir 버퍼 크기
 * @param index_dir   인덱스 디렉토리 경로를 저장할 버퍼
 * @param index_size  index_dir 버퍼 크기
 * @param prefix      디렉토리 이름 접두사
 * @return            성공 시 1, 실패 시 0
 */
static int create_temp_workspace(char *base_path,
                                 size_t base_size,
                                 char *schema_dir,
                                 size_t schema_size,
                                 char *data_dir,
                                 size_t data_size,
                                 char *index_dir,
                                 size_t index_size,
                                 const char *prefix)
{
    snprintf(base_path, base_size, "/tmp/%sXXXXXX", prefix);
    if (mkdtemp(base_path) == NULL) return 0;

    snprintf(schema_dir, schema_size, "%s/schemas", base_path);
    snprintf(data_dir, data_size, "%s/data", base_path);
    snprintf(index_dir, index_size, "%s/indexes", base_path);

    return ensure_directory(schema_dir) &&
           ensure_directory(data_dir) &&
           ensure_directory(index_dir);
}

/* 빈 SQL 문자열을 파싱할 때 오류가 반환되는지 검증한다. */
static int test_parse_empty_sql_fail(void)
{
    TokenList tokens;
    SqlProgram program;
    ErrorInfo error;

    if (!tokenize_sql("", &tokens, &error)) return 0;
    if (parse_program(&tokens, &program, &error)) return 0;
    return strstr(error.message, "비어") != NULL;
}

/* INSERT 후 WHERE 조건이 있는 SELECT가 올바른 행을 반환하는지 검증한다. */
static int test_insert_and_select_where_execution(void)
{
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char index_dir[256];

    if (!create_temp_workspace(base_dir, sizeof(base_dir),
                               schema_dir, sizeof(schema_dir),
                               data_dir, sizeof(data_dir),
                               index_dir, sizeof(index_dir),
                               "sqlproc_where_executor_test_")) {
        return 0;
    }

    char insert_sql_path[256];
    char select_sql_path[256];
    char output_path[256];
    char schema_path[256];
    char data_path[256];
    snprintf(insert_sql_path, sizeof(insert_sql_path), "%s/insert.sql", base_dir);
    snprintf(select_sql_path, sizeof(select_sql_path), "%s/select.sql", base_dir);
    snprintf(output_path, sizeof(output_path), "%s/select.out", base_dir);
    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(data_path, sizeof(data_path), "%s/users.csv", data_dir);

    if (!write_text_file(schema_path, "id:int:pk,name:string,age:int\n")) return 0;
    if (!write_text_file(insert_sql_path,
                         "INSERT INTO users (id, name, age) VALUES (1, 'kim', 20);"
                         "INSERT INTO users (id, name, age) VALUES (2, 'lee', 30);\n")) {
        return 0;
    }

    AppConfig insert_config;
    memset(&insert_config, 0, sizeof(insert_config));
    snprintf(insert_config.schema_dir, sizeof(insert_config.schema_dir), "%s", schema_dir);
    snprintf(insert_config.data_dir, sizeof(insert_config.data_dir), "%s", data_dir);
    snprintf(insert_config.index_dir, sizeof(insert_config.index_dir), "%s", index_dir);
    insert_config.has_input_path = 1;
    snprintf(insert_config.input_path, sizeof(insert_config.input_path), "%s", insert_sql_path);

    if (run_program(&insert_config) != 0) return 0;
    if (!file_equals_text(data_path, "id,name,age\n1,kim,20\n2,lee,30\n")) return 0;

    if (!write_text_file(select_sql_path,
                         "SELECT name, age FROM users WHERE age >= 25 AND id >= 2;\n")) {
        return 0;
    }

    AppConfig select_config;
    memset(&select_config, 0, sizeof(select_config));
    snprintf(select_config.schema_dir, sizeof(select_config.schema_dir), "%s", schema_dir);
    snprintf(select_config.data_dir, sizeof(select_config.data_dir), "%s", data_dir);
    snprintf(select_config.index_dir, sizeof(select_config.index_dir), "%s", index_dir);
    select_config.has_input_path = 1;
    snprintf(select_config.input_path, sizeof(select_config.input_path), "%s", select_sql_path);

    if (!capture_run_program(&select_config, output_path)) return 0;
    return file_equals_text(output_path, "name\tage\nlee\t30\n");
}

/* B+ 트리 인덱스 생성, INSERT 후 인덱스 갱신, 범위 쿼리가 올바르게 동작하는지 검증한다. */
static int test_btree_index_persistence_and_scan(void)
{
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char index_dir[256];

    if (!create_temp_workspace(base_dir, sizeof(base_dir),
                               schema_dir, sizeof(schema_dir),
                               data_dir, sizeof(data_dir),
                               index_dir, sizeof(index_dir),
                               "sqlproc_btree_index_test_")) {
        return 0;
    }

    char insert_sql_path[256];
    char create_index_sql_path[256];
    char insert_after_index_sql_path[256];
    char select_sql_path[256];
    char output_path[256];
    char schema_path[256];
    char index_path[256];
    snprintf(insert_sql_path, sizeof(insert_sql_path), "%s/insert.sql", base_dir);
    snprintf(create_index_sql_path, sizeof(create_index_sql_path), "%s/create_index.sql", base_dir);
    snprintf(insert_after_index_sql_path, sizeof(insert_after_index_sql_path),
             "%s/insert_after_index.sql", base_dir);
    snprintf(select_sql_path, sizeof(select_sql_path), "%s/select.sql", base_dir);
    snprintf(output_path, sizeof(output_path), "%s/output.txt", base_dir);
    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(index_path, sizeof(index_path), "%s/idx_users_age.idx", index_dir);

    if (!write_text_file(schema_path, "id:int:pk,name:string,age:int\n")) return 0;
    if (!write_text_file(insert_sql_path,
                         "INSERT INTO users (id, name, age) VALUES (1, 'kim', 20);"
                         "INSERT INTO users (id, name, age) VALUES (2, 'lee', 30);\n")) {
        return 0;
    }

    AppConfig config;
    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);
    snprintf(config.index_dir, sizeof(config.index_dir), "%s", index_dir);
    config.has_input_path = 1;
    snprintf(config.input_path, sizeof(config.input_path), "%s", insert_sql_path);
    if (run_program(&config) != 0) return 0;

    if (!write_text_file(create_index_sql_path, "CREATE INDEX idx_users_age ON users(age);\n")) return 0;
    snprintf(config.input_path, sizeof(config.input_path), "%s", create_index_sql_path);
    if (run_program(&config) != 0) return 0;
    if (access(index_path, F_OK) != 0) return 0;

    if (!write_text_file(insert_after_index_sql_path,
                         "INSERT INTO users (id, name, age) VALUES (3, 'park', 40);\n")) {
        return 0;
    }
    snprintf(config.input_path, sizeof(config.input_path), "%s", insert_after_index_sql_path);
    if (run_program(&config) != 0) return 0;

    TableSchema schema;
    ErrorInfo error;
    if (!load_table_schema(schema_dir, "users", &schema, &error)) return 0;

    SelectStatement statement;
    memset(&statement, 0, sizeof(statement));
    snprintf(statement.table_name, sizeof(statement.table_name), "users");
    statement.where_clause.count = 1;
    snprintf(statement.where_clause.items[0].column_name,
             sizeof(statement.where_clause.items[0].column_name), "age");
    statement.where_clause.items[0].operator_type = COMPARE_GREATER_EQUAL;
    statement.where_clause.items[0].value.type = LITERAL_INT;
    snprintf(statement.where_clause.items[0].value.text,
             sizeof(statement.where_clause.items[0].value.text), "30");

    long offsets[SQLPROC_MAX_INDEX_RESULTS];
    int offset_count;
    int used_index;
    if (!try_collect_offsets_from_indexes(&config, &schema, &statement,
                                          offsets, &offset_count, &used_index, &error)) {
        return 0;
    }
    if (!used_index || offset_count != 2) return 0;

    if (!write_text_file(select_sql_path,
                         "SELECT id, name, age FROM users WHERE age >= 30 AND id >= 2;\n")) {
        return 0;
    }
    snprintf(config.input_path, sizeof(config.input_path), "%s", select_sql_path);
    if (!capture_run_program(&config, output_path)) return 0;

    return file_equals_text(output_path, "id\tname\tage\n2\tlee\t30\n3\tpark\t40\n");
}

/* 중복 키가 많을 때 B+ 트리 분할과 등호 쿼리가 올바르게 동작하는지 검증한다. */
static int test_btree_duplicate_split_query(void)
{
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char index_dir[256];

    if (!create_temp_workspace(base_dir, sizeof(base_dir),
                               schema_dir, sizeof(schema_dir),
                               data_dir, sizeof(data_dir),
                               index_dir, sizeof(index_dir),
                               "sqlproc_btree_duplicate_test_")) {
        return 0;
    }

    char insert_sql_path[256];
    char create_index_sql_path[256];
    char select_sql_path[256];
    char output_path[256];
    char schema_path[256];
    snprintf(insert_sql_path, sizeof(insert_sql_path), "%s/insert.sql", base_dir);
    snprintf(create_index_sql_path, sizeof(create_index_sql_path), "%s/create_index.sql", base_dir);
    snprintf(select_sql_path, sizeof(select_sql_path), "%s/select.sql", base_dir);
    snprintf(output_path, sizeof(output_path), "%s/output.txt", base_dir);
    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);

    if (!write_text_file(schema_path, "id:int:pk,age:int\n")) return 0;

    FILE *file = fopen(insert_sql_path, "wb");
    if (file == NULL) return 0;

    for (int i = 1; i <= 20; i++) {
        int age = (i <= 10) ? 30 : 100 + i;
        fprintf(file, "INSERT INTO users (id, age) VALUES (%d, %d);", i, age);
    }
    fputc('\n', file);
    fclose(file);

    AppConfig config;
    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);
    snprintf(config.index_dir, sizeof(config.index_dir), "%s", index_dir);
    config.has_input_path = 1;
    snprintf(config.input_path, sizeof(config.input_path), "%s", insert_sql_path);
    if (run_program(&config) != 0) return 0;

    if (!write_text_file(create_index_sql_path, "CREATE INDEX idx_users_age ON users(age);\n")) return 0;
    snprintf(config.input_path, sizeof(config.input_path), "%s", create_index_sql_path);
    if (run_program(&config) != 0) return 0;

    TableSchema schema;
    ErrorInfo error;
    if (!load_table_schema(schema_dir, "users", &schema, &error)) return 0;

    SelectStatement statement;
    memset(&statement, 0, sizeof(statement));
    snprintf(statement.table_name, sizeof(statement.table_name), "users");
    statement.where_clause.count = 1;
    snprintf(statement.where_clause.items[0].column_name,
             sizeof(statement.where_clause.items[0].column_name), "age");
    statement.where_clause.items[0].operator_type = COMPARE_EQUAL;
    statement.where_clause.items[0].value.type = LITERAL_INT;
    snprintf(statement.where_clause.items[0].value.text,
             sizeof(statement.where_clause.items[0].value.text), "30");

    long offsets[SQLPROC_MAX_INDEX_RESULTS];
    int offset_count;
    int used_index;
    if (!try_collect_offsets_from_indexes(&config, &schema, &statement,
                                          offsets, &offset_count, &used_index, &error)) {
        return 0;
    }
    if (!used_index || offset_count != 10) return 0;

    if (!write_text_file(select_sql_path, "SELECT id, age FROM users WHERE age = 30;\n")) return 0;
    snprintf(config.input_path, sizeof(config.input_path), "%s", select_sql_path);
    if (!capture_run_program(&config, output_path)) return 0;

    char expected_output[1024];
    snprintf(expected_output, sizeof(expected_output), "id\tage\n");
    for (int i = 1; i <= 10; i++) {
        char line[64];
        snprintf(line, sizeof(line), "%d\t30\n", i);
        if (!append_text(expected_output, sizeof(expected_output), line)) return 0;
    }

    return file_equals_text(output_path, expected_output);
}

/* REPL 모드에서 INSERT와 SELECT가 올바르게 동작하는지 검증한다. */
static int test_interactive_insert_and_select(void)
{
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char index_dir[256];

    if (!create_temp_workspace(base_dir, sizeof(base_dir),
                               schema_dir, sizeof(schema_dir),
                               data_dir, sizeof(data_dir),
                               index_dir, sizeof(index_dir),
                               "sqlproc_repl_test_")) {
        return 0;
    }

    char output_path[256];
    char schema_path[256];
    snprintf(output_path, sizeof(output_path), "%s/output.txt", base_dir);
    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    if (!write_text_file(schema_path, "id:int:pk,name:string,age:int\n")) return 0;

    AppConfig config;
    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);
    snprintf(config.index_dir, sizeof(config.index_dir), "%s", index_dir);

    if (!capture_run_program_with_input(&config,
                                        "INSERT INTO users VALUES (1, 'park', 40);\n"
                                        "SELECT * FROM users;\n"
                                        "quit\n",
                                        output_path)) {
        return 0;
    }

    return file_equals_text(output_path, "id\tname\tage\n1\tpark\t40\n");
}

/* PRIMARY KEY 중복 삽입 시 두 번째 INSERT가 거부되고 CSV가 롤백되는지 검증한다. */
static int test_primary_key_duplicate_fail(void)
{
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char index_dir[256];

    if (!create_temp_workspace(base_dir, sizeof(base_dir),
                               schema_dir, sizeof(schema_dir),
                               data_dir, sizeof(data_dir),
                               index_dir, sizeof(index_dir),
                               "sqlproc_pk_test_")) {
        return 0;
    }

    char sql_path[256];
    char data_path[256];
    char schema_path[256];
    snprintf(sql_path, sizeof(sql_path), "%s/input.sql", base_dir);
    snprintf(data_path, sizeof(data_path), "%s/users.csv", data_dir);
    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);

    if (!write_text_file(schema_path, "id:int:pk,name:string,age:int\n")) return 0;
    if (!write_text_file(sql_path,
                         "INSERT INTO users VALUES (1, 'kim', 20);"
                         "INSERT INTO users VALUES (1, 'lee', 30);\n")) {
        return 0;
    }

    AppConfig config;
    memset(&config, 0, sizeof(config));
    config.has_input_path = 1;
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);
    snprintf(config.index_dir, sizeof(config.index_dir), "%s", index_dir);
    snprintf(config.input_path, sizeof(config.input_path), "%s", sql_path);

    FILE *null_file = fopen("/dev/null", "wb");
    if (null_file == NULL) return 0;

    fflush(stderr);
    int saved_stderr = dup(STDERR_FILENO);
    if (saved_stderr < 0) { fclose(null_file); return 0; }

    if (dup2(fileno(null_file), STDERR_FILENO) < 0) {
        close(saved_stderr);
        fclose(null_file);
        return 0;
    }

    int result = run_program(&config);
    fflush(stderr);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);
    fclose(null_file);

    if (result == 0) return 0;
    return file_equals_text(data_path, "id,name,age\n1,kim,20\n");
}

/* 모든 테스트를 순서대로 실행하고 결과를 출력한다.
 *
 * @return  모든 테스트 통과 시 0, 실패 시 1
 */
int main(void)
{
    if (!test_parse_arguments_success())              { fprintf(stderr, "test_parse_arguments_success failed\n");              return 1; }
    if (!test_parse_arguments_fail())                 { fprintf(stderr, "test_parse_arguments_fail failed\n");                 return 1; }
    if (!test_parse_arguments_repl_success())         { fprintf(stderr, "test_parse_arguments_repl_success failed\n");         return 1; }
    if (!test_tokenize_select())                      { fprintf(stderr, "test_tokenize_select failed\n");                      return 1; }
    if (!test_parse_insert_statement())               { fprintf(stderr, "test_parse_insert_statement failed\n");               return 1; }
    if (!test_parse_insert_without_column_list())     { fprintf(stderr, "test_parse_insert_without_column_list failed\n");     return 1; }
    if (!test_parse_select_where_and_create_index())  { fprintf(stderr, "test_parse_select_where_and_create_index failed\n");  return 1; }
    if (!test_parse_where_limit_fail())               { fprintf(stderr, "test_parse_where_limit_fail failed\n");               return 1; }
    if (!test_run_program_success())                  { fprintf(stderr, "test_run_program_success failed\n");                  return 1; }
    if (!test_parse_empty_sql_fail())                 { fprintf(stderr, "test_parse_empty_sql_fail failed\n");                 return 1; }
    if (!test_insert_and_select_where_execution())    { fprintf(stderr, "test_insert_and_select_where_execution failed\n");    return 1; }
    if (!test_btree_index_persistence_and_scan())     { fprintf(stderr, "test_btree_index_persistence_and_scan failed\n");     return 1; }
    if (!test_btree_duplicate_split_query())          { fprintf(stderr, "test_btree_duplicate_split_query failed\n");          return 1; }
    if (!test_interactive_insert_and_select())        { fprintf(stderr, "test_interactive_insert_and_select failed\n");        return 1; }
    if (!test_primary_key_duplicate_fail())           { fprintf(stderr, "test_primary_key_duplicate_fail failed\n");           return 1; }

    printf("All executor tests passed.\n");
    return 0;
}
