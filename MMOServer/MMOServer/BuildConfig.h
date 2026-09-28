#pragma once

// ==========================================================================
// 게임 빌드 토글 — 누구에게 무엇을 보낼지 (MMOServer 전용)
//
// 전송·송신 경로 토글(USE_LOCKFREE_SENDQ · USE_SEND_COALESCING · USE_ZERO_SNDBUF ·
// USE_SEND_THREAD · USE_RIO_TRANSPORT)은 서버 코어로 갔다 → ServerCore/Base/CoreConfig.h
// 아래 교차제약 일부가 그 토글을 참조하므로 먼저 include 한다.
//
// 주의: #if 로 이 토글을 분기하는 모든 TU에서 "가장 먼저" include 해야 한다.
//       include 순서가 어긋나 정의가 보이지 않으면 매크로는 조용히 0으로
//       평가되어 A/B 빌드가 어긋날 수 있다.
// ==========================================================================
#include <CoreConfig.h>

// 섹터 단위 묶음 패킷 실험 — 이동/sync를 이벤트마다 즉시 브로드캐스트하지 않고,
// 틱 끝에 "섹터별 최종 상태(위치·방향·이동상태)"를 한 패킷(S2C_SECTOR_UPDATES)으로 묶어 그 섹터 주변 9섹터에만 전달.
// 목적: 수신자별 복사 횟수를 이벤트 수와 무관하게 (플레이어 × AOI섹터)로 상한 고정.
//   1: dirty 마킹 → 틱 끝 FlushSectorUpdates로 섹터 묶음 송신 [실험]
//   0: 기존 — 이벤트마다 즉시 BroadcastAroundSector (baseline)
//   주의: 틱 내 start→stop 전이 "연출"은 최종상태만 남아 손실(위치 정합은 유지).
#define USE_SECTOR_AGGREGATION 1

// 멤버십(시야 진입/이탈 CREATE/DELETE) 아웃바운드 팬아웃 중복 빌드 제거 실험 — Phase 1.
// ProcessSectorChange에서 "나를 상대에게 통보"(수신자마다 내용 동일)는 지금 수신자 수만큼 패킷을 재빌드한다.
// → 1회만 빌드해 배치 AddRef로 팬아웃(BroadcastSectorPacket 소유권/계측 패턴 재사용).
//   "상대를 나에게 통보"(수신자마다 내용 제각각)는 Phase 1 범위 밖이라 개별 송신 유지.
//   1: 아웃바운드 1회 빌드 후 팬아웃 [실험]
//   0: 기존 — 수신자마다 SendCreateOtherPlayer/SendDeletePlayer로 매번 빌드 (baseline)
//   전제: USE_SECTOR_AGGREGATION=1 (이동 묶음 다음 병목이 멤버십). 송신 순서·수신자·횟수(_membershipSends)는 불변,
//         이득은 버퍼 빌드(Alloc+직렬화) 반복 제거로 _membershipCostUs 절감뿐. A/B는 같은 바이너리 토글로 측정.
#define USE_MEMBERSHIP_FANOUT_DEDUP 1

// 멤버십 인바운드(상대→나) 묶음 실험 — Phase 2.
// ProcessSectorChange에서 "상대를 나(mover)에게 통보"는 수신자가 mover 1명뿐인데 상대 수만큼
// 개별 패킷(빌드+송신)을 만든다 → 상대들을 배치 패킷(S2C_CREATE/DELETE_PLAYER_BATCH, 상한 초과 시 청크 분할)으로 접음.
//   1: 인바운드 배치 송신 — 빌드·송신 모두 상대 N개→청크 수 [실험]
//   0: 기존 — 상대마다 SendCreateOtherPlayer/SendDeletePlayer 개별 송신 (P1 채택 상태 = baseline)
//   전제: USE_MEMBERSHIP_FANOUT_DEDUP=1 (P1 위에 얹는 증분). 수신자·엔트리 순서·_membershipSends(엔트리당 1)는 불변,
//         이득은 패킷당 고정비(Alloc+헤더+enqueue) 제거와 송신 패킷수 감소. A/B는 이 토글로 측정.
#define USE_MEMBERSHIP_INBOUND_BUNDLE 1

