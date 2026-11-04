#pragma once
// ==========================================================================
// CAuthTokenLedger — 로그인 토큰 장부: 만들기 · 확인 · 만료 정리
//
//   토큰은 일회용이다. 확인하면 지운다 — 이미 쓴 토큰은 NotFound 다.
//   만료는 이 서버의 시계로만 판정한다 — 서버 사이에 시각을 비교할 일이 없다.
//   만료된 항목은 60초 더 보관해 그동안 Expired 로 답한다. 바로 지우면 NotFound 가 되어
//   「너무 늦었다」와 「위조됐다」를 지표로 못 가른다. mmo_auth_token_total{result="expired"} 가
//   2분을 5분으로 올릴 근거라 이 구분이 필요하다(플랜 4절).
//   재시작하면 장부가 사라져 NotFound 가 되고, 웹이 새 토큰을 받는다.
//
//   로직 스레드 전용이라 락이 없다. 시각은 인자로 받는다 — 시험이 2분을 안 기다린다.
// ==========================================================================

#include <cstdint>
#include <string>
#include <unordered_map>

#include <Identity/GameIds.h>
#include <Protocol/ServerLinkProtocol.h>

class CAuthTokenLedger
{
public:
    static constexpr int TOKEN_BYTES = LINK_CODE_BYTES;
    static constexpr int EXPIRED_KEEP_SEC = 60;

    explicit CAuthTokenLedger(int ttlSec) : _ttlMs(int64_t{ ttlSec } * 1000) {}

    // 새 토큰을 out 에 쓴다. 계정당 개수를 안 막는다 — 창을 둘 띄워도 각자 받는다
    void Issue(AccountId accountId, int64_t nowMs, char (&out)[TOKEN_BYTES]);

    // Ok 이면 outAccount 에 계정 번호를 담고 장부에서 지운다
    TokenCheckResult Consume(const char (&token)[TOKEN_BYTES], int64_t nowMs, AccountId& outAccount);

    // 만료 표시 · 보관이 끝난 항목 지우기 — 주기 작업
    void Sweep(int64_t nowMs);

    int LiveCount() const { return _liveCount; }                       // 아직 쓸 수 있는 토큰 수 — mmo_auth_token_pending
    int StoredCount() const { return static_cast<int>(_entries.size()); }   // 보관 중까지 — 분모

private:
    struct Entry
    {
        AccountId accountId = NO_ACCOUNT;
        int64_t   expireMs = 0;
        bool      live = true;
    };

    std::unordered_map<std::string, Entry> _entries;
    int64_t _ttlMs;
    int     _liveCount = 0;
};
