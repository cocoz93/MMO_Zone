// ==========================================================================
// CSectorManager — 존 내부 2D 그리드 섹터 시스템
//
// [설계 기준]
//  - 콘솔 클라이언트 80x21 뷰 (카메라 크기, 맵 크기와 무관)
//  - 섹터 크기 20x20 (정사각형), 맵 크기는 INI 설정 (기본 120x120 → 6x6 그리드)
//  - 주변 9섹터 순회 시 커버 범위 60x60 → 클라 뷰(80x21)보다 넉넉
//  - 섹터 크기 ≈ 클라 화면 반절, 시야보다 크게 잡아 화면 내 팝업 방지
//
// [사용 흐름]
//  Init(mapWidth, mapHeight, sectorSize) → 존 생성 시 호출 (INI에서 로드된 값)
//  CalcSectorX/Y()    → 월드 좌표에서 섹터 좌표 계산
//  AddCharacter/Remove   → 섹터에 캐릭터 등록/해제 (플레이어·몬스터 공통 — vector<CCharacter*>)
//  GetAroundCharacters() → 브로드캐스트 대상 수집
//  GetSectorDiff()    → 섹터 변경 시 시야 진입/이탈 섹터 계산 (CREATE/DELETE용)
// ==========================================================================
#pragma once

#include <cstdint>
#include <vector>

class CCharacter;

class CSectorManager
{
public:
    CSectorManager();
    ~CSectorManager();

    // 초기화 (맵 크기, 섹터 크기)
    bool Init(int32_t mapWidth, int32_t mapHeight, int32_t sectorSize);

    // 월드 좌표 → 섹터 좌표 변환
    int32_t CalcSectorX(float worldX) const;
    int32_t CalcSectorY(float worldY) const;

    // 캐릭터 등록/해제
    void AddCharacter(CCharacter* player, int32_t sectorX, int32_t sectorY);
    void RemoveCharacter(CCharacter* player, int32_t sectorX, int32_t sectorY);

    // 단일 섹터의 캐릭터 목록
    const std::vector<CCharacter*>& GetSectorCharacters(int32_t sectorX, int32_t sectorY) const;

    // 주변 9섹터(본인 섹터 포함) 캐릭터 수집
    // exclude가 지정되면 해당 캐릭터는 제외
    void GetAroundCharacters(int32_t sectorX, int32_t sectorY,
                          std::vector<CCharacter*>& outCharacters,
                          CCharacter* exclude = nullptr) const;

    // 섹터 이동 시 새로 시야에 들어온/나간 섹터 목록 계산
    // oldSector 주변에는 있었지만 newSector 주변에는 없는 섹터 → outRemoved
    // newSector 주변에는 있지만 oldSector 주변에는 없던 섹터 → outAdded
    struct SectorPos { int32_t x; int32_t y; };
    void GetSectorDiff(int32_t oldSectorX, int32_t oldSectorY,
                       int32_t newSectorX, int32_t newSectorY,
                       SectorPos* outAdded, int32_t& outAddedCount,
                       SectorPos* outRemoved, int32_t& outRemovedCount) const;

    int32_t GetSectorSize() const { return _sectorSize; }

    // 그리드 크기 (USE_BROADCAST_BUNDLE: 수신섹터 순회용)
    int32_t GetSectorCountX() const { return _sectorCountX; }
    int32_t GetSectorCountY() const { return _sectorCountY; }

    static constexpr int32_t MAX_AROUND_SECTORS = 9;

private:
    bool IsValidSector(int32_t sectorX, int32_t sectorY) const;

    // 평탄화 인덱스 접근 헬퍼
    std::vector<CCharacter*>& SectorAt(int32_t y, int32_t x) { return _sectors[y * _sectorCountX + x]; }
    const std::vector<CCharacter*>& SectorAt(int32_t y, int32_t x) const { return _sectors[y * _sectorCountX + x]; }

    // 주변 9섹터 좌표 수집 (경계 클램핑 포함, 고정 배열)
    // outSectors: 최대 9개, outCount: 실제 개수
    void GetAroundSectorList(int32_t sectorX, int32_t sectorY,
                             SectorPos* outSectors, int32_t& outCount) const;

    int32_t _sectorSize;
    int32_t _sectorCountX;
    int32_t _sectorCountY;

    // 2D 그리드 (평탄화): SectorAt(y, x)로 접근
    std::vector<std::vector<CCharacter*>> _sectors;

    // GetSectorCharacters에서 범위 밖 요청 시 반환할 빈 벡터
    static const std::vector<CCharacter*> EMPTY_SECTOR;
};
