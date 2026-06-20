#ifndef _SLEEPLOCK_H
#define _SLEEPLOCK_H

#include "types.h"
#include "spinlock.h"

struct sleeplock {
  uint locked;
  struct spinlock lk;
  char *name;
  int pid;
};

void acquiresleep(struct sleeplock*);
void releasesleep(struct sleeplock*);
int holdingsleep(struct sleeplock*);
void initsleeplock(struct sleeplock*, char*);

#endif