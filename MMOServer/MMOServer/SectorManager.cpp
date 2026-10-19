#include "SectorManager.h"
#include "Player.h"   // As<CPlayer> — 종류로 목록을 고르는 자리(AddCharacter·RemoveCharacter)
#include <algorithm>
#include <cassert>   // 도달불가 불변식 검증 (릴리즈에서는 사라짐)

const std::vector<CPlayer*>    CSectorManager::EMPTY_PLAYERS;
const std::vector<CCharacter*> CSectorManager::EMPTY_OTHERS;

namespace
{
    // 한 목록에 넣기/빼기 — 플레이어 목록과 나머지 목록이 같은 꼴이라 한 벌만 둔다.
    template <typename T>
    void PushTo(std::vector<T*>& list, T* c)
    {
        c->_sectorIndex = static_cast<int32_t>(list.size());  // push 직전 크기 = 새 슬롯 인덱스
        list.push_back(c);
    }

    // 순서 보존 불필요 → 캐시 인덱스로 O(1) swap-and-pop (Zone::_listIndex와 동일 패턴)
    template <typename T>
    void PopFrom(std::vector<T*>& list, T* c)
    {
        int32_t idx = c->_sectorIndex;
        // [불변식] idx는 항상 자기 목록 안 자기 자리를 가리킨다 — PushTo가 push 직전 크기로 세팅하고,
        //   아래 swap-and-pop이 당겨온 원소의 인덱스까지 갱신한 뒤 -1로 되돌린다. 호출부 4곳
        //   (Zone::LeaveZone·Zone::Tick, CMoveHandler::RecvMoveStart·RecvMoveStop)은 모두 직전까지
        //   등록돼 있던 캐릭터를 그 등록 섹터 좌표로 넘기고, 중복 호출 경로도 없다
        //   (LeaveZone 호출은 RemoveFromZone 단 한 곳, 그 뒤엔 슬롯이 비거나 새 존에 재등록된다).
        //   어느 목록인지는 RemoveCharacter 가 AddCharacter 와 같은 기준(_kind)으로 고른다.
        assert(idx >= 0 && idx < static_cast<int32_t>(list.size()) && list[idx] == c);

        T* back = list.back();
        list[idx] = back;
        back->_sectorIndex = idx;   // 당겨온 원소의 인덱스 갱신 (LeaveZone의 back->_listIndex=idx 와 동일)
        list.pop_back();
        c->_sectorIndex = -1;
    }
}

CSectorManager::CSectorManager()
    : _sectorSize(0)
    , _sectorCountX(0)
    , _sectorCountY(0)
{
}

CSectorManager::~CSectorManager()
{
}

bool CSectorManager::Init(int32_t mapWidth, int32_t mapHeight, int32_t sectorSize)
{
    if (mapWidth < 2 || mapHeight < 2 || sectorSize <= 0)
        return false;

    _sectorSize = sectorSize;
    _sectorCountX = (mapWidth + sectorSize - 1) / sectorSize;
    _sectorCountY = (mapHeight + sectorSize - 1) / sectorSize;

    // 2D 그리드 할당 (평탄화) — 목록 둘이 같은 칸 번호를 쓴다
    _players.resize(_sectorCountY * _sectorCountX);
    _others.resize(_sectorCountY * _sectorCountX);

    return true;
}

int32_t CSectorManager::CalcSectorX(float worldX) const
{
    int32_t sx = static_cast<int32_t>(worldX) / _sectorSize;

    if (sx < 0) sx = 0;
    if (sx >= _sectorCountX) sx = _sectorCountX - 1;

    return sx;
}

int32_t CSectorManager::CalcSectorY(float worldY) const
{
    int32_t sy = static_cast<int32_t>(worldY) / _sectorSize;

    if (sy < 0) sy = 0;
    if (sy >= _sectorCountY) sy = _sectorCountY - 1;

    return sy;
}

void CSectorManager::AddCharacter(CCharacter* character, int32_t sectorX, int32_t sectorY)
{
    if (!IsValidSector(sectorX, sectorY))
        return;

    // 종류로 목록을 고른다 — 여기와 RemoveCharacter 둘뿐이다. _kind 는 생성자 뒤 안 바뀌므로 둘이 어긋날 길이 없다.
    if (CPlayer* p = As<CPlayer>(character))
        PushTo(PlayersAt(sectorY, sectorX), p);
    else
        PushTo(OthersAt(sectorY, sectorX), character);
}

