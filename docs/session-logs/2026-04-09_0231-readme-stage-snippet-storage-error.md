# 요청 요약

- `src` 코드 기준으로 단계별 예시가 맞는지 확인하고 README를 수정한다.
- 시연 예시 아래에 실제 데모 SQL 스니펫을 추가한다.
- 우리 팀의 포인트에 `storage.c` 오류 처리 예시를 추가한다.

# 결정 사항

- 단계별 예시는 `main/app/tokenizer/parser/executor/storage`의 현재 함수 흐름에 맞춘다.
- 시연 예시는 이미지 아래에 SQL 코드 블록을 함께 배치한다.
- 오류 처리 포인트는 tokenizer, parser, executor에 이어 storage까지 분리해 설명한다.

# 현재 브랜치 상태

- `simplify` 브랜치에서 작업했다.

# 완료한 작업

- `README.md`의 executor 단계 예시를 `load_table_schema() -> build_insert_row_values() -> storage_append_row()` 기준으로 수정했다.
- 시연 예시 아래에 요청한 `INSERT` / `SELECT` SQL 스니펫을 추가했다.
- `우리 팀의 포인트`에 `storage.c`의 파일/CSV 오류 처리 메시지 표와 예시를 추가했다.

# 리뷰 결과

- 단계별 예시가 실제 `src` 코드 흐름과 더 직접적으로 맞아졌다.
- 발표에서 정상 흐름과 스토리지 오류 흐름을 함께 설명하기 쉬워졌다.

# 다음 작업

- 필요하면 storage 오류 예시도 실제 캡처 이미지로 따로 준비한다.

# 남은 리스크

- README의 시각 자료는 코드 흐름을 요약한 그림이라 함수 내부 모든 분기까지는 보여 주지 않는다.
