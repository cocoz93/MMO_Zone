#include "ChatHandler.h"

#include "SessionHub.h"
#include "SightBroadcaster.h"
#include "MapManager.h"
#include "Zone.h"
#include "Player.h"           // CPlayer · 운영자 표시 · GM 칸 · 가방(Inventory.h — USE_INVENTORY)
#include "PacketRoutes.h"
#include "PacketBuild.h"      // 머리 쓰기·닫기 · 체력 바뀜(MakeActorStats — /gm)
#include "SerialBuffer.h"
#include <Protocol/Protocol.h>
#include <Common/ErrorLog.h>  // SLOG_INFO — /item · /gm 기록
#include <cassert>            // 직렬화 크기 검증
#include <cstddef>            // offsetof — 채팅 크기 검증
#include <cstring>            // strcmp — 운영 로그인 열쇠 대조
#include "SelfNotifier.h"     // 명령 답(알림 한 줄)
#include "GameMonitor.h"      // 명령 거절 · /gm 켜고 끈 수(_chat)
#if USE_COMBAT
#include "StatSystem.h"       // /gm 의 스탯 다시 세우기
#endif
#if USE_INVENTORY
#include "ItemHandler.h"
#include "GameDataFiles.h"
#include "GameDataCsv.h"      // 채팅 명령 인자를 숫자로 읽는다(표 파서와 같은 규칙)
#endif

using namespace PacketBuild;   // 아래 조립 함수들이 머리 쓰기·닫기를 이름만으로 부른다

// 채팅·운영 S2C 조립 — 이 조각만 부른다(GameServer.cpp 에서 옮김 · 몸·주석 그대로).
//   쓰는 법(스칼라는 << · 고정배열·문자열은 SetData(raw) · pack(1))은 PacketBuild.h 머리의 [규칙] 주석과 같다.

namespace
{
    CSerialBuffer* MakeAdminLoginOk()
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_ADMIN_LOGIN_OK::TYPE);    // 페이로드 없음 (헤더만)
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_ADMIN_LOGIN_OK));
        return buf;
    }

    CSerialBuffer* MakeAdminLoginFail()
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_ADMIN_LOGIN_FAIL::TYPE);  // 페이로드 없음 (헤더만)
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_ADMIN_LOGIN_FAIL));
        return buf;
    }

    // 가변 길이 — message는 고정배열/문자열이라 SetData(raw). msgLen은 null 제외 글자 수.
    CSerialBuffer* MakeChat(int32_t playerId, uint8_t displayChar, uint8_t colorIndex,
                             ChatChar* message, uint16_t msgLen)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_CHAT::TYPE);
        *buf << static_cast<int>(playerId);
        *buf << static_cast<uint8_t>(displayChar);
        *buf << static_cast<uint8_t>(colorIndex);
        buf->SetData(reinterpret_cast<char*>(message),
            (msgLen + 1) * sizeof(ChatChar));              // message (가변, null 포함, raw)
        FinalizePacket(buf);
        assert(buf->GetDataSize() ==
            offsetof(MSG_S2C_CHAT, message) + (msgLen + 1) * sizeof(ChatChar));
        return buf;
    }
}

// 받은 패킷 등록 — 게임 서버 등록의 채팅 · 운영자 로그인 두 줄 그대로(게임 서버 나누기 0-④ 의 등록에서 이 조각 몫 둘 · 13 단계).
bool CChatHandler::RegisterRoutes(CPacketRoutes& routes)
{
    bool ok = true;
    ok &= routes.OnPlayer(MsgType::C2S_CHAT,       sizeof(MsgHeader) + sizeof(ChatChar),   // 가변 길이: 최소 1글자
                          [this](CPlayer* p, CSerialBuffer* m) { RecvChat(p, m); });
    ok &= routes.OnPlayer(MsgType::C2S_ADMIN_LOGIN, sizeof(MSG_C2S_ADMIN_LOGIN), [this](CPlayer* p, CSerialBuffer* m) { RecvAdminLogin(p, m); });
    return ok;
}

