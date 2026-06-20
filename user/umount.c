#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  if(argc != 2){
    fprintf(2, "Usage: umount <path>\n");
    exit(1);
  }

  if(umount(argv[1]) < 0){
    fprintf(2, "umount %s failed\n", argv[1]);
    exit(1);
  }

  exit(0);
}
