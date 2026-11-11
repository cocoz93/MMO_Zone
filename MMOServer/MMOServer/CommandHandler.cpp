#include "CommandHandler.h"

#include "SightBroadcaster.h"
#include "MapManager.h"
#include "Zone.h"
#include "Player.h"           // CPlayer · 운영자 표시 · GM 칸 · 체력 컴포넌트 · 가방(Inventory.h — USE_INVENTORY)
#include "PacketBuild.h"      // 머리 쓰기·닫기 · 속도 바뀜(MakeActorSpeed — /gm)
#include "SerialBuffer.h"
#include <Protocol/Protocol.h>
#include <Common/ErrorLog.h>  // SLOG_INFO — /item · /gm · /die 기록
#include <Text/TextUtf.h>     // 대리쌍 가르기(낱말 나누기)
#include <cassert>            // 직렬화 크기 검증
#include "SelfNotifier.h"     // 명령 답(알림 한 줄)
#include "GameMonitor.h"      // 명령 거절 · /gm 켜고 끈 수(_chat)
#if USE_COMBAT
#include "StatSystem.h"       // /gm 의 스탯 다시 세우기
#include "CombatSystem.h"     // /die 의 쓰러뜨리기
#endif
#if USE_PARTY
#include "PartyHandler.h"     // /party
#include <Identity/CharacterName.h>
#endif
#if USE_MAIL
#include "MailHandler.h"      // /accmail — 운영자 지급
#endif
#if USE_INVENTORY
#include "ItemHandler.h"
#include "GameDataFiles.h"
#include <Data/GameDataCsv.h> // 채팅 명령 인자를 숫자로 읽는다(표 파서와 같은 규칙)
#endif

using namespace PacketBuild;   // 아래 조립 함수(MakeActorSpeed)가 머리 쓰기·닫기를 이름만으로 부른다

// 채팅 명령 — ChatHandler.cpp 에서 옮김(10-26 · 몸·주석 그대로, 이름 CChatHandler → CCommandHandler · HandleChatCommand → Handle · /die 더함).

// ==========================================================================
// 채팅 명령 — 틀(낱말 나누기 · 표 · 권한 · 거절)은 늘 있고, 명령 줄만 토글을 본다(/item USE_INVENTORY · /gm · /die USE_COMBAT)
// ==========================================================================

namespace
{
    // 채팅(UTF-16) → 낱말들(UTF-8). 한글 인자는 /party 이름 · /accmail 제목 · 본문이 쓴다.
    //   ★ NUL 에서 멈춘다 — 웹·콘솔 클라는 끝에 NUL 을 붙여 보내고(proto.js chat()), RecvChat 의
    //     글자 수는 헤더 크기로 역산해 그 NUL 까지 센다. 안 멈추면 마지막 낱말이 "5\0" 이 되어 숫자로 안 읽힌다.
    //   대리쌍(이모지 같은 BMP 밖 글자 — 두 칸)은 4바이트 한 글자로 옮긴다(/accmail 본문 · mail-plan.md 6절 보내기).
    //   짝 없는 칸은 그 칸 하나를 3바이트로 그대로 둔다 — 받는 쪽(TextUtf::FromUtf8 · CharacterName::FromUtf8)이 대리쌍 영역이라 거절한다.
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
            if (c < 0x80)
            {
                cur += static_cast<char>(c);
            }
            else if (c < 0x800)
            {
                cur += static_cast<char>(0xC0 | (c >> 6));
                cur += static_cast<char>(0x80 | (c & 0x3F));
            }
            else if (TextUtf::IsHighSurrogate(c) && i + 1 < len && TextUtf::IsLowSurrogate(msg[i + 1]))
            {
                const uint32_t cp = 0x10000 + ((static_cast<uint32_t>(c) - 0xD800) << 10) + (static_cast<uint32_t>(msg[i + 1]) - 0xDC00);
                cur += static_cast<char>(0xF0 | (cp >> 18));
                cur += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                cur += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                cur += static_cast<char>(0x80 | (cp & 0x3F));
                ++i;
            }
            else
            {
                cur += static_cast<char>(0xE0 | (c >> 12));
                cur += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                cur += static_cast<char>(0x80 | (c & 0x3F));
            }
        }
        if (!cur.empty())
            out.push_back(cur);
    }

}