void CChatHandler::RecvChat(CPlayer* player, CSerialBuffer* pMsg)
{
    CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone == nullptr)
        return;

    // 가변 길이 수신: header.size 기반으로 실제 메시지 길이 역산
    MsgHeader header{};
    pMsg->PeekData(reinterpret_cast<char*>(&header), sizeof(header));

    uint16_t recvSize = header.size;
    if (recvSize > sizeof(MSG_C2S_CHAT))
        recvSize = sizeof(MSG_C2S_CHAT);  // 최대 크기 제한

    MSG_C2S_CHAT recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), recvSize);

    // 메시지 길이 계산 (바이트 → wchar_t 글자 수)
    uint16_t msgBytes = recvSize - sizeof(MsgHeader);
    msgBytes -= msgBytes % sizeof(ChatChar);  // 문자 경계 정렬 (홀수 바이트 방어)
    uint16_t msgLen = msgBytes / sizeof(ChatChar);
    if (msgLen > CHAT_MSG_MAX_LEN - 1)
        msgLen = CHAT_MSG_MAX_LEN - 1;
    recvMsg.message[msgLen] = L'\0';  // null 종단 보장

    // '/' 로 시작하면 명령이다 — 방송하지 않는다. 운영자가 아닌 사람의 /item 이 채팅으로 새지 않게,
    //   권한 검사보다 "방송 안 함"이 먼저다. 부하 더미의 채팅 문구에는 '/' 로 시작하는 것이 없다.
    //   토글이 없다 — 명령이 하나도 없는 빌드에서도 가로챈다(명령 줄만 토글을 본다 · 게임 서버 나누기 14 단계 전에는 USE_INVENTORY 안).
    if (msgLen > 0 && recvMsg.message[0] == u'/')
    {
        HandleChatCommand(player, recvMsg.message, msgLen);
        return;
    }

#if USE_BROADCAST_BUNDLE
    // [digest] 즉시 브로드캐스트 대신 보류 등록 — 틱 끝 digest에 합류 (실송신 시점은 기존과 동일한 틱 끝 flush).
    // 소스 섹터는 수신 시점 좌표로 고정 → 기존(즉시 배포)의 AOI 의미 보존.
    _sightBroadcaster.RegisterSectorItem(zone, player->_zoneId, player->_sectorX, player->_sectorY,
        MakeChat(player->_actorId, player->_displayChar, player->_colorIndex,
            recvMsg.message, msgLen));
#else
    // 본인 포함 브로드캐스트 (송신 함수가 소유권 소비)
    _sightBroadcaster.BroadcastAroundSector(zone, player,
        MakeChat(player->_actorId, player->_displayChar, player->_colorIndex,
            recvMsg.message, msgLen), false);
#endif
}

// ==========================================================================
// 채팅 명령 — 틀(낱말 나누기 · 표 · 권한 · 거절)은 늘 있고, 명령 줄만 토글을 본다(/item USE_INVENTORY · /gm USE_COMBAT)
// ==========================================================================

namespace
{
    // 채팅(UTF-16) → 낱말들. 명령과 인자가 ASCII 라 그 밖의 글자는 '?' 로 바꾼다
    //   (한글 인자를 받는 명령이 생기면 그때 UTF-8 로 옮긴다).
    //   ★ NUL 에서 멈춘다 — 웹·콘솔 클라는 끝에 NUL 을 붙여 보내고(proto.js chat()), RecvChat 의
    //     글자 수는 헤더 크기로 역산해 그 NUL 까지 센다. 안 멈추면 마지막 낱말이 "5\0" 이 되어 숫자로 안 읽힌다.
    void SplitCommandWords(const ChatChar* msg, uint16_t len, std::vector<std::string>& out)
    {
        out.clear();
        std::string cur;
        for (uint16_t i = 0; i < len; ++i)
        {
            const ChatChar c = msg[i];
            if (c == u'\0')
                break;
            if (c == u' ' || c == u'\t')
            {
                if (!cur.empty())
                {
                    out.push_back(cur);
                    cur.clear();
                }
                continue;
            }
            cur += (c < 0x80) ? static_cast<char>(c) : '?';
        }
        if (!cur.empty())
            out.push_back(cur);
    }

#if USE_COMBAT
    // 속도 — 본인 + 주변(BroadcastAroundSector, excludeSelf=false — Protocol.h MSG_S2C_ACTOR_SPEED).
    //   부르는 곳이 /gm 하나라 명령 구역에 둔다(/gm 과 같은 토글 — 안 쓰는 파일 안 함수로 경고가 나지 않게). 속도를 바꾸는 둘째 길(효과 등)이 생기면 위의 Make 묶음으로 옮긴다.
    CSerialBuffer* MakeActorSpeed(int32_t actorId, int32_t speed)
    {
        CSerialBuffer* buf = CSerialBuffer::Alloc();
        BeginPacket(buf, MSG_S2C_ACTOR_SPEED::TYPE);
        *buf << static_cast<int>(actorId);
        *buf << static_cast<int>(speed);
        FinalizePacket(buf);
        assert(buf->GetDataSize() == sizeof(MSG_S2C_ACTOR_SPEED));
        return buf;
    }
#endif
}

