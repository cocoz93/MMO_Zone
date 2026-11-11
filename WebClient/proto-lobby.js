// 계정 서버 · 캐릭터 서버 와이어 — Shared/Protocol/AccountProtocol.h · CharacterProtocol.h 와 나란히 볼 것(#pragma pack(1), little-endian).
//   login.html · character.html 은 게임 패킷을 안 쓰므로 proto.js 와 나눈다.
(function (global) {
  "use strict";
  const MSG = {
    C2S_ACCOUNT_LOGIN:2000, S2C_ACCOUNT_LOGIN:2001,
    C2S_CHAR_LOGIN:2100, S2C_CHAR_LOGIN:2101, S2C_CHAR_LIST:2102,
    C2S_CHAR_CREATE:2103, S2C_CHAR_CREATE:2104,
    C2S_CHAR_DELETE:2105, S2C_CHAR_DELETE:2106,
    C2S_CHAR_RESTORE:2107, S2C_CHAR_RESTORE:2108,
    C2S_CHAR_SELECT:2109, S2C_CHAR_SELECT:2110,
    C2S_CHAR_HEARTBEAT:2111,
  };
  // 결과 값 — 서버 enum 과 같은 순서. 지표 라벨과 같은 이름을 쓴다
  const ACCOUNT_LOGIN = ['OK','BAD_FORM','LOGIN_DISABLED','BUSY','DB_FAIL'];
  const CHAR_LOGIN = ['OK','BAD_TOKEN','EXPIRED','SERVER_DOWN','BUSY','DB_FAIL'];
  const CHAR_CREATE = ['OK','TAKEN','BAD_FORM','BAD_JOB','BAD_LOOK','SLOT_TAKEN','BUSY','DB_FAIL'];
  const CHAR_DELETE = ['OK','NOT_MINE','IN_GAME','BUSY','DB_FAIL'];
  const CHAR_RESTORE = ['OK','NOT_MINE','EXPIRED','BUSY','DB_FAIL'];
  const CHAR_SELECT = ['OK','NOT_MINE','DELETED','ZONE_DOWN','ZONE_FULL','BUSY','DB_FAIL','DUPLICATE'];

  const LOGIN_NAME_MIN = 4, LOGIN_NAME_MAX = 16, LOGIN_NAME_BYTES = 17;
  const CODE_BYTES = 64, WS_ADDR_BYTES = 64, LOOK_BYTES = 32;
  const NAME_MIN = 2, NAME_MAX = 6;
  const LOOK_VERSION = 1, GENDER_MALE = 1, GENDER_FEMALE = 2;
  // 계정당 칸 수 — 서버 CharacterSlots 와 같아야 한다. 서버가 패킷으로 알려 주지 않는 값이다(바꿀 때 같이 바꿀 것)
  const SLOT_COUNT = 4;
  const LIST_ENTRY_BYTES = 65;

  // 탭 저장소 — 탭을 닫으면 사라진다. 입장 번호를 주소에 넣으면 방문 기록에 남으므로 여기로 넘긴다
  const STORE = { loginName:'mmo.loginName', authToken:'mmo.authToken', enterCode:'mmo.enterCode', zoneWs:'mmo.zoneWs', notice:'mmo.notice' };

  function hdr(size, type) {
    const b = new ArrayBuffer(size), d = new DataView(b);
    d.setUint16(0, size, true); d.setUint16(2, type, true);
    return { b, d, u: new Uint8Array(b) };
  }
  const toHex = bytes => Array.from(bytes, v => v.toString(16).padStart(2, '0')).join('');
  const fromHex = hex => { const out = new Uint8Array(hex.length >> 1); for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.substr(i * 2, 2), 16); return out; };

  // 글자 규칙 — 서버가 다시 본다. 여기서는 바로 안내하려고 본다(Shared/Identity/CharacterName.h · AccountRules)
  const isValidLoginName = s => /^[0-9A-Za-z]+$/.test(s) && s.length >= LOGIN_NAME_MIN && s.length <= LOGIN_NAME_MAX;
  const isValidCharName = s => /^[0-9A-Za-z가-힣]+$/.test(s) && s.length >= NAME_MIN && s.length <= NAME_MAX;

  const enc = {
    accountLogin(name) { const { b, u } = hdr(4 + LOGIN_NAME_BYTES, MSG.C2S_ACCOUNT_LOGIN); for (let i = 0; i < name.length && i < LOGIN_NAME_MAX; i++) u[4 + i] = name.charCodeAt(i) & 0x7F; return b; },
    charLogin(tokenBytes) { const { b, u } = hdr(4 + CODE_BYTES, MSG.C2S_CHAR_LOGIN); u.set(tokenBytes.subarray(0, CODE_BYTES), 4); return b; },
    charCreate({ slot, name, job, look }) {
      const { b, d, u } = hdr(4 + 1 + NAME_MAX * 2 + 4 + LOOK_BYTES, MSG.C2S_CHAR_CREATE);
      d.setInt8(4, slot);
      for (let i = 0; i < NAME_MAX && i < name.length; i++) d.setUint16(5 + i * 2, name.charCodeAt(i), true);
      d.setInt32(17, job, true);
      u.set(lookBytes(look), 21);
      return b;
    },
    charDelete(id) { return idPacket(MSG.C2S_CHAR_DELETE, id); },
    charRestore(id) { return idPacket(MSG.C2S_CHAR_RESTORE, id); },
    charSelect(id) { return idPacket(MSG.C2S_CHAR_SELECT, id); },
    heartbeat() { return hdr(4, MSG.C2S_CHAR_HEARTBEAT).b; },
  };
  function idPacket(type, id) { const { b, d } = hdr(12, type); d.setBigInt64(4, BigInt(id), true); return b; }

  // 외형 칸 — Shared/Identity/CharacterLookBlob.h 판 1 과 같은 자리
  function lookBytes(l) { const out = new Uint8Array(LOOK_BYTES); out[0] = LOOK_VERSION; out[1] = l.gender; out[2] = l.skin; out[3] = l.hair; out[4] = l.top; out[5] = l.bottom; return out; }
  function readLook(bytes) { return bytes[0] === LOOK_VERSION ? { gender: bytes[1], skin: bytes[2], hair: bytes[3], top: bytes[4], bottom: bytes[5] } : null; }

  function readText(u8) { let n = 0; while (n < u8.length && u8[n] !== 0) n++; return String.fromCharCode.apply(null, u8.subarray(0, n)); }

  function decode(dv, base, size) {
    const type = dv.getUint16(base + 2, true), o = base + 4;
    const u8 = new Uint8Array(dv.buffer, dv.byteOffset + base, size);
    switch (type) {
      case MSG.S2C_ACCOUNT_LOGIN: return { type, result: ACCOUNT_LOGIN[dv.getUint8(o)], token: u8.slice(5, 5 + CODE_BYTES) };
      case MSG.S2C_CHAR_LOGIN: return { type, result: CHAR_LOGIN[dv.getUint8(o)] };
      case MSG.S2C_CHAR_LIST: {
        const count = dv.getUint16(o, true), rows = [];
        for (let i = 0; i < count; i++) {
          const r = o + 2 + i * LIST_ENTRY_BYTES;
          let name = '';
          for (let k = 0; k < NAME_MAX; k++) { const c = dv.getUint16(r + 9 + k * 2, true); if (c === 0) break; name += String.fromCharCode(c); }
          rows.push({ id: dv.getBigInt64(r, true).toString(), slot: dv.getInt8(r + 8), name, job: dv.getInt32(r + 21, true), level: dv.getInt32(r + 25, true),
                      look: readLook(u8.subarray(r - base + 29, r - base + 61)), deleteLeftSec: dv.getInt32(r + 61, true) });
        }
        return { type, rows };
      }
      case MSG.S2C_CHAR_CREATE: return { type, result: CHAR_CREATE[dv.getUint8(o)], characterId: dv.getBigInt64(o + 1, true).toString() };
      case MSG.S2C_CHAR_DELETE: return { type, result: CHAR_DELETE[dv.getUint8(o)] };
      case MSG.S2C_CHAR_RESTORE: return { type, result: CHAR_RESTORE[dv.getUint8(o)] };
      case MSG.S2C_CHAR_SELECT: return { type, result: CHAR_SELECT[dv.getUint8(o)], enterCode: u8.slice(5, 5 + CODE_BYTES), zoneWs: readText(u8.subarray(5 + CODE_BYTES, 5 + CODE_BYTES + WS_ADDR_BYTES)) };
      default: return { type };
    }
  }

  // 패킷 묶기 — TCP 조각이 그대로 온다
  class Reassembler {
    constructor(onPacket) { this.onPacket = onPacket; this.buf = new Uint8Array(0); }
    push(chunk) {
      const joined = new Uint8Array(this.buf.length + chunk.length); joined.set(this.buf); joined.set(chunk, this.buf.length);
      const dv = new DataView(joined.buffer);
      let off = 0;
      while (joined.length - off >= 4) {
        const size = dv.getUint16(off, true);
        if (size < 4 || joined.length - off < size) break;
        this.onPacket(decode(dv, off, size));
        off += size;
      }
      this.buf = joined.slice(off);
    }
  }

  // 계정 서버에서 토큰을 받는다. 서버가 회신 뒤 곧 끊으므로 연결을 들고 있지 않는다
  //   답 = { result:'OK'|…|'NO_SERVER', token:hex }
  function fetchToken(url, name) {
    return new Promise(res => {
      let done = false;
      const fin = out => { if (done) return; done = true; clearTimeout(timer); try { ws.close(); } catch (e) { } res(out); };
      const timer = setTimeout(() => fin({ result: 'NO_SERVER' }), 5000);
      const ws = new WebSocket(url);
      ws.binaryType = 'arraybuffer';
      const re = new Reassembler(p => { if (p.type === MSG.S2C_ACCOUNT_LOGIN) fin({ result: p.result, token: toHex(p.token) }); });
      ws.onopen = () => ws.send(enc.accountLogin(name));
      ws.onmessage = ev => re.push(new Uint8Array(ev.data));
      ws.onerror = () => fin({ result: 'NO_SERVER' });
      ws.onclose = () => fin({ result: 'NO_SERVER' });
    });
  }

  // 외형 표(/data/LookTable.csv) — 서버 파서와 같은 규칙: 빈 줄 · '#' 주석을 건너뛴다
  //   out[gender][part] = [{ id, sprite, head }] (번호 순서)
  function parseLookTable(text) {
    const out = { 1: { skin: [], hair: [], top: [], bottom: [] }, 2: { skin: [], hair: [], top: [], bottom: [] } };
    for (const raw of text.split(/\r?\n/)) {
      const line = raw.trim();
      if (!line || line[0] === '#') continue;
      const [gender, part, id, sprite, head] = line.split(',');
      if (out[gender] && out[gender][part]) out[gender][part].push({ id: Number(id), sprite, head: head || '' });
    }
    for (const g of [1, 2]) for (const part of Object.keys(out[g])) out[g][part].sort((a, b) => a.id - b.id);
    return out;
  }
  // 그릴 부위 이름 — 아래부터 위로. live.html 의 z순서와 같다: 몸 → 머리 → 바지 → 상의 → 머리카락. 표에 없는 번호면 null
  function lookLayers(table, look) {
    if (!table || !look || !table[look.gender]) return null;
    const t = table[look.gender], find = (part, id) => t[part].find(e => e.id === id);
    const skin = find('skin', look.skin), hair = find('hair', look.hair), top = find('top', look.top), bottom = find('bottom', look.bottom);
    return (skin && hair && top && bottom) ? [skin.sprite, skin.head, bottom.sprite, top.sprite, hair.sprite] : null;
  }

  global.LobbyProto = { MSG, STORE, SLOT_COUNT, GENDER_MALE, GENDER_FEMALE, NAME_MAX, LOGIN_NAME_MAX, enc, Reassembler, fetchToken, toHex, fromHex,
                        isValidLoginName, isValidCharName, parseLookTable, lookLayers };
})(typeof window !== 'undefined' ? window : globalThis);
