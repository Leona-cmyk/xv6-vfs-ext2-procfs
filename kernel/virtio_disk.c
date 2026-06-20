//
// Multi-disk virtio-blk driver (polling/interrupt hybrid for stability).
//
#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "virtio.h"

#define VIRTIO_REG(dev, reg) ((volatile uint32*)((uint64)VIRTIO0 + (dev)*0x1000 + (reg)))

static struct {
  struct spinlock vdisk_lock;
  struct virtq_desc *desc;
  struct virtq_avail *avail;
  struct virtq_used *used;
  char free[NUM];
  struct virtio_blk_req ops[NUM];
  struct {
    struct buf *b;
    uint8 status;
  } info[NUM];
  uint16 used_idx;
  int initialized;
} disks[MAX_VIRTIO_DISKS];

// Allocate two contiguous pages for each disk's queue
__attribute__((aligned (PGSIZE))) static char vq_pages[MAX_VIRTIO_DISKS][2 * PGSIZE];

void
virtio_disk_init(void)
{
  for (int i = 0; i < MAX_VIRTIO_DISKS; i++) {
    memset(&disks[i], 0, sizeof(disks[i]));
    initlock(&disks[i].vdisk_lock, "virtio_disk");

    uint32 magic = *VIRTIO_REG(i, VIRTIO_MMIO_MAGIC_VALUE);
    uint32 ver   = *VIRTIO_REG(i, VIRTIO_MMIO_VERSION);
    uint32 did   = *VIRTIO_REG(i, VIRTIO_MMIO_DEVICE_ID);
    uint32 vid   = *VIRTIO_REG(i, VIRTIO_MMIO_VENDOR_ID);

    if(magic != 0x74726976 || (ver != 1 && ver != 2) || did != 2 || vid != 0x554d4551) {
      continue; // Skip if not a valid disk
    }

    uint32 status = 0;
    status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
    *VIRTIO_REG(i, VIRTIO_MMIO_STATUS) = status;

    status |= VIRTIO_CONFIG_S_DRIVER;
    *VIRTIO_REG(i, VIRTIO_MMIO_STATUS) = status;

    uint32 features = *VIRTIO_REG(i, VIRTIO_MMIO_DEVICE_FEATURES);
    features &= ~(1 << VIRTIO_BLK_F_RO);
    features &= ~(1 << VIRTIO_BLK_F_SCSI);
    features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
    features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
    features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
    features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
    *VIRTIO_REG(i, VIRTIO_MMIO_DRIVER_FEATURES) = features;

    status |= VIRTIO_CONFIG_S_FEATURES_OK;
    *VIRTIO_REG(i, VIRTIO_MMIO_STATUS) = status;

    *VIRTIO_REG(i, VIRTIO_MMIO_GUEST_PAGE_SIZE) = PGSIZE;
    *VIRTIO_REG(i, VIRTIO_MMIO_QUEUE_SEL) = 0;
    *VIRTIO_REG(i, VIRTIO_MMIO_QUEUE_NUM) = NUM;

    memset(vq_pages[i], 0, 2 * PGSIZE);
    disks[i].desc = (struct virtq_desc *)vq_pages[i];
    disks[i].avail = (struct virtq_avail *)(vq_pages[i] + NUM * sizeof(struct virtq_desc));
    disks[i].used = (struct virtq_used *)(vq_pages[i] + PGSIZE);

    if (ver == 1) {
      *VIRTIO_REG(i, VIRTIO_MMIO_QUEUE_ALIGN) = PGSIZE;
      *VIRTIO_REG(i, VIRTIO_MMIO_QUEUE_PFN) = ((uint64)disks[i].desc) >> 12;
    } else {
      uint64 pa = (uint64)disks[i].desc;
      *VIRTIO_REG(i, VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint32)pa;
      *VIRTIO_REG(i, VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint32)(pa >> 32);
      pa = (uint64)disks[i].avail;
      *VIRTIO_REG(i, VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint32)pa;
      *VIRTIO_REG(i, VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint32)(pa >> 32);
      pa = (uint64)disks[i].used;
      *VIRTIO_REG(i, VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint32)pa;
      *VIRTIO_REG(i, VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint32)(pa >> 32);
      *VIRTIO_REG(i, VIRTIO_MMIO_QUEUE_READY) = 0x1;
    }

    status |= VIRTIO_CONFIG_S_DRIVER_OK;
    *VIRTIO_REG(i, VIRTIO_MMIO_STATUS) = status;

    for(int j = 0; j < NUM; j++)
      disks[i].free[j] = 1;
    
    disks[i].initialized = 1;
  }
}

static int
alloc_desc(int dev)
{
  for(int i = 0; i < NUM; i++){
    if(disks[dev].free[i]){
      disks[dev].free[i] = 0;
      return i;
    }
  }
  return -1;
}

static void
free_desc(int dev, int i)
{
  if(i >= NUM)
    panic("free_desc 1");
  if(disks[dev].free[i])
    panic("free_desc 2");
  disks[dev].desc[i].addr = 0;
  disks[dev].desc[i].len = 0;
  disks[dev].desc[i].flags = 0;
  disks[dev].desc[i].next = 0;
  disks[dev].free[i] = 1;
  wakeup(&disks[dev].free[0]);
}

static void
free_chain(int dev, int i)
{
  while(1){
    int flag = disks[dev].desc[i].flags;
    int nxt = disks[dev].desc[i].next;
    free_desc(dev, i);
    if(flag & VRING_DESC_F_NEXT)
      i = nxt;
    else
      break;
  }
}

static int
alloc3_desc(int dev, int *idx)
{
  for(int i = 0; i < 3; i++){
    idx[i] = alloc_desc(dev);
    if(idx[i] < 0){
      for(int j = 0; j < i; j++)
        free_desc(dev, idx[j]);
      return -1;
    }
  }
  return 0;
}

static void
virtio_disk_check_used(int i)
{
  while(disks[i].used_idx != disks[i].used->idx){
    __sync_synchronize();
    int id = disks[i].used->ring[disks[i].used_idx % NUM].id;

    struct buf *b = disks[i].info[id].b;
    if(b){
      if(disks[i].info[id].status != 0)
        b->valid = 0;
      else
        b->valid = 1;
      b->disk = 0;   // disk is done with buf
      wakeup(b);
    }

    disks[i].used_idx++;
  }
}

void
virtio_disk_rw(struct buf *b, int dev, int write)
{
  if(dev >= MAX_VIRTIO_DISKS || !disks[dev].initialized)
    panic("virtio_disk_rw: invalid dev");

  acquire(&disks[dev].vdisk_lock);

  int idx[3];
  while(alloc3_desc(dev, idx) != 0)
    sleep(&disks[dev].free[0], &disks[dev].vdisk_lock);

  struct virtio_blk_req *buf0 = &disks[dev].ops[idx[0]];
  buf0->type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
  buf0->reserved = 0;
  buf0->sector = b->blockno * (BSIZE / 512);

  disks[dev].desc[idx[0]].addr = (uint64)buf0;
  disks[dev].desc[idx[0]].len = sizeof(struct virtio_blk_req);
  disks[dev].desc[idx[0]].flags = VRING_DESC_F_NEXT;
  disks[dev].desc[idx[0]].next = idx[1];

  disks[dev].desc[idx[1]].addr = (uint64)b->data;
  disks[dev].desc[idx[1]].len = BSIZE;
  disks[dev].desc[idx[1]].flags = (write ? 0 : VRING_DESC_F_WRITE) | VRING_DESC_F_NEXT;
  disks[dev].desc[idx[1]].next = idx[2];

  disks[dev].info[idx[0]].status = 0xff;
  disks[dev].desc[idx[2]].addr = (uint64)&disks[dev].info[idx[0]].status;
  disks[dev].desc[idx[2]].len = 1;
  disks[dev].desc[idx[2]].flags = VRING_DESC_F_WRITE;
  disks[dev].desc[idx[2]].next = 0;

  b->disk = 1;
  disks[dev].info[idx[0]].b = b;

  disks[dev].avail->ring[disks[dev].avail->idx % NUM] = idx[0];
  __sync_synchronize();
  disks[dev].avail->idx += 1;
  __sync_synchronize();
  *VIRTIO_REG(dev, VIRTIO_MMIO_QUEUE_NOTIFY) = 0;

  // Wait for disk to finish
  int timeout = 0;
  while(b->disk == 1) {
    if (timeout++ > 10000000) {
      break;
    }
    __sync_synchronize();
    virtio_disk_check_used(dev);
  }

  disks[dev].info[idx[0]].b = 0;
  free_chain(dev, idx[0]);

  release(&disks[dev].vdisk_lock);
}

void
virtio_disk_intr(void)
{
  for (int i = 0; i < MAX_VIRTIO_DISKS; i++) {
    if (!disks[i].initialized) continue;
    
    acquire(&disks[i].vdisk_lock);

    uint32 status = *VIRTIO_REG(i, VIRTIO_MMIO_INTERRUPT_STATUS);
    *VIRTIO_REG(i, VIRTIO_MMIO_INTERRUPT_ACK) = status & 0x3;

    virtio_disk_check_used(i);

    release(&disks[i].vdisk_lock);
  }
}
