/* License-free PowerPC test input: a dense switch that the compiler should lower to a jump table. */
typedef unsigned int u32;
__attribute__((noinline)) u32 dispatch(u32 op, u32 a, u32 b) {
  switch (op) {
    case 0: return a + b;      case 1: return a - b;      case 2: return a * b;
    case 3: return b ? a / b : 0; case 4: return a & b;   case 5: return a | b;
    case 6: return a ^ b;      case 7: return a << (b & 31); case 8: return a >> (b & 31);
    case 9: return ~a;         case 10: return a > b;     case 11: return a < b;
    case 12: return a == b;    case 13: return (a + b) * 3; case 14: return a * a + b * b;
    case 15: return a % (b | 1);
    default: return 0xFFFFFFFFu;
  }
}
static u32 g_result[16];
void _start(void) {
  for (u32 i = 0; i < 16; i++) g_result[i] = dispatch(i, 7, 3);
  for (;;) {}
}
