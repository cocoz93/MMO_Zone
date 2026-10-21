// ==========================================================================
// NpcManager — 존 하나의 NPC (토글 없음 — 맵·포털처럼 세계 데이터)
//
// [책임]
//  - NPC 객체 소유 (unique_ptr 목록) — 존과 같이 태어나고 같이 사라지므로 존이 이 관리자를 값으로 든다(몬스터 관리자와 형제)
//  - 깔기 — 존의 첫 틱에 그 맵의 NPC 표 줄마다 하나씩. 그 뒤로 할 일이 없다(리젠·판단·죽음이 없다)
//    채널이 새로 생겨도 그 존의 첫 틱이 깔아 준다 — 채널을 만드는 쪽이 NPC 를 몰라도 된다
//  - 송신·번호 발급은 하지 않는다 — 일이 생기면 INpcEvents 로 알리고, 받는 쪽(NPC 조각 CNpcHandler)이 한다
//
// [왜 첫 틱인가] 존을 만드는 곳(맵 관리자)에선 번호 발급기(세션 허브)를 모른다 — 몬스터가 첫 틱에 까는 것과 같은 까닭.
//
// [계층 경계]
//  CZone · CNpc · NPC 표를 안다. 세션·패킷·CGameServer 는 모른다. 기획 데이터는 존이 생길 때 받아 쥔다(몬스터 관리자와 같음).
// ==========================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

class CZone;
class CNpc;                // 전방 선언이면 된다 — unique_ptr 을 지우는 생성자·소멸자 몸체는 .cpp 에 둔다(Zone.h 와 같은 규칙)
class CGameDataFiles;

// NPC 쪽이 게임 서버에 부탁하는 것 — NPC 조각 CNpcHandler 가 구현한다(IMonsterEvents 와 같은 꼴).
//   ★ 순수 가상이라 하나를 빠뜨리면 컴파일이 안 된다.
class INpcEvents
{
public:
    virtual ~INpcEvents() = default;

    // 번호 하나 — 사건이 아니라 부탁이다. 번호 공간이 플레이어·몬스터와 하나라서(단일 증가 계수기) 여기서 못 뽑는다.
    virtual int32_t AllocActorId() = 0;

    // 등장 — 지금 자리 주변에 CREATE(SPAWN). 깔 때. 표시 문자·정의 번호(와이어 값)는 받는 쪽이 넣는다.
    virtual void OnNpcSpawn(CZone* zone, CNpc* npc) = 0;
};

class CNpcManager
{
public:
    // gameData — 기획 데이터. 존이 생길 때 받아 끝까지 쥔다([계층 경계]).
    CNpcManager(CZone& zone, const CGameDataFiles& gameData);
    ~CNpcManager();

    CNpcManager(const CNpcManager&) = delete;
    CNpcManager& operator=(const CNpcManager&) = delete;

    // 게임 루프 1-1b) — 아직 안 깔았으면 그 맵의 NPC 를 다 깐다. 그 뒤로는 곧바로 끝난다.
    //   생성자에서 안 하는 이유 — 그때는 맵 번호를 아직 모르고(CZone::Init 전) 번호 발급기도 없다.
    void Tick(INpcEvents& events);

    int32_t Count() const { return static_cast<int32_t>(_npcs.size()); }

    // 목록을 밖에 내주는 함수는 두지 않는다 — NPC 는 섹터 「그 밖」 목록에서 찾는다(상점 판정 — 주변 섹터).

private:
    CZone& _zone;                        // 나를 든 존 — 존이 나보다 오래 산다(멤버)
    const CGameDataFiles& _gameData;     // 기획 데이터 — 게임 서버가 들고 기동 뒤 안 바뀐다. 존보다 오래 산다

    // 소유 — 존이 사라질 때(멤버 소멸) 같이 사라진다. 원소는 존의 캐릭터 목록·섹터에도 들어 있다(같은 객체, 거기는 빌린 포인터).
    std::vector<std::unique_ptr<CNpc>> _npcs;
    bool _spawned = false;               // 첫 틱에 깔았나
};
