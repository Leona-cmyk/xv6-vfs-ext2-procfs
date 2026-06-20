#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"

// 使用绝对路径，确保能找到源文件
#define SRC_FILENAME "/tests/hello.c"
#define DST_FILENAME "/mnt/hello.c"

int test_cross_fs_copy() {
  int score = 0;
  printf("将从 xv6 文件系统复制文件到 EXT2 文件系统\n\n");

  // 打开源文件
  int src_fd = open(SRC_FILENAME, O_RDONLY);
  if(src_fd < 0) {
    printf("错误: 找不到源文件 %s\n", SRC_FILENAME);
    return score;
  }
  score++;

  // 创建目标文件 (在 EXT2 文件系统中)
  int dst_fd = open(DST_FILENAME, O_WRONLY | O_CREATE);
  if(dst_fd < 0) {
    printf("错误: 无法创建目标文件 %s\n", DST_FILENAME);
    printf("请先执行 mount\n");
    close(src_fd);
    return score;
  }
  score++;

  // 复制数据
  char copy_buf[512];
  int copy_n;
  while((copy_n = read(src_fd, copy_buf, sizeof(copy_buf))) > 0) {
    if(write(dst_fd, copy_buf, copy_n) != copy_n) {
      printf("✗ 写入失败\n");
      close(src_fd);
      close(dst_fd);
      return score;
    }
  }

  close(src_fd);
  close(dst_fd);

  if(copy_n < 0) {
    printf("✗ 读取失败\n");
    return score;
  }
  score += 3;

  printf("✓ 文件复制成功！\n\n");

  // 验证复制的文件
  printf("验证复制结果:\n");
  int fd = open(DST_FILENAME, O_RDONLY);
  if(fd < 0) {
    printf("✗ 无法打开复制的文件\n");
    return score;
  }
  score++;

  static char verify_buf[256];
  int verify_n = read(fd, verify_buf, sizeof(verify_buf) - 1);
  if(verify_n < 0) {
    printf("✗ 读取复制的文件失败\n");
    close(fd);
    return score;
  }
  verify_buf[verify_n] = '\0';
  close(fd);
  score++;

  printf("✓ 复制成功！文件内容: \n%s\n", verify_buf);
  printf("现在您可以用以下命令查看复制的文件:\n");
  printf("cat %s\n", DST_FILENAME);

  printf("%s(): succeed (score: %d)\n", __func__, score);
  return score;
}

int main(int argc, char *argv[]) {
  printf("\n--------- %s: begin... ---------------------{\n", argv[0]);
  int score = 0;
  score = test_cross_fs_copy();
  printf("--------- %s: finished (total score: %d)----}\n", argv[0], score);
  return 0;
}