void CChatHandler::HandleChatCommand(CPlayer* player, const ChatChar* msg, uint16_t len)
{
    CGameMonitor::ChatCounters& counters = _gameMonitor._chat;

    // 명령은 사람이 치는 것이라 드물다 — 할당을 아끼지 않는다
    std::vector<std::string> words;
    SplitCommandWords(msg, len, words);
    const std::string cmd = words.empty() ? std::string("/") : words[0];   // 첫 글자가 '/' 라 비지 않는다

    // 명령 표 — 이름 → 처리 함수(if 사슬 금지 — 명령이 늘면 한 줄 더한다).
    //   지금 명령은 전부 운영자 전용이라 권한은 표에서 찾은 뒤 한 번 본다. 누구나 쓰는 명령이 생기면 칸을 더한다.
    using Runner = void (CChatHandler::*)(CPlayer*, const std::vector<std::string>&);
    struct Command
    {
        const char* name;
        Runner      run;
    };
    //   줄마다 그 명령이 기대는 토글을 본다 — 줄이 빠진 빌드에서 그 명령은 「모르는 명령」이다.
    static constexpr Command COMMANDS[] =
    {
#if USE_INVENTORY
        { "/item", &CChatHandler::RunItemCommand },   // /item <아이템번호> [개수]
#endif
#if USE_COMBAT
        { "/gm",   &CChatHandler::RunGmCommand   },   // /gm — GM 모드 켜고 끄기
#endif
        { nullptr, nullptr },   // 빈 줄 — 명령이 하나도 없는 빌드(전투를 끈 빌드)에서도 표가 크기 0 이 되지 않게(C++ 는 크기 0 배열을 못 둔다)
    };

    const Command* found = nullptr;
    for (const Command& c : COMMANDS)
    {
        if (c.name != nullptr && cmd == c.name)
        {
            found = &c;
            break;
        }
    }

    if (found == nullptr)
    {
        counters._commandRejected[static_cast<int>(ChatCommandReject::UNKNOWN_COMMAND)].Inc();
        _selfNotifier.SendSystemMessage(player, "모르는 명령입니다: " + cmd);
        return;
    }

    if (!player->_isAdmin)
    {
        counters._commandRejected[static_cast<int>(ChatCommandReject::NOT_ADMIN)].Inc();
        _selfNotifier.SendSystemMessage(player, "운영자만 쓸 수 있는 명령입니다: " + cmd);
        return;
    }

    (this->*(found->run))(player, words);
}

