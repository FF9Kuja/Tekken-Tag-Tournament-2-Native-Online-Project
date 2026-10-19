/* License-free PowerPC test input for recompiler smoke tests (no game code).
   Exercises integer, loop, call, switch (jump table), float and memory ops. */
typedef unsigned int u32;
__attribute__((noinline)) u32 mul_add(u32 a, u32 b, u32 c) { return a * b + c; }
__attribute__((noinline)) u32 sum_array(const u32* p, u32 n) { u32 s = 0; for (u32 i = 0; i < n; i++) s += p[i]; return s; }
__attribute__((noinline)) u32 classify(u32 x) {
  switch (x) {
    case 0: return 10; case 1: return 20; case 2: return 30; case 3: return 40;
    case 4: return 50; case 5: return 60; case 6: return 70; case 7: return 80;
    default: return 0;
  }
}
__attribute__((noinline)) float lerp(float a, float b, float t) { return a + (b - a) * t; }
__attribute__((noinline)) void fill(u32* p, u32 n, u32 v) { for (u32 i = 0; i < n; i++) p[i] = v; }
static u32 g_buf[16];
void _start(void) {
  fill(g_buf, 16, 3);
  volatile u32 r = mul_add(sum_array(g_buf, 16), classify(5), 7);
  volatile float f = lerp(1.0f, 2.0f, 0.5f);
  (void)r; (void)f;
  for (;;) {}
}
