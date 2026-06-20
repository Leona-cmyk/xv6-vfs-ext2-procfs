
#ifndef _BUF_H
#define _BUF_H

#include "types.h"
#include "sleeplock.h"  // 添加这行！

// 定义块大小
#define BSIZE 1024

struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  struct buf *prev; // LRU cache list
  struct buf *next;
  uchar data[BSIZE];
};

#endif
