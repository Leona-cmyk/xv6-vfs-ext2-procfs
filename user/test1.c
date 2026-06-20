#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"

#define NULL 0
#define LEN 1024

/* 文件拷贝测试。
   返回值：表示这次测试的得分。*/
int test_copy_file(const char *src, const char *dst) {
  int inf, outf;
  int score = 0;

  if (src == NULL || dst == NULL) {
    fprintf(2, "%s: illegal parameters\n", __func__);
    return score;
  }
  
  if ((inf = open(src, O_RDONLY)) < 0) {
    fprintf(2, "%s: open %s error\n", __func__, src);
    return score;
  }
  score++;
  
  if ((outf = open(dst, O_WRONLY | O_CREATE | O_TRUNC)) < 0) {
    fprintf(2, "%s: open %s error\n", __func__, dst);
    close(inf);
    return score;
  }
  score++;

  int i;
  char buf[LEN];
  do {
    i = read(inf, buf, LEN);
    if (write(outf, buf, i) != i) {
      fprintf(2, "test1: write error\n");
      close(inf);
      close(outf);
      return score;
    }
  } while (i);
  close(inf);
  close(outf);
  score+=3;

  printf("%s(%s,%s): succeed (score: %d)\n", __func__, src, dst, score);
  return score;
}


// 调整测试顺序：先用已存在的 /tests/hello.c 创建其他文件
static const char * fnames[][2] = {
  {"/tests/hello.c",  "/mnt/hello.c"},   // 1. /tests/hello.c 存在，创建 /mnt/hello.c
  {"/tests/hello.c",  "/hello.c"},       // 2. /tests/hello.c 存在，创建 /hello.c
  {"/hello.c",        "/mnt/hello2.c"},  // 3. /hello.c 现在存在了
  {"/mnt/hello.c",    "/hello2.c"}       // 4. /mnt/hello.c 现在存在了
};

// number of elements in fixed-size array
#define NELEM(x) (sizeof(x)/sizeof((x)[0]))
#define NUM_TESTS NELEM(fnames)

int main(int argc, char *argv[]) {
  printf("\n--------- %s: begin... ---------------------{\n", argv[0]);
  int score = 0;
  for (int i=0; i<NUM_TESTS; i++) {
    score += test_copy_file(fnames[i][0], fnames[i][1]);
  }
  printf("--------- %s: finished (total score: %d)----}\n", argv[0], score);
  return 0;
}
