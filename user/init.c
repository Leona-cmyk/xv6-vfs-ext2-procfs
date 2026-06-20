// init: The initial user-level program

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/spinlock.h"
#include "kernel/sleeplock.h"
#include "kernel/fs.h"
#include "kernel/file.h"
#include "user/user.h"
#include "kernel/fcntl.h"

char *argv[] = { "sh", 0 };

int
main(void)
{
  int pid, wpid;
  int fd;

  fd = open("console", O_RDWR);
  if(fd < 0){
    mknod("console", CONSOLE, 0);
    fd = open("console", O_RDWR);
  }
  if(fd < 0) {
    exit(1);
  }
  dup(fd);  // stdout
  dup(fd);  // stderr

  printf("init: Starting xv6...\n");

  // 创建挂载点目录（不自动挂载，用户需手动执行 mount 命令）
  if(mkdir("/mnt") < 0)
    printf("init: mkdir /mnt failed (may already exist)\n");
  if(mkdir("/proc") < 0)
    printf("init: mkdir /proc failed (may already exist)\n");

  printf("init: To mount ext2, run: mount 1 2 /mnt\n");
  printf("init: To mount procfs, run: mount 99 3 /proc\n");

  for(;;){
    printf("init: starting sh\n");
    pid = fork();
    if(pid < 0){
      printf("init: fork failed\n");
      exit(1);
    }
    if(pid == 0){
      exec("sh", argv);
      printf("init: exec sh failed\n");
      exit(1);
    }

    for(;;){
      wpid = wait((int *) 0);
      if(wpid == pid){
        break;
      } else if(wpid < 0){
        printf("init: wait returned an error\n");
        exit(1);
      } else {
        // it was a parentless process; do nothing.
      }
    }
  }
}
