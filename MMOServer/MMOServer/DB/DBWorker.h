#pragma once

// ==========================================================================
// CDBWorker — DB 실행기: 저장 묶음과 불러오기를 실행하고 결과를 돌려준다
//
// [모델] 서버 메모리 = 진실, DB = 저장소.
//        게임루프(캐릭터 저장소)가 바뀐 캐릭터의 「묶음」(바뀐 캐릭터 행 + 저장 안 된 가방 칸)을 큐에 넣으면,
//        전용 워커 스레드가 MySQL 에 쓴다(문장이 둘 이상이면 트랜잭션 하나). 게임 틱은 막지 않는다.
//
// [설계] 송신 워커(CIOCPServer::SendWorker) 패턴을 그대로 복제:
//        게임스레드 push(lock+notify) / 워커스레드 swap-out 배치 처리.
//        잡은 값 타입 스냅샷이라 플레이어 삭제 후 실행돼도 안전(수명 무관).
//
// [왕복] 저장도 불러오기도 답이 돌아온다 — 저장은 회신(성공·실패), 불러오기는 결과.
//        무엇을 다시 실을지·불러오기를 들여보낼지는 받는 쪽(캐릭터 저장소)이 정한다 — 워커는 주인을 모르고 실행·회신만 한다.
//        가는 길은 「같은 큐」 — 같은 캐릭터는 같은 워커를 타고 큐는 앞에서부터 실행되므로
//        "옛 접속의 마지막 저장 → 새 접속의 불러오기" 차례가 지켜진다(그 저장이 실패했는지는 회신을 먼저 보는 받는 쪽이 가린다).
//        오는 길은 회신 상자 하나(저장 회신 · 불러오기 결과) — 워커가 넣고, 게임 루프가 틱마다 통째로 꺼낸다.
// ==========================================================================

#include <Identity/GameIds.h>
#include <Identity/CharacterName.h>       // 소유 표 이름 칸 크기
#include <Identity/CharacterLookBlob.h>   // 소유 표 외형 칸 크기
#include <Db/DbConnection.h>   // DBConfig — 세 서버가 같이 쓴다
#include "BuildConfig.h"

#include <cstdint>
#include <string>

// characters 의 hp·mp 열에서 「저장된 적 없음」 — 들어올 때 가득 채운다(CStatSystem::RestoreHpMp 는 음수를 이렇게 읽는다).
//   열을 붙이기 전 행의 기본값이고(schema.sql), 새 캐릭터·전투나 스킬을 끈 빌드도 이 값을 싣는다.
//   0 이 아닌 이유 — 체력 0 은 「죽은 채 저장됨」이라 부활 규칙을 탄다(CEntryHandler::OnCharacterLoaded).
//   (USE_DB_WORKER 밖에 두는 이유 — 입장 판정 구조체(EnterPlacement)의 기본값이라 토글을 끈 빌드도 쓴다)
constexpr int32_t DB_HP_MP_UNSAVED = -1;

#if USE_DB_WORKER

#include <thread>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include <vector>
#include <atomic>
#include <memory>
#include <cstddef>

class CMonitorManager;

// 바이트 열 하나 — 길이가 바뀌는 VARBINARY 를 고정 크기 버퍼로 나른다. 값 타입이라 큐 복사·union 에 그대로 든다.
//   내용(형식)은 모른다 — 묶고 푸는 쪽은 게임 스레드(CooldownBuffBlob.h). len 0 = 빈 값.
//   len 이 unsigned long 인 이유 — MYSQL_BIND::length 가 이 타입을 가리킨다(바인딩이 이 자리를 그대로 준다).
//   ★ 불러온 값이 N 보다 길면 len 은 원래 길이(N 초과)로 오고 data 엔 앞 N 바이트만 든다 — 읽는 쪽이 len > N 을 불량으로 본다.
template <size_t N>
struct DBBytes
{
    unsigned long len;
    uint8_t       data[N];
};

// 쿨타임·버프 열의 버퍼 크기 — 형식(CooldownBuffBlob.h)의 최대 길이와 같다(기술 목록 24개 = 2 + 24×10 · 버프 표 16줄 = 2 + 16×10).
//   기술 목록 상한·버프 표 상한을 늘려 형식이 커지면 CharacterStore.cpp 의 static_assert 가 여기를 올리라고 막는다. 열 자체는 VARBINARY(255).
constexpr size_t DB_SKILL_COOLDOWNS_BYTES = 242;
constexpr size_t DB_BUFFS_BYTES           = 162;
constexpr size_t DB_ROOM_LOCKOUTS_BYTES   = 194;   // RoomLockoutBlob::MAX_BYTES 와 같다(CharacterStore.cpp static_assert)

// 캐릭터 행 값 = 값 스냅샷 (포인터 없음 → 플레이어가 사라져도 안전). 저장 묶음의 머리(DBSaveBundle::row)이자 행 문장의 버퍼 꼴.
//   ※ level·exp 는 USE_EXP 로, hp·mp 는 USE_COMBAT·USE_SKILL 로 안 가른다 — 열은 테이블에 늘 있고, 숫자 넷을 싣는 값이
//     "토글마다 문장이 갈리는" 값보다 훨씬 싸다. 토글이 가르는 것은 "값이 바뀌는 코드" 쪽이다.
//     쿨타임·버프 묶음(USE_SKILL·USE_COMBAT)도 같다 — 토글을 끈 빌드는 빈 값을 싣는다.
//   ※ DB 열과의 대응은 DBWorker.cpp 의 열 표(kCharColumns) 한 곳에만 있다 — 열을 늘리면
//     여기와 DBResult 에 같은 이름의 멤버를 넣고 표에 한 줄. SQL·바인딩·복사는 표에서 나온다.
struct DBCharRow
{
    int64_t characterId;
    float   x;
    float   y;
    int32_t mapId;
    int32_t level;
    int64_t exp;
    int32_t hp;      // 현재 체력 — 최대치는 저장하지 않는다(들어올 때 레벨 표·장비로 다시 세운다). DB_HP_MP_UNSAVED = 모름
    int32_t mp;      // 현재 마력 — 같은 규칙
    DBBytes<DB_SKILL_COOLDOWNS_BYTES> skillCooldowns;   // 돌고 있는 스킬 쿨타임 묶음 — 빈 값 = 없음(스킬을 끈 빌드도 빈 값)
    DBBytes<DB_BUFFS_BYTES>           buffs;            // 걸려 있는 버프 묶음 — 같은 규칙(전투를 끈 빌드도 빈 값)
    int32_t job;     // 직업(JobTable.csv) — 0 = 아직 안 고름
    DBBytes<DB_ROOM_LOCKOUTS_BYTES> roomLockouts;   // 보스방 다시 들어가기 잠금 묶음 — 빈 값 = 없음(형식은 RoomLockoutBlob.h)
    int64_t money;   // 가진 돈(전) — 열은 가방을 켠 빌드만 쓴다(DBWorker.cpp kCharColumns). 칸은 늘 있어 열 표의 offsetof 가 빌드마다 같다
};

