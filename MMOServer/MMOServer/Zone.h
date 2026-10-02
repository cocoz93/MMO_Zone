// ==========================================================================
// CZone — 존 단위 데이터 컨테이너
//
// [책임]
//  - 맵 크기, CSectorManager, 캐릭터 목록 소유 (vector<CCharacter*> — 플레이어·몬스터 공통)
//  - 몬스터 관리자 소유 (_monsterManager, 값) — 몬스터 객체·깔기·판단·부활은 그쪽(MonsterManager.h).
//    존과 같이 태어나고 같이 사라지므로. 플레이어 객체는 접속 수명이라 CGameServer 가 소유한다
//  - 캐릭터 입장/퇴장, 섹터 등록/해제 등 데이터 관리
//  - 맵 경계 규칙(맵 안 검사·클램프·벽 판정) — 맵 크기를 든 곳이 여기라서
//  - 패킷 처리·브로드캐스트는 CGameServer, 몬스터 판단은 CMonsterManager 가 담당
//
// [설계]
//  - 싱글스레드 전제 (게임 로직 스레드 1개에서만 호출)
//  - 스폰 좌표는 맵 전체 균등분포 랜덤 (경계 1칸 여유)
// ==========================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "Actor.h"         // Direction — 벽 판정(IsBlockedByWall)이 방향을 받는다
#include "BuildConfig.h"
#include "SectorManager.h"
#include "MonsterManager.h"   // 몬스터 관리자 — 존이 값으로 든다(USE_MONSTER 가 꺼져 있으면 빈 헤더)

class CCharacter;

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
    CZone();
    ~CZone();

    // 복사 금지 — 몬스터 관리자가 *this 를 든다. 복사하면 새 존의 관리자가 옛 존을 가리킨다.
    //   (지금은 복사하는 코드가 없어도 형질 검사로는 "복사 가능"이 나온다 — 쓰는 날 막히게 명시한다)
    CZone(const CZone&) = delete;
    CZone& operator=(const CZone&) = delete;

    // 존 초기화 (존 ID, 맵 ID, 맵 크기, 섹터 크기)
    bool Init(int32_t zoneId, int32_t mapId, int32_t mapWidth, int32_t mapHeight, int32_t sectorSize);

    // 캐릭터 입장 — 스폰 좌표 설정, 섹터 등록
    // actorId 발급은 CGameServer 가 담당 — 객체를 만들고 지우는 것은 플레이어는 CGameServer,
    //   몬스터는 그 존의 몬스터 관리자(CMonsterManager)가 한다
    bool EnterZone(CCharacter* player);

    // 자리를 받는 입장 — 위 EnterZone 과 하는 일이 같고 스폰 좌표만 호출부가 준다(DB 에서 불러온 자리).
    //   입장 절차는 이쪽 하나뿐이다: EnterZone 은 무작위 자리를 뽑아 이걸 부른다.
    //   좌표가 맵 안인지는 호출부가 본다(IsInsideMap 으로) — 여기는 받은 대로 넣는다.
    bool EnterZoneAt(CCharacter* player, float x, float y);

    // 캐릭터 퇴장 — 섹터 해제, 목록에서 제거 (delete하지 않음)
    void LeaveZone(CCharacter* player);

    // 프레임 갱신 — MOVING 캐릭터 좌표 갱신, 섹터 변경 감지
    // outClampedCharacters: 맵 경계 클램핑으로 IDLE 전환된 캐릭터 (MOVE_STOP 브로드캐스트 필요)
    void Tick(float deltaTime, std::vector<SectorChangeInfo>& outSectorChanges,
             std::vector<CCharacter*>& outClampedCharacters);

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
    //   옮겼으면 true — 화면(CREATE/DELETE)을 고치는 것은 부른 쪽 몫이다.
    bool UpdateSector(CCharacter* character);

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
    //   게임 루프가 틱의 두 시점(1-1 판단 · 3-2 부활)에 이걸 꺼내 부른다(GetSectorManager 와 같은 꼴).
    //   몬스터는 _characterList·섹터에도 들어간다(거기 있어야 움직이고 보인다) — 둘 다 빌린 포인터만 든다.
    CMonsterManager& GetMonsterManager() { return _monsterManager; }
#endif

private:
    // 스폰 좌표 계산 (맵 전체 랜덤)
    void CalcSpawnPos(float& outX, float& outY) const;

    int32_t _zoneId;
    int32_t _mapId;
    int32_t _mapWidth;
    int32_t _mapHeight;

    CSectorManager _sectorManager;

    // 순회용 (Tick 캐시 지역성) — 플레이어·몬스터가 함께 담긴다
    std::vector<CCharacter*> _characterList;

    // _characterList 중 _kind == PLAYER 인 것의 수 (채널 정원·빈 채널 판정 전용).
    //   목록에 넣고 빼는 그 자리에서만 증감한다 — 어긋나면 채널이 안 지워지거나 정원이 새어 나간다.
    int32_t _playerCount;

#if USE_MONSTER
    // 몬스터 관리자 — 몬스터 소유·깔았나 표시·부활 대기열. *this 를 들므로 존은 복사하지 않는다(위 삭제 두 줄).
    //   해제는 존 소멸(빈 채널 정리·서버 종료) 때 멤버 소멸로 — 밖에서 먼저 불러 줄 것이 없다.
    CMonsterManager _monsterManager{ *this };
#endif
};
