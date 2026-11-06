/* Stage 0 throwaway probe. Answers "what does the Renode model really do?".
 * Not product code: raw addresses, no layering. NODE_ID and USE_PLL come from build.sh. */
#include <stdint.h>
#define R(a) (*(volatile uint32_t *)(a))
#define RCC 0x58024400u
#define PWR 0x58024800u
#define U1  0x40011000u
#define U3  0x40004800u
#define CAN 0x4000A000u
#define MRAM 0x4000AC00u
#define NVIC_ISER(n) R(0xE000E100u + 4u*(n))
#define RX_OFF 0x000u
#define TX_OFF 0x200u
#define ELEM 40u

extern uint32_t _estack, _etext, _sdata, _edata, _sbss, _ebss;
static volatile uint32_t ms, rx_total, rx_from[5], rx_fd, rx_brs, u1_irqs;
static volatile uint32_t last_seq[5], gap_max[5], last_rx_ms[5], seq_lost;
static char txq[1024]; static volatile uint32_t qh, qt;
static uint32_t tx_seq, tx_full, sysclk_hz;

static void q(char c) { uint32_t n = (qh + 1u) & 1023u; if (n != qt) { txq[qh] = c; qh = n; } }
static void qs(const char *s) { while (*s) q(*s++); }
static void qu(uint32_t v) { char b[11]; int i = 0; do { b[i++] = (char)('0' + v % 10u); v /= 10u; } while (v); while (i) q(b[--i]); }
static void qx(uint32_t v) { for (int i = 28; i >= 0; i -= 4) q("0123456789ABCDEF"[(v >> i) & 15u]); }
static void flush(void) { R(U1 + 0x00) |= 1u << 7; } /* TXEIE */

