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

/* 커맨드라인 인수를 파싱하여 AppConfig를 채운다.
 * 지원 형식: --schema-dir, --data-dir, --index-dir, [input.sql]
 *
 * @param argc    커맨드라인 인수 개수
 * @param argv    커맨드라인 인수 배열
 * @param config  결과를 저장할 AppConfig 포인터
 * @return        성공 시 1, 인수 형식이 잘못되면 0
 */
int parse_arguments(int argc, char **argv, AppConfig *config)
{
    if (argc != 7 && argc != 8) {
        return 0;
    }

    memset(config, 0, sizeof(*config));

    int option_limit = argc;
    if (argc == 8) {
        option_limit = argc - 1;
        config->has_input_path = 1;
    }

    for (int i = 1; i < option_limit; i += 2) {
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

/* SQL 파일을 읽어 buffer에 저장한다.
 * 파일이 buffer_size - 1 바이트를 초과하면 오류를 반환한다.
 *
 * @param path         읽을 SQL 파일 경로
 * @param buffer       내용을 저장할 버퍼
 * @param buffer_size  버퍼 크기
 * @param error        오류 정보 저장 포인터
 * @return             성공 시 1, 실패 시 0
 */
int load_sql_file(const char *path, char *buffer, size_t buffer_size, ErrorInfo *error)
{
    memset(error, 0, sizeof(*error));

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        snprintf(error->message, sizeof(error->message), "SQL 파일을 열 수 없습니다.");
        return 0;
    }

    size_t total_size = fread(buffer, 1, buffer_size - 1, file);
    if (ferror(file)) {
        fclose(file);
        snprintf(error->message, sizeof(error->message), "SQL 파일을 읽는 중 오류가 발생했습니다.");
        return 0;
    }

    size_t read_size = fread(buffer, 1, 1, file);
    if (read_size > 0) {
        fclose(file);
        snprintf(error->message, sizeof(error->message), "SQL 파일이 너무 큽니다.");
        return 0;
    }

    fclose(file);
    buffer[total_size] = '\0';
    return 1;
}

/* ErrorInfo의 오류 메시지와 위치 정보를 stderr에 출력한다.
 * message가 비어 있으면 아무것도 출력하지 않는다.
 *
 * @param error  출력할 오류 정보 포인터
 */
void print_error(const ErrorInfo *error)
{
    if (error->message[0] == '\0') {
        return;
    }
    if (error->line > 0) {
        fprintf(stderr, "오류: %s (line %d, column %d)\n",
                error->message, error->line, error->column);
        return;
    }
    fprintf(stderr, "오류: %s\n", error->message);
}

/* SQL 문자열을 토크나이징·파싱·실행하는 공통 파이프라인을 수행한다.
 *
 * @param config    실행 설정 구조체 포인터
 * @param sql_text  실행할 SQL 문자열
 * @param error     오류 정보 저장 포인터
 * @return          성공 시 1, 실패 시 0
 */
static int run_sql_text(const AppConfig *config, const char *sql_text, ErrorInfo *error)
{
    TokenList tokens;
    SqlProgram program;

    if (!tokenize_sql(sql_text, &tokens, error)) return 0;
    if (!parse_program(&tokens, &program, error)) return 0;
    return execute_program(config, &program, error);
}

/* 문자열의 마지막 의미 있는 문자(공백/개행 제외)가 세미콜론인지 확인한다.
 *
 * @param text  검사할 문자열
 * @return      세미콜론으로 끝나면 1, 아니면 0
 */
static int ends_with_semicolon(const char *text)
{
    size_t length = strlen(text);
    while (length > 0) {
        char ch = text[length - 1];
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
            length -= 1;
            continue;
        }
        return ch == ';';
    }
    return 0;
}

/* 문자열 앞뒤 공백·개행·탭을 제거한 결과를 dest에 복사한다.
 *
 * @param dest       결과를 저장할 버퍼
 * @param dest_size  dest 버퍼 크기
 * @param src        원본 문자열
 */
static void trim_copy(char *dest, size_t dest_size, const char *src)
{
    size_t start = 0;
    while (src[start] == ' ' || src[start] == '\n' || src[start] == '\r' || src[start] == '\t') {
        start += 1;
    }

    size_t end = strlen(src);
    while (end > start) {
        char ch = src[end - 1];
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
            end -= 1;
            continue;
        }
        break;
    }

    size_t length = end - start;
    if (length >= dest_size) {
        length = dest_size - 1;
    }
    memcpy(dest, src + start, length);
    dest[length] = '\0';
}

/* 입력 문자열이 REPL 종료 명령(exit, quit, exit;, quit;)인지 확인한다.
 *
 * @param text  검사할 입력 문자열
 * @return      종료 명령이면 1, 아니면 0
 */
static int is_exit_command(const char *text)
{
    char trimmed[64];
    trim_copy(trimmed, sizeof(trimmed), text);
    if (strcmp(trimmed, "exit") == 0  || strcmp(trimmed, "quit") == 0)  return 1;
    if (strcmp(trimmed, "exit;") == 0 || strcmp(trimmed, "quit;") == 0) return 1;
    return 0;
}

/* 터미널에서 SQL 입력을 반복 수신하여 실행하는 대화형 모드를 실행한다.
 * 세미콜론이 나올 때까지 여러 줄 입력을 누적한 뒤 한 번에 실행한다.
 *
 * @param config  실행 설정 구조체 포인터
 * @return        성공 시 0, 오류 시 1
 */
static int run_interactive_mode(const AppConfig *config)
{
    char sql_buffer[SQLPROC_MAX_SQL_SIZE];
    sql_buffer[0] = '\0';

    int show_prompt = isatty(STDIN_FILENO);

    char line[1024];
    while (1) {
        if (show_prompt) {
            printf(sql_buffer[0] == '\0' ? "sqlproc> " : "...> ");
            fflush(stdout);
        }

        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;
        }

        if (sql_buffer[0] == '\0' && is_exit_command(line)) {
            return 0;
        }

        size_t current_length = strlen(sql_buffer);
        size_t line_length = strlen(line);

        if (current_length + line_length >= sizeof(sql_buffer)) {
            fprintf(stderr, "오류: 입력 SQL이 너무 깁니다.\n");
            sql_buffer[0] = '\0';
            continue;
        }

        memcpy(sql_buffer + current_length, line, line_length + 1);

        if (!ends_with_semicolon(sql_buffer)) {
            continue;
        }

        ErrorInfo error;
        memset(&error, 0, sizeof(error));
        if (!run_sql_text(config, sql_buffer, &error)) {
            print_error(&error);
        }
        sql_buffer[0] = '\0';
    }

    if (sql_buffer[0] != '\0') {
        fprintf(stderr, "오류: 문장 끝에는 세미콜론이 필요합니다.\n");
        return 1;
    }

    return 0;
}

/* 설정에 따라 파일 실행 모드 또는 대화형 모드로 프로그램을 실행한다.
 *
 * @param config  실행 설정 구조체 포인터
 * @return        성공 시 0, 오류 시 1
 */
int run_program(const AppConfig *config)
{
    if (!config->has_input_path) {
        return run_interactive_mode(config);
    }

    char sql_text[SQLPROC_MAX_SQL_SIZE];
    ErrorInfo error;

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
