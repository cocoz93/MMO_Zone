#pragma once

#include <cstdint>

#include "Character.h"

struct NpcDef;   // NpcTable.h — NPC 표의 한 줄

// ==========================================================================
// CNpc — 마을 NPC (말 걸고 사고파는 사람 — 맞지도 때리지도 않는다)
//
// [경계] 컴포넌트가 없다(체력·공격·효과 0) — 섹터 「그 밖」 목록에 들어가 평타·스킬 후보에서 저절로 빠진다(Character.h [컨테이너]).
//        말 걸기 칸(CCharacter::Talk)에 자기 NPC 표 줄을 붙인다 — 사고팔기 판정은 타입(As<CNpc>)이 아니라 이 칸으로 찾는다
//        (싸우는 NPC 가 생기면 「체력 + 공격 + 말 걸기 칸」 — 상속을 안 늘린다).
//        이름·인사·상점 번호는 다 표에 있고 바뀌는 상태가 없다 — 그래서 이 클래스엔 칸이 표 줄 하나뿐이다.
//
// [수명] 존의 NPC 관리자(CNpcManager)가 unique_ptr 로 든다 — 존과 같이 태어나(첫 틱) 같이 사라진다. 움직이지 않는다(속도 0).
//
// [세션] 없다 — 보내는 함수가 CPlayer* 만 받으므로 NPC 에게 패킷을 보내는 코드는 컴파일이 안 된다(Character.h [세션]).
// ==========================================================================
class CNpc : public CCharacter
{
public:
    // 내 NPC 표 줄로 만든다 — 종류를 NPC 로 잡고 말 걸기 칸에 그 줄을 붙인다. 자리는 존 입장(EnterZoneAt)이 정한다.
    explicit CNpc(const NpcDef& def);
    ~CNpc() override;

    // As<CNpc> 판정 — Character.h As
    static bool IsA(const CCharacter& c) { return c._kind == CharacterKind::NPC; }

    // 무슨 NPC 냐 — NPC 표의 한 줄. 표는 기동 뒤 안 바뀌어 가리킨 줄이 안 흔들린다(몬스터의 _def 와 같은 꼴).
    const NpcDef& _def;
};
