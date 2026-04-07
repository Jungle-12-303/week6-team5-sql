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
static int create_temp_workspace(char *base_path,
                                 size_t base_size,
                                 char *schema_dir,
                                 size_t schema_size,
                                 char *data_dir,
                                 size_t data_size,
                                 char *index_dir,
                                 size_t index_size,
                                 const char *prefix);

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

    if (!parse_arguments(8, argv, &config)) {
        return 0;
    }

    if (strcmp(config.schema_dir, "schemas") != 0) {
        return 0;
    }

    if (strcmp(config.data_dir, "data") != 0) {
        return 0;
    }

    if (strcmp(config.index_dir, "indexes") != 0) {
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

static int test_parse_select_where_and_create_index(void)
{
    TokenList tokens;
    SqlProgram program;
    ErrorInfo error;
    const char *sql;

    sql = "SELECT * FROM users WHERE age >= 20 AND id = 1;"
          "CREATE INDEX idx_users_age ON users(age);";

    if (!tokenize_sql(sql, &tokens, &error)) {
        return 0;
    }

    if (!parse_program(&tokens, &program, &error)) {
        return 0;
    }

    if (program.count != 2) {
        return 0;
    }

    if (program.items[0].type != STATEMENT_SELECT) {
        return 0;
    }

    if (!program.items[0].select_statement.select_all) {
        return 0;
    }

    if (program.items[0].select_statement.where_clause.count != 2) {
        return 0;
    }

    if (program.items[0].select_statement.where_clause.items[0].operator_type !=
        COMPARE_GREATER_EQUAL) {
        return 0;
    }

    if (program.items[0].select_statement.where_clause.items[1].column_location.column <= 0) {
        return 0;
    }

    if (strcmp(program.items[1].create_index_statement.index_name, "idx_users_age") != 0) {
        return 0;
    }

    return strcmp(program.items[1].create_index_statement.column_name, "age") == 0;
}

static int test_parse_where_limit_fail(void)
{
    TokenList tokens;
    SqlProgram program;
    ErrorInfo error;
    const char *sql;

    sql = "SELECT * FROM users WHERE age >= 20 AND id = 1 AND name = 'kim';";

    if (!tokenize_sql(sql, &tokens, &error)) {
        return 0;
    }

    if (parse_program(&tokens, &program, &error)) {
        return 0;
    }

    return strstr(error.message, "최대 2개") != NULL;
}

static int test_run_program_success(void)
{
    AppConfig config;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char index_dir[256];
    char path[256];
    char output_path[256];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               index_dir,
                               sizeof(index_dir),
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
    snprintf(config.index_dir, sizeof(config.index_dir), "%s", index_dir);
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
    if (mkdir(path, 0777) == 0) {
        return 1;
    }

    return access(path, F_OK) == 0;
}

static int write_text_file(const char *path, const char *text)
{
    FILE *file;

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

    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }

    size = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[size] = '\0';
    return strstr(buffer, needle) != NULL;
}

static int file_equals_text(const char *path, const char *expected_text)
{
    char buffer[2048];
    FILE *file;
    size_t size;

    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }

    size = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[size] = '\0';
    return strcmp(buffer, expected_text) == 0;
}