#if USE_MEMBERSHIP_INBOUND_BUNDLE && !USE_MEMBERSHIP_FANOUT_DEDUP
	#error "USE_MEMBERSHIP_INBOUND_BUNDLE requires USE_MEMBERSHIP_FANOUT_DEDUP=1 (P1 위에 얹는 증분)"
#endif

// 브로드캐스트 수신섹터 digest 실험 — Phase 3.
// 지금은 소스 섹터마다 "번들 1개 → 주변 3×3 주민 개별 RequestSendMsg"라 수신자×아이템만큼
// 세션 핀(원자연산 2개)+링 뮤텍스+복사를 반복한다 (동접 5000 실측 ~27만회/틱 = broadcast_copy 27.5ms 지배분).
// → 뒤집어서 "수신 섹터별로 이웃 9섹터 번들+채팅을 raw 바이트 1덩어리(digest)로 연접 → 주민당 1회 송신".
//   같은 섹터 주민은 수신 집합이 동일(채팅 excludeSelf=false 본인 포함 정책)하므로 연접 결과 공유 가능.
//   와이어 바이트·패킷 순서 의미·클라 파싱 완전 불변 (coalescing이 이미 틱 끝 1회 송신이라 연접 스트림 동일).
//   채팅도 즉시 브로드캐스트 대신 틱 끝 digest에 합류 (실송신 시점은 기존과 동일 — Deferred flush가 원래 틱 끝).
//   1: 틱 끝 FlushSectorSends — 수신섹터 연접 배포 [실험]
//   0: 기존 — FlushSectorUpdates(소스 섹터 팬아웃) + 채팅 즉시 BroadcastAroundSector (baseline)
#define USE_BROADCAST_BUNDLE 1

//   의존: 이동 번들(SECTOR_AGGREGATION)과 틱 끝 flush(SEND_COALESCING) 위에서만 성립.
#if USE_BROADCAST_BUNDLE && (!USE_SECTOR_AGGREGATION || !USE_SEND_COALESCING)
	#error "USE_BROADCAST_BUNDLE requires USE_SECTOR_AGGREGATION=1 && USE_SEND_COALESCING=1"
#endif
//   배타: digest는 raw 바이트를 링버퍼에 직접 적재 — 포인터 큐(LockFree SendQ)와 양립 불가.
#if USE_BROADCAST_BUNDLE && USE_LOCKFREE_SENDQ
	#error "USE_BROADCAST_BUNDLE requires USE_LOCKFREE_SENDQ=0 (RingBuffer 경로 전용)"
#endif

// 멤버십(시야 진입/이탈 CREATE/DELETE) 아웃바운드 digest 합류 실험 — Phase 4.
// ProcessSectorChange 아웃바운드는 지금 diff 섹터 주민마다 개별 RequestSendMsg를 호출한다
// (동접 5600 실측 ~17만회/틱 — 건당 세션 핀+링 뮤텍스+복사 고정비가 membership_ms의 지배분).
// → 주민들이 어차피 틱마다 받는 digest에 "직송" 아이템으로 합류시켜 추가 송신 호출을 0으로.
//   직송 = 방송(3×3)과 달리 정확히 그 섹터 주민에게만 전달. 연접 순서는 직송이 방송보다 앞
//   (CREATE가 그 대상의 이동 번들보다 먼저 도착 보장 — 클라 파싱 순서 의미 불변).
//   1: 아웃바운드 직송 digest 합류 [실험]
//   0: 기존 — FanoutToSectors 개별 송신 (P1+P2+P3 채택 상태 = baseline)
#define USE_MEMBERSHIP_DIGEST 1

//   의존: 수신섹터 digest 파이프라인(연접·배포·해제) + 아웃바운드 1회 빌드(점프 폴백이 FanoutToSectors 사용).
#if USE_MEMBERSHIP_DIGEST && (!USE_BROADCAST_BUNDLE || !USE_MEMBERSHIP_FANOUT_DEDUP)
	#error "USE_MEMBERSHIP_DIGEST requires USE_BROADCAST_BUNDLE=1 && USE_MEMBERSHIP_FANOUT_DEDUP=1"
#endif

