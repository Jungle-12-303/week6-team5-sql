#include <stdio.h>

#include "sqlproc.h" // include/sqlproc.h

/* 프로그램 사용법을 stderr에 출력한다. */
static void print_usage(void)
{
    fprintf(stderr,
            "usage: ./sqlproc --schema-dir <dir> --data-dir <dir> "
            "--index-dir <dir> [input.sql]\n");
}

/* 프로그램 진입점. 커맨드라인 인수를 파싱한 뒤 프로그램을 실행한다. */
int main(int argc, char **argv)
{
    AppConfig config;
    if (!parse_arguments(argc, argv, &config))
    {
        print_usage();
        return 1;
    }
    return run_program(&config);
}
