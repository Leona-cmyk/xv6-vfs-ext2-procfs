#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

#define stat xv6_stat  // avoid clash with host struct stat
#include "kernel/types.h"
#include "kernel/fs.h"
#include "kernel/stat.h"
#include "kernel/param.h"

#ifndef static_assert
#define static_assert(a, b) do { switch (0) case 0: case (a): ; } while (0)
#endif

#define NINODES 200

// Disk layout:
// [ boot block | sb block | log | inode blocks | free bit map | data blocks ]

int nbitmap = FSSIZE/BPB + 1;
int ninodeblocks = NINODES / IPB + 1;
int nlog = LOGBLOCKS+1;   // Header followed by LOGBLOCKS data blocks.
int nmeta;    // Number of meta blocks (boot, sb, nlog, inode, bitmap)
int nblocks;  // Number of data blocks

int fsfd;
struct superblock sb;
char zeroes[BSIZE];
uint freeinode = 1;
uint freeblock;

// 目录缓存：用于跟踪已创建的目录
#define MAX_DIRS 32
struct {
  char name[DIRSIZ];
  uint inum;
  uint parent_inum;
} dircache[MAX_DIRS];
int ndirs = 0;

void balloc(int);
void wsect(uint, void*);
void winode(uint, struct dinode*);
void rinode(uint inum, struct dinode *ip);
void rsect(uint sec, void *buf);
uint ialloc(ushort type);
void iappend(uint inum, void *p, int n);
void die(const char *);
uint get_or_create_dir(uint parent_inum, const char *dirname);
uint resolve_path(uint rootino, const char *path, char *basename);

// convert to riscv byte order
ushort
xshort(ushort x)
{
  ushort y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  return y;
}

uint
xint(uint x)
{
  uint y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  a[2] = x >> 16;
  a[3] = x >> 24;
  return y;
}

// 在目录中查找子目录
uint
find_dir_in_cache(uint parent_inum, const char *dirname)
{
  for(int i = 0; i < ndirs; i++){
    if(dircache[i].parent_inum == parent_inum &&
       strncmp(dircache[i].name, dirname, DIRSIZ) == 0){
      return dircache[i].inum;
    }
  }
  return 0;
}

// 创建子目录并返回其 inode 号
uint
get_or_create_dir(uint parent_inum, const char *dirname)
{
  uint inum;
  struct dirent de;
  
  // 检查是否已存在
  inum = find_dir_in_cache(parent_inum, dirname);
  if(inum != 0){
    return inum;
  }
  
  // 创建新目录
  inum = ialloc(T_DIR);
  
  // 添加 "."
  bzero(&de, sizeof(de));
  de.inum = xshort(inum);
  strcpy(de.name, ".");
  iappend(inum, &de, sizeof(de));
  
  // 添加 ".."
  bzero(&de, sizeof(de));
  de.inum = xshort(parent_inum);
  strcpy(de.name, "..");
  iappend(inum, &de, sizeof(de));
  
  // 在父目录中添加条目
  bzero(&de, sizeof(de));
  de.inum = xshort(inum);
  strncpy(de.name, dirname, DIRSIZ);
  iappend(parent_inum, &de, sizeof(de));
  
  // 添加到缓存
  if(ndirs < MAX_DIRS){
    strncpy(dircache[ndirs].name, dirname, DIRSIZ);
    dircache[ndirs].inum = inum;
    dircache[ndirs].parent_inum = parent_inum;
    ndirs++;
  }
  
  printf("mkfs: created directory '%s' (inum=%d, parent=%d)\n", dirname, inum, parent_inum);
  return inum;
}

// 解析路径，创建中间目录，返回最终目录的 inode 号和文件名
uint
resolve_path(uint rootino, const char *path, char *basename)
{
  uint current_inum = rootino;
  char pathcopy[256];
  char *p, *component;
  char *last_component = NULL;
  
  strncpy(pathcopy, path, sizeof(pathcopy) - 1);
  pathcopy[sizeof(pathcopy) - 1] = '\0';
  
  // 找到所有路径组件
  p = pathcopy;
  while(*p){
    // 跳过开头的 '/'
    while(*p == '/') p++;
    if(*p == '\0') break;
    
    component = p;
    
    // 找到下一个 '/'
    while(*p && *p != '/') p++;
    
    // 记住最后一个组件（文件名）
    if(*p == '\0'){
      // 这是最后一个组件，应该是文件名
      last_component = component;
    } else {
      *p = '\0';
      p++;
      
      // 这是一个中间目录
      if(*component != '\0'){
        current_inum = get_or_create_dir(current_inum, component);
      }
    }
  }
  
  // 复制文件名
  if(last_component){
    strncpy(basename, last_component, DIRSIZ);
  } else {
    basename[0] = '\0';
  }
  
  return current_inum;
}

int
main(int argc, char *argv[])
{
  int i, cc, fd;
  uint rootino, inum, off;
  struct dirent de;
  char buf[BSIZE];
  struct dinode din;

  static_assert(sizeof(int) == 4, "Integers must be 4 bytes!");

  if(argc < 2){
    fprintf(stderr, "Usage: mkfs fs.img files...\n");
    exit(1);
  }

  assert((BSIZE % sizeof(struct dinode)) == 0);
  assert((BSIZE % sizeof(struct dirent)) == 0);

  fsfd = open(argv[1], O_RDWR|O_CREAT|O_TRUNC, 0666);
  if(fsfd < 0)
    die(argv[1]);

  // 1 fs block = 1 disk sector
  nmeta = 2 + nlog + ninodeblocks + nbitmap;
  nblocks = FSSIZE - nmeta;

  sb.magic = FSMAGIC;
  sb.size = xint(FSSIZE);
  sb.nblocks = xint(nblocks);
  sb.ninodes = xint(NINODES);
  sb.nlog = xint(nlog);
  sb.logstart = xint(2);
  sb.inodestart = xint(2+nlog);
  sb.bmapstart = xint(2+nlog+ninodeblocks);

  printf("nmeta %d (boot, super, log blocks %u, inode blocks %u, bitmap blocks %u) blocks %d total %d\n",
         nmeta, nlog, ninodeblocks, nbitmap, nblocks, FSSIZE);

  freeblock = nmeta;     // the first free block that we can allocate

  for(i = 0; i < FSSIZE; i++)
    wsect(i, zeroes);

  memset(buf, 0, sizeof(buf));
  memmove(buf, &sb, sizeof(sb));
  wsect(1, buf);

  rootino = ialloc(T_DIR);
  assert(rootino == ROOTINO);

  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, ".");
  iappend(rootino, &de, sizeof(de));

  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, "..");
  iappend(rootino, &de, sizeof(de));

  for(i = 2; i < argc; i++){
    char *filepath = argv[i];
    char *shortpath;
    char basename[DIRSIZ + 1];
    uint dir_inum;
    
    // 去掉 "user/" 前缀
    if(strncmp(filepath, "user/", 5) == 0)
      shortpath = filepath + 5;
    else
      shortpath = filepath;
    
    // 解析路径
    dir_inum = resolve_path(rootino, shortpath, basename);
    
    // 跳过前导 '_'（可执行文件命名约定）
    char *filename = basename;
    if(filename[0] == '_')
      filename += 1;
    
    assert(strlen(filename) <= DIRSIZ);
    
    if((fd = open(filepath, 0)) < 0)
      die(filepath);

    inum = ialloc(T_FILE);

    bzero(&de, sizeof(de));
    de.inum = xshort(inum);
    strncpy(de.name, filename, DIRSIZ);
    iappend(dir_inum, &de, sizeof(de));

    while((cc = read(fd, buf, sizeof(buf))) > 0)
      iappend(inum, buf, cc);

    close(fd);
    
    printf("mkfs: added '%s' -> '%s' (inum=%d, dir=%d)\n", filepath, filename, inum, dir_inum);
  }

  // fix size of root inode dir
  rinode(rootino, &din);
  off = xint(din.size);
  off = ((off/BSIZE) + 1) * BSIZE;
  din.size = xint(off);
  winode(rootino, &din);

  // 修复所有创建的子目录的大小
  for(i = 0; i < ndirs; i++){
    rinode(dircache[i].inum, &din);
    off = xint(din.size);
    off = ((off/BSIZE) + 1) * BSIZE;
    din.size = xint(off);
    winode(dircache[i].inum, &din);
  }

  balloc(freeblock);

  exit(0);
}

void
wsect(uint sec, void *buf)
{
  if(lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE)
    die("lseek");
  if(write(fsfd, buf, BSIZE) != BSIZE)
    die("write");
}

void
winode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn;
  struct dinode *dip;

  bn = IBLOCK(inum, sb);
  rsect(bn, buf);
  dip = ((struct dinode*)buf) + (inum % IPB);
  *dip = *ip;
  wsect(bn, buf);
}

void
rinode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn;
  struct dinode *dip;

  bn = IBLOCK(inum, sb);
  rsect(bn, buf);
  dip = ((struct dinode*)buf) + (inum % IPB);
  *ip = *dip;
}

void
rsect(uint sec, void *buf)
{
  if(lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE)
    die("lseek");
  if(read(fsfd, buf, BSIZE) != BSIZE)
    die("read");
}

uint
ialloc(ushort type)
{
  uint inum = freeinode++;
  struct dinode din;

  bzero(&din, sizeof(din));
  din.type = xshort(type);
  din.nlink = xshort(1);
  din.size = xint(0);
  winode(inum, &din);
  return inum;
}

void
balloc(int used)
{
  uchar buf[BSIZE];
  int i;

  printf("balloc: first %d blocks have been allocated\n", used);
  assert(used < BPB);
  bzero(buf, BSIZE);
  for(i = 0; i < used; i++){
    buf[i/8] = buf[i/8] | (0x1 << (i%8));
  }
  printf("balloc: write bitmap block at sector %d\n", sb.bmapstart);
  wsect(sb.bmapstart, buf);
}

#define min(a, b) ((a) < (b) ? (a) : (b))

void
iappend(uint inum, void *xp, int n)
{
  char *p = (char*)xp;
  uint fbn, off, n1;
  struct dinode din;
  char buf[BSIZE];
  uint indirect[NINDIRECT];
  uint x;

  rinode(inum, &din);
  off = xint(din.size);
  // printf("append inum %d at off %d sz %d\n", inum, off, n);
  while(n > 0){
    fbn = off / BSIZE;
    assert(fbn < MAXFILE);
    if(fbn < NDIRECT){
      if(xint(din.addrs[fbn]) == 0){
        din.addrs[fbn] = xint(freeblock++);
      }
      x = xint(din.addrs[fbn]);
    } else {
      if(xint(din.addrs[NDIRECT]) == 0){
        din.addrs[NDIRECT] = xint(freeblock++);
      }
      rsect(xint(din.addrs[NDIRECT]), (char*)indirect);
      if(indirect[fbn - NDIRECT] == 0){
        indirect[fbn - NDIRECT] = xint(freeblock++);
        wsect(xint(din.addrs[NDIRECT]), (char*)indirect);
      }
      x = xint(indirect[fbn-NDIRECT]);
    }
    n1 = min(n, (fbn + 1) * BSIZE - off);
    rsect(x, buf);
    bcopy(p, buf + off - (fbn * BSIZE), n1);
    wsect(x, buf);
    n -= n1;
    off += n1;
    p += n1;
  }
  din.size = xint(off);
  winode(inum, &din);
}

void
die(const char *s)
{
  perror(s);
  exit(1);
}
