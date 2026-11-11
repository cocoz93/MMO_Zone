#pragma once
// ==========================================================================
// SessionTag — 코어 세션의 구분 값 이름 (CIOCPServer::AddListener · ConnectTo 의 tag)
//
//   값이 곧 코어의 정수다. 코어는 뜻을 모르고 이벤트에 실어 나를 뿐이다.
// ==========================================================================

enum class SessionTag : int
{
    Web = 0,        // 코어 기본 — 웹·콘솔·더미가 들어오는 포트
    LinkUp = 1,     // 내가 위층에 접속한 연결
    LinkDown = 2,   // 아래층이 내게 접속한 연결
    LinkUpWorld = 3,   // 존 서버가 월드 서버에 접속한 연결 — 존 서버는 위층이 둘(캐릭터 · 월드)이라 값으로 가른다
};

inline constexpr int SESSION_TAG_COUNT = 4;
