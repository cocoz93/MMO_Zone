#pragma once

#include <cstdint>
#include <string>
#include <vector>

// ==========================================================================
// CMapTable — 맵 목록과 크기 (Run/bin/MapTable.csv)
//
// [어디 것이냐] 기획 데이터다. 「맵 0 은 120×120」은 세계의 모양이라 여기,
//        섹터 크기·채널 정원은 시야·부하를 조절하는 운영 값이라 서버 INI 다(ServerConfig::MapOpsOf).
//        둘을 맵 번호로 엮어 MapConfig 를 만드는 곳은 main 한 곳이다.
//        10-09 전에는 맵 개수·크기도 INI(MapCount·Width·Height)에 있었다.
//
// [토글 없음] 맵은 컨텐츠가 아니라 세계 자체라 전투·몬스터를 다 끈 빌드도 이 표로 맵을 세운다.
//
// [목록 = 줄] 한 줄이 맵 하나. 첫 줄이 기본 접속 맵이다(CGameServer::Init 의 _defaultMapId).
//        스폰 표가 이 번호로 맵을 가리키고, 읽을 때 이 표와 대조한다(CGameDataFiles::LoadAll 순서).
//
// [와이어 상한은 여기서 안 본다] 섹터 묶음이 좌표를 실어 나르는 눈금의 상한(POS_QUANT_MAX_MAP_SIZE)은
//        와이어 사정이라 패킷을 아는 게임 서버가 맵을 등록하기 전에 본다(CGameServer::Init).
// ==========================================================================
struct MapDef
{
    int32_t mapId    = 0;
    int32_t width    = 0;   // 칸
    int32_t height   = 0;   // 칸
    int32_t fileLine = 0;   // 스폰 표 대조에서 틀렸을 때 가리킬 줄
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
