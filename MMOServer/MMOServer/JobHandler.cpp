#include "JobHandler.h"

#include "SessionHub.h"
#include "SelfNotifier.h"
#include "StatSystem.h"       // USE_COMBAT
#include "Player.h"
#include "GameMonitor.h"
#include "GameDataFiles.h"
#include "PacketRoutes.h"
#include "SerialBuffer.h"
#if USE_DB_WORKER
#include "CharacterStore.h"
#endif
#include <Protocol/Protocol.h>
#include <Common/ErrorLog.h>

bool CJobHandler::RegisterRoutes(CPacketRoutes& routes)
{
    return routes.OnPlayer(MsgType::C2S_CHOOSE_JOB, sizeof(MSG_C2S_CHOOSE_JOB), [this](CPlayer* p, CSerialBuffer* m) { RecvChooseJob(p, m); });
}

// 답은 결과와 상관없이 언제나 지금 값 한 장 — 거절 패킷을 따로 두지 않는다(Protocol.h 「직업」)
void CJobHandler::RecvChooseJob(CPlayer* player, CSerialBuffer* pMsg)
{
    MSG_C2S_CHOOSE_JOB recvMsg{};
    pMsg->GetData(reinterpret_cast<char*>(&recvMsg), sizeof(recvMsg));

    const JobChoose result = Choose(player, recvMsg.jobId);
    _gameMonitor._job._choose[static_cast<int>(result)].Inc();
    _selfNotifier.SendMyJob(player);
}

JobChoose CJobHandler::Choose(CPlayer* player, int32_t jobId)
{
    // 한 번뿐 — 바꾸는 길은 없다(직업마다 스탯·기술이 달라 되돌리면 쿨타임 회피 같은 틈이 생긴다)
    if (player->_job != 0)
        return JobChoose::ALREADY;
    if (!_gameDataFiles.Jobs().Has(jobId))
        return JobChoose::BAD_JOB;
#if USE_COMBAT
    // 쓰러진 채 고르면 아래 가득 채우기가 죽은 채 체력을 채운다
    if (player->_health.IsDead())
        return JobChoose::DEAD;
#endif

    player->_job = jobId;
#if USE_SKILL
    // 고르기 전에 이미 든 것(job 칸이 빈 공용 기술)은 AddSkill 이 건너뛴다
    for (int32_t skillId : _gameDataFiles.Skill().PlayerSkillIdsOf(jobId))
        player->_attack.AddSkill(skillId);
#endif
#if USE_COMBAT
    // 가득 채운다 — 직업 줄의 최대치로 시작한다. 알림은 틱 끝 스탯 알림(CStatSync)이 보낸다
    _statSystem.RecalcStats(player, true);
#endif
#if USE_DB_WORKER
    // 고른 순간 저장 — 안 그러면 다음 주기 전에 서버가 죽었을 때 다시 고르는 창이 뜬다
    _characterStore.RequestSave(player, SaveReason::JOB);
#endif
    SLOG_INFO("[JOB] 고름 (character={}, job={})", player->_characterId, jobId);
    return JobChoose::OK;
}
