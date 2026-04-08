# 요청 요약

- README 실행 흐름 섹션의 단계별 예시 이미지를 발표 대본 기준으로 더 구체적으로 조정

# 결정 사항

- 단계별 예시 이미지는 `tokenizer`, `parser-SELECT`, `executor/storage-SELECT`, `parser-INSERT`, `executor/storage-INSERT` 5개로 유지
- 각 다이어그램에 실제 구조체 이름과 함수 흐름을 더 분명하게 표시

# 현재 브랜치 상태

- `simplify`

# 완료한 작업

- `README.md` 실행 흐름 단계별 예시 이미지 제목 정리
- `parser.c` 예시에 `Statement -> SelectStatement`, `Statement -> InsertStatement` 흐름 반영
- `executor.c + storage.c` 예시에 `users.schema` 확인 단계 추가
- INSERT 예시에 스키마 순서 재배치와 최종 CSV 저장 결과를 더 명확히 표시

# 리뷰 결과

- 문서 수정만 진행

# 다음 작업

- 필요하면 다이어그램 문구를 더 짧은 발표 버전으로 축약

# 남은 리스크

- mermaid 줄바꿈 표현은 렌더러에 따라 보이는 높이가 조금 달라질 수 있음
