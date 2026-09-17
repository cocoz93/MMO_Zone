# EchoDummyClient

IOCP 서버 네트워크 라이브러리의 무결성을 검증하기 위한 에코 스트레스 테스트 클라이언트.

## 주요 기능

- 다수 클라이언트 동시 접속 / 고빈도 패킷 송수신 / 연결-해제 반복 테스트
- 에코 응답 무결성 검증 (순차 증가 uint64 값 비교)
- Prometheus 메트릭 엔드포인트 (RTT 히스토그램 / PPS / 에러 카운터)
- 비정상 시나리오 감지: 패킷 유실, 순서 깨짐, 서버측 연결 끊김, 타임아웃

## 상세 문서

[Notion - EchoDummy 네트워크 로직 무결성 검증](https://feline-vacation-d6d.notion.site/36216a0b9f5981f9a769d38222a820b7)
