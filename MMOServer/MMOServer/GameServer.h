// ==========================================================================
// CGameServer — 서버 메인 컨트롤러
//
// [책임]
//  - 서버 모드에 따라 에코 테스트 / MMO 게임 서버 동작 결정
//  - CIOCPServer로부터 NetworkEvent를 Pop하여 게임 로직 처리
//  - 패킷 파싱 및 타입별 핸들러 호출
//  - 게임 루프 스레드 소유 (고정 프레임 Tick)
//  - CMapManager를 통해 다중 존 관리
//
// [사용 흐름]
//  1. main()에서 CGameServer 생성
//  2. Init(ServerMode, ...) → 모드에 따라 내부 구성
//  3. Start() → 네트워크 시작 + (게임 모드 시 게임 루프 시작)
//  4. Stop() → 게임 루프 종료 + 네트워크 종료
// ==========================================================================
#pragma once

#include <cstdint>
#include <cassert>   // 도달불가 불변식 검증 (릴리즈에서는 사라짐)
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <queue>
#include <unordered_set>
#include <unordered_map>

#include "NetIoModel.h"   // 컴파일타임 I/O 모델 별칭 (IOCP/epoll) — IOCPServer.h를 여기서 흡수
#include <Protocol/Protocol.h>   // 게임 패킷 정의 — 전에는 IOCPServer.h를 통해 딸려왔다
#include "BuildConfig.h"   // USE_COMBAT 등 실험·컨텐츠 토글 — 간접 포함에 기대지 않고 직접 읽는다
#include "MapManager.h"
#include "Player.h"
#include "Combat.h"        // 전투 판정 (순수 함수 — 송신·존·섹터를 모른다)
#include "MonitorManager.h"
#include "GameMonitor.h"   // 게임 컨텐츠 지표 — 코어(CMonitorManager)와 별개 객체
#include "Common.h"
#include "GameDataFiles.h"  // 기획 데이터 파일(레벨 표·아이템 표) — 표가 하나도 안 켜졌으면 빈 헤더
#include "DB/DBWorker.h"

class CSerialBuffer;

class CGameServer
{
public:
    CGameServer(CMonitorManager& monitor, CGameMonitor& gameMonitor);
    ~CGameServer();

    // 에코 테스트 모드 초기화
    bool Init(ServerMode mode, int port, int maxClients);

    // 게임 서버 모드 초기화 (맵 설정 배열)
    bool Init(ServerMode mode, int port, int maxClients,
              const MapConfig* maps, int32_t mapCount, int workerThreads = 0, int sendWorkers = 0,
              int rioWorkers = 0, int completionBatch = 0, int sendDepth = 1);

    // DB 저장 파이프라인 초기화 (USE_DB_WORKER=0이면 no-op). Start() 전에 호출.
    bool InitDB(const DBConfig& dbConfig, int savePeriodSec, int workerCount, int queueMax);

    // 이벤트 큐 과부하 시 신규 accept 차단 임계 (0=끄기). Init() 뒤, Start() 전에 호출.
    void SetEventQueueAcceptLimit(int limit)
    {
        if (_network)
            _network->SetEventQueueAcceptLimit(limit);
    }

    // 이동 속도(칸/초) — 0 이면 지금까지의 동작(사람 25~50 무작위·뱀 15). ServerConfig [Game] 참조.
    //   Start() 전에 호출. 새로 만드는 캐릭터부터 적용된다.
    void SetMoveSpeeds(int playerSpeed, int snakeSpeed)
    {
        _playerSpeed = playerSpeed;
        _snakeSpeed  = snakeSpeed;
    }

    // 존당 몬스터 수 — 0 이면 MONSTERS_PER_ZONE(30). ServerConfig [Game] MonsterPerZone 참조.
    //   Start() 전에 호출. 맵을 넓히면 같은 마리 수로는 화면이 비므로 넓이에 맞춰 같이 올린다.
    void SetMonsterPerZone(int count)
    {
        _monsterPerZone = count;
    }

#if USE_GAME_DATA_FILES
    // 기획 데이터 파일(레벨 표·아이템 표)을 읽는다 — 기동 때 한 번, DB 접속보다 "앞"에서 부른다.
    //   실패하면 false 이고 main 이 서버를 안 띄운다. dir 은 끝에 경로 구분자가 붙은 디렉토리.
    bool LoadGameDataFiles(const std::string& dir) { return _gameDataFiles.LoadAll(dir); }
#endif

#if USE_EXP
    // ── 경험치·레벨 ──
    // 경험치를 더하고, 올라갈 만큼 올린다. 레벨이 올랐으면 true.
    //   ★ 부르는 자리가 둘이다 — 몬스터를 잡았을 때, 그리고 불러왔을 때.
    //     틱마다 레벨을 보는 자리가 없어서, 불러올 때 안 부르면 표를 낮춘 패치 뒤에 들어온 사람이
    //     "몬스터를 하나 잡기 전까지" 옛 레벨로 서 있게 된다.
    bool GainExp(CPlayer* player, int64_t amount);

    // 레벨을 표 안으로 내린다 (행이 표 밖 레벨을 들고 있을 때). 내렸으면 true.
    //   ★ 접근자가 조용히 clamp 하지 않는 이유가 이것 — 여기서 한 번 내리고 세야
    //     "표 밖 레벨이 몇 건이었나"가 계수기에 남는다.
    bool ClampLevelToTable(CPlayer* player);

    // 지금 레벨의 표 값으로 최대 체력·공격력을 맞춘다 (레벨업·입장 공통).
    void ApplyLevelStats(CPlayer* player, bool refillHp);

    // 본인에게 레벨·경험치를 알린다. 만렙이면 nextLevelExp 를 0 으로 보낸다.
    void SendExp(CPlayer* player);
#endif

    // 서버 시작/종료
    bool Start();
    void Stop();

private:
    // 게임 루프 (별도 스레드, GameServer 모드 전용)
    void GameLoopThread();

    // NetworkEvent 디스패치
    void ProcessNetworkEvents();
    void OnConnected(int64_t sessionId);
    void OnDisconnected(int64_t sessionId);
    void OnReceived(int64_t sessionId, CSerialBuffer* pMsg);

    // ── 입장(스폰) ──
    // 불러온 결과를 "스폰이 쓸 꼴"로 바꾼 것. 판정하는 쪽(OnCharacterLoaded·OnConnected)이
    //   채우고 SpawnPlayer 가 쓴다. 판정과 스폰을 가른 이유 — 갈래(찾음·없음·맵 없음·좌표 불량)는
    //   넷인데 스폰 절차는 하나다.
    //   ★ USE_DB_WORKER 밖에 둔다 — 토글을 끈 빌드의 접속 처리도 SpawnPlayer 를 부른다.
    //   ★ level·exp 가 여기 있는 이유 — 스폰이 패킷을 보내기 "전"에 값이 서 있어야 한다.
    //     스폰 뒤에 바깥에서 넣으면 그 패킷이 기본값을 싣고 나간다.
    struct EnterPlacement
    {
        bool    hasPos  = false;   // false = 무작위 자리
        float   x       = 0.0f;
        float   y       = 0.0f;
        bool    dbDirty = true;    // 스폰 자리를 다음 주기에 저장할까 (DB 와 같은 값으로 들어왔으면 false)
        int32_t level   = 1;       // 불러온 값. 새 캐릭터·못 읽음이면 기본값 그대로
        int64_t exp     = 0;
#if USE_DB_WORKER && USE_INVENTORY
        // 불러온 가방 줄이 든 결과 — 찾음·없음 갈래에서 채운다(없음이어도 가방 줄은 있을 수 있다). nullptr = 가방 없음.
        //   회신 큐의 원소를 가리킨다 — OnCharacterLoaded 가 SpawnPlayer 를 동기로 부르는 동안만 살아 있다.
        const DBResult* loaded = nullptr;
#endif
    };

    // 플레이어를 만들어 존에 넣고 본인·주변에 알린다 (OnConnected 의 몸이 옮겨 온 것).
    //   존은 호출부가 구해서 준다 — 좌표 검사에 그 존의 크기가 필요해서 판정하는 쪽이 이미 들고 있다.
    //   반환 nullptr = 입장 실패(zone 이 nullptr 이거나 존 입장 실패) → 호출부가 세션을 끊는다.
    CPlayer* SpawnPlayer(int64_t sessionId, int32_t actorId, int64_t accountId,
                         CZone* zone, const EnterPlacement& place);

    // 저장된 좌표를 그대로 써도 되나 — 이동 쪽 클램프(CZone::Tick)와 같은 범위를 쓴다.
    static bool IsValidSpawnPos(float x, float y, int32_t mapWidth, int32_t mapHeight);

    // 패킷 핸들러
    void RecvMoveStart(CPlayer* player, CSerialBuffer* pMsg);
    void RecvMoveStop(CPlayer* player, CSerialBuffer* pMsg);
    void RecvChat(CPlayer* player, CSerialBuffer* pMsg);
    void RecvZoneChange(CPlayer* player, CSerialBuffer* pMsg);
    void RecvAdminLogin(CPlayer* player, CSerialBuffer* pMsg);

#if USE_COMBAT
    // ── 전투 (USE_COMBAT) ──
    // 판정은 namespace Combat이 하고, 여기서는 그 결과를 세계에 적용한다
    //   (송신 · 부활 대기열 등록 · 지표 올리기).
    void RecvAttack(CPlayer* player, CSerialBuffer* pMsg);

    // 부활 대기자 훑기 — 틱마다 1회. 존 전체를 돌지 않으려고 죽은 것만 따로 들고 있는다.
    void TickRespawn();

    // 부활 대기열에서 빼기 — 퇴장 경로가 반드시 부른다. 안 부르면 다음 틱이 해제된 포인터를 밟는다.
    void RemoveFromDeadActors(CCharacter* player);

