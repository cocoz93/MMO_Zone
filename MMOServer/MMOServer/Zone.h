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

#include "SectorManager.h"

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

    // 존 초기화 (존 ID, 맵 ID, 맵 크기, 섹터 크기)
    bool Init(int32_t zoneId, int32_t mapId, int32_t mapWidth, int32_t mapHeight, int32_t sectorSize);

    // 캐릭터 입장 — 스폰 좌표 설정, 섹터 등록
    // actorId와 CCharacter의 생성/삭제는 CGameServer가 담당
    bool EnterZone(CCharacter* player);

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
    // 채널 정원·빈 채널 판정용 — 지금은 플레이어만 담겨 목록 크기와 같다.
    //   몬스터가 들어오면 세션 있는 것만 세야 한다 (몬스터가 정원을 차지하면 안 된다).
    int32_t GetPlayerCount() const { return static_cast<int32_t>(_characterList.size()); }
    const std::vector<CCharacter*>& GetCharacters() const { return _characterList; }

    // 섹터매니저 접근 (이동 처리 등 외부에서 필요)
    CSectorManager& GetSectorManager() { return _sectorManager; }
    const CSectorManager& GetSectorManager() const { return _sectorManager; }

private:
    // 스폰 좌표 계산 (맵 전체 랜덤)
    void CalcSpawnPos(float& outX, float& outY) const;

    int32_t _zoneId;
    int32_t _mapId;
    int32_t _mapWidth;
    int32_t _mapHeight;

    CSectorManager _sectorManager;

    // 순회용 (Tick 캐시 지역성)
    std::vector<CCharacter*> _characterList;
};
