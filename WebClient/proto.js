// Wire protocol for the IOCP MMO server, ported 1:1 from Shared/Protocol/Protocol.h
// #pragma pack(1), little-endian. Shared by the Node probe and the browser client.
(function (global) {
  "use strict";
  const MSG = {
    ECHO:0,
    C2S_MOVE_START:1000, C2S_MOVE_STOP:1001,
    S2C_MOVE_START:1002, S2C_MOVE_STOP:1003,
    S2C_CREATE_MY_PLAYER:1004, S2C_CREATE_OTHER_PLAYER:1005, S2C_DELETE_PLAYER:1006,
    C2S_CHAT:1007, S2C_CHAT:1008,
    S2C_SYNC_POSITION:1009,
    S2C_ZONE_INFO:1010, C2S_ZONE_CHANGE:1011, S2C_ZONE_CHANGE_OK:1012, S2C_ZONE_CHANGE_FAIL:1013,
    C2S_HEARTBEAT:1014,
    C2S_ADMIN_LOGIN:1015, S2C_ADMIN_LOGIN_OK:1016, S2C_ADMIN_LOGIN_FAIL:1017,
    S2C_ERROR:1018,
    S2C_SECTOR_UPDATES:1019, S2C_CREATE_PLAYER_BATCH:1020, S2C_DELETE_PLAYER_BATCH:1021,
    // 전투 (USE_COMBAT) — 번호가 한 칸만 밀려도 조용히 엉뚱한 패킷을 읽으므로 Protocol.h 와 나란히 볼 것
    C2S_ATTACK:1022, S2C_ATTACK:1023, S2C_DAMAGE:1024, S2C_DEATH:1025,
    S2C_RESPAWN:1026, S2C_ACTOR_STATS:1027,
    // 입장 (USE_LOGIN) — 접속 직후 한 번 보낸다. 새 타입은 늘 끝에만 붙는다
    C2S_ENTER_GAME:1028,
    // 경험치·레벨 (USE_EXP) — 본인 전용
    S2C_EXP:1029,
    // 인벤토리 (USE_INVENTORY) — 본인 전용. 가방 전체는 입장 때 한 번, 칸 갱신은 바뀔 때마다
    S2C_INVENTORY:1030, S2C_INVENTORY_SLOT:1031,
    C2S_USE_ITEM:1032, S2C_USE_ITEM_FAIL:1033,
    // 땅의 아이템·줍기 (USE_DROP) — 땅 번호(groundId)는 캐릭터 번호와 따로 센다
    S2C_GROUND_ITEM_CREATE:1034, S2C_GROUND_ITEM_DELETE:1035,
    C2S_PICKUP:1036, S2C_PICKUP_FAIL:1037,
    // 스탯·장착 (USE_COMBAT · USE_EQUIP) — 내 스탯은 본인 전용. 끼우기·빼기 성공은 칸 갱신 둘 + MY_STATS 가 곧 결과다
    S2C_MY_STATS:1038, C2S_EQUIP:1039, C2S_UNEQUIP:1040, S2C_EQUIP_FAIL:1041,
    // 스킬 (USE_SKILL) — 방향·대상은 서버가 정한다. 시전은 주변 방송, 거절은 본인에게만
    C2S_USE_SKILL:1042, S2C_SKILL_CAST:1043, S2C_SKILL_FAIL:1044,
    // 마력 (USE_SKILL) — 본인에게만 온다. 대상 번호를 싣는다(ACTOR_STATS 와 같은 꼴)
    S2C_ACTOR_MP:1045,
    // 지속 효과 (USE_COMBAT) — 본인에게만 온다. 걸림·되감김·만료·죽음·존 이동, remainMs 0 = 풀림. 공격력 숫자는 MY_STATS 가 뒤따른다
    S2C_ACTOR_BUFF:1046,
    // 포털 (토글 없음) — 포털 위에서 상호작용 키. 성공은 존 이동과 같은 와이어(ZONE_INFO → ZONE_CHANGE_OK), 실패는 ZONE_CHANGE_FAIL 사유 4·5
    C2S_PORTAL_USE:1047,
    // 스킬 쿨타임 상태 (USE_SKILL) — 본인에게만 온다. 입장·존 이동 때 아직 돌고 있는 칸마다 한 장(남은 ms). 시전 순간의 쿨타임은 지금처럼 SKILL_CAST 로 센다
    S2C_SKILL_COOLDOWN_REMAIN:1048,
    // 이동 속도 (토글 없음) — 본인 + 주변. 등장 순간의 속도는 CREATE 가 싣고, 이 장은 그 뒤에 바뀐 값(지금은 운영자 /gm 켬·끔뿐)
    S2C_ACTOR_SPEED:1049,
    // 파티 (USE_PARTY) — 고르기는 화면에서 캐릭터를 눌러서(캐릭터에 이름이 없다). 무엇이 갈지(초대·신청·추천)는 서버가 가린다
    C2S_PARTY_REQUEST:1050, C2S_PARTY_ANSWER:1051, C2S_PARTY_LEAVE:1052, C2S_PARTY_KICK:1053, C2S_PARTY_LEADER:1054, C2S_PARTY_DISBAND:1055,
    S2C_PARTY_ASK:1056, S2C_PARTY_ASK_END:1057, S2C_PARTY_NOTICE:1058, S2C_PARTY_INFO:1059, S2C_PARTY_MEMBER:1060
  };
  // 파티 — Protocol.h PartyAskKind · PartyNotice 순서 그대로(값이 와이어에 실린다). 한글 문구는 live.html 이 이름으로 고른다
  const PARTY_ASK_KIND = ['INVITE','APPLY','SUGGEST'];
  const PARTY_NOTICE = ['SENT_INVITE','SENT_APPLY','SENT_SUGGEST',
    'NO_TARGET','SELF','SAME_PARTY','BOTH_IN_PARTY','FULL','NOT_LEADER','NOT_IN_PARTY','NOT_MEMBER','TARGET_BUSY','SELF_BUSY','NO_REQUEST',
    'DECLINED','EXPIRED','CANCELLED','JOINED','LEFT','KICKED','LEADER','DISBANDED'];
  const PARTY_MAX_MEMBERS = 8;      // Protocol.h PARTY_MAX_MEMBERS — INFO 줄 수 상한
  const PARTY_MEMBER_BYTES = 22;    // Protocol.h PartyMemberEntry = PartyPerson 10 + hp·maxHp·mapId 12
  // 아이템 쓰기 거절 사유 — Protocol.h UseItemFail 순서 그대로(값이 와이어에 실린다). 서버가 이 순서로 보고 먼저 걸린 하나를 보낸다
  const USE_FAIL = ['BAD_SLOT','EMPTY','NOT_USABLE','DEAD','COOLDOWN','FULL_HP','FULL_MP'];
  // 땅의 아이템 — Protocol.h GroundItemAppear · GroundItemGone · PickupFail 순서 그대로(값이 와이어에 실린다)
  const GROUND_APPEAR = ['SEEN','DROP'];
  const GROUND_GONE   = ['OUT_OF_SIGHT','PICKED','EXPIRED'];
  const PICKUP_FAIL   = ['DEAD','NO_ITEM','NOT_OWNER','TAB_FULL'];
  // 가방 탭 구간 — Protocol.h INV_TAB_RANGES 사본(InvTab 순서: 장비·소비·기타). 칸 번호가 곧 DB 열쇠라
  //   한쪽만 고치면 웹이 엉뚱한 탭에 그린다 — Protocol.h 와 나란히 볼 것.
  const INV_TAB_RANGES = [ { first:0, n:20 }, { first:100, n:20 }, { first:200, n:20 } ];
  // 착용 칸 — Protocol.h EQUIP_SLOT_FIRST + EquipSlot 사본(지금은 무기 하나). 가방 탭과 따로 세고, 가방과 같은 칸 목록(S2C_INVENTORY)에 실려 온다
  const EQUIP_SLOT = { WEAPON: 900 };
  // 끼우기·빼기 거절 사유 — Protocol.h EquipFail 순서 그대로(값이 와이어에 실린다)
  const EQUIP_FAIL = ['BAD_SLOT','EMPTY','NOT_EQUIPPABLE','DEAD','COOLDOWN','TAB_FULL'];
  // 스킬 거절 사유 — Protocol.h SkillFail 순서 그대로(값이 와이어에 실린다). 거절이면 체력·마력·쿨타임이 그대로다
  const SKILL_FAIL = ['BAD_SKILL','DEAD','COOLDOWN','LOW_HP','NO_TARGET','LOW_MP'];
  // 지속 효과 종류 — Protocol.h BuffKind 순서 그대로(값이 와이어에 실린다). 스킬 표의 effect 「buff_<종류>」 와 이름이 같다
  const BUFF_KIND = ['attack_mul'];
  // 맵 이동 실패 사유 — Protocol.h MSG_S2C_ZONE_CHANGE_FAIL 의 reason 주석 순서 그대로(존 이동 요청 · 포털 공통)
  const ZONE_FAIL = ['NOT_FOUND','FULL','SAME_ZONE','NOT_ALLOWED','TOO_FAR','NO_PORTAL'];
  const NAME =Object.fromEntries(Object.entries(MSG).map(([k,v])=>[v,k]));
  const DIR = { NONE:0, UP:1, DOWN:2, LEFT:3, RIGHT:4 };  // Player.h
  const MOVE = { IDLE:0, MOVING:1 };
  const POS_QUANT_INV = 1 / 512;   // SectorUpdateEntry ships coords as uint16 ticks (Protocol.h POS_QUANT_SCALE)
  const U16LE = new TextDecoder('utf-16le');
  const U8 = new TextDecoder('utf-8');

  // 등장 상태 조각 — Protocol.h ActorSnapshot(CREATE 셋의 맨 끝). 필드를 늘리면 여기와 아래 묶음 보폭을 같이 고칠 것
  function readSnap(dv, o) { return { hp:dv.getInt32(o,true), maxHp:dv.getInt32(o+4,true) }; }
  const CREATE_ENTRY_BYTES = 29;   // Protocol.h CreatePlayerEntry = 몸통 21 + ActorSnapshot 8
  // 남의 캐릭터 한 명분 — Protocol.h CreatePlayerEntry. CREATE_OTHER 의 몸통이자 묶음 엔트리라 둘이 이것 하나를 부른다
  function readCreateEntry(dv, o) {
    return { playerId:dv.getInt32(o,true), direction:dv.getUint8(o+4), moveState:dv.getUint8(o+5),
      displayChar:dv.getUint8(o+6), colorIndex:dv.getUint8(o+7), spawnReason:dv.getUint8(o+8),
      x:dv.getFloat32(o+9,true), y:dv.getFloat32(o+13,true), speed:dv.getInt32(o+17,true),
      ...readSnap(dv, o+21) };
  }
  // 파티 사람 조각 — Protocol.h PartyPerson 10B = actorId i32 · displayChar u8 · colorIndex u8 · level i32
  function readPartyPerson(dv, o) {
    return { actorId:dv.getInt32(o,true), displayChar:dv.getUint8(o+4), colorIndex:dv.getUint8(o+5), level:dv.getInt32(o+6,true) };
  }
  // 파티원 한 줄 — Protocol.h PartyMemberEntry 22B = PartyPerson 10 · hp i32 · maxHp i32 · mapId i32. INFO 의 줄이자 MEMBER 의 몸통
  function readPartyMember(dv, o) {
    return { ...readPartyPerson(dv, o), hp:dv.getInt32(o+10,true), maxHp:dv.getInt32(o+14,true), mapId:dv.getInt32(o+18,true) };
  }

  // ---- decode one packet body (dv positioned at packet start, total = header.size) ----
  function decodePacket(dv, base, size) {
    const type = dv.getUint16(base + 2, true);
    const p = { type, name: NAME[type] || ('?' + type) };
    const o = base + 4; // after header
    switch (type) {
      case MSG.S2C_ZONE_INFO:
        p.mapId=dv.getInt32(o,true); p.channelIndex=dv.getInt32(o+4,true);
        p.mapWidth=dv.getInt32(o+8,true); p.mapHeight=dv.getInt32(o+12,true); p.sectorSize=dv.getInt32(o+16,true);
        break;
      // ZONE_CHANGE_OK 23B = mapId i32 · channelIndex i32 · playerId i32 · displayChar u8 · colorIndex u8 · direction u8 · x f32 · y f32
      //   존을 옮긴 내 자리 — 체력은 안 싣는다(존을 옮겨도 그대로다)
      case MSG.S2C_ZONE_CHANGE_OK:
        p.mapId=dv.getInt32(o,true); p.channelIndex=dv.getInt32(o+4,true); p.playerId=dv.getInt32(o+8,true);
        p.displayChar=dv.getUint8(o+12); p.colorIndex=dv.getUint8(o+13); p.direction=dv.getUint8(o+14);
        p.x=dv.getFloat32(o+15,true); p.y=dv.getFloat32(o+19,true); break;
      //   ZONE_CHANGE_FAIL 1B = reason u8 (ZONE_FAIL)
      case MSG.S2C_ZONE_CHANGE_FAIL:
        p.reason=dv.getUint8(o); p.reasonName=ZONE_FAIL[p.reason] || ('?'+p.reason); break;
      case MSG.S2C_CREATE_MY_PLAYER:
        p.playerId=dv.getInt32(o,true); p.direction=dv.getUint8(o+4);
        p.displayChar=dv.getUint8(o+5); p.colorIndex=dv.getUint8(o+6);
        p.x=dv.getFloat32(o+7,true); p.y=dv.getFloat32(o+11,true); p.speed=dv.getInt32(o+15,true);
        Object.assign(p, readSnap(dv, o+19));
        break;
      case MSG.S2C_CREATE_OTHER_PLAYER:
        Object.assign(p, readCreateEntry(dv, o));
        break;
      case MSG.S2C_DELETE_PLAYER:
        p.playerId=dv.getInt32(o,true); break;
      case MSG.S2C_MOVE_START:
      case MSG.S2C_MOVE_STOP:
        p.playerId=dv.getInt32(o,true); p.direction=dv.getUint8(o+4);
        p.x=dv.getFloat32(o+5,true); p.y=dv.getFloat32(o+9,true); break;
      case MSG.S2C_SYNC_POSITION:
        p.playerId=dv.getInt32(o,true); p.x=dv.getFloat32(o+4,true); p.y=dv.getFloat32(o+8,true); break;
      case MSG.S2C_CHAT: {
        p.playerId=dv.getInt32(o,true); p.displayChar=dv.getUint8(o+4); p.colorIndex=dv.getUint8(o+5);
        // wchar_t[] UTF-16LE from o+6 .. end, includes trailing NUL code unit
        const bytes = size - 10; // header4 + pid4 + 2 flag bytes = 10
        let s = '';
        if (bytes > 0) {
          const sub = new Uint8Array(dv.buffer, dv.byteOffset + o + 6, bytes);
          s = U16LE.decode(sub);
          const nul = s.indexOf('\u0000'); if (nul >= 0) s = s.slice(0, nul);
        }
        p.message = s; break;
      }
      case MSG.S2C_SECTOR_UPDATES: {
        const count = dv.getUint16(o, true); p.count = count; p.entries = [];
        let e = o + 2;
        for (let i=0;i<count;i++){
          const ds = dv.getUint8(e+8);   // low nibble = Direction, high nibble = MoveState
          p.entries.push({ playerId:dv.getInt32(e,true),
            x:dv.getUint16(e+4,true)*POS_QUANT_INV, y:dv.getUint16(e+6,true)*POS_QUANT_INV,
            direction:ds & 0x0F, moveState:ds >> 4 }); e += 9;
        }
        break;
      }
      case MSG.S2C_CREATE_PLAYER_BATCH: {
        const count = dv.getUint16(o, true); p.count = count; p.entries = [];
        let e = o + 2;
        for (let i=0;i<count;i++){
          p.entries.push(readCreateEntry(dv, e)); e += CREATE_ENTRY_BYTES;
        }
        break;
      }
      case MSG.S2C_DELETE_PLAYER_BATCH: {
        const count = dv.getUint16(o, true); p.count = count; p.entries = [];
        let e = o + 2;
        for (let i=0;i<count;i++){ p.entries.push({ playerId:dv.getInt32(e,true) }); e += 4; }
        break;
      }
      case MSG.S2C_ERROR: {
        // char[256] UTF-8 — 서버가 한글 답(채팅 명령 등)을 보낸다. 'ascii' 로 풀면 한글이 깨진다.
        //   NUL 에서 자른다 — 뒤는 0 으로 채워져 있다.
        const sub = new Uint8Array(dv.buffer, dv.byteOffset + o, size - 4);
        const s = U8.decode(sub); const nul = s.indexOf('\u0000');
        p.message = nul >= 0 ? s.slice(0, nul) : s; break;
      }
      // ---- 전투 (USE_COMBAT) — 필드 순서는 Protocol.h 의 구조체 그대로 ----
      case MSG.S2C_ATTACK:                       // 헛스윙도 온다 (휘두르는 그림용)
        p.attackerId=dv.getInt32(o,true); p.direction=dv.getUint8(o+4); break;
      case MSG.S2C_DAMAGE:
        p.targetId=dv.getInt32(o,true); p.attackerId=dv.getInt32(o+4,true);
        p.damage=dv.getInt32(o+8,true); p.remainHp=dv.getInt32(o+12,true); break;
      case MSG.S2C_DEATH:
        p.targetId=dv.getInt32(o,true); break;
      case MSG.S2C_RESPAWN:
        p.targetId=dv.getInt32(o,true);
        p.x=dv.getFloat32(o+4,true); p.y=dv.getFloat32(o+8,true); p.hp=dv.getInt32(o+12,true); break;
      case MSG.S2C_ACTOR_STATS:                  // 체력 갱신(물약·레벨업) — 등장 순간 체력은 CREATE 가 싣는다
        p.actorId=dv.getInt32(o,true); p.hp=dv.getInt32(o+4,true); p.maxHp=dv.getInt32(o+8,true); break;
      // ---- 경험치·레벨 (USE_EXP) — 본인 전용. 표는 서버 것이라 막대의 분자·분모가 같이 온다 ----
      //   #pragma pack(1) 이라 int32 뒤에 int64 가 곧바로 붙는다(패딩 없음) → 오프셋 4,8,16,24
      //   nextLevelExp 가 0 이면 만렙 — 나누지 말고 MAX 로 그린다
      case MSG.S2C_EXP:
        p.level=dv.getInt32(o,true);
        p.exp=Number(dv.getBigInt64(o+4,true));
        p.levelBaseExp=Number(dv.getBigInt64(o+12,true));
        p.nextLevelExp=Number(dv.getBigInt64(o+20,true)); break;
      // ---- 인벤토리 (USE_INVENTORY) — 칸 한 개 = slot u16 + itemId i32 + count i32 = 10B (pack(1)) ----
      //   전체 목록은 빈 칸을 안 싣는다. count 는 header.size 로 한 번 더 막는다(서버 이상 시 넘겨 읽지 않게).
      case MSG.S2C_INVENTORY: {
        let count = dv.getUint16(o, true);
        const avail = Math.floor((size - 6) / 10); if (count > avail) count = avail;
        p.count = count; p.entries = [];
        let e = o + 2;
        for (let i=0;i<count;i++){ p.entries.push({ slot:dv.getUint16(e,true), itemId:dv.getInt32(e+2,true), count:dv.getInt32(e+6,true) }); e += 10; }
        break;
      }
      case MSG.S2C_INVENTORY_SLOT:               // 칸의 최종값 — itemId 0 = 비었다
        p.slot=dv.getUint16(o,true); p.itemId=dv.getInt32(o+2,true); p.count=dv.getInt32(o+6,true); break;
      case MSG.S2C_USE_ITEM_FAIL:                // 성공은 따로 안 온다 — 칸 갱신과 S2C_ACTOR_STATS(마력 물약이면 S2C_ACTOR_MP)가 곧 결과다
        p.slot=dv.getUint16(o,true); p.reason=dv.getUint8(o+2); p.reasonName=USE_FAIL[p.reason] || ('?'+p.reason); break;
      // ---- 땅의 아이템 (USE_DROP) — pack(1) 이라 int32·float 뒤 uint8 이 곧바로 붙는다 ----
      //   CREATE 25B = groundId i32 · itemId i32 · count i32 · x f32 · y f32 · reason u8 (Protocol.h static_assert)
      //   같은 groundId 가 두 번 올 수 있다 — 받는 쪽은 덮어쓴다. 주인은 안 실린다
      case MSG.S2C_GROUND_ITEM_CREATE:
        p.groundId=dv.getInt32(o,true); p.itemId=dv.getInt32(o+4,true); p.count=dv.getInt32(o+8,true);
        p.x=dv.getFloat32(o+12,true); p.y=dv.getFloat32(o+16,true);
        p.reason=dv.getUint8(o+20); p.reasonName=GROUND_APPEAR[p.reason] || ('?'+p.reason); break;
      //   DELETE 13B = groundId i32 · reason u8 · pickerId i32 (PICKED 일 때 주운 캐릭터, 아니면 0). 모르는 번호면 받는 쪽이 버린다
      case MSG.S2C_GROUND_ITEM_DELETE:
        p.groundId=dv.getInt32(o,true); p.reason=dv.getUint8(o+4); p.reasonName=GROUND_GONE[p.reason] || ('?'+p.reason);
        p.pickerId=dv.getInt32(o+5,true); break;
      //   PICKUP_FAIL 9B = reason u8 · itemId i32 (TAB_FULL 이면 고른 아이템 — 어느 탭이 찼는지 안다). 성공은 DELETE(PICKED, 나)
      case MSG.S2C_PICKUP_FAIL:
        p.reason=dv.getUint8(o); p.reasonName=PICKUP_FAIL[p.reason] || ('?'+p.reason); p.itemId=dv.getInt32(o+1,true); break;
      // ---- 스탯·장착 (USE_COMBAT · USE_EQUIP) ----
      //   MY_STATS 12B = attack i32 · defense i32 — 최종값(레벨 표 + 착용 장비). 클라는 합을 셈하지 않는다
      case MSG.S2C_MY_STATS:
        p.attack=dv.getInt32(o,true); p.defense=dv.getInt32(o+4,true); break;
      //   EQUIP_FAIL 7B = slot u16 · reason u8 (요청에 실린 칸 번호 그대로)
      case MSG.S2C_EQUIP_FAIL:
        p.slot=dv.getUint16(o,true); p.reason=dv.getUint8(o+2); p.reasonName=EQUIP_FAIL[p.reason] || ('?'+p.reason); break;
      // ---- 스킬 (USE_SKILL) ----
      //   SKILL_CAST 10B = casterId i32 · skillId u8 · direction u8 — 시전자 주변 방송(본인 포함). 쿨타임은 표의 cooldownMs 로 받은 순간부터 센다
      case MSG.S2C_SKILL_CAST:
        p.casterId=dv.getInt32(o,true); p.skillId=dv.getUint8(o+4); p.direction=dv.getUint8(o+5); break;
      //   SKILL_FAIL 6B = skillId u8 · reason u8 (요청에 실린 번호 그대로)
      case MSG.S2C_SKILL_FAIL:
        p.skillId=dv.getUint8(o); p.reason=dv.getUint8(o+1); p.reasonName=SKILL_FAIL[p.reason] || ('?'+p.reason); break;
      //   ACTOR_MP 16B = actorId i32 · mp i32 · maxMp i32 — 본인에게만(접속·존 이동·레벨업·스킬 소모·마력 물약)
      case MSG.S2C_ACTOR_MP:
        p.actorId=dv.getInt32(o,true); p.mp=dv.getInt32(o+4,true); p.maxMp=dv.getInt32(o+8,true); break;
      //   ACTOR_BUFF 17B = actorId i32 · kind u8 · value i32 · remainMs u32 — 본인에게만(걸림·되감김·만료·죽음·존 이동). remainMs 0 = 풀림
      case MSG.S2C_ACTOR_BUFF:
        p.actorId=dv.getInt32(o,true); p.kind=dv.getUint8(o+4); p.kindName=BUFF_KIND[p.kind] || ('?'+p.kind);
        p.value=dv.getInt32(o+5,true); p.remainMs=dv.getUint32(o+9,true); break;
      //   SKILL_COOLDOWN_REMAIN 13B = actorId i32 · skillId u8 · remainMs u32 — 본인에게만(입장·존 이동). remainMs 는 늘 1 이상
      case MSG.S2C_SKILL_COOLDOWN_REMAIN:
        p.actorId=dv.getInt32(o,true); p.skillId=dv.getUint8(o+4); p.remainMs=dv.getUint32(o+5,true); break;
      //   ACTOR_SPEED 12B = actorId i32 · speed i32(칸/초 — CREATE 의 speed 와 같은 단위) — 본인 + 주변
      case MSG.S2C_ACTOR_SPEED:
        p.actorId=dv.getInt32(o,true); p.speed=dv.getInt32(o+4,true); break;
      // ---- 파티 (USE_PARTY) ----
      //   PARTY_ASK 33B = requestId u32 · kind u8 · from PartyPerson(10) · about PartyPerson(10) · remainMs u32 — 받는 사람에게만
      case MSG.S2C_PARTY_ASK:
        p.requestId=dv.getUint32(o,true); p.kind=dv.getUint8(o+4); p.kindName=PARTY_ASK_KIND[p.kind] || ('?'+p.kind);
        p.from=readPartyPerson(dv, o+5); p.about=readPartyPerson(dv, o+15); p.remainMs=dv.getUint32(o+25,true); break;
      //   PARTY_ASK_END 9B = requestId u32 · reason u8 (PARTY_NOTICE — EXPIRED · CANCELLED). 창을 닫는다
      case MSG.S2C_PARTY_ASK_END:
        p.requestId=dv.getUint32(o,true); p.reason=dv.getUint8(o+4); p.reasonName=PARTY_NOTICE[p.reason] || ('?'+p.reason); break;
      //   PARTY_NOTICE 9B = code u8 (PARTY_NOTICE) · actorId i32 (관련 사람, 없으면 0)
      case MSG.S2C_PARTY_NOTICE:
        p.code=dv.getUint8(o); p.codeName=PARTY_NOTICE[p.code] || ('?'+p.code); p.actorId=dv.getInt32(o+1,true); break;
      //   PARTY_INFO 가변 = partyId i32 · leaderId i32 · count u8 · 파티원 줄(22B) × count — count 0 = 파티 없음.
      //   count 는 header.size 와 상한으로 한 번 더 막는다(서버 이상 시 넘겨 읽지 않게 — S2C_INVENTORY 와 같은 꼴)
      case MSG.S2C_PARTY_INFO: {
        p.partyId=dv.getInt32(o,true); p.leaderId=dv.getInt32(o+4,true);
        let count=dv.getUint8(o+8);
        const avail=Math.floor((size-13)/PARTY_MEMBER_BYTES); if(count>avail) count=avail; if(count>PARTY_MAX_MEMBERS) count=PARTY_MAX_MEMBERS;
        p.count=count; p.members=[];
        for(let i=0;i<count;i++) p.members.push(readPartyMember(dv, o+9+i*PARTY_MEMBER_BYTES));
        break;
      }
      //   PARTY_MEMBER 26B = 파티원 한 줄(22B) — 바뀐 사람만(0.5초마다 서버가 견준다)
      case MSG.S2C_PARTY_MEMBER:
        Object.assign(p, readPartyMember(dv, o)); break;
      default: break; // ADMIN_* etc. — not needed for the view
    }
    return p;
  }

  // ---- stream reassembler: TCP is a byte stream; WS frames != packet boundaries ----
  function Reassembler(onPacket) {
    let buf = new Uint8Array(0);
    this.push = (chunk) => {
      const u8 = chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk);
      const merged = new Uint8Array(buf.length + u8.length);
      merged.set(buf, 0); merged.set(u8, buf.length); buf = merged;
      let off = 0;
      const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
      while (buf.length - off >= 4) {
        const size = dv.getUint16(off, true);
        if (size < 4 || size > 4096) { off = buf.length; break; } // corrupt -> drop
        if (buf.length - off < size) break;                       // wait for more
        try { onPacket(decodePacket(dv, off, size)); } catch (e) { /* skip bad packet */ }
        off += size;
      }
      buf = off > 0 ? buf.slice(off) : buf;
    };
  }

  // ---- encoders (C2S) ----
  function hdr(size, type){ const b=new ArrayBuffer(size); const d=new DataView(b); d.setUint16(0,size,true); d.setUint16(2,type,true); return {b,d}; }
  const enc = {
    heartbeat(){ return hdr(4, MSG.C2S_HEARTBEAT).b; },
    // C2S_ENTER_GAME: header(4) + int64 accountId + char ticket[64] = 76B.
    //   티켓은 아직 안 쓴다 — ArrayBuffer 가 0 으로 채워져 나오므로 그대로 둔다.
    //   accountId 0 = "번호를 지정하지 않음" → 서버가 새 번호를 붙인다(새로고침마다 다른 캐릭터).
    enterGame(accountId){ const {b,d}=hdr(76,MSG.C2S_ENTER_GAME); d.setBigInt64(4, BigInt(accountId||0), true); return b; },
    // 바디 없음 — 방향은 서버가 들고 있고, 대상도 서버가 "앞 한 칸"으로 고른다
    attack(){ return hdr(4, MSG.C2S_ATTACK).b; },
    // C2S_ADMIN_LOGIN: header(4) + char key[64] = 68B. 키는 ASCII — 마지막 한 바이트는 NUL 자리로 남긴다
    adminLogin(key){ const {b,d}=hdr(68,MSG.C2S_ADMIN_LOGIN); const s=String(key||'').slice(0,63);
      for(let i=0;i<s.length;i++) d.setUint8(4+i, s.charCodeAt(i) & 0x7F); return b; },
    // C2S_USE_ITEM: header(4) + slot u16 = 6B
    useItem(slot){ const {b,d}=hdr(6,MSG.C2S_USE_ITEM); d.setUint16(4, slot, true); return b; },
    // C2S_PICKUP: 바디 없음 — 서버가 2칸 안에서 주울 수 있는 가장 가까운 것 하나를 고른다
    pickup(){ return hdr(4, MSG.C2S_PICKUP).b; },
    // C2S_EQUIP: header(4) + slot u16 = 6B — 가방 칸(장비 탭)
    equip(slot){ const {b,d}=hdr(6,MSG.C2S_EQUIP); d.setUint16(4, slot, true); return b; },
    // C2S_UNEQUIP: header(4) + slot u16 = 6B — 착용 칸(EQUIP_SLOT)
    unequip(slot){ const {b,d}=hdr(6,MSG.C2S_UNEQUIP); d.setUint16(4, slot, true); return b; },
    // C2S_USE_SKILL: header(4) + skillId u8 = 5B — 방향·대상은 서버가 정한다(지금 바라보는 쪽)
    useSkill(skillId){ const {b,d}=hdr(5,MSG.C2S_USE_SKILL); d.setUint8(4, skillId); return b; },
    // C2S_PORTAL_USE: header(4) + portalId i32 = 8B — 어디로 가는지는 서버 포털 표가 정한다(클라는 번호만)
    portalUse(portalId){ const {b,d}=hdr(8,MSG.C2S_PORTAL_USE); d.setInt32(4, portalId, true); return b; },
    // ---- 파티 (USE_PARTY) ----
    // C2S_PARTY_REQUEST: header(4) + targetId i32 = 8B — 화면에서 누른 캐릭터. 무엇이 갈지는 서버가 가린다
    partyRequest(targetId){ const {b,d}=hdr(8,MSG.C2S_PARTY_REQUEST); d.setInt32(4, targetId, true); return b; },
    // C2S_PARTY_ANSWER: header(4) + requestId u32 + accept u8 = 9B
    partyAnswer(requestId, accept){ const {b,d}=hdr(9,MSG.C2S_PARTY_ANSWER); d.setUint32(4, requestId>>>0, true); d.setUint8(8, accept?1:0); return b; },
    // C2S_PARTY_LEAVE · DISBAND: 몸 없음 / KICK · LEADER: header(4) + targetId i32 = 8B (파티장만)
    partyLeave(){ return hdr(4, MSG.C2S_PARTY_LEAVE).b; },
    partyKick(targetId){ const {b,d}=hdr(8,MSG.C2S_PARTY_KICK); d.setInt32(4, targetId, true); return b; },
    partyLeader(targetId){ const {b,d}=hdr(8,MSG.C2S_PARTY_LEADER); d.setInt32(4, targetId, true); return b; },
    partyDisband(){ return hdr(4, MSG.C2S_PARTY_DISBAND).b; },
    moveStart(dir,x,y){ const {b,d}=hdr(13,MSG.C2S_MOVE_START); d.setUint8(4,dir); d.setFloat32(5,x,true); d.setFloat32(9,y,true); return b; },
    moveStop(dir,x,y){ const {b,d}=hdr(13,MSG.C2S_MOVE_STOP); d.setUint8(4,dir); d.setFloat32(5,x,true); d.setFloat32(9,y,true); return b; },
    // C2S_CHAT: header + wchar_t[] UTF-16LE, 널 종단 포함, (len+1)*2 바이트만 전송 (서버가 header.size로 역산)
    chat(str){
      const s = (str||'').slice(0,511);            // 버퍼 512(널포함) 안으로
      const size = 4 + (s.length+1)*2;
      const {b,d} = hdr(size, MSG.C2S_CHAT);
      let o=4; for(let i=0;i<s.length;i++){ d.setUint16(o, s.charCodeAt(i), true); o+=2; }
      d.setUint16(o, 0, true);                       // NUL terminator
      return b;
    }
  };

  const API = { MSG, NAME, DIR, MOVE, INV_TAB_RANGES, EQUIP_SLOT, USE_FAIL, EQUIP_FAIL, SKILL_FAIL, BUFF_KIND, ZONE_FAIL, GROUND_APPEAR, GROUND_GONE, PICKUP_FAIL,
                PARTY_ASK_KIND, PARTY_NOTICE, PARTY_MAX_MEMBERS, decodePacket, Reassembler, enc };
  if (typeof module !== 'undefined' && module.exports) module.exports = API;
  else global.Proto = API;
})(typeof window !== 'undefined' ? window : globalThis);
