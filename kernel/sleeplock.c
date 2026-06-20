// Sleeping locks

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"

void
initsleeplock(struct sleeplock *lk, char *name)
{
  initlock(&lk->lk, "sleep lock");
  lk->name = name;
  lk->locked = 0;
  lk->pid = 0;
}

void
acquiresleep(struct sleeplock *lk)
{
  acquire(&lk->lk);
  while (lk->locked) {
    struct proc *p = myproc();
    if(p == 0) {
      // 在没有进程上下文的初始化阶段，如果锁已被持有，可能是初始化问题
      // 尝试等待一小段时间后重试，或者直接 panic
      release(&lk->lk);
      printf("acquiresleep: no proc context, lock=%d, spinning...\n", lk->locked);
      for(volatile int i = 0; i < 1000000; i++);
      acquire(&lk->lk);
      if(lk->locked) {
        panic("acquiresleep: cannot acquire sleep lock during kernel init");
      }
      break;
    }
    sleep(lk, &lk->lk);
  }
  lk->locked = 1;
  struct proc *p = myproc();
  if(p != 0)
    lk->pid = p->pid;
  else
    lk->pid = -1;
  release(&lk->lk);
}

void
releasesleep(struct sleeplock *lk)
{
  acquire(&lk->lk);
  lk->locked = 0;
  lk->pid = 0;
  wakeup(lk);
  release(&lk->lk);
}

int
holdingsleep(struct sleeplock *lk)
{
  int r;
  
  acquire(&lk->lk);
  struct proc *p = myproc();
  r = lk->locked && p && (lk->pid == p->pid);
  release(&lk->lk);
  return r;
}



