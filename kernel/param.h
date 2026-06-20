#define NPROC        64  // maximum number of processes
#define NCPU          8  // maximum number of CPUs
#define NOFILE       16  // open files per process
#define NFILE       100  // open files per system
#define NINODE       100  // maximum number of active i-nodes (增加以支持压力测试)
#define NDEV         10  // maximum major device number
#define ROOTDEV       0  // device number of file system root disk
#define MAXARG       32  // max exec arguments
#define MAXOPBLOCKS  10  // max # of blocks any FS op writes
#define LOGBLOCKS    (MAXOPBLOCKS*3)  // max data blocks in on-disk log
#define NBUF         60  // size of disk block cache (increased for concurrent I/O)
#define FSSIZE       2000  // size of file system in blocks
#define MAXPATH      128   // maximum file path name
#define USERSTACK    1     // user stack pages
#define BSIZE 1024  // 块大小
#define MAX_DISKS 100  // 支持更多盘，包括虚拟的 Procfs 设备
#define MAX_VIRTIO_DISKS 2 // 实际的物理 VirtIO 磁盘数
