#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "BuildConfig.h"   // USE_COMBAT — 안 읽으면 아래 #if 가 조용히 0 이 된다
#include "Buff.h"          // MAX_BUFF_ID · MAX_BUFF_ROWS

#if USE_COMBAT

// 버프가 바꾸는 스탯 — 코드로 늘어나는 축 하나. 새 종류 = 여기 한 줄 + 표 이름 한 줄(BuffTable.cpp STAT_NAMES) + CStatSystem::BuildStats 한 줄.
enum class BuffStat : uint8_t
{
    ATTACK,    // 공격력
    DEFENSE,   // 방어력
    MAX_HP,    // 최대 체력 — 지금 체력은 안 올린다(넘은 몫만 깎는다)
    MAX_MP,    // 최대 마력 — 지금 마력은 안 올린다(넘은 몫만 깎는다)
    MOVE_SPEED,// 이동 속도 — 바뀌면 주변에 ACTOR_SPEED(CBuffSystem → IBuffEvents::OnSpeedChanged). 최소 1
    ROOT,      // 묶임 — add 1 만 쓴다. 걸린 동안 안 움직인다. 지금은 몬스터만 막는다(이동 결정 CMonsterManager::Think) — 사람에게 걸면 아무 일도 안 난다
    COUNT
};

// 바꾸는 방식 — 둘째 축. 적용 순서는 고정이다: 바탕 + 장비 → add 합 → pct 합 → mul 곱(CStatSystem::BuildStats).
enum class BuffModKind : uint8_t
{
    ADD,   // 더하기 — 값만큼 (0 이 아닌 정수)
    PCT,   // 퍼센트 — 값 % 만큼 늘림 (30 = 1.3배, 내림)
    MUL,   // 곱하기 — 값 배 (2 = 2배)
    COUNT
};

struct BuffMod
{
    BuffStat    stat  = BuffStat::COUNT;
    BuffModKind kind  = BuffModKind::COUNT;
    int32_t     value = 0;
};

constexpr int32_t MAX_BUFF_MODS = 4;   // 한 버프가 바꾸는 항목 수 상한

// ==========================================================================
// CBuffTable — 버프 정의 (Run/bin/BuffTable.csv)
//
// [어디 것이냐] 기획 데이터다. 「이중공격은 공격력을 2배로 30초」는 전원 공통이라 여기,
//        「이 캐릭터에게 언제까지 걸려 있나」는 캐릭터 쪽(효과 컴포넌트의 목록 CBuffComponent::_activeBuffs)이다.
//
// [색인] 번호는 색인이 아니다 — 줄은 파일 순서로 들고, 번호로 찾을 땐 번호 → 줄 표(_rowById)를 본다.
//        줄마다 안쪽 순번(index, 0~)을 매긴다 — 지표 칸이 이 순번이다(스킬 표와 같은 꼴).
//        번호는 와이어(S2C_ACTOR_BUFF u16)와 DB(characters.buffs)에도 실린다 — 웹이 같은 파일에서 값을 읽는다.
//
// [버프마다 클래스 없음] 버프마다 다른 것은 지속과 수정 항목뿐이라 표 한 줄이다.
//        코드로 늘어나는 것은 스탯 종류(BuffStat)와 방식(BuffModKind) 두 축이다.
//
// [거는 쪽을 모른다] 스킬 표가 이 표를 먼저 읽고 번호를 대조한다(LoadAll 순서). 아이템이 걸게 되어도 같은 줄을 쓴다.
// ==========================================================================
struct BuffDef
{
    int32_t     buffId         = 0;   // 1~MAX_BUFF_ID — 표 읽기가 겹침을 막는다
    int32_t     index          = 0;   // 안쪽 순번 0~(줄 수 − 1) — 파일 순서. 지표 칸(GameMonitor.h BuffCounters)이 이 값이다
    std::string name;                 // UTF-8. 서버는 로그에만 쓴다
    int32_t     durationMs     = 0;   // 파일에 적힌 값 그대로 — 로그용
    uint64_t    durationFrames = 0;   // 지속(틱) — 올림(쿨타임과 같은 까닭 — 짧아지지 않게). 30000 → 750
    BuffMod     mods[MAX_BUFF_MODS];
    int32_t     modCount       = 0;   // 1~MAX_BUFF_MODS
};

class CBuffTable
{
public:
    static constexpr int32_t MAX_DURATION_MS = 3600000;   // 1시간 — 그 위는 오타로 본다(스킬 쿨타임 상한과 같은 값)
    static constexpr int32_t MAX_ADD         = 1000000;   // |값| 상한 — 오타 하나가 값을 터뜨리지 않게
    static constexpr int32_t MAX_PCT         = 1000;      // % 상한
    static constexpr int32_t MAX_MUL         = 10;        // 배수 상한 — 1 은 아무 일도 안 한다(표 읽기가 막는다). 원본 오중공격이 5

    // 파일을 읽어 표를 세운다. 실패하면 false 이고 까닭을 로그로 남긴다 — main 이 서버를 안 띄운다.
    bool Load(const std::string& path);

    // 없는 번호면 nullptr — DB 에서 온 번호(패치로 지웠을 수 있다)를 그대로 넣어도 된다.
    const BuffDef* Find(int32_t buffId) const
    {
        const auto it = _rowById.find(buffId);
        return (it == _rowById.end()) ? nullptr : &_rows[it->second];
    }

    int32_t Count() const { return static_cast<int32_t>(_rows.size()); }

    // 안쪽 순번으로 — 0~Count()−1. 지표 이름표(버프 번호)를 적어 둘 때 훑는다(CGameServer::LoadGameDataFiles)
    const BuffDef& Row(int32_t index) const { return _rows[static_cast<size_t>(index)]; }

private:
    std::vector<BuffDef> _rows;                      // 파일 순서 — 색인 = BuffDef::index. 읽은 뒤로 안 늘어 Find 의 포인터가 안 흔들린다
    std::unordered_map<int32_t, size_t> _rowById;    // 버프 번호 → _rows 색인
};

#endif // USE_COMBAT