    // 공격 본체 — 플레이어(RecvAttack)와 몬스터(ThinkMonster)가 같이 쓴다.
    //   읽는 필드가 전부 CCharacter 것이라 세션 핸들러에서 갈라낼 수 있었다.
    void PerformAttack(CZone* zone, CCharacter* attacker);
#endif

#if USE_INVENTORY
    // ── 인벤토리 (USE_INVENTORY) ──
    // '/' 로 시작하는 채팅 — 방송하지 않고 명령으로 처리한다. 답은 본인에게 S2C_ERROR 한 줄.
    //   지금 명령은 /item 하나(운영자 전용 — 드랍이 생기기 전까지 가방을 채우는 유일한 길).
    void HandleChatCommand(CPlayer* player, const ChatChar* msg, uint16_t len);

    // 바뀐 칸을 내보낸다 — 칸 패킷 하나씩(+ 4단계에서 DB 저장 하나씩). 세는 것도 여기 한 곳이다.
    //   ★ 칸을 바꾼 자리는 반드시 부른다. 넣기·쓰기·드랍 어느 길이든 내보내는 길은 이것 하나다.
    void FlushInventory(CPlayer* player);

    // 가방 전체 — 입장 때 한 번(빈 가방이어도 한 장).
    void SendInventory(CPlayer* player);

    // 본인에게 알림 한 줄 — 기존 S2C_ERROR 를 쓴다(콘솔은 이미 UTF-8 로 풀어 보여 준다).
    void SendSystemMessage(CPlayer* player, const std::string& utf8);

    // 아이템 쓰기(지금은 물약만) — 거절이면 사유를 본인에게, 성공이면 칸 갱신 + 체력 통보(주변 포함).
    void RecvUseItem(CPlayer* player, CSerialBuffer* pMsg);

    // 쓸 수 있나 — 사유를 UseItemFail 순서대로 보고 먼저 걸린 하나를 돌려준다. 통과면 COUNT.
    //   판정을 값을 돌려주는 함수로 뺀 이유 — 사유가 여섯이라 호출부에서 if 사슬로 쓰면 하나를 빠뜨려도 안 보인다.
    UseItemFail CheckUseItem(const CPlayer* player, uint16_t slot, int& outIndex, const ItemDef*& outDef) const;

#if USE_DB_WORKER
    // 불러온 가방 줄을 검사해 넣는다 — 칸 범위·아이템 표·탭·쌓임 한도. 안 맞는 줄은 세고 건너뛴다(깎아 넣지 않는다).
    //   바뀐 칸 표시를 안 켠다(DB 와 같은 값) — 건너뛴 줄은 DB 에 그대로 남고, 그 칸에 새 아이템이 들어오면 덮인다.
    void LoadInventoryRows(CPlayer* player, const DBResult& result);
#endif

    // 물약 대기시간 1초 — 서버 틱 수로 잰다(사용자 결정 09-23). 틱 주기가 바뀌어도 1초가 되게 FRAME_PER_SEC 로 쓴다.
    static constexpr uint64_t ITEM_USE_COOLDOWN_FRAMES = 25;   // = FRAME_PER_SEC (아래 선언보다 앞이라 숫자로 — 단언이 맞춘다)
#endif

#if USE_MONSTER
    // ── 몬스터 (USE_MONSTER) ──
    // 게임 루프 1-1) — 좌표 갱신(2단계) 바로 앞에 놓는다.
    //   2단계인 지금은 "아직 몬스터를 안 깐 존"을 찾아 스폰하는 일만 한다.
    //   3단계에서 AI 판단이 여기 붙는다. 순서가 뒤면 AI 가 정한 방향이 한 틱씩 밀린다.
    void TickMonsters();

    // 존 하나에 몬스터를 깐다 — 위치·방향·섹터 등록은 CZone::EnterZone 이 전부 해 준다.
    void SpawnMonsters(CZone* zone);

    // 존이 사라지기 전에 몬스터를 회수한다(빈 채널 정리 · 서버 종료).
    //   BroadcastLeaveZone 이 섹터 변경·부활 대기열까지 훑어 주므로 죽은 주소가 안 남는다.
    void DespawnMonsters(CZone* zone, bool broadcast);

    // 몬스터 하나의 판단 — 대상 찾기·상태 적용·송신은 여기가, "무엇을 할까"는 MonsterAI 가.
    void ThinkMonster(CZone* zone, CMonster* monster);

    // 시야 안에서 가장 가까운 산 플레이어 (없으면 nullptr). 몬스터는 후보에서 거른다.
    CCharacter* FindAITarget(CZone* zone, CMonster* monster);

    static constexpr int32_t MONSTERS_PER_ZONE = 30;   // 섹터 36 개에 마리당 1 개꼴
#endif

#if USE_DB_WORKER
    // DB 저장 파이프라인 (dirty flag 기반 비동기 위치 저장)
    bool ShouldSave(const CPlayer* player) const;        // 저장 대상 판정(MOVING||dirty) — 주기·종료 공통
    DBSaveJob MakeSaveJob(const CPlayer* player) const;  // 위치 스냅샷 생성 (단건·배치 공용)
    void EnqueuePlayerSave(CPlayer* player);   // 단건: 스냅샷 enqueue + dirty clear (로그아웃)
    void TickPeriodicSave();                   // 주기: 저장 대상 배치를 1회 핸드오프
    void SaveAllPlayers();                     // 종료: 전원 최종 저장(안전망, dirty 무관)

