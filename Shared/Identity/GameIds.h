#pragma once
//==================================================
// 번호 전용 타입 — 계정 · 캐릭터 · 액터 · 세션 · 구역 · 맵 · 존 서버
//   전부 정수라서 다른 번호를 그대로 넘겨도 컴파일이 됐다(액터 번호가 저장 열쇠로 들어가던 자리가 그 예).
//   번호마다 타입을 나눠 섞으면 빌드가 멈추게 한다. 실행 때 드는 비용은 없다 — 값은 정수 그대로다.
//
//   경계 = 패킷 구조체와 DB 바인딩 칸은 정수로 둔다(와이어 바이트 불변 · 콘솔·부하 더미 공유).
//          코어도 정수다(게임과 무관한 층). 받는 함수에서 한 번 이 타입으로 바꾸고, 안쪽은 이 타입만 쓴다.
//   변환 = 번호에서 다른 번호로 가는 길은 이름 있는 함수에만 둔다(구역 → 맵은 CMapManager::GetMapIdFromZoneId).
//==================================================
#include <cstdint>
#include <type_traits>

enum class AccountId    : int64_t {};   // 계정 서버가 발급 · 영구
enum class CharacterId  : int64_t {};   // 캐릭터 서버가 발급 · 영구 · 저장 열쇠
enum class ActorId      : int32_t {};   // 존 서버가 발급 · 접속 동안 · 패킷에 싣는 번호
enum class SessionId    : int64_t {};   // 접속 연결 번호(코어가 발급)
enum class ZoneId       : int32_t {};   // 프로세스 안의 구역 번호(맵 번호 × 채널 상한 + 채널)
enum class MapId        : int32_t {};   // 맵 표의 번호
enum class ZoneServerId : int32_t {};   // 존 서버 프로세스 번호(등록·입장 대상)

// 「없음」 값 — 전용 타입으로 옮기기 전 코드가 쓰던 값에 이름만 붙였다
//   액터 0 = 패킷의 「관련 사람 없음」(번호는 1부터 나간다) · 캐릭터 0 = 아직 안 정해짐 · 땅 아이템 주인 없음
inline constexpr SessionId   NO_SESSION{ -1 };
inline constexpr CharacterId NO_CHARACTER{ 0 };
inline constexpr ActorId     NO_ACTOR{ 0 };
inline constexpr ZoneId      NO_ZONE{ -1 };
inline constexpr MapId       NO_MAP{ -1 };

// 정수로 — 패킷 · DB 바인딩 · 코어 호출 · 표 첨자처럼 정수가 필요한 자리에서만 쓴다
template <typename Id>
constexpr std::underlying_type_t<Id> ToInt(Id id)
{
    static_assert(std::is_enum_v<Id>, "번호 전용 타입만 넘긴다");
    return static_cast<std::underlying_type_t<Id>>(id);
}

// 로그 — fmt 가 인자 의존 조회로 찾아 정수로 찍는다(SLOG_* 의 {} 가 바꾸기 전과 같은 글자를 낸다)
constexpr int64_t format_as(AccountId id)    { return ToInt(id); }
constexpr int64_t format_as(CharacterId id)  { return ToInt(id); }
constexpr int32_t format_as(ActorId id)      { return ToInt(id); }
constexpr int64_t format_as(SessionId id)    { return ToInt(id); }
constexpr int32_t format_as(ZoneId id)       { return ToInt(id); }
constexpr int32_t format_as(MapId id)        { return ToInt(id); }
constexpr int32_t format_as(ZoneServerId id) { return ToInt(id); }

// 섞이지 않는다는 약속을 컴파일 시점에 못 박는다 — 정수와 서로 저절로 바뀌지 않고, 번호끼리도 안 바뀐다
static_assert(!std::is_convertible_v<ActorId, int32_t> && !std::is_convertible_v<int32_t, ActorId>);
static_assert(!std::is_convertible_v<CharacterId, int64_t> && !std::is_convertible_v<int64_t, CharacterId>);
static_assert(!std::is_convertible_v<ActorId, CharacterId> && !std::is_convertible_v<CharacterId, ActorId>);
static_assert(!std::is_convertible_v<SessionId, CharacterId> && !std::is_convertible_v<AccountId, CharacterId>);
static_assert(!std::is_convertible_v<ZoneId, MapId> && !std::is_convertible_v<ZoneId, ZoneServerId>);
