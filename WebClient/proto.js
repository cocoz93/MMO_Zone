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
    C2S_PICKUP:1036, S2C_PICKUP_FAIL:1037
  };
  // 아이템 쓰기 거절 사유 — Protocol.h UseItemFail 순서 그대로(값이 와이어에 실린다). 서버가 이 순서로 보고 먼저 걸린 하나를 보낸다
  const USE_FAIL = ['BAD_SLOT','EMPTY','NOT_USABLE','DEAD','COOLDOWN','FULL_HP'];
  // 땅의 아이템 — Protocol.h GroundItemAppear · GroundItemGone · PickupFail 순서 그대로(값이 와이어에 실린다)
  const GROUND_APPEAR = ['SEEN','DROP'];
  const GROUND_GONE   = ['OUT_OF_SIGHT','PICKED','EXPIRED'];
  const PICKUP_FAIL   = ['DEAD','NO_ITEM','NOT_OWNER','TAB_FULL'];
  // 가방 탭 구간 — Protocol.h INV_TAB_RANGES 사본(InvTab 순서: 장비·소비·기타). 칸 번호가 곧 DB 열쇠라
  //   한쪽만 고치면 웹이 엉뚱한 탭에 그린다 — Protocol.h 와 나란히 볼 것.
  const INV_TAB_RANGES = [ { first:0, n:20 }, { first:100, n:20 }, { first:200, n:20 } ];
  const NAME = Object.fromEntries(Object.entries(MSG).map(([k,v])=>[v,k]));
  const DIR = { NONE:0, UP:1, DOWN:2, LEFT:3, RIGHT:4 };  // Player.h
  const MOVE = { IDLE:0, MOVING:1 };
  const POS_QUANT_INV = 1 / 512;   // SectorUpdateEntry ships coords as uint16 ticks (Protocol.h POS_QUANT_SCALE)
  const U16LE = new TextDecoder('utf-16le');
  const U8 = new TextDecoder('utf-8');

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
      case MSG.S2C_CREATE_MY_PLAYER:
        p.playerId=dv.getInt32(o,true); p.direction=dv.getUint8(o+4);
        p.displayChar=dv.getUint8(o+5); p.colorIndex=dv.getUint8(o+6);
        p.x=dv.getFloat32(o+7,true); p.y=dv.getFloat32(o+11,true); p.speed=dv.getInt32(o+15,true);
        break;
      case MSG.S2C_CREATE_OTHER_PLAYER:
        p.playerId=dv.getInt32(o,true); p.direction=dv.getUint8(o+4); p.moveState=dv.getUint8(o+5);
        p.displayChar=dv.getUint8(o+6); p.colorIndex=dv.getUint8(o+7); p.spawnReason=dv.getUint8(o+8);
        p.x=dv.getFloat32(o+9,true); p.y=dv.getFloat32(o+13,true); p.speed=dv.getInt32(o+17,true);
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
          p.entries.push({ playerId:dv.getInt32(e,true), direction:dv.getUint8(e+4), moveState:dv.getUint8(e+5),
            displayChar:dv.getUint8(e+6), colorIndex:dv.getUint8(e+7), spawnReason:dv.getUint8(e+8),
            x:dv.getFloat32(e+9,true), y:dv.getFloat32(e+13,true), speed:dv.getInt32(e+17,true) }); e += 21;
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
      case MSG.S2C_ACTOR_STATS:                  // CREATE 에 HP 자리가 없어 뒤따라 오는 것
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
      case MSG.S2C_USE_ITEM_FAIL:                // 성공은 따로 안 온다 — 칸 갱신과 S2C_ACTOR_STATS 가 곧 결과다
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
      default: break; // ZONE_CHANGE_OK/FAIL, ADMIN_* etc. — not needed for the view
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

  const API = { MSG, NAME, DIR, MOVE, INV_TAB_RANGES, USE_FAIL, GROUND_APPEAR, GROUND_GONE, PICKUP_FAIL, decodePacket, Reassembler, enc };
  if (typeof module !== 'undefined' && module.exports) module.exports = API;
  else global.Proto = API;
})(typeof window !== 'undefined' ? window : globalThis);