static int capture_run_program(const AppConfig *config, const char *output_path)
{
    FILE *file;
    int saved_stdout;
    int result;

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
    if (mkdtemp(base_path) == NULL) {
        return 0;
    }

    snprintf(schema_dir, schema_size, "%s/schemas", base_path);
    snprintf(data_dir, data_size, "%s/data", base_path);
    snprintf(index_dir, index_size, "%s/indexes", base_path);

    return ensure_directory(schema_dir) &&
           ensure_directory(data_dir) &&
           ensure_directory(index_dir);
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

static int test_insert_and_select_where_execution(void)
{
    AppConfig insert_config;
    AppConfig select_config;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char index_dir[256];
    char insert_sql_path[256];
    char select_sql_path[256];
    char output_path[256];
    char schema_path[256];
    char data_path[256];

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               index_dir,
                               sizeof(index_dir),
                               "sqlproc_where_executor_test_")) {
        return 0;
    }

    snprintf(insert_sql_path, sizeof(insert_sql_path), "%s/insert.sql", base_dir);
    snprintf(select_sql_path, sizeof(select_sql_path), "%s/select.sql", base_dir);
    snprintf(output_path, sizeof(output_path), "%s/select.out", base_dir);
    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(data_path, sizeof(data_path), "%s/users.csv", data_dir);

    if (!write_text_file(schema_path, "id:int,name:string,age:int\n")) {
        return 0;
    }

    if (!write_text_file(insert_sql_path,
                         "INSERT INTO users (id, name, age) VALUES (1, 'kim', 20);"
                         "INSERT INTO users (id, name, age) VALUES (2, 'lee', 30);\n")) {
        return 0;
    }

    memset(&insert_config, 0, sizeof(insert_config));
    snprintf(insert_config.schema_dir, sizeof(insert_config.schema_dir), "%s", schema_dir);
    snprintf(insert_config.data_dir, sizeof(insert_config.data_dir), "%s", data_dir);
    snprintf(insert_config.index_dir, sizeof(insert_config.index_dir), "%s", index_dir);
    snprintf(insert_config.input_path, sizeof(insert_config.input_path), "%s", insert_sql_path);

    if (run_program(&insert_config) != 0) {
        return 0;
    }

    if (!file_equals_text(data_path, "id,name,age\n1,kim,20\n2,lee,30\n")) {
        return 0;
    }

    if (!write_text_file(select_sql_path,
                         "SELECT name, age FROM users WHERE age >= 25 AND id >= 2;\n")) {
        return 0;
    }

    memset(&select_config, 0, sizeof(select_config));
    snprintf(select_config.schema_dir, sizeof(select_config.schema_dir), "%s", schema_dir);
    snprintf(select_config.data_dir, sizeof(select_config.data_dir), "%s", data_dir);
    snprintf(select_config.index_dir, sizeof(select_config.index_dir), "%s", index_dir);
    snprintf(select_config.input_path, sizeof(select_config.input_path), "%s", select_sql_path);

    if (!capture_run_program(&select_config, output_path)) {
        return 0;
    }

    if (!file_equals_text(output_path, "name\tage\nlee\t30\n")) {
        return 0;
    }

    return 1;
}

