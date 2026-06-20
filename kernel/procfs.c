/**
 * @file procfs.c
 * @brief proc 虚拟文件系统实现
 * 
 * 本文件实现了 Linux 风格的 /proc 虚拟文件系统，提供运行时系统信息查询接口。
 * 主要功能包括：
 * - /proc/uptime  - 系统运行时间（tick 数）
 * - /proc/meminfo - 内存使用情况
 * - /proc/[pid]/  - 进程信息目录
 * - /proc/[pid]/status - 进程状态信息
 * 
 * procfs 是一个纯虚拟文件系统，所有数据动态生成，不占用磁盘空间。
 * 通过 VFS 层与其他文件系统统一管理。
 * 
 * @author xv6 VFS Extension Team
 * @version 1.0
 * @date 2026-01-13
 * 
 * @see vfs.h - VFS 核心数据结构定义
 */

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "file.h"
#include "vfs.h"

/*============================================================================
 *                          Procfs Inode 号分配方案
 *===========================================================================*/

/** @brief /proc 根目录 inode 号 */
#define PROC_ROOT_INO    2000

/** @brief /proc/uptime 文件 inode 号 */
#define PROC_UPTIME_INO  2001

/** @brief /proc/meminfo 文件 inode 号 */
#define PROC_MEMINFO_INO 2002

/** @brief 进程目录 inode 基址
 * 
 * 进程目录 inode 计算方式：
 * - /proc/[pid] 目录：PROC_PID_BASE + pid * 10
 * - /proc/[pid]/status：PROC_PID_BASE + pid * 10 + 1
 */
#define PROC_PID_BASE    3000

/*============================================================================
 *                          辅助函数
 *===========================================================================*/

/**
 * @brief 将整数转换为字符串
 * 
 * @param n 要转换的整数
 * @param s 输出字符串缓冲区
 */
static void itoa(int n, char s[]) {
    int i, sign;
    
    if ((sign = n) < 0) n = -n;
    i = 0;
    
    /* 逆序生成数字 */
    do {
        s[i++] = n % 10 + '0';
    } while ((n /= 10) > 0);
    
    if (sign < 0) s[i++] = '-';
    s[i] = '\0';
    
    /* 反转字符串 */
    for (int j = 0, k = i-1; j < k; j++, k--) {
        char temp = s[j];
        s[j] = s[k];
        s[k] = temp;
    }
}

/**
 * @brief 检查字符串是否为纯数字（用于识别 PID 目录）
 * 
 * @param s 输入字符串
 * @return int 是纯数字返回 1，否则返回 0
 */
static int is_pid_str(const char *s) {
    if(*s == 0) return 0;
    while(*s) {
        if(*s < '0' || *s > '9') return 0;
        s++;
    }
    return 1;
}

/*============================================================================
 *                          Procfs 读取操作
 *===========================================================================*/

/**
 * @brief 读取 procfs 文件内容
 * 
 * 根据 inode 号识别目标文件，动态生成并返回对应内容。
 * 支持的文件类型：
 * - PROC_UPTIME_INO: 返回系统 tick 数
 * - PROC_MEMINFO_INO: 返回内存使用统计
 * - PROC_ROOT_INO: 返回 /proc 目录项
 * - 进程目录和 status 文件
 * 
 * @param ip       目标 inode
 * @param user_dst 是否写入用户空间
 * @param dst      目标地址
 * @param off      读取偏移量
 * @param n        请求字节数
 * @return int     成功返回读取字节数，失败返回 -1，EOF 返回 0
 */
int procfs_readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n) {
    char buf[512];
    int len = 0;

    /* 处理 /proc/uptime */
    if(ip->inum == PROC_UPTIME_INO) {
        acquire(&tickslock);
        uint xticks = ticks;
        release(&tickslock);
        len = snprintf(buf, sizeof(buf), "%d\n", xticks);
    }
    /* 处理 /proc/meminfo */
    else if(ip->inum == PROC_MEMINFO_INO) {
        uint64 freepages = kfreemem();
        uint64 freemem = freepages * PGSIZE;
        uint64 totalmem = PHYSTOP - KERNBASE;
        uint64 usedmem = totalmem - freemem;
        
        len = snprintf(buf, sizeof(buf),
            "MemTotal:     %d kB\n"
            "MemFree:      %d kB\n"
            "MemUsed:      %d kB\n"
            "PageSize:     %d B\n"
            "FreePages:    %d\n",
            (int)(totalmem / 1024),
            (int)(freemem / 1024),
            (int)(usedmem / 1024),
            PGSIZE,
            (int)freepages);
    }
    /* 处理 /proc/[pid]/status */
    else if(ip->inum >= PROC_PID_BASE && (ip->inum % 10 == 1)) {
        int pid = (ip->inum - PROC_PID_BASE) / 10;
        struct proc *p = getproc(pid);
        if(!p || p->state == UNUSED) return -1;
        
        char *states[] = {
            [UNUSED]    "unused",
            [USED]      "used",
            [SLEEPING]  "sleeping",
            [RUNNABLE]  "runnable",
            [RUNNING]   "running",
            [ZOMBIE]    "zombie"
        };
        len = snprintf(buf, sizeof(buf), 
            "name: %s\npid: %d\nstate: %s\n", 
            p->name, p->pid, states[p->state]);
    }
    /* 处理 /proc 根目录读取 */
    else if(ip->inum == PROC_ROOT_INO) {
        struct dirent de;
        if(off % sizeof(de) != 0 || n < sizeof(de)) return 0;
        int index = off / sizeof(de);
        
        /* 固定条目 */
        if(index == 0) {
            de.inum = PROC_ROOT_INO;
            safestrcpy(de.name, ".", sizeof(de.name));
        } else if(index == 1) {
            de.inum = PROC_ROOT_INO;
            safestrcpy(de.name, "..", sizeof(de.name));
        } else if(index == 2) {
            de.inum = PROC_UPTIME_INO;
            safestrcpy(de.name, "uptime", sizeof(de.name));
        } else if(index == 3) {
            de.inum = PROC_MEMINFO_INO;
            safestrcpy(de.name, "meminfo", sizeof(de.name));
        } else {
            /* 遍历进程表生成 PID 目录条目 */
            int pid_idx = index - 4;
            int found = 0;
            for(int i = 0; i < NPROC; i++) {
                struct proc *p = getproc(i);
                if(p && p->state != UNUSED) {
                    if(found == pid_idx) {
                        de.inum = PROC_PID_BASE + i * 10;
                        itoa(p->pid, de.name);
                        goto found_dir;
                    }
                    found++;
                }
            }
            return 0;  /* 已读取完所有条目 */
        }
found_dir:
        if(either_copyout(user_dst, dst, &de, sizeof(de)) < 0) return -1;
        return sizeof(de);
    }
    /* 处理 /proc/[pid] 进程目录读取 */
    else if(ip->inum >= PROC_PID_BASE && (ip->inum % 10 == 0)) {
        struct dirent de;
        if(off % sizeof(de) != 0 || n < sizeof(de)) return 0;
        int index = off / sizeof(de);
        
        if(index == 0) {
            de.inum = ip->inum;
            safestrcpy(de.name, ".", sizeof(de.name));
        } else if(index == 1) {
            de.inum = PROC_ROOT_INO;
            safestrcpy(de.name, "..", sizeof(de.name));
        } else if(index == 2) {
            de.inum = ip->inum + 1;
            safestrcpy(de.name, "status", sizeof(de.name));
        } else {
            return 0;
        }
        if(either_copyout(user_dst, dst, &de, sizeof(de)) < 0) return -1;
        return sizeof(de);
    }

    /* 处理普通文件内容读取 */
    if(off >= len) return 0;
    if(off + n > len) n = len - off;
    if(either_copyout(user_dst, dst, buf + off, n) < 0) return -1;
    return n;
}

/*============================================================================
 *                          Procfs 目录查找
 *===========================================================================*/

/**
 * @brief 在 procfs 目录中查找条目
 * 
 * @param dev       设备号（未使用，procfs 为虚拟文件系统）
 * @param dir_inum  目录 inode 号
 * @param name      要查找的名称
 * @param inum      输出参数，返回找到的 inode 号
 * @return int      成功返回 0，未找到返回 -1
 */
int procfs_find_entry(int dev, uint32 dir_inum, const char *name, uint32 *inum) {
    /* /proc 根目录查找 */
    if(dir_inum == PROC_ROOT_INO) {
        if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0) {
            *inum = PROC_ROOT_INO; 
            return 0;
        }
        if(namecmp(name, "uptime") == 0) {
            *inum = PROC_UPTIME_INO; 
            return 0;
        }
        if(namecmp(name, "meminfo") == 0) {
            *inum = PROC_MEMINFO_INO; 
            return 0;
        }
        /* 查找 PID 目录 */
        if(is_pid_str(name)) {
            int pid = atoi(name);
            for(int i = 0; i < NPROC; i++) {
                struct proc *p = getproc(i);
                if(p && p->state != UNUSED && p->pid == pid) {
                    *inum = PROC_PID_BASE + i * 10;
                    return 0;
                }
            }
        }
    } 
    /* /proc/[pid] 进程目录查找 */
    else if(dir_inum >= PROC_PID_BASE && (dir_inum % 10 == 0)) {
        if(namecmp(name, ".") == 0) { 
            *inum = dir_inum; 
            return 0; 
        }
        if(namecmp(name, "..") == 0) { 
            *inum = PROC_ROOT_INO; 
            return 0; 
        }
        if(namecmp(name, "status") == 0) { 
            *inum = dir_inum + 1; 
            return 0; 
        }
    }
    return -1;
}

/*============================================================================
 *                          Procfs 状态信息
 *===========================================================================*/

/**
 * @brief 获取 procfs 文件状态信息
 * 
 * @param ip 目标 inode
 * @param st stat 结构体指针
 */
void procfs_stat(struct inode *ip, struct stat *st) {
    st->dev = ip->dev;
    st->ino = ip->inum;
    
    /* 判断是目录还是文件 */
    if(ip->inum == PROC_ROOT_INO || 
       (ip->inum >= PROC_PID_BASE && (ip->inum % 10 == 0))) {
        st->type = T_DIR;
        st->size = 0;
    } else {
        st->type = T_FILE;
        st->size = 0;
    }
    st->nlink = 1;
}

/*============================================================================
 *                          VFS 接口适配器
 *===========================================================================*/

/**
 * @brief Procfs VFS lookup 接口适配器
 * 
 * @param dp   父目录 inode
 * @param name 要查找的名称
 * @param poff 偏移量输出（未使用）
 * @return struct inode* 成功返回目标 inode，失败返回 NULL
 */
struct inode* procfs_lookup_vfs(struct inode *dp, char *name, uint *poff) {
    uint32 inum;
    if(procfs_find_entry(dp->dev, dp->inum, name, &inum) < 0) 
        return 0;
    
    struct inode *ip = iget(dp->dev, inum);
    
    /* 设置 inode 类型 */
    if(inum == PROC_ROOT_INO || (inum >= PROC_PID_BASE && (inum % 10 == 0)))
        ip->type = T_DIR;
    else
        ip->type = T_FILE;
    ip->valid = 1;
    
    return ip;
}

/*============================================================================
 *                          操作表定义
 *===========================================================================*/

/** @brief Procfs inode 操作表 */
struct inode_operations procfs_i_op = {
    .lookup = procfs_lookup_vfs,
    .link   = 0,      /* procfs 不支持硬链接 */
    .unlink = 0,      /* procfs 不支持删除 */
    .mkdir  = 0,      /* procfs 不支持创建目录 */
    .itrunc = 0,      /* procfs 不支持截断 */
};

/** @brief Procfs 文件操作表 */
struct file_operations procfs_f_op = {
    .read    = procfs_readi,
    .write   = 0,     /* procfs 不支持写入 */
    .stat    = procfs_stat,
    .iupdate = 0,     /* procfs 不需要更新 inode */
};

/** @brief Procfs 文件系统类型定义 */
struct filesystem_type procfs_fs_type = {
    .name       = "procfs",
    .fs_type    = FS_PROC,
    .root_inum  = 2000,
    .i_op       = &procfs_i_op,
    .f_op       = &procfs_f_op,
    .probe      = 0,
    .read_super = 0,
};
