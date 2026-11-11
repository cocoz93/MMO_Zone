#include <Identity/SecureRandom.h>   // ★ 맨 앞 — WinSock2 를 Windows.h 보다 먼저 싣는다(뒤 헤더가 Windows.h 를 먼저 끌면 sockaddr 재정의 · EnterGate.cpp 와 같은 차례)
#include "MailSystem.h"

#if USE_MAIL

#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include "Player.h"          // 가방 · 생사 · 존 번호 · 우편 상태와 대기 문장
#include "MailDraft.h"       // MailDraft — 대기 문장
#include "GameDataFiles.h"   // 맵 표(맵 종류) · 아이템 표(거래 가능 · 넣기의 정의)
#include "GameMonitor.h"     // 우편 카운터 · 돈 지표
#include "MapManager.h"      // GetMapIdFromZoneId — 존 번호 → 맵 번호 풀기(정적 함수만)
#if USE_TRADE
#include "TradeSystem.h"     // Busy — 거래 신청 · 거래창
#endif
#include <Identity/CharacterName.h>    // 받는 이름 검사 · 이름 UTF-8
#include <Text/TextUtf.h>              // 제목 · 본문 검사와 UTF-8
#include <Common/ErrorLog.h>

bool MailAllowedOn(MapKind kind)
{
    static_assert(static_cast<int>(MapKind::COUNT) == 3, "맵 종류를 늘렸으면 우편이 되는지 여기서 정할 것");
    switch (kind)
    {
    case MapKind::TOWN:     return true;    // 마을
    case MapKind::DUNGEON:  return true;    // 사냥터
    case MapKind::INSTANCE: return false;   // 파티 방 — 거래와 같은 규칙(mail-plan.md 9절 NOT_HERE)
    case MapKind::COUNT:    break;
    }
    return false;
}

namespace
{
    // 제목이 공백뿐인가 — 반각 공백과 한글 자판의 전각 공백(U+3000) 둘을 공백으로 본다(빈 제목을 공백으로 우회하지 못하게)
    bool OnlySpaces(const char16_t* cells, int length)
    {
        for (int i = 0; i < length; ++i)
        {
            if (cells[i] != u' ' && cells[i] != u'\x3000')
                return false;
        }
        return true;
    }

    // UTF-8 글자를 DB 칸에 — 칸 크기는 글자 수 × 4 라 와이어 칸에서 온 글자는 늘 들어간다(MailDraft.h static_assert). 넘으면 잘못 부른 것
    template <size_t N>
    void PutBytes(DBBytes<N>& dst, const std::string& src)
    {
        assert(src.size() <= N);
        const size_t n = (src.size() <= N) ? src.size() : N;
        std::memcpy(dst.data, src.data(), n);
        dst.len = static_cast<unsigned long>(n);
    }

    // 로그용 — 칸 목록 "id x count, …"
    std::string ItemsText(const MailDraft& d)
    {
        std::string s;
        for (int i = 0; i < d.itemCount; ++i)
        {
            if (i > 0)
                s += ", ";
            s += std::to_string(d.items[i].itemId) + " x " + std::to_string(d.items[i].count);
        }
        return s;
    }

    std::string ItemsText(const MailAttach::Items& a)
    {
        std::string s;
        for (int i = 0; i < a.count; ++i)
        {
            if (i > 0)
                s += ", ";
            s += std::to_string(a.items[i].itemId) + " x " + std::to_string(a.items[i].count);
        }
        return s;
    }

    // 로그용 — send_key 16바이트를 16진 32자로(문의 때 DB 의 send_key 와 맞춰 본다)
    std::string KeyHex(const DBBytes<DB_MAIL_SEND_KEY_BYTES>& key)
    {
        static const char* digits = "0123456789abcdef";
        std::string s;
        for (unsigned long i = 0; i < key.len; ++i)
        {
            s += digits[key.data[i] >> 4];
            s += digits[key.data[i] & 0x0F];
        }
        return s;
    }
}

CMailSystem::CMailSystem(const CGameDataFiles& tables,
#if USE_TRADE
                         const CTradeSystem& tradeSystem,
#endif
                         CGameMonitor& gameMonitor)
    : _tables(tables)
#if USE_TRADE
    , _tradeSystem(tradeSystem)
#endif
    , _gameMonitor(gameMonitor)
{
}

