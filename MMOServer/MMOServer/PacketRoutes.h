#pragma once

// ==========================================================================
// CPacketRoutes — 받은 패킷의 분기 표: 패킷 번호 → (최소 크기, 처리기, 플레이어 찾기 전/후)
//
// [책임] 받은 C2S 를 어디로 보낼지와, 몇 바이트 이상이어야 받는지를 한 줄에 같이 든다.
//        지금까지는 크기 표(GetExpectedSize 의 switch)와 분기(OnReceived 의 switch)가 따로 있어,
//        하나만 고치면 패킷이 조용히 버려졌다(크기 줄을 빠뜨린 함정 — 답도 로그도 없다). 한 줄이면 둘이 어긋날 수 없다.
// [쓰는 법] 게임 서버가 Init 에서 등록한다(게임 서버 나누기 0-④). 모듈을 떼어 낸 뒤에는 그 모듈이 자기 줄을 등록한다.
//           등록이 실패하면(범위 밖 · 두 번 등록) false — 부르는 쪽은 기동을 거절한다.
// [경우 셋] ⑴ 플레이어를 찾은 뒤 처리(거의 전부) ⑵ 찾기 전 처리(입장 요청 — 그때는 아직 플레이어가 없다)
//           ⑶ 크기만(하트비트 · 로그인 끈 빌드의 입장 요청) — 처리기 없이 받아 넘긴다.
//           등록 안 된 번호 · 크기 미달은 부르는 쪽이 버리고 오류로 센다.
// [모르는 것] 세션·플레이어 찾기·버퍼 해제는 부르는 쪽(CGameServer::OnReceived) 몫이다.
// ==========================================================================

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

#include <Protocol/Protocol.h>

class CPlayer;
class CSerialBuffer;

class CPacketRoutes
{
public:
    using PlayerHandler  = std::function<void(CPlayer*, CSerialBuffer*)>;
    using SessionHandler = std::function<void(int64_t, CSerialBuffer*)>;

    struct Route
    {
        bool           registered = false;
        uint16_t       minSize    = 0;   // 이보다 작으면 버린다(가변 길이는 최소 길이)
        PlayerHandler  onPlayer;         // ⑴ 플레이어를 찾은 뒤
        SessionHandler onSession;        // ⑵ 찾기 전 — 있으면 ⑴ 보다 먼저, 플레이어를 안 찾는다
    };

    // ⑶ 크기만 — 처리기 없이 받아 넘긴다
    bool Size(MsgType type, uint16_t minSize)                        { return Put(type, minSize, nullptr, nullptr); }
    // ⑴ 플레이어를 찾은 뒤 처리
    bool OnPlayer(MsgType type, uint16_t minSize, PlayerHandler h)   { return Put(type, minSize, std::move(h), nullptr); }
    // ⑵ 플레이어를 찾기 전 처리
    bool OnSession(MsgType type, uint16_t minSize, SessionHandler h) { return Put(type, minSize, nullptr, std::move(h)); }

    // 등록 안 됐으면 nullptr
    const Route* Find(MsgType type) const
    {
        const size_t i = IndexOf(type);
        return (i < COUNT && _routes[i].registered) ? &_routes[i] : nullptr;
    }

private:
    // 패킷 번호는 C2S_MOVE_START(1000)부터 붙어 있다 — 그 뒤 256 칸을 평평한 배열로 든다(지금 마지막 번호 1060).
    //   번호가 BASE + COUNT 를 넘는 새 타입을 등록하면 Put 이 false 를 돌려 기동이 거절된다(조용히 버리지 않는다).
    static constexpr uint16_t BASE  = static_cast<uint16_t>(MsgType::C2S_MOVE_START);
    static constexpr size_t   COUNT = 256;

    static size_t IndexOf(MsgType type)
    {
        const uint16_t v = static_cast<uint16_t>(type);
        return v >= BASE ? static_cast<size_t>(v - BASE) : COUNT;
    }

    bool Put(MsgType type, uint16_t minSize, PlayerHandler onPlayer, SessionHandler onSession)
    {
        const size_t i = IndexOf(type);
        if (i >= COUNT || _routes[i].registered || minSize < sizeof(MsgHeader))
            return false;
        Route& r     = _routes[i];
        r.registered = true;
        r.minSize    = minSize;
        r.onPlayer   = std::move(onPlayer);
        r.onSession  = std::move(onSession);
        return true;
    }

    std::array<Route, COUNT> _routes{};
};
