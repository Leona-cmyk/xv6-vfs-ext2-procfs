//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head;
} bcache;

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  // Create linked list of buffers
  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    initsleeplock(&b->lock, "buffer");
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  acquire(&bcache.lock);

  // Is the block already cached?
  for(b = bcache.head.next; b != &bcache.head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, dev, 0);
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, b->dev, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&bcache.lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
  
  release(&bcache.lock);
}

void
bpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

void
bunpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}

// 调试函数：打印缓冲区缓存状态
void
bdebug(void)
{
  struct buf *b;
  
  acquire(&bcache.lock);
  
  printf("=== Buffer Cache Debug ===\n");
  printf("Total buffers: %d\n", NBUF);
  
  int used = 0;
  for(b = bcache.head.next; b != &bcache.head; b = b->next){
    if(b->refcnt > 0 || b->valid){
      used++;
      printf("Buf %p: dev=%d, blockno=%d, valid=%d, refcnt=%d\n",
             b, b->dev, b->blockno, b->valid, b->refcnt);
    }
  }
  
  printf("Used buffers: %d/%d\n", used, NBUF);
  release(&bcache.lock);
}

// 测试读取磁盘块
int
btest_read(int disk_id, uint blockno)
{
  // 使用设备号 = 磁盘ID的映射
  struct buf *b = bread(disk_id, blockno);
  
  if(b && b->valid) {
    printf("✓ Successfully read block %d from disk %d\n", blockno, disk_id);
    
    // 打印前32字节用于调试
    printf("First 32 bytes: ");
    for(int i = 0; i < 32; i++) {
      printf("%02x ", b->data[i] & 0xFF);
      if((i+1) % 16 == 0) printf("\n               ");
    }
    printf("\n");
    
    brelse(b);
    return 0;
  } else {
    printf("✗ Failed to read block %d from disk %d\n", blockno, disk_id);
    if(b) brelse(b);
    return -1;
  }
}

// 简单的磁盘探测函数
void
disk_probe(void)
{
  printf("\n=== Disk Probe ===\n");
  
  // 尝试探测多个可能的磁盘
  for (int disk_id = 0; disk_id < 4; disk_id++) {
    printf("Probing disk %d... ", disk_id);
    
    // 尝试读取块0（通常所有磁盘都有）
    struct buf *b = bread(disk_id, 0);
    
    if (b && b->valid) {
      printf("✓ Found (first byte: 0x%02x)\n", b->data[0] & 0xFF);
      brelse(b);
    } else {
      printf("✗ Not found\n");
      if (b) brelse(b);
    }
  }
  printf("=== End Probe ===\n\n");
}