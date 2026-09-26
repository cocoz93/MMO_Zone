// ==========================================================================
// CZone — 존 단위 데이터 컨테이너
//
// [책임]
//  - 맵 크기, CSectorManager, 캐릭터 목록 소유 (vector<CCharacter*> — 플레이어·몬스터 공통)
//  - 캐릭터 입장/퇴장, 섹터 등록/해제 등 데이터 관리
//  - 게임 로직(패킷 처리, 브로드캐스트)은 CGameServer가 담당
//
// [설계]
//  - 싱글스레드 전제 (게임 로직 스레드 1개에서만 호출)
//  - 스폰 좌표는 맵 전체 균등분포 랜덤 (경계 1칸 여유)
// ==========================================================================
#pragma once

#include <cstdint>
#include <vector>

#include "BuildConfig.h"
#include "SectorManager.h"

class CCharacter;
#if USE_MONSTER
class CMonster;   // 포인터로만 담으므로 전방 선언이면 된다 (Monster.h 를 끌어오지 않는다)
#endif

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

    // 존 초기화 (존 ID, 맵 ID, 맵 크기, 섹터 크기)
    bool Init(int32_t zoneId, int32_t mapId, int32_t mapWidth, int32_t mapHeight, int32_t sectorSize);

    // 캐릭터 입장 — 스폰 좌표 설정, 섹터 등록
    // actorId와 CCharacter의 생성/삭제는 CGameServer가 담당
    bool EnterZone(CCharacter* player);

    // 자리를 받는 입장 — 위 EnterZone 과 하는 일이 같고 스폰 좌표만 호출부가 준다(DB 에서 불러온 자리).
    //   입장 절차는 이쪽 하나뿐이다: EnterZone 은 무작위 자리를 뽑아 이걸 부른다.
    //   좌표가 맵 안인지는 호출부가 본다(CGameServer::IsValidSpawnPos) — 여기는 받은 대로 넣는다.
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
    // 채널 정원·빈 채널 판정용 — _characterList.size() 가 아니라 별도 카운터다.
    //   몬스터가 정원을 차지하면 사람이 못 들어오고, 몬스터만 남은 채널은 영영 안 지워진다.
    //   세는 기준은 _kind 다(Character.h [종류]).
    int32_t GetPlayerCount() const { return _playerCount; }
    const std::vector<CCharacter*>& GetCharacters() const { return _characterList; }

    // 섹터매니저 접근 (이동 처리 등 외부에서 필요)
    CSectorManager& GetSectorManager() { return _sectorManager; }
    const CSectorManager& GetSectorManager() const { return _sectorManager; }

#if USE_MONSTER
    // ── 몬스터 목록 ── AI 를 돌릴 대상만 모은 것. _characterList 와 별개로 둔다.
    //   주민 목록을 안 가르는 이유는 "누구에게 보낼까"라 종류가 상관없어서였고,
    //   여기는 "누가 스스로 판단하나"라 종류가 곧 조건이다. 몬스터는 두 목록에 다 들어간다
    //   (_characterList 에 있어야 눈에 보이고 맞는다).
    //   ★ 소유는 CGameServer 다 — 이 목록도 delete 하지 않는다(_characterList 와 같은 규칙,
    //     Zone.cpp 소멸자 주석 참조).
    //   ★ 원소 타입이 CCharacter* 가 아니라 CMonster* 인 이유 — AI 가 _aiState 같은 몬스터 전용
    //     필드를 읽는데, CCharacter* 로 담으면 순회마다 내려 캐스팅을 하게 되고 그 캐스팅은
    //     "여기엔 몬스터만 들어 있다"는 전제에 기댄다. 타입으로 담으면 컴파일러가 그 전제를 지킨다.
    void AddMonster(CMonster* monster) { _monsterList.push_back(monster); }
    const std::vector<CMonster*>& GetMonsters() const { return _monsterList; }
    void ClearMonsters() { _monsterList.clear(); }

    // 이 존에 몬스터를 이미 깔았나 — 게임 루프가 새 존을 발견하는 표식.
    //   존 생성 직후에 부를 자리가 없어서(CreateChannel 이 private 이고 호출처가 전부
    //   MapManager 안이다) 루프가 훑어서 채운다. 존이 지워지면 이 플래그도 같이 사라진다.
    bool IsMonsterSpawned() const { return _monstersSpawned; }
    void MarkMonsterSpawned() { _monstersSpawned = true; }
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
    // AI 순회 전용 목록 — 원소는 _characterList 에도 들어 있다(같은 포인터).
    //   ★ LeaveZone 은 이 목록을 건드리지 않는다. 몬스터가 LeaveZone 을 타는 길은
    //     CGameServer::DespawnMonsters 하나뿐인데, 거기서 이 목록을 돌며 부르기 때문이다
    //     — 순회 중에 같은 벡터에서 원소를 빼면 동작을 보장 못 한다. 다 돈 뒤 ClearMonsters 로 비운다.
    std::vector<CMonster*> _monsterList;

    bool _monstersSpawned = false;
#endif
};
