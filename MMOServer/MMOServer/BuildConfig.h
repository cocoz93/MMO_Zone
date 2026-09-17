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
