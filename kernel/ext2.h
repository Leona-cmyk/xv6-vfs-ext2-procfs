/**
 * @file ext2.h
 * @brief EXT2 文件系统数据结构与接口定义
 * 
 * 本文件定义了 EXT2 文件系统的核心数据结构，包括：
 * - 超级块（superblock）
 * - 块组描述符（group descriptor）
 * - 索引节点（inode）
 * - 目录项（directory entry）
 * 
 * EXT2 是 Linux 第二代扩展文件系统，本实现为简化版本，
 * 仅包含基本功能所需的字段，足以支持文件读写和目录操作。
 * 
 * @author xv6 VFS Extension Team
 * @version 1.0
 * @date 2026-01-13
 * 
 * @see ext2.c - EXT2 文件系统驱动实现
 */

#ifndef _EXT2_H_
#define _EXT2_H_

#include "types.h"

/*============================================================================
 *                          EXT2 超级块结构
 *===========================================================================*/

/**
 * @brief EXT2 超级块结构（简化版）
 * 
 * 超级块位于文件系统的第 1 块（偏移 1024 字节），
 * 包含文件系统的全局元数据信息。
 * 
 * @note 完整的 EXT2 超级块为 1024 字节，此处仅定义必要字段
 */
struct ext2_superblock {
    uint32 s_inodes_count;      /**< 文件系统中 inode 总数 */
    uint32 s_blocks_count;      /**< 文件系统中块总数 */
    uint32 s_r_blocks_count;    /**< 为超级用户保留的块数 */
    uint32 s_free_blocks_count; /**< 空闲块数量 */
    uint32 s_free_inodes_count; /**< 空闲 inode 数量 */
    uint32 s_first_data_block;  /**< 第一个数据块号（通常为 0 或 1） */
    uint32 s_log_block_size;    /**< 块大小 = 1024 << s_log_block_size */
    uint32 s_log_frag_size;     /**< 片段大小对数（已弃用） */
    uint32 s_blocks_per_group;  /**< 每个块组包含的块数 */
    uint32 s_frags_per_group;   /**< 每个块组包含的片段数 */
    uint32 s_inodes_per_group;  /**< 每个块组包含的 inode 数 */
    uint32 s_mtime;             /**< 最后挂载时间（Unix 时间戳） */
    uint32 s_wtime;             /**< 最后写入时间（Unix 时间戳） */
    uint16 s_mnt_count;         /**< 自上次完整检查后的挂载次数 */
    uint16 s_max_mnt_count;     /**< 强制检查前的最大挂载次数 */
    uint16 s_magic;             /**< EXT2 魔数（0xEF53） */
    uint16 s_state;             /**< 文件系统状态 */
    uint16 s_errors;            /**< 检测到错误时的行为 */
    uint16 s_minor_rev_level;   /**< 次要修订级别 */
    uint32 s_lastcheck;         /**< 最后检查时间 */
    uint32 s_checkinterval;     /**< 检查间隔 */
    uint32 s_creator_os;        /**< 创建此文件系统的操作系统 */
    uint32 s_rev_level;         /**< 修订级别 */
    uint16 s_def_resuid;        /**< 保留块的默认用户 ID */
    uint16 s_def_resgid;        /**< 保留块的默认组 ID */
    uint32 s_first_ino;         /**< 第一个非保留 inode 号 */
    uint16 s_inode_size;        /**< inode 结构大小（字节） */
    uint16 s_block_group_nr;    /**< 此超级块所在的块组号 */
    uint32 s_feature_compat;    /**< 兼容特性位图 */
    uint32 s_feature_incompat;  /**< 不兼容特性位图 */
    uint32 s_feature_ro_compat; /**< 只读兼容特性位图 */
    uint8 s_uuid[16];           /**< 文件系统 UUID */
    char s_volume_name[16];     /**< 卷名 */
    char s_last_mounted[64];    /**< 最后挂载路径 */
    uint32 s_algo_bitmap;       /**< 压缩算法位图 */
};

/*============================================================================
 *                          EXT2 块组描述符
 *===========================================================================*/

/**
 * @brief EXT2 块组描述符结构
 * 
 * 块组描述符表位于超级块之后（块号 2），
 * 每个条目描述一个块组的位置信息。
 */
struct ext2_group_desc {
    uint32 bg_block_bitmap;      /**< 块位图所在块号 */
    uint32 bg_inode_bitmap;      /**< inode 位图所在块号 */
    uint32 bg_inode_table;       /**< inode 表起始块号 */
    uint16 bg_free_blocks_count; /**< 块组中空闲块数 */
    uint16 bg_free_inodes_count; /**< 块组中空闲 inode 数 */
    uint16 bg_used_dirs_count;   /**< 块组中目录数 */
    uint16 bg_pad;               /**< 填充对齐 */
    uint32 bg_reserved[3];       /**< 保留字段 */
};

/*============================================================================
 *                          EXT2 索引节点（Inode）
 *===========================================================================*/

/**
 * @brief EXT2 inode 结构
 * 
 * 每个文件/目录在磁盘上对应一个 inode，
 * 包含文件的元数据和数据块指针。
 */
struct ext2_inode {
    uint16 i_mode;        /**< 文件类型和访问权限 */
    uint16 i_uid;         /**< 文件所有者用户 ID（低 16 位） */
    uint32 i_size;        /**< 文件大小（字节） */
    uint32 i_atime;       /**< 最后访问时间 */
    uint32 i_ctime;       /**< inode 修改时间 */
    uint32 i_mtime;       /**< 文件内容修改时间 */
    uint32 i_dtime;       /**< 删除时间 */
    uint16 i_gid;         /**< 文件所有者组 ID（低 16 位） */
    uint16 i_links_count; /**< 硬链接计数 */
    uint32 i_blocks;      /**< 文件占用的 512 字节块数 */
    uint32 i_flags;       /**< 文件标志 */
    uint32 i_reserved1;   /**< 操作系统相关字段 */
    uint32 i_block[15];   /**< 数据块指针数组
                           *   - i_block[0-11]: 直接块
                           *   - i_block[12]: 一级间接块
                           *   - i_block[13]: 二级间接块
                           *   - i_block[14]: 三级间接块 */
    uint32 i_version;     /**< 文件版本（用于 NFS） */
    uint32 i_file_acl;    /**< 文件 ACL */
    uint32 i_dir_acl;     /**< 目录 ACL（或文件大小高 32 位） */
    uint32 i_faddr;       /**< 片段地址（已弃用） */
    uint8 i_frag;         /**< 片段号 */
    uint8 i_fsize;        /**< 片段大小 */
    uint16 i_pad1;        /**< 填充 */
    uint32 i_reserved2[2];/**< 保留字段 */
};

/*============================================================================
 *                          EXT2 目录项
 *===========================================================================*/

/**
 * @brief EXT2 目录项结构
 * 
 * 目录文件由一系列变长目录项组成，
 * 每个目录项关联一个文件名和 inode 号。
 */
struct ext2_dir_entry {
    uint32 inode;      /**< inode 号（0 表示已删除） */
    uint16 rec_len;    /**< 目录项记录长度（到下一个条目的距离） */
    uint8 name_len;    /**< 文件名长度（不包含 NUL） */
    uint8 file_type;   /**< 文件类型（EXT2_FT_*） */
    char name[255];    /**< 文件名（不以 NUL 结尾） */
};

/*============================================================================
 *                          文件类型常量
 *===========================================================================*/

/** @brief 未知文件类型 */
#define EXT2_FT_UNKNOWN     0

/** @brief 普通文件 */
#define EXT2_FT_REG_FILE    1

/** @brief 目录 */
#define EXT2_FT_DIR         2

/** @brief 字符设备 */
#define EXT2_FT_CHRDEV      3

/** @brief 块设备 */
#define EXT2_FT_BLKDEV      4

/** @brief 命名管道（FIFO） */
#define EXT2_FT_FIFO        5

/** @brief 套接字 */
#define EXT2_FT_SOCK        6

/** @brief 符号链接 */
#define EXT2_FT_SYMLINK     7

/*============================================================================
 *                          Inode 模式常量
 *===========================================================================*/

/** @brief 文件类型掩码 */
#define EXT2_S_IFMT        0xF000

/** @brief 普通文件 */
#define EXT2_S_IFREG       0x8000

/** @brief 目录 */
#define EXT2_S_IFDIR       0x4000

/** @brief 字符设备 */
#define EXT2_S_IFCHR       0x2000

/** @brief 块设备 */
#define EXT2_S_IFBLK       0x1000

/** @brief FIFO */
#define EXT2_S_IFIFO       0x1000

/** @brief 套接字 */
#define EXT2_S_IFSOCK      0xC000

/** @brief 符号链接 */
#define EXT2_S_IFLNK       0xA000

/*============================================================================
 *                          EXT2 魔数
 *===========================================================================*/

/** @brief EXT2 超级块魔数 */
#define EXT2_SUPER_MAGIC 0xEF53

/*============================================================================
 *                          函数声明
 *===========================================================================*/

/* 超级块和块组操作 */
int ext2_read_super(int dev, struct ext2_superblock *sb);
void ext2_print_super(struct ext2_superblock *sb);
int ext2_read_group_desc(int dev, uint32 group, struct ext2_group_desc *gd);

/* Inode 操作 */
int ext2_read_inode(int dev, uint32 inode, struct ext2_inode *inode_struct);
int ext2_write_inode(int dev, uint32 inode, struct ext2_inode *inode_struct, uint32 inode_size);
void ext2_iupdate(struct inode *ip);
void ext2_itrunc(struct inode *ip);

/* 块和 inode 分配 */
int ext2_alloc_block(int dev, uint32 *result);
int ext2_alloc_inode(int dev, uint32 type, uint32 *result);

/* 目录操作 */
int ext2_find_entry(int dev, uint32 dir_inode, const char *name, uint32 *inode_num);
int ext2_find_path(int dev, const char *path, uint32 *inode_num);
int ext2_dirlink(struct inode *dp, char *name, uint inum, short type);

/* 文件读写 */
int ext2_readi(struct inode *ip, int user_dst, uint64 dst, uint32 offset, uint32 size);
int ext2_writei(struct inode *ip, int user_src, uint64 src, uint32 offset, uint32 size);

#endif /* _EXT2_H_ */
