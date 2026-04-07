#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlproc.h"

int parse_arguments(int argc, char **argv, AppConfig *config)
{
    int i;

    if (argc != 8) {
        return 0;
    }

    memset(config, 0, sizeof(*config));

    for (i = 1; i < argc - 1; i += 2) {
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

    snprintf(config->input_path, sizeof(config->input_path), "%s", argv[argc - 1]);
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

int run_program(const AppConfig *config)
{
    char sql_text[SQLPROC_MAX_SQL_SIZE];
    TokenList tokens;
    SqlProgram program;
    ErrorInfo error;

    if (!load_sql_file(config->input_path, sql_text, sizeof(sql_text), &error)) {
        print_error(&error);
        return 1;
    }

    if (!tokenize_sql(sql_text, &tokens, &error)) {
        print_error(&error);
        return 1;
    }

    if (!parse_program(&tokens, &program, &error)) {
        print_error(&error);
        return 1;
    }

    if (!execute_program(config, &program, &error)) {
        print_error(&error);
        return 1;
    }

    return 0;
}
