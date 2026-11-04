#pragma once
// ==========================================================================
// CDbConnection — MySQL 연결 열기 · 시한 설정 · 연결 유실 판정 (세 서버가 같은 코드를 쓴다)
//
//   존의 CDBWorker 는 Open·IsConnectionLostErrno 정적 함수만 쓴다(자기 prepared 문장을 직접 관리한다).
//   로비 서버는 객체를 워커마다 하나 들고, 문장 캐시와 실패 표시까지 쓴다(CDbPipeline).
//   mysql.h 를 헤더에 안 넣는 까닭 — 부품 헤더 전부가 MySQL 헤더에 묶이지 않게(CDBWorker 와 같은 void*).
// ==========================================================================

#include <string>
#include <unordered_map>

// DB 접속 설정 — ServerConfig가 INI [DB] 섹션에서 채워 넘긴다.
// (USE_DB_WORKER 토글과 무관하게 정의 — GameServer::InitDB가 값으로 참조)
//   ★ 기본값을 여기 두지 않는다 — 정본은 ServerConfig 의 db* 멤버이고 main 이 일곱 칸을 전부 채운다(10-08 2차 검사 5번).
//     여기 숫자를 두면 아무도 안 읽는 셋째 벌이 된다. 0·빈 문자열 = 아직 안 채워짐.
struct DBConfig
{
    std::string host;
    int         port              = 0;
    std::string user;
    std::string password;
    std::string database;
    int         connectTimeoutSec = 0;
    int         rwTimeoutSec      = 0;   // 읽기/쓰기 소켓 타임아웃(초) — DB 무응답 시 쿼리 무한대기 방지
};

class CDbConnection
{
public:
    // 새 연결(MYSQL*). 실패하면 nullptr — 까닭은 로그로 남긴다
    static void* OpenRaw(const DBConfig& config);

    // 오류 번호가 「연결이 끊겼다」인가 — 다시 연결해 볼 값어치가 있는 오류다
    static bool IsConnectionLostErrno(unsigned int err);

    explicit CDbConnection(const DBConfig& config) : _config(config) {}
    ~CDbConnection() { Close(); }

    CDbConnection(const CDbConnection&) = delete;
    CDbConnection& operator=(const CDbConnection&) = delete;

    bool Open();
    void Close();
    bool IsOpen() const { return _mysql != nullptr; }

    void* Handle() const { return _mysql; }   // MYSQL*

    // 문장 하나를 준비해 돌려준다(MYSQL_STMT*). 같은 글자면 재사용하고, 연결이 바뀌면 새로 만든다.
    //   실패하면 nullptr — 실패 표시와 유실 판정은 이미 끝난 상태다
    void* Prepare(const char* sql);

    // 트랜잭션 머리·꼬리처럼 결과 없는 문장. 실패하면 false — 실패 표시까지 끝낸다
    bool Exec(const char* sql);

    // 문장이 실패했다 — 로그 · 실패 표시 · 유실이면 연결을 닫는다(다음 작업이 다시 연다).
    //   errno 를 돌려준다(부르는 쪽이 1062 같은 값을 가른다). stmt 는 이 뒤에 닫혔을 수 있으니 다시 쓰지 말 것
    unsigned int NoteStmtError(void* stmt, const char* what);

    // 연결이 없어 작업을 못 했다 — 실패 표시만
    void NoteNotOpen() { _jobFailed = true; }

    // 이번 작업에서 실패 표시가 있었나(읽으면 지운다) — CDbPipeline 이 mmo_db_failed_total 로 센다
    bool TakeJobFailed()
    {
        const bool failed = _jobFailed;
        _jobFailed = false;
        return failed;
    }

private:
    void CloseStatements();

    DBConfig _config;
    void*    _mysql = nullptr;
    bool     _jobFailed = false;
    std::unordered_map<std::string, void*> _statements;   // 글자 → MYSQL_STMT*. 연결과 수명이 같다
};
