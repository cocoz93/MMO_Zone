'use strict';
// 캐릭터 부위 표 — 웹이 쓰는 부위 이름(live.html MBODY·FHAIR… · NPC_LOOK) → LPC 생성기 원본 폴더(spritesheets/ 아래).
//   원본 폴더마다 동작별 그림이 한 장씩 있다(walk.png · slash.png …). 부위 이름의 색(amber·red …)은 원본에 없다 —
//   원본은 색 없는 기본 한 벌이고, 색은 palettes.json(원본 색 → 우리 색)이 입힌다.
//   대응은 10-29 에 옛 걷기 그림(assets/lpc_layers.js)의 투명 자리 모양을 원본 후보 300벌과 겹쳐 골랐다 —
//   57장 중 44장은 모양 100% · 색 1:1, 13장(b_skeleton · h_long · hf_long · h_parted · h_plain · lf_* · tf_*)은 원본이 그 뒤 고쳐져 조금 어긋난다.
//   b_musc 는 옛 그림에 있었지만 쓰는 곳이 없어 뺐다(10-30).
//   값이 배열이면 원본 여러 장을 앞에서부터 차례로 겹친 한 장이다(뒤 → 앞). 겹치기는 layerImage 한 곳.
module.exports.LAYERS = {
  b_amber:      'body/bodies/male',
  b_bronze:     'body/bodies/male',
  b_brown:      'body/bodies/male',
  b_fem:        'body/bodies/female',
  b_green:      'body/bodies/male',
  b_light:      'body/bodies/male',
  b_olive:      'body/bodies/male',
  b_skeleton:   ['body/bodies/skeleton', 'head/heads/skeleton/adult'],   // 옛 그림은 해골 머리까지 한 장 — 지금 원본은 몸·머리가 나뉘어 몸만 쓰면 머리가 없다(10-30 사진)
  b_zombie:     'body/bodies/male',
  bf_amber:     'body/bodies/female',
  bf_bronze:    'body/bodies/female',
  bf_olive:     'body/bodies/female',
  h_afro:       'hair/afro/adult',
  h_bedhead:    'hair/bedhead/adult',
  h_long:       'hair/long/adult',
  h_messy1:     'hair/messy1/adult',
  h_mop:        'hair/mop/adult',
  h_parted:     'hair/parted/adult',
  h_plain:      'hair/plain/adult',
  h_spiked:     'hair/spiked/adult',
  hd_amber:     'head/heads/human/male',
  hd_bronze:    'head/heads/human/male',
  hd_brown:     'head/heads/human/male',
  hd_f_amber:   'head/heads/human/female',
  hd_f_bronze:  'head/heads/human/female',
  hd_f_light:   'head/heads/human/female',
  hd_f_olive:   'head/heads/human/female',
  hd_green:     'head/heads/human/male',
  hd_light:     'head/heads/human/male',
  hd_olive:     'head/heads/human/male',
  hd_zombie:    'head/heads/human/male',
  hf_bangslong: 'hair/bangslong/adult',
  hf_braid:     'hair/braid/adult/fg',
  hf_long:      'hair/long/adult',
  hf_pixie:     'hair/pixie/adult',
  hf_ponytail:  ['hair/ponytail/adult/bg', 'hair/ponytail/adult/fg'],   // 옛 그림이 뒤(bg)·앞(fg) 두 장을 겹친 한 장이었다 — 겹침 99% vs 앞만 92%(10-30)
  l_blue:       'legs/pants/male',
  l_brown:      'legs/pants/male',
  l_charcoal:   'legs/pants/male',
  l_leather:    'legs/pants/male',
  l_navy:       'legs/pants/male',
  l_teal:       'legs/pants/male',
  lf_brown:     'legs/pants/thin',
  lf_navy:      'legs/pants/thin',
  lf_rose:      'legs/pants/thin',
  lf_teal:      'legs/pants/thin',
  t_forest:     'torso/clothes/longsleeve/longsleeve/male',
  t_purple:     'torso/clothes/longsleeve/longsleeve/male',
  t_red:        'torso/clothes/longsleeve/longsleeve/male',
  t_slate:      'torso/clothes/longsleeve/longsleeve/male',
  t_teal:       'torso/clothes/longsleeve/longsleeve/male',
  t_yellow:     'torso/clothes/longsleeve/longsleeve/male',
  tf_navy:      'torso/clothes/longsleeve/longsleeve/female',
  tf_purple:    'torso/clothes/longsleeve/longsleeve/female',
  tf_rose:      'torso/clothes/longsleeve/longsleeve/female',
  tf_teal:      'torso/clothes/longsleeve/longsleeve/female',
};