// 가방 한 줄 — 불러온 줄(DB 값 그대로 · 검사는 게임 스레드가 한다: 칸 범위·아이템 표·탭·쌓임 한도)이자 저장 묶음의 칸 줄(최종값).
//   칸 번호를 int32 로 받는 이유 — 행이 망가져 있으면 음수나 큰 값일 수 있다. 줄이기 전에 걸러야 한다.
//   멤버 이름은 DBInvRowBuf 와 같아야 한다 — 열 표(DBWorker.cpp kInvColumns)가 이름으로 잇는다.
//   (USE_INVENTORY 밖에 두는 이유 — 저장 묶음 넣기 함수가 이 꼴을 받는다. 가방을 끈 빌드는 늘 0줄이다)
struct DBInvRow
{
    int32_t slot;
    int32_t itemId;
    int32_t count;
};

// 저장 묶음에 실을 덩어리 — 캐릭터당 하나인 덩어리마다 비트 하나(save-skip-row-plan.md). 여러 줄인 덩어리(가방 칸)는 줄 수(slotCount)로 표시한다.
//   0 = 「안 씀」이라, 묶음을 만드는 자리(CCharacterStore::PrepareBundle · LeaveFinal)가 반드시 정한다.
constexpr uint8_t SAVE_PART_CHAR_ROW = 1u << 0;   // characters 행 — 행이 바뀌었을 때만(PrepareBundle 이 CharRowChanged 로 켠다)
constexpr uint8_t SAVE_PART_ALL      = SAVE_PART_CHAR_ROW;   // 나갈 때·종료 — 판정 없이 다 쓴다(덩어리 비트를 늘리면 여기에도 더할 것)

// 저장 묶음의 머리 — 실을 덩어리 + 캐릭터 행 값 + 칸 줄 수 + 주인 + 번호(save-unify-plan.md 「묶음」).
//   칸 줄은 원소 안에 두지 않는다 — 슬롯 큐 옆 줄 벡터(DBWorkerSlot::rows)에 이어 붙이고 시작 자리만 든다.
//   원소 안 고정 배열이면 대부분 0줄인 주기 묶음마다 칸 61줄(732B)이 붙는다.
//   문장이 하나면(행만 · 칸 1줄만) 그 자체가 트랜잭션, 둘 이상이면 START TRANSACTION … COMMIT 으로 감싼다.
// 저장 묶음의 주인 없음 — 저장소의 떠난 기록 · 종료 때 묶음(회신은 캐릭터 번호로 찾는다)
inline constexpr SessionId NO_OWNER_SESSION{ 0 };

struct DBSaveBundle
{
    DBCharRow row;         // 캐릭터 행 10열(돈 열은 가방을 켠 빌드만) + 캐릭터(열 표 kCharColumns 가 이 꼴의 멤버를 가리킨다)
                           //   행 비트가 꺼져 있어도 캐릭터 번호는 늘 쓴다(큐 고르기 · 칸 문장 · 회신 · 로그)
    SessionId   sessionId;   // 주인 — 살아 있는 사람의 세션 번호 · 0 = 저장소의 떠난 기록·종료(회신은 캐릭터 번호로 찾는다)
    uint32_t  bundleNo;    // 회신과 짝을 맞추는 번호(0 은 안 씀)
    uint32_t  slotBegin;   // 줄 벡터 안 시작 자리 — 넣는 함수가 매긴다(부르는 쪽 값은 무시)
    int32_t   slotCount;   // 칸 줄 수(0 ~ 가방 칸 수)
    uint8_t   parts;       // 실을 덩어리 비트(SAVE_PART_*) — 칸은 slotCount 가 표시한다. 비트도 칸도 없는 묶음은 넣지 않는다(PushBundleLocked)
    uint8_t   pairNext;    // 1 = 큐에서 바로 뒤 머리와 한 트랜잭션(짝 묶음 — 거래 · EnqueueSavePair 만 켠다) · 0 = 혼자
    int32_t   ownerZone;   // 주인 존 서버 번호 — 불러올 때 받은 값. 저장 때 DB 의 값과 맞춰 본다(펜스 · world-server-plan.md 2절 3·4번)
    uint64_t  ownerSeq;    // 주인이 바뀐 횟수 — 위와 같다. 떠난 기록에도 같이 간다
    uint32_t  mailBegin;   // 우편 문장 벡터 안 시작 자리 — 칸 줄처럼 넣는 함수가 매긴다(DBWorkerSlot::mails · 부르는 쪽 값은 무시)
    uint8_t   mailCount;   // 우편 문장 수(0 또는 1 — 한 사람의 우편 동작은 한 번에 하나 · mail-plan.md 5절). 칸 문장 뒤에 실행한다
};

// 저장 묶음의 결말 — 셋을 가른다. FENCED 는 실패와 다르다: 실패는 다시 실으면 되지만, FENCED 는 이 캐릭터의 주인이 다른 존 서버로
//   옮겨 갔다는 사실이라 다시 실으면 안 된다(받는 쪽이 끊는다 · CCharacterStore::OnSaveReply).
enum class DBSaveResult : uint8_t
{
    OK,
    FAILED,
    FENCED,
    COUNT
};

// 저장 회신 — 모든 묶음이 하나씩 받는다(성공 · 실패 · 펜스). 무엇을 다시 실을지는 받는 쪽(저장소)이 정한다 — 워커는 주인을 모른다.
struct DBSaveReply
{
    CharacterId  characterId;
    SessionId  sessionId;   // 묶음에 실어 보낸 값 그대로
    uint32_t bundleNo;
    DBSaveResult result;
};

// 불러오기 잡 — 캐릭터 번호로 캐릭터 행을 읽는다. 저장과 달리 결과가 회신 큐로 돌아온다.
struct DBLoadJob
{
    CharacterId characterId;
    SessionId sessionId;   // 결과의 주인. 게임 스레드가 세션 번호 "전체"로 대조한다(자리 재사용 방어)
    int64_t requestNs;   // 요청 시각(steady_clock, ns) — 요청→결과 수령 지연을 재는 기준
    bool    readRoster;  // 소유 표(이름·몸)도 읽나 — 입장 번호 길만 true. 시험 입장 길은 소유 구간이 막혀 행이 있을 수 없고 더미 DB 왕복이 그대로여야 한다
};

#if USE_INVENTORY
// 가방 한 줄 문장의 버퍼 꼴(USE_INVENTORY) — 캐릭터 + 칸 + 아이템 + 개수. 칸 UPSERT 의 파라미터 · 가방 SELECT 의 파라미터·결과 열.
//   저장 묶음의 칸 줄(DBInvRow)을 실행 직전에 캐릭터와 함께 여기로 옮긴다(RunBundleOnce). 칸 줄은 최종값이라 두 번 써도 같다.
struct DBInvRowBuf
{
    int64_t characterId;
    int32_t slot;        // 칸 번호 (Protocol.h INV_TAB_RANGES)
    int32_t itemId;      // 0 = 빈 칸 — 행을 지우지 않고 0 으로 덮는다(DB 계정에 DELETE 권한이 없다)
    int32_t count;
};

// 결과에 실을 수 있는 가방 줄 수 — 칸 60 의 두 배. 망가진 행이 섞여 있어도 멀쩡한 행을 잃지 않게 넉넉히 둔다.
//   넘는 줄은 싣지 않고 수만 센다(DBResult::invOverflow → 게임 쪽 "too_many_rows" 카운터).
constexpr int DB_INV_MAX_ROWS = 120;
#endif

// 저장 파이프라인 지표(저장 성공·실패·드롭·큐 깊이·워커 수) — 위와 같은 쪽(GameMonitor.h), 생성자로 참조를 받는다.
//   10-06 코어 CMonitorManager::_db* 에서 옮김 — DB 워커는 게임 프로젝트 구성요소라 코어 그릇을 안 본다.
struct DBSaveCounters;

// 작업 종류 — 큐 원소에 붙는 표시. 워커와 게임 루프가 switch 로 가른다.
//   ※ 종류를 늘리면 COUNT 가 바뀌고, 각 switch 옆의 static_assert 가 컴파일을 막아
//     빠뜨린 case 를 찾아가게 한다. (이 프로젝트 빌드 옵션엔 -Wswitch·C4062 가 꺼져 있어
//     컴파일러 경고로는 못 잡는다 — 그래서 경고 대신 단언에 맡긴다.)
enum class DBJobKind : uint8_t
{
    LOAD_CHARACTER,    // 캐릭터 행 + 가방 SELECT — 왕복. 버리지 않는다(버리면 그 사람이 못 들어온다)
    SAVE_BUNDLE,       // 저장 묶음(캐릭터 행 0~1 + 칸 0~N · 실을 덩어리는 DBSaveBundle::parts) — 트랜잭션 하나 · 회신 하나(성공·실패). 다시 싣기는 저장소가 정한다
    MAIL,              // 우편 잡(DBMailJob — 읽기·표시) — 왕복. 회신은 우편 회신 목록으로(ResultBox::mails). 우편을 끈 빌드는 안 낸다
    COUNT
};
constexpr int DB_JOB_KIND_COUNT = static_cast<int>(DBJobKind::COUNT);

// 소유 표(character_roster) 칸 크기 — name = VARCHAR(6) utf8mb4(글자당 최대 4바이트) · look = VARBINARY(32)
constexpr size_t DB_ROSTER_NAME_BYTES = CHAR_NAME_MAX_LEN * 4;
constexpr size_t DB_ROSTER_LOOK_BYTES = CharacterLookBlob::COLUMN_BYTES;

// 소유 표 한 줄의 버퍼 꼴 — characterId 가 WHERE 파라미터, 나머지가 결과(존은 이 표를 읽기만 한다).
//   멤버 이름이 DBResult 와 같아야 한다(열 표가 이름으로 잇는다)
struct DBRosterRowBuf
{
    int64_t characterId;
    DBBytes<DB_ROSTER_NAME_BYTES> rosterName;
    DBBytes<DB_ROSTER_LOOK_BYTES> rosterLook;
};

// 주인 열 둘의 버퍼 꼴 — 잠금 읽기(characterId 가 파라미터, 나머지가 결과) · 주인 적기(셋 다 파라미터).
//   열 표(kCharColumns)에 안 넣는 까닭 — 표에 넣으면 UPSERT 의 ON DUPLICATE KEY UPDATE 에도 붙어 저장마다 주인을 덮어쓴다.
struct DBOwnerBuf
{
    int64_t  characterId;
    int32_t  ownerZone;
    uint64_t ownerSeq;
};

// ── 우편(mail-plan.md 5절 · 8절) ──
//   값 타입은 USE_MAIL 밖에 둔다 — 저장 묶음 넣기 함수가 이 꼴을 받는다(DBInvRow 와 같은 까닭). 우편을 끈 빌드는 묶음의 우편 문장이
//   늘 0 개이고 우편 잡을 안 낸다. 문장 준비·실행만 USE_MAIL 안이다(DBWorker.cpp).
//   글자 칸은 UTF-8 바이트다 — 열이 utf8mb4 VARCHAR 라 글자당 최대 4바이트(schema.sql character_mail).
constexpr size_t DB_MAIL_NAME_BYTES     = DB_ROSTER_NAME_BYTES;   // from_name VARCHAR(6) — 소유 표 이름 칸과 같은 꼴(이름 읽기가 그 문장을 같이 쓴다)
constexpr size_t DB_MAIL_TITLE_BYTES    = 20 * 4;    // title VARCHAR(20) — 글자 수는 Protocol.h MAIL_TITLE_MAX 와 같다(MailDraft.h static_assert)
constexpr size_t DB_MAIL_BODY_BYTES     = 200 * 4;   // body VARCHAR(200) — MAIL_BODY_MAX 와 같다(같은 자리)
constexpr size_t DB_MAIL_ATTACH_BYTES   = 66;        // attach — MailAttach::MAX_BYTES 와 같다(같은 자리). 열은 VARBINARY(255) — 더 긴 값은 len 이 넘쳐 불량으로 읽힌다
constexpr size_t DB_MAIL_SEND_KEY_BYTES = 16;        // send_key BINARY(16)

// 저장 묶음에 실리는 우편 문장의 종류 — 묶음 하나에 많아야 하나다(DBSaveBundle::mailCount).
//   셋 다 두 번 써도 결과가 같다 — COMMIT 도중 끊기면 묶음째 다시 쓰기 때문이다(RunBundleOnce).
enum class DBMailStmtKind : uint8_t
{
    SEND_INSERT,       // 보내기 — 받는 사람의 우편 한 줄. send_key 가 같으면 아무것도 안 바꾼다
    TAKE_UPDATE,       // 받기 — 내 우편의 taken_at. COALESCE 라 처음 값이 남는다
    ACC_TAKE_INSERT,   // 계정 우편 받기 — 받은 상태(taken_at)로 내 우편 한 줄. from_account_mail_id 가 같으면 아무것도 안 바꾼다
    COUNT
};

// 우편 문장 값 — 묶음의 칸 줄처럼 원소 밖(DBWorkerSlot::mails)에 이어 붙이고 머리는 자리만 든다(DBSaveBundle::mailBegin).
//   만드는 쪽은 게임 스레드(우편 규칙) · 싣는 쪽은 저장소 · 실행은 워커. 실행 직전 슬롯 버퍼(bMail)로 통째로 옮긴다.
struct DBMailStmt
{
    DBMailStmtKind kind;
    int64_t mailId;              // TAKE_UPDATE 의 대상 우편
    int64_t characterId;         // 우편 줄의 주인 — SEND_INSERT = 받는 사람 · 나머지 = 묶음의 캐릭터(나)
    int64_t accountId;           // ACC_TAKE_INSERT — 확정 뒤 계정 서버에 DONE 을 보낼 계정(떠난 기록으로 확정돼도 보낸다 · mail-plan.md 6절 7번)
    int64_t fromCharacterId;     // SEND_INSERT — 보낸 캐릭터
    int64_t money;
    int64_t fromAccountMailId;   // ACC_TAKE_INSERT — 계정 우편 번호(UNIQUE 가 두 번 받기를 막는다)
    int32_t expireDays;          // 기한 일수 — expires_at 은 DB 시각으로 센다(DATE_ADD(NOW(3), …) · 서버 시계를 안 쓴다)
    DBBytes<DB_MAIL_NAME_BYTES>     fromName;   // SEND_INSERT — 보낸 사람 이름
    DBBytes<DB_MAIL_TITLE_BYTES>    title;
    DBBytes<DB_MAIL_BODY_BYTES>     body;
    DBBytes<DB_MAIL_ATTACH_BYTES>   attach;     // 첨부 바이트(MailAttach.h) — 빈 값 = 없음
    DBBytes<DB_MAIL_SEND_KEY_BYTES> sendKey;    // SEND_INSERT — 다시 쓰기 방지 난수(len 16)
};

// 우편 잡 — 저장 묶음 밖의 읽기·표시. 결과가 회신(DBMailReply)으로 돌아온다(mail-plan.md 8절).
enum class DBMailOp : uint8_t
{
    FIND_RECEIVER,   // 받는 이름 → 캐릭터 번호 · 지움 표시 · 그 사람의 살아 있는 우편 수(받는 사람이 접속 중이 아닐 수 있어 DB 에서 센다)
    LIST,            // 내 살아 있는 우편 줄 — 본문 없음 · 새것부터 · 최대 DB_MAIL_LIST_MAX
    READ_BODY,       // 한 통 읽기 + read_at 적기
    READ_ONE,        // 받기 판정용 한 통 읽기 — read_at 은 안 적는다
    ERASE,           // deleted_at 적기 — 첨부·돈이 없는 우편만(DB 계정에 DELETE 권한이 없다)
    COUNT_PENDING,   // 붉은 수 — 살아 있는 우편 중 안 읽었거나 첨부·돈이 남은 것
    FIND_MOVED,      // 계정 우편 번호로 받은 줄이 있나 — 「이미 옮겼나」(mail-plan.md 6절)
    FIND_NAME,       // 캐릭터 번호 → 이름 — 「다른 캐릭터가 받는 중」 안내
    COUNT
};
constexpr int DB_MAIL_LIST_MAX = 100;   // 목록 줄 상한 — 핸들러가 10줄씩 나눠 보낸다(mail-plan.md 5절)

// 우편 잡 값 — 큐 원소 union 에 든다(값 타입 · 저장 묶음 머리보다 작다 — DBJob 옆 static_assert)
struct DBMailJob
{
    DBMailOp    op;
    SessionId   sessionId;          // 회신의 주인 — 게임 스레드가 세션 번호 전체 + 캐릭터로 사람을 다시 찾는다
    CharacterId characterId;        // 요청한 캐릭터 — 큐 고르기(같은 사람의 앞선 저장 뒤에 선다) · 「내 우편」 조건
    int64_t     mailId;             // READ_BODY · READ_ONE · ERASE = 내 우편 번호 · FIND_MOVED = 계정 우편 번호
    CharacterId otherCharacterId;   // FIND_NAME — 이름을 읽을 캐릭터
    DBBytes<DB_MAIL_NAME_BYTES> name;   // FIND_RECEIVER — 받는 이름
};

// 우편 목록 한 줄 — 본문을 뺀 줄(S2C_MAIL_LIST 한 줄의 재료). 바이트는 DB 값 그대로 — 검사·풀기는 게임 스레드가 한다
struct DBMailBrief
{
    int64_t mailId;
    int64_t money;
    int32_t remainSec;   // 기한까지 남은 초(DB 시각)
    int32_t read;        // 1 = 읽음(read_at 이 있다)
    DBBytes<DB_MAIL_NAME_BYTES>   fromName;
    DBBytes<DB_MAIL_TITLE_BYTES>  title;
    DBBytes<DB_MAIL_ATTACH_BYTES> attach;
};

// 우편 한 통 — 본문까지(READ_BODY · READ_ONE)
struct DBMailRow
{
    int64_t mailId;
    int64_t fromCharacterId;
    int64_t money;
    DBBytes<DB_MAIL_NAME_BYTES>   fromName;
    DBBytes<DB_MAIL_TITLE_BYTES>  title;
    DBBytes<DB_MAIL_BODY_BYTES>   body;
    DBBytes<DB_MAIL_ATTACH_BYTES> attach;
};

// 우편 잡 회신 — 잡마다 하나(성공·실패 · 회신이 반드시 온다 — 사람별 우편 상태가 이것으로 풀린다).
//   ★ ok=false(못 읽음)와 found=false(없음)를 섞지 않는다 — 없음은 GONE·NO_TARGET 이고 못 읽음은 DB_ERROR 다.
struct DBMailReply
{
    DBMailOp    op;
    SessionId   sessionId;       // 잡에 실어 보낸 값 그대로
    CharacterId characterId;
    int64_t     mailId;          // 잡에 실어 보낸 값 그대로
    bool        ok = false;      // false = 조회 실패(커넥션 · 쿼리 오류) — 아래 값은 쓰지 않는다
    bool        found = false;   // 행이 있나 — FIND_RECEIVER(이름) · READ_*(우편) · ERASE(바뀐 행) · FIND_MOVED(받은 줄) · FIND_NAME(캐릭터)
    bool        targetDeleted = false;   // FIND_RECEIVER — 받는 캐릭터가 지움 표시돼 있다
    int64_t     foundId = 0;     // FIND_RECEIVER = 받는 캐릭터 번호 · FIND_MOVED = 받은 줄의 mail_id
    int64_t     count = 0;       // FIND_RECEIVER = 받는 사람의 살아 있는 우편 수 · COUNT_PENDING = 붉은 수 · ERASE = 바뀐 행 수
    DBBytes<DB_MAIL_NAME_BYTES> name{};   // FIND_NAME — 이름
    DBMailRow   row{};           // READ_BODY · READ_ONE
    std::vector<DBMailBrief> lines;   // LIST — 새것부터
};

#if USE_MAIL
// 우편 잡 문장의 버퍼 꼴 — 파라미터(캐릭터 · 우편 번호 · 이름)와 숫자 결과(번호 · 표시 · 수). 잡마다 쓰는 칸만 채운다
struct DBMailJobBuf
{
    int64_t characterId;
    int64_t mailId;
    DBBytes<DB_MAIL_NAME_BYTES> name;
    int64_t foundId;   // 결과 — 받는 캐릭터 번호 · 받은 줄 번호
    int32_t flag;      // 결과 — 지움 표시(0·1)
    int64_t count;     // 결과 — 우편 수
};
#endif

// 준비해 두는 문장 — 작업 종류와 따로 센다. 저장 묶음은 캐릭터 행 + 칸 문장을, 불러오기는 캐릭터 행 + 가방 문장을 쓴다
//   (전에는 종류마다 문장 하나 + 불러오기의 둘째 문장이 따로였다 — 묶음도 둘째 문장이 생겨 표로 모음).
enum class DBStmt : uint8_t
{
    CHAR_UPSERT,       // characters 행 UPSERT — 저장 묶음의 첫 문장
    CHAR_SELECT,       // characters 행 SELECT — 불러오기의 첫 문장
#if USE_INVENTORY
    SLOT_UPSERT,       // inventory 칸 UPSERT — 저장 묶음의 칸 문장
    INV_SELECT,        // inventory 줄 SELECT — 불러오기의 둘째 문장
#endif
    ROSTER_SELECT,     // character_roster 이름·몸 SELECT — 불러오기의 마지막 문장(입장 번호 길만)
    OWNER_SELECT,      // 주인 열 잠금 읽기(FOR UPDATE) — 불러오기·저장 묶음의 첫 문장
    OWNER_UPDATE,      // 주인 열 적기 — 불러올 때 주인이 바뀌었으면 · 행 없던 캐릭터의 첫 저장 뒤
#if USE_MAIL
    MAIL_SEND_INSERT,      // 저장 묶음의 우편 문장 — 보내기
    MAIL_TAKE_UPDATE,      // 저장 묶음의 우편 문장 — 받기
    MAIL_ACC_TAKE_INSERT,  // 저장 묶음의 우편 문장 — 계정 우편 받기
    MAIL_FIND_RECEIVER,    // 우편 잡 — 받는 이름 찾기 + 우편 수
    MAIL_LIST,             // 우편 잡 — 목록
    MAIL_READ,             // 우편 잡 — 한 통 읽기(READ_BODY · READ_ONE)
    MAIL_MARK_READ,        // 우편 잡 — read_at 적기(READ_BODY)
    MAIL_ERASE,            // 우편 잡 — deleted_at 적기
    MAIL_COUNT_PENDING,    // 우편 잡 — 붉은 수
    MAIL_FIND_MOVED,       // 우편 잡 — 받은 줄 찾기(FIND_NAME 은 ROSTER_SELECT 를 같이 쓴다)
#endif
    COUNT
};
constexpr int DB_STMT_COUNT = static_cast<int>(DBStmt::COUNT);

// 큐 원소 = 종류 + 몸. 몸은 둘 다 값 타입(POD)이라 union 으로 겹친다 — 수명 무관, 복사는 memcpy.
//   저장 묶음의 칸 줄은 원소 밖(DBWorkerSlot::rows)이라 원소 크기는 묶음 머리 크기로 묶인다.
struct DBJob
{
    DBJobKind kind;
    union
    {
        DBLoadJob    load;
        DBSaveBundle bundle;
        DBMailJob    mail;
    };

    static DBJob Load(const DBLoadJob& l) { DBJob j; j.kind = DBJobKind::LOAD_CHARACTER; j.load = l; return j; }
    static DBJob Bundle(const DBSaveBundle& b) { DBJob j; j.kind = DBJobKind::SAVE_BUNDLE; j.bundle = b; return j; }
    static DBJob Mail(const DBMailJob& m) { DBJob j; j.kind = DBJobKind::MAIL; j.mail = m; return j; }
};
static_assert(sizeof(DBMailJob) <= sizeof(DBSaveBundle), "우편 잡이 큐 원소를 키운다 — 큰 값은 원소 밖에 둘 것(우편 문장처럼)");

// 불러오기 결과 — 셋을 가른다.
//   ★ NOT_FOUND 와 FAILED 를 절대 섞지 않는다. 실패를 "없음"으로 받으면 새 캐릭터로 스폰되고,
//     다음 저장(UPSERT)이 진짜 행을 덮어쓴다. 0("없었다")과 "못 읽었다"는 다른 사실이다.
enum class DBResultCode : uint8_t
{
    FOUND,        // 행이 있다 — x·y·mapId 유효
    NOT_FOUND,    // 행이 없다 — 새 캐릭터(정상)
    FAILED,       // 조회 실패(커넥션 없음·쿼리 오류) — 들여보내면 안 된다
    COUNT
};

// 회신 큐 원소 — 워커가 채워 넣고 게임 루프가 꺼낸다. 값 타입이라 요청자가 사라져도 안전하다.
struct DBResult
{
    DBJobKind    kind;
    DBResultCode code;
    CharacterId      characterId;
    SessionId      sessionId;   // 요청 때 실어 보낸 값 그대로
    int64_t      requestNs;   // 요청 때 실어 보낸 값 그대로
    float        x;           // ↓ FOUND 일 때만 유효 — 멤버 이름이 DBCharRow 와 같아야 한다(열 표가 이름으로 잇는다)
    float        y;
    int32_t      mapId;
    int32_t      level;
    int64_t      exp;
    int32_t      hp;          // DB_HP_MP_UNSAVED = 저장된 적 없음 · 0 = 죽은 채 저장됨(OnCharacterLoaded 가 부활 규칙으로 바꾼다)
    int32_t      mp;
    DBBytes<DB_SKILL_COOLDOWNS_BYTES> skillCooldowns;   // 바이트 그대로 — 검사·풀기는 게임 스레드(SpawnPlayer). len > 버퍼 = 잘림(불량)
    DBBytes<DB_BUFFS_BYTES>           buffs;
    DBBytes<DB_ROOM_LOCKOUTS_BYTES>   roomLockouts;   // 바이트 그대로 — 풀기는 게임 스레드(RestoreRoomLockouts)
    int32_t      job;         // DB 값 그대로 — 표에 있는 번호인지는 게임 스레드(OnCharacterLoaded)가 본다
    // ↓ FOUND·NOT_FOUND 둘 다 유효 — 이 값을 들고 있다가 저장 묶음마다 싣는다(펜스). 행이 없으면 (내 존 서버 번호, 0)
    int32_t      ownerZone;
    uint64_t     ownerSeq;
    // ↓ FOUND·NOT_FOUND 둘 다 유효 — 소유 표는 characters 행과 따로 있다. 바이트 그대로 — 검사·풀기는 게임 스레드(SpawnPlayer).
    //   rosterFound = false 는 「행이 없다」는 사실이다. 조회가 실패하면 code 가 FAILED 다 — 없음과 못 읽음을 섞지 않는다.
    bool         rosterFound;
    DBBytes<DB_ROSTER_NAME_BYTES> rosterName;
    DBBytes<DB_ROSTER_LOOK_BYTES> rosterLook;
    int64_t      money;       // DB 값 그대로 — 범위(0~MONEY_MAX) 검사는 게임 스레드(CCharacterStore::LoadInventoryRows — 망가진 값은 안 넣는다)
#if USE_INVENTORY
    // ↓ FOUND·NOT_FOUND 둘 다 유효 — 캐릭터 행이 없어도 가방은 읽는다(칸만 쓰는 묶음은 행이 이미 써진 뒤에만 나오지만 —
    //   새 캐릭터는 첫 묶음에 _dbDirty 로 행이 실린다 —, 저장 묶음 전의 칸 즉시 저장 시절에 생긴 「가방 행만 있는 캐릭터」이 DB 에 남아 있을 수 있다).
    //   가방 조회가 실패하면 code 는 FAILED 다 — 빈 가방으로 들여보내면 첫 아이템이 진짜 행을 덮는다.
    int32_t      invCount;     // inv 에 실린 줄 수
    int32_t      invOverflow;  // DB_INV_MAX_ROWS 를 넘어 못 실은 줄 수 (정상이면 0)
    DBInvRow     inv[DB_INV_MAX_ROWS];
#endif
};

// 워커 슬롯 — 스레드/커넥션/큐 1세트 (CIOCPServer::SendWorker 패턴).
//   mutex/cv는 이동불가라 unique_ptr로 힙 고정 후 포인터만 벡터에 보관.
struct DBWorkerSlot
{
    std::thread              thread;
    std::mutex               mutex;
    std::condition_variable  cv;
    std::vector<DBJob>       queue;           // 게임 push / 워커 swap-out
    std::vector<DBInvRow>    rows;            // 저장 묶음의 칸 줄 — queue 와 같은 락에서 같이 맞바꾼다(묶음 머리가 시작 자리·줄 수를 든다)
    std::vector<DBMailStmt>  mails;           // 저장 묶음의 우편 문장 — rows 와 같은 꼴(묶음 머리의 mailBegin · mailCount)
    size_t                   loadCount = 0;   // queue 안의 불러오기 수(mutex 보호) — 저장 상한과 따로 센다
    size_t                   bundleCount = 0; // queue 안의 저장 묶음 수(mutex 보호) — 묶음끼리 센 상한(EnqueueSave)
    size_t                   mailJobCount = 0;   // queue 안의 우편 잡 수(mutex 보호) — 불러오기처럼 따로 센 상한(EnqueueMail)
    void*                    mysql = nullptr; // 실제 MYSQL* (헤더 오염 차단 위해 void*)
    std::chrono::steady_clock::time_point lastFailedConnect{};   // 마지막 재연결 실패 시각 — 기본값이면 실패 기록 없음(ReconnectDue)
    void*                    stmt[DB_STMT_COUNT] = {};   // MYSQL_STMT* — 문장별 prepared 핸들 (커넥션 종속, DBStmt 순서)
    DBCharRow                bSave{};         // 캐릭터 행 UPSERT 파라미터 버퍼 — execute 직전 묶음 머리의 행을 통째로 복사. 열 표(kCharColumns)가 멤버 자리를 가리킨다
    DBCharRow                bLoad{};         // 캐릭터 행 SELECT — characterId 가 WHERE 파라미터, 나머지 멤버가 결과 열(fetch 가 채운다)
#if USE_INVENTORY
    DBInvRowBuf              bItem{};               // 칸 UPSERT 파라미터 버퍼 — 칸 줄을 캐릭터와 함께 복사. 열 표(kInvColumns)가 멤버 자리를 가리킨다
    DBInvRowBuf              bInvLoad{};            // 가방 SELECT — characterId 가 WHERE 파라미터, 나머지 멤버가 결과 열(fetch 가 줄마다 채운다)
#endif
    DBRosterRowBuf           bRoster{};       // 소유 표 SELECT — characterId 가 WHERE 파라미터, 나머지 멤버가 결과
    DBOwnerBuf               bOwner{};        // 주인 잠금 읽기 — characterId 가 파라미터, 나머지가 결과
    DBOwnerBuf               bOwnerSet{};     // 주인 적기 — 셋 다 파라미터
#if USE_MAIL
    DBMailStmt               bMail{};         // 묶음의 우편 문장 파라미터 — 실행 직전 우편 문장을 통째로 복사(셋이 같은 버퍼를 다른 열 조합으로 묶는다)
    DBMailJobBuf             bMailJob{};      // 우편 잡의 파라미터·숫자 결과
    DBMailBrief              bMailBrief{};    // 목록 결과 한 줄 — fetch 가 줄마다 채우고 회신 벡터로 옮긴다
    DBMailRow                bMailRow{};      // 한 통 읽기 결과
#endif
    int                      index = 0;
};

// K개 워커 스레드 + 커넥션. characterId % K 고정 분배(같은 유저=같은 워커=UPSERT 순서 보장).
//   ★ 예외 하나 — 짝 묶음(거래 · EnqueueSavePair)은 두 캐릭터 중 작은 번호의 워커로 간다. 다른 쪽 사람의 순서는 큐가 아니라
//     저장소가 지킨다 — 짝이 확정될 때까지 그 사람의 다른 쓰기(혼자 묶음 · 떠난 기록)를 붙잡는다(CCharacterStore 「짝 저장」).
//   백프레셔: 저장 묶음끼리 센 수가 queueMax 에 닿으면 거절(카운터+ERROR) — 부르는 쪽이 표시를 되살리거나 떠난 기록을 남겨
//   다음 계기에 다시 싣는다. 살아 있는 캐릭터는 비행 중 묶음이 하나라 정상 상태에선 안 닿는다.
class CDBWorker
{
public:
    explicit CDBWorker(DBSaveCounters& counters);   // 저장 파이프라인 지표 자리 — 게임 그릇(CGameMonitor::_db)
    ~CDBWorker();

    // K 커넥션 + K 워커 스레드 시작. 하나라도 접속 실패 시 전체 롤백 후 false (fail-fast).
    //   zoneServerId = 이 존 서버의 번호 — 불러올 때 주인으로 적는다(펜스).
    //   reconnectMinSec = 재연결이 실패한 뒤 다시 해 보기까지의 최소 간격.
    bool Start(const DBConfig& config, int workerCount, int queueMax, int reconnectMinSec, ZoneServerId zoneServerId);

    // 게임루프 전용 — 불러오기 1건. 저장과 같은 슬롯 큐(characterId%K)에 넣어 캐릭터별 순서를 지킨다.
    //   저장 상한(queueMax)은 건너뛴다 — 저장은 버려도 다음 주기가 덮지만, 불러오기는 버리면 그 사람이
    //   영영 못 들어온다. 대신 큐에 든 불러오기 수를 따로 세서 같은 상한을 넘으면 거절한다
    //   (끝없이 쌓이는 큐를 만들지 않는다. 정상 상태에선 안 닿는 값 — 닿았다면 DB 가 멈춘 것).
    //   반환 false = 거절됨. 결과가 오지 않으므로 호출부가 그 자리에서 처리해야 한다.
    bool EnqueueLoad(const DBLoadJob& job);

    // 게임루프 전용 — 저장 묶음 1건(실을 덩어리 비트 + 칸 줄 n). 같은 캐릭터 = 같은 슬롯 큐(characterId%K) — 불러오기와 차례가 맞물린다.
    //   묶음끼리 센 수로 QueueMax 를 건다(불러오기 loadCount 와 같은 꼴 · 남의 원소까지 세지 않는다).
    //   반환 false = 거절(dropped 를 센다). 묶음은 버려지지만 부르는 쪽이 표시를 되살리거나 기록을 남겨 다음 계기에 다시 싣는다.
    //   mail = 같은 트랜잭션에 실을 우편 문장(없으면 nullptr — 기본값을 안 둔다 · 부르는 자리마다 실을지 정하게)
    bool EnqueueSave(const DBSaveBundle& head, const DBInvRow* rows, int rowCount, const DBMailStmt* mail);

    // 게임루프·종료 전용 — 묶음 여럿(주기 · 종료). allRows = 묶음들의 칸 줄을 이어 붙인 것, heads[i].slotBegin = 그 안의 시작 자리.
    //   allMails = 우편 문장을 이어 붙인 것, heads[i].mailBegin · mailCount = 그 안의 자리.
    //   슬롯마다 락 1회. outRefused 가 있으면 거절된 묶음의 heads 안 색인을 담는다(꼬리부터 거절 — 앞에서부터 받는다).
    void EnqueueSaveBatch(const std::vector<DBSaveBundle>& heads, const std::vector<DBInvRow>& allRows,
                          const std::vector<DBMailStmt>& allMails, std::vector<size_t>* outRefused = nullptr);

    // 게임루프 전용 — 우편 잡 1건(읽기·표시). 같은 캐릭터 = 같은 슬롯 큐(characterId%K) — 그 사람의 앞선 저장 뒤에 선다
    //   (보낸 우편이 확정된 뒤의 목록 읽기가 그 우편을 본다). 불러오기처럼 잡끼리 센 수로 같은 상한을 건다.
    //   반환 false = 거절(회신이 안 온다 — 부르는 쪽이 그 자리에서 DB_ERROR 로 닫는다).
    bool EnqueueMail(const DBMailJob& job);

    // 게임루프·종료 전용 — 짝 묶음 1건(두 캐릭터 = 트랜잭션 하나 · 거래 — trade-plan.md 「저장」).
    //   ★ 큐는 두 캐릭터 중 작은 번호의 슬롯이다(같은 짝은 늘 같은 큐 — 종료 때 다시 낸 짝 묶음이 떠 있던 짝 묶음 뒤에 줄 선다).
    //     다른 쪽 사람의 순서는 큐가 지켜 주지 않는다 — 부르는 쪽(CCharacterStore)이 짝이 확정될 때까지 두 사람의 다른 쓰기를 붙잡는다.
    //   두 머리를 한 락 안에 이어 넣어 워커가 한 번에 꺼낸다(a.pairNext = 1). 상한은 둘 다 들어갈 때만 — 반쪽만 넣지 않는다.
    //   회신은 머리마다 하나씩(같은 성패). 반환 false = 거절(둘 다 안 들어감 · dropped 를 2 센다).
    bool EnqueueSavePair(const DBSaveBundle& a, const DBInvRow* aRows, int aCount,
                         const DBSaveBundle& b, const DBInvRow* bRows, int bCount);

    // 게임루프 전용 — 쌓인 결과를 통째로 꺼낸다(락 1회). 세 벡터 다 비어 있어야 한다.
    //   벡터를 맞바꾸므로 양쪽 다 용량이 보존된다 → 틱마다 할당하지 않는다
    //   (SwapOutNetworkEvents 와 같은 방식). 처리 중에 도착한 결과는 다음 틱으로 넘어간다.
    //   ★ 저장 회신과 불러오기 결과를 한 락에 같이 꺼낸다 — 같은 캐릭터의 묶음 회신이 뒤 불러오기 결과보다 먼저 들어오므로
    //     (같은 큐 차례 · 묶음마다 곧바로 회신) 받는 쪽이 회신을 먼저 처리하면 불러오기 결과를 볼 때 그 캐릭터의 저장 결말을 이미 안다.
    //   우편 회신도 같은 락에 꺼낸다 — 같은 사람의 저장 회신(보내기 확정)이 뒤 우편 잡 회신보다 먼저 처리된다.
    void SwapOutResults(std::vector<DBResult>& loads, std::vector<DBSaveReply>& saves, std::vector<DBMailReply>& mails);

    // 모든 슬롯의 잔여 잡을 비우고 워커 종료 (멱등 — 소멸자에서도 호출).
    //   드레인 중에 끝난 불러오기의 결과는 회신 큐에 쌓인 채 버려진다(게임 스레드가 이미 끝난 뒤다).
    void Shutdown(int drainTimeoutSec);

private:
    // ExecLoad 한 번의 결말 — 커넥션 유실만 따로 가른다(읽기는 멱등이라 재연결 후 한 번 더 해 볼 수 있다).
    enum class LoadOutcome : uint8_t { FOUND, NOT_FOUND, QUERY_ERROR, CONNECTION_LOST };

    void WorkerThread(int idx);
    bool Connect(void*& outMysql);            // 슬롯 커넥션 1개 생성
    bool PrepareStmt(DBWorkerSlot& slot);     // slot.mysql로 종류별 prepared stmt 준비 + 바인딩 (하나라도 실패하면 전부 닫고 false)
    void CloseStmts(DBWorkerSlot& slot);      // 종류별 stmt 전부 닫기 (커넥션은 건드리지 않음)
    void DropConnection(DBWorkerSlot& slot);  // stmt 전부 + 커넥션 닫고 null 로 — 유실 확인 시
    // 결과는 항상 채운다(실패도 결과다). reconnectFailedThisBatch = 이 묶음에서 재연결이 실패한 적 있나(WorkerThread 지역값).
    void ExecLoad(DBWorkerSlot& slot, const DBLoadJob& job, DBResult& out, bool& reconnectFailedThisBatch);
    LoadOutcome RunLoadOnce(DBWorkerSlot& slot, const DBLoadJob& job, DBResult& out);    // 트랜잭션 하나 — 아래 몸을 감싼다
    LoadOutcome RunLoadBody(DBWorkerSlot& slot, const DBLoadJob& job, DBResult& out);    // 주인 잠금 → 캐릭터 행 → 가방 → 소유 표
#if USE_INVENTORY
    // 불러오기의 둘째 문장 — 가방 줄을 out.inv 에 채운다. 결말은 LoadOutcome 로(FOUND = 성공, 줄이 0 이어도).
    LoadOutcome RunInventoryLoad(DBWorkerSlot& slot, const DBLoadJob& job, DBResult& out);
#endif
    // 불러오기의 마지막 문장 — 소유 표의 이름·몸을 out 에 채운다(입장 번호 길만). FOUND = 성공 — 행이 없어도 성공이다(rosterFound 가 가른다)
    LoadOutcome RunRosterLoad(DBWorkerSlot& slot, const DBLoadJob& job, DBResult& out);
    // 가방·소유 표 문장의 오류 분류(유실 / 그 밖). stmt 는 MYSQL_STMT* — 헤더를 mysql.h 로 더럽히지 않으려고 void*
    static LoadOutcome ClassifyStmtError(void* stmt, const char* what, CharacterId characterId);
    // 불러오기용 재연결 — 이 묶음에서 이미 실패했으면 시도하지 않는다(죽은 DB 에 불러오기마다 접속 제한 시간을 쓰지 않게).
    bool ReconnectDue(const DBWorkerSlot& slot) const;   // 마지막 실패에서 최소 간격이 지났나 — 재연결을 부르는 자리 둘이 같이 본다
    bool TryReconnectForLoad(DBWorkerSlot& slot, bool& reconnectFailedThisBatch);
    void PushResult(const DBResult& result);  // 워커 스레드 → 회신 큐

    // 저장 묶음 — 불러오기와 같은 재연결 규칙(없으면 그 자리 재연결 · 도중에 끊기면 재연결 뒤 묶음째 한 번 더).
    //   반환 = 결말(성공 · 실패 · 펜스). 회신은 부르는 쪽(WorkerThread)이 곧바로 넣는다.
    //   heads·rows = 머리 n 개(1 = 혼자 · 2 = 짝 묶음)와 그 칸 줄 — n 개를 트랜잭션 하나로 쓴다.
    enum class SaveOutcome : uint8_t { OK, QUERY_ERROR, CONNECTION_LOST, FENCED };
    static constexpr int MAX_BUNDLE_HEADS = 2;
    //   mails = 머리마다의 우편 문장(그 머리의 mailCount 가 0 이면 그 자리 값은 안 본다)
    DBSaveResult ExecBundle(DBWorkerSlot& slot, const DBSaveBundle* const* heads, const DBInvRow* const* rows,
                            const DBMailStmt* const* mails, int n, bool& reconnectFailedThisBatch);
    SaveOutcome RunBundleOnce(DBWorkerSlot& slot, const DBSaveBundle* const* heads, const DBInvRow* const* rows,
                              const DBMailStmt* const* mails, int n);
    void PushSaveReply(const DBSaveReply& reply);   // 워커 스레드 → 회신 큐(저장 회신 목록)
    // 묶음 하나를 슬롯 큐에 — 락을 쥔 채 부른다(EnqueueSave · EnqueueSaveBatch · EnqueueSavePair 공용). 거절이면 false.
    bool PushBundleLocked(DBWorkerSlot& slot, DBSaveBundle head, const DBInvRow* rows, int rowCount, const DBMailStmt* mail);

    // 우편 잡 — 불러오기와 같은 재연결 규칙(읽기·표시라 두 번 해도 같다 · 유실이면 재연결 뒤 한 번 더). 회신은 늘 채운다.
    void ExecMailJob(DBWorkerSlot& slot, const DBMailJob& job, DBMailReply& out, bool& reconnectFailedThisBatch);
    void PushMailReply(DBMailReply&& reply);        // 워커 스레드 → 회신 큐(우편 회신 목록)
#if USE_MAIL
    // 잡 한 번 — FOUND = 성공(행이 없어도 성공 · out.found 가 가른다)
    LoadOutcome RunMailOnce(DBWorkerSlot& slot, const DBMailJob& job, DBMailReply& out);
    LoadOutcome RunMailList(DBWorkerSlot& slot, const DBMailJob& job, DBMailReply& out);
    // 결과 한 줄짜리 문장(받는 이름 · 한 통 · 붉은 수 · 받은 줄 · 이름) — 실행 · 묶은 결과 버퍼에 한 줄 받기 · found = 줄이 있었나
    LoadOutcome RunMailSelectOne(DBWorkerSlot& slot, int stmtIdx, const char* what, CharacterId characterId, bool& found);
#endif

#if USE_DB_FAULT_INJECT
    // 실패 주입(시험 빌드 전용 — BuildConfig.h). Start 가 환경 변수 MMO_DB_FAULT 를 읽어 채운다.
    //   꼴: "<종류>:<값>,character=<캐릭터>,times=<횟수>" — 종류 error:k · lost:k(묶음의 k 번째 문장 뒤) · down:n(재연결 n 번 실패) · delay:ms
    enum class FaultKind : uint8_t { NONE, ERROR_AFTER, LOST_AFTER, DOWN, DELAY };
    FaultKind            _faultKind = FaultKind::NONE;
    int                  _faultArg = 0;
    CharacterId              _faultCharacter = NO_CHARACTER;
    std::atomic<int>     _faultTimesLeft{ 0 };
    std::atomic<int>     _faultDownLeft{ 0 };      // 남은 「재연결 실패」 수 — down 이 걸리면 채워지고 Connect 가 깎는다
    void LoadFaultPlan();
    bool TakeFault(CharacterId characterId);              // 이 캐릭터 묶음에 주입이 걸리나(남은 횟수 하나 깎음)
#endif

    int SlotIndex(CharacterId characterId) const    // characterId % K (음수 방어)
    {
        return static_cast<int>(static_cast<uint64_t>(characterId) % static_cast<uint64_t>(_workerCount));
    }

    DBSaveCounters&  _counters;   // 저장 성공·실패·드롭·큐 깊이·워커 수 — 게임 그릇(GameMonitor.h DBSaveCounters)
    DBConfig         _config;
    int              _workerCount = 0;   // Start 가 넣는다
    int              _queueMax    = 0;
    ZoneServerId     _zoneServerId{ 0 };   // Start 가 넣는다 — 불러올 때 주인으로 적는 번호
    std::chrono::seconds _reconnectMin{ 5 };   // Start 가 넣는다 — ReconnectDue 의 간격

    std::vector<std::unique_ptr<DBWorkerSlot>> _workers;
    std::atomic<bool> _stop{ false };
    bool              _started = false;
    bool              _libInited = false;   // mysql_library_init 성공 여부 (library_end 짝 맞춤)

    // 회신 큐 — 워커 K개가 넣고 게임 스레드 하나가 꺼낸다. 워커가 소유하므로 종료 때 안전하다:
    //   게임 스레드가 먼저 끝나고 워커가 잔여를 비우는 동안에도 결과를 넣을 자리가 살아 있다.
    //   alignas(64): 워커가 쓰는 이 묶음이, 게임 스레드가 읽는 위 멤버들과 캐시 줄을 나눠 쓰지 않게 띄운다.
    struct alignas(64) ResultBox
    {
        std::mutex               mutex;
        std::vector<DBResult>    items;   // 불러오기 결과
        std::vector<DBSaveReply> saves;   // 저장 회신 — 불러오기 결과(가방 줄 120 을 드는 큰 꼴)와 따로 작은 꼴로
        std::vector<DBMailReply> mails;   // 우편 잡 회신
    };
    ResultBox _results;
};

#endif // USE_DB_WORKER
