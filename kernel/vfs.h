/**
 * @file vfs.h
 * @brief 虚拟文件系统（VFS）核心数据结构与接口定义
 * 
 * 本文件定义了 VFS 层的核心数据结构和接口，包括：
 * - 文件系统类型标识常量
 * - inode 操作接口（inode_operations）
 * - 文件操作接口（file_operations）
 * - 文件系统类型结构（filesystem_type）
 * - 挂载点结构（mount）
 * - Dentry Cache 结构（dentry）
 * 
 * VFS 通过定义统一的操作接口，使上层代码无需关心底层文件系统的具体实现。
 * 
 * @author xv6 VFS Extension Team
 * @version 1.0
 * @date 2026-01-13
 */

#ifndef _VFS_H_
#define _VFS_H_

#include "param.h"

/*============================================================================
 *                          文件系统类型标识
 *===========================================================================*/

/** @brief xv6 原生文件系统 */
#define FS_XV6  1

/** @brief EXT2 文件系统 */
#define FS_EXT2 2

/** @brief proc 虚拟文件系统 */
#define FS_PROC 3

/** @brief procfs 虚拟设备号 */
#define PROC_DEV 99

/** @brief 最大支持的文件系统类型数 */
#define MAX_FILESYSTEMS 8

/*============================================================================
 *                          Dentry Cache 配置
 *===========================================================================*/

/** @brief Dentry Cache 条目总数 */
#define DCACHE_SIZE     64

/** @brief Dentry Cache 哈希表桶数 */
#define DCACHE_HASH     32

/** @brief Dentry 名称最大长度（与 DIRSIZ 相同） */
#define DENTRY_NAME_LEN 14

/*============================================================================
 *                          前置声明
 *===========================================================================*/

struct inode;
struct stat;
struct dirent;
struct superblock;

/*============================================================================
 *                          VFS 操作接口
 *===========================================================================*/

/**
 * @brief Inode 操作接口
 * 
 * 定义了与 inode 相关的操作，主要用于目录操作和文件元数据管理。
 * 每种文件系统需要实现自己的操作表。
 */
struct inode_operations {
    /**
     * @brief 在目录中查找指定名称的条目
     * @param dp   父目录 inode
     * @param name 要查找的名称
     * @param poff 返回条目偏移量（可选）
     * @return 找到返回 inode 指针，未找到返回 NULL
     */
    struct inode* (*lookup)(struct inode *dp, char *name, uint *poff);
    
    /**
     * @brief 在目录中创建新条目（硬链接）
     * @param dp   父目录 inode
     * @param name 新条目名称
     * @param inum 目标 inode 号
     * @param type 文件类型
     * @return 成功返回 0，失败返回 -1
     */
    int (*link)(struct inode *dp, char *name, uint inum, short type);
    
    /**
     * @brief 从目录中删除条目
     * @param dp   父目录 inode
     * @param name 要删除的条目名称
     * @return 成功返回 0，失败返回 -1
     */
    int (*unlink)(struct inode *dp, char *name);
    
    /**
     * @brief 创建子目录
     * @param dp   父目录 inode
     * @param name 新目录名称
     * @return 成功返回 0，失败返回 -1
     */
    int (*mkdir)(struct inode *dp, char *name);
    
    /**
     * @brief 截断文件（释放所有数据块）
     * @param ip 目标 inode
     */
    void (*itrunc)(struct inode *ip);
};

/**
 * @brief 文件操作接口
 * 
 * 定义了与文件数据相关的操作，主要用于读写和元数据同步。
 */
struct file_operations {
    /**
     * @brief 读取文件数据
     * @param ip       文件 inode
     * @param user_dst 是否写入用户空间
     * @param dst      目标地址
     * @param off      起始偏移量
     * @param n        请求字节数
     * @return 成功返回读取字节数，失败返回 -1
     */
    int (*read)(struct inode *ip, int user_dst, uint64 dst, uint off, uint n);
    
    /**
     * @brief 写入文件数据
     * @param ip       文件 inode
     * @param user_src 是否从用户空间读取
     * @param src      源地址
     * @param off      起始偏移量
     * @param n        请求字节数
     * @return 成功返回写入字节数，失败返回 -1
     */
    int (*write)(struct inode *ip, int user_src, uint64 src, uint off, uint n);
    
    /**
     * @brief 获取文件状态信息
     * @param ip 文件 inode
     * @param st stat 结构体指针
     */
    void (*stat)(struct inode *ip, struct stat *st);
    
    /**
     * @brief 将 inode 元数据同步到磁盘
     * @param ip 文件 inode
     */
    void (*iupdate)(struct inode *ip);
};

/*============================================================================
 *                          文件系统类型结构
 *===========================================================================*/

/**
 * @brief 文件系统类型描述结构
 * 
 * 用于动态注册文件系统类型，包含文件系统的基本信息和操作表。
 */
struct filesystem_type {
    /** @brief 文件系统名称（如 "ext2", "xv6"） */
    char name[16];
    
    /** @brief 文件系统类型标识（FS_XV6, FS_EXT2 等） */
    int fs_type;
    
    /** @brief 根目录 inode 号 */
    uint32 root_inum;
    
    /** @brief inode 操作表指针 */
    struct inode_operations *i_op;
    
    /** @brief 文件操作表指针 */
    struct file_operations *f_op;
    
    /**
     * @brief 检测设备是否为此文件系统类型
     * @param dev 设备号
     * @return 是返回 1，否返回 0
     */
    int (*probe)(int dev);
    
    /**
     * @brief 读取超级块（可选）
     * @param dev 设备号
     * @param sb  超级块结构体指针
     * @return 成功返回 0，失败返回 -1
     */
    int (*read_super)(int dev, struct superblock *sb);
};

/*============================================================================
 *                          挂载点结构
 *===========================================================================*/

/**
 * @brief 挂载点结构
 * 
 * 描述一个文件系统挂载实例，包含挂载路径、设备信息和操作表。
 */
struct mount {
    /** @brief 挂载点路径（如 "/mnt"） */
    char path[MAXPATH];
    
    /** @brief 设备号 */
    int dev;
    
    /** @brief 文件系统类型 */
    int fs_type;
    
    /** @brief 根目录 inode 号 */
    uint32 root_inum;
    
    /** @brief inode 操作表指针 */
    struct inode_operations *i_op;
    
    /** @brief 文件操作表指针 */
    struct file_operations *f_op;
    
    /** @brief 链表下一个挂载点 */
    struct mount *next;
};

/*============================================================================
 *                          Dentry Cache 结构
 *===========================================================================*/

/**
 * @brief Dentry Cache 条目结构
 * 
 * 缓存目录项查找结果，加速路径解析。
 * 使用 LRU 策略进行缓存替换。
 */
struct dentry {
    /** @brief 条目是否有效 */
    int valid;
    
    /** @brief 设备号 */
    int dev;
    
    /** @brief 父目录 inode 号 */
    uint parent_inum;
    
    /** @brief 文件/目录名 */
    char name[DENTRY_NAME_LEN];
    
    /** @brief 对应的 inode 号 */
    uint inum;
    
    /** @brief LRU 时间戳 */
    uint lru_tick;
    
    /** @brief 哈希链表下一个节点 */
    struct dentry *hash_next;
};

/*============================================================================
 *                          VFS 全局接口声明
 *===========================================================================*/

/* VFS 核心管理 */
void            vfs_init(void);
int             register_filesystem(struct filesystem_type *fs);
int             unregister_filesystem(int fs_type);
struct filesystem_type* get_filesystem(int fs_type);

/* 挂载点管理 */
int             mount_fs(int dev, int fs_type, const char *path);
int             umount_fs(const char *path);
struct mount*   find_mount(const char *path);
int             translate_path(const char *full_path, char *rel_path, int *dev, int *fs_type);

/* Dentry Cache 接口 */
void            dcache_init(void);
struct dentry*  dcache_lookup(int dev, uint parent_inum, const char *name);
void            dcache_add(int dev, uint parent_inum, const char *name, uint inum);
void            dcache_invalidate(int dev, uint parent_inum, const char *name);
void            dcache_invalidate_dev(int dev);

#endif /* _VFS_H_ */