void CSectorManager::RemoveCharacter(CCharacter* character, int32_t sectorX, int32_t sectorY)
{
    if (!IsValidSector(sectorX, sectorY))
        return;

    if (CPlayer* p = As<CPlayer>(character))
        PopFrom(PlayersAt(sectorY, sectorX), p);
    else
        PopFrom(OthersAt(sectorY, sectorX), character);
}

const std::vector<CPlayer*>& CSectorManager::GetSectorPlayers(int32_t sectorX, int32_t sectorY) const
{
    if (!IsValidSector(sectorX, sectorY))
        return EMPTY_PLAYERS;

    return PlayersAt(sectorY, sectorX);
}

const std::vector<CCharacter*>& CSectorManager::GetSectorOthers(int32_t sectorX, int32_t sectorY) const
{
    if (!IsValidSector(sectorX, sectorY))
        return EMPTY_OTHERS;

    return OthersAt(sectorY, sectorX);
}

void CSectorManager::GetAroundPlayers(int32_t sectorX, int32_t sectorY,
                                      std::vector<CPlayer*>& outPlayers,
                                      const CCharacter* exclude) const
{
    outPlayers.clear();

    SectorPos aroundSectors[MAX_AROUND_SECTORS];
    int32_t count = 0;
    GetAroundSectorList(sectorX, sectorY, aroundSectors, count);

    for (int32_t i = 0; i < count; ++i)
    {
        for (CPlayer* p : PlayersAt(aroundSectors[i].y, aroundSectors[i].x))
        {
            if (p != exclude)
                outPlayers.push_back(p);
        }
    }
}

void CSectorManager::GetSectorDiff(int32_t oldSectorX, int32_t oldSectorY,
                                   int32_t newSectorX, int32_t newSectorY,
                                   SectorPos* outAdded, int32_t& outAddedCount,
                                   SectorPos* outRemoved, int32_t& outRemovedCount) const
{
    outAddedCount = 0;
    outRemovedCount = 0;

    SectorPos oldList[MAX_AROUND_SECTORS];
    SectorPos newList[MAX_AROUND_SECTORS];
    int32_t oldCount = 0;
    int32_t newCount = 0;
    GetAroundSectorList(oldSectorX, oldSectorY, oldList, oldCount);
    GetAroundSectorList(newSectorX, newSectorY, newList, newCount);

    // newList에 있지만 oldList에 없는 것 → added
    for (int32_t i = 0; i < newCount; ++i)
    {
        bool found = false;
        for (int32_t j = 0; j < oldCount; ++j)
        {
            if (newList[i].x == oldList[j].x && newList[i].y == oldList[j].y)
            {
                found = true;
                break;
            }
        }
        if (!found)
            outAdded[outAddedCount++] = newList[i];
    }

    // oldList에 있지만 newList에 없는 것 → removed
    for (int32_t i = 0; i < oldCount; ++i)
    {
        bool found = false;
        for (int32_t j = 0; j < newCount; ++j)
        {
            if (oldList[i].x == newList[j].x && oldList[i].y == newList[j].y)
            {
                found = true;
                break;
            }
        }
        if (!found)
            outRemoved[outRemovedCount++] = oldList[i];
    }
}

bool CSectorManager::IsValidSector(int32_t sectorX, int32_t sectorY) const
{
    return sectorX >= 0 && sectorX < _sectorCountX
        && sectorY >= 0 && sectorY < _sectorCountY;
}

void CSectorManager::GetAroundSectorList(int32_t sectorX, int32_t sectorY,
                                         SectorPos* outSectors, int32_t& outCount) const
{
    outCount = 0;

    for (int32_t dy = -1; dy <= 1; ++dy)
    {
        for (int32_t dx = -1; dx <= 1; ++dx)
        {
            int32_t nx = sectorX + dx;
            int32_t ny = sectorY + dy;

            if (IsValidSector(nx, ny))
            {
                outSectors[outCount++] = { nx, ny };
            }
        }
    }
}
