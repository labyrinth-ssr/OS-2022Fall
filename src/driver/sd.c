#include <common/sem.h>
#include <driver/sddef.h>
#include <kernel/mem.h>
#include <kernel/sched.h>

static BufQueue bqueue;
static SpinLock sdlock;
static Semaphore rw_done;

void bufqueue_init(BufQueue *x) {
  x->begin = x->end = 0;
  x->sz = 0;
  init_spinlock(&x->lk);
}
void bufqueue_lock(BufQueue *x) { _acquire_spinlock(&x->lk); }
void bufqueue_unlock(BufQueue *x) { _release_spinlock(&x->lk); }
void bufqueue_push(BufQueue *x, buf *buf_item) {
  auto item = &buf_item->node;
  init_list_node(item);
  if (x->sz == 0) {
    x->begin = x->end = item;

  } else {
    _merge_list(x->end, item);
    x->end = item;
  }
  x->sz++;
}
void bufqueue_pop(BufQueue *x) {
  if (x->sz == 0)
    PANIC();
  if (x->sz == 1) {
    x->begin = x->end = 0;
  } else {
    auto t = x->begin;
    x->begin = x->begin->next;
    _detach_from_list(t);
  }
  x->sz--;
}
buf *bufqueue_front(BufQueue *x) {
  if (!x || !x->begin)
    PANIC();
  return container_of(x->begin, buf, node);
}
bool bufqueue_empty(BufQueue *x) { return x->sz == 0; }

/*
 * Initialize SD card.
 * Returns zero if initialization was successful, non-zero otherwise.
 */
int sdInit();
void sd_intr();
/*
Wait for interrupt.
return after interrupt handling
*/
static int sdWaitForInterrupt(unsigned int mask);
/*
data synchronization barrier.
use before access memory
*/
static ALWAYS_INLINE void arch_dsb_sy();
/*
call handler when interrupt
*/
void set_interrupt_handler(InterruptType type, InterruptHandler handler);
/*

*/
ALWAYS_INLINE u32 get_EMMC_DATA() { return *EMMC_DATA; }
ALWAYS_INLINE u32 get_and_clear_EMMC_INTERRUPT() {
  u32 t = *EMMC_INTERRUPT;
  *EMMC_INTERRUPT = t;
  return t;
}

/*
 * Initialize SD card and parse MBR.
 * 1. The first partition should be FAT and is used for booting.
 * 2. The second partition is used by our file system.
 *
 * See https://en.wikipedia.org/wiki/Master_boot_record
 */

void sd_init() {
  /*
   * 1.call sdInit.
   * 2.Initialize the lock and request queue if any.
   * 3.Read and parse 1st block (MBR) and collect whatever
   * information you want.
   * 4.set interrupt handler for IRQ_SDIO,IRQ_ARASANSDIO
   *
   * Hint:
   * 1.Maybe need to use sd_start for reading, and
   * sdWaitForInterrupt for clearing certain interrupt.
   * 2.Remember to call sd_init() at somewhere.
   * 3.the first number is 0.
   * 4.don't forget to call this function somewhere
   * TODO: Lab5 driver.
   */
  sdInit();
  init_spinlock(&sdlock);
  bufqueue_init(&bqueue);
  set_interrupt_handler(IRQ_ARASANSDIO, sd_intr);
  set_interrupt_handler(IRQ_SDIO, sd_intr);
  auto get_MBR = (buf *)kalloc(sizeof(buf));
  memset(get_MBR, 0, sizeof(*get_MBR));
  bufqueue_lock(&bqueue);
  bufqueue_push(&bqueue, get_MBR);
  bufqueue_unlock(&bqueue);
  auto save_flag = get_MBR->flags;
  get_and_clear_EMMC_INTERRUPT();
  _acquire_spinlock(&sdlock);
  sd_start(get_MBR);
  _release_spinlock(&sdlock);
  init_sem(&rw_done, 0);
  while (save_flag == get_MBR->flags) {
    wait_sem(&rw_done);
  }
  auto LBA = *(u32 *)((u64)get_MBR->data + 0x1CE + 0x8);
  auto sec_num = *(u32 *)((u64)get_MBR->data + 0x1CE + 0xC);
  printk("mbr data: LBA:%d, sec_num:%d\n", LBA, sec_num);
}

