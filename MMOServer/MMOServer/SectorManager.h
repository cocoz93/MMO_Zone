// ==========================================================================
// CSectorManager — 존 내부 2D 그리드 섹터 시스템
//
// [설계 기준]
//  - 콘솔 클라이언트 80x21 뷰 (카메라 크기, 맵 크기와 무관)
//  - 섹터 크기 20x20 (정사각형, INI 운영 값), 맵 크기는 맵 표 MapTable.csv (기본 120x120 → 6x6 그리드)
//  - 주변 9섹터 순회 시 커버 범위 60x60 → 클라 뷰(80x21)보다 넉넉
//  - 섹터 크기 ≈ 클라 화면 반절, 시야보다 크게 잡아 화면 내 팝업 방지
//
// [목록 셋] 섹터 한 칸은 목록을 셋 든다 — 플레이어(CPlayer*) · 맞을 수 있는 나머지(CCharacter* — 체력 컴포넌트가 있는 비플레이어, 지금은 몬스터)
//  · 그 밖(CCharacter* — 체력 컴포넌트가 없는 것, 지금은 마을 NPC). 한 캐릭터는 셋 중 한 목록에만 들어간다.
//  목록을 묻는 자리는 AddCharacter·RemoveCharacter 둘뿐이고(종류와 컴포넌트는 생성자에서만 정해지므로 넣을 때와 뺄 때
//  다른 목록을 고를 길이 없다 — Character.h 컴포넌트 자리), 그 뒤는 목록이 곧 「무엇을 할 수 있나」다.
//   - 패킷을 받는 쪽은 플레이어뿐 → 방송·팬아웃·digest 는 플레이어 목록만 돈다(뱀 거르기 검사가 없다)
//   - "보이는 것 전부"가 필요한 자리(시야 진입·이탈 때 상대 목록)는 ForEachInSector 로 세 목록을 차례로 돈다
//   - "맞을 수 있는 것 전부"가 필요한 자리(플레이어의 공격 대상 찾기)는 ForEachHittableInSector 로 앞 둘만 돈다
//   - 몬스터 AI 의 대상 찾기·몬스터의 물기는 플레이어 목록만 돈다 → 뱀끼리 쫓거나 무는 일이 구조로 막힌다
//  가르는 기준은 "무엇을 할 수 있나"(받을 수 있나 · 맞을 수 있나)다 — 종류마다 목록을 두지 않는다. 펫이 생겨도 체력 컴포넌트가 있으면 둘째 목록이다.
//
// [땅의 아이템] 여기 없다 — 존의 땅 아이템 관리자가 같은 칸 번호로 든다(GroundItemManager.h).
//  땅은 아이템을 담는 그릇이라 물건과 규칙을 그쪽이 함께 든다. 여기는 캐릭터 목록 셋만 — 그리드 계산(CalcSectorX/Y · CellIndex · IsValidSector)은 빌려 준다.
//
// [그리드 규칙은 여기 한 곳] 섹터 좌표 ↔ 칸 번호(CellIndex/CellPos) · 칸 수(CellCount) · 주변 9섹터(GetAroundSectorList) ·
//  서로 보이나(IsAdjacent). 섹터별로 무언가를 두는 쪽(땅 아이템 관리자 · 게임 서버 송신 보류 버퍼)은 식을 베끼지 않고 이걸 부른다.
//
// [사용 흐름]
//  Init(mapWidth, mapHeight, sectorSize) → 존 생성 시 호출 (INI에서 로드된 값)
//  CalcSectorX/Y()          → 월드 좌표에서 섹터 좌표 계산
//  CellIndex/CellPos/Count  → 섹터 좌표 ↔ 칸 번호 · 칸 수 (평탄화 규칙 — 식은 여기뿐)
//  AddCharacter/Remove      → 섹터에 캐릭터 등록/해제 (사람인가 → 체력 컴포넌트가 있나로 목록을 고른다)
//  GetSectorPlayers/Hittables/NonHittables → 한 섹터의 목록 셋 · ForEachInSector → 셋을 차례로 · ForEachHittableInSector → 앞 둘
//  ForEachSectorInRange()   → 반경이 걸치는 섹터만 돌기 (공격 사거리·몬스터 시야·줍기 — 어느 섹터를 도나는 여기뿐)
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

    // 캐릭터 등록/해제 — 사람인가 → 체력 컴포넌트가 있나로 목록을 고른다(SectorManager.cpp ListOf). 목록을 묻는 자리는 이 둘뿐이다
    void AddCharacter(CCharacter* character, int32_t sectorX, int32_t sectorY);
    void RemoveCharacter(CCharacter* character, int32_t sectorX, int32_t sectorY);

    // 단일 섹터의 목록 셋 (범위 밖이면 빈 목록)
    const std::vector<CPlayer*>&    GetSectorPlayers(int32_t sectorX, int32_t sectorY) const;
    const std::vector<CCharacter*>& GetSectorHittables(int32_t sectorX, int32_t sectorY) const;      // 맞을 수 있는 나머지 — 체력 컴포넌트 있음
    const std::vector<CCharacter*>& GetSectorNonHittables(int32_t sectorX, int32_t sectorY) const;   // 그 밖 — 체력 컴포넌트 없음

    // 세 목록을 차례로 돈다 — "보이는 것 전부"가 필요한 자리 전용(시야 진입·이탈 때 상대 목록).
    //   한 자리에 모아 두는 이유 — 한쪽 목록을 빠뜨리면 컴파일이 아니라 화면에서 드러나므로, 빠뜨릴 자리를 이 하나로 줄인다.
    //   fn 은 CCharacter* 하나를 받는다. 도는 동안 목록을 바꾸지 말 것(등록·해제 금지 — 호출처는 전부 송신·판정뿐이다).
    template <typename Fn>
    void ForEachInSector(int32_t sectorX, int32_t sectorY, Fn&& fn) const
    {
        for (CPlayer* p : GetSectorPlayers(sectorX, sectorY))
            fn(p);
        for (CCharacter* c : GetSectorHittables(sectorX, sectorY))
            fn(c);
        for (CCharacter* c : GetSectorNonHittables(sectorX, sectorY))
            fn(c);
    }

    // 맞을 수 있는 것만 차례로 — 플레이어 + 맞을 수 있는 나머지(플레이어의 공격 대상 찾기). 그 밖 목록(체력 컴포넌트 없음)은 안 돈다.
    //   한 자리에 모아 두는 이유 · 지킬 것은 ForEachInSector 와 같다. 여기서 나온 것은 체력 컴포넌트가 늘 있다(전투가 단언으로 꺼낸다).
    template <typename Fn>
    void ForEachHittableInSector(int32_t sectorX, int32_t sectorY, Fn&& fn) const
    {
        for (CPlayer* p : GetSectorPlayers(sectorX, sectorY))
            fn(p);
        for (CCharacter* c : GetSectorHittables(sectorX, sectorY))
            fn(c);
    }

    // 반경이 걸치는 섹터만 돈다 — 「어느 섹터를 도나」(그리드 규칙)는 여기, 「그 섹터에서 무엇을 보나」는 fn 이 정한다.
    //   fn 은 (sectorX, sectorY) 를 받는다. 중심 ± range 를 CalcSectorX/Y 로 자르므로 맵 밖은 가장자리 섹터로 묶인다.
    //   GetAroundPlayers(3×3 고정 · 벡터 복사) 대신 참조로 훑는 자리가 쓴다 — 공격 사거리 · 몬스터 시야 · 줍기 반경.
    //   섹터가 20타일이라 대부분 1칸, 경계에 서 있을 때만 2~4칸이다. 도는 동안 목록을 바꾸지 말 것(ForEachInSector 와 같음).
    template <typename Fn>
    void ForEachSectorInRange(float x, float y, float range, Fn&& fn) const
    {
        const int32_t sx0 = CalcSectorX(x - range);
        const int32_t sx1 = CalcSectorX(x + range);
        const int32_t sy0 = CalcSectorY(y - range);
        const int32_t sy1 = CalcSectorY(y + range);
        for (int32_t sy = sy0; sy <= sy1; ++sy)
        {
            for (int32_t sx = sx0; sx <= sx1; ++sx)
                fn(sx, sy);
        }
    }

    // 주변 9섹터(본인 섹터 포함) 플레이어 수집 — 브로드캐스트 대상. exclude 가 지정되면 제외한다.
    //   exclude 가 CCharacter* 인 이유 — 본인이 뱀일 수 있다(뱀의 공격 방송). 그때는 플레이어 목록에서 뺄 것이 없어 그냥 전부 모인다.
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

    // 그리드 크기 — 전체 칸을 훑는 쪽(땅 아이템 수명 훑기)이 쓴다
    int32_t GetSectorCountX() const { return _sectorCountX; }
    int32_t GetSectorCountY() const { return _sectorCountY; }

    // 칸 번호 — 그리드를 한 줄 배열에 까는 규칙(행 우선: y × 가로칸수 + x). 섹터별로 무언가를 두는 쪽
    //   (여기의 목록 셋 · 땅 아이템 관리자의 목록 · 게임 서버의 송신 보류 버퍼)이 전부 이 둘을 쓴다 —
    //   식과 가로칸수를 여기 한 곳에만 둔다(사본 금지). CellCount 는 그 배열의 크기.
    int32_t CellIndex(int32_t sectorX, int32_t sectorY) const { return sectorY * _sectorCountX + sectorX; }
    void    CellPos(int32_t cellIndex, int32_t& outSectorX, int32_t& outSectorY) const
    {
        outSectorY = cellIndex / _sectorCountX;
        outSectorX = cellIndex % _sectorCountX;
    }
    int32_t CellCount() const { return _sectorCountX * _sectorCountY; }

    // 그리드 안의 섹터 좌표인가 — 땅 아이템 관리자도 같은 그리드를 쓰므로 공개
    bool IsValidSector(int32_t sectorX, int32_t sectorY) const;

    static constexpr int32_t MAX_AROUND_SECTORS = 9;

    // 주변 9섹터 좌표 수집 (경계 클램핑 포함, 고정 배열)
    // outSectors: 최대 9개, outCount: 실제 개수
    //   시야 규칙은 아래 IsAdjacent 와 같다(dx, dy ∈ [-1, 1]) — 하나를 바꾸면 다른 하나도 바꿀 것.
    void GetAroundSectorList(int32_t sectorX, int32_t sectorY,
                             SectorPos* outSectors, int32_t& outCount) const;

    // 두 섹터가 서로의 시야(주변 9섹터) 안인가 — 체비쇼프 거리 ≤ 1.
    //   GetAroundSectorList(SectorManager.cpp)가 dx,dy ∈ [-1,1]을 모으는 것과 같은 판정을
    //   섹터 목록을 만들지 않고 좌표만으로 계산한다(맵 밖 섹터는 애초에 주민이 없어 결과 동일).
    //   ※ 섹터 좌표는 존마다 별개 그리드이므로, 호출부에서 반드시 같은 존인지 먼저 확인할 것.
    static bool IsAdjacent(int32_t x1, int32_t y1, int32_t x2, int32_t y2)
    {
        int32_t dx = x1 - x2;
        int32_t dy = y1 - y2;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        return dx <= 1 && dy <= 1;
    }

private:
    // 평탄화 인덱스 접근 헬퍼 — 한 칸 = 플레이어 목록 + 맞을 수 있는 나머지 + 그 밖 (칸 번호 식은 CellIndex 한 곳)
    std::vector<CPlayer*>&          PlayersAt(int32_t y, int32_t x)            { return _players[CellIndex(x, y)]; }
    const std::vector<CPlayer*>&    PlayersAt(int32_t y, int32_t x) const      { return _players[CellIndex(x, y)]; }
    std::vector<CCharacter*>&       HittablesAt(int32_t y, int32_t x)          { return _hittables[CellIndex(x, y)]; }
    const std::vector<CCharacter*>& HittablesAt(int32_t y, int32_t x) const    { return _hittables[CellIndex(x, y)]; }
    std::vector<CCharacter*>&       NonHittablesAt(int32_t y, int32_t x)       { return _nonHittables[CellIndex(x, y)]; }
    const std::vector<CCharacter*>& NonHittablesAt(int32_t y, int32_t x) const { return _nonHittables[CellIndex(x, y)]; }

    int32_t _sectorSize;
    int32_t _sectorCountX;
    int32_t _sectorCountY;

    // 2D 그리드 (평탄화): PlayersAt/HittablesAt/NonHittablesAt(y, x)로 접근. 같은 칸 번호가 세 벡터의 같은 자리다
    std::vector<std::vector<CPlayer*>>    _players;
    std::vector<std::vector<CCharacter*>> _hittables;
    std::vector<std::vector<CCharacter*>> _nonHittables;

    // 범위 밖 요청 시 반환할 빈 목록
    static const std::vector<CPlayer*>    EMPTY_PLAYERS;
    static const std::vector<CCharacter*> EMPTY_CHARACTERS;
};
