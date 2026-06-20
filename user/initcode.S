# Initial process that execs /init.
# This code runs in user space.

# System call numbers (如果缺少syscall.h，直接使用数字)
# exec系统调用号通常是 7（在xv6-riscv中）
# exit系统调用号通常是 10

.globl start
start:
        # exec(init, argv)
        la a0, init      # 第一个参数：程序路径
        la a1, argv      # 第二个参数：参数数组
        li a7, 7         # SYS_exec = 7
        ecall

        # for(;;) exit();
exit:
        li a7, 10        # SYS_exit = 10
        ecall
        jal exit

# char init[] = "/init\0";
init:
  .string "/init\0"

# char *argv[] = { init, 0 };
.p2align 2
argv:
  .long init
  .long 0