    // DB 왕복 — 캐릭터 불러오기 (요청은 워커 큐로, 결과는 회신 큐로 돌아와 다음 틱에 꺼낸다)
    bool RequestCharacterLoad(int64_t sessionId, int64_t accountId);   // 조회 1건 투입 + 계수. false = 거절됨(결과가 안 온다)
    void ProcessDbResults();                                           // 틱마다: 회신 큐를 통째로 꺼내 종류별로 넘긴다
    void OnCharacterLoaded(const DBResult& result, int64_t nowNs);     // 불러오기 결과 1건 처리 — 판정 후 스폰

    // 대기표 — 접속은 했지만 아직 게임에 못 들어온 세션. 세션은 아래 둘 중 하나이거나 "게임 중"이고,
    //   셋 중 둘일 수는 없다. sessionId == -1 이 빈 자리다(CPlayer::_sessionId 와 같은 약속).
    //   세션 번호 "전체"를 들고 있어야 같은 자리에 다른 사람이 들어왔을 때 옛 결과를 가려낸다.
    //   ★ 접속한 세션은 반드시 이 표에 자리를 잡는다 — 어느 표에도 없는 세션이 생기면
    //     "동접 = 게임 중 + 불러오는 중 + 입장 대기" 등식이 늘 틀린 채로 울려 쓸모가 없어진다.
    enum class EnterState : uint8_t
    {
        WAITING_ENTER,   // 접속함. 입장 요청(C2S_ENTER_GAME)을 기다린다 — USE_LOGIN=1 에서만 생긴다
        LOADING,         // 입장 요청을 받아 조회를 던졌다. 결과를 기다린다
    };
    struct PendingEnter
    {
        int64_t    sessionId = -1;
        int64_t    accountId = 0;   // [입장 대기] 구간에서는 아직 0 이다 — 입장 요청이 와야 정해진다
        int32_t    actorId   = 0;   // 같음. 캐릭터 번호도 입장 요청 때 뽑는다
        EnterState state     = EnterState::WAITING_ENTER;
    };
    // 대기표를 바꾸는 자리는 이 셋뿐이다 — 게이지 둘(mmo_enter_waiting·mmo_enter_pending)도 여기서만 움직인다.
    //   접속·입장요청·결과·끊김 네 곳이 제각기 올리고 내리면 한 곳만 빠져도 게이지가 영영 안 맞는다.
    void SetPendingEnter(int64_t sessionId, int64_t accountId, int32_t actorId, EnterState state);
    bool PromotePendingEnter(int64_t sessionId, int64_t accountId, int32_t actorId);  // [입장 대기] → [불러오는 중]
    bool TakePendingEnter(int64_t sessionId, PendingEnter& out);                      // 세션 번호 전체가 같으면 꺼내고 비운다
    void AdjustPendingCount(EnterState state, int delta);                             // 상태별 수 + 게이지를 한 자리에서 움직인다
#endif

#if USE_LOGIN
    // ── 입장 요청 (USE_LOGIN) ──
    // 이 핸들러만 OnReceived 의 FindPlayer 문턱 "앞"에서 불린다 — 입장 요청이 올 때는 아직
    //   플레이어가 없어서, 문턱 뒤에 두면 영영 여기까지 못 온다.
    void RecvEnterGame(int64_t sessionId, CSerialBuffer* pMsg);

    // 같은 계정으로 이미 들어와 있는 세션을 걷어낸다 (게임 중 / 불러오는 중 양쪽).
    //   새 조회를 던지기 "전"에 불러야 한다 — 정리가 넣는 최종 저장이 그 조회보다 큐에서 앞서야
    //   새 접속이 방금 저장한 자리를 읽는다(같은 계정은 같은 워커를 탄다).
    void KickDuplicateAccount(int64_t accountId, int64_t exceptSessionId);
#endif

    // 섹터 변경 시 시야 진입/이탈 브로드캐스트
    void ProcessSectorChange(CZone* zone, CCharacter* player,
                             int32_t oldSectorX, int32_t oldSectorY);

    // 섹터 변경 대기열에 추가 (중복 플레이어는 최초 출발 섹터만 유지)
    void PushSectorChange(CCharacter* player, int32_t oldSectorX, int32_t oldSectorY);

    // 같은 틱에 함께 이동한 쌍 보정 — 틱 끝 일괄 통보가 구조적으로 놓치는 CREATE/DELETE를 보충.
    //   ProcessSectorChange는 이탈/진입 섹터의 "틱 끝 시점" 주민을 읽는데, 상대도 같은 틱에
    //   움직였으면 그 조회에 안 걸린다 → 상호 DELETE 누락(유령) / 상호 CREATE 누락(투명인간).
    //   기존 송신 경로는 그대로 두고 두 패스 모두 놓친 쌍만 보충한다 (중복 없음).
    void FixSameTickMoverPairs();

    // 존 입장/퇴장 브로드캐스트
    //   BroadcastEnterZone — 플레이어 입장: 양방향(주변 → 나를 생성, 나 → 주변을 생성)
    //   BroadcastSpawn     — 세션 없는 것(몬스터)의 등장: 주변에 알리기만. 받는 쪽이 없으니 인바운드가 없다
    //   BroadcastLeaveZone — 퇴장·회수: 주변에 나를 삭제 (플레이어·몬스터 공용)
    void BroadcastEnterZone(CZone* zone, CPlayer* player, SpawnReason reason);
    void BroadcastSpawn(CZone* zone, CCharacter* who, SpawnReason reason);
    void BroadcastLeaveZone(CZone* zone, CCharacter* player);

    // ── 플레이어 ID 발급 + 표시 속성 ──

    int32_t AllocPlayerId();
    static uint8_t CalcDisplayChar(int32_t playerId);
    static uint8_t CalcColorIndex(int32_t playerId);

    // ── 패킷 전송 추상화 ──
    // 모든 S2C는 직렬화 버퍼(CSerialBuffer)로 조립 후 아래 오버로드로 전송한다.

    // 단일 플레이어에게 패킷 전송 (CSerialBuffer — 소유권 1을 소비)
    // 빌더가 반환한 RefCount=1 버퍼를 넘기면, 송신 후 SubRef로 소유권을 회수한다.
    //   받는 쪽은 CPlayer* 뿐이다 — 몬스터를 넘기면 컴파일이 안 된다(Player.h [세션]).
    void SendPacket(CPlayer* target, CSerialBuffer* pMsg);

    // 주변 브로드캐스트 (CSerialBuffer — 빌더 RefCount=1 버퍼를 소비)
    void BroadcastAroundSector(CZone* zone, CCharacter* player, CSerialBuffer* pMsg, bool excludeSelf = true);

    // ── 이동 알림 헬퍼 ──
    // 이동 시작/정지/동기화 통보의 "묶음(dirty 마킹) ↔ 즉시 브로드캐스트" 분기를 한 곳에 흡수.
    // 호출부는 의도만 표시, 실제 경로는 USE_SECTOR_AGGREGATION이 결정 (동작 불변).
    void NotifyMoveStart(CZone* zone, CCharacter* player);
    void NotifyMoveStop(CZone* zone, CCharacter* player, bool excludeSelf);
    void NotifyMoveSync(CZone* zone, CCharacter* player);

    // ── 섹터 묶음 경로 (USE_SECTOR_AGGREGATION) ──
    void MarkMoveDirty(CCharacter* player);   // 즉시 브로드캐스트 대신 dirty 등록 (중복 방지)
    void FlushSectorUpdates();             // 틱 끝: dirty를 섹터별로 묶어 주변에 송신
    // 섹터 좌표 기준 주변 9섹터에 묶음 패킷 전송 (BroadcastAroundSector의 소유권/계측 패턴 복제)
    void BroadcastSectorPacket(CZone* zone, int32_t sectorX, int32_t sectorY, CSerialBuffer* pMsg);

#if USE_BROADCAST_BUNDLE
    // ── 수신섹터 digest 경로 (Phase 3) — FlushSectorUpdates를 대체 ──
    // 소스 섹터별 보류물(이동 번들 + 채팅)을 수신 섹터 기준으로 뒤집어, 이웃 9섹터 내용을
    // raw 바이트 1덩어리로 연접 후 주민당 RequestSendRaw 1회. 같은 섹터 주민은 수신 집합이 동일해 공유 가능.
    void RegisterSectorItem(CZone* zone, int32_t zoneId, int32_t sectorX, int32_t sectorY,
                            CSerialBuffer* pMsg);   // 보류 등록 (버퍼 소유권 1을 digest 배포 후 회수)
    void FlushSectorSends();                        // 틱 끝: 번들 빌드 → 수신섹터 연접 → 배포 → 일괄 해제
#if USE_MEMBERSHIP_DIGEST
    // ── 멤버십 직송 경로 (Phase 4) — 아웃바운드 CREATE/DELETE를 digest에 합류 ──
    // 방송(3×3 이웃 배포)과 달리 "정확히 그 섹터 주민에게만" 전달. 연접 순서는 직송이 방송보다 앞.
    void RegisterSectorDirectItem(CZone* zone, int32_t zoneId, int32_t sectorX, int32_t sectorY,
                                  CSerialBuffer* pMsg);   // 직송 보류 등록 (버퍼 소유권 1을 배포 후 회수)
#endif
#endif

#if USE_MEMBERSHIP_FANOUT_DEDUP
    // 멤버십 아웃바운드 팬아웃 (Phase 1) — 미리 빌드한 버퍼 1개를 여러 섹터의 플레이어에게 배치 AddRef로 전송.
    // BroadcastSectorPacket의 소유권/계측 패턴을 (주변 9섹터가 아닌) 임의 섹터 배열 버전으로 확장. 버퍼 소유권 1을 소비.
    void FanoutToSectors(CZone* zone,
                         const CSectorManager::SectorPos* sectors, int32_t sectorCount,
                         CSerialBuffer* pMsg, CCharacter* exclude);
#if USE_MEMBERSHIP_DIGEST
    // [Phase 4] 아웃바운드 멤버십을 직송 보류로 등록 (+자기 섹터 점프 폴백). 버퍼 소유권 1을 소비.
    void RegisterOutboundToSectors(CZone* zone, int32_t zoneId,
                                   const CSectorManager::SectorPos* sectors, int32_t sectorCount,
                                   CSerialBuffer* pMsg, CCharacter* exclude);
#endif
#endif

    // 패킷별 전송 함수 (Fill + Send)
    void SendZoneInfo(CPlayer* target, CZone* zone);
    void SendCreateMyPlayer(CPlayer* target);
    // target(받는 쪽)은 CPlayer*, who(그려질 대상)는 CCharacter* — 몬스터가 섹터를 옮기면 who 가 몬스터다.
    //   받는 쪽에 몬스터를 넘기는 코드는 타입이 막는다.
    void SendCreateOtherPlayer(CPlayer* target, CCharacter* who, SpawnReason reason = SpawnReason::NORMAL);
    void SendDeletePlayer(CPlayer* target, CCharacter* who);
#if USE_MEMBERSHIP_INBOUND_BUNDLE
    // [Phase 2] 인바운드 멤버십 배치 — 상대 목록을 배치 상한씩 잘라 mover 1명에게 송신 (엔트리당 _membershipSends 1 집계)
    void SendCreatePlayerBatch(CPlayer* target, CCharacter* const* others, int count);
    void SendDeletePlayerBatch(CPlayer* target, CCharacter* const* others, int count);
#endif
    void SendSyncPosition(CPlayer* target);
    void SendZoneChangeOk(CPlayer* target, int32_t mapId, int32_t channelIndex);
    void SendZoneChangeFail(CPlayer* target, uint8_t reason);

#if USE_COMBAT
    // 스탯 통보 — CREATE 계열에 HP 자리가 없어 뒤따라 보낸다.
    //   호출처 9곳이 아니라 CREATE 송신 함수 3곳 안에서 부른다(갈래를 3으로 닫으려고).
    void SendActorStats(CPlayer* target, const CCharacter* actor);
#endif

    // 벽 방향 검증 — 경계 위치에서 벽 쪽 이동 차단
    bool IsBlockedByWall(CZone* zone, CCharacter* player, Direction dir);

    // C2S_MOVE_START 좌표 수용의 이동량 예산 검사 — 경과분 적립 후 통과하면 소비까지 수행.
    // 반환 false면 수용하지 않고 서버 좌표를 유지한다(기존 범위초과 경로와 동일한 처리).
    bool ConsumeMoveBudget(CPlayer* player, float distSq);

    // 패킷 타입별 최소 크기 반환 (0이면 알 수 없는 타입)
    static uint16_t GetExpectedSize(MsgType type);

private:
    ServerMode _mode;

    CMonitorManager& _monitor;       // 코어 지표 — 배치 송신 경로에서 코어 대신 세는 자리가 있어 게임도 참조한다
    CGameMonitor&    _gameMonitor;   // 게임 컨텐츠 지표 (전투)
    std::unique_ptr<NetIoModel> _network;
    CMapManager _mapManager;

    std::thread _gameThread;
    std::atomic<bool> _running;
    // 프레임 설정
    static constexpr int FRAME_PER_SEC = 25;
    static constexpr int FRAME_INTERVAL_MS = 1000 / FRAME_PER_SEC;  // 40ms

    static constexpr int CLEANUP_INTERVAL_FRAMES = 25 * 30;  // 30초마다 빈 채널 정리
    static constexpr int SYNC_INTERVAL_FRAMES = 13;          // 13 × 40ms ≈ 500ms 주기적 위치 동기화
    static constexpr float SYNC_DISTANCE_THRESHOLD_SQ = 4.0f; // 델타 동기화 임계값 제곱 (2타일)
    static constexpr float MOVE_START_ACCEPT_DIST_SQ = 64.0f; // C2S_MOVE_START 좌표 수용 임계값 제곱 (8타일)

    // 좌표 수용 예산 — 위 임계값은 1회 한도일 뿐 빈도 제한이 없어, STOP/START를 연타하면
    //   매 전이마다 8타일씩 수용돼 사실상 무제한 순간이동이 됐다(수신 빈도 제한은 서버 어디에도 없음).
    //   상한은 위 임계값과 같은 8타일 → 정상 클라의 단발 보정은 지금과 동일하게 통과.
    //   적립은 자기 속도 × 여유계수로 시간 비례 → 연타해도 장기 평균은 적립 속도로 수렴한다.
    //   단 서버 주도 이동(매 틱 speed×dt)은 예산 검사 대상이 아니라 그 위에 얹히므로,
    //   치트 이동량 상한은 speed×(1+SLACK)이다. SLACK 1.25일 때 실측 2.09배 → 0.25로 조여 총합 1.25배.
    static constexpr float MOVE_BUDGET_CAP = 8.0f;     // 예산 상한 (타일) — MOVE_START_ACCEPT_DIST_SQ의 제곱근
    // 적립 여유계수 (네트워크 지터·프레임 흔들림 흡수). 낮출수록 조이지만 정상 클라 오탐 위험이 커진다:
    //   정상 소모 ≈ RTT/2 × speed(예측-서버 좌표 오차), 적립은 speed×SLACK/초.
    //   speed 30·RTT 200ms(오차 3타일/회) 기준 견디는 MOVE_START 빈도 = 초당 2.5회.
    //   루프백 실측은 RTT≈0이라 이 오탐을 못 잡는다 → mmo_move_budget_rejects_total 실네트워크 관측 필요.
    static constexpr float MOVE_BUDGET_SLACK = 0.25f;

    int32_t _defaultMapId = 0;  // 최초 접속 시 입장할 맵
    int32_t _nextPlayerId = 1;  // 전역 playerId 카운터 (싱글스레드 게임 루프)

#if USE_GAME_DATA_FILES
    // 기획 데이터 — 기동 때 한 번 읽고 그 뒤로는 읽기만 한다(게임 스레드가 매 전투·명령에서 본다).
    CGameDataFiles _gameDataFiles;
#endif

    // 네트워크 경계: sessionId → CPlayer* (수신 시 플레이어 조회)
    //   sessionId 상위 16비트 = 세션 슬롯 인덱스 → 해시맵 대신 인덱스 배열 직접 접근 (캐시미스 제거).
    //   슬롯 재사용(ABA)은 FindPlayer의 _sessionId 일치 검사로 방어. 접근은 게임 스레드 단독.
    std::vector<CPlayer*> _sessionSlots;

    // 슬롯 조회 + 재사용 가드 — 등록된 세션이 아니면 nullptr
    CPlayer* FindPlayer(int64_t sessionId) const
    {
        uint16_t idx = CSession::ExtractIndex(sessionId);
        // [불변식] idx < _sessionSlots.size() — 슬롯 배열은 Init에서 maxClients로 assign되고,
        //   세션 인덱스는 같은 maxClients로 잡힌 CIOCPServer 세션 배열의 인덱스라 항상 그 미만이다.
        //   ※ 바로 아래가 배열 인덱싱이다. 이 불변식이 깨지면 릴리즈에서는 곧장 범위 밖 접근이 된다
        //     (다른 불변식 assert들과 달리 실패 대가가 메모리 손상). 슬롯 크기 산정이나 maxClients
        //     전달 경로를 손보면 반드시 디버그 빌드로 여기를 통과시킬 것.
        assert(idx < _sessionSlots.size());
        CPlayer* p = _sessionSlots[idx];
        return (p != nullptr && p->_sessionId == sessionId) ? p : nullptr;
    }

    // 프레임 진입 시점 이벤트를 스왑해 받는 로컬 큐 (멤버 재사용 → deque 블록 보존으로 매 프레임 재할당 방지)
    std::queue<NetworkEvent> _localEvents;
    int _cleanupFrameCount = 0;
    int _syncFrameCount = 0;
    uint64_t _frameCount = 0;   // 단조 증가 프레임 번호 (이동 예산의 경과 시간 산출용)

    // 이동 속도 덮어쓰기(칸/초) — 0 이면 각 생성자의 기본값을 그대로 쓴다(SetMoveSpeeds)
    int _playerSpeed = 0;
    int _snakeSpeed  = 0;
    int _monsterPerZone = 0;   // 존당 몬스터 수 — 0 이면 MONSTERS_PER_ZONE(SetMonsterPerZone)

    // 섹터 변경 배치 처리용 대기열 (프레임 내 수집 → 틱 후 일괄 처리)
    std::vector<SectorChangeInfo> _pendingSectorChanges;
    std::unordered_set<CCharacter*> _sectorChangedSet;  // 이미 기록된 플레이어 필터

    // 프레임 재사용 버퍼 (힙 할당 방지 — clear()로 capacity 유지)
    std::vector<CCharacter*> _broadcastBuffer;       // BroadcastAroundSector 전용
    std::vector<CCharacter*> _eventAroundBuffer;     // 접속/해제/존이동 전용
    std::vector<SectorChangeInfo> _tickSectorChanges;
    std::vector<CCharacter*> _tickClampedPlayers;
#if USE_COMBAT
    // 공격 후보 버퍼는 두지 않는다 — 사거리가 걸치는 섹터의 주민 목록을 참조로 훑어 복사가 없다.
    //   (처음엔 GetAroundCharacters 가 벡터를 요구해 _attackTargetBuffer 를 뒀었다)

    // 죽어서 부활을 기다리는 캐릭터. 여기만 틱 너머로 포인터를 든다 —
    //   퇴장 경로(BroadcastLeaveZone·OnDisconnected)가 반드시 회수해야 한다.
    //   같은 규율이 이미 dirtyMovers·_sectorChangedSet·_pendingSectorChanges에 있다.
    std::vector<CCharacter*> _deadActors;
#endif

    // ── 섹터 아웃박스 ── 틱-지연 배칭 상태를 한 덩어리로 응집.
    //   생산(틱 중): MarkMoveDirty / RegisterSectorItem  →  배출(틱 끝): FlushSectorUpdates / FlushSectorSends
    //   흩어져 있던 상태를 묶어 "위에서 모아 아래서 비운다"의 대상이 한눈에 보이게 함 (동작 불변).
    struct SectorOutbox
    {
        // USE_SECTOR_AGGREGATION: 이번 틱 이동/sync로 상태가 바뀐 플레이어 (틱 끝에 섹터별 묶음 송신)
        std::vector<CCharacter*> dirtyMovers;

#if USE_BROADCAST_BUNDLE
        // USE_BROADCAST_BUNDLE: 존별 섹터 그리드에 이번 틱 보류물(번들+채팅)을 모았다가 틱 끝 digest로 배포.
        // zoneId 키로 보관 — 틱 중 존이 사라져도 해제 루프는 존 포인터 없이 안전 (기존 GetZone null 가드 패턴).
        struct ZonePending
        {
            int32_t countX = 0, countY = 0;                  // 섹터 그리드 크기 (존 최초 등록 시 확정)
            std::vector<std::vector<CSerialBuffer*>> items;  // flatIdx → 보류 버퍼들 (등록순)
            std::vector<uint64_t> recvEpoch;                 // 수신섹터 중복 마킹 (틱당 epoch 증가로 clear 생략)
#if USE_MEMBERSHIP_DIGEST
            // USE_MEMBERSHIP_DIGEST: 직송(그 섹터 주민 전용) 보류물 — 멤버십 CREATE/DELETE.
            std::vector<std::vector<CSerialBuffer*>> directItems;   // flatIdx → 직송 버퍼들 (등록순)
#endif
        };
        std::unordered_map<int32_t, ZonePending> pendingByZone;
        std::vector<std::pair<int32_t, int32_t>> touchedSectors;   // (zoneId, flatIdx) — 보류물 있는 소스 섹터
#if USE_MEMBERSHIP_DIGEST
        std::vector<std::pair<int32_t, int32_t>> touchedDirectSectors;  // (zoneId, flatIdx) — 직송물 있는 섹터
#endif
        std::vector<std::pair<int32_t, int32_t>> receiverSectors;  // (zoneId, flatIdx) — 이번 틱 수신 후보 (재사용)
        std::vector<char> concatBuf;                               // 수신섹터별 연접 버퍼 (재사용)
        uint64_t flushEpoch = 0;                                   // 64비트 — wrap 없음 (0 = 미마킹 초기값과 충돌 방지)
#endif
    } _sectorOutbox;

#if USE_BROADCAST_BUNDLE
    // 존 최초 등록 시 그리드 확정 — 방송(items)/직송(directItems) 어느 쪽이 먼저 와도 1회만.
    // (선언 위치: 중첩 타입 SectorOutbox::ZonePending을 인자로 받아 struct 정의 뒤에 와야 함)
    void EnsureZonePending(CZone* zone, SectorOutbox::ZonePending& zp);
#endif

#if USE_MEMBERSHIP_INBOUND_BUNDLE
    // USE_MEMBERSHIP_INBOUND_BUNDLE: 인바운드 멤버십 배치 수집 전용 (ProcessSectorChange 내 수집→직렬화→송신 완결, 이월 없음)
    std::vector<CCharacter*> _membershipInboundBuffer;
#endif

    // 비용종류별 계측 — 틱 내 누적 후 틱 끝 1회 모니터 반영 (단일 게임루프 스레드 전용 → 누적 자체엔 원자연산 불필요)
    // BroadcastAroundSector가 network/game_logic/broadcast_sync 어느 단계에서 호출돼도 이 멤버에 합산된다.
    int64_t _tickBroadcastGatherUs = 0;   // GetAroundCharacters 주변 모으기
    int64_t _tickBroadcastEnqueueUs = 0;  // 수신자별 처리(복사 포함)
    int64_t _tickMembershipSends = 0;     // 멤버십 변경 복사(BroadcastAroundSector 밖 경로) 송신 횟수
    int64_t _tickMembershipUs = 0;        // 멤버십 송신(ProcessSectorChange 구간) 시간 — 틱 끝 _membershipCostUs로 반영
    int64_t _tickPairFixes = 0;           // 같은 틱 이동자 쌍 보정 발동 수 — 틱 끝 _membershipPairFixes로 반영
    int64_t _tickMoveBudgetRejects = 0;   // 이동 예산 초과로 거부한 좌표 수용 수 — 틱 끝 _moveBudgetRejects로 반영

#if USE_DB_WORKER
    // DB 저장 파이프라인 (dirty flag 기반 비동기 위치 저장)
    std::unique_ptr<CDBWorker> _dbWorker;
    int _dbSavePeriodFrames = 250;   // SavePeriodSec × 25fps (InitDB에서 설정)
    int _dbSaveFrameCounter  = 0;

    // 회신 큐에서 통째로 꺼내 받는 로컬 버퍼 (멤버 재사용 — 워커 쪽 벡터와 맞바꾸므로 양쪽 용량이 보존된다.
    //   _localEvents 와 같은 방식). 접근은 게임 스레드 단독.
    std::vector<DBResult> _dbResults;

    // 대기표 — _sessionSlots 와 같은 크기·같은 색인(해시맵 아님). Init 에서 나란히 잡는다.
    //   개수는 배열을 훑지 않으려고 상태별로 따로 센다. 접근은 게임 스레드 단독.
    std::vector<PendingEnter> _pendingEnter;
    int32_t _pendingLoadingCount = 0;   // [불러오는 중] — DB 결과를 기다린다
    int32_t _pendingWaitingCount = 0;   // [입장 대기]  — 입장 요청을 기다린다 (USE_LOGIN=1 에서만 0 이 아니다)
#endif
};
