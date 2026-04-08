# 요청 요약

- README의 오류 처리 섹션에 executor 오류도 추가한다.
- 시연 예시 이미지를 제공된 실제 PNG 스크린샷으로 교체한다.

# 결정 사항

- 오류 처리 분류는 tokenizer, parser, executor 3단계로 정리한다.
- 시연 이미지는 `docs/images/demo-run.png`로 저장해 README에서 직접 참조한다.

# 현재 브랜치 상태

- `simplify` 브랜치에서 작업했다.

# 완료한 작업

- `README.md`의 오류 처리 섹션 제목을 `tokenizer, parser, executor의 오류를 나눠서 처리`로 수정했다.
- executor 대표 오류 메시지 2개를 표에 추가했다.
- 오류 처리 다이어그램과 예시 목록에 executor 오류를 반영했다.
- 제공된 PNG 스크린샷을 `docs/images/demo-run.png`로 복사하고 시연 예시에 연결했다.

# 리뷰 결과

- 오류 처리 설명이 실제 실행 단계까지 확장되어 더 완결된 발표 흐름이 됐다.
- 시연 예시가 재구성 SVG가 아니라 실제 캡처 이미지로 바뀌어 설득력이 좋아졌다.

# 다음 작업

- 필요하면 불필요해진 `docs/images/demo-run.svg`를 정리한다.

# 남은 리스크

- 실제 PNG 파일 크기가 커서 README 렌더링 환경에 따라 로딩이 느릴 수 있다.