/* Start the request for b. Caller must hold sdlock. */
static void sd_start(struct buf *b) {
  // Address is different depending on the card type.
  // HC pass address as block #.
  // SC pass address straight through.
  int bno =
      sdCard.type == SD_TYPE_2_HC ? (int)b->blockno : (int)b->blockno << 9;
  int write = b->flags & B_DIRTY;

  // printk("- sd start: cpu %d, flag 0x%x, bno %d, write=%d\n", cpuid(),
  // b->flags, bno, write);

  arch_dsb_sy();
  // Ensure that any data operation has completed before doing the transfer.
  if (*EMMC_INTERRUPT) {
    printk("emmc interrupt flag should be empty: 0x%x. \n", *EMMC_INTERRUPT);
    PANIC();
  }
  arch_dsb_sy();

  // Work out the status, interrupt and command values for the transfer.
  int cmd = write ? IX_WRITE_SINGLE : IX_READ_SINGLE;
  int resp;
  *EMMC_BLKSIZECNT = 512;

  if ((resp = sdSendCommandA(cmd, bno))) {
    printk("* EMMC send command error.\n");
    PANIC();
  }

  int done = 0;
  u32 *intbuf = (u32 *)b->data;
  if (!(((i64)b->data) & 0x03) == 0) {
    printk("Only support word-aligned buffers. \n");
    PANIC();
  }

  if (write) {
    // Wait for ready interrupt for the next block.
    if ((resp = sdWaitForInterrupt(INT_WRITE_RDY))) {
      printk("* EMMC ERROR: Timeout waiting for ready to write\n");
      PANIC();
    }
    if (*EMMC_INTERRUPT) {
      printk("%d\n", *EMMC_INTERRUPT);
      PANIC();
    }
    while (done < 128)
      *EMMC_DATA = intbuf[done++];
  }
}

/* The interrupt handler. Sync buf with disk.*/
void sd_intr() {
  /*
   * Pay attention to whether there is any element in the buflist.
   * Understand the meanings of EMMC_INTERRUPT, EMMC_DATA, INT_DATA_DONE,
   * INT_READ_RDY, B_DIRTY, B_VALID and some other flags.
   *
   * Notice that reading and writing are different, you can use flags
   * to identify.
   *
   * If B_DIRTY is set, write buf to disk, clear B_DIRTY, set B_VALID.
   * Else if B_VALID is not set, read buf from disk, set B_VALID.
   *
   * Remember to clear the flags after reading/writing.
   *
   * When finished, remember to use pop and check whether the list is
   * empty, if not, continue to read/write.
   *
   * You may use some buflist functions, arch_dsb_sy(), sd_start(), post_sem()
   * and sdWaitForInterrupt() to complete this function.
   *
   * TODO: Lab5 driver.
   */
  bufqueue_lock(&bqueue);
  auto b = bufqueue_front(&bqueue);
  if (b->flags & B_DIRTY) {
    b->flags = B_VALID;
    bufqueue_pop(&bqueue);
    _acquire_spinlock(&sdlock);
    sdWaitForInterrupt(INT_DATA_DONE);
    _release_spinlock(&sdlock);
    post_sem(&rw_done);
  } else if (!(b->flags & B_VALID)) {
    _acquire_spinlock(&sdlock);
    if (sdWaitForInterrupt(INT_READ_RDY)) {
      printk("* EMMC ERROR: Timeout waiting for ready to read\n");
      PANIC();
    }
    if (*EMMC_INTERRUPT) {
      printk("%d\n", *EMMC_INTERRUPT);
      PANIC();
    }
    u32 *intbuf = (u32 *)b->data;
    (void)intbuf;
    int done = 0;
    while (done < 128)
      intbuf[done++] = *EMMC_DATA;
    sdWaitForInterrupt(INT_DATA_DONE);
    _release_spinlock(&sdlock);
    b->flags = B_VALID;
    bufqueue_pop(&bqueue);
    post_sem(&rw_done);
  }
  if (!bufqueue_empty(&bqueue)) {
    printk("que not empty\n");
    bufqueue_unlock(&bqueue);
    sd_start(bufqueue_front(&bqueue));
  }

  bufqueue_unlock(&bqueue);
}

