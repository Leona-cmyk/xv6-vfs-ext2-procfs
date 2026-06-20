
#ifndef _VIRTIO_DISK_H
#define _VIRTIO_DISK_H

struct buf;

void virtio_disk_init(void);
void virtio_disk_rw(struct buf *, int, int);
void virtio_disk_intr(void);

#endif
