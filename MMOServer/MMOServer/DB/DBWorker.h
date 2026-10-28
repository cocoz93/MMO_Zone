#pragma once

// ==========================================================================
// CDBWorker — DB 실행기: 저장 묶음과 불러오기를 실행하고 결과를 돌려준다
//
// [모델] 서버 메모리 = 진실, DB = 저장소.
//        게임루프(캐릭터 저장소)가 바뀐 캐릭터의 「묶음」(캐릭터 행 + 저장 안 된 가방 칸)을 큐에 넣으면,
//        전용 워커 스레드가 MySQL 에 쓴다(칸이 있으면 트랜잭션 하나). 게임 틱은 막지 않는다.
//
// [설계] 송신 워커(CIOCPServer::SendWorker) 패턴을 그대로 복제:
//        게임스레드 push(lock+notify) / 워커스레드 swap-out 배치 처리.
//        잡은 값 타입 스냅샷이라 플레이어 삭제 후 실행돼도 안전(수명 무관).
//
// [왕복] 저장도 불러오기도 답이 돌아온다 — 저장은 회신(성공·실패), 불러오기는 결과.
//        무엇을 다시 실을지·불러오기를 들여보낼지는 받는 쪽(캐릭터 저장소)이 정한다 — 워커는 주인을 모르고 실행·회신만 한다.
//        가는 길은 「같은 큐」 — 같은 계정은 같은 워커를 타고 큐는 앞에서부터 실행되므로
//        "옛 접속의 마지막 저장 → 새 접속의 불러오기" 차례가 지켜진다(그 저장이 실패했는지는 회신을 먼저 보는 받는 쪽이 가린다).
//        오는 길은 회신 상자 하나(저장 회신 · 불러오기 결과) — 워커가 넣고, 게임 루프가 틱마다 통째로 꺼낸다.
// ==========================================================================

#include "BuildConfig.h"

#include <cstdint>
#include <string>

// DB 접속 설정 — ServerConfig가 INI [DB] 섹션에서 채워 넘긴다.
// (USE_DB_WORKER 토글과 무관하게 정의 — GameServer::InitDB가 값으로 참조)
//   ★ 기본값을 여기 두지 않는다 — 정본은 ServerConfig 의 db* 멤버이고 main 이 일곱 칸을 전부 채운다(10-08 2차 검사 5번).
//     여기 숫자를 두면 아무도 안 읽는 셋째 벌이 된다. 0·빈 문자열 = 아직 안 채워짐.
struct DBConfig
{
    std::string host;
    int         port              = 0;
    std::string user;
    std::string password;
    std::string database;
    int         connectTimeoutSec = 0;
    int         rwTimeoutSec      = 0;   // 읽기/쓰기 소켓 타임아웃(초) — DB 무응답 시 쿼리 무한대기 방지
};

// characters 의 hp·mp 열에서 「저장된 적 없음」 — 들어올 때 가득 채운다(CStatSystem::RestoreHpMp 는 음수를 이렇게 읽는다).
//   열을 붙이기 전 행의 기본값이고(schema.sql), 새 캐릭터·전투나 스킬을 끈 빌드도 이 값을 싣는다.
//   0 이 아닌 이유 — 체력 0 은 「죽은 채 저장됨」이라 부활 규칙을 탄다(CEntryHandler::OnCharacterLoaded).
//   (USE_DB_WORKER 밖에 두는 이유 — 입장 판정 구조체(EnterPlacement)의 기본값이라 토글을 끈 빌드도 쓴다)
constexpr int32_t DB_HP_MP_UNSAVED = -1;

#if USE_DB_WORKER

#include <thread>
#include <mutex>
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

// 쿨타임·버프 열의 버퍼 크기 — 형식(CooldownBuffBlob.h)의 최대 길이와 같다(스킬 8칸 = 2 + 8×9 · 버프 1종 = 2 + 1×13).
//   스킬·버프 종류를 늘려 형식이 커지면 CharacterStore.cpp 의 static_assert 가 여기를 올리라고 막는다. 열 자체는 VARBINARY(255).
constexpr size_t DB_SKILL_COOLDOWNS_BYTES = 74;
constexpr size_t DB_BUFFS_BYTES           = 15;

