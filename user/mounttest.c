#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

int main(int argc, char *argv[])
{
  int fd;
  char buf[256];  // 增大缓冲区以读取完整文件内容
  int n;

  printf("Mount test: Reading from EXT2 filesystem...\n");

  // 1. 测试列出目录
  printf("\nListing /mnt directory:\n");
  fd = open("/mnt", O_RDONLY);
  if(fd < 0){
    printf("Failed to open /mnt\n");
  } else {
    struct dirent de;
    while(read(fd, &de, sizeof(de)) == sizeof(de)){
      if(de.inum != 0){
        printf("  ino: %d, name: %s\n", de.inum, de.name);
      }
    }
    close(fd);
  }

  // 2. 测试读取文件
  printf("\nReading /mnt/hello.c:\n");
  fd = open("/mnt/hello.c", O_RDONLY);
  if(fd < 0){
    printf("Failed to open /mnt/hello.c\n");
  } else {
    n = read(fd, buf, sizeof(buf)-1);
    if(n > 0){
      buf[n] = '\0';
      printf("Content: %s\n", buf);
    } else {
      printf("Failed to read content (n=%d)\n", n);
    }
    close(fd);
  }

  exit(0);
}
