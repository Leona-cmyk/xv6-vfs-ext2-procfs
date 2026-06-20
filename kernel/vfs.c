/**
 * @file vfs.c
 * @brief 虚拟文件系统（VFS）层实现
 * 
 * 本文件实现了 xv6 操作系统的虚拟文件系统层，提供统一的文件系统抽象接口。
 * 主要功能包括：
 * - 文件系统类型的动态注册与注销
 * - 挂载点管理（mount/umount）
 * - 路径解析与转换
 * - Dentry Cache（目录项缓存）加速路径查找
 * 
 * VFS 层使得 xv6 能够同时支持多种文件系统（如 xv6 原生 FS、EXT2、procfs），
 * 并通过统一的接口进行访问，实现了文件系统的多态性。
 * 
 * @author xv6 VFS Extension Team
 * @version 1.0
 * @date 2026-01-13
 * 
 * @see vfs.h - VFS 核心数据结构定义
 * @see fs.c  - 具体文件系统实现
 */

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "buf.h"
#include "file.h"
#include "ext2.h"
#include "vfs.h"

/*============================================================================
 *                          全局变量定义
 *===========================================================================*/

/** @brief 全局挂载点链表头指针 */
struct mount *mounts = 0;

/** @brief 挂载点链表保护锁 */
struct spinlock mounts_lock;

/** @brief 已注册的文件系统类型表 */
static struct filesystem_type *registered_fs[MAX_FILESYSTEMS];

/** @brief 文件系统类型表保护锁 */
static struct spinlock fs_lock;

/** @brief Dentry Cache 数组 */
static struct dentry dcache[DCACHE_SIZE];

/** @brief Dentry Cache 哈希表 */
static struct dentry *dcache_hash[DCACHE_HASH];

/** @brief Dentry Cache 保护锁 */
static struct spinlock dcache_lock;

/** @brief LRU 时间戳计数器，用于 Dentry Cache 替换策略 */
static uint dcache_tick;

/*============================================================================
 *                          Dentry Cache 实现
 *===========================================================================*/

/**
 * @brief 计算 Dentry Cache 哈希值
 * 
 * 基于设备号、父目录 inode 号和文件名计算哈希值，
 * 用于快速定位缓存条目。
 * 
 * @param dev         设备号
 * @param parent_inum 父目录 inode 号
 * @param name        文件/目录名
 * @return uint       哈希值（0 ~ DCACHE_HASH-1）
 */
static uint
dcache_hashfn(int dev, uint parent_inum, const char *name)
{
  uint h = dev * 31 + parent_inum;
  while(*name) {
    h = h * 31 + (unsigned char)*name++;
  }
  return h % DCACHE_HASH;
}

/**
 * @brief 初始化 Dentry Cache
 * 
 * 清空所有缓存条目和哈希表，初始化保护锁。
 * 该函数在系统启动时由 vfs_init() 调用。
 */
void
dcache_init(void)
{
  initlock(&dcache_lock, "dcache");
  dcache_tick = 0;
  
    /* 完全清零所有 dentry 条目 */
    memset(dcache, 0, sizeof(dcache));
    
    /* 清零哈希表 */
    memset(dcache_hash, 0, sizeof(dcache_hash));
  }
  
/**
 * @brief 在 Dentry Cache 中查找条目
 * 
 * 根据设备号、父目录 inode 号和文件名查找缓存条目。
 * 如果找到，更新 LRU 时间戳并返回条目指针。
 * 
 * @param dev         设备号
 * @param parent_inum 父目录 inode 号
 * @param name        文件/目录名
 * @return struct dentry* 找到返回条目指针，未找到返回 NULL
 */