// 캐릭터 행 값 = 값 스냅샷 (포인터 없음 → 플레이어가 사라져도 안전). 저장 묶음의 머리(DBSaveBundle::row)이자 행 문장의 버퍼 꼴.
//   ※ level·exp 는 USE_EXP 로, hp·mp 는 USE_COMBAT·USE_SKILL 로 안 가른다 — 열은 테이블에 늘 있고, 숫자 넷을 싣는 값이
//     "토글마다 문장이 갈리는" 값보다 훨씬 싸다. 토글이 가르는 것은 "값이 바뀌는 코드" 쪽이다.
//     쿨타임·버프 묶음(USE_SKILL·USE_COMBAT)도 같다 — 토글을 끈 빌드는 빈 값을 싣는다.
//   ※ DB 열과의 대응은 DBWorker.cpp 의 열 표(kCharColumns) 한 곳에만 있다 — 열을 늘리면
//     여기와 DBResult 에 같은 이름의 멤버를 넣고 표에 한 줄. SQL·바인딩·복사는 표에서 나온다.
struct DBCharRow
{
    int64_t accountId;
    float   x;
    float   y;
    int32_t mapId;
    int32_t level;
    int64_t exp;
    int32_t hp;      // 현재 체력 — 최대치는 저장하지 않는다(들어올 때 레벨 표·장비로 다시 세운다). DB_HP_MP_UNSAVED = 모름
    int32_t mp;      // 현재 마력 — 같은 규칙
    DBBytes<DB_SKILL_COOLDOWNS_BYTES> skillCooldowns;   // 돌고 있는 스킬 쿨타임 묶음 — 빈 값 = 없음(스킬을 끈 빌드도 빈 값)
    DBBytes<DB_BUFFS_BYTES>           buffs;            // 걸려 있는 버프 묶음 — 같은 규칙(전투를 끈 빌드도 빈 값)
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

// 저장 묶음의 머리 — 캐릭터 행 통째 + 칸 줄 수 + 주인 + 번호(save-unify-plan.md 「묶음」).
//   칸 줄은 원소 안에 두지 않는다 — 슬롯 큐 옆 줄 벡터(DBWorkerSlot::rows)에 이어 붙이고 시작 자리만 든다.
//   원소 안 고정 배열이면 대부분 0줄인 주기 묶음마다 칸 61줄(732B)이 붙는다.
//   칸이 0줄이면 문장 하나(그 자체가 트랜잭션), 1줄 이상이면 START TRANSACTION … COMMIT 으로 감싼다.
struct DBSaveBundle
{
    DBCharRow row;         // 캐릭터 행 10열(돈 열은 가방을 켠 빌드만) + 계정(열 표 kCharColumns 가 이 꼴의 멤버를 가리킨다)
    int64_t   sessionId;   // 주인 — 살아 있는 사람의 세션 번호 · 0 = 저장소의 떠난 기록·종료(회신은 계정 번호로 찾는다)
    uint32_t  bundleNo;    // 회신과 짝을 맞추는 번호(0 은 안 씀)
    uint32_t  slotBegin;   // 줄 벡터 안 시작 자리 — 넣는 함수가 매긴다(부르는 쪽 값은 무시)
    int32_t   slotCount;   // 칸 줄 수(0 ~ 가방 칸 수)
    uint8_t   pairNext;    // 1 = 큐에서 바로 뒤 머리와 한 트랜잭션(짝 묶음 — 거래 · EnqueueSavePair 만 켠다) · 0 = 혼자
};

// 저장 회신 — 모든 묶음이 하나씩 받는다(성공 · 실패). 무엇을 다시 실을지는 받는 쪽(저장소)이 정한다 — 워커는 주인을 모른다.
struct DBSaveReply
{
    int64_t  accountId;
    int64_t  sessionId;   // 묶음에 실어 보낸 값 그대로
    uint32_t bundleNo;
    bool     ok;
};

// 불러오기 잡 — 계정 번호로 캐릭터 행을 읽는다. 저장과 달리 결과가 회신 큐로 돌아온다.
struct DBLoadJob
{
    int64_t accountId;
    int64_t sessionId;   // 결과의 주인. 게임 스레드가 세션 번호 "전체"로 대조한다(자리 재사용 방어)
    int64_t requestNs;   // 요청 시각(steady_clock, ns) — 요청→결과 수령 지연을 재는 기준
};

#if USE_INVENTORY
// 가방 한 줄 문장의 버퍼 꼴(USE_INVENTORY) — 계정 + 칸 + 아이템 + 개수. 칸 UPSERT 의 파라미터 · 가방 SELECT 의 파라미터·결과 열.
//   저장 묶음의 칸 줄(DBInvRow)을 실행 직전에 계정과 함께 여기로 옮긴다(RunBundleOnce). 칸 줄은 최종값이라 두 번 써도 같다.
struct DBInvRowBuf
{
    int64_t accountId;
    int32_t slot;        // 칸 번호 (Protocol.h INV_TAB_RANGES)
    int32_t itemId;      // 0 = 빈 칸 — 행을 지우지 않고 0 으로 덮는다(DB 계정에 DELETE 권한이 없다)
    int32_t count;
};

// 결과에 실을 수 있는 가방 줄 수 — 칸 60 의 두 배. 망가진 행이 섞여 있어도 멀쩡한 행을 잃지 않게 넉넉히 둔다.
//   넘는 줄은 싣지 않고 수만 센다(DBResult::invOverflow → 게임 쪽 "too_many_rows" 계수기).
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
    SAVE_BUNDLE,       // 저장 묶음(캐릭터 행 + 칸 0~N) — 트랜잭션 하나 · 회신 하나(성공·실패). 다시 싣기는 저장소가 정한다
    COUNT
};
constexpr int DB_JOB_KIND_COUNT = static_cast<int>(DBJobKind::COUNT);

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
    };

    static DBJob Load(const DBLoadJob& l) { DBJob j; j.kind = DBJobKind::LOAD_CHARACTER; j.load = l; return j; }
    static DBJob Bundle(const DBSaveBundle& b) { DBJob j; j.kind = DBJobKind::SAVE_BUNDLE; j.bundle = b; return j; }
};

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
    int64_t      accountId;
    int64_t      sessionId;   // 요청 때 실어 보낸 값 그대로
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
    int64_t      money;       // DB 값 그대로 — 범위(0~MONEY_MAX) 검사는 게임 스레드(CCharacterStore::LoadInventoryRows — 망가진 값은 안 넣는다)
