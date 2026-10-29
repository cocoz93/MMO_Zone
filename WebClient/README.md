# MMO 웹 클라이언트 데모 (실서버 접속)

콘솔 `GameClient`를 대체하는 **브라우저 픽셀 클라이언트**. 실제 `MMOServer`에 접속해
다른 플레이어(더미)들의 이동·시야(AOI) 진입/이탈·채팅을 실시간으로 그리고,
화면 위에 **서버 내부 관측 패널**(내 좌표·섹터·시야 인원·초당 패킷·수신량)을 얹는다.

**서버는 한 줄도 고치지 않는다.** 브라우저는 WebSocket만 쓰므로, 그 사이에 얇은
WS↔TCP 릴레이(`relay.js`, Node)를 두어 서버의 TCP 프로토콜을 그대로 흘려보낸다.

```
브라우저(live.html) ──WebSocket──▶ relay.js ──TCP──▶ MMOServer(무수정) ◀── MMOStressClient(더미들)
```

## 실행 (더블클릭)

| 파일 | 용도 |
|---|---|
| **`run-webclient.bat`** | **평소 루틴.** 서버·더미를 평소대로 켠 뒤 이걸 실행 → 릴레이(있으면 재사용) + 브라우저로 `live.html`. |
| `run-all.bat` | 전체 한 번에 — 서버 + 더미 + 릴레이 + 클라. (`-Dummies N`으로 더미 수 조절) |
| `stop.bat` | 릴레이·더미 정리(서버 유지). `stop.bat -StopServer` = 서버까지. |
| `relay-only.bat` | 릴레이만 별도 창(로그 표시, Ctrl+C 종료). 디버깅용. |

- **서버·더미·릴레이는 각각 보이는 창으로 뜬다**(백그라운드 아님) — 로그가 보이고, 창을 닫으면 그 프로세스가 꺼진다. 한꺼번에 끄려면 `stop.bat`.
- 내부 로직은 `_launch.ps1`(기동) / `_stop.ps1`(종료) — .bat이 이걸 부름. 직접 안 건드려도 됨.
- 서버가 시작하려면 **MySQL(3306)** 이 떠 있어야 한다(`USE_DB_WORKER=1` 빌드 기준, 인증용이 아니라 위치 저장용).
  순수 무DB로 돌리려면 `MMOServer/MMOServer/BuildConfig.h`의 `USE_DB_WORKER 0` 후 서버만 재빌드.
- `_launch.ps1`은 `Run\bin\MMOStressConfig.ini`를 잠깐 낮췄다가 **원복**한다(스트레스 설정 보존).
- 인코딩: `.ps1`=UTF-8 **BOM**(PS5.1 한글), `.bat`=**CP949 무BOM**(cmd 한글). 편집 시 유지할 것.

## 조작
- **WASD / 방향키** — 내 캐릭터를 실제 서버에서 이동(서버 권위 위치로 렌더).
- 좌하단 주변 채팅(서버가 AOI = 내 섹터 ±1 로만 뿌린다) / 우상단 **SERVER INTERNALS** 패널 / 우하단 미니맵.
- **I** — 가방 열고 닫기(Esc 도 닫음). 서버 패널 아래에 뜨고, 탭(장비·소비·기타)은 마우스로 누른다. 칸에 마우스를 올리면 설명.
- 물약은 칸을 **두 번 눌러** 쓴다(서버가 1초 대기시간을 건다). 체력이 가득하거나 쓰러져 있으면 안 줄고, 까닭이 `[System]` 으로 온다.
- **Z** — 줍기. 뱀을 잡으면 드랍 표(`Run/bin/DropTable.csv`)대로 아이템이 발밑에 떨어진다. 가까이(2칸) 가서 Z 한 번에 가장 가까운 것 하나가 가방에 들어간다.
  막타 친 사람이 10초 먼저 주울 수 있고, 안 주우면 60초 뒤 사라진다. 가방이 차 있거나 남의 것이면 까닭이 `[System]` 으로 온다.
- **C** — 상태창 열고 닫기(Esc 도 닫음). 레벨·체력·공격력·방어력·무기 칸. 공격력·방어력은 서버가 보낸 최종값(레벨 표 + 무기)이다.
  가방 장비 탭의 무기를 **두 번 눌러** 끼우고(차 있으면 맞바꿈), 상태창 무기 칸을 **두 번 눌러** 벗는다(장비 탭 첫 빈 칸으로). 1초 대기 · 쓰러져 있거나 장비 탭이 가득하면 까닭이 `[System]` 으로 온다.
- 운영자가 가방을 바로 채우기 — 주소에 `?admin=키` 를 붙여 운영자로 들어온 뒤 채팅창에 `/item <아이템번호> [개수]`.
  `/` 로 시작하는 채팅은 방송되지 않고 서버 명령으로 처리되며, 답은 채팅 칸에 `[System]` 으로 온다.