#if USE_COMBAT
// /gm — GM 모드 켜고 끄기(토글). 값과 저장하지 않는 까닭은 Player.h 의 GM 칸.
//   켜면 속도·최대 체력·공격력을 GM 값으로 바꾸고 체력·마력을 가득 채운다. 끄면 속도를 되돌리고 스탯을 레벨 표로 다시 세운다
//   (체력·마력은 넘는 몫만 깎는다 — RecalcStats refill=false).
//   알림은 레벨업과 같은 셋(체력 본인+주변 · 공격력 본인 · 마력 본인)에 속도(본인+주변)를 더한다 — 남의 화면도 그 속도로 앞질러 그린다.
void CChatHandler::RunGmCommand(CPlayer* player, const std::vector<std::string>& words)
{
    CGameMonitor::ChatCounters& counters = _gameMonitor._chat;

    if (words.size() != 1)
    {
        counters._commandRejected[static_cast<int>(ChatCommandReject::BAD_ARGS)].Inc();
        _selfNotifier.SendSystemMessage(player, "쓰는 법: /gm (켜고 끈다 — 인자 없음)");
        return;
    }

    // 쓰러진 채 켜면 아래 「가득 채움」이 부활 절차(마을로 보내기 · 부활 방송) 없이 되살린다 — 거절한다.
    //   끄기도 같이 막는다(쓰러진 채로는 상태를 안 바꾼다는 한 줄 규칙 — 부활하면 그때 끄면 된다).
    if (player->_health.IsDead())
    {
        counters._commandRejected[static_cast<int>(ChatCommandReject::DEAD)].Inc();
        _selfNotifier.SendSystemMessage(player, "쓰러진 채로는 GM 모드를 바꿀 수 없습니다");
        return;
    }

    player->_gmMode = !player->_gmMode;
    if (player->_gmMode)
    {
        player->_speedBeforeGm = player->_speed;
        player->_speed         = CPlayer::GM_SPEED;
    }
    else
    {
        player->_speed = player->_speedBeforeGm;
    }
    _statSystem.RecalcStats(player, player->_gmMode);   // 켤 때만 가득 채운다
    counters._gmToggled[player->_gmMode ? 1 : 0].Inc();

    CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone != nullptr)
    {
        _sightBroadcaster.BroadcastAroundSector(zone, player, MakeActorStats(player->_actorId, player->_health.Hp(), player->_health.MaxHp()), false);
        _sightBroadcaster.BroadcastAroundSector(zone, player, MakeActorSpeed(player->_actorId, player->_speed), false);
    }
    _selfNotifier.SendMyStats(player);
#if USE_SKILL
    _selfNotifier.SendActorMp(player);   // 켤 때 가득 채운 마력 — 끌 때는 안 바뀌지만 같은 한 벌로 보낸다
#endif

    SLOG_INFO("[GM] /gm (account={}, player={}) -> {} (speed {}, maxHp {}, attack {})",
              player->_accountId, player->_actorId, player->_gmMode ? "on" : "off",
              player->_speed, player->_health.MaxHp(), player->_attack._attackPower);
    // 웹 채팅창 한 줄(400px)에 값이 다 들어가는 길이로 — 길면 끝이 「…」로 잘려 공격력이 안 보인다(10-16 헤드리스 사진)
    _selfNotifier.SendSystemMessage(player, std::string(player->_gmMode ? "GM 켬" : "GM 끔")
        + " — 속도 " + std::to_string(player->_speed) + " · 체력 " + std::to_string(player->_health.MaxHp())
        + " · 공격력 " + std::to_string(player->_attack._attackPower));
}
#endif // USE_COMBAT

