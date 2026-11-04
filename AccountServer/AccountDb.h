#pragma once
// ==========================================================================
// CAccountDb — 계정 표의 prepared 문장과 바인딩. 스레드와 큐는 CDbPipeline 이 맡는다
//
//   DB 워커 스레드에서 돈다. mysql.h 를 헤더에 안 넣는다(CDbConnection 과 같은 까닭).
// ==========================================================================

#include <string>

#include <Db/DbConnection.h>
#include <Identity/GameIds.h>

class CAccountDb
{
public:
    enum Op : int { OP_LOGIN = 0, OP_COUNT };   // mmo_db_query_seconds 의 op — 아래 이름과 같은 차례

    static const char* OpName(Op op);

    struct LoginRow
    {
        bool      ok = false;        // false = DB 실패(이름 잘못이 아니다)
        bool      created = false;   // 이번에 새로 만들었다
        AccountId accountId = NO_ACCOUNT;
    };

    // 있으면 번호를 읽고, 없으면 INSERT 로 발급한다. 끝에 last_login_at 을 갱신한다.
    //   같은 이름이 동시에 오면 유일 키가 한쪽을 막고(1062), 막힌 쪽은 다시 읽어 같은 번호를 받는다.
    //   워커가 달라도, 계정 서버가 둘이어도 답이 같다 — 워커 고르기에 기대지 않는다.
    static LoginRow FindOrCreate(CDbConnection& conn, const std::string& loginName);
};
