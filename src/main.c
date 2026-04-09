#include <stdio.h>

#include "sqlproc.h"

/*
 * 사용법 문자열에서 각 인자가 뜻하는 것:
 * - --schema-dir <dir>:
 *   테이블 스키마 파일(.schema)들이 들어 있는 디렉터리 경로입니다.
 *   예: users 테이블은 <dir>/users.schema 파일로 읽습니다.
 *
 * - --data-dir <dir>:
 *   실제 CSV 데이터 파일들이 저장되는 디렉터리 경로입니다.
 *   예: users 테이블은 <dir>/users.csv 파일로 저장/조회합니다.
 *
 * - <input.sql>:
 *   실행할 SQL 파일 경로입니다.
 *   이 프로젝트는 SQL 파일 실행 모드만 지원합니다.
 */
static void print_usage(void)
{
    fprintf(stderr,
            "usage: ./sqlproc --schema-dir <dir> --data-dir <dir> <input.sql>\n");
}

int main(int argc, char **argv)
{
    /*
     * AppConfig는 명령행 인자를 해석한 결과를 담는 실행 설정 구조체입니다.
     * - schema_dir: 스키마 디렉터리
     * - data_dir: 데이터 CSV 디렉터리
     * - input_path: 실행할 SQL 파일 경로
     */
    AppConfig config;

    /*
     * argc / argv는 main 함수가 운영체제로부터 받는 기본 명령행 인자입니다.
     * - argc: 인자 개수
     * - argv: 각 인자 문자열 배열
     *
     * 예를 들어 아래처럼 실행하면:
     *   ./build/sqlproc --schema-dir ./examples/schemas \
     *     --data-dir ./demo-data ./examples/demo.sql
     *
     * argv는 대략 아래 순서가 됩니다.
     * - argv[0]: ./build/sqlproc
     * - argv[1]: --schema-dir
     * - argv[2]: ./examples/schemas
     * - argv[3]: --data-dir
     * - argv[4]: ./demo-data
     * - argv[5]: ./examples/demo.sql
     *
     * parse_arguments는 이 값을 읽어 config 구조체로 정리합니다.
     */
    if (!parse_arguments(argc, argv, &config)) {
        /*
         * 인자 형식이 잘못되면 실제 실행을 진행하지 않고
         * 사용법 문자열만 stderr로 출력한 뒤 종료합니다.
         */
        print_usage();
        return 1;
    }

    /* 인자 해석이 끝난 뒤의 실제 실행은 run_program이 담당합니다. */
    return run_program(&config);
}