#if USE_INVENTORY
// /item <아이템번호> [개수] — 개수를 안 적으면 1. 가방에 바로 넣는다(드랍이 생기기 전까지 가방을 채우던 길).
void CChatHandler::RunItemCommand(CPlayer* player, const std::vector<std::string>& words)
{
    CGameMonitor::InventoryCounters& counters = _gameMonitor._inventory;

    // /item <아이템번호> [개수]  — 개수를 안 적으면 1
    int64_t itemId = 0;
    int64_t count  = 1;
    const bool okId    = words.size() >= 2 && GameDataCsv::ParseInt(words[1], itemId);
    const bool okCount = words.size() < 3 || GameDataCsv::ParseInt(words[2], count);
    if (!okId || !okCount || words.size() > 3 || count < 1 || count > 9999)
    {
        _gameMonitor._chat._commandRejected[static_cast<int>(ChatCommandReject::BAD_ARGS)].Inc();   // 명령 거절은 채팅 묶음 — 넣기 거절과 다른 사실
        _selfNotifier.SendSystemMessage(player, "쓰는 법: /item <아이템번호> [개수 1~9999]");
        return;
    }

    // 번호 범위를 먼저 본다 — int32 로 줄이기 전에 걸러야 큰 값이 엉뚱한 번호로 접히지 않는다
    const ItemDef* def = (itemId > 0 && itemId <= CItemTable::MAX_ITEM_ID)
                       ? _gameDataFiles.Item().Find(static_cast<int32_t>(itemId))
                       : nullptr;
    if (def == nullptr)
    {
        counters._addRejected[static_cast<int>(InvAddResult::UNKNOWN_ITEM)].Inc();
        _selfNotifier.SendSystemMessage(player, "없는 아이템 번호입니다: " + words[1]);
        return;
    }

    const InvAddResult result = player->_inventory.TryAdd(*def, static_cast<int32_t>(count));
    switch (result)
    {
    case InvAddResult::OK:
        // 개수(_itemsAdded)는 칸에 드는 아이템만 — 돈의 개수는 금액(전)이라 단위가 달라 돈 들어옴에만 센다
        if (def->store == ItemStore::MONEY)
            counters._moneyIn[static_cast<int>(MoneySource::COMMAND)].Add(count);   // 개수 = 금액(전)
        else
            counters._itemsAdded.Add(count);
        SLOG_INFO("[ITEM] /item (account={}, player={}) -> {} x{} (item {})",
                  player->_accountId, player->_actorId, def->name, count, def->itemId);
        _itemHandler.FlushInventory(player);
        return;

    case InvAddResult::TAB_FULL:
        _selfNotifier.SendSystemMessage(player, std::string("가방에 자리가 없습니다 — ") + InvTabDisplayName(def->tab)
                                  + " 탭에 " + def->name + " " + std::to_string(count) + "개를 다 둘 수 없습니다");
        break;

    case InvAddResult::MONEY_FULL:     // 돈 아이템 — 받으면 상한을 넘는다(아무것도 안 바뀜)
        _selfNotifier.SendSystemMessage(player, "돈이 넘칩니다 — 들 수 있는 돈은 " + std::to_string(MONEY_MAX) + " 전까지입니다(지금 "
                                  + std::to_string(player->_inventory.Money()) + ")");
        break;

    case InvAddResult::UNKNOWN_ITEM:   // TryAdd 는 이 둘을 안 돌려준다 — 위에서 이미 걸렀다
    case InvAddResult::BAD_COUNT:
        _selfNotifier.SendSystemMessage(player, std::string("넣지 못했습니다: ") + InvAddResultName(result));
        break;

    case InvAddResult::COUNT:          // 값이 아니라 개수 표시 — 아래 배열 색인으로 쓰면 범위 밖이다
        assert(false && "TryAdd 가 COUNT 를 돌려줄 수는 없다");
        return;
    }
    counters._addRejected[static_cast<int>(result)].Inc();
}
#endif // USE_INVENTORY

// ── 운영자 인증 ──

// 부하 테스트 도구용 고정 키 — 실서비스 인증 아님.
// 와이어에 평문으로 흐르고 해시·시도 제한도 없다. admin 권한 효과는 채널 인원 제한 우회(CMapInstance::HasRoom)와
//   다른 맵 지정 이동(CMapManager::PickZoneChangeTarget — 일반은 포털로만) 둘, 그리고 채팅 운영 명령이다.
// 실계정 연동 단계에서 계정 DB 조회 기반 인증으로 교체한다.
static constexpr char ADMIN_KEY[] = "admin1234";

void CChatHandler::RecvAdminLogin(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_ADMIN_LOGIN recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));

    // null-terminate 보장
    recvMsg.key[ADMIN_KEY_MAX_LEN - 1] = '\0';

    if (strcmp(recvMsg.key, ADMIN_KEY) == 0)
    {
        player->_isAdmin = true;
        _sessionHub.SendPacket(player, MakeAdminLoginOk());
    }
    else
    {
        _sessionHub.SendPacket(player, MakeAdminLoginFail());
    }
}