struct dentry*
dcache_lookup(int dev, uint parent_inum, const char *name)
{
  uint h = dcache_hashfn(dev, parent_inum, name);
  struct dentry *d;
  
  acquire(&dcache_lock);
  
    /* 遍历哈希链表查找匹配条目 */
  for(d = dcache_hash[h]; d; d = d->hash_next) {
    if(d->valid && d->dev == dev && d->parent_inum == parent_inum &&
       strncmp(d->name, name, DENTRY_NAME_LEN) == 0) {
            /* 更新 LRU 时间戳 */
      d->lru_tick = ++dcache_tick;
      release(&dcache_lock);
      return d;
    }
  }
  
  release(&dcache_lock);
  return 0;
}

/**
 * @brief 查找最久未使用的 Dentry Cache 条目
 * 
 * 用于缓存满时的替换策略（LRU）。
 * 优先返回空闲条目，否则返回 LRU 时间戳最小的条目。
 * 
 * @return struct dentry* 可用于替换的条目指针
 * 
 * @note 调用者需持有 dcache_lock
 */
static struct dentry*
dcache_find_lru(void)
{
  struct dentry *lru = 0;
  uint min_tick = 0xffffffff;
  
  for(int i = 0; i < DCACHE_SIZE; i++) {
    if(!dcache[i].valid) {
            return &dcache[i];  /* 找到空闲条目 */
    }
    if(dcache[i].lru_tick < min_tick) {
      min_tick = dcache[i].lru_tick;
      lru = &dcache[i];
    }
  }
  
  return lru;
}

/**
 * @brief 从哈希链表中移除 Dentry Cache 条目
 * 
 * @param d 要移除的条目指针
 * 
 * @note 调用者需持有 dcache_lock
 */
static void
dcache_remove_from_hash(struct dentry *d)
{
  if(!d->valid) return;
  
  uint h = dcache_hashfn(d->dev, d->parent_inum, d->name);
  struct dentry **pp = &dcache_hash[h];
  
  while(*pp) {
    if(*pp == d) {
      *pp = d->hash_next;
      return;
    }
    pp = &(*pp)->hash_next;
  }
}

/**
 * @brief 向 Dentry Cache 添加条目
 * 
 * 如果条目已存在则更新，否则分配新条目（可能替换 LRU 条目）。
 * 
 * @param dev         设备号
 * @param parent_inum 父目录 inode 号
 * @param name        文件/目录名
 * @param inum        目标 inode 号
 */
void
dcache_add(int dev, uint parent_inum, const char *name, uint inum)
{
  acquire(&dcache_lock);
  
    /* 先检查是否已存在 */
  uint h = dcache_hashfn(dev, parent_inum, name);
  struct dentry *d;
  
  for(d = dcache_hash[h]; d; d = d->hash_next) {
    if(d->valid && d->dev == dev && d->parent_inum == parent_inum &&
       strncmp(d->name, name, DENTRY_NAME_LEN) == 0) {
            /* 已存在，更新 inum 和 LRU */
      d->inum = inum;
      d->lru_tick = ++dcache_tick;
      release(&dcache_lock);
      return;
    }
  }
  
    /* 找一个空闲或 LRU 条目 */
  d = dcache_find_lru();
  if(!d) {
    release(&dcache_lock);
        return;
  }
  
    /* 如果是替换旧条目，先从哈希链表移除 */
  dcache_remove_from_hash(d);
  
    /* 填充新条目 */
  d->valid = 1;
  d->dev = dev;
  d->parent_inum = parent_inum;
  safestrcpy(d->name, name, DENTRY_NAME_LEN);
  d->inum = inum;
  d->lru_tick = ++dcache_tick;
  
    /* 添加到哈希链表头部 */
  d->hash_next = dcache_hash[h];
  dcache_hash[h] = d;
  
  release(&dcache_lock);
}

/**
 * @brief 使特定 Dentry Cache 条目失效
 * 
 * 用于文件/目录删除时同步更新缓存。
 * 
 * @param dev         设备号
 * @param parent_inum 父目录 inode 号
 * @param name        文件/目录名
 */
