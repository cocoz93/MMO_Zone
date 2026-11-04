#pragma once
// ==========================================================================
// CPacketTable — 패킷 번호 → 최소 크기 · 처리 함수
//
//   CPacketRoutes 는 CPlayer 와 존 번호대에 묶여 있어(PacketRoutes.h) 따로 둔다.
//   표마다 자기 번호대를 갖는다 — 계정 2000~ · 캐릭터 2100~ · 서버 간 3000~. 잘못 온 패킷이 다른 표의 번호와 우연히 맞지 않게 하기 위함이다.
//   같은 번호를 두 번 등록하거나 범위 밖이면 Register 가 false 를 돌리고 서버가 뜨지 않는다.
//   버퍼 반납은 부르는 쪽(고리)이 맡는다 — 처리 함수는 SubRef 를 부르지 않는다.
// ==========================================================================

#include <cstdint>
#include <functional>
#include <vector>

#include <Identity/GameIds.h>
#include "SerialBuffer.h"

class CPacketTable
{
public:
    using Handler = std::function<void(SessionId, CSerialBuffer*)>;

    enum class Dispatch
    {
        Handled,      // 처리 함수를 불렀다
        UnknownType,  // 없는 번호 — 범위 밖 포함
        TooSmall,     // 머리 크기가 최소 크기보다 작다
        NoHeader,     // 머리조차 없다(코어가 걸러서 오지 않는 값이지만 갈래를 비우지 않는다)
    };

    // [firstType, lastType] 번호대의 표. 양 끝 포함
    CPacketTable(uint16_t firstType, uint16_t lastType);

    // 기동 때 — 범위 밖이거나 이미 등록된 번호면 false
    bool Register(uint16_t type, uint16_t minSize, Handler handler);

    // 패킷 구조체 하나로 번호·크기를 함께 — 등록 실수의 자리를 줄인다
    template <typename TPacket>
    bool Register(Handler handler)
    {
        return Register(TPacket::TYPE, static_cast<uint16_t>(sizeof(TPacket)), std::move(handler));
    }

    // 머리를 읽어 처리 함수를 부른다. 결과의 지표 처리는 부르는 쪽 몫이다
    Dispatch Route(SessionId sessionId, CSerialBuffer* msg) const;

private:
    struct Entry
    {
        uint16_t minSize = 0;
        Handler handler;
    };

    uint16_t _firstType;
    uint16_t _lastType;
    std::vector<Entry> _entries;   // 색인 = 번호 - _firstType. handler 가 비었으면 빈 칸
};
