
#ifndef _FILE_H
#define _FILE_H

#include "types.h"
#include "sleeplock.h"  // 添加这行！非常重要！

struct pipe;
struct inode;

struct file {
  enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE } type;
  int ref; // reference count
  char readable;
  char writable;
  struct pipe *pipe; // FD_PIPE
  struct inode *ip;  // FD_INODE and FD_DEVICE
  uint off;          // FD_INODE
  short major;       // FD_DEVICE
};

#define major(dev)  ((dev) >> 16 & 0xFFFF)
#define minor(dev)  ((dev) & 0xFFFF)
#define mkdev(m,n)  ((uint)((m)<<16| (n)))

#include "vfs.h"

// in-memory copy of an inode
struct inode {
  uint dev;           // Device number
  uint inum;          // Inode number
  int ref;            // Reference count
  struct sleeplock lock; // protects everything below here
  int valid;          // inode has been read from disk?
  int fs_type;        // 文件系统类型: FS_XV6 或 FS_EXT2

  struct inode_operations *i_op; // Inode 操作表
  struct file_operations *f_op;  // 文件操作表

  short type;         // copy of disk inode
  short major;
  short minor;
  short nlink;
  uint size;
  
  union {
    uint addrs[NDIRECT+1];
    struct {
      uint32 i_block[15];
      uint32 i_flags;
    } ext2;
  };
};

// map major device number to device functions.
struct devsw {
  int (*read)(int, uint64, int);
  int (*write)(int, uint64, int);
};

extern struct devsw devsw[];

#define CONSOLE 1

#endif
