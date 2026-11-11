#pragma once
// ==========================================================================
// CCharacterDb — 소유 표의 prepared 문장 · 트랜잭션 · 바인딩. 스레드와 큐는 CDbPipeline 이 맡는다
//
//   DB 워커 스레드에서 돈다. mysql.h 를 헤더에 안 넣는다(CDbConnection 과 같은 까닭).
//   상한 · 이름 판정은 DB 가 한다 — 유일 키라서 같은 자리 · 같은 이름에 둘이 못 들어간다.
//   유예 계산의 시계는 DB 하나다(CURRENT_TIMESTAMP). 주기 작업을 두지 않는다 — 유예 지난 자리는
//   그 계정이 로그인하거나 캐릭터를 만들 때 풀린다.
// ==========================================================================

#include <cstdint>
#include <vector>

#include <Db/DbConnection.h>
#include <Identity/GameIds.h>
#include <Protocol/CharacterProtocol.h>

class CCharacterDb
{
public:
    enum Op : int { OP_LIST = 0, OP_CREATE, OP_DELETE, OP_RESTORE, OP_SELECT, OP_COUNT };   // mmo_db_query_seconds 의 op

    static const char* OpName(Op op);

    struct ListResult
    {
        bool ok = false;   // false = DB 실패(「캐릭터 없음」이 아니다)
        std::vector<CharListEntry> rows;
    };

    struct CreateRequest
    {
        AccountId account = NO_ACCOUNT;
        int       slot = 0;
        char16_t  name[CHAR_NAME_MAX_LEN]{};
        int       job = 0;
        uint8_t   look[CHAR_LOOK_BYTES]{};
    };

    struct CreateOutcome
    {
        CharCreateResult result = CharCreateResult::DB_FAIL;
        CharacterId      characterId = NO_CHARACTER;
        ListResult       list;   // OK 일 때만 읽는다
    };

    struct DeleteOutcome
    {
        CharDeleteResult result = CharDeleteResult::DB_FAIL;
        ListResult       list;
    };

    struct RestoreOutcome
    {
        CharRestoreResult result = CharRestoreResult::DB_FAIL;
        ListResult        list;
    };

    struct SelectChecked
    {
        CharSelectResult result = CharSelectResult::DB_FAIL;
        ZoneServerId     ownerZone{ 0 };   // characters.owner_zone — 행이 없거나 0 이면 새 캐릭터
    };

    // 유예 지난 자리를 풀고 목록을 읽는다 — 유예 지난 행은 목록에 안 나온다
    static ListResult LoadList(CDbConnection& conn, AccountId account, int graceDays);

    // 소유 표와 characters 를 한 트랜잭션으로 넣는다(job · map_id = -1 「아직 입장 안 함」)
    //   1062 인데 같은 계정 · 자리 · 이름 행이 있으면 앞선 회신이 유실된 것이다 → OK
    static CreateOutcome Create(CDbConnection& conn, const CreateRequest& request, int graceDays);

    // inGame = 요청 받을 때의 접속 판정. 소유 확인이 먼저다 — 남의 캐릭터가 접속 중인지를 알려 주지 않는다
    static DeleteOutcome Delete(CDbConnection& conn, AccountId account, CharacterId character, bool inGame, int graceDays);

    static RestoreOutcome Restore(CDbConnection& conn, AccountId account, CharacterId character, int graceDays);

    // 소유 · 삭제 확인 + 주인 존 서버 읽기. OK 이면 입장 번호로 넘어간다. ZONE_* 는 여기서 안 나온다
    static SelectChecked CheckSelect(CDbConnection& conn, AccountId account, CharacterId character);
};