// ==========================================================================
// 사전 검사
// ==========================================================================
MailResult CMailSystem::CheckReadDoor(const CPlayer* p) const
{
    if (p->_accountId == NO_ACCOUNT)
        return MailResult::NO_CHARACTER;   // 시험 입장 길 — 이름이 없고 부하 측정 조건이 바뀌면 안 된다(mail-plan.md 2절 · 더미는 이번 플랜에서 뺌)
    const MapDef* map = _tables.Maps().Find(CMapManager::GetMapIdFromZoneId(p->_zoneId));
    if (map == nullptr || !MailAllowedOn(map->kind))
        return MailResult::NOT_HERE;
    if (p->_mailOp != CPlayer::MailOp::IDLE)
        return MailResult::BUSY;           // 앞 우편 동작이 진행 중
    return MailResult::OK;
}

MailResult CMailSystem::CheckItemDoor(const CPlayer* p) const
{
    const MailResult door = CheckReadDoor(p);
    if (door != MailResult::OK)
        return door;
    if (p->_health.IsDead())
        return MailResult::DEAD;
#if USE_TRADE
    // 거래 신청이나 거래창 — 짝 묶음이 우편으로 줄어든 칸을 우편 문장 없이 싣지 않게(mail-plan.md 5절 「거래와 함께 있을 수 없다」).
    //   위에서 _mailOp 이 IDLE 인 것을 봤으므로 여기 Busy 는 거래만 본다
    if (_tradeSystem.Busy(p))
        return MailResult::BUSY;
#endif
    if (p->_dbFlight.paired)
        return MailResult::BUSY;           // 거래는 끝났어도 짝 묶음이 확정 전이다
    return MailResult::OK;
}

MailResult CMailSystem::CheckAgain(const CPlayer* p) const
{
    const MapDef* map = _tables.Maps().Find(CMapManager::GetMapIdFromZoneId(p->_zoneId));
    if (map == nullptr || !MailAllowedOn(map->kind))
        return MailResult::NOT_HERE;       // 왕복 사이에 파티 방으로 옮겼다
    if (p->_health.IsDead())
        return MailResult::DEAD;
    return MailResult::OK;
}

// ==========================================================================
// 왕복 동작 열기 · 닫기
// ==========================================================================
MailResult CMailSystem::BeginList(CPlayer* p)
{
    const MailResult r = CheckReadDoor(p);
    if (r == MailResult::OK)
        p->_mailOp = CPlayer::MailOp::LIST;
    return r;
}

MailResult CMailSystem::BeginRead(CPlayer* p)
{
    const MailResult r = CheckReadDoor(p);
    if (r == MailResult::OK)
        p->_mailOp = CPlayer::MailOp::READ;
    return r;
}

MailResult CMailSystem::BeginErase(CPlayer* p)
{
    const MailResult r = CheckReadDoor(p);
    if (r == MailResult::OK)
        p->_mailOp = CPlayer::MailOp::ERASE;
    return r;
}

MailResult CMailSystem::BeginTake(CPlayer* p)
{
    const MailResult r = CheckItemDoor(p);
    if (r == MailResult::OK)
        p->_mailOp = CPlayer::MailOp::TAKE_READ;
    return r;
}

void CMailSystem::End(CPlayer* p)
{
    assert(p->_mailOp != CPlayer::MailOp::SEND_SAVE && p->_mailOp != CPlayer::MailOp::TAKE_SAVE && "가방이 바뀐 뒤에는 확정 회신만 푼다");
    assert(!(p->_mailDraft && p->_mailDraft->ready) && "대기로 올린 문장을 버리면 줄인 칸이 사라진다");
    if (p->_mailDraft && !p->_mailDraft->ready)
        p->_mailDraft.reset();   // 찾기 중 초안 — 아직 아무것도 안 줄였다
    p->_mailOp = CPlayer::MailOp::IDLE;
}

