'use strict';
// 마을(맵 0) 배치 원본 — build.js 가 읽어 assets/town.js 를 만든다. 고친 뒤 `node tools/town/build.js`.
//   좌표 = 월드 칸(서버와 같은 단위). 마을 60×60 = 땅 40×40칸(한 칸 1.5) · 시작 구역 (27~33) · 포털 1 (55,30) — 서버 표(Run/bin/MapTable.csv · PortalTable.csv)와 맞춘 것
//   땅: 바탕 하나 + 층(뒤 층이 위). 층마다 지형 이름(terrain-v7.tsx 의 <terrain name>)과 모양 목록 — 모서리 하나라도 모양 안이면 그 칸에 경계 타일이 깔린다
//     모양 = { ell:[중심x, 중심y, 반지름x, 반지름y] } · { rect:[x0, y0, x1, y1] } · { seg:[x0, y0, x1, y1, 반지름] }(굵은 선분)
//   물건: [그림 이름, x, 발밑 y] — 그림 이름은 build.js 의 PREVIEW.regions(집 다섯) · cottage_small · PROPS · 깊이 정렬은 발밑 y 로
//   나무: [종류(pine·bush — 웹 기존 나무 그림), x, y, 변형 0·1]
module.exports = {
  mapId: 0,
  cells: 40,
  ground: {
    base: 'Dirt_Brown',
    layers: [
      { terrain: 'Dirt_Tan', shapes: [{ ell: [20, 42, 5, 3] }, { ell: [44, 9, 4, 2.5] }, { ell: [10, 25, 3.5, 2.5] }] },          // 밝은 흙 얼룩
      { terrain: 'Grass', shapes: [{ ell: [58.5, 58, 4.5, 4] }, { ell: [53, 54, 7.5, 5.5] }] },                                   // 풀 = 연못가 · 남동 귀퉁이만
      { terrain: 'Soil', shapes: [{ rect: [2.5, 38.5, 10.5, 44.5] }] },                                                           // 텃밭
      { terrain: 'Stone_Tan', shapes: [                                                                                           // 돌바닥 광장 + 큰길
        { ell: [30, 30, 9.5, 7] },
        { rect: [30, 28, 58.5, 32] },     // 동쪽 큰길 — 포털 (55,30)
        { rect: [28, -1, 32, 30] },       // 북쪽 큰길 — 대문 아래
        { rect: [12, 28, 30, 31.5] },     // 서쪽 길
        { rect: [28, 30, 31.5, 47] },     // 남쪽 길
      ] },
      { terrain: 'Water', shapes: [{ ell: [53.5, 54, 4.6, 3] }] },                                                                // 연못
    ],
  },
  objects: [
    // 집 8채 — 북: ㄱ자 · 대문 쌍둥이 · 지붕창 큰 집 / 가운데: 2층(서) · 곁채(동) / 남: 지붕창 큰 집 · 작은 초가 · ㄱ자
    ['house_lshape', 9, 17.5], ['house_gate', 30, 16.5], ['house_dormer', 50.5, 19.5], ['house_twostory', 7, 35], ['house_annex', 51, 44.5],
    ['house_dormer', 13.5, 56], ['cottage_small', 25, 56.5], ['house_lshape', 39.5, 57],
    // 광장·길
    ['well', 23.5, 25.8], ['stall_green', 36.5, 24.8], ['stall_orange', 23, 36.5], ['stall_grey', 37.5, 37.2], ['barrels3', 41, 27], ['barrels2', 19.5, 23],
    ['cart', 43.6, 36.6], ['table', 33.6, 37.8], ['signpost', 46.6, 27.4], ['barrels2', 33.4, 23.2],
    // 집 둘레
    ['firewood', 15.6, 19.3], ['barrels3', 19.6, 17.8], ['firewood', 12.6, 36.2], ['hay_square', 12.4, 41.6], ['wheelbarrow', 14.6, 45.6], ['scarecrow', 6.5, 41.8],
    ['hay_round', 2.2, 45.2], ['barrels2', 44.2, 45.2], ['firewood', 57.6, 45.2], ['barrels3', 57.2, 20.6], ['hay_square', 44.8, 21.4],
    ['hay_round', 3.2, 50.5], ['barrels2', 6.8, 51.6], ['barrels2', 31.8, 57.2], ['firewood', 47.8, 57.8], ['table', 18.5, 9.2], ['barrels3', 40.5, 8.2],
  ],
  trees: [
    ['pine', 1.5, 3.5, 0], ['bush', 4.5, 1.5, 1], ['pine', 57.5, 4, 1], ['bush', 55, 1.5, 0], ['pine', 58, 59, 0], ['bush', 55.5, 58.8, 1],
    ['pine', 2, 59, 1], ['bush', 49, 52.8, 0], ['pine', 58.6, 50.5, 0],
  ],
};
