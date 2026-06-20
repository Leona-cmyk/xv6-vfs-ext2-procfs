/**
 * @file fs.c
 * @brief xv6 文件系统核心实现（集成 VFS 支持）
 * 
 * 本文件实现了 xv6 文件系统的五层架构：
 *   - Blocks:      原始磁盘块分配器
 *   - Log:         多步更新的崩溃恢复
 *   - Files:       inode 分配、读写、元数据管理
 *   - Directories: 特殊内容的 inode（其他 inode 的列表）
 *   - Names:       路径解析（如 /usr/rtm/xv6/fs.c）
 * 
 * VFS 集成：
 *   本文件同时作为 VFS 的底层实现，通过 xv6_i_op 和 xv6_f_op
 *   操作表提供统一接口，支持多文件系统共存。
 * 
 * @author MIT xv6 Team, VFS Extension Team
 * @version 2.0
 * @date 2026-01-13
 */

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"
#include "vfs.h"
#include "ext2.h"

#define min(a, b) ((a) < (b) ? (a) : (b))

struct superblock sb[MAX_DISKS];

static void readsb(int dev, struct superblock *sb) {
  struct buf *bp;
  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

int is_ext2_filesystem(int dev) {
  struct buf *bp;
  if (dev < 0 || dev >= MAX_DISKS) return 0;
  bp = bread(dev, 1);
  if (!bp) return 0;
  uint16 magic = *((uint16*)(bp->data + 0x38));
  brelse(bp);
  return (magic == 0xEF53);
}

void fsinit(int dev) {
  if (is_ext2_filesystem(dev)) {
    sb[dev].magic = EXT2_SUPER_MAGIC;
    sb[dev].size = 0;
    return;
  }
  readsb(dev, &sb[dev]);
  if(sb[dev].magic != FSMAGIC) panic("fsinit: invalid file system");
  initlog(dev, &sb[dev]);
  ireclaim(dev);
}

struct superblock* getsb(int dev) {
  if (dev < 0 || dev >= MAX_DISKS) {
    if (dev == PROC_DEV) return 0;
    panic("getsb: invalid device");
  }
  return &sb[dev];
}

static void bzero(int dev, int bno) {
  struct buf *bp;
  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

static uint balloc(uint dev) {
  int b, bi, m;
  struct buf *bp;
  struct superblock *sbp = getsb(dev);
  // Ext2 设备应使用 ext2_alloc_block，不应调用此函数
  if (dev == 1 || (sbp && sbp->magic == EXT2_SUPER_MAGIC)) return 0;
  if (dev == PROC_DEV) return 0;
  if(!sbp) return 0;
  for(b = 0; b < sbp->size; b += BPB){
    bp = bread(dev, BBLOCK(b, *sbp));
    for(bi = 0; bi < BPB && b + bi < sbp->size; bi++){
      m = 1 << (bi % 8);
      if((bp->data[bi/8] & m) == 0){
        bp->data[bi/8] |= m;
        log_write(bp);
        brelse(bp);
        bzero(dev, b + bi);
        return b + bi;
      }
    }
    brelse(bp);
  }
  return 0;
}

static void bfree(int dev, uint b) {
  struct buf *bp;
  int bi, m;
  struct superblock *sbp = getsb(dev);
  // Ext2 设备应使用 ext2 的块释放函数，不应调用此函数
  if (dev == 1 || (sbp && sbp->magic == EXT2_SUPER_MAGIC)) return;
  if (dev == PROC_DEV) return;
  if(!sbp) return;
  bp = bread(dev, BBLOCK(b, *sbp));
  bi = b % BPB;
  m = 1 << (bi % 8);
  if((bp->data[bi/8] & m) == 0) panic("bfree: free");
  bp->data[bi/8] &= ~m;
  log_write(bp);
  brelse(bp);
}

/*============================================================================
 *                          Inode 表（全局 inode 缓存）
 *===========================================================================*/

/**
 * @brief 内存中的 inode 缓存表
 * 
 * 所有活跃的 inode 都缓存在此表中，通过引用计数管理生命周期。
 * 表大小由 NINODE 参数控制。
 */
struct {
  struct spinlock lock;      /**< 保护 inode 表的自旋锁 */
  struct inode inode[NINODE]; /**< inode 缓存数组 */
} itable;

/**
 * @brief 初始化 inode 表
 * 
 * 初始化 inode 表的保护锁和每个 inode 的睡眠锁。
 * 在系统启动时调用。
 */
void iinit() {
  initlock(&itable.lock, "itable");
  for(int i = 0; i < NINODE; i++) initsleeplock(&itable.inode[i].lock, "inode");
}

static uint bmap(struct inode *ip, uint bn) {
  uint addr, *a;
  struct buf *bp;
  if(bn < NDIRECT){
    if((addr = ip->addrs[bn]) == 0){
      if((addr = balloc(ip->dev)) == 0) return 0;
      ip->addrs[bn] = addr;
    }
    return addr;
  }
  bn -= NDIRECT;
  if(bn < NINDIRECT){
    if((addr = ip->addrs[NDIRECT]) == 0){
      if((addr = balloc(ip->dev)) == 0) return 0;
      ip->addrs[NDIRECT] = addr;
    }
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if((addr = a[bn]) == 0){
      if((addr = balloc(ip->dev)) != 0){
        a[bn] = addr;
        log_write(bp);
      }
    }
    brelse(bp);
    return addr;
  }
  panic("bmap: out");
}

// XV6 专属实现
void xv6_itrunc(struct inode *ip) {
  int i, j;
  struct buf *bp;
  uint *a;
  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }
  if(ip->addrs[NDIRECT]){
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++) if(a[j]) bfree(ip->dev, a[j]);
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }
  ip->size = 0;
  iupdate(ip);
}

void xv6_iupdate(struct inode *ip) {
  struct buf *bp;
  struct dinode *dip;
  struct superblock *sbp = getsb(ip->dev);
  bp = bread(ip->dev, IBLOCK(ip->inum, *sbp));
  dip = (struct dinode*)bp->data + ip->inum%IPB;
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  log_write(bp);
  brelse(bp);
}

int xv6_readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n) {
  uint tot, m;
  struct buf *bp;
  if(off > ip->size || off + n < off) return 0;
  if(off + n > ip->size) n = ip->size - off;
  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    uint addr = bmap(ip, off/BSIZE);
    if(addr == 0) break;
    bp = bread(ip->dev, addr);
    m = min(n - tot, BSIZE - off%BSIZE);
    if(either_copyout(user_dst, dst, bp->data + (off % BSIZE), m) == -1) {
      brelse(bp);
      return -1;
    }
    brelse(bp);
  }
  return tot;
}

int xv6_writei(struct inode *ip, int user_src, uint64 src, uint off, uint n) {
  uint tot, m;
  struct buf *bp;
  if(off > ip->size || off + n < off) return -1;
  if(off + n > MAXFILE*BSIZE) return -1;
  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
    uint addr = bmap(ip, off/BSIZE);
    if(addr == 0) break;
    bp = bread(ip->dev, addr);
    m = min(n - tot, BSIZE - off%BSIZE);
    if(either_copyin(bp->data + (off % BSIZE), user_src, src, m) == -1) {
      brelse(bp);
      break;
    }
    log_write(bp);
    brelse(bp);
  }
  if(off > ip->size) ip->size = off;
  iupdate(ip);
  return tot;
}

struct inode* xv6_lookup(struct inode *dp, char *name, uint *poff) {
  uint off;
  struct dirent de;
  if(dp->type != T_DIR) panic("xv6_lookup: not dir");
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de)) panic("xv6_lookup: read");
    if(de.inum == 0) continue;
    if(strncmp(name, de.name, DIRSIZ) == 0){
      if(poff) *poff = off;
      return iget(dp->dev, de.inum);
    }
  }
  return 0;
}

int xv6_link(struct inode *dp, char *name, uint inum, short type) {
  int off;
  struct dirent de;
  struct inode *ip;
  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);
    return -1;
  }
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de)) panic("xv6_link: read");
    if(de.inum == 0) break;
  }
  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de)) return -1;
  return 0;
}

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

int xv6_unlink(struct inode *dp, char *name) {
  // 这里暂时简化，保持逻辑正确但不一定完整
  // 原本 unlink 逻辑在 sys_unlink 中，我们需要将其迁入此处
  return -1; 
}

/*============================================================================
 *                          xv6 文件系统 VFS 操作表
 *===========================================================================*/

/** @brief xv6 文件系统的 inode 操作表 */
struct inode_operations xv6_i_op = {
  .lookup = xv6_lookup,
  .link = xv6_link,
  .unlink = xv6_unlink,
  .mkdir = 0,
  .itrunc = xv6_itrunc,
};

/** @brief xv6 文件系统的文件操作表 */
struct file_operations xv6_f_op = {
  .read = xv6_readi,
  .write = xv6_writei,
  .stat = 0,
  .iupdate = xv6_iupdate,
};

/** @brief xv6 文件系统类型描述符（用于 VFS 注册） */
struct filesystem_type xv6_fs_type = {
  .name = "xv6",
  .fs_type = FS_XV6,
  .root_inum = 1,
  .i_op = &xv6_i_op,
  .f_op = &xv6_f_op,
  .probe = 0,
  .read_super = 0,
};

/*============================================================================
 *                          VFS 通用接口实现
 *===========================================================================*/

/**
 * @brief 分配新的 inode
 * 
 * 在指定设备上分配一个新的 inode。根据设备类型自动选择
 * xv6 或 EXT2 的分配逻辑。
 * 
 * @param dev  设备号
 * @param type 文件类型（T_FILE, T_DIR, T_DEVICE）
 * @return struct inode* 成功返回新 inode 指针，失败返回 NULL
 */
struct inode* ialloc(uint dev, short type) {
  int inum;
  struct buf *bp;
  struct dinode *dip;
  struct superblock *sbp = getsb(dev);
  if (dev == 1 || (sbp && sbp->magic == EXT2_SUPER_MAGIC)) {
    uint32 inum_ext2;
    if (ext2_alloc_inode(dev, type, &inum_ext2) < 0) return 0;
    struct inode *ip = iget(dev, inum_ext2);
    // 返回未锁定的 inode，与 xv6 原生行为一致
    // create() 函数会在之后调用 ilock()
    return ip;
  }
  if (dev == PROC_DEV) return 0;
  if(!sbp || sbp->ninodes == 0) return 0;
  for(inum = 1; inum < sbp->ninodes; inum++){
    bp = bread(dev, IBLOCK(inum, *sbp));
    dip = (struct dinode*)bp->data + inum%IPB;
    if(dip->type == 0){
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      log_write(bp);
      brelse(bp);
      return iget(dev, inum);
    }
    brelse(bp);
  }
  return 0;
}

/**
 * @brief 将 inode 元数据同步到磁盘
 * 
 * 通过 VFS 操作表调用具体文件系统的更新函数。
 * 
 * @param ip 目标 inode
 */
void iupdate(struct inode *ip) {
  if(ip->f_op && ip->f_op->iupdate) ip->f_op->iupdate(ip);
}

/**
 * @brief 获取 inode 引用
 * 
 * 在 inode 缓存表中查找或分配指定的 inode，并增加引用计数。
 * 返回的 inode 未锁定，需要调用 ilock() 后才能访问其内容。
 * 
 * @param dev  设备号
 * @param inum inode 编号
 * @return struct inode* 成功返回 inode 指针，表满返回 NULL
 * 
 * @note 返回的 inode 必须通过 iput() 释放
 */
struct inode* iget(uint dev, uint inum) {
  struct inode *ip, *empty;
  acquire(&itable.lock);
  empty = 0;
  for(ip = &itable.inode[0]; ip < &itable.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      ip->ref++;
      release(&itable.lock);
      return ip;
    }
    if(empty == 0 && ip->ref == 0) empty = ip;
  }
  if(empty == 0) {
    release(&itable.lock);
    return 0;  // inode 表已满，返回 NULL 而不是 panic
  }
  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  
  if (dev == 1) {
    ip->fs_type = FS_EXT2;
    extern struct inode_operations ext2_i_op;
    extern struct file_operations ext2_f_op;
    ip->i_op = &ext2_i_op; ip->f_op = &ext2_f_op;
  } else if (dev == PROC_DEV) {
    ip->fs_type = FS_PROC;
    extern struct inode_operations procfs_i_op;
    extern struct file_operations procfs_f_op;
    ip->i_op = &procfs_i_op; ip->f_op = &procfs_f_op;
  } else {
    ip->fs_type = FS_XV6;
    ip->i_op = &xv6_i_op; ip->f_op = &xv6_f_op;
  }
  release(&itable.lock);
  return ip;
}

/**
 * @brief 复制 inode 引用（增加引用计数）
 * 
 * @param ip 源 inode
 * @return struct inode* 同一个 inode 指针
 */
struct inode* idup(struct inode *ip) {
  acquire(&itable.lock);
  ip->ref++;
  release(&itable.lock);
  return ip;
}

/**
 * @brief 锁定 inode 并从磁盘加载内容
 * 
 * 获取 inode 的睡眠锁，如果 inode 内容尚未加载（valid == 0），
 * 则根据文件系统类型从磁盘读取 inode 元数据。
 * 
 * @param ip 目标 inode（必须已通过 iget 获取）
 * 
 * @note 必须与 iunlock() 配对使用
 */
void ilock(struct inode *ip) {
  struct buf *bp;
  struct dinode *dip;
  struct superblock *sbp;
  if(ip == 0 || ip->ref < 1) panic("ilock");
  acquiresleep(&ip->lock);
  if(ip->valid == 0){
    if (ip->fs_type == FS_EXT2) {
      struct ext2_inode ei;
      ext2_read_inode(ip->dev, ip->inum, &ei);
      if ((ei.i_mode & 0xF000) == 0x4000) ip->type = T_DIR;
      else ip->type = T_FILE;
      ip->nlink = ei.i_links_count;
      ip->size = ei.i_size;
      memmove(ip->ext2.i_block, ei.i_block, sizeof(ei.i_block));
      ip->valid = 1;
    } else if (ip->fs_type == FS_PROC) {
      if(ip->inum == 2000 || (ip->inum >= 3000 && (ip->inum % 10 == 0)))
        ip->type = T_DIR;
      else
        ip->type = T_FILE;
      ip->valid = 1;
    } else {
      sbp = getsb(ip->dev);
      bp = bread(ip->dev, IBLOCK(ip->inum, *sbp));
      dip = (struct dinode*)bp->data + ip->inum%IPB;
      ip->type = dip->type;
      ip->nlink = dip->nlink;
      ip->size = dip->size;
      memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
      brelse(bp);
      ip->valid = 1;
    }
  }
}

/**
 * @brief 解锁 inode
 * 
 * 释放 inode 的睡眠锁，允许其他进程访问。
 * 
 * @param ip 已锁定的 inode
 */
void iunlock(struct inode *ip) {
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1) panic("iunlock");
  releasesleep(&ip->lock);
}

/**
 * @brief 释放 inode 引用
 * 
 * 减少 inode 引用计数。当引用计数降为 0 且 nlink 为 0 时，
 * 释放 inode 占用的磁盘空间（truncate + 标记为空闲）。
 * 
 * @param ip 要释放的 inode
 * 
 * @note 这是 inode 生命周期管理的关键函数
 */
void iput(struct inode *ip) {
  if (ip->fs_type == FS_PROC) {
    acquire(&itable.lock);
    ip->ref--;
    release(&itable.lock);
    return;
  }
  acquire(&itable.lock);
  if(ip->ref == 1 && ip->valid && ip->nlink == 0){
    acquiresleep(&ip->lock);
    release(&itable.lock);
    itrunc(ip);
    ip->type = 0;
    iupdate(ip);
    ip->valid = 0;
    releasesleep(&ip->lock);
    acquire(&itable.lock);
  }
  ip->ref--;
  release(&itable.lock);
}

void iunlockput(struct inode *ip) {
  iunlock(ip);
  iput(ip);
}

void ireclaim(int dev) {
  struct superblock *sbp = getsb(dev);
  if (dev == 1 || (sbp && sbp->magic == EXT2_SUPER_MAGIC)) return;
  if (dev == PROC_DEV || !sbp) return;
  for (int inum = 1; inum < sbp->ninodes; inum++) {
    struct buf *bp = bread(dev, IBLOCK(inum, *sbp));
    struct dinode *dip = (struct dinode *)bp->data + inum % IPB;
    if (dip->type != 0 && dip->nlink == 0) {
      struct inode *ip = iget(dev, inum);
      brelse(bp);
      begin_op(); ilock(ip); iunlock(ip); iput(ip); end_op();
    } else brelse(bp);
  }
}

void itrunc(struct inode *ip) {
  if (ip->i_op && ip->i_op->itrunc) ip->i_op->itrunc(ip);
}

void stati(struct inode *ip, struct stat *st) {
  if (ip->f_op && ip->f_op->stat) { ip->f_op->stat(ip, st); return; }
  st->dev = ip->dev; st->ino = ip->inum; st->type = ip->type;
  st->nlink = ip->nlink; st->size = ip->size;
}

int readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n) {
  if(ip->f_op && ip->f_op->read) return ip->f_op->read(ip, user_dst, dst, off, n);
  return -1;
}

int writei(struct inode *ip, int user_src, uint64 src, uint off, uint n) {
  if(ip->f_op && ip->f_op->write) return ip->f_op->write(ip, user_src, src, off, n);
  return -1;
}

/**
 * @brief 在目录中查找指定名称的条目
 * 
 * 通过 VFS 操作表调用具体文件系统的 lookup 函数。
 * 
 * @param dp   父目录 inode
 * @param name 要查找的文件/目录名
 * @param poff 返回条目在目录中的偏移量（可选）
 * @return struct inode* 找到返回 inode 指针，未找到返回 NULL
 */
struct inode* dirlookup(struct inode *dp, char *name, uint *poff) {
  struct inode *ip;
  
  if (dp->i_op && dp->i_op->lookup) {
    ip = dp->i_op->lookup(dp, name, poff);
    return ip;
  }
  return 0;
}

/**
 * @brief 在目录中创建新条目（硬链接）
 * 
 * 通过 VFS 操作表调用具体文件系统的 link 函数。
 * 
 * @param dp   父目录 inode
 * @param name 新条目名称
 * @param inum 目标 inode 号
 * @param type 文件类型
 * @return int 成功返回 0，失败返回 -1
 */
int dirlink(struct inode *dp, char *name, uint inum, short type) {
  if (dp->i_op && dp->i_op->link) {
    return dp->i_op->link(dp, name, inum, type);
  }
  return -1;
}

/**
 * @brief 从路径中提取下一个路径元素
 * 
 * @param path 输入路径
 * @param name 输出的路径元素名称
 * @return char* 剩余路径，无更多元素返回 NULL
 */
static char* skipelem(char *path, char *name) {
  char *s;
  int len;
  while(*path == '/') path++;
  if(*path == 0) return 0;
  s = path;
  while(*path != '/' && *path != 0) path++;
  len = path - s;
  if(len >= DIRSIZ) memmove(name, s, DIRSIZ);
  else { memmove(name, s, len); name[len] = 0; }
  while(*path == '/') path++;
  return path;
}

/**
 * @brief 路径解析核心函数
 * 
 * 解析路径字符串，逐级查找目录项，最终返回目标 inode。
 * 支持绝对路径和相对路径，自动处理挂载点穿越。
 * 
 * @param path        路径字符串
 * @param nameiparent 若为 1，返回父目录而非目标本身
 * @param name        返回最后一个路径元素的名称
 * @return struct inode* 成功返回 inode 指针，失败返回 NULL
 * 
 * @note 这是 VFS 路径解析的核心，支持多文件系统穿越
 */
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;
  char full_path[MAXPATH];
  int is_absolute = (*path == '/');

  if(is_absolute) {
    ip = iget(ROOTDEV, ROOTINO);
    if(ip == 0) return 0;
    full_path[0] = '/';
    full_path[1] = '\0';
  } else {
    ip = idup(myproc()->cwd);
    if(ip == 0) return 0;
    full_path[0] = '\0';
  }

  while((path = skipelem(path, name)) != 0){
    ilock(ip);
    if(ip->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    if(nameiparent && *path == '\0'){
      iunlock(ip);
      return ip;
    }

    // 构建当前完整路径
    char new_full[MAXPATH];
    if(is_absolute) {
      if(full_path[1] != '\0') {
        snprintf(new_full, MAXPATH, "%s/%s", full_path, name);
      } else {
        snprintf(new_full, MAXPATH, "/%s", name);
      }
    } else {
      safestrcpy(new_full, name, MAXPATH);
    }

    // 检查当前路径是否为挂载点
    struct mount *m = find_mount(new_full);
    if(m && strlen(m->path) == strlen(new_full)) {
      iunlockput(ip);
      ip = iget(m->dev, m->root_inum);
      safestrcpy(full_path, new_full, MAXPATH);
      continue;
    }

    if((next = dirlookup(ip, name, 0)) == 0){
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = next;
    safestrcpy(full_path, new_full, MAXPATH);
  }
  if(nameiparent){
    iput(ip);
    return 0;
  }
  return ip;
}

struct inode* namei(char *path) { char name[DIRSIZ]; return namex(path, 0, name); }
struct inode* nameiparent(char *path, char *name) { return namex(path, 1, name); }
