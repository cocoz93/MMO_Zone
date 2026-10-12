#pragma once

#include <cstdint>
#include <string>
#include <vector>

// ==========================================================================
// CMapTable — 맵 목록과 크기 (Run/bin/MapTable.csv)
//
// [어디 것이냐] 기획 데이터다. 「던전(맵 1)은 120×120」은 세계의 모양이라 여기,
//        섹터 크기·채널 정원은 시야·부하를 조절하는 운영 값이라 서버 INI 다(ServerConfig::MapOpsOf).
//        둘을 맵 번호로 엮어 MapConfig 를 만드는 곳은 main 한 곳이다.
//        10-09 전에는 맵 개수·크기도 INI(MapCount·Width·Height)에 있었다.
//
// [토글 없음] 맵은 컨텐츠가 아니라 세계 자체라 전투·몬스터를 다 끈 빌드도 이 표로 맵을 세운다.
//
// [목록 = 줄] 한 줄이 맵 하나. 첫 줄이 기본 접속 맵이다(CGameServer::Init 의 _defaultMapId) — 마을(town)이어야 한다.
//        새 캐릭터가 들어가는 곳이고 죽으면 돌아오는 곳이다(CGameServer::TickRespawn).
//        스폰 표가 이 번호로 맵을 가리키고, 읽을 때 이 표와 대조한다(CGameDataFiles::LoadAll 순서).
//
// [종류·시작 구역] 칸 = mapId,width,height,kind,startX0,startY0,startX1,startY1.
//        kind 는 마을(town — 몬스터 구역 0)·던전(dungeon — 구역 ≥1). 구역 개수는 스폰 표가 읽을 때 대조한다.
//        시작 구역은 「지정 자리 없이 이 맵에 들어올 때」의 자리다(CZone::CalcSpawnPos) — 마을은 반드시, 던전은 비우면 맵 전체 무작위.
//
// [와이어 상한은 여기서 안 본다] 섹터 묶음이 좌표를 실어 나르는 눈금의 상한(POS_QUANT_MAX_MAP_SIZE)은
//        와이어 사정이라 패킷을 아는 게임 서버가 맵을 등록하기 전에 본다(CGameServer::Init).
// ==========================================================================
// 맵 종류 — 표의 글자는 MapTable.cpp 의 이름표(KIND_NAMES)다. 갈래가 늘면 둘을 같이 넓힌다(개수 static_assert).
enum class MapKind : uint8_t
{
    TOWN,      // "town"    — 몬스터 구역 0 · 시작 구역 필수
    DUNGEON,   // "dungeon" — 몬스터 구역 ≥1 · 시작 구역은 있어도 되고 없어도 된다
    COUNT
};

// 시작 구역 — 지정 자리 없이 이 맵에 들어올 때(새 캐릭터 · 저장 자리 못 씀 · 다른 맵으로 옮겨 옴) 이 사각형 안 무작위.
//   좌표(타일 단위 실수)로 읽고 양끝을 포함한다. has == false 면 맵 전체 무작위(CZone::CalcSpawnPos).
struct StartArea
{
    bool    has = false;
    int32_t x0  = 0;
    int32_t y0  = 0;
    int32_t x1  = 0;
    int32_t y1  = 0;
};

struct MapDef
{
    int32_t   mapId    = 0;
    int32_t   width    = 0;   // 칸
    int32_t   height   = 0;   // 칸
    MapKind   kind     = MapKind::DUNGEON;
    StartArea start;
    int32_t   fileLine = 0;   // 스폰 표 대조에서 틀렸을 때 가리킬 줄
};

class CMapTable
{
public:
    static constexpr int32_t MAX_MAP_ID = 1000;     // 스폰 표가 맵 번호를 색인으로 쓴다 — 오타 하나가 배열을 키우지 않게
    static constexpr int32_t MAX_SIZE   = 100000;   // 칸 수 상한 — 읽은 값을 int32 로 옮겨도 넘치지 않게(실제 상한은 와이어, 위 주석)

    // 파일을 읽어 표를 세운다. 실패하면 false 이고 까닭을 로그로 남긴다 — main 이 서버를 안 띄운다.
    bool Load(const std::string& path);

    // 없는 번호면 nullptr — 스폰 표가 맵 번호를 대조하는 길이다.
    const MapDef* Find(int32_t mapId) const;

    // 파일 순서 그대로 — 첫 줄이 기본 접속 맵
    const std::vector<MapDef>& All() const { return _rows; }

private:
    std::vector<MapDef> _rows;   // 파일 순서. 맵이 몇 개뿐이라 찾기는 앞에서부터 훑는다
};
