#pragma once
// ==========================================================================
// CCharSessionTable — 세션과 계정 묶기 · 로그인 단계 · 처리 중 요청 · 같은 계정의 옛 세션 찾기
//
//   세션 번호로 찾는 맵이라 코어 세션 표 크기에 묶이지 않는다. 로직 스레드 전용 · 네트워크 모름.
//   한 계정 한 창 — 새 로그인이 옛 세션을 끊는다(끊기는 부르는 쪽이 한다).
//   계정당 처리 중 요청은 하나뿐이다(BUSY) — 한 계정의 만들기 · 삭제 · 고르기가 엇갈리지 않게.
// ==========================================================================

#include <cstddef>
#include <unordered_map>

#include <Identity/GameIds.h>

class CCharSessionTable
{
public:
    enum class Stage
    {
        Fresh,      // 접속만 함 · 로그인 실패 뒤도 여기로 돌아온다(웹이 새 토큰으로 다시 낸다)
        Verifying,  // 토큰 확인 · 목록 읽는 중
        LoggedIn,
        Replaced,   // 같은 계정의 새 로그인에 밀려 끊기는 중 — 보낸 패킷을 조용히 버린다
    };

    struct Entry
    {
        Stage     stage = Stage::Fresh;
        AccountId account = NO_ACCOUNT;
        bool      busy = false;
    };

    void Add(SessionId sessionId);
    void Remove(SessionId sessionId);

    Entry* Find(SessionId sessionId);

    // Fresh → Verifying. 그 밖의 단계면 false — 로그인을 두 번 낸 것이다
    bool BeginLogin(SessionId sessionId);

    // Verifying → Fresh
    void FailLogin(SessionId sessionId);

    // Verifying → LoggedIn. 같은 계정의 옛 세션을 Replaced 로 돌리고 돌려준다(없으면 NO_SESSION)
    SessionId CompleteLogin(SessionId sessionId, AccountId accountId);

    // 처리 중 표시. 이미 처리 중이면 false — BUSY 로 답한다
    bool BeginRequest(SessionId sessionId);
    void EndRequest(SessionId sessionId);

    size_t Count() const { return _entries.size(); }
    size_t AccountCount() const { return _byAccount.size(); }

private:
    std::unordered_map<int64_t, Entry> _entries;
    std::unordered_map<int64_t, SessionId> _byAccount;
};
