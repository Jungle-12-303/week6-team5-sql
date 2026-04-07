#ifndef SQLPROC_H
#define SQLPROC_H

#define SQLPROC_MAX_NAME_LEN 64
#define SQLPROC_MAX_VALUE_LEN 64
#define SQLPROC_MAX_COLUMNS 16
#define SQLPROC_MAX_PREDICATES 2

typedef struct {
    char schema_dir[256];
    char data_dir[256];
    char index_dir[256];
    char input_path[256];
} AppConfig;

int run_program(const AppConfig *config);
int parse_arguments(int argc, char **argv, AppConfig *config);

#endif