bool CMailSystem::Waiting(const CPlayer* p, DBMailOp op) const
{
    static_assert(static_cast<int>(DBMailOp::COUNT) == 8, "DBMailOp 을 늘렸으면 어느 상태가 그 회신을 기다리는지 여기서 정할 것");
    switch (op)
    {
    case DBMailOp::LIST:          return p->_mailOp == CPlayer::MailOp::LIST;
    case DBMailOp::READ_BODY:     return p->_mailOp == CPlayer::MailOp::READ;
    case DBMailOp::ERASE:         return p->_mailOp == CPlayer::MailOp::ERASE;
    case DBMailOp::FIND_RECEIVER: return p->_mailOp == CPlayer::MailOp::FIND;
    case DBMailOp::READ_ONE:      return p->_mailOp == CPlayer::MailOp::TAKE_READ;
    case DBMailOp::COUNT_PENDING: return true;    // 입장 때 수 세기는 상태를 안 쓴다(mail-plan.md 7절 끝) — 다른 동작과 겹쳐도 해가 없다
    case DBMailOp::FIND_MOVED:    return false;   // 계정 우편(8단계) — 아직 내는 곳이 없다
    case DBMailOp::FIND_NAME:     return false;
    case DBMailOp::COUNT:         break;
    }
    return false;
}

bool CMailSystem::Confirm(CPlayer* p, DBMailStmtKind kind)
{
    static_assert(static_cast<int>(DBMailStmtKind::COUNT) == 3, "우편 문장 종류를 늘렸으면 어느 상태가 그 확정을 기다리는지 여기서 정할 것");
    CPlayer::MailOp want = CPlayer::MailOp::IDLE;
    switch (kind)
    {
    case DBMailStmtKind::SEND_INSERT:     want = CPlayer::MailOp::SEND_SAVE; break;
    case DBMailStmtKind::TAKE_UPDATE:     want = CPlayer::MailOp::TAKE_SAVE; break;
    case DBMailStmtKind::ACC_TAKE_INSERT: return false;   // 계정 우편(8단계) — 아직 만드는 곳이 없다
    case DBMailStmtKind::COUNT:           return false;
    }
    if (p->_mailOp != want)
        return false;
    p->_mailOp = CPlayer::MailOp::IDLE;
    return true;
}

// ==========================================================================
// 보내기
// ==========================================================================
MailResult CMailSystem::BeginSend(CPlayer* p, const MSG_C2S_MAIL_SEND& form)
{
    // 1 — 사전 검사
    const MailResult door = CheckItemDoor(p);
    if (door != MailResult::OK)
        return door;

    // 와이어 칸을 정렬된 지역 배열로 옮긴다 — 패킷 구조체는 pack(1) 이라 칸 주소를 넘기지 않는다
    char16_t toName[CHAR_NAME_MAX_LEN];
    char16_t title[MAIL_TITLE_MAX];
    char16_t body[MAIL_BODY_MAX];
    std::memcpy(toName, form.toName, sizeof(toName));
    std::memcpy(title, form.title, sizeof(title));
    std::memcpy(body, form.body, sizeof(body));
    const int64_t money = form.money;
    const int itemCount = form.itemCount;
    MailSendSlot slots[MAIL_MAX_ITEMS];
    std::memcpy(slots, form.items, sizeof(slots));

    // 2 — 받는 이름
    if (!CharacterName::IsValid(toName))
        return MailResult::BAD_NAME;

    // 3 — 제목 · 본문: 첫 0 뒤 칸은 전부 0 · 제어 문자 없음(본문만 줄바꿈 허용) · 홀로 남은 대리쌍 없음 · 제목이 공백뿐이 아님
    const int titleLen = TextUtf::CellLength(title, MAIL_TITLE_MAX);
    const int bodyLen  = TextUtf::CellLength(body, MAIL_BODY_MAX);
    if (titleLen < 1 || bodyLen < 0)
        return MailResult::BAD_TEXT;
    if (!TextUtf::PairsOk(title, titleLen) || !TextUtf::PairsOk(body, bodyLen))
        return MailResult::BAD_TEXT;
    if (TextUtf::HasControl(title, titleLen, false) || TextUtf::HasControl(body, bodyLen, true))
        return MailResult::BAD_TEXT;
    if (OnlySpaces(title, titleLen))
        return MailResult::BAD_TEXT;

    // 4 — 돈 · 칸 수
    if (money < 0 || money > MONEY_MAX)
        return MailResult::BAD_MONEY;
    if (itemCount > MAIL_MAX_ITEMS)
        return MailResult::BAD_SLOT;

    // 5 — 칸마다: 범위 · 착용 칸 · 빈 칸 · 같은 칸 두 번(거래의 올리기와 같은 검사 · TradeSystem.cpp Offer)
    const CInventory& inv = p->_inventory;
    int index[MAIL_MAX_ITEMS] = {};
    for (int i = 0; i < itemCount; ++i)
    {
        InvTab tab;
        if (!FindInvSlot(slots[i].slot, tab, index[i]) || IsWornIndex(index[i]) || inv.At(index[i]).itemId == 0)
            return MailResult::BAD_SLOT;
        for (int j = 0; j < i; ++j)
        {
            if (index[j] == index[i])
                return MailResult::BAD_SLOT;
        }
    }
    // 6 — 칸마다: 표에 있고 거래할 수 있다
    for (int i = 0; i < itemCount; ++i)
    {
        const ItemDef* def = _tables.Item().Find(inv.At(index[i]).itemId);
        if (def == nullptr || !def->tradable)
            return MailResult::NOT_TRADABLE;
    }
    // 7 — 칸마다: 개수 1 ~ 그 칸 개수(일부만 보내기 됨)
    for (int i = 0; i < itemCount; ++i)
    {
        if (slots[i].count < 1 || slots[i].count > inv.At(index[i]).count)
            return MailResult::BAD_COUNT;
    }
    // 8 — 돈 + 수수료 ≤ 쓸 수 있는 돈
    if (money + _rules.sendFee > inv.SpendableMoney())
        return MailResult::NOT_ENOUGH_MONEY;

    // ── 초안 — 받는 사람 번호는 찾기 회신이 채운다(FinishSend). 아무것도 안 줄였다 ──
    auto draft = std::make_unique<MailDraft>();
    draft->ready = false;
    draft->itemCount = static_cast<uint8_t>(itemCount);
    MailAttach::Items attach;
    attach.count = itemCount;
    for (int i = 0; i < itemCount; ++i)
    {
        draft->items[i] = MailDraftItem{ index[i], inv.At(index[i]).itemId, slots[i].count };
        attach.items[i] = MailItemEntry{ inv.At(index[i]).itemId, slots[i].count };
    }

    DBMailStmt& s = draft->stmt;
    s.kind              = DBMailStmtKind::SEND_INSERT;
    s.mailId            = 0;
    s.characterId       = 0;                         // 받는 사람 — FinishSend
    s.accountId         = 0;
    s.fromCharacterId   = ToInt(p->_characterId);
    s.money             = money;
    s.fromAccountMailId = 0;
    s.expireDays        = _rules.expireDays;
    PutBytes(s.fromName, CharacterName::ToUtf8(p->_name));
    std::string text;
    TextUtf::ToUtf8(title, titleLen, text);          // 대리쌍은 위에서 봤다 — 실패하지 않는다
    PutBytes(s.title, text);
    TextUtf::ToUtf8(body, bodyLen, text);
    PutBytes(s.body, text);
    {
        uint8_t bytes[MailAttach::MAX_BYTES];
        const int n = MailAttach::Write(attach, bytes);   // 칸 0 개면 0 바이트(빈 값 = 없음)
        std::memcpy(s.attach.data, bytes, static_cast<size_t>(n));
        s.attach.len = static_cast<unsigned long>(n);
    }
    // 같은 INSERT 가 두 번 실려도 한 줄만 남게 하는 열쇠(mail-plan.md 5절) — 늘 16바이트를 채운다(UNIQUE 열이라 빈 값이면 두 번째 우편부터 막힌다)
    if (!SecureRandom::Fill(s.sendKey.data, DB_MAIL_SEND_KEY_BYTES))
    {
        SLOG_ERROR("[MAIL] send key random failed (character={}) — 보내기를 DB_ERROR 로 닫는다", ToInt(p->_characterId));
        return MailResult::DB_ERROR;
    }
    s.sendKey.len = static_cast<unsigned long>(DB_MAIL_SEND_KEY_BYTES);

    p->_mailDraft = std::move(draft);
    p->_mailOp = CPlayer::MailOp::FIND;
    return MailResult::OK;
}

MailResult CMailSystem::FinishSend(CPlayer* p, const DBMailReply& found)
{
    assert(p->_mailOp == CPlayer::MailOp::FIND && p->_mailDraft && !p->_mailDraft->ready);
    MailDraft& d = *p->_mailDraft;
    CInventory& inv = p->_inventory;

    // 차례(mail-plan.md 5절 「찾기 회신 뒤」): CheckAgain → DB_ERROR → NO_TARGET → SELF → TARGET_DELETED → TARGET_FULL → BAG_CHANGED → NOT_ENOUGH_MONEY.
    //   DB_ERROR 를 행 판정보다 앞에 둔다 — 못 읽은 회신(ok=false)의 found·count 는 뜻이 없다(DBMailReply)
    MailResult r = CheckAgain(p);
    if (r == MailResult::OK && !found.ok)                                      r = MailResult::DB_ERROR;
    if (r == MailResult::OK && !found.found)                                   r = MailResult::NO_TARGET;
    if (r == MailResult::OK && found.foundId == ToInt(p->_characterId))        r = MailResult::SELF;
    if (r == MailResult::OK && found.targetDeleted)                            r = MailResult::TARGET_DELETED;
    if (r == MailResult::OK && found.count >= _rules.maxUnread)                r = MailResult::TARGET_FULL;
    if (r == MailResult::OK)
    {
        // 왕복 사이에 칸이 바뀌었나 — 같은 아이템이고 보낼 개수가 아직 있나(칸을 미리 잠그지 않는다 · 5절 4번)
        for (int i = 0; i < d.itemCount; ++i)
        {
            const Item& it = inv.At(d.items[i].index);
            if (it.itemId != d.items[i].itemId || it.count < d.items[i].count)
            {
                r = MailResult::BAG_CHANGED;
                break;
            }
        }
    }
    if (r == MailResult::OK && d.stmt.money + _rules.sendFee > inv.SpendableMoney()) r = MailResult::NOT_ENOUGH_MONEY;
    if (r != MailResult::OK)
    {
        End(p);   // 초안을 버린다 — 아직 아무것도 안 줄였다
        return r;
    }

    // ── 통과 — 같은 함수 안에서 칸·돈을 줄이고 문장을 대기로 올리고 로그를 남긴다 ──
    for (int i = 0; i < d.itemCount; ++i)
        inv.Remove(d.items[i].index, d.items[i].count);
    const int64_t spend = d.stmt.money + _rules.sendFee;
    if (spend > 0)
        inv.SpendMoney(spend);
    d.stmt.characterId = found.foundId;
    d.ready = true;
    p->_mailOp = CPlayer::MailOp::SEND_SAVE;

    auto& counters = _gameMonitor._inventory;
    if (d.stmt.money > 0)
        counters._moneyOut[static_cast<int>(MoneySink::MAIL_SEND)].Add(d.stmt.money);
    if (_rules.sendFee > 0)
        counters._moneyOut[static_cast<int>(MoneySink::MAIL_FEE)].Add(_rules.sendFee);
    _gameMonitor._mail._sent.Inc();
    SLOG_INFO("[MAIL] send from={} to={} key={} items=[{}] money {} fee {}",
              ToInt(p->_characterId), found.foundId, KeyHex(d.stmt.sendKey), ItemsText(d), d.stmt.money, _rules.sendFee);
    return MailResult::OK;
}

// ==========================================================================
// 받기
// ==========================================================================
MailResult CMailSystem::CheckTake(const CPlayer* p, int64_t money, const MailAttach::Items& attach) const
{
    // 복사본에 넣어 본다 — 원본은 안 바뀐다(거래의 맞바꾸기와 같은 꼴 · TradeSystem.cpp)
    CInventory copy = p->_inventory;
    for (int i = 0; i < attach.count; ++i)
    {
        const ItemDef* def = _tables.Item().Find(attach.items[i].itemId);
        if (def == nullptr)
            return MailResult::ITEM_UNKNOWN;
        static_assert(static_cast<int>(InvAddResult::COUNT) == 5, "InvAddResult 를 늘렸으면 우편 받기 사유를 여기서 정할 것");
        switch (copy.TryAdd(*def, attach.items[i].count))
        {
        case InvAddResult::OK:           break;
        case InvAddResult::TAB_FULL:     return MailResult::BAG_FULL;
        case InvAddResult::MONEY_FULL:   return MailResult::MONEY_FULL;    // 돈 아이템 첨부(보내기 칸에서는 안 생긴다 — 돈은 칸이 아니다)
        case InvAddResult::UNKNOWN_ITEM: return MailResult::ITEM_UNKNOWN;
        case InvAddResult::BAD_COUNT:    return MailResult::ITEM_UNKNOWN;  // 첨부 풀기가 1 미만을 거른다 — 여기 오면 바이트가 깨진 것
        case InvAddResult::COUNT:        return MailResult::ITEM_UNKNOWN;
        }
    }
    if (money > 0 && !copy.TryAddMoney(money))
        return MailResult::MONEY_FULL;
    return MailResult::OK;
}

