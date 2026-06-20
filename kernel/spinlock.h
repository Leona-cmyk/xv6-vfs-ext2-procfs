
#ifndef _SPINLOCK_H
#define _SPINLOCK_H

#include "types.h"

struct cpu;

// Mutual exclusion lock.
struct spinlock {
  uint locked;       // Is the lock held?

  // For debugging:
  char *name;        // Name of lock.
  struct cpu *cpu;   // The cpu holding the lock.
};

void acquire(struct spinlock*);
void release(struct spinlock*);
void initlock(struct spinlock*, char*);
int holding(struct spinlock*);
void push_off(void);
void pop_off(void);

#endif
