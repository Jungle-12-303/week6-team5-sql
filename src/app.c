#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sqlproc.h"

/*
 * 이 파일은 프로그램의 "실행 제어"를 담당합니다.
 * - 명령행 인자를 읽어 AppConfig를 채우고
 * - SQL 파일 실행 모드와 CLI 입력(REPL) 모드를 나누고
 * - SQL 문자열을 토크나이저 -> 파서 -> 실행기로 넘기고
 * - 사용자에게 오류를 출력합니다.
 *
 * 즉 main.c와 실제 SQL 엔진 사이를 연결하는 중간 계층입니다.
 */

static int run_sql_text(const AppConfig *config, const char *sql_text, ErrorInfo *error);
static int run_interactive_mode(const AppConfig *config);
static int ends_with_semicolon(const char *text);
static void trim_copy(char *dest, size_t dest_size, const char *src);
static int is_exit_command(const char *text);

int parse_arguments(int argc, char **argv, AppConfig *config)
{
    int i;
    int option_limit;

    /*
     * 지원하는 실행 형식은 두 가지입니다.
     *
     * 1. SQL 파일 실행 모드
     *    ./sqlproc --schema-dir <dir> --data-dir <dir> --index-dir <dir> <input.sql>
     *    -> argc == 8
     *
     * 2. 대화형 CLI 입력 모드
     *    ./sqlproc --schema-dir <dir> --data-dir <dir> --index-dir <dir>
     *    -> argc == 7
     */
    if (argc != 7 && argc != 8) {
        return 0;
    }

    /*
     * 이전 실행 값이 남지 않도록 config 전체를 0으로 초기화합니다.
     * 문자열 버퍼는 빈 문자열이 되고, has_input_path도 0으로 시작합니다.
     */
    memset(config, 0, sizeof(*config));
    option_limit = argc;
    if (argc == 8) {
        /*
         * 마지막 인자를 SQL 파일 경로로 해석해야 하므로
         * 옵션 파싱 루프에서는 마지막 인자를 제외합니다.
         */
        option_limit = argc - 1;
        config->has_input_path = 1;
    }

    /*
     * argv는 "--옵션 값" 쌍으로 들어오므로 2칸씩 전진합니다.
     * 예:
     * - argv[1] = --schema-dir
     * - argv[2] = ./examples/schemas
     */
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

    /*
     * 세 디렉터리는 항상 필요합니다.
     * - schema_dir: <table>.schema 를 읽는 위치
     * - data_dir: <table>.csv 를 저장/조회하는 위치
     * - index_dir: <index>.idx 를 저장/조회하는 위치
     */
    if (config->schema_dir[0] == '\0' ||
        config->data_dir[0] == '\0' ||
        config->index_dir[0] == '\0') {
        return 0;
    }

    if (config->has_input_path) {
        /*
         * SQL 파일 실행 모드일 때만 마지막 argv를 input_path에 저장합니다.
         * REPL 모드에서는 input_path를 비워 둡니다.
         */
        snprintf(config->input_path, sizeof(config->input_path), "%s", argv[argc - 1]);
    }

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
     */
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
     * 파일 모드와 REPL 모드가 모두 결국 여기로 들어옵니다.
     *
     * 흐름:
     * 1. SQL 문자열 -> TokenList
     * 2. TokenList -> SqlProgram(AST)
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

static int ends_with_semicolon(const char *text)
{
    size_t length;

    /*
     * REPL에서 문장이 끝났는지 확인하는 함수입니다.
     * 마지막 의미 있는 문자가 ';' 인지 검사합니다.
     * 뒤쪽 공백/개행/탭은 무시합니다.
     */
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

    /*
     * 문자열 앞뒤 공백을 제거한 결과를 dest에 복사합니다.
     * REPL에서 " quit ", "exit;" 같은 종료 명령을 안정적으로 비교하려고 사용합니다.
     */
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

    /*
     * SQL 실행이 아니라 REPL 종료를 의미하는 입력인지 확인합니다.
     * 지원 값:
     * - exit
     * - quit
     * - exit;
     * - quit;
     */
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

    /*
     * line:
     * - 사용자가 한 번 입력한 한 줄을 임시로 담습니다.
     *
     * sql_buffer:
     * - 여러 줄 입력을 이어 붙여 하나의 완성된 SQL 문장으로 만드는 버퍼입니다.
     * - 세미콜론이 나올 때까지 여기에 누적합니다.
     */
    sql_buffer[0] = '\0';

    /*
     * 표준 입력이 실제 터미널일 때만 프롬프트를 보여 줍니다.
     * 파이프/리다이렉션 입력일 때는 프롬프트를 숨겨 출력이 깔끔하게 유지되게 합니다.
     *
     * isatty(fd)는 컴퓨터에게
     * "지금 입력이 사람이 키보드로 직접 치는 터미널 화면에서 오고 있니?"
     * 하고 물어보는 함수입니다.
     *
     * 쉽게 말하면:
     * - 1이 나오면:
     *   사람이 화면에서 직접 입력 중이라는 뜻입니다.
     *   그래서 "sqlproc> " 같은 안내 문구를 보여 줘도 됩니다.
     *
     * - 0이 나오면:
     *   입력이 파일이나 파이프에서 들어오고 있다는 뜻입니다.
     *   이때는 안내 문구를 출력하면 결과가 지저분해질 수 있으므로 숨깁니다.
     *
     * 여기서 STDIN_FILENO는 표준 입력(stdin)의 파일 디스크립터 번호입니다.
     * 보통 "키보드로 들어오는 입력 통로"라고 생각하면 됩니다.
     *
     * 예시 1:
     *   ./build/sqlproc --schema-dir ... --data-dir ... --index-dir ...
     *   -> 사용자가 직접 키보드로 입력
     *   -> isatty(...) == 1
     *   -> sqlproc> 프롬프트를 보여 줌
     *
     * 예시 2:
     *   printf "SELECT * FROM users;\n" | ./build/sqlproc --schema-dir ...
     *   -> 입력이 키보드가 아니라 printf 결과에서 들어옴
     *   -> isatty(...) == 0
     *   -> 프롬프트를 숨김
     */
    show_prompt = isatty(STDIN_FILENO);

    while (1) {
        ErrorInfo error;
        size_t current_length;
        size_t line_length;

        /*
         * 새 문장을 시작할 때는 sqlproc> 프롬프트,
         * 이전 줄에 이어 입력 중이면 ...> 프롬프트를 보여 줍니다.
         *
         * fflush(stdout)는 "화면에 보여 주기로 한 글자를 바로 지금 내보내라"는 뜻입니다.
         *
         * 쉽게 말하면:
         * - printf("sqlproc> "); 만 하면
         *   컴퓨터가 "이 글자는 잠깐 가지고 있다가 나중에 보여 줘야지" 하고
         *   바로 안 보여 줄 때가 있습니다.
         *
         * - fflush(stdout); 를 하면
         *   "아니, 나중 말고 지금 바로 화면에 보여 줘!" 라고 시키는 것입니다.
         *
         * 왜 필요하냐면,
         * 사용자가 입력하기 전에 "sqlproc> " 안내 문구가 먼저 보여야 하기 때문입니다.
         * 이 줄이 없으면 어떤 환경에서는 프롬프트가 늦게 보일 수 있습니다.
         *
         * 예시:
         * - printf("sqlproc> ");
         * - fflush(stdout);
         * - 그 다음 사용자가 SQL 입력
         */
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

        /*
         * 버퍼가 비어 있는 상태에서 quit/exit가 들어오면
         * SQL로 처리하지 않고 바로 프로그램을 종료합니다.
         */
        if (sql_buffer[0] == '\0' && is_exit_command(line)) {
            return 0;
        }

        current_length = strlen(sql_buffer);
        line_length = strlen(line);

        /*
         * 현재 누적 버퍼 + 새 입력 줄이 최대 SQL 버퍼 크기를 넘는지 확인합니다.
         * 넘으면 지금까지 입력은 버리고 다음 입력부터 다시 받습니다.
         */
        if (current_length + line_length >= sizeof(sql_buffer)) {
            fprintf(stderr, "오류: 입력 SQL이 너무 깁니다.\n");
            sql_buffer[0] = '\0';
            continue;
        }

        /*
         * 새 입력 줄을 sql_buffer 뒤에 이어 붙입니다.
         * 아직 세미콜론이 없으면 문장이 끝나지 않은 것이므로 계속 입력을 받습니다.
         */
        memcpy(sql_buffer + current_length, line, line_length + 1);

        if (!ends_with_semicolon(sql_buffer)) {
            continue;
        }

        /*
         * 이제 sql_buffer 안에는 세미콜론까지 포함된 "완성된 SQL 문장 1개"가 들어 있습니다.
         * 그래서 이 문장을 실제 SQL 엔진으로 보내 실행합니다.
         *
         * 먼저 memset(&error, 0, sizeof(error)); 로 error 구조체를 깨끗하게 비웁니다.
         * 이유:
         * - 바로 전에 다른 SQL 문장에서 오류가 났을 수도 있고
         * - 그 이전 오류 메시지가 남아 있으면 지금 문장 결과와 섞일 수 있기 때문입니다.
         *
         * 그 다음 run_sql_text(...)를 호출합니다.
         * 이 함수 안에서는 아래 순서가 실행됩니다.
         * 1. 문자열을 토큰으로 자름
         * 2. 토큰을 문장 구조(AST)로 바꿈
         * 3. 실제 INSERT / SELECT / CREATE INDEX 실행
         *
         * 만약 실행에 실패하면:
         * - !run_sql_text(...) 가 참이 됩니다.
         * - print_error(&error); 로 왜 실패했는지 화면에 알려 줍니다.
         * - sql_buffer[0] = '\0'; 로 지금까지 모은 SQL 문장을 지웁니다.
         *   왜냐하면 잘못된 문장을 버리고 새 문장부터 다시 받아야 하기 때문입니다.
         * - continue; 로 while 처음으로 돌아가 다음 입력을 기다립니다.
         *
         * 중요한 점:
         * - SQL 문장 하나가 실패해도 프로그램 전체는 꺼지지 않습니다.
         * - REPL은 계속 살아 있어서 사용자가 다음 SQL을 다시 입력할 수 있습니다.
         *
         * 예시:
         * - 사용자가 잘못된 SQL을 입력함
         *   SELECT FROM users;
         * - run_sql_text(...) 실패
         * - "오류: ..." 출력
         * - 프로그램 종료 안 함
         * - 다시 sqlproc> 프롬프트를 보여 줌
         */
        memset(&error, 0, sizeof(error));
        if (!run_sql_text(config, sql_buffer, &error)) {
            print_error(&error);
            sql_buffer[0] = '\0';
            continue;
        }

        /*
         * 성공적으로 한 문장을 실행했으므로 버퍼를 비우고 다음 문장을 받습니다.
         */
        sql_buffer[0] = '\0';
    }

    /*
     * EOF로 입력이 끝났는데 버퍼 안에 세미콜론 없는 미완성 문장이 남아 있으면
     * 조용히 무시하지 않고 오류로 처리합니다.
     */
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

    /*
     * 이 함수는 최종 실행 모드를 결정합니다.
     *
     * - has_input_path == 0:
     *   SQL 파일 없이 실행된 상태이므로 CLI 입력(REPL) 모드로 들어갑니다.
     *
     * - has_input_path == 1:
     *   input_path에 지정된 SQL 파일을 읽어 한 번 실행합니다.
     */
    if (!config->has_input_path) {
        return run_interactive_mode(config);
    }

    /*
     * 파일 실행 모드에서는:
     * 1. SQL 파일을 문자열로 읽고
     * 2. run_sql_text로 실행하고
     * 3. 실패 시 에러 메시지를 출력합니다.
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
