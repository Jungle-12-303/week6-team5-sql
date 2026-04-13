#include "sqlproc.h"

int main(int argc, char **argv)
{
    /*
     * main은 운영체제에서 받은 명령행 인자를 그대로 run_cli에 넘깁니다.
     * 실제 도움말 출력, 오류 메시지 처리, SQL 실행 진입은 app.c가 담당합니다.
     */
    return run_cli(argc, argv);
}
