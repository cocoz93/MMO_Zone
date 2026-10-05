// ==========================================================================
// CSectorManager — 존 내부 2D 그리드 섹터 시스템
//
// [설계 기준]
//  - 콘솔 클라이언트 80x21 뷰 (카메라 크기, 맵 크기와 무관)
//  - 섹터 크기 20x20 (정사각형), 맵 크기는 INI 설정 (기본 120x120 → 6x6 그리드)
//  - 주변 9섹터 순회 시 커버 범위 60x60 → 클라 뷰(80x21)보다 넉넉
//  - 섹터 크기 ≈ 클라 화면 반절, 시야보다 크게 잡아 화면 내 팝업 방지
//
// [목록 둘] 섹터 한 칸은 목록을 둘 든다 — 플레이어(CPlayer*)와 나머지(CCharacter* — 몬스터, 앞으로 NPC·펫).
//  한 캐릭터는 둘 중 한 목록에만 들어간다. 종류를 묻는 자리는 AddCharacter·RemoveCharacter 둘뿐이고
//  (_kind 는 생성자에서만 정해지므로 넣을 때와 뺄 때 다른 목록을 고를 길이 없다), 그 뒤는 목록이 곧 종류다.
//   - 패킷을 받는 쪽은 플레이어뿐 → 방송·팬아웃·digest 는 플레이어 목록만 돈다(뱀 거르기 검사가 없다)
//   - "보이는 것 전부"가 필요한 자리(시야 진입 때 상대 목록, 플레이어의 공격 대상 찾기)는 ForEachInSector 로 두 목록을 차례로 돈다
//   - 몬스터 AI 의 대상 찾기·몬스터의 물기는 플레이어 목록만 돈다 → 뱀끼리 쫓거나 무는 일이 구조로 막힌다
//  가르는 기준은 "받을 수 있나"다 — 종류마다 목록을 두지 않는다. NPC·펫이 생겨도 나머지 목록에 들어가고 목록은 안 는다.
//
// [땅의 아이템] 여기 없다 — 존의 땅 아이템 관리자가 같은 칸 번호로 든다(GroundItemManager.h).
//  땅은 아이템을 담는 그릇이라 물건과 규칙을 그쪽이 함께 든다. 여기는 캐릭터 목록 둘만 — 칸 번호 계산(CalcSectorX/Y)은 빌려 준다.
//
// [사용 흐름]
//  Init(mapWidth, mapHeight, sectorSize) → 존 생성 시 호출 (INI에서 로드된 값)
//  CalcSectorX/Y()          → 월드 좌표에서 섹터 좌표 계산
//  AddCharacter/Remove      → 섹터에 캐릭터 등록/해제 (종류로 목록을 고른다)
//  GetSectorPlayers/Others  → 한 섹터의 목록 둘 · ForEachInSector → 둘을 차례로
//  GetAroundPlayers()       → 주변 9섹터 플레이어 수집 (브로드캐스트 대상)
//  GetSectorDiff()          → 섹터 변경 시 시야 진입/이탈 섹터 계산 (CREATE/DELETE용)
//  IsAdjacent()             → 두 섹터가 서로 보이나 (시야 규칙 — GetAroundSectorList 와 같은 판정)
// ==========================================================================
#pragma once

#include <cstdint>
#include <vector>

