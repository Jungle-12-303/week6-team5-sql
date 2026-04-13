# 요청 요약

- `--help` 또는 `-help` 인자를 입력하면 도움말이 출력되도록 수정
- 잘못된 실행 인자를 입력하면 오류 메시지와 사용법이 함께 출력되도록 수정

# 결정 사항

- 명령행 인자 해석 결과를 `성공 / 도움말 / 오류`로 구분하는 `ArgumentParseResult` 열거형을 추가했다.
- CLI 분기 처리는 `run_cli()`로 모아 `main.c`는 진입만 담당하도록 단순화했다.
- 도움말은 실제 실행 파일 경로(`argv[0]`)를 반영해 출력하도록 했다.

# 현재 브랜치 상태

- 브랜치: `main`
- 작업 트리: CLI 도움말 관련 파일 수정 상태

# 완료한 작업

- `parse_arguments()`가 상세 오류 메시지를 채우도록 변경
- `--help`, `-help` 입력 시 종료 코드 `0`으로 도움말 출력 추가
- 인자 개수 오류, 알 수 없는 옵션 오류에서 도움말 동시 출력 추가
- 테스트 러너에 도움말/오류 출력 검증 추가
- README에 도움말 실행 예시 추가

# 리뷰 결과

- 정확성: `--help`, `-help`, 인자 개수 오류, 알 수 없는 옵션 케이스를 확인했다.
- 자료구조 무결성: 기존 `AppConfig`, `ErrorInfo` 흐름을 유지하면서 인자 해석 결과만 별도 enum으로 분리했다.
- 초심자 가독성: `main -> run_cli -> parse_arguments/run_program` 흐름으로 단순화해 읽기 쉽게 정리했다.

# 다음 작업

- 필요하면 README의 실행 예시에 잘못된 인자 출력 예시까지 추가할 수 있다.
- feature 브랜치 전략과 GitHub 절차가 필요하면 이 변경을 기준으로 이어서 정리할 수 있다.

# 남은 리스크

- 도움말 출력은 현재 `stderr`로 통일되어 있다.
- `--help`와 다른 인자를 함께 섞어 넣는 확장 해석은 아직 지원하지 않는다.

# 검증 결과

- `make`
- `make test`
- `./build/sqlproc --help`
- `./build/sqlproc -help`
- `./build/sqlproc --schema-dir ./examples/schemas --wrong-option ./demo-data ./examples/demo.sql`