#if USE_INVENTORY
    // ↓ FOUND·NOT_FOUND 둘 다 유효 — 캐릭터 행이 없어도 가방은 읽는다(지금은 칸을 쓰는 묶음이 캐릭터 행도 같이 쓰지만,
    //   저장 묶음 전의 칸 즉시 저장 시절에 생긴 「가방 행만 있는 계정」이 DB 에 남아 있을 수 있다).
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
    size_t                   loadCount = 0;   // queue 안의 불러오기 수(mutex 보호) — 저장 상한과 따로 센다
    size_t                   bundleCount = 0; // queue 안의 저장 묶음 수(mutex 보호) — 묶음끼리 센 상한(EnqueueSave)
    void*                    mysql = nullptr; // 실제 MYSQL* (헤더 오염 차단 위해 void*)
    void*                    stmt[DB_STMT_COUNT] = {};   // MYSQL_STMT* — 문장별 prepared 핸들 (커넥션 종속, DBStmt 순서)
    DBCharRow                bSave{};         // 캐릭터 행 UPSERT 파라미터 버퍼 — execute 직전 묶음 머리의 행을 통째로 복사. 열 표(kCharColumns)가 멤버 자리를 가리킨다
    DBCharRow                bLoad{};         // 캐릭터 행 SELECT — accountId 가 WHERE 파라미터, 나머지 멤버가 결과 열(fetch 가 채운다)
#if USE_INVENTORY
    DBInvRowBuf              bItem{};               // 칸 UPSERT 파라미터 버퍼 — 칸 줄을 계정과 함께 복사. 열 표(kInvColumns)가 멤버 자리를 가리킨다
    DBInvRowBuf              bInvLoad{};            // 가방 SELECT — accountId 가 WHERE 파라미터, 나머지 멤버가 결과 열(fetch 가 줄마다 채운다)
#endif
    int                      index = 0;
};

// K개 워커 스레드 + 커넥션. accountId % K 고정 분배(같은 유저=같은 워커=UPSERT 순서 보장).
//   ★ 예외 하나 — 짝 묶음(거래 · EnqueueSavePair)은 두 계정 중 작은 번호의 워커로 간다. 다른 쪽 사람의 순서는 큐가 아니라
//     저장소가 지킨다 — 짝이 확정될 때까지 그 사람의 다른 쓰기(혼자 묶음 · 떠난 기록)를 붙잡는다(CCharacterStore 「짝 저장」).
//   백프레셔: 저장 묶음끼리 센 수가 queueMax 에 닿으면 거절(카운터+ERROR) — 부르는 쪽이 표시를 되살리거나 떠난 기록을 남겨
//   다음 계기에 다시 싣는다. 살아 있는 캐릭터는 비행 중 묶음이 하나라 정상 상태에선 안 닿는다.
class CDBWorker
{
public:
    explicit CDBWorker(DBSaveCounters& counters);   // 저장 파이프라인 지표 자리 — 게임 그릇(CGameMonitor::_db)
    ~CDBWorker();

