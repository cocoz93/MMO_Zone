#include "Npc.h"

#include "NpcTable.h"   // NpcDef — 내 표 줄

CNpc::CNpc(const NpcDef& def)
    : _def(def)
{
    // 자기 종류를 잡는다 — 파생이 정하는 게 규칙(Character.h [종류])
    _kind = CharacterKind::NPC;

    // 부품은 안 붙인다(맞지도 때리지도 않는다 — 섹터 「그 밖」 목록). 말 걸기 칸만 — 생성자 몸에서 한 번(Character.h 부품 자리와 같은 규칙)
    AttachTalk(&def);

    // 표시 문자(_displayChar)·정의 번호(_defId)는 여기서 안 넣는다 — 와이어 값이라 등장 알림을 받는 게임 서버의 NPC 조각이 넣는다
    //   (CNpcHandler::OnNpcSpawn — 몬스터와 같은 자리).
    _colorIndex = 0;

    // 서 있기만 한다 — 속도 0(CActor 가 0 으로 두고 파생이 정하는 규칙 그대로)
    _speed = 0;
}

CNpc::~CNpc()
{
}