// DB 저장 파이프라인 실험 — dirty flag 기반 비동기 위치 저장 (존 서버 관점)
//   서버 메모리 = 진실, DB = 저장소. 바뀐 플레이어만 주기적으로 전용 워커 스레드가 MySQL에 UPSERT.
//   목적: 전량 저장 부담을 dirty flag로 줄이고, 저장 I/O를 게임 틱 임계경로에서 떼어낸다.
//   1: DB 워커 활성 (MySQL 필요, 지표 mmo_db_*) [실험]
//   0: DB 없이 기존 동작 그대로 (회귀 기준선) [기본]
#ifndef USE_DB_WORKER            // 빌드 시스템이 먼저 정하면 그 값을 따른다(리눅스 검증용)
#define USE_DB_WORKER 1
#endif

// 전투 컨텐츠 — HP·공격·피해·죽음·부활 (판정은 서버, 클라는 그리기만)
//   CActor 계층 위에 얹는 첫 컨텐츠. 웹 클라와 언리얼 클라가 같은 패킷을 그대로 받아쓴다.
//   1: 전투 활성 — C2S_ATTACK 처리 + S2C_ATTACK/DAMAGE/DEATH/RESPAWN/ACTOR_STATS 송신 [기본]
//   0: 전투 경로가 컴파일에서 빠짐 — 실측 기준선과 같은 바이너리
//   주의: 1로 켠 채 부하 더미를 돌리려면 더미가 새 S2C 타입을 알아야 한다.
//         DummyClient::GetPacketSize가 모르는 타입에 0을 돌려주면 packetParseFail → Disconnect라
//         전투 패킷이 섹터에 뿌려지는 순간 더미가 전부 떨어진다.
#define USE_COMBAT 1

// 몬스터 — 맵에 사는 적. 첫 종류는 뱀 하나다.
//   CCharacter 계층 위에 얹는 두 번째 컨텐츠. 전투(USE_COMBAT)가 이미 CCharacter 기준이라
//   몬스터는 "섹터에 들어 있기만" 하면 맞고 죽고 되살아난다 — 그 경로에 새 코드가 없다.
//   1: 존마다 몬스터 스폰 + AI 갱신 [기본]
//   0: 몬스터 경로가 컴파일에서 빠짐 — 기존 A/B 실측과 같은 바이너리
//   와이어는 안 바뀐다: 몬스터는 기존 CREATE 패킷의 displayChar 에 예약값(0x01~)을 실어 보낸다.
//     그래서 부하 더미는 새 타입을 몰라도 되고, 이 토글을 켜도 더미가 안 떨어진다.
//   주의: 0 으로 껐을 때 컴파일이 깨지면 계층이 잘못 얽힌 것이다(USE_COMBAT=0 과 같은 뜻의 시험).
#define USE_MONSTER 1

// 입장 요청 — 계정 번호를 클라가 들고 온다 (C2S_ENTER_GAME).
//   DB 왕복 2단계까지는 접속하는 순간 서버가 번호를 지어내 그 번호로 불러왔다. 그래서 저장은 되는데
//   다시 들어오면 새 번호라 자기 행을 못 찾았다. 이 토글이 그 한 줄을 바꾼다.
//   1: 접속만으로는 캐릭터가 안 생긴다. 입장 요청을 받아야 그 번호로 불러온다 [기본]
//      같은 계정이 이미 들어와 있으면 옛 세션을 정리하고 끊는다.
//   0: 접속하자마자 서버가 번호를 붙여 불러온다 (2단계까지의 동작)
//   ※ 켜면 입장 요청을 안 보내는 클라는 못 들어온다 — 언리얼 클라가 여기 해당한다(보류 중이라 수용).
//     콘솔·부하·웹 셋은 이미 보낸다.
//   기본값이 USE_DB_WORKER 를 따라가는 이유: 입장 요청의 몸이 DB 조회 하나라, 불러올 데가 없으면
//     이 토글은 뜻이 없다. 리눅스 빌드는 CMake 가 -DUSE_DB_WORKER=0 을 주므로 여기서 같이 꺼진다
//     (안 그러면 아래 #error 로 리눅스 빌드가 통째로 막힌다 — 실제로 한 번 막혔다).
#ifndef USE_LOGIN
#define USE_LOGIN USE_DB_WORKER
#endif