    // K 커넥션 + K 워커 스레드 시작. 하나라도 접속 실패 시 전체 롤백 후 false (fail-fast).
    bool Start(const DBConfig& config, int workerCount, int queueMax);

    // 게임루프 전용 — 불러오기 1건. 저장과 같은 슬롯 큐(accountId%K)에 넣어 계정별 순서를 지킨다.
    //   저장 상한(queueMax)은 건너뛴다 — 저장은 버려도 다음 주기가 덮지만, 불러오기는 버리면 그 사람이
    //   영영 못 들어온다. 대신 큐에 든 불러오기 수를 따로 세서 같은 상한을 넘으면 거절한다
    //   (끝없이 쌓이는 큐를 만들지 않는다. 정상 상태에선 안 닿는 값 — 닿았다면 DB 가 멈춘 것).
    //   반환 false = 거절됨. 결과가 오지 않으므로 호출부가 그 자리에서 처리해야 한다.
    bool EnqueueLoad(const DBLoadJob& job);

    // 게임루프 전용 — 저장 묶음 1건(캐릭터 행 + 칸 줄 n). 같은 계정 = 같은 슬롯 큐(accountId%K) — 불러오기와 차례가 맞물린다.
    //   묶음끼리 센 수로 QueueMax 를 건다(불러오기 loadCount 와 같은 꼴 · 남의 원소까지 세지 않는다).
    //   반환 false = 거절(dropped 를 센다). 묶음은 버려지지만 부르는 쪽이 표시를 되살리거나 기록을 남겨 다음 계기에 다시 싣는다.
    bool EnqueueSave(const DBSaveBundle& head, const DBInvRow* rows, int rowCount);

    // 게임루프·종료 전용 — 묶음 여럿(주기 · 종료). allRows = 묶음들의 칸 줄을 이어 붙인 것, heads[i].slotBegin = 그 안의 시작 자리.
    //   슬롯마다 락 1회. outRefused 가 있으면 거절된 묶음의 heads 안 색인을 담는다(꼬리부터 거절 — 앞에서부터 받는다).
    void EnqueueSaveBatch(const std::vector<DBSaveBundle>& heads, const std::vector<DBInvRow>& allRows,
                          std::vector<size_t>* outRefused = nullptr);

    // 게임루프·종료 전용 — 짝 묶음 1건(두 캐릭터 = 트랜잭션 하나 · 거래 — trade-plan.md 「저장」).
    //   ★ 큐는 두 계정 중 작은 번호의 슬롯이다(같은 짝은 늘 같은 큐 — 종료 때 다시 낸 짝 묶음이 떠 있던 짝 묶음 뒤에 줄 선다).
    //     다른 쪽 사람의 순서는 큐가 지켜 주지 않는다 — 부르는 쪽(CCharacterStore)이 짝이 확정될 때까지 두 사람의 다른 쓰기를 붙잡는다.
    //   두 머리를 한 락 안에 이어 넣어 워커가 한 번에 꺼낸다(a.pairNext = 1). 상한은 둘 다 들어갈 때만 — 반쪽만 넣지 않는다.
    //   회신은 머리마다 하나씩(같은 성패). 반환 false = 거절(둘 다 안 들어감 · dropped 를 2 센다).
    bool EnqueueSavePair(const DBSaveBundle& a, const DBInvRow* aRows, int aCount,
                         const DBSaveBundle& b, const DBInvRow* bRows, int bCount);

    // 게임루프 전용 — 쌓인 결과를 통째로 꺼낸다(락 1회). 두 벡터 다 비어 있어야 한다.
    //   벡터를 맞바꾸므로 양쪽 다 용량이 보존된다 → 틱마다 할당하지 않는다
    //   (SwapOutNetworkEvents 와 같은 방식). 처리 중에 도착한 결과는 다음 틱으로 넘어간다.
    //   ★ 저장 회신과 불러오기 결과를 한 락에 같이 꺼낸다 — 같은 계정의 묶음 회신이 뒤 불러오기 결과보다 먼저 들어오므로
    //     (같은 큐 차례 · 묶음마다 곧바로 회신) 받는 쪽이 회신을 먼저 처리하면 불러오기 결과를 볼 때 그 계정의 저장 결말을 이미 안다.
    void SwapOutResults(std::vector<DBResult>& loads, std::vector<DBSaveReply>& saves);

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
    LoadOutcome RunLoadOnce(DBWorkerSlot& slot, const DBLoadJob& job, DBResult& out);
#if USE_INVENTORY
    // 불러오기의 둘째 문장 — 가방 줄을 out.inv 에 채운다. 결말은 LoadOutcome 로(FOUND = 성공, 줄이 0 이어도).
    LoadOutcome RunInventoryLoad(DBWorkerSlot& slot, const DBLoadJob& job, DBResult& out);
    // 가방 문장의 오류 분류(유실 / 그 밖). stmt 는 MYSQL_STMT* — 헤더를 mysql.h 로 더럽히지 않으려고 void*
    static LoadOutcome ClassifyStmtError(void* stmt, const char* what, int64_t accountId);
#endif
    // 불러오기용 재연결 — 이 묶음에서 이미 실패했으면 시도하지 않는다(죽은 DB 에 불러오기마다 접속 제한 시간을 쓰지 않게).
    bool TryReconnectForLoad(DBWorkerSlot& slot, bool& reconnectFailedThisBatch);
    void PushResult(const DBResult& result);  // 워커 스레드 → 회신 큐

    // 저장 묶음 — 불러오기와 같은 재연결 규칙(없으면 그 자리 재연결 · 도중에 끊기면 재연결 뒤 묶음째 한 번 더).
    //   반환 = 성공. 회신은 부르는 쪽(WorkerThread)이 곧바로 넣는다.
    //   heads·rows = 머리 n 개(1 = 혼자 · 2 = 짝 묶음)와 그 칸 줄 — n 개를 트랜잭션 하나로 쓴다.
    enum class SaveOutcome : uint8_t { OK, QUERY_ERROR, CONNECTION_LOST };
    static constexpr int MAX_BUNDLE_HEADS = 2;
    bool ExecBundle(DBWorkerSlot& slot, const DBSaveBundle* const* heads, const DBInvRow* const* rows, int n, bool& reconnectFailedThisBatch);
    SaveOutcome RunBundleOnce(DBWorkerSlot& slot, const DBSaveBundle* const* heads, const DBInvRow* const* rows, int n);
    void PushSaveReply(const DBSaveReply& reply);   // 워커 스레드 → 회신 큐(저장 회신 목록)
    // 묶음 하나를 슬롯 큐에 — 락을 쥔 채 부른다(EnqueueSave · EnqueueSaveBatch 공용). 거절이면 false.
    bool PushBundleLocked(DBWorkerSlot& slot, DBSaveBundle head, const DBInvRow* rows, int rowCount);

#if USE_DB_FAULT_INJECT
    // 실패 주입(시험 빌드 전용 — BuildConfig.h). Start 가 환경 변수 MMO_DB_FAULT 를 읽어 채운다.
    //   꼴: "<종류>:<값>,account=<계정>,times=<횟수>" — 종류 error:k · lost:k(묶음의 k 번째 문장 뒤) · down:n(재연결 n 번 실패) · delay:ms
    enum class FaultKind : uint8_t { NONE, ERROR_AFTER, LOST_AFTER, DOWN, DELAY };
    FaultKind            _faultKind = FaultKind::NONE;
    int                  _faultArg = 0;
    int64_t              _faultAccount = 0;
    std::atomic<int>     _faultTimesLeft{ 0 };
    std::atomic<int>     _faultDownLeft{ 0 };      // 남은 「재연결 실패」 수 — down 이 걸리면 채워지고 Connect 가 깎는다
    void LoadFaultPlan();
    bool TakeFault(int64_t accountId);              // 이 계정 묶음에 주입이 걸리나(남은 횟수 하나 깎음)
#endif

    int SlotIndex(int64_t accountId) const    // accountId % K (음수 방어)
    {
        return static_cast<int>(static_cast<uint64_t>(accountId) % static_cast<uint64_t>(_workerCount));
    }

    DBSaveCounters&  _counters;   // 저장 성공·실패·드롭·큐 깊이·워커 수 — 게임 그릇(GameMonitor.h DBSaveCounters)
    DBConfig         _config;
    int              _workerCount = 0;   // Start 가 넣는다
    int              _queueMax    = 0;

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
    };
    ResultBox _results;
};

#endif // USE_DB_WORKER
