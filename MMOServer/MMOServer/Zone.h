// ==========================================================================
// CZone — 존 단위 데이터 컨테이너
//
// [책임]
//  - 맵 크기, CSectorManager, 캐릭터 목록 소유 (vector<CCharacter*> — 플레이어·몬스터 공통)
//  - 몬스터 관리자 소유 (_monsterManager, 값) — 몬스터 객체·깔기·판단은 그쪽(MonsterManager.h).
//    존과 같이 태어나고 같이 사라지므로. 플레이어 객체는 접속 수명이라 CGameServer 가 소유한다
//  - 캐릭터 입장/퇴장, 섹터 등록/해제 등 데이터 관리
//  - 맵 경계 규칙(맵 안 검사·클램프·벽 판정) — 맵 크기를 든 곳이 여기라서
//  - 섹터 이동 대기열 — 담긴 옛 섹터 좌표가 이 존의 격자라서. 읽어서 CREATE/DELETE 를 보내는 것은 CGameServer
//  - 땅 아이템 관리자 소유 (_groundItemManager, 값) — 땅 아이템 목록과 놓기·수명·줍기 고르기·개수는 그쪽
//    (GroundItemManager.h). 무엇이 떨어질지(굴림)와 송신은 CGameServer
//  - 패킷 처리·브로드캐스트는 CGameServer, 몬스터 판단은 CMonsterManager 가 담당
//
// [설계]
//  - 싱글스레드 전제 (게임 로직 스레드 1개에서만 호출)
//  - 스폰 좌표는 맵 전체 균등분포 랜덤 (경계 1칸 여유)
// ==========================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <unordered_set>
#include <vector>

#include "Actor.h"         // Direction — 벽 판정(IsBlockedByWall)이 방향을 받는다
#include "BuildConfig.h"
#include "MapTable.h"       // StartArea — 맵 표의 시작 구역(CalcSpawnPos 가 읽는다)
#include "SectorManager.h"
#include "MonsterManager.h"   // 몬스터 관리자 — 존이 값으로 든다(USE_MONSTER 가 꺼져 있으면 빈 헤더)
#include "GroundItemManager.h" // 땅 아이템 관리자 — 존이 값으로 든다(USE_DROP 이 꺼져 있으면 빈 헤더)

class CCharacter;
class CZone;
class CGameDataFiles;   // GameDataFiles.h — 몬스터 관리자에 건넨다(존은 안 읽는다)

// 존 안에서 정한 걷기·멈춤을 게임 서버에 알리는 통로 — CGameServer 가 구현한다(존은 게임 서버를 모른다).
//   존 틱의 벽 멈춤과 몬스터 판단(CMonsterManager)이 같이 쓴다. 패킷으로 걷고 멈추는 플레이어는 안 온다.
//   부를 때마다 받는다(Tick 인자 · MonsterContext) — 존을 만드는 곳이 MapManager 안뿐이라서(MonsterManager.h 와 같은 까닭).
class IZoneEvents
{
public:
    virtual ~IZoneEvents() = default;

    // 걷기 시작(또는 걷는 중에 방향을 틀었다) · 멈춤 — 원인(벽·판단)은 싣지 않는다.
    virtual void OnCharacterMoveStart(CZone* zone, CCharacter* character) = 0;
    virtual void OnCharacterMoveStop(CZone* zone, CCharacter* character) = 0;
};

// Tick에서 섹터 변경이 발생한 캐릭터 정보
struct SectorChangeInfo
{
    CCharacter* character;
    int32_t oldSectorX;
    int32_t oldSectorY;
};

class CZone
{
public:
    // gameData — 기획 데이터. 몬스터 관리자가 몬스터·스폰 표를 쥐게 건넨다(USE_MONSTER — 끈 빌드엔 받는 쪽이 없다).
    explicit CZone(const CGameDataFiles& gameData);
    ~CZone();

    // 복사 금지 — 몬스터 관리자가 *this 를 든다. 복사하면 새 존의 관리자가 옛 존을 가리킨다.
    //   (지금은 복사하는 코드가 없어도 형질 검사로는 "복사 가능"이 나온다 — 쓰는 날 막히게 명시한다)
    CZone(const CZone&) = delete;
    CZone& operator=(const CZone&) = delete;

