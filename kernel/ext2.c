/**
 * @file ext2.c
 * @brief EXT2 文件系统驱动实现
 * 
 * 本文件实现了 EXT2 文件系统的核心功能，包括：
 * - 超级块和块组描述符的读写
 * - 索引节点（inode）的读写和分配
 * - 目录项的查找、创建和删除
 * - 文件数据块的读写
 * - VFS 接口适配器
 * 
 * @author xv6 VFS Extension Team
 * @version 1.0
 * @date 2026-01-13
 * 
 * @note 本实现仅支持 EXT2 的基本功能，不支持扩展属性和日志
 */

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "fs.h"
#include "file.h"
#include "ext2.h"
#include "buf.h"
#include <stddef.h>

/** @brief 块大小，EXT2 默认为 1024 字节 */
#define BSIZE 1024

/*============================================================================
 *                          字符串工具函数
 *===========================================================================*/

/**
 * @brief 比较两个字符串
 * 
 * @param s1 第一个字符串
 * @param s2 第二个字符串
 * @return int 返回值：0 表示相等，正数表示 s1 > s2，负数表示 s1 < s2
 */
int strcmp(const char *s1, const char *s2) {
  while(*s1 && *s2 && *s1 == *s2) {
    s1++;
    s2++;
  }
  return *s1 - *s2;
}

/*============================================================================
 *                          超级块操作
 *===========================================================================*/

/**
 * @brief 读取 EXT2 超级块
 * 
 * 从指定设备读取 EXT2 文件系统的超级块信息。
 * EXT2 超级块位于设备的第 1 块（偏移 1024 字节）。
 * 
 * @param dev 设备号
 * @param sb  超级块结构体指针，用于存储读取的数据
 * @return int 成功返回 0，失败返回 -1
 * 
 * @note 函数会验证 EXT2 魔数（0xEF53）以确认文件系统类型
 */
int ext2_read_super(int dev, struct ext2_superblock *sb) {
    struct buf *bp;
    uint32 magic;
    
    /* 读取超级块所在的磁盘块（块号 1） */
    bp = bread(dev, 1);
    if(!bp || !bp->valid) {
        if(bp) brelse(bp);
        return -1;
    }
    
    /* 验证 EXT2 魔数，偏移量为 56 字节 */
    magic = *((uint16*)(bp->data + 56));
    if(magic != EXT2_SUPER_MAGIC) {
        brelse(bp);
        return -1;
    }
    
    /* 复制超级块数据到输出结构体 */
    memmove(sb, bp->data, sizeof(struct ext2_superblock));
    brelse(bp);
    
    return 0;
}

/**
 * @brief 打印 EXT2 超级块信息（调试用）
 * 
 * @param sb 超级块结构体指针
 */
void ext2_print_super(struct ext2_superblock *sb) {
    printf("EXT2 Superblock:\n");
    printf("  Magic: 0x%x\n", sb->s_magic);
    printf("  Inodes count: %d\n", sb->s_inodes_count);
    printf("  Blocks count: %d\n", sb->s_blocks_count);
    printf("  Block size: %d\n", 1024 << sb->s_log_block_size);
}

/**
 * @brief 写入 EXT2 超级块
 * 
 * 将修改后的超级块数据写回磁盘。
 * 
 * @param dev 设备号
 * @param sb  超级块结构体指针
 */
static void
ext2_write_superblock(int dev, struct ext2_superblock *sb) {
    struct buf *bp = bread(dev, 1);
    if (!bp) return;
    memmove(bp->data, sb, sizeof(struct ext2_superblock));
    log_write(bp);
    brelse(bp);
}

/*============================================================================
 *                          块组描述符操作
 *===========================================================================*/

/**
 * @brief 写入块组描述符
 * 
 * 将修改后的块组描述符写回磁盘。
 * 
 * @param dev   设备号
 * @param group 块组编号
 * @param gd    块组描述符结构体指针
 */
static void
ext2_write_group_desc(int dev, uint32 group, struct ext2_group_desc *gd) {
    struct buf *bp = bread(dev, 2);
    if (!bp) return;
    memmove(bp->data + group * sizeof(struct ext2_group_desc), gd, sizeof(struct ext2_group_desc));
    log_write(bp);
    brelse(bp);
}

/**
 * @brief 在位图中标记一个空闲位为已使用
 * 
 * 遍历位图查找第一个空闲位并将其标记为已使用。
 * 
 * @param bp    位图块的缓冲区
 * @param limit 搜索的最大位数
 * @param res   输出参数，返回找到的位索引
 * @return int  成功返回 0，失败返回 -1
 */
static int
ext2_mark_used(struct buf *bp, uint32 limit, uint32 *res) {
    uint8 *data = (uint8*)bp->data;
    for(uint32 i = 0; i < limit; i++) {
        uint32 byte = i / 8;
        uint32 bit = i % 8;
        uint8 mask = 1 << bit;
        if(!(data[byte] & mask)) {
            data[byte] |= mask;
            log_write(bp);
            *res = i;
            return 0;
        }
    }
    return -1;
}

/**
 * @brief 读取块组描述符
 * 
 * 从磁盘读取指定块组的描述符信息。
 * 块组描述符表位于超级块之后（块号 2）。
 * 
 * @param dev   设备号
 * @param group 块组编号
 * @param gd    块组描述符结构体指针，用于存储读取的数据
 * @return int  成功返回 0，失败返回 -1
 */
int ext2_read_group_desc(int dev, uint32 group, struct ext2_group_desc *gd) {
    struct buf *bp;
    uint32 block_num;
    
    /* 块组描述符表在块 2（紧跟超级块） */
    block_num = 2;
    
    bp = bread(dev, block_num);
    if(!bp || !bp->valid) {
        if(bp) brelse(bp);
        return -1;
    }
    
    /* 复制指定块组的描述符 */
    memmove(gd, bp->data + group * sizeof(struct ext2_group_desc), sizeof(struct ext2_group_desc));
    brelse(bp);
    
    return 0;
}

/*============================================================================
 *                          索引节点（Inode）操作
 *===========================================================================*/

/**
 * @brief 更新磁盘上的 EXT2 inode
 * 
 * 将内存中的 inode 信息同步写入磁盘。
 * 
 * @param ip 要更新的 inode 指针
 */
void
ext2_iupdate(struct inode *ip) {
    struct ext2_superblock sb;
    struct ext2_inode inode;
    
    if(ext2_read_super(ip->dev, &sb) < 0)
        return;
    if(ext2_read_inode(ip->dev, ip->inum, &inode) < 0)
        return;
    
    /* 同步 inode 元数据 */
    inode.i_size = ip->size;
    inode.i_links_count = ip->nlink;
    memmove(inode.i_block, ip->ext2.i_block, sizeof(inode.i_block));
    inode.i_flags = ip->ext2.i_flags;
    
    uint32 inode_size = sb.s_inode_size ? sb.s_inode_size : sizeof(struct ext2_inode);
    ext2_write_inode(ip->dev, ip->inum, &inode, inode_size);
}

/**
 * @brief 截断 EXT2 文件
 * 
 * 将文件大小重置为 0。
 * 
 * @param ip 要截断的 inode 指针
 * 
 * @todo 完善数据块释放逻辑
 */
void
ext2_itrunc(struct inode *ip)
{
    ip->size = 0;
    ext2_iupdate(ip);
}

/**
 * @brief 读取 EXT2 inode
 * 
 * 从磁盘读取指定 inode 号的 inode 结构。
 * 
 * @param dev          设备号
 * @param inode        inode 编号（从 1 开始）
 * @param inode_struct inode 结构体指针，用于存储读取的数据
 * @return int         成功返回 0，失败返回 -1
 * 
 * @note EXT2 的 inode 编号从 1 开始，0 表示无效
 */
int ext2_read_inode(int dev, uint32 inode, struct ext2_inode *inode_struct) {
    struct buf *bp;
    struct ext2_superblock sb;
    struct ext2_group_desc gd;
    uint32 block_group, inode_index, block_num, offset;
    uint32 inode_size;
    
    /* 读取超级块获取文件系统参数 */
    if(ext2_read_super(dev, &sb) < 0) {
        return -1;
    }

    inode_size = sb.s_inode_size ? sb.s_inode_size : sizeof(struct ext2_inode);
    
    /* 计算 inode 所在的块组 */
    block_group = (inode - 1) / sb.s_inodes_per_group;
    
    /* 读取块组描述符获取 inode 表位置 */
    if(ext2_read_group_desc(dev, block_group, &gd) < 0) {
        return -1;
    }
    
    /* 计算 inode 在块组内的索引 */
    inode_index = (inode - 1) % sb.s_inodes_per_group;
    
    /* 计算 inode 所在的磁盘块号 */
    block_num = gd.bg_inode_table + (inode_index * inode_size) / BSIZE;
    
    /* 读取包含该 inode 的磁盘块 */
    bp = bread(dev, block_num);
    if(!bp || !bp->valid) {
        if(bp) brelse(bp);
        return -1;
    }
    
    /* 计算 inode 在块内的偏移量 */
    offset = (inode_index * inode_size) % BSIZE;
    
    /* 复制 inode 数据 */
    memmove(inode_struct, bp->data + offset, sizeof(struct ext2_inode));
    brelse(bp);
    
    return 0;
}

/**
 * @brief 写入 EXT2 inode
 * 
 * 将 inode 结构写入磁盘。
 * 
 * @param dev          设备号
 * @param inode        inode 编号
 * @param inode_struct inode 结构体指针
 * @param inode_size   inode 大小（字节）
 * @return int         成功返回 0，失败返回 -1
 */
int
ext2_write_inode(int dev, uint32 inode, struct ext2_inode *inode_struct, uint32 inode_size)
{
    struct buf *bp;
    struct ext2_superblock sb;
    struct ext2_group_desc gd;
    uint32 block_group, inode_index, block_num, offset;

    if(ext2_read_super(dev, &sb) < 0)
        return -1;
    
    inode_size = inode_size ? inode_size : sizeof(struct ext2_inode);
    block_group = (inode - 1) / sb.s_inodes_per_group;
    
    if(ext2_read_group_desc(dev, block_group, &gd) < 0)
        return -1;
    
    inode_index = (inode - 1) % sb.s_inodes_per_group;
    block_num = gd.bg_inode_table + (inode_index * inode_size) / BSIZE;
    offset = (inode_index * inode_size) % BSIZE;

    bp = bread(dev, block_num);
    if(!bp || !bp->valid) {
        if(bp) brelse(bp);
        return -1;
    }
    
    memmove(bp->data + offset, inode_struct, sizeof(struct ext2_inode));
    log_write(bp);
    brelse(bp);
    return 0;
}

/*============================================================================
 *                          块分配操作
 *===========================================================================*/

/**
 * @brief 分配一个数据块
 * 
 * 在 EXT2 文件系统中分配一个空闲的数据块。
 * 
 * @param dev    设备号
 * @param result 输出参数，返回分配的块号
 * @return int   成功返回 0，失败返回 -1
 */
int
ext2_alloc_block(int dev, uint32 *result)
{
    struct ext2_superblock sb;
    struct ext2_group_desc gd;
    
    if(ext2_read_super(dev, &sb) < 0) {
        return -1;
    }
    
    /* 目前只在块组 0 中分配 */
    uint32 group = 0;
    if(ext2_read_group_desc(dev, group, &gd) < 0) {
        return -1;
    }
    
    uint32 limit = sb.s_blocks_per_group;
    struct buf *bp = bread(dev, gd.bg_block_bitmap);
    if(!bp || !bp->valid) {
        if(bp) brelse(bp);
        return -1;
    }
    
    /* 在位图中查找并标记空闲块 */
    uint32 blk_idx;
    if(ext2_mark_used(bp, limit, &blk_idx) < 0) {
        brelse(bp);
        return -1;
    }
    brelse(bp);
    
    /* 更新块组描述符和超级块的空闲块计数 */
    gd.bg_free_blocks_count--;
    sb.s_free_blocks_count--;
    ext2_write_group_desc(dev, group, &gd);
    ext2_write_superblock(dev, &sb);
    
    /* 计算实际的块号 */
    *result = sb.s_first_data_block + group * sb.s_blocks_per_group + blk_idx;
    return 0;
}

/**
 * @brief 分配一个 inode
 * 
 * 在 EXT2 文件系统中分配一个空闲的 inode。
 * 
 * @param dev    设备号
 * @param type   文件类型（T_FILE, T_DIR 等）
 * @param result 输出参数，返回分配的 inode 号
 * @return int   成功返回 0，失败返回 -1
 */
int
ext2_alloc_inode(int dev, uint32 type, uint32 *result)
{
    struct ext2_superblock sb;
    struct ext2_group_desc gd;
    
    if(ext2_read_super(dev, &sb) < 0) {
        return -1;
    }
    
    /* 目前只在块组 0 中分配 */
    uint32 group = 0;
    if(ext2_read_group_desc(dev, group, &gd) < 0) {
        return -1;
    }
    
    uint32 limit = sb.s_inodes_per_group;
    struct buf *bp = bread(dev, gd.bg_inode_bitmap);
    if(!bp || !bp->valid) {
        if(bp) brelse(bp);
        return -1;
    }
    
    /* 在位图中查找并标记空闲 inode */
    uint32 ino_idx;
    if(ext2_mark_used(bp, limit, &ino_idx) < 0) {
        brelse(bp);
        return -1;
    }
    brelse(bp);
    
    /* 更新块组描述符和超级块的空闲 inode 计数 */
    gd.bg_free_inodes_count--;
    sb.s_free_inodes_count--;
    ext2_write_group_desc(dev, group, &gd);
    ext2_write_superblock(dev, &sb);
    
    /* 计算实际的 inode 号（从 1 开始） */
    *result = group * sb.s_inodes_per_group + ino_idx + 1;
    return 0;
}

/*============================================================================
 *                          目录操作
 *===========================================================================*/

/**
 * @brief 在目录中添加目录项
 * 
 * 在指定目录中创建一个新的目录项，将名称与 inode 关联。
 * 
 * @param dp   父目录的 inode
 * @param name 新条目的名称
 * @param inum 新条目的 inode 号
 * @param type 文件类型（T_FILE, T_DIR 等）
 * @return int 成功返回 0，失败返回 -1
 * 
 * @note 如果 type 为 T_DIR，会自动创建 "." 和 ".." 条目
 */
int
ext2_dirlink(struct inode *dp, char *name, uint inum, short type)
{
    struct ext2_inode di;
    struct buf *bp;
    uint32 block, offset, name_len, needed_len;
    struct ext2_dir_entry *entry, *new_entry;

    name_len = strlen(name);
    if(name_len > 255) return -1;
    
    /* 计算目录项所需空间：8 字节头 + 名称 + 4 字节对齐 */
    needed_len = (8 + name_len + 3) & ~3;

    if(ext2_read_inode(dp->dev, dp->inum, &di) < 0) return -1;

    /* 遍历直接块查找或创建空间 */
    for(block = 0; block < 12; block++) {
        uint32 bnum = di.i_block[block];
        
        /* 需要分配新块 */
        if(bnum == 0) {
            if(ext2_alloc_block(dp->dev, &bnum) < 0) return -1;
            di.i_block[block] = bnum;
            di.i_size += BSIZE;
            dp->size = di.i_size;
            ext2_iupdate(dp);
            
            /* 初始化新块并创建目录项 */
            bp = bread(dp->dev, bnum);
            memset(bp->data, 0, BSIZE);
            new_entry = (struct ext2_dir_entry*)bp->data;
            new_entry->inode = inum;
            new_entry->rec_len = BSIZE;
            new_entry->name_len = name_len;
            new_entry->file_type = (type == T_DIR) ? EXT2_FT_DIR : EXT2_FT_REG_FILE;
            memmove(new_entry->name, name, name_len);
            log_write(bp);
            brelse(bp);

            /* 为新目录创建 . 和 .. 条目 */
            if(type == T_DIR) {
                struct inode *nip = iget(dp->dev, inum);
                ilock(nip);
                ext2_dirlink(nip, ".", inum, T_DIR);
                ext2_dirlink(nip, "..", dp->inum, T_DIR);
                nip->nlink++;
                ext2_iupdate(nip);
                iunlock(nip);
                dp->nlink++;
                ext2_iupdate(dp);
            }
            return 0;
        }

        /* 在现有块中查找空间 */
        bp = bread(dp->dev, bnum);
        offset = 0;
        while(offset < BSIZE) {
            entry = (struct ext2_dir_entry*)(bp->data + offset);
            uint32 actual_len = (8 + entry->name_len + 3) & ~3;
            
            /* 复用已删除的条目 */
            if(entry->inode == 0 && entry->rec_len >= needed_len) {
                entry->inode = inum;
                entry->name_len = name_len;
                entry->file_type = (type == T_DIR) ? EXT2_FT_DIR : EXT2_FT_REG_FILE;
                memmove(entry->name, name, name_len);
                log_write(bp);
                brelse(bp);
                return 0;
            }

            /* 分割现有条目的剩余空间 */
            if(entry->rec_len >= actual_len + needed_len) {
                uint32 old_rec_len = entry->rec_len;
                entry->rec_len = actual_len;
                
                new_entry = (struct ext2_dir_entry*)(bp->data + offset + actual_len);
                new_entry->inode = inum;
                new_entry->rec_len = old_rec_len - actual_len;
                new_entry->name_len = name_len;
                new_entry->file_type = (type == T_DIR) ? EXT2_FT_DIR : EXT2_FT_REG_FILE;
                memmove(new_entry->name, name, name_len);
                
                log_write(bp);
                brelse(bp);

                /* 为新目录创建 . 和 .. 条目 */
                if(type == T_DIR) {
                    struct inode *nip = iget(dp->dev, inum);
                    ilock(nip);
                    ext2_dirlink(nip, ".", inum, T_DIR);
                    ext2_dirlink(nip, "..", dp->inum, T_DIR);
                    nip->nlink++;
                    ext2_iupdate(nip);
                    iunlock(nip);
                    dp->nlink++;
                    ext2_iupdate(dp);
                }
                return 0;
            }
            offset += entry->rec_len;
            if(entry->rec_len == 0) break;
        }
        brelse(bp);
    }

    return -1;
}

/**
 * @brief 在目录中查找指定名称的条目
 * 
 * @param dev       设备号
 * @param dir_inode 目录的 inode 号
 * @param name      要查找的名称
 * @param inode_num 输出参数，返回找到的 inode 号
 * @return int      成功返回 0，未找到返回 -1
 */
int ext2_find_entry(int dev, uint32 dir_inode, const char *name, uint32 *inode_num) {
    struct ext2_inode inode;
    struct buf *bp;
    uint32 block, offset, name_len;
    struct ext2_dir_entry *entry;
    char entry_name[256];
    
    /* 读取目录 inode */
    if(ext2_read_inode(dev, dir_inode, &inode) < 0) {
        return -1;
    }
    
    /* 验证是否为目录 */
    if((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
        return -1;
    }
    
    /* 遍历目录的所有数据块 */
    for(block = 0; block < 12 && inode.i_block[block] != 0; block++) {
        bp = bread(dev, inode.i_block[block]);
        if(!bp || !bp->valid) {
            if(bp) brelse(bp);
            continue;
        }
        
        /* 遍历块中的所有目录项 */
        offset = 0;
        while(offset < BSIZE) {
            entry = (struct ext2_dir_entry *)(bp->data + offset);
            
            /* 跳过已删除的条目 */
            if(entry->inode == 0) {
                if(entry->rec_len == 0) break;
                offset += entry->rec_len;
                continue;
            }
            
            /* 提取并比较文件名 */
            name_len = entry->name_len;
            if(name_len > 255) {
                name_len = 255;
            }
            memmove(entry_name, entry->name, name_len);
            entry_name[name_len] = '\0';
            
            if(strcmp(entry_name, name) == 0) {
                *inode_num = entry->inode;
                brelse(bp);
                return 0;
            }
            
            offset += entry->rec_len;
            if(entry->rec_len == 0) break;
        }
        
        brelse(bp);
    }
    
    return -1;
}

/**
 * @brief 从目录中删除指定条目
 * 
 * @param dp   目录的 inode
 * @param name 要删除的条目名称
 * @return int 成功返回 0，失败返回 -1
 */
int
ext2_unlink(struct inode *dp, char *name)
{
    uint32 inum;
    struct buf *bp;
    struct ext2_dir_entry *entry;
    struct ext2_inode di;

    if(ext2_read_inode(dp->dev, dp->inum, &di) < 0) return -1;

    /* 遍历目录块查找目标条目 */
    for(int block = 0; block < 12 && di.i_block[block] != 0; block++) {
        bp = bread(dp->dev, di.i_block[block]);
        uint32 offset = 0;
        
        while(offset < BSIZE) {
            entry = (struct ext2_dir_entry *)(bp->data + offset);
            char entry_name[256];
            int name_len = entry->name_len > 255 ? 255 : entry->name_len;
            memmove(entry_name, entry->name, name_len);
            entry_name[name_len] = '\0';

            if(entry->inode != 0 && strcmp(entry_name, name) == 0) {
                inum = entry->inode;
                entry->inode = 0;  /* 标记为已删除 */
    log_write(bp);
    brelse(bp);
    
                /* 减少目标文件的链接计数 */
                struct inode *ip = iget(dp->dev, inum);
                ilock(ip);
                if(ip->nlink > 0) {
                    ip->nlink--;
                    ext2_iupdate(ip);
                }
                iunlockput(ip);
    return 0;
}
            offset += entry->rec_len;
            if(entry->rec_len == 0) break;
        }
        brelse(bp);
    }
    return -1;
}

/**
 * @brief 解析路径并查找目标 inode
 * 
 * 支持多级目录路径解析，如 "/dir1/dir2/file"。
 * 
 * @param dev       设备号
 * @param path      路径字符串
 * @param inode_num 输出参数，返回目标的 inode 号
 * @return int      成功返回 0，失败返回 -1
 * 
 * @note EXT2 根目录的 inode 号固定为 2
 */
int ext2_find_path(int dev, const char *path, uint32 *inode_num) {
    struct ext2_inode inode;
    char name[256];
    const char *ptr;
    int ret;
    
    /* 从根目录开始（inode 2） */
    *inode_num = 2;
    
    /* 跳过开头的 '/' */
    if (*path == '/') {
        path++;
    }
    
    /* 空路径表示根目录 */
    if (*path == '\0') {
        return 0;
    }
    
    /* 逐级解析路径组件 */
    ptr = path;
    while (*ptr != '\0') {
        /* 提取下一个路径组件 */
        int i = 0;
        while (*ptr != '/' && *ptr != '\0' && i < 255) {
            name[i++] = *ptr++;
        }
        name[i] = '\0';
        
        /* 在当前目录中查找 */
        ret = ext2_find_entry(dev, *inode_num, name, inode_num);
        if (ret < 0) {
            return ret;
        }
        
        /* 如果还有更多路径，验证当前是目录 */
        if (*ptr == '/') {
            ret = ext2_read_inode(dev, *inode_num, &inode);
            if (ret < 0) {
                return ret;
            }
            
            if ((inode.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
                return -1;
            }
            
            ptr++;  /* 跳过 '/' */
        }
    }
    
    return 0;
}

/*============================================================================
 *                          文件读写操作
 *===========================================================================*/

/**
 * @brief 写入文件数据
 * 
 * 将数据写入 EXT2 文件，支持自动扩展文件大小。
 * 
 * @param ip       文件的 inode
 * @param user_src 是否从用户空间读取数据
 * @param src      源数据地址
 * @param offset   写入起始偏移量
 * @param size     写入字节数
 * @return int     成功返回写入的字节数，失败返回 -1
 */
int ext2_writei(struct inode *ip, int user_src, uint64 src, uint32 offset, uint32 size);

int
ext2_writei(struct inode *ip, int user_src, uint64 src, uint32 offset, uint32 size)
{
    int dev = ip->dev;
    struct buf *bp;
    uint32 block_size = BSIZE;
    uint32 bytes_written = 0;
    
    while(size > 0) {
        uint32 block_offset = offset % block_size;
        uint32 bytes_to_write = block_size - block_offset;
        if(bytes_to_write > size)
            bytes_to_write = size;
        
        uint32 bn = offset / block_size;
        uint32 disk_block = 0;
        
        /* 直接块（0-11） */
        if(bn < 12) {
            disk_block = ip->ext2.i_block[bn];
            if(disk_block == 0) {
                /* 分配新块 */
                if(ext2_alloc_block(dev, &disk_block) < 0)
                    return -1;
                ip->ext2.i_block[bn] = disk_block;
                ext2_iupdate(ip);
            }
        } else {
            /* 一级间接块（12-267） */
            uint32 indirect = ip->ext2.i_block[12];
            if(indirect != 0) {
                bp = bread(dev, indirect);
                if(!bp || !bp->valid) {
                    if(bp) brelse(bp);
                    return -1;
                }
                uint32 *ptr = (uint32*)bp->data;
                disk_block = ptr[bn - 12];
                brelse(bp);
            }
        }
        
        if(disk_block == 0)
            return -1;
        
        /* 读取块、写入数据、回写 */
        bp = bread(dev, disk_block);
        if(!bp || !bp->valid) {
            if(bp) brelse(bp);
            return -1;
        }
        
        if(either_copyin(bp->data + block_offset, user_src, src, bytes_to_write) < 0) {
            brelse(bp);
            return -1;
        }
        
        log_write(bp);
        brelse(bp);
        
        offset += bytes_to_write;
        src += bytes_to_write;
        size -= bytes_to_write;
        bytes_written += bytes_to_write;
    }
    
    /* 更新文件大小 */
    if(offset > ip->size)
        ip->size = offset;
    ext2_iupdate(ip);
    
    return bytes_written;
}

/**
 * @brief 读取文件数据
 * 
 * 从 EXT2 文件读取数据。对于目录，会自动转换为 xv6 的 dirent 格式。
 * 
 * @param ip       文件的 inode
 * @param user_dst 是否写入用户空间
 * @param dst      目标缓冲区地址
 * @param offset   读取起始偏移量
 * @param size     请求读取的字节数
 * @return int     成功返回读取的字节数，失败返回 -1，EOF 返回 0
 */
int ext2_readi(struct inode *ip, int user_dst, uint64 dst, uint32 offset, uint32 size) {
    struct buf *bp, *indirect_bp;
    uint32 block_size, block_offset, bytes_to_read, bytes_read;
    uint32 *indirect_ptr;
    int dev = ip->dev;
    
    /* 目录读取：转换为 xv6 dirent 格式 */
    if (ip->type == T_DIR) {
        struct ext2_dir_entry *entry;
        uint32 current_off = 0;
        int target_index = offset / sizeof(struct dirent);
        int current_index = 0;
        struct dirent xv6_de;
        
        /* 遍历目录块 */
        for (int i = 0; i < 12 && ip->ext2.i_block[i] != 0; i++) {
            bp = bread(dev, ip->ext2.i_block[i]);
            if(!bp || !bp->valid) {
                if(bp) brelse(bp);
                continue;
            }
            
            current_off = 0;
            while (current_off < BSIZE) {
                entry = (struct ext2_dir_entry *)(bp->data + current_off);
                if (entry->inode != 0) {
                    if (current_index == target_index) {
                        /* 转换为 xv6 dirent 格式 */
                        memset(&xv6_de, 0, sizeof(xv6_de));
                        xv6_de.inum = (ushort)entry->inode;
                        int name_len = entry->name_len;
                        if (name_len > 14) name_len = 14;
                        memmove(xv6_de.name, entry->name, name_len);
                        
                        if (either_copyout(user_dst, dst, &xv6_de, sizeof(xv6_de)) < 0) {
                            brelse(bp);
                            return -1;
                        }
                        brelse(bp);
                        return sizeof(xv6_de);
                    }
                    current_index++;
                }
                current_off += entry->rec_len;
                if (entry->rec_len == 0) break;
            }
            brelse(bp);
        }
        return 0;  /* EOF */
    }

    /* 普通文件读取 */
    if(offset >= ip->size) {
        return 0;  /* EOF */
    }
    if(offset + size > ip->size) {
        size = ip->size - offset;
    }
    
    block_size = BSIZE;
    bytes_read = 0;
    
    while(size > 0) {
        block_offset = offset % block_size;
        bytes_to_read = block_size - block_offset;
        if(bytes_to_read > size) bytes_to_read = size;

        uint32 bn = offset / block_size;
        uint32 disk_block = 0;

        /* 直接块（0-11） */
        if(bn < 12) {
            disk_block = ip->ext2.i_block[bn];
        } 
        /* 一级间接块（12-267） */
        else if(bn < 12 + 256) {
            uint32 indirect_bn = ip->ext2.i_block[12];
            if(indirect_bn != 0) {
                indirect_bp = bread(dev, indirect_bn);
                if(indirect_bp && indirect_bp->valid) {
                    indirect_ptr = (uint32*)indirect_bp->data;
                    disk_block = indirect_ptr[bn - 12];
                }
                if(indirect_bp) brelse(indirect_bp);
            }
        } else {
            break;  /* 不支持更深层的间接块 */
        }

        if(disk_block == 0) {
            /* 稀疏文件：读取时返回零 */
            uchar zero = 0;
            for(int k = 0; k < bytes_to_read; k++) {
                if(either_copyout(user_dst, dst + k, &zero, 1) < 0) break;
            }
        } else {
            bp = bread(dev, disk_block);
            if(!bp || !bp->valid) {
                if(bp) brelse(bp);
                break;
            }
            if(either_copyout(user_dst, dst, bp->data + block_offset, bytes_to_read) < 0) {
                brelse(bp);
                break;
            }
            brelse(bp);
        }

        offset += bytes_to_read;
        dst += bytes_to_read;
        size -= bytes_to_read;
        bytes_read += bytes_to_read;
    }
    
    return bytes_read;
}

/*============================================================================
 *                          VFS 接口适配器
 *===========================================================================*/

/**
 * @brief VFS lookup 接口适配器
 * 
 * 在目录中查找指定名称的文件/目录。
 * 
 * @param dp   父目录 inode
 * @param name 要查找的名称
 * @param poff 偏移量输出（未使用）
 * @return struct inode* 成功返回目标 inode，失败返回 NULL
 */
struct inode*
ext2_lookup_vfs(struct inode *dp, char *name, uint *poff)
{
  uint32 inum;
  if (ext2_find_entry(dp->dev, dp->inum, name, &inum) < 0)
    return 0;
  return iget(dp->dev, inum);
}

/**
 * @brief VFS stat 接口适配器
 * 
 * 获取文件状态信息。
 * 
 * @param ip 目标 inode
 * @param st stat 结构体指针
 */
void
ext2_stat_vfs(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

/*============================================================================
 *                          操作表定义
 *===========================================================================*/

/** @brief EXT2 inode 操作表 */
struct inode_operations ext2_i_op = {
    .lookup  = ext2_lookup_vfs,
    .link    = ext2_dirlink,
    .unlink  = (int (*)(struct inode*, char*))ext2_unlink,
    .mkdir   = 0,
    .itrunc  = ext2_itrunc,
};

/** @brief EXT2 文件操作表 */
struct file_operations ext2_f_op = {
    .read    = ext2_readi,
    .write   = ext2_writei,
    .stat    = ext2_stat_vfs,
  .iupdate = ext2_iupdate,
};

/** @brief EXT2 文件系统类型定义 */
struct filesystem_type ext2_fs_type = {
    .name       = "ext2",
    .fs_type    = FS_EXT2,
    .root_inum  = 2,
    .i_op       = &ext2_i_op,
    .f_op       = &ext2_f_op,
    .probe      = 0,
  .read_super = 0,
};
