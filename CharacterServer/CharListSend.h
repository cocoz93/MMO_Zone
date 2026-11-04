#pragma once
// ==========================================================================
// 목록 패킷 보내기 — 로그인 성공 뒤와 만들기 · 삭제 · 복구 성공 뒤에 같은 꼴로 나간다
// ==========================================================================

#include <algorithm>
#include <cstring>
#include <vector>

#include <Lobby/PacketSend.h>
#include <Protocol/CharacterProtocol.h>

namespace CharListSend
{
    inline void Send(CIOCPServer& network, CMonitorManager& monitor, SessionId sessionId, const std::vector<CharListEntry>& rows)
    {
        const int count = (std::min)(static_cast<int>(rows.size()), CHAR_LIST_MAX);   // 괄호 = 윈도 min 매크로 막기
        const int size = static_cast<int>(sizeof(S2C_CHAR_LIST)) + count * static_cast<int>(sizeof(CharListEntry));

        S2C_CHAR_LIST head{};
        head.header.size = static_cast<uint16_t>(size);
        head.header.type = static_cast<MsgType>(S2C_CHAR_LIST::TYPE);
        head.count = static_cast<uint16_t>(count);

        char buffer[sizeof(S2C_CHAR_LIST) + CHAR_LIST_MAX * sizeof(CharListEntry)];
        std::memcpy(buffer, &head, sizeof(head));
        if (count > 0)
            std::memcpy(buffer + sizeof(head), rows.data(), static_cast<size_t>(count) * sizeof(CharListEntry));

        PacketSend::SendBytes(network, monitor, sessionId, buffer, size);   // 웹이 먼저 나갔으면 false — 다음 로그인에 다시 받는다
    }
}