    // 존 초기화 (존 ID, 맵 ID, 맵 크기, 섹터 크기)
    bool Init(int32_t zoneId, int32_t mapId, int32_t mapWidth, int32_t mapHeight, int32_t sectorSize, const StartArea& start);

    // 캐릭터 입장 — 스폰 좌표 설정, 섹터 등록
    // actorId 발급은 CGameServer 가 담당 — 객체를 만들고 지우는 것은 플레이어는 CGameServer,
    //   몬스터는 그 존의 몬스터 관리자(CMonsterManager)가 한다
    // ★ 실패가 없다 — 들어갈 수 있는지(정원·같은 존 등)는 부르는 쪽이 들어가기 "전에" 다 본다
    //   (존 이동은 CMapManager::PickZoneChangeTarget). 그래서 존 이동은 「확인 → 옛 존 나가기 → 여기」 순서이고,
    //   나간 뒤에 들어가다 실패해 되돌리는 길이 없다. 확인할 것이 늘면 이 함수가 아니라 확인 쪽에 넣는다.
    void EnterZone(CCharacter* player);

    // 자리를 받는 입장 — 위 EnterZone 과 하는 일이 같고 스폰 좌표만 호출부가 준다(DB 에서 불러온 자리 · 포털 도착 자리).
    //   입장 절차는 이쪽 하나뿐이다: EnterZone 은 들어오는 자리(시작 구역 · 없으면 무작위 — CalcSpawnPos)를 뽑아 이걸 부른다.
    //   좌표가 맵 안인지는 호출부가 본다(IsInsideMap 으로) — 여기는 받은 대로 넣는다.
    void EnterZoneAt(CCharacter* player, float x, float y);

    // 캐릭터 퇴장 — 섹터 해제, 목록에서 제거 (delete하지 않음)
    void LeaveZone(CCharacter* player);

    // 프레임 갱신 — MOVING 캐릭터 좌표 갱신, 섹터 변경 감지(섹터 이동 대기열에 넣는다)
    //   맵 경계에 막혀 세운 캐릭터는 그 자리에서 events.OnCharacterMoveStop 으로 알린다
    void Tick(float deltaTime, IZoneEvents& events);

    // 존 정보
    int32_t GetZoneId() const { return _zoneId; }
    int32_t GetMapId() const { return _mapId; }
    int32_t GetMapWidth() const { return _mapWidth; }
    int32_t GetMapHeight() const { return _mapHeight; }

    // ── 맵 경계 ── 경계 판정은 이 넷에 모았다. 값 둘을 쓴다:
    //   맵 안 = 0 <= 좌표 < 맵 크기 (틱 클램프가 손대지 않는 범위)
    //   벽 선 = 맵 크기 - 1        (밖으로 나간 좌표를 되돌려 놓는 자리 · 벽에 붙었나의 기준)
    //   콘솔 클라(GameClient PlayerManager)가 같은 식을 복제하고, 웹 클라(live.html advance)가 벽 선을 상한으로 쓴다 — 바꾸면 같이.
    bool IsInsideMap(float x, float y) const;                     // 저장된 자리를 그대로 써도 되나 (NaN·무한대는 밖)
    bool ClampToMap(float& x, float& y) const;                    // 맵 밖이면 가장자리(0 또는 벽 선)로 되돌림. 되돌렸으면 true
    void ClampToWallLine(float& x, float& y) const;               // 벽 선까지 닫힌 범위로 자름 — 땅 아이템 놓을 자리
    bool IsBlockedByWall(float x, float y, Direction dir) const;  // 벽에 붙은 채 벽 쪽으로 가려 하나

    // ── 섹터 이동 ── 좌표를 바꾼 쪽이 부른다. 좌표가 가리키는 섹터로 등록을 옮긴다.
    //   화면(CREATE/DELETE)을 언제 고치느냐로 둘:
    bool UpdateSector(CCharacter* character);           // 옮기기만. 옮겼으면 true — 화면은 부른 쪽이 바로 고친다
    void UpdateSectorAndQueue(CCharacter* character);   // 옮기고 섹터 이동 대기열에 — 화면은 틱 끝(게임 루프 3)에 CGameServer 가 고친다