void usart1_isr(void) {
  u1_irqs++;
  if (qt != qh) { R(U1 + 0x28) = (uint8_t)txq[qt]; qt = (qt + 1u) & 1023u; }
  else R(U1 + 0x00) &= ~(1u << 7);
}
void systick_isr(void) { ms++; }
void fdcan_isr(void) {
  R(CAN + 0x50) = 1u; /* clear RF0N */
  while (R(CAN + 0xA4) & 0x7Fu) {
    uint32_t gi = (R(CAN + 0xA4) >> 8) & 0x3Fu, a = MRAM + RX_OFF + gi * ELEM;
    uint32_t w1 = R(a + 4), src = R(a + 8) & 0xFFu, seq = R(a + 12);
    if (src >= 1u && src <= 4u) {
      if (last_rx_ms[src] != 0u) { uint32_t g = ms - last_rx_ms[src]; if (g > gap_max[src]) gap_max[src] = g; }
      if (last_seq[src] != 0u && seq != last_seq[src] + 1u) seq_lost++;
      last_rx_ms[src] = ms; last_seq[src] = seq; rx_from[src]++;
    }
    rx_total++; if (w1 & (1u << 21)) rx_fd++; if (w1 & (1u << 20)) rx_brs++;
    R(CAN + 0xA8) = gi;
  }
}
static void can_send(void) {
  uint32_t fqs = R(CAN + 0xC4);
  if (fqs & (1u << 21)) { tx_full++; return; }
  uint32_t pi = (fqs >> 16) & 0x1Fu, a = MRAM + TX_OFF + pi * ELEM;
  R(a) = (0x020u + NODE_ID) << 18;
  R(a + 4) = (13u << 16) | (1u << 20) | (1u << 21); /* DLC13=32B, BRS, FDF */
  R(a + 8) = NODE_ID; R(a + 12) = ++tx_seq;
  for (uint32_t i = 16; i < 40; i += 4) R(a + i) = 0;
  R(CAN + 0xD0) = 1u << pi;
}
static uint32_t wait_set(uint32_t addr, uint32_t mask, uint32_t val) {
  for (uint32_t i = 0; i < 100000u; i++) if ((R(addr) & mask) == val) return 1u;
  return 0u;
}
static void can_init(void) {
  R(RCC + 0xEC) |= 1u << 8;                     /* FDCANEN */
  R(CAN + 0x18) |= 1u; wait_set(CAN + 0x18, 1u, 1u);
  R(CAN + 0x18) |= 2u;                          /* CCE */
  R(CAN + 0x18) |= (1u << 8) | (1u << 9);       /* FDOE BRSE */
  R(CAN + 0x1C) = (15u << 25) | (1u << 16) | (62u << 8) | 15u;            /* NBTP */
  R(CAN + 0x0C) = (1u << 23) | (1u << 16) | (13u << 8) | (4u << 4) | 4u;  /* DBTP */
  R(CAN + 0x48) = (15u << 8) | 2u;              /* TDCR */
  R(CAN + 0x80) = 0u;                           /* GFC: non-matching -> FIFO0 */
  R(CAN + 0xA0) = (8u << 16) | RX_OFF;          /* RXF0C: 8 elements */
  R(CAN + 0xBC) = 5u;                           /* RXESC: 32 B */
  R(CAN + 0xC0) = (4u << 24) | TX_OFF;          /* TXBC: 4-deep FIFO */
  R(CAN + 0xC8) = 5u;                           /* TXESC: 32 B */
  R(CAN + 0x54) = 1u; R(CAN + 0x5C) = 1u;       /* IE RF0N, ILE EINT0 */
  NVIC_ISER(0) = 1u << 19;
  R(CAN + 0x18) &= ~3u;
}
static void clock_init(void) {
#if USE_PLL
  uint32_t ok = wait_set(PWR + 0x18, 1u << 13, 1u << 13);
  R(RCC + 0x28) = (4u << 4) | (4u << 12) | 0u;  /* HSI, DIVM1=4, DIVM2=4 */
  R(RCC + 0x30) = (49u << 0) | (1u << 9);       /* 16MHz*50/2 = 400 MHz */
  R(RCC + 0x38) = (19u << 0) | (3u << 9) | (3u << 16);   /* PLL2: 16*20/4 = 80 MHz P and Q */
  R(RCC + 0x00) |= (1u << 24) | (1u << 26);
  uint32_t p1 = wait_set(RCC, 1u << 25, 1u << 25), p2 = wait_set(RCC, 1u << 27, 1u << 27);
  R(RCC + 0x10) = (R(RCC + 0x10) & ~7u) | 3u;
  uint32_t sw = wait_set(RCC + 0x10, 7u << 3, 3u << 3);
  sysclk_hz = 400000000u; qs("clk PLL vos="); qu(ok); qs(" pll1="); qu(p1); qs(" pll2="); qu(p2); qs(" sws="); qu(sw); q('\n');
#else
  sysclk_hz = 64000000u; qs("clk HSI\n");
#endif
}
void reset_handler(void) {
  uint32_t *s = &_etext, *d = &_sdata; while (d < &_edata) *d++ = *s++;
  for (d = &_sbss; d < &_ebss; ) *d++ = 0;
  R(0xE000ED08u) = 0x08000000u;
  R(RCC + 0xF0) |= 1u << 4;  R(RCC + 0xE8) |= 1u << 18;
  R(U1 + 0x0C) = 556u; R(U1 + 0x00) = (1u << 3) | 1u;
  R(U3 + 0x0C) = 347u; R(U3 + 0x00) = (1u << 3) | 1u;
  NVIC_ISER(1) = 1u << 5; /* USART1 = 37 */
  __asm volatile("cpsie i");
  qs("MCU"); qu(NODE_ID); qs(" boot\n");
  clock_init();
  R(0xE000E014u) = sysclk_hz / 1000u - 1u; R(0xE000E018u) = 0; R(0xE000E010u) = 7u;
  can_init();
  qs("regs NBTP="); qx(R(CAN + 0x1C)); qs(" DBTP="); qx(R(CAN + 0x0C)); qs(" TDCR="); qx(R(CAN + 0x48));
  qs(" CCCR="); qx(R(CAN + 0x18)); qs(" RCC_CFGR="); qx(R(RCC + 0x10)); q('\n'); flush();
  uint32_t t10 = 0, t100 = 0;
  for (;;) {
    uint32_t now = ms;
    if (now - t10 >= 10u) { t10 = now; can_send(); }
    if (now - t100 >= 100u) {
      t100 = now;
      qs("t="); qu(now); qs(" tx="); qu(tx_seq); qs(" full="); qu(tx_full); qs(" rx="); qu(rx_total);
      qs(" n1="); qu(rx_from[1]); qs(" n2="); qu(rx_from[2]); qs(" n3="); qu(rx_from[3]); qs(" n4="); qu(rx_from[4]);
      qs(" fd="); qu(rx_fd); qs(" brs="); qu(rx_brs); qs(" lost="); qu(seq_lost);
      qs(" gap="); qu(gap_max[1]); q('/'); qu(gap_max[2]); q('/'); qu(gap_max[3]); q('/'); qu(gap_max[4]);
      qs(" brp="); qx(R(CAN + 0xCC)); qs(" fqs="); qx(R(CAN + 0xC4)); qs(" irq="); qu(u1_irqs); q('\n'); flush();
      if (NODE_ID == 1u) for (const char *m = "UARTLINK\n"; *m; m++) { R(U3 + 0x28) = (uint8_t)*m; }
    }
  }
}
void default_isr(void) { for (;;) {} }
__attribute__((section(".isr_vector"), used)) void (*const vectors[16 + 64])(void) = {
  [0] = (void (*)(void))&_estack, [1] = reset_handler,
  [2 ... 14] = default_isr, [15] = systick_isr, [16 ... 79] = default_isr,
  [16 + 19] = fdcan_isr, [16 + 37] = usart1_isr,
};