class CCharacter;
class CPlayer;

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

    // 캐릭터 등록/해제 — 종류로 목록을 고른다(AsPlayer). 종류를 묻는 자리는 이 둘뿐이다
    void AddCharacter(CCharacter* character, int32_t sectorX, int32_t sectorY);
    void RemoveCharacter(CCharacter* character, int32_t sectorX, int32_t sectorY);

    // 단일 섹터의 목록 둘 (범위 밖이면 빈 목록)
    const std::vector<CPlayer*>&    GetSectorPlayers(int32_t sectorX, int32_t sectorY) const;
    const std::vector<CCharacter*>& GetSectorOthers(int32_t sectorX, int32_t sectorY) const;

    // 두 목록을 차례로 돈다 — "보이는 것 전부"가 필요한 자리 전용(시야 진입 때 상대 목록, 플레이어의 공격 대상 찾기).
    //   한 자리에 모아 두는 이유 — 한쪽 목록을 빠뜨리면 컴파일이 아니라 화면에서 드러나므로, 빠뜨릴 자리를 이 하나로 줄인다.
    //   fn 은 CCharacter* 하나를 받는다. 도는 동안 목록을 바꾸지 말 것(등록·해제 금지 — 호출처는 전부 송신·판정뿐이다).
    template <typename Fn>
    void ForEachInSector(int32_t sectorX, int32_t sectorY, Fn&& fn) const
    {
        for (CPlayer* p : GetSectorPlayers(sectorX, sectorY))
            fn(p);
        for (CCharacter* c : GetSectorOthers(sectorX, sectorY))
            fn(c);
    }

    // 주변 9섹터(본인 섹터 포함) 플레이어 수집 — 브로드캐스트 대상. exclude 가 지정되면 제외한다.
    //   exclude 가 CCharacter* 인 이유 — 본인이 뱀일 수 있다(뱀의 공격·부활 방송). 그때는 플레이어 목록에서 뺄 것이 없어 그냥 전부 모인다.
    void GetAroundPlayers(int32_t sectorX, int32_t sectorY,
                          std::vector<CPlayer*>& outPlayers,
                          const CCharacter* exclude = nullptr) const;

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

    // 주변 9섹터 좌표 수집 (경계 클램핑 포함, 고정 배열)
    // outSectors: 최대 9개, outCount: 실제 개수
    //   시야 규칙은 아래 IsAdjacent 와 같다(dx, dy ∈ [-1, 1]) — 하나를 바꾸면 다른 하나도 바꿀 것.
    void GetAroundSectorList(int32_t sectorX, int32_t sectorY,
                             SectorPos* outSectors, int32_t& outCount) const;

    // 두 섹터가 서로의 시야(주변 9섹터) 안인가 — 체비쇼프 거리 ≤ 1.
    //   GetAroundSectorList(SectorManager.cpp)가 dx,dy ∈ [-1,1]을 모으는 것과 같은 판정을
    //   섹터 목록을 만들지 않고 좌표만으로 계산한다(맵 밖 섹터는 애초에 주민이 없어 결과 동일).
    //   ※ 섹터 좌표는 존마다 별개 격자이므로, 호출부에서 반드시 같은 존인지 먼저 확인할 것.
    static bool IsAdjacent(int32_t x1, int32_t y1, int32_t x2, int32_t y2)
    {
        int32_t dx = x1 - x2;
        int32_t dy = y1 - y2;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        return dx <= 1 && dy <= 1;
    }

private:
    bool IsValidSector(int32_t sectorX, int32_t sectorY) const;

    // 평탄화 인덱스 접근 헬퍼 — 한 칸 = 플레이어 목록 + 나머지 목록
    std::vector<CPlayer*>&          PlayersAt(int32_t y, int32_t x)       { return _players[y * _sectorCountX + x]; }
    const std::vector<CPlayer*>&    PlayersAt(int32_t y, int32_t x) const { return _players[y * _sectorCountX + x]; }
    std::vector<CCharacter*>&       OthersAt(int32_t y, int32_t x)        { return _others[y * _sectorCountX + x]; }
    const std::vector<CCharacter*>& OthersAt(int32_t y, int32_t x) const  { return _others[y * _sectorCountX + x]; }

    int32_t _sectorSize;
    int32_t _sectorCountX;
    int32_t _sectorCountY;

    // 2D 그리드 (평탄화): PlayersAt/OthersAt(y, x)로 접근. 같은 칸 번호가 두 벡터의 같은 자리다
    std::vector<std::vector<CPlayer*>>    _players;
    std::vector<std::vector<CCharacter*>> _others;

    // 범위 밖 요청 시 반환할 빈 목록
    static const std::vector<CPlayer*>    EMPTY_PLAYERS;
    static const std::vector<CCharacter*> EMPTY_OTHERS;
};
