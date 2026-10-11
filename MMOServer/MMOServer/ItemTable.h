#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "BuildConfig.h"
#include <Protocol/Protocol.h>   // InvTab — 아이템이 어느 탭에 들어가나

#if USE_INVENTORY

// ==========================================================================
// CItemTable — 아이템 정의 (이름 · 탭 · 쌓임 한도 · 회복량(체력·마력) · 착용 공격력)
//
// [어디 것이냐] 기획 데이터다(Run/bin/ItemTable.csv). 가방 안의 "무엇이 몇 개"만 유저 데이터다.
//        웹 클라도 같은 파일을 직접 읽어 이름·그림을 그린다 — 두 벌로 적으면 어긋난다.
//        판정(쌓임 한도·회복량)은 서버만 한다.
//
// [쌓임 한도] 종류로 가르지 않는다 — 규칙은 "같은 아이템이면 maxStack 까지 합친다" 한 줄이고,
//        장비는 maxStack 이 1 이라 저절로 안 쌓인다. 그래서 로드가 "장비인데 1 이 아님"을 막는다.
//
// [색인] 번호가 곧 색인이다. 번호는 성기게 써도 된다(1001·2001 식) — 구멍은 itemId 0 인 빈 행.
//        번호 상한을 두는 이유는 오타 하나(100000)가 배열을 통째로 키우지 않게.
// ==========================================================================
struct ItemDef
{
    int32_t     itemId   = 0;              // 0 = 이 번호의 아이템은 없다(배열 구멍)
    std::string name;                      // UTF-8. 서버는 로그·명령 답에만 쓴다
    InvTab      tab      = InvTab::COUNT;
    int32_t     maxStack = 0;              // 한 칸에 몇 개까지
    int32_t     healHp   = 0;              // 소비 아이템을 쓰면 채우는 체력 (그 밖은 0)
    int32_t     healMp   = 0;              // 소비 아이템을 쓰면 채우는 마력 (그 밖은 0) — 한 물약 한 효과(healHp 와 둘 다 1 이상이면 로드가 막는다)
    int32_t     attack   = 0;              // 착용하면 더하는 공격력 — 장비만 (그 밖은 0, 로드가 막는다)

    // 효과가 있나 — 표 읽기가 묻는다(소비 아이템인데 효과 칸이 다 0이면 거부). 빌드와 무관하다 — 한 표를 모든 빌드가 읽는다.
    bool HasEffect() const { return healHp > 0 || healMp > 0; }

    // 이 빌드에서 쓸 수 있나 — 사용 판정(CItemSystem::CheckUseItem)이 묻는다. 이 물음의 답은 여기 하나다(10-08 2차 검사 8번).
    //   마력 회복은 스킬(USE_SKILL)을 켠 빌드에서만 친다 — 마력이 없는 빌드에서 마력 물약은 NOT_USABLE 이다(표는 받아 준다).
    //   예전엔 표 읽기와 사용 판정이 같은 조건을 따로 적어, 한쪽만 풀면 「표는 받아 주는데 못 쓰는 아이템」이 생길 수 있었다.
    //   그래서 한 함수로 모았는데, 마력이 생기며 두 물음의 답이 빌드마다 달라졌다(10-11) — 하나로 두면 스킬을 끈 빌드가
    //   마력 물약 줄에서 표 읽기를 거부해 서버가 안 뜨거나, 그걸 피하려고 사용 판정에 조건을 따로 적게 된다.
    //   그래서 물음마다 한 곳(위 HasEffect · 이 함수)에서 답한다. 효과(버프 등)가 늘면 두 함수를 같이 넓힌다.
    bool IsUsable() const
    {
#if USE_SKILL
        return tab == InvTab::USE && (healHp > 0 || healMp > 0);
#else
        return tab == InvTab::USE && healHp > 0;
#endif
    }
};

class CItemTable
{
public:
    static constexpr int32_t MAX_ITEM_ID = 10000;

    // 파일을 읽어 표를 세운다. 실패하면 false 이고 까닭을 로그로 남긴다 — main 이 서버를 안 띄운다.
    bool Load(const std::string& path);

    // 없는 번호면 nullptr — 명령·불러오기가 "없는 아이템"을 가르는 유일한 길이다.
    const ItemDef* Find(int32_t itemId) const
    {
        if (itemId <= 0 || itemId >= static_cast<int32_t>(_rows.size()))
            return nullptr;
        const ItemDef& d = _rows[static_cast<size_t>(itemId)];
        return d.itemId == 0 ? nullptr : &d;
    }

    int32_t Count() const { return _count; }

private:
    std::vector<ItemDef> _rows;   // 색인 = 아이템 번호. 구멍은 itemId 0
    int32_t _count = 0;           // 실제로 정의된 아이템 수
};

// 탭의 파일 속 이름(equip·use·etc)과 사람이 읽는 이름(장비·소비·기타) — InvTab 순서.
//   글자 → enum 은 이 표를 돈다(탭마다 if 를 쓰지 않는다).
const char* InvTabFileName(InvTab tab);
const char* InvTabDisplayName(InvTab tab);

#endif // USE_INVENTORY