void CCommandHandler::Handle(CPlayer* player, const ChatChar* msg, uint16_t len)
{
    CGameMonitor::ChatCounters& counters = _gameMonitor._chat;

    // 명령은 사람이 치는 것이라 드물다 — 할당을 아끼지 않는다
    std::vector<std::string> words;
    SplitCommandWords(msg, len, words);
    const std::string cmd = words.empty() ? std::string("/") : words[0];   // 첫 글자가 '/' 라 비지 않는다

    // /test — 운영자 켜기(사용자 결정 11-04). 표 밖인 까닭: 표의 명령은 전부 아래 운영자 검사를 지나는데, 이것은 그 검사를 통과하게 만드는 쪽이다.
    //   꺼져 있으면(AllowDevAdmin=0) 표에 없는 이름이라 아래에서 「모르는 명령」으로 거절된다. 로그인 서버가 붙으면 계정 권한으로 옮긴다.
    if (_allowDevAdmin && cmd == "/test" && words.size() == 1)
    {
        player->_isAdmin = true;
        _selfNotifier.SendAdminOn(player);
        SLOG_INFO("[CMD] /test -> admin on (actor={}, character={})", player->_actorId, player->_characterId);
        return;
    }

    // 명령 표 — 이름 → 처리 함수(if 사슬 금지 — 명령이 늘면 한 줄 더한다).
    //   지금 명령은 전부 운영자 전용이라 권한은 표에서 찾은 뒤 한 번 본다. 누구나 쓰는 명령이 생기면 칸을 더한다.
    using Runner = void (CCommandHandler::*)(CPlayer*, const std::vector<std::string>&);
    struct Command
    {
        const char* name;
        Runner      run;
    };
    //   줄마다 그 명령이 기대는 토글을 본다 — 줄이 빠진 빌드에서 그 명령은 「모르는 명령」이다.
    static constexpr Command COMMANDS[] =
    {
#if USE_INVENTORY
        { "/item", &CCommandHandler::RunItemCommand },   // /item <아이템번호> [개수]
#endif
#if USE_COMBAT
        { "/gm",   &CCommandHandler::RunGmCommand   },   // /gm — GM 모드 켜고 끄기
        { "/die",  &CCommandHandler::RunDieCommand  },   // /die — 그 자리에서 쓰러지기
#endif
#if USE_PARTY
        { "/party", &CCommandHandler::RunPartyCommand },   // /party <이름> — 다른 존 서버의 사람도 부른다
#endif
#if USE_MAIL
        { "/accmail", &CCommandHandler::RunAccMailCommand },   // /accmail <계정번호|all> <돈> <아이템:개수,…|-> <제목> <본문…> · /accmail confirm
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

#if USE_PARTY
// /party <이름> — 이름으로 파티에 부른다. 찾기 · 초대 규칙은 월드 서버 몫이다
void CCommandHandler::RunPartyCommand(CPlayer* player, const std::vector<std::string>& words)
{
    char16_t name[CHAR_NAME_MAX_LEN];
    if (words.size() != 2 || !CharacterName::FromUtf8(words[1].data(), words[1].size(), name))
    {
        _gameMonitor._chat._commandRejected[static_cast<int>(ChatCommandReject::BAD_ARGS)].Inc();
        _selfNotifier.SendSystemMessage(player, "사용법: /party 이름");
        return;
    }
    _partyHandler.InviteByName(player, name);
    SLOG_INFO("[CMD] /party {} (character={})", words[1], player->_characterId);
}
#endif

#if USE_MAIL
// /accmail — 운영자의 계정 우편 지급(mail-plan.md 6절 보내기). 인자 검사 · 전 계정 대기 · 보내기 · 답은 우편 처리기 몫이다
//   (답이 서버 간 왕복 뒤에 와서 그때 운영자를 다시 찾아야 하고, 30초 대기도 우편 처리기가 든다). 여기는 표의 입구다.
void CCommandHandler::RunAccMailCommand(CPlayer* player, const std::vector<std::string>& words)
{
    if (!_mailHandler.RunAdminSend(player, words))
        _gameMonitor._chat._commandRejected[static_cast<int>(ChatCommandReject::BAD_ARGS)].Inc();
}
#endif

#if USE_COMBAT
// /gm — GM 모드 켜고 끄기(토글). 값과 저장하지 않는 까닭은 Player.h 의 GM 칸.
//   켜면 속도·최대 체력·공격력을 GM 값으로 바꾸고 체력·마력을 가득 채운다. 끄면 속도를 되돌리고 스탯을 레벨 표로 다시 세운다
//   (체력·마력은 넘는 몫만 깎는다 — RecalcStats refill=false).
//   알림은 레벨업과 같은 셋(체력 본인+주변 · 공격력 본인 · 마력 본인)에 속도(본인+주변)를 더한다 — 남의 화면도 그 속도로 앞질러 그린다.
void CCommandHandler::RunGmCommand(CPlayer* player, const std::vector<std::string>& words)
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
    // 속도도 아래 스탯 세우기가 세운다(GM 이면 GM_SPEED · 아니면 바탕 속도 — 속도 버프는 그 위에)
    // 켤 때만 가득 채운다. 바뀐 체력·공격력·마력은 여기서 표시가 남아 틱 끝 스탯 알림(CStatSync)이 보낸다 — 끌 때 안 바뀐 마력은 안 간다
    _statSystem.RecalcStats(player, player->_gmMode);
    counters._gmToggled[player->_gmMode ? 1 : 0].Inc();

    // 속도 — 위 스탯 세우기가 값을 세웠지만 틱 끝 스탯 알림(CStatSync)은 속도를 안 싣는다 — 여기서 바로 주변에
    if (CZone* zone = _mapManager.GetZone(player->_zoneId))
        _sightBroadcaster.BroadcastAroundSector(zone, player, MakeActorSpeed(player->_actorId, player->_speed), false);

    SLOG_INFO("[GM] /gm (character={}, player={}) -> {} (speed {}, maxHp {}, attack {})",
              player->_characterId, player->_actorId, player->_gmMode ? "on" : "off",
              player->_speed, player->_health.MaxHp(), player->_attack._attackPower);
    // 웹 채팅창 한 줄(400px)에 값이 다 들어가는 길이로 — 길면 끝이 「…」로 잘려 공격력이 안 보인다(10-16 헤드리스 사진)
    _selfNotifier.SendSystemMessage(player, std::string(player->_gmMode ? "GM 켬" : "GM 끔")
        + " — 속도 " + std::to_string(player->_speed) + " · 체력 " + std::to_string(player->_health.MaxHp())
        + " · 공격력 " + std::to_string(player->_attack._attackPower));
}

// /die — 그 자리에서 쓰러지기(10-26). 쓰러짐이 끝내는 것(거래 · 효과)을 몬스터 없이 바로 보려고 넣었다.
//   맞지 않고 죽는 길(CCombatSystem::Kill) — 죽음 표시 · 걷기 멈춤 · 죽음 계수 · 죽은 목록 · DEATH 방송 · 효과 풀림이 맞아 죽을 때와 같은 한 길이고,
//   부활도 같다(CCombatHandler::TickRespawn 의 부활 대기열). 피해 방송(DAMAGE)은 없다 — 맞은 게 아니라서.
void CCommandHandler::RunDieCommand(CPlayer* player, const std::vector<std::string>& words)
{
    CGameMonitor::ChatCounters& counters = _gameMonitor._chat;

    if (words.size() != 1)
    {
        counters._commandRejected[static_cast<int>(ChatCommandReject::BAD_ARGS)].Inc();
        _selfNotifier.SendSystemMessage(player, "쓰는 법: /die (그 자리에서 쓰러진다 — 인자 없음)");
        return;
    }

    if (player->_health.IsDead())
    {
        counters._commandRejected[static_cast<int>(ChatCommandReject::DEAD)].Inc();
        _selfNotifier.SendSystemMessage(player, "이미 쓰러져 있습니다");
        return;
    }

    // CChatHandler::RecvChat 은 존을 찾은 뒤에만 명령을 넘긴다 — 없을 수 없지만 /gm 과 같이 본다
    CZone* zone = _mapManager.GetZone(player->_zoneId);
    if (zone == nullptr)
        return;

    _combatSystem.Kill(zone, player, _frameCount);

    SLOG_INFO("[GM] /die (character={}, player={})", player->_characterId, player->_actorId);
    _selfNotifier.SendSystemMessage(player, "쓰러졌습니다 (/die)");
}
#endif // USE_COMBAT

#if USE_INVENTORY
// /item <아이템번호> [개수] — 개수를 안 적으면 1. 가방에 바로 넣는다(드랍이 생기기 전까지 가방을 채우던 길).
void CCommandHandler::RunItemCommand(CPlayer* player, const std::vector<std::string>& words)
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
        SLOG_INFO("[ITEM] /item (character={}, player={}) -> {} x{} (item {})",
                  player->_characterId, player->_actorId, def->name, count, def->itemId);
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
