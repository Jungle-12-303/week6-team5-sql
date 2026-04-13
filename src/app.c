#include <stdio.h>
#include <string.h>

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
static int is_help_argument(const char *arg);
static void print_usage(const char *program_name);

ArgumentParseResult parse_arguments(int argc, char **argv, AppConfig *config, ErrorInfo *error)
{
    int i;

    /*
     * 지원하는 실행 형식:
     *   ./sqlproc --schema-dir <dir> --data-dir <dir> <input.sql>
     *   -> argc == 6
     */
    /*
     * 이전 실행 값이 남지 않도록 config 전체를 0으로 초기화합니다.
     */
    memset(config, 0, sizeof(*config));
    memset(error, 0, sizeof(*error));

    if (argc == 2 && is_help_argument(argv[1])) {
        return ARGUMENT_PARSE_HELP;
    }

    if (argc != 6) {
        snprintf(error->message,
                 sizeof(error->message),
                 "실행 인자 수가 올바르지 않습니다.");
        return ARGUMENT_PARSE_ERROR;
    }

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
            snprintf(error->message,
                     sizeof(error->message),
                     "알 수 없는 실행 옵션입니다: %s",
                     argv[i]);
            return ARGUMENT_PARSE_ERROR;
        }
    }

    /*
     * schema_dir, data_dir는 항상 필요합니다.
     */
    if (config->schema_dir[0] == '\0') {
        snprintf(error->message,
                 sizeof(error->message),
                 "--schema-dir 옵션이 필요합니다.");
        return ARGUMENT_PARSE_ERROR;
    }

    if (config->data_dir[0] == '\0') {
        snprintf(error->message,
                 sizeof(error->message),
                 "--data-dir 옵션이 필요합니다.");
        return ARGUMENT_PARSE_ERROR;
    }

    snprintf(config->input_path, sizeof(config->input_path), "%s", argv[argc - 1]);
    return ARGUMENT_PARSE_OK;
}

int load_sql_file(const char *path, char *buffer, size_t buffer_size, ErrorInfo *error)
{
    FILE *file;
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

    if (total_size == buffer_size - 1) {
        char probe;
        size_t extra_size;

        /*
         * 버퍼가 가득 찼을 때만 1바이트를 더 읽어 실제로 더 큰 파일인지 확인합니다.
         * 한 번 더 읽혔다면 잘린 내용을 실행하지 않고 실패합니다.
         */
        extra_size = fread(&probe, 1, 1, file);
        if (extra_size > 0) {
            fclose(file);
            snprintf(error->message, sizeof(error->message), "SQL 파일이 너무 큽니다.");
            return 0;
        }
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

int run_cli(int argc, char **argv)
{
    AppConfig config;
    ErrorInfo error;
    ArgumentParseResult parse_result;

    parse_result = parse_arguments(argc, argv, &config, &error);
    if (parse_result == ARGUMENT_PARSE_HELP) {
        print_usage(argv[0]);
        return 0;
    }

    if (parse_result == ARGUMENT_PARSE_ERROR) {
        print_error(&error);
        print_usage(argv[0]);
        return 1;
    }

    return run_program(&config);
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

static int is_help_argument(const char *arg)
{
    return strcmp(arg, "--help") == 0 || strcmp(arg, "-help") == 0;
}

static void print_usage(const char *program_name)
{
    const char *display_name;

    display_name = program_name;
    if (display_name == NULL || display_name[0] == '\0') {
        display_name = "./sqlproc";
    }

    fprintf(stderr,
            "usage: %s --schema-dir <dir> --data-dir <dir> <input.sql>\n"
            "help : %s --help | %s -help\n",
            display_name,
            display_name,
            display_name);
}

int run_program(const AppConfig *config)
{
    char sql_text[SQLPROC_MAX_SQL_SIZE];
    ErrorInfo error;

    /*
     * SQL 파일을 읽어 한 번 실행합니다.
     */
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
