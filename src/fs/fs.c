#include "driver/sd.h"
#include <common/defines.h>
#include <fs/block_device.h>
#include <fs/cache.h>
#include <fs/defines.h>
#include <fs/file.h>
#include <fs/fs.h>
#include <fs/inode.h>
#include <kernel/init.h>
#include <kernel/printk.h>

void init_filesystem() {
  sd_init();
  compiler_fence();
  arch_fence();
  init_block_device();

  const SuperBlock *sblock = get_super_block();
  init_bcache(sblock, &block_device);
  init_inodes(sblock, &bcache);
  init_ftable();
}
define_rest_init(fs) {
  init_filesystem();
  printk("fs init ok\n");
}