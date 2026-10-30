// tools/lpc/build.js 가 만든다(손으로 고치지 말 것) — anim.json 과 같은 값. 웹은 file:// 에서도 읽히게 <script> 로 받는다
window.LPC_ANIM = {
 "frame": 64,
 "rows": [
  "up",
  "left",
  "down",
  "right"
 ],
 "walk": {
  "cols": 9,
  "idle": 0,
  "loop": [
   1,
   2,
   3,
   4,
   5,
   6,
   7,
   8
  ],
  "frameMs": 125
 },
 "slash": {
  "cols": 6,
  "frames": [
   0,
   1,
   2,
   3,
   4,
   5
  ],
  "frameMs": 60
 },
 "idle": {
  "cols": 2,
  "loop": [
   0,
   0,
   1
  ],
  "frameMs": 125
 },
 "weapons": [
  "dagger"
 ]
};
