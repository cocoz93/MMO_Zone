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
    S2C_EXP:1029
  };
  const NAME = Object.fromEntries(Object.entries(MSG).map(([k,v])=>[v,k]));
  const DIR = { NONE:0, UP:1, DOWN:2, LEFT:3, RIGHT:4 };  // Player.h
  const MOVE = { IDLE:0, MOVING:1 };
  const POS_QUANT_INV = 1 / 512;   // SectorUpdateEntry ships coords as uint16 ticks (Protocol.h POS_QUANT_SCALE)
  const U16LE = new TextDecoder('utf-16le');

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
        const sub = new Uint8Array(dv.buffer, dv.byteOffset + o, size - 4);
        p.message = new TextDecoder('ascii').decode(sub).replace(/\u0000+$/,''); break;
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

  const API = { MSG, NAME, DIR, MOVE, decodePacket, Reassembler, enc };
  if (typeof module !== 'undefined' && module.exports) module.exports = API;
  else global.Proto = API;
})(typeof window !== 'undefined' ? window : globalThis);
