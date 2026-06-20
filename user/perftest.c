#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"

#define ITERATIONS 1000  // 重复查找次数

// 获取当前时间（tick）
uint64 get_ticks() {
    uint64 ticks;
    asm volatile("rdtime %0" : "=r" (ticks));
    return ticks;
}

// 测试路径查找性能
void test_lookup_performance(const char *path, int iterations) {
    int fd;
    uint64 start, end;
    
    printf("测试路径: %s\n", path);
    printf("重复次数: %d\n", iterations);
    
    start = get_ticks();
    
    for(int i = 0; i < iterations; i++) {
        fd = open(path, O_RDONLY);
        if(fd < 0) {
            printf("打开失败: %s\n", path);
            return;
        }
        close(fd);
    }
    
    end = get_ticks();
    
    uint64 elapsed = end - start;
    printf("总时间: %d ticks\n", (int)elapsed);
    printf("平均时间: %d ticks/次\n", (int)(elapsed / iterations));
    printf("每秒操作数: %d ops/sec\n\n", 
           (int)(iterations * 10000000ULL / elapsed));
}

// 创建多级目录结构用于测试
void setup_test_dirs() {
    printf("创建测试目录结构...\n");
    mkdir("/testperf");
    mkdir("/testperf/level1");
    mkdir("/testperf/level1/level2");
    mkdir("/testperf/level1/level2/level3");
    
    int fd = open("/testperf/level1/level2/level3/testfile", O_CREATE | O_WRONLY);
    if(fd >= 0) {
        write(fd, "test data\n", 10);
        close(fd);
    }
    printf("测试环境准备完成\n\n");
}

int main(int argc, char *argv[]) {
    printf("========================================\n");
    printf("   VFS 路径查找性能测试\n");
    printf("========================================\n\n");
    
    // 创建测试环境
    setup_test_dirs();
    
    // 测试1: 根目录文件（浅层路径）
    printf("【测试1】浅层路径查找\n");
    printf("----------------------------------------\n");
    test_lookup_performance("/README", ITERATIONS);
    
    // 测试2: 深层路径查找（受益于 Dentry Cache）
    printf("【测试2】深层路径查找（4级目录）\n");
    printf("----------------------------------------\n");
    test_lookup_performance("/testperf/level1/level2/level3/testfile", ITERATIONS);
    
    // 测试3: 重复查找同一路径（Dentry Cache 命中）
    printf("【测试3】重复查找（Cache 命中率测试）\n");
    printf("----------------------------------------\n");
    int fd;
    uint64 start = get_ticks();
    for(int i = 0; i < ITERATIONS * 2; i++) {
        fd = open("/testperf/level1/level2/level3/testfile", O_RDONLY);
        close(fd);
    }
    uint64 end = get_ticks();
    printf("重复 %d 次查找\n", ITERATIONS * 2);
    printf("总时间: %d ticks\n", (int)(end - start));
    printf("平均时间: %d ticks/次\n\n", (int)((end - start) / (ITERATIONS * 2)));
    
    // 测试4: 不同路径交替查找（Cache 利用率）
    printf("【测试4】多路径交替查找\n");
    printf("----------------------------------------\n");
    start = get_ticks();
    for(int i = 0; i < ITERATIONS; i++) {
        fd = open("/testperf/level1/level2/level3/testfile", O_RDONLY);
        close(fd);
        fd = open("/README", O_RDONLY);
        close(fd);
    }
    end = get_ticks();
    printf("交替查找 %d 次\n", ITERATIONS);
    printf("总时间: %d ticks\n", (int)(end - start));
    printf("平均时间: %d ticks/次\n\n", (int)((end - start) / (ITERATIONS * 2)));
    
    printf("========================================\n");
    printf("测试完成！\n");
    printf("========================================\n");
    
    exit(0);
}