void
dcache_invalidate(int dev, uint parent_inum, const char *name)
{
  acquire(&dcache_lock);
  
  uint h = dcache_hashfn(dev, parent_inum, name);
  struct dentry **pp = &dcache_hash[h];
  
  while(*pp) {
    struct dentry *d = *pp;
    if(d->valid && d->dev == dev && d->parent_inum == parent_inum &&
       strncmp(d->name, name, DENTRY_NAME_LEN) == 0) {
      *pp = d->hash_next;
      d->valid = 0;
      d->hash_next = 0;
      release(&dcache_lock);
      return;
    }
    pp = &(*pp)->hash_next;
  }
  
  release(&dcache_lock);
}

/**
 * @brief 使指定设备上的所有 Dentry Cache 条目失效
 * 
 * 用于文件系统卸载（umount）时清理缓存。
 * 
 * @param dev 设备号
 */
void
dcache_invalidate_dev(int dev)
{
  acquire(&dcache_lock);
  
  for(int i = 0; i < DCACHE_SIZE; i++) {
    if(dcache[i].valid && dcache[i].dev == dev) {
      dcache_remove_from_hash(&dcache[i]);
      dcache[i].valid = 0;
      dcache[i].hash_next = 0;
    }
  }
  
  release(&dcache_lock);
}

/*============================================================================
 *                          VFS 核心初始化
 *===========================================================================*/

/**
 * @brief 初始化 VFS 子系统
 * 
 * 初始化挂载点链表、文件系统类型表和 Dentry Cache。
 * 该函数在系统启动时调用。
 */
void
vfs_init(void)
{
  initlock(&mounts_lock, "mounts");
  initlock(&fs_lock, "fs_types");
  mounts = 0;
    
    /* 清空已注册文件系统类型表 */
  for(int i = 0; i < MAX_FILESYSTEMS; i++)
    registered_fs[i] = 0;
  
    /* 初始化 Dentry Cache */
  dcache_init();
}

/*============================================================================
 *                          文件系统类型注册
 *===========================================================================*/

/**
 * @brief 注册文件系统类型
 * 
 * 将新的文件系统类型添加到已注册列表中。
 * 每种文件系统类型只能注册一次。
 * 
 * @param fs 文件系统类型结构体指针
 * @return int 成功返回 0，失败返回 -1
 * 
 * @note 失败原因：参数无效、已注册或注册表已满
 */
int
register_filesystem(struct filesystem_type *fs)
{
  if(!fs || fs->fs_type <= 0) return -1;

  acquire(&fs_lock);
  
    /* 检查是否已注册 */
  for(int i = 0; i < MAX_FILESYSTEMS; i++) {
    if(registered_fs[i] && registered_fs[i]->fs_type == fs->fs_type) {
      release(&fs_lock);
            return -1;
    }
  }

    /* 找一个空位 */
  for(int i = 0; i < MAX_FILESYSTEMS; i++) {
    if(registered_fs[i] == 0) {
      registered_fs[i] = fs;
      release(&fs_lock);
      return 0;
    }
  }

  release(&fs_lock);
    return -1;
}

/**
 * @brief 注销文件系统类型
 * 
 * 从已注册列表中移除指定的文件系统类型。
 * 
 * @param fs_type 文件系统类型标识
 * @return int    成功返回 0，未找到返回 -1
 */
int
unregister_filesystem(int fs_type)
{
  acquire(&fs_lock);
  
  for(int i = 0; i < MAX_FILESYSTEMS; i++) {
    if(registered_fs[i] && registered_fs[i]->fs_type == fs_type) {
      registered_fs[i] = 0;
      release(&fs_lock);
      return 0;
    }
  }

  release(&fs_lock);
  return -1;
}

/**
 * @brief 获取已注册的文件系统类型
 * 
 * @param fs_type 文件系统类型标识
 * @return struct filesystem_type* 找到返回类型结构体指针，未找到返回 NULL
 */
struct filesystem_type*
get_filesystem(int fs_type)
{
  struct filesystem_type *fs = 0;
  
  acquire(&fs_lock);
  for(int i = 0; i < MAX_FILESYSTEMS; i++) {
    if(registered_fs[i] && registered_fs[i]->fs_type == fs_type) {
      fs = registered_fs[i];
      break;
    }
  }
  release(&fs_lock);
  
  return fs;
}

/*============================================================================
 *                          挂载点管理
 *===========================================================================*/

/**
 * @brief 挂载文件系统
 * 
 * 将指定设备上的文件系统挂载到指定路径。
 * 
 * @param dev     设备号
 * @param fs_type 文件系统类型（FS_XV6, FS_EXT2, FS_PROC）
 * @param path    挂载点路径
 * @return int    成功返回 0，失败返回 -1
 * 
 * @note EXT2 文件系统挂载前会验证超级块魔数
 * 
 * @example
 * mount_fs(1, FS_EXT2, "/mnt");     // 挂载 EXT2 到 /mnt
 * mount_fs(99, FS_PROC, "/proc");   // 挂载 procfs 到 /proc
 */
int
mount_fs(int dev, int fs_type, const char *path)
{
  struct mount *m;
  struct filesystem_type *fs;

    /* 验证路径有效性 */
  if(!path || strlen(path) >= MAXPATH) {
    return -1;
  }

    /* 查找已注册的文件系统类型 */
  fs = get_filesystem(fs_type);
  if(!fs) {
        /* 如果没有注册，使用默认逻辑（向后兼容） */
    if(fs_type != FS_XV6 && fs_type != FS_EXT2 && fs_type != FS_PROC) {
      return -1;
    }
  }

    /* EXT2 文件系统：验证超级块 */
  if(fs_type == FS_EXT2) {
    struct ext2_superblock sb;
    if(ext2_read_super(dev, &sb) < 0) {
      return -1;
    }
  }

    /* 分配挂载点结构 */
  m = (struct mount*)kalloc();
  if(!m) {
    return -1;
  }
  memset(m, 0, sizeof(*m));

    /* 初始化挂载点 */
  m->dev = dev;
  m->fs_type = fs_type;
  safestrcpy(m->path, path, MAXPATH);

    /* 设置操作表和根 inode 号 */
  if(fs) {
    m->root_inum = fs->root_inum;
    m->i_op = fs->i_op;
    m->f_op = fs->f_op;
  } else {
        /* 默认逻辑 */
    if(fs_type == FS_PROC) m->root_inum = 2000;
    else if(fs_type == FS_EXT2) m->root_inum = 2;
    else m->root_inum = 1;
  }

    /* 添加到挂载点链表头部 */
  acquire(&mounts_lock);
  m->next = mounts;
  mounts = m;
  release(&mounts_lock);

  return 0;
}

/**
 * @brief 卸载文件系统
 * 
 * 从指定路径卸载已挂载的文件系统，并清理相关的 Dentry Cache。
 * 
 * @param path 挂载点路径
 * @return int 成功返回 0，未找到返回 -1
 */
int
umount_fs(const char *path)
{
  struct mount *m, **pp;

  if(!path) {
    return -1;
  }

  acquire(&mounts_lock);

  pp = &mounts;
  while((m = *pp) != 0) {
    if(strncmp(m->path, path, MAXPATH) == 0) {
      int dev = m->dev;
      *pp = m->next;
      release(&mounts_lock);
      
            /* 使该设备的所有 Dentry Cache 条目失效 */
      dcache_invalidate_dev(dev);
      
      kfree((void*)m);
      return 0;
    }
    pp = &m->next;
  }

  release(&mounts_lock);
  return -1;
}

/**
 * @brief 根据路径查找挂载点
 * 
 * 返回最匹配给定路径的挂载点。
 * 
 * @param path 文件路径
 * @return struct mount* 找到返回挂载点指针，未找到返回 NULL
 */
struct mount*
find_mount(const char *path)
{
  struct mount *m;
  int path_len, mount_len;

  if(!path) {
    return 0;
  }

  path_len = strlen(path);

  acquire(&mounts_lock);

  for(m = mounts; m; m = m->next) {
    mount_len = strlen(m->path);
    if(path_len >= mount_len && 
       strncmp(path, m->path, mount_len) == 0 &&
       (mount_len == 1 || path[mount_len] == '/' || path[mount_len] == '\0')) {
      release(&mounts_lock);
      return m;
    }
  }

  release(&mounts_lock);
  return 0;
}

/**
 * @brief 根据设备号查找挂载点
 * 
 * @param dev 设备号
 * @return struct mount* 找到返回挂载点指针，未找到返回 NULL
 */
struct mount*
find_mount_by_dev(int dev)
{
    struct mount *m;
    
    acquire(&mounts_lock);
    for(m = mounts; m; m = m->next) {
        if(m->dev == dev) {
            release(&mounts_lock);
            return m;
        }
    }
    release(&mounts_lock);
    return 0;
}

/*============================================================================
 *                          路径转换
 *===========================================================================*/

/**
 * @brief 将绝对路径转换为相对于挂载点的路径
 * 
 * 用于跨文件系统操作时，将用户提供的绝对路径转换为
 * 目标文件系统内的相对路径。
 * 
 * @param full_path 完整路径（输入）
 * @param rel_path  相对路径（输出）
 * @param dev       设备号（输出）
 * @param fs_type   文件系统类型（输出）
 * @return int      成功返回 0，失败返回 -1
 * 
 * @example
 * 假设 /mnt 挂载了设备 1：
 * translate_path("/mnt/dir/file", rel_path, &dev, &fs_type)
 * 结果：rel_path = "/dir/file", dev = 1
 */
int
translate_path(const char *full_path, char *rel_path, int *dev, int *fs_type)
{
  struct mount *m;
  int mount_len;

  if(!full_path || !rel_path) {
    return -1;
  }

  acquire(&mounts_lock);

  for(m = mounts; m; m = m->next) {
    mount_len = strlen(m->path);
    if(strncmp(full_path, m->path, mount_len) == 0 &&
       (mount_len == 1 || full_path[mount_len] == '/' || full_path[mount_len] == '\0')) {
      *dev = m->dev;
      *fs_type = m->fs_type;
            
      if(mount_len == 1 && m->path[0] == '/') {
        if(full_path[mount_len] == '\0') {
          rel_path[0] = '/';
          rel_path[1] = '\0';
        } else {
          safestrcpy(rel_path, full_path + mount_len, MAXPATH);
        }
      } else {
        if(full_path[mount_len] == '\0') {
          safestrcpy(rel_path, "/", MAXPATH);
        } else {
          safestrcpy(rel_path, full_path + mount_len, MAXPATH);
        }
      }
      release(&mounts_lock);
      return 0;
    }
  }

  release(&mounts_lock);
  return -1;
}

/*============================================================================
 *                          VFS 通用文件操作
 *===========================================================================*/

/**
 * @brief VFS 通用文件读取接口
 * 
 * 通过 VFS 层读取指定路径的文件内容。
 * 自动处理路径解析和文件系统分发。
 * 
 * @param path 文件路径
 * @param buf  读取缓冲区
 * @param size 请求读取的字节数
 * @return int 成功返回读取的字节数，失败返回 -1
 */
int
vfs_read_file(const char *path, void *buf, int size)
{
  char rel_path[MAXPATH];
  int dev, fs_type;
  struct ext2_inode inode;

  if(translate_path(path, rel_path, &dev, &fs_type) < 0) {
    return -1;
  }

  if(fs_type == FS_XV6) {
        return -1;  /* 暂未实现 */
  } else if(fs_type == FS_EXT2) {
    uint32 inode_num;
    if(ext2_find_path(dev, rel_path, &inode_num) < 0) {
      return -1;
    }
    if(ext2_read_inode(dev, inode_num, &inode) < 0) {
      return -1;
    }
    struct inode *ip = iget(dev, inode_num);
    ilock(ip);
    int r = ext2_readi(ip, 0, (uint64)buf, 0, size);
    iunlockput(ip);
    return r;
  }

  return -1;
}
