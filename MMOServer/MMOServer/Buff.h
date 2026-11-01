// ==========================================================================
// Buff — 지속 효과(버프)의 캐릭터 쪽 항목 (USE_COMBAT)
//
// [어디 것이냐] 「이중공격이 공격력을 몇 배로 몇 초 동안」은 전원 공통이라 버프 표(BuffTable.h), 「이 캐릭터에게 언제까지 걸려 있나」만 여기다(ActiveBuff).
//        끝난 사유(BuffEnd — 지표 라벨)도 여기 둔다. 규칙(걸기·만료·정리)은 CBuffSystem(BuffSystem.h), 스탯에 반영은 CStatSystem::BuildStats.
//
// [정체 = 버프 번호] 「어느 스킬이 걸었나」는 안 든다 — 스킬이 걸든 아이템이 걸든 번호가 같으면 같은 버프이고, 다시 걸면 시간만 되감긴다.
//        번호는 이름표다(배열 칸이 아니다 — 기술 목록과 같은 꼴). 걸린 것만 목록에 든다(CBuffComponent::_activeBuffs).
//
// [토글] 전투(USE_COMBAT) 소유다 — 바꾸는 값이 전투 스탯이고 거는 쪽이 스킬만이 아니라서. 스킬을 꺼도 틀은 남고 거는 곳만 없다.
// ==========================================================================
#pragma once

#include <cstdint>

#include "BuildConfig.h"   // USE_COMBAT — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_COMBAT

// 버프 번호 상한 — 와이어(u16)와 DB 묶음(2바이트)에 실리는 크기다. 배열 색인이 아니다(띄엄띄엄 매겨도 된다).
constexpr int32_t MAX_BUFF_ID = 0xFFFF;

// 버프 표 줄 수 상한 — 지표 칸 수(GameMonitor.h BuffCounters)이자 한 캐릭터에게 동시에 걸릴 수 있는 수다.
//   같은 번호는 목록에 하나뿐이고 표에 있는 번호만 걸리므로 목록 길이가 저절로 이 안이다.
//   DB 묶음 = 2 + 16 × 10 = 162바이트(DBWorker.h DB_BUFFS_BYTES · CharacterStore.cpp static_assert).
constexpr int32_t MAX_BUFF_ROWS = 16;

// 걸려 있는 버프 하나 — 효과 컴포넌트의 목록 CBuffComponent::_activeBuffs(CharacterComponents.h). 값은 표에 있다(버프 번호로 찾는다).
//   사람 것은 DB 에 저장한다(characters.buffs, 10-13 — 그 전엔 접속을 끊으면 사라졌다).
struct ActiveBuff
{
    int32_t  buffId      = 0;   // 버프 표 번호
    uint64_t expireFrame = 0;   // 이 프레임부터 풀린다(CGameServer::_frameCount 기준)
};

// 효과가 끝난 사유 — 빼는 자리가 셋이라 사유마다 한 칸(드랍의 GroundItemRemoval 과 같은 꼴).
//   등식(조용해진 뒤): Σ mmo_buff_applied_total + Σ mmo_buff_restored_total = Σ mmo_buff_ended_total + mmo_buff_active
enum class BuffEnd : uint8_t
{
    EXPIRED,   // 시간이 다 됐다 — CBuffSystem::Tick
    DEATH,     // 죽어서 풀렸다 — CBuffSystem::ClearAll(알림 있음)
    LEAVE,     // 퇴장으로 잊었다 — CBuffSystem::Forget(알림 없음 — 곧 해제된다)
    COUNT
};

#endif // USE_COMBAT