    // 섹터 이동 대기열 — 이번 틱에 섹터를 넘은 캐릭터와 첫 출발 섹터. 게임 루프 3)에 TakeSightChanges 가 읽고 비운다.
    //   퇴장하는 캐릭터는 LeaveZone 이 뺀다. 존이 지워지면 같이 사라진다.
    const std::vector<SectorChangeInfo>& GetSectorChanges() const { return _pendingSectorChanges; }

    // 원위치 복귀 — 첫 출발 섹터 = 지금 섹터면 이번 틱 시야 변화가 없다(넘었다가 되돌아옴). 대기열 항목만으로 정해진다.
    static bool IsReturnedHome(const SectorChangeInfo& change);

    // ── 시야 변화 ── 섹터 이동 대기열을 읽어 「누가 누구를 새로 보게 / 못 보게 됐나」를 정한다.
    //   보내는 것(패킷·송신 방식·계수)은 CGameServer 몫이다 — 여기는 명단만 만든다(존 계층은 세션·패킷을 모른다).
    //   명단은 섹터 단위다 — 받는 쪽이 섹터별 digest 로 묶어 보내므로 사람 단위로 펼치지 않는다.

    // 섹터를 넘은 캐릭터 하나 — 못 보게 된 섹터 · 새로 보게 된 섹터(GetSectorDiff 출력 그대로)
    struct SightMove
    {
        CCharacter* mover;
        CSectorManager::SectorPos removed[CSectorManager::MAX_AROUND_SECTORS];
        CSectorManager::SectorPos added[CSectorManager::MAX_AROUND_SECTORS];
        int32_t removedCount;
        int32_t addedCount;
    };
    // 같은 틱에 함께 움직여 위 섹터 단위 처리가 서로를 놓친 쌍 — visible 이면 서로 CREATE, 아니면 서로 DELETE
    struct SightPairFix
    {
        CCharacter* a;
        CCharacter* b;
        bool visible;
    };
    struct SightChanges
    {
        std::vector<SightMove> moves;           // 대기열 순서 — 원위치 복귀는 뺐다
        std::vector<SightPairFix> pairFixes;    // moves 를 다 보낸 뒤에 보낼 것
    };

    // 틱 끝(게임 루프 3) — 대기열로 명단을 만들고 대기열을 비운다. 돌려준 명단은 다음 호출까지 유효하다(존이 든 재사용 버퍼).
    const SightChanges& TakeSightChanges();

    // 틱 중간 조회 — 퇴장·땅 아이템이 일어난 자리에서 부른다(대기열은 그대로 둔다).
    //   CollectLeaveOldOnly : 이번 틱에 섹터를 넘은 퇴장자의 「옛 자리에서만 보이던 섹터」(안 넘었으면 0개).
    //                         outOldX/Y = 퇴장자의 옛 섹터(안 넘었으면 지금 섹터) — 아래 CollectLateMovers 에 넘긴다
    //   CollectLateMovers   : 「이동 전엔 대상을 보고 있었는데 지금 자리로는 안 보이고, 대상의 옛 자리로도 안 걸리는」 이동자.
    //                         대상이 안 움직이는 것(땅 아이템)이면 옛 = 지금으로 부른다. 몬스터도 돌려준다 — 받는 쪽 거르기는 부르는 쪽
    void CollectLeaveOldOnly(CCharacter* leaver, CSectorManager::SectorPos* outSectors, int32_t& outCount,
                             int32_t& outOldX, int32_t& outOldY) const;
    void CollectLateMovers(int32_t targetOldX, int32_t targetOldY, int32_t targetCurX, int32_t targetCurY,
                           const CCharacter* exclude, std::vector<CCharacter*>& out) const;

    // 채널 정원·빈 채널 판정용 — _characterList.size() 가 아니라 별도 카운터다.
    //   몬스터가 정원을 차지하면 사람이 못 들어오고, 몬스터만 남은 채널은 영영 안 지워진다.
    //   세는 기준은 _kind 다(Character.h [종류]).
    int32_t GetPlayerCount() const { return _playerCount; }
    const std::vector<CCharacter*>& GetCharacters() const { return _characterList; }