MailResult CMailSystem::FinishTake(CPlayer* p, const DBMailReply& reply)
{
    assert(p->_mailOp == CPlayer::MailOp::TAKE_READ);
    // 차례(mail-plan.md 5절 받기 4번): CheckAgain → DB_ERROR → GONE → ApplyTake(NO_ATTACH → ITEM_UNKNOWN → BAG_FULL · MONEY_FULL)
    MailResult r = CheckAgain(p);
    if (r == MailResult::OK && !reply.ok)    r = MailResult::DB_ERROR;
    if (r == MailResult::OK && !reply.found) r = MailResult::GONE;     // 없음 · 이미 받음 · 기한 지남 · 지움 · 남의 우편 번호
    if (r == MailResult::OK)                 r = ApplyTake(p, reply.row);
    if (r != MailResult::OK)
        End(p);
    return r;
}

MailResult CMailSystem::ApplyTake(CPlayer* p, const DBMailRow& row)
{
    if (row.attach.len == 0 && row.money == 0)
        return MailResult::NO_ATTACH;   // 글만 있는 우편 — 받기 버튼이 흐려 보통은 안 온다
    MailAttach::Items attach;
    if (!MailAttach::Read(row.attach.data, static_cast<int>(row.attach.len), attach) || row.money < 0)
    {
        SLOG_ERROR("[MAIL] take attach broken (character={}, mail={}, bytes={}, money={}) — 우편은 남는다",
                   ToInt(p->_characterId), row.mailId, row.attach.len, row.money);
        return MailResult::ITEM_UNKNOWN;
    }
    const MailResult r = CheckTake(p, row.money, attach);
    if (r != MailResult::OK)
    {
        if (r == MailResult::ITEM_UNKNOWN)
            SLOG_ERROR("[MAIL] take item not in table (character={}, mail={}, items=[{}]) — 우편은 남는다",
                       ToInt(p->_characterId), row.mailId, ItemsText(attach));
        return r;
    }

    // ── 통과 — 같은 판정으로 원본에 넣는다(위 복사본이 됐으니 원본도 된다 · 같은 게임 스레드라 그 사이에 안 바뀐다) ──
    CInventory& inv = p->_inventory;
    for (int i = 0; i < attach.count; ++i)
    {
        const InvAddResult added = inv.TryAdd(*_tables.Item().Find(attach.items[i].itemId), attach.items[i].count);
        assert(added == InvAddResult::OK);
        (void)added;
    }
    if (row.money > 0)
    {
        const bool ok = inv.TryAddMoney(row.money);
        assert(ok);
        (void)ok;
    }

    assert(!p->_mailDraft && "우편 문장은 한 사람에게 많아야 하나다");
    auto draft = std::make_unique<MailDraft>();
    draft->itemCount = static_cast<uint8_t>(attach.count);
    for (int i = 0; i < attach.count; ++i)
        draft->items[i] = MailDraftItem{ -1, attach.items[i].itemId, attach.items[i].count };   // 받기는 칸 색인이 여럿일 수 있어 안 적는다
    DBMailStmt& s = draft->stmt;
    s = DBMailStmt{};
    s.kind        = DBMailStmtKind::TAKE_UPDATE;   // taken_at=COALESCE(taken_at, NOW(3)) — 두 번 써도 처음 값이 남는다
    s.mailId      = row.mailId;
    s.characterId = ToInt(p->_characterId);
    s.money       = row.money;
    draft->ready = true;
    p->_mailDraft = std::move(draft);
    p->_mailOp = CPlayer::MailOp::TAKE_SAVE;

    if (row.money > 0)
        _gameMonitor._inventory._moneyIn[static_cast<int>(MoneySource::MAIL)].Add(row.money);
    _gameMonitor._mail._taken[static_cast<int>(MailTab::CHARACTER)].Inc();
    SLOG_INFO("[MAIL] take character={} mail={} items=[{}] money {} account_mail={}",
              ToInt(p->_characterId), row.mailId, ItemsText(attach), row.money, 0);
    return MailResult::OK;
}

#endif // USE_MAIL
