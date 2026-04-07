#include <stdio.h>
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

int run_program(const AppConfig *config)
{
    (void)config;
    fprintf(stderr, "기능 구현 전 스캐폴딩 단계입니다.\n");
    return 1;
}
