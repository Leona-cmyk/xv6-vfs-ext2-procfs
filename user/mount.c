#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  if(argc != 4){
    fprintf(2, "Usage: mount <dev> <type> <path>\n");
    exit(1);
  }

  int dev = atoi(argv[1]);
  int type = atoi(argv[2]);
  char *path = argv[3];

  if(mount(dev, type, path) < 0){
    fprintf(2, "mount %s failed\n", path);
    exit(1);
  }

  exit(0);
}