    // 섹터매니저 접근 (이동 처리 등 외부에서 필요)
    CSectorManager& GetSectorManager() { return _sectorManager; }
    const CSectorManager& GetSectorManager() const { return _sectorManager; }

#if USE_MONSTER
    // ── 몬스터 ── 이 존의 몬스터를 소유하고 깔고 판단하는 관리자(MonsterManager.h).
    //   게임 루프가 틱마다(1-1 판단) 이걸 꺼내 부른다(GetSectorManager 와 같은 꼴).
    //   몬스터는 _characterList·섹터에도 들어간다(거기 있어야 움직이고 보인다) — 둘 다 빌린 포인터만 든다.
    CMonsterManager& GetMonsterManager() { return _monsterManager; }
#endif

#if USE_DROP
    // ── 땅 아이템 ── 이 존 땅에 떨어진 것과 그 규칙을 함께 든 관리자(GroundItemManager.h).
    //   게임 서버가 떨어뜨릴 때·줍기·틱 3-3(수명 훑기)·채널 정리·시야 송신 때 꺼내 부른다(몬스터 관리자와 같은 꼴).
    CGroundItemManager& GetGroundItemManager() { return _groundItemManager; }
#endif

private:
    // 스폰 좌표 계산 — 시작 구역이 있으면 그 안, 없으면 맵 전체 무작위(맵 표 StartArea)
    void CalcSpawnPos(float& outX, float& outY) const;

    // 섹터 이동 대기열에 추가 (같은 캐릭터는 최초 출발 섹터만 유지)
    void PushSectorChange(CCharacter* character, int32_t oldSectorX, int32_t oldSectorY);
    const SectorChangeInfo* FindSectorChange(CCharacter* character) const;   // 없으면 nullptr
    void ClearSectorChanges();

    // 같은 틱 이동자 쌍 판정 — TakeSightChanges 가 부른다. 까닭은 Zone.cpp 머리 주석
    void CollectPairFixes(std::vector<SightPairFix>& out) const;

    int32_t _zoneId;
    int32_t _mapId;
    int32_t _mapWidth;
    int32_t _mapHeight;
    StartArea _start;   // 지정 자리 없이 들어올 때의 자리 — 없으면(has == false) 맵 전체(CalcSpawnPos)

    CSectorManager _sectorManager;

    // 순회용 (Tick 캐시 지역성) — 플레이어·몬스터가 함께 담긴다
    std::vector<CCharacter*> _characterList;

    // _characterList 중 _kind == PLAYER 인 것의 수 (채널 정원·빈 채널 판정 전용).
    //   목록에 넣고 빼는 그 자리에서만 증감한다 — 어긋나면 채널이 안 지워지거나 정원이 새어 나간다.
    int32_t _playerCount;

    // 섹터 이동 대기열 (틱 안에 모음 → 게임 루프 3)에서 일괄 처리). 이 존 캐릭터만 들어온다(PushSectorChange 단언)
    std::vector<SectorChangeInfo> _pendingSectorChanges;
    std::unordered_set<CCharacter*> _sectorChangedSet;  // 이미 기록된 캐릭터 필터

    // 시야 변화 명단 — TakeSightChanges 가 틱마다 비우고 채운다(capacity 유지)
    SightChanges _sightChanges;

#if USE_MONSTER
    // 몬스터 관리자 — 몬스터 소유·깔았나 표시. *this 를 들므로 존은 복사하지 않는다(위 삭제 두 줄).
    //   해제는 존 소멸(빈 채널 정리·서버 종료) 때 멤버 소멸로 — 밖에서 먼저 불러 줄 것이 없다.
    //   기획 데이터를 받으므로 생성자 초기화 목록에서 세운다(Zone.cpp).
    CMonsterManager _monsterManager;
#endif

#if USE_DROP
    // 땅 아이템 관리자 — 땅 아이템(값)·규칙·이 존 개수. 칸 번호는 위 _sectorManager 와 같다(Init 이 맞춘다).
    //   *this 를 들므로 존은 복사하지 않는다(위 삭제 두 줄).
    CGroundItemManager _groundItemManager{ *this };
#endif
};
