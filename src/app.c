#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "sqlproc.h"

/*
 * 이 파일은 프로그램의 "실행 제어"를 담당합니다.
 * - 명령행 인자를 읽어 AppConfig를 채우고
 * - SQL 파일을 읽어 토크나이저 -> 파서 -> 실행기로 넘기고
 * - 사용자에게 오류를 출력합니다.
 *
 * 즉 main.c와 실제 SQL 엔진 사이를 연결하는 중간 계층입니다.
 */

static int run_sql_text(const AppConfig *config, const char *sql_text, ErrorInfo *error);

static double elapsed_milliseconds(const struct timeval *start, const struct timeval *end)
{
    long seconds;
    long microseconds;

    seconds = end->tv_sec - start->tv_sec;
    microseconds = end->tv_usec - start->tv_usec;
    return (double)seconds * 1000.0 + (double)microseconds / 1000.0;
}

int parse_arguments(int argc, char **argv, AppConfig *config)
{
    int i;

    /*
     * 지원하는 실행 형식:
     *   ./sqlproc --schema-dir <dir> --data-dir <dir> <input.sql>
     *   -> argc == 6
     */
    if (argc != 6) {
        return 0;
    }

    /*
     * 이전 실행 값이 남지 않도록 config 전체를 0으로 초기화합니다.
     */
    memset(config, 0, sizeof(*config));

    /*
     * argv는 "--옵션 값" 쌍으로 들어오므로 2칸씩 전진합니다.
     * 마지막 인자(argv[5])는 SQL 파일 경로입니다.
     */
    for (i = 1; i < argc - 1; i += 2) {
        if (strcmp(argv[i], "--schema-dir") == 0) {
            snprintf(config->schema_dir, sizeof(config->schema_dir), "%s", argv[i + 1]);
        } else if (strcmp(argv[i], "--data-dir") == 0) {
            snprintf(config->data_dir, sizeof(config->data_dir), "%s", argv[i + 1]);
        } else {
            return 0;
        }
    }

    /*
     * schema_dir, data_dir는 항상 필요합니다.
     */
    if (config->schema_dir[0] == '\0' || config->data_dir[0] == '\0') {
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

    /*
     * 호출자에게 이전 오류 정보가 섞이지 않도록 먼저 초기화합니다.
     */
    memset(error, 0, sizeof(*error));

    /*
     * SQL 파일을 바이너리 모드로 열어 전체 내용을 그대로 읽습니다.
     * 이 프로젝트는 파일 내용을 메모리에 한 번 올린 뒤 문자열처럼 처리합니다.
     */
    file = fopen(path, "rb");
    if (file == NULL) {
        snprintf(error->message, sizeof(error->message), "SQL 파일을 열 수 없습니다.");
        return 0;
    }

    /*
     * 버퍼 끝 1바이트는 문자열 종료 문자('\0')를 위해 비워 둡니다.
     */
    total_size = fread(buffer, 1, buffer_size - 1, file);
    if (ferror(file)) {
        fclose(file);
        snprintf(error->message, sizeof(error->message), "SQL 파일을 읽는 중 오류가 발생했습니다.");
        return 0;
    }

    /*
     * 파일이 버퍼보다 큰지 확인하기 위해 1바이트를 더 읽어 봅니다.
     * 추가로 읽히면 파일이 너무 큰 것이므로 잘린 채 실행하지 않고 실패합니다.
     * buffer 대신 probe를 사용해 이미 읽은 SQL 내용이 덮이지 않도록 합니다.
     */
    {
        char probe;
        read_size = fread(&probe, 1, 1, file);
    }
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
    /*
     * 빈 오류는 출력하지 않습니다.
     * 호출자가 "오류가 없다"는 상태를 빈 message로 표현할 수 있기 때문입니다.
     */
    if (error->message[0] == '\0') {
        return;
    }

    /*
     * line/column이 있으면 파서/실행기에서 위치를 계산해 넣은 경우이므로
     * 사용자에게 함께 보여 줍니다.
     */
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

    /*
     * 이 함수는 "SQL 문자열 1개를 실제 실행하는 공통 파이프라인"입니다.
     *
     * 흐름:
     * 1. SQL 문자열 -> TokenList
     * 2. TokenList -> SqlProgram
     * 3. SqlProgram -> execute_program
     */
    if (!tokenize_sql(sql_text, &tokens, error)) {
        return 0;
    }

    if (!parse_program(&tokens, &program, error)) {
        return 0;
    }

    return execute_program(config, &program, error);
}

int run_program(const AppConfig *config)
{
    char sql_text[SQLPROC_MAX_SQL_SIZE];
    ErrorInfo error;
    struct timeval start_time;
    struct timeval end_time;
    double total_elapsed_ms;
    int exit_code;

    exit_code = 0;
    gettimeofday(&start_time, NULL);

    /*
     * SQL 파일을 읽어 한 번 실행합니다.
     */
    if (!load_sql_file(config->input_path, sql_text, sizeof(sql_text), &error)) {
        print_error(&error);
        exit_code = 1;
    } else if (!run_sql_text(config, sql_text, &error)) {
        print_error(&error);
        exit_code = 1;
    }

    gettimeofday(&end_time, NULL);
    total_elapsed_ms = elapsed_milliseconds(&start_time, &end_time);
    fprintf(stderr, "총 실행 시간: %.3f ms\n", total_elapsed_ms);

    return exit_code;
}