- 아이템 이름·그림은 **`serve.js` 로 열 때만** 나온다(`Run/4. Web_client.bat` = `http://127.0.0.1:8200/live.html`) — 아이템 표를 `/data/ItemTable.csv` 로 받기 때문이다. 다른 길로 열면 칸에 아이템 번호가 회색으로 찍힌다(동작은 같다).

## 구성 파일
| 파일 | 역할 |
|---|---|
| `live.html` | 브라우저 클라(픽셀 렌더 + 서버 내부 오버레이). `?snap`=스냅샷 렌더(검증용) |
| `proto.js` | 와이어 프로토콜 파서/인코더 — `Shared/Protocol/Protocol.h` 1:1 이식 (pack(1), 리틀엔디안) |
| `relay.js` | WS↔TCP 릴레이(프로토콜 무관 바이트 파이프). `--ws 9000 --server-port 6000` |
| `serve.js` | 정적 서버 `node serve.js --port 8200`. `WebClient/` 밖은 막고, 예외로 `/data/ItemTable.csv` 하나만 `Run/bin/ItemTable.csv` 를 내준다(웹이 아이템 이름·그림 번호를 서버와 같은 파일에서 읽는다) |
| `assets/items.js` | 아이템 그림 시트 — [LPC] Items and game effects(512×512, 32px 칸 16×16), base64. 크레딧: Reemax(Tuomo Untinen) 외 — 원문 `assets/items_credits.txt` (CC-BY-SA 3.0 / GPL 3.0 / GPL 2.0, https://opengameart.org/content/lpc-items-and-game-effects) |
| `assets/assetsB.js` | LPC 스프라이트/타일(base64). 크레딧: LPC — Sharm·Redshrike·Mandi Paugh·William.Thompsonj (CC-BY-SA) |
| `assets/monsters.js` | 몬스터 그림(base64) — 뱀·구렁이(LPC monsters, CC-BY-SA 3.0) · 뱀왕·가짜 뱀왕(LPC 캐릭터 생성기 층을 겹친 사람형 16열 × 4행 — 도마뱀 머리·꼬리 · 도포 Luke Mehl · 지팡이 bluecarrot16 외, OGA-BY 3.0 / CC-BY-SA 3.0 / GPL 3.0 / CC-BY 4.0). 원문 `assets/monsters_credits.txt` |
| `assets/lpc_layers.js` | 캐릭터 합성용 LPC 레이어(몸/머리/바지/상의/머리카락) 걸음 블록 576×256, base64. 크레딧: Universal LPC Spritesheet Character Generator (CC-BY-SA) |
| `assets/town.js` | 마을(맵 0) 그림 — 땅 아틀라스 + 층별 칸 번호 · 초가집 6꼴 · 장터 소품 · 자리 · 포털 돌 받침, base64. **`tools/town/build.js` 가 만든다(손으로 고치지 말 것)** — 배치는 `tools/town/town0.js`, 원본 그림은 `tools/town/src/`. 크레딧: [LPC] Terrains · [LPC] Medieval Village Decorations (CC-BY-SA 4.0 / 3.0) · [LPC] Thatched-roof Cottage · [LPC] Windows & Doors · LPC Base Assets (CC-BY-SA 3.0 / GPL 3.0) · Teleporter Circle — Clint Bellanger (CC-BY 3.0) — 원문 `assets/town_credits.txt` |
| `fonts/galmuri/` | 캔버스 글꼴 Galmuri(픽셀) woff2 8벌 — Galmuri7·9·11·11 Bold·14 · GalmuriMono7·9·11 v2.40.3. 크레딧: Lee Minseo(quiple) — SIL OFL 1.1, 원문 `fonts/galmuri/LICENSE.txt` (https://github.com/quiple/galmuri). 크기 고르기는 `live.html` `uiFont()` |
| `tools/town/` | 마을 그림 만들기 — `node tools/town/build.js`(크롬 필요: 그림 오리기·합치기를 헤드리스 크롬 캔버스로). 집 다섯 꼴은 Cottage 팩 미리보기 그림에서 땅 색 픽셀을 지워 오려 낸 것 |

## 프로토콜 메모 (재구현 근거)
- 헤더 4B = `size(u16, 헤더포함)` + `type(u16, MsgType)`. 로그인/인증 패킷 없음 — TCP 접속 즉시
  서버가 `S2C_ZONE_INFO` → `S2C_CREATE_MY_PLAYER` → 주변 `CREATE_OTHER`를 밀어준다.
- 위치 스트림: `S2C_SECTOR_UPDATES`(가변, entry 9B — 좌표는 1/512타일 눈금의 u16, 방향·이동상태는 한 바이트에 니블로).
  시야 진입/이탈: `CREATE/DELETE_PLAYER(_BATCH)`.
  채팅: `S2C_CHAT`(UTF-16LE, 널종단). 좌표 보정: `S2C_SYNC_POSITION`.
- 살아있으려면 60초 세션 타임아웃 전에 아무 패킷(가장 간단히 `C2S_HEARTBEAT`)을 주기 송신 → 클라는 15초마다 보낸다.
- 월드 120×120, 섹터 20 (`Run\bin\MMOServerConfig.ini`).
