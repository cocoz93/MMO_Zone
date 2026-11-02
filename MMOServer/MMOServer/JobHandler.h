// ==========================================================================
// CJobHandler — 직업 고르기(C2S_CHOOSE_JOB)
//
// [책임]
//  받은 패킷 등록 · 고르기 판정(한 번뿐 · 표에 있는 번호 · 살아 있을 때) · 기술 목록과 스탯을 그 직업으로 세우기 · 고른 순간 저장 요청 · 답(S2C_MY_JOB).
//
// [경계]
//  입장 때 직업을 세우는 것은 CEntryHandler::SpawnPlayer, 입장 때 보내는 것은 CPlayerPlacement::NotifyArrival 이다. 여기는 고르는 순간만.
//  직업이 가르는 값은 표에 있다(레벨 표 · 스킬 표의 job 칸) — 이 파일에 직업 번호를 적지 않는다.
// ==========================================================================
#pragma once

#include <cstdint>

#include "BuildConfig.h"

class CPlayer;
class CSerialBuffer;
class CPacketRoutes;
class CSelfNotifier;
class CGameMonitor;
class CGameDataFiles;
class CStatSystem;
class CCharacterStore;
enum class JobChoose : uint8_t;

class CJobHandler
{
public:
    CJobHandler(CSelfNotifier& selfNotifier,
#if USE_COMBAT
                CStatSystem& statSystem,
#endif
#if USE_DB_WORKER
                CCharacterStore& characterStore,
#endif
                CGameMonitor& gameMonitor, const CGameDataFiles& gameDataFiles)
        : _selfNotifier(selfNotifier)
#if USE_COMBAT
        , _statSystem(statSystem)
#endif
#if USE_DB_WORKER
        , _characterStore(characterStore)
#endif
        , _gameMonitor(gameMonitor), _gameDataFiles(gameDataFiles) {}

    bool RegisterRoutes(CPacketRoutes& routes);

private:
    void RecvChooseJob(CPlayer* player, CSerialBuffer* pMsg);
    JobChoose Choose(CPlayer* player, int32_t jobId);

    CSelfNotifier& _selfNotifier;
#if USE_COMBAT
    CStatSystem& _statSystem;
#endif
#if USE_DB_WORKER
    CCharacterStore& _characterStore;
#endif
    CGameMonitor& _gameMonitor;
    const CGameDataFiles& _gameDataFiles;
};