static int test_btree_index_persistence_and_scan(void)
{
    AppConfig config;
    TableSchema schema;
    SelectStatement statement;
    ErrorInfo error;
    char base_dir[256];
    char schema_dir[256];
    char data_dir[256];
    char index_dir[256];
    char insert_sql_path[256];
    char create_index_sql_path[256];
    char insert_after_index_sql_path[256];
    char select_sql_path[256];
    char output_path[256];
    char schema_path[256];
    char index_path[256];
    long offsets[SQLPROC_MAX_INDEX_RESULTS];
    int offset_count;
    int used_index;

    if (!create_temp_workspace(base_dir,
                               sizeof(base_dir),
                               schema_dir,
                               sizeof(schema_dir),
                               data_dir,
                               sizeof(data_dir),
                               index_dir,
                               sizeof(index_dir),
                               "sqlproc_btree_index_test_")) {
        return 0;
    }

    snprintf(insert_sql_path, sizeof(insert_sql_path), "%s/insert.sql", base_dir);
    snprintf(create_index_sql_path, sizeof(create_index_sql_path), "%s/create_index.sql", base_dir);
    snprintf(insert_after_index_sql_path,
             sizeof(insert_after_index_sql_path),
             "%s/insert_after_index.sql",
             base_dir);
    snprintf(select_sql_path, sizeof(select_sql_path), "%s/select.sql", base_dir);
    snprintf(output_path, sizeof(output_path), "%s/output.txt", base_dir);
    snprintf(schema_path, sizeof(schema_path), "%s/users.schema", schema_dir);
    snprintf(index_path, sizeof(index_path), "%s/idx_users_age.idx", index_dir);

    if (!write_text_file(schema_path, "id:int,name:string,age:int\n")) {
        return 0;
    }

    if (!write_text_file(insert_sql_path,
                         "INSERT INTO users (id, name, age) VALUES (1, 'kim', 20);"
                         "INSERT INTO users (id, name, age) VALUES (2, 'lee', 30);\n")) {
        return 0;
    }

    memset(&config, 0, sizeof(config));
    snprintf(config.schema_dir, sizeof(config.schema_dir), "%s", schema_dir);
    snprintf(config.data_dir, sizeof(config.data_dir), "%s", data_dir);
    snprintf(config.index_dir, sizeof(config.index_dir), "%s", index_dir);
    snprintf(config.input_path, sizeof(config.input_path), "%s", insert_sql_path);

    if (run_program(&config) != 0) {
        return 0;
    }

    if (!write_text_file(create_index_sql_path,
                         "CREATE INDEX idx_users_age ON users(age);\n")) {
        return 0;
    }

    snprintf(config.input_path, sizeof(config.input_path), "%s", create_index_sql_path);
    if (run_program(&config) != 0) {
        return 0;
    }

    if (access(index_path, F_OK) != 0) {
        return 0;
    }

    if (!write_text_file(insert_after_index_sql_path,
                         "INSERT INTO users (id, name, age) VALUES (3, 'park', 40);\n")) {
        return 0;
    }

    snprintf(config.input_path, sizeof(config.input_path), "%s", insert_after_index_sql_path);
    if (run_program(&config) != 0) {
        return 0;
    }

    if (!load_table_schema(schema_dir, "users", &schema, &error)) {
        return 0;
    }

    memset(&statement, 0, sizeof(statement));
    snprintf(statement.table_name, sizeof(statement.table_name), "users");
    statement.where_clause.count = 1;
    snprintf(statement.where_clause.items[0].column_name,
             sizeof(statement.where_clause.items[0].column_name),
             "age");
    statement.where_clause.items[0].operator_type = COMPARE_GREATER_EQUAL;
    statement.where_clause.items[0].value.type = LITERAL_INT;
    snprintf(statement.where_clause.items[0].value.text,
             sizeof(statement.where_clause.items[0].value.text),
             "30");

    if (!try_collect_offsets_from_indexes(&config,
                                          &schema,
                                          &statement,
                                          offsets,
                                          &offset_count,
                                          &used_index,
                                          &error)) {
        return 0;
    }

    if (!used_index || offset_count != 2) {
        return 0;
    }

    if (!write_text_file(select_sql_path,
                         "SELECT id, name, age FROM users WHERE age >= 30 AND id >= 2;\n")) {
        return 0;
    }

    snprintf(config.input_path, sizeof(config.input_path), "%s", select_sql_path);
    if (!capture_run_program(&config, output_path)) {
        return 0;
    }

    return file_equals_text(output_path, "id\tname\tage\n2\tlee\t30\n3\tpark\t40\n");
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

    if (!test_parse_select_where_and_create_index()) {
        fprintf(stderr, "test_parse_select_where_and_create_index failed\n");
        return 1;
    }

    if (!test_parse_where_limit_fail()) {
        fprintf(stderr, "test_parse_where_limit_fail failed\n");
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

    if (!test_insert_and_select_where_execution()) {
        fprintf(stderr, "test_insert_and_select_where_execution failed\n");
        return 1;
    }

    if (!test_btree_index_persistence_and_scan()) {
        fprintf(stderr, "test_btree_index_persistence_and_scan failed\n");
        return 1;
    }

    printf("All executor tests passed.\n");
    return 0;
}