// 부위 하나의 원본 폴더들(늘 배열) · 그 동작의 원본을 차례로 겹친 그림 — make-palettes.js 와 build.js 가 같이 쓴다
const dirsOf = v => Array.isArray(v) ? v : [v];
module.exports.dirsOf = dirsOf;
module.exports.layerImage = (PNG, srcDir, v, anim) => {
  const fs = require('fs'), path = require('path');
  let img = null;
  for (const d of dirsOf(v)) {
    const im = PNG.decode(fs.readFileSync(path.join(srcDir, d, anim + '.png')));
    if (!img) { img = im; continue; }
    if (im.w !== img.w || im.h !== img.h) throw new Error('겹칠 그림의 크기가 다르다: ' + d);
    for (let i = 0; i < img.data.length; i += 4) if (im.data[i + 3] > 0) im.data.copy(img.data, i, i, i + 4);   // 원본은 알파가 0 아니면 거의 255 — 앞 장이 덮는다
  }
  return img;
};

// 동작 — 원본 파일 이름. 걷기 9장 · 베기 6장 · 숨쉬기 2장 × 4방향(위·왼·아래·오른 줄), 한 칸 64px
//   쓰러짐(hurt)은 원본이 정면 한 줄 6장뿐이다 — 어느 방향을 보다 죽어도 정면으로 엎어진다
module.exports.ANIMS = ['walk', 'slash', 'idle', 'hurt'];

// 무기 — 겉모습 이름(ItemTable.csv look 칸) → 원본 그림. front = 몸 앞 · behind = 몸 뒤(원본 폴더 behind/ — 위를 볼 때 칼이 몸에 가린다)
//   뱀왕의 지팡이(gnarled)는 원본에 베기가 없어 안 넣는다(플랜 10-29 결정 · 찌르기만 있음)
module.exports.WEAPONS = {
  dagger: {
    walk:  { front: 'weapon/sword/dagger/walk/dagger.png',  behind: 'weapon/sword/dagger/behind/walk/dagger.png' },
    slash: { front: 'weapon/sword/dagger/slash/dagger.png', behind: 'weapon/sword/dagger/behind/slash/dagger.png' },
    hurt:  { front: 'weapon/sword/dagger/hurt/dagger.png' },   // 원본에 뒤 몫이 없다(정면뿐이라 칼이 몸에 가릴 일이 없다) — build.js 가 빈 그림으로 채운다
  },
};

// 원본에 없는 무기 동작 — 다른 동작의 한 장을 그 동작의 모든 칸에 그대로 쓴다(build.js).
//   숨쉬기: 원본 단검에 숨쉬기가 없다. 숨쉬기 두 장은 상체만 움직이고 칼 쥔 손 자리는 서 있는 장(걷기 0번)과 같아서
//   (10-30 칼 칸 둘레 대조 — 남자 옆·정면 0px · 여자 옆만 1px 위 · 위를 볼 땐 칼이 안 보임) 칼은 서 있는 장 그대로 둔다
module.exports.WEAPON_DERIVED = {
  idle: { from: 'walk', frame: 0 },
};
