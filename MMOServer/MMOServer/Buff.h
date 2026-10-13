// ==========================================================================
// Buff — 지속 효과(버프)의 칸 (USE_COMBAT)
//
// [어디 것이냐] 종류(BuffKind)는 와이어 enum 이다(Protocol.h — InvTab 처럼 서버가 그대로 쓴다). 여기는 캐릭터마다의 칸(BuffSlot)과
//        끝난 사유(BuffEnd — 지표 라벨)만 둔다. 규칙(걸기·만료·정리)은 CBuffSystem(BuffSystem.h), 스탯에 반영은 CStatSystem::RecalcStats.
//
// [정체 = 종류] 「어느 스킬이 걸었나」는 안 든다 — 스킬이 걸든 아이템이 걸든 같은 칸이고, 같은 종류는 나중 것이 값·시간을 덮는다
//        (이중공격 ↔ 삼중공격 — 「같은 무리」 규칙이 저절로 된다, 10-12). 그래서 벡터·할당이 없고 종류마다 칸 하나다.
//
// [토글] 전투(USE_COMBAT) 소유다 — 바꾸는 값이 전투 스탯이고 거는 쪽이 스킬만이 아니라서. 스킬을 꺼도 틀은 남고 거는 곳만 없다.
// ==========================================================================
#pragma once

#include <cstdint>

#include "BuildConfig.h"   // USE_COMBAT — 안 읽으면 아래 #if 가 조용히 0 이 된다

#if USE_COMBAT

#include <Protocol/Protocol.h>   // BuffKind — 와이어 값이자 칸 색인

constexpr int BUFF_KIND_COUNT = static_cast<int>(BuffKind::COUNT);

// 종류 하나의 칸 — CCharacter::_buffs[종류]. 사람 것은 DB 에 저장한다(characters.buffs, 10-13 — 그 전엔 접속을 끊으면 사라졌다).
struct BuffSlot
{
    int32_t  value       = 0;   // 종류마다 뜻이 다르다 — ATTACK_MUL 은 배수
    uint64_t expireFrame = 0;   // 이 프레임부터 풀린다(CGameServer::_frameCount 기준). 0 = 비어 있다

    bool IsActive() const { return expireFrame != 0; }
};

// 효과가 끝난 사유 — 빼는 자리가 셋이라 사유마다 한 칸(드랍의 GroundItemRemoval 과 같은 꼴).
//   등식(조용해진 뒤): Σ mmo_buff_applied_total = Σ mmo_buff_ended_total + mmo_buff_active
enum class BuffEnd : uint8_t
{
    EXPIRED,   // 시간이 다 됐다 — CBuffSystem::Tick
    DEATH,     // 죽어서 풀렸다 — CBuffSystem::ClearAll(알림 있음)
    LEAVE,     // 퇴장으로 잊었다 — CBuffSystem::Forget(알림 없음 — 곧 해제된다)
    COUNT
};

#endif // USE_COMBAT