// 둘을 따로 정하려 했을 때만 걸린다 — 기본값은 위에서 이미 맞춰 놨다.
#if USE_LOGIN && !USE_DB_WORKER
	#error "USE_LOGIN requires USE_DB_WORKER=1 (입장 요청의 몸이 DB 조회 하나다)"
#endif

// 경험치·레벨 — 몬스터를 잡으면 쌓이고, 표의 값을 넘으면 레벨이 오른다.
//   ★ 레벨별 필요 경험치·최대 체력·공격력은 "기획 데이터"라 코드가 아니라 파일에 둔다
//     (Run/bin/LevelTable.csv). 기동 때 한 번 읽어 배열로 들고, 몬스터를 잡을 때마다 다시 읽지 않는다.
//     DB 가 아니라 파일인 이유 — 밸런스 값이 코드와 같은 커밋에 들어가 같이 롤백된다.
//     DB 에 있는 것은 "내 레벨·내 경험치"(유저 데이터)뿐이고, "레벨 10 이 되려면 얼마"는 여기 없다.
//   1: 경험치·레벨 활성 — 표를 읽고, 몬스터를 잡으면 쌓이고, 레벨이 오른다 [기본]
//   0: 표를 안 읽고 경험치 경로가 컴파일에서 빠짐. characters 의 level·exp 열은 그대로 오가지만
//      값을 바꾸는 코드가 없어 늘 1·0 이다(위치가 DB 토글과 무관하게 도는 것과 같은 결).
//   기본값이 USE_COMBAT 을 따라가는 이유: 경험치를 얻는 유일한 길이 전투다. 전투를 끄면 같이 꺼져
//     아래 #error 를 안 만난다. 리눅스는 -DUSE_DB_WORKER=0 이지만 USE_COMBAT 은 1 이라 여기도 1 이다.
#ifndef USE_EXP
#define USE_EXP USE_COMBAT
#endif

#if USE_EXP && !USE_COMBAT
	#error "USE_EXP requires USE_COMBAT=1 (경험치를 얻는 유일한 길이 전투다)"
#endif

// 인벤토리(가방) — 종류별 탭 셋(장비·소비·기타)에 20칸씩. 무게는 없고 칸 수만 센다.
//   ★ 아이템 정의(이름·탭·쌓임 한도·회복량)는 기획 데이터라 파일에 둔다(Run/bin/ItemTable.csv).
//     가방 안의 "무엇이 몇 개"만 유저 데이터다.
//   1: 가방 활성 — 표를 읽고, 입장 때 가방을 보내고, 운영자 채팅 명령 /item 으로 채운다 [기본]
//   0: 가방 경로가 컴파일에서 빠짐 — '/' 로 시작하는 채팅도 지금처럼 그냥 방송된다
//   기본값이 USE_COMBAT 을 따라가는 이유: 물약이 체력을 채운다(3단계). 전투를 끄면 같이 꺼져
//     아래 #error 를 안 만난다. USE_EXP 와 같은 꼴.
//   주의: 켠 채 부하 더미를 돌리려면 더미가 S2C_INVENTORY 를 알아야 한다 — 빈 가방이라도
//         입장 때 한 장 가므로, 모르면 더미 전원이 접속하자마자 떨어진다(위 USE_COMBAT 주의와 같은 함정).
#ifndef USE_INVENTORY
#define USE_INVENTORY USE_COMBAT
#endif

#if USE_INVENTORY && !USE_COMBAT
	#error "USE_INVENTORY requires USE_COMBAT=1 (물약이 체력을 채운다)"
#endif

// 기획 데이터 파일이 하나라도 필요한가 — 표를 담는 자리(CGameDataFiles)와 CSV 도우미가 이걸 본다.
//   표마다 토글이 따로라, "레벨 표는 끄고 아이템 표만 켠" 빌드에서도 담는 자리는 살아 있어야 한다.
//   스폰 구역 표(SpawnRegion.csv)는 몬스터 토글을 따른다 — 뱀이 태어날 자리라 몬스터를 끄면 뜻이 없다.
#define USE_GAME_DATA_FILES (USE_EXP || USE_INVENTORY || USE_MONSTER)
