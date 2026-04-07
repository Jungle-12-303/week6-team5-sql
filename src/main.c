#include <stdio.h>

#include "sqlproc.h"

static void print_usage(void)
{
    fprintf(stderr,
            "usage: ./sqlproc --schema-dir <dir> --data-dir <dir> "
            "--index-dir <dir> <input.sql>\n");
}

int main(int argc, char **argv)
{
    AppConfig config;

    if (!parse_arguments(argc, argv, &config)) {
        print_usage();
        return 1;
    }

    return run_program(&config);
}