void sdrw(buf *b) {
  /*
   * 1.add buf to the queue
   * 2.if no buf in queue before,send request now
   * 3.'loop' until buf flag is modified
   *
   * You may use some buflist functions, arch_dsb_sy(),
   * sd_start(), wait_sem() to complete this function.
   *  TODO: Lab5 driver.
   */
  init_sem(&rw_done, 0);
  bufqueue_lock(&bqueue);
  _acquire_spinlock(&sdlock);
  bufqueue_push(&bqueue, b);
  sd_start(b);
  _release_spinlock(&sdlock);
  if (bufqueue_empty(&bqueue)) {
    bufqueue_unlock(&bqueue);
    auto flag_save = b->flags;
    while (flag_save == b->flags) {
      wait_sem(&rw_done);
    }
  } else {
    auto b_first = bufqueue_front(&bqueue);
    bufqueue_unlock(&bqueue);
    auto flag_save = b_first->flags;
    while (flag_save == b_first->flags) {
      wait_sem(&rw_done);
    }
  }
}

/* SD card test and benchmark. */
void sd_test() {
  static struct buf b[1 << 11];
  int n = sizeof(b) / sizeof(b[0]);
  printk("n = %d\n", n);
  int mb = (n * BSIZE) >> 20;
  // assert(mb);
  if (!mb)
    PANIC();
  i64 f, t;
  asm volatile("mrs %[freq], cntfrq_el0" : [ freq ] "=r"(f));
  printk("- sd test: begin nblocks %d\n", n);

  printk("- sd check rw...\n");
  // Read/write test
  for (int i = 1; i < n; i++) {
    // Backup.
    b[0].flags = 0;
    b[0].blockno = (u32)i;
    sdrw(&b[0]);
    // Write some value.
    b[i].flags = B_DIRTY;
    b[i].blockno = (u32)i;
    for (int j = 0; j < BSIZE; j++)
      b[i].data[j] = (u8)((i * j) & 0xFF);
    sdrw(&b[i]);

    memset(b[i].data, 0, sizeof(b[i].data));
    // Read back and check
    b[i].flags = 0;
    sdrw(&b[i]);
    for (int j = 0; j < BSIZE; j++) {
      //   assert(b[i].data[j] == (i * j & 0xFF));
      if (b[i].data[j] != (i * j & 0xFF))
        PANIC();
      // printk("%d",j);
    }
    // Restore previous value.
    b[0].flags = B_DIRTY;
    sdrw(&b[0]);
  }

  // Read benchmark
  arch_dsb_sy();
  t = (i64)get_timestamp();
  arch_dsb_sy();
  for (int i = 0; i < n; i++) {
    b[i].flags = 0;
    b[i].blockno = (u32)i;
    sdrw(&b[i]);
  }
  arch_dsb_sy();
  t = (i64)get_timestamp() - t;
  arch_dsb_sy();
  printk("- read %dB (%dMB), t: %lld cycles, speed: %lld.%lld MB/s\n",
         n * BSIZE, mb, t, mb * f / t, (mb * f * 10 / t) % 10);

  // Write benchmark
  arch_dsb_sy();
  t = (i64)get_timestamp();
  arch_dsb_sy();
  for (int i = 0; i < n; i++) {
    b[i].flags = B_DIRTY;
    b[i].blockno = (u32)i;
    sdrw(&b[i]);
  }
  arch_dsb_sy();
  t = (i64)get_timestamp() - t;
  arch_dsb_sy();

  printk("- write %dB (%dMB), t: %lld cycles, speed: %lld.%lld MB/s\n",
         n * BSIZE, mb, t, mb * f / t, (mb * f * 10 / t) % 10);
}