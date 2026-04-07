#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sqlproc.h"

static int run_sql_text(const AppConfig *config, const char *sql_text, ErrorInfo *error);
static int run_interactive_mode(const AppConfig *config);
static int ends_with_semicolon(const char *text);
static void trim_copy(char *dest, size_t dest_size, const char *src);
static int is_exit_command(const char *text);

int parse_arguments(int argc, char **argv, AppConfig *config)
{
    int i;
    int option_limit;

    if (argc != 7 && argc != 8) {
        return 0;
    }

    memset(config, 0, sizeof(*config));
    option_limit = argc;
    if (argc == 8) {
        option_limit = argc - 1;
        config->has_input_path = 1;
    }

    for (i = 1; i < option_limit; i += 2) {
        if (strcmp(argv[i], "--schema-dir") == 0) {
            snprintf(config->schema_dir, sizeof(config->schema_dir), "%s", argv[i + 1]);
        } else if (strcmp(argv[i], "--data-dir") == 0) {
            snprintf(config->data_dir, sizeof(config->data_dir), "%s", argv[i + 1]);
        } else if (strcmp(argv[i], "--index-dir") == 0) {
            snprintf(config->index_dir, sizeof(config->index_dir), "%s", argv[i + 1]);
        } else {
            return 0;
        }
    }

    if (config->schema_dir[0] == '\0' ||
        config->data_dir[0] == '\0' ||
        config->index_dir[0] == '\0') {
        return 0;
    }

    if (config->has_input_path) {
        snprintf(config->input_path, sizeof(config->input_path), "%s", argv[argc - 1]);
    }

    return 1;
}

int load_sql_file(const char *path, char *buffer, size_t buffer_size, ErrorInfo *error)
{
    FILE *file;
    size_t read_size;
    size_t total_size;

    memset(error, 0, sizeof(*error));

    file = fopen(path, "rb");
    if (file == NULL) {
        snprintf(error->message, sizeof(error->message), "SQL 파일을 열 수 없습니다.");
        return 0;
    }

    total_size = fread(buffer, 1, buffer_size - 1, file);
    if (ferror(file)) {
        fclose(file);
        snprintf(error->message, sizeof(error->message), "SQL 파일을 읽는 중 오류가 발생했습니다.");
        return 0;
    }

    read_size = fread(buffer, 1, 1, file);
    if (read_size > 0) {
        fclose(file);
        snprintf(error->message, sizeof(error->message), "SQL 파일이 너무 큽니다.");
        return 0;
    }

    fclose(file);
    buffer[total_size] = '\0';
    return 1;
}

void print_error(const ErrorInfo *error)
{
    if (error->message[0] == '\0') {
        return;
    }

    if (error->line > 0) {
        fprintf(stderr, "오류: %s (line %d, column %d)\n",
                error->message,
                error->line,
                error->column);
        return;
    }

    fprintf(stderr, "오류: %s\n", error->message);
}

static int run_sql_text(const AppConfig *config, const char *sql_text, ErrorInfo *error)
{
    TokenList tokens;
    SqlProgram program;

    if (!tokenize_sql(sql_text, &tokens, error)) {
        return 0;
    }

    if (!parse_program(&tokens, &program, error)) {
        return 0;
    }

    return execute_program(config, &program, error);
}

static int ends_with_semicolon(const char *text)
{
    size_t length;

    length = strlen(text);
    while (length > 0) {
        char ch;

        ch = text[length - 1];
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
            length -= 1;
            continue;
        }

        return ch == ';';
    }

    return 0;
}

static void trim_copy(char *dest, size_t dest_size, const char *src)
{
    size_t start;
    size_t end;
    size_t length;

    start = 0;
    while (src[start] == ' ' || src[start] == '\n' || src[start] == '\r' || src[start] == '\t') {
        start += 1;
    }

    end = strlen(src);
    while (end > start) {
        char ch;

        ch = src[end - 1];
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
            end -= 1;
            continue;
        }

        break;
    }

    length = end - start;
    if (length >= dest_size) {
        length = dest_size - 1;
    }

    memcpy(dest, src + start, length);
    dest[length] = '\0';
}

static int is_exit_command(const char *text)
{
    char trimmed[64];

    trim_copy(trimmed, sizeof(trimmed), text);

    if (strcmp(trimmed, "exit") == 0 || strcmp(trimmed, "quit") == 0) {
        return 1;
    }

    if (strcmp(trimmed, "exit;") == 0 || strcmp(trimmed, "quit;") == 0) {
        return 1;
    }

    return 0;
}

static int run_interactive_mode(const AppConfig *config)
{
    char line[1024];
    char sql_buffer[SQLPROC_MAX_SQL_SIZE];
    int show_prompt;

    sql_buffer[0] = '\0';
    show_prompt = isatty(STDIN_FILENO);

    while (1) {
        ErrorInfo error;
        size_t current_length;
        size_t line_length;

        if (show_prompt) {
            if (sql_buffer[0] == '\0') {
                printf("sqlproc> ");
            } else {
                printf("...> ");
            }
            fflush(stdout);
        }

        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;
        }

        if (sql_buffer[0] == '\0' && is_exit_command(line)) {
            return 0;
        }

        current_length = strlen(sql_buffer);
        line_length = strlen(line);
        if (current_length + line_length >= sizeof(sql_buffer)) {
            fprintf(stderr, "오류: 입력 SQL이 너무 깁니다.\n");
            sql_buffer[0] = '\0';
            continue;
        }

        memcpy(sql_buffer + current_length, line, line_length + 1);

        if (!ends_with_semicolon(sql_buffer)) {
            continue;
        }

        memset(&error, 0, sizeof(error));
        if (!run_sql_text(config, sql_buffer, &error)) {
            print_error(&error);
            sql_buffer[0] = '\0';
            continue;
        }

        sql_buffer[0] = '\0';
    }

    if (sql_buffer[0] != '\0') {
        fprintf(stderr, "오류: 문장 끝에는 세미콜론이 필요합니다.\n");
        return 1;
    }

    return 0;
}

int run_program(const AppConfig *config)
{
    char sql_text[SQLPROC_MAX_SQL_SIZE];
    ErrorInfo error;

    if (!config->has_input_path) {
        return run_interactive_mode(config);
    }

    if (!load_sql_file(config->input_path, sql_text, sizeof(sql_text), &error)) {
        print_error(&error);
        return 1;
    }

    if (!run_sql_text(config, sql_text, &error)) {
        print_error(&error);
        return 1;
    }

    return 0;
}
