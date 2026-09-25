#ifndef DIAG_H
#define DIAG_H

/* Prints the reset cause and any crash record left by the previous run,
 * then arms the hardware watchdog (2s). Call once, early in main().
 */
void diag_init(void);

/* Feed the watchdog - call from main()'s loop. */
void diag_feed(void);

#endif
