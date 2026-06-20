# xv6-VFS: 虚拟文件系统扩展

基于 **xv6-riscv-rev5**（比赛官方提供源码）的虚拟文件系统（VFS）扩展实现，支持多文件系统挂载与统一访问。

## 项目简介

本项目在 xv6 操作系统基础上实现了完整的 VFS 抽象层，支持：
- **xv6 原生文件系统**：保持完全兼容
- **EXT2 文件系统**：支持挂载 Linux 格式化的 EXT2 磁盘镜像
- **Procfs 虚拟文件系统**：动态展示内核和进程状态信息


> 视频内容：系统启动、EXT2 挂载、Procfs 挂载、文件操作、测试验证

## 功能特性

### 核心架构
- 定义 `inode_operations` 和 `file_operations` 接口，实现多态文件系统操作
- 虚拟超级块（superblock）管理多文件系统实例
- 目录项缓存（Dentry Cache）优化路径查找性能

### 多文件系统支持
| 文件系统 | 挂载点 | 功能描述 |
|---------|--------|---------|
| xv6fs   | /      | 原生根文件系统 |
| EXT2    | /mnt   | 外部 EXT2 磁盘镜像 |
| Procfs  | /proc  | 进程与系统信息 |

### 系统调用
- `mount(dev, fstype, path)` - 动态挂载文件系统
- `umount(path)` - 卸载已挂载的文件系统

## 开发环境

### 环境配置
- **开发工具**：VSCode + Remote-SSH 扩展
- **远程服务器**：Ubuntu（WSL2 / 云服务器 / 虚拟机）
- **编译工具链**：RISC-V GNU 工具链
- **模拟器**：QEMU（riscv64-softmmu）

### Ubuntu 依赖安装
```bash
sudo apt-get install git build-essential gdb-multiarch qemu-system-misc gcc-riscv64-linux-gnu binutils-riscv64-linux-gnu
```

## 编译与运行

### 编译内核
```bash
make clean
make
```

### 启动系统
```bash
make qemu
```

### 带双磁盘启动（xv6 + EXT2）
```bash
make qemu-ext2
```

## 使用示例

### 挂载 EXT2 文件系统
```bash
$ mkdir /mnt
$ mount 1 2 /mnt        # 设备1, 类型EXT2(2), 挂载点/mnt
$ ls /mnt
$ cat /mnt/hello.txt
$ umount /mnt
```

### 挂载 Procfs
```bash
$ mkdir /proc
$ mount 99 3 /proc      # 虚拟设备99, 类型Procfs(3), 挂载点/proc
$ cat /proc/uptime
$ cat /proc/meminfo
$ cat /proc/1/status
$ umount /proc
```

## 测试

### 运行用户测试
```bash
$ usertests
```

### 运行压力测试
```bash
$ grind
```

### 挂载功能测试
```bash
$ mounttest
```

## 目录结构

```
xv6_work/
├── kernel/
│   ├── vfs.h          # VFS 核心数据结构定义
│   ├── vfs.c          # VFS 实现（挂载管理、Dentry Cache）
│   ├── fs.c           # xv6 原生文件系统（已适配 VFS）
│   ├── ext2.h         # EXT2 数据结构定义
│   ├── ext2.c         # EXT2 文件系统驱动
│   ├── procfs.c       # Procfs 虚拟文件系统
│   └── ...
├── user/
│   ├── usertests.c    # 综合测试程序
│   ├── grind.c        # 压力测试程序
│   ├── mounttest.c    # 挂载功能测试
│   └── ...
├── mkfs/
│   └── mkfs.c         # 文件系统镜像生成工具
├── ext2.img           # EXT2 测试磁盘镜像
├── fs.img             # xv6 文件系统镜像
├── Makefile
└── README.md
```

## 技术实现

### VFS 架构图
```
┌─────────────────────────────────────────┐
│            用户空间 (User Space)          │
│   open() / read() / write() / close()   │
└────────────────────┬────────────────────┘
                     │ 系统调用
┌────────────────────▼────────────────────┐
│              VFS 抽象层                   │
│  ┌─────────────┐  ┌─────────────────┐   │
│  │inode_operations│ │file_operations │   │
│  └─────────────┘  └─────────────────┘   │
└────────────────────┬────────────────────┘
          ┌──────────┼──────────┐
          ▼          ▼          ▼
    ┌─────────┐ ┌─────────┐ ┌─────────┐
    │  xv6fs  │ │  EXT2   │ │ Procfs  │
    └─────────┘ └─────────┘ └─────────┘
```

## 参考资料

- [xv6-riscv-rev5](https://github.com/mit-pdos/xv6-riscv/tree/xv6-riscv-rev5) - 比赛官方提供的 xv6 第 5 版源码（本项目基础）
- [MIT 6.1810 xv6](https://pdos.csail.mit.edu/6.1810/)
- [EXT2 文件系统规范](https://www.nongnu.org/ext2-doc/ext2.html)
- [Linux VFS 设计](https://www.kernel.org/doc/html/latest/filesystems/vfs.html)

## 致谢

本项目基于比赛官方提供的 **xv6-riscv-rev5** 源码开发，感谢 MIT PDOS 实验室的开源贡献。
