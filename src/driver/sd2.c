
// #include <driver/sddef.h>

// /*
//  * Initialize SD card.
//  * Returns zero if initialization was successful, non-zero otherwise.
//  */
// int sdInit();
// /*
// Wait for interrupt.
// return after interrupt handling
// 等待中断
// 中断处理后返回
// */
// static int sdWaitForInterrupt(unsigned int mask);
// /*
// data synchronization barrier.
// use before access memory
// 数据同步障碍
// 访问内存前使用
// */
// static ALWAYS_INLINE void arch_dsb_sy();
// /*
// call handler when interrupt
// */
// void set_interrupt_handler(InterruptType type, InterruptHandler handler);
// /*

// */
// ALWAYS_INLINE u32 get_EMMC_DATA() {
//     return *EMMC_DATA;
// }
// ALWAYS_INLINE u32 get_and_clear_EMMC_INTERRUPT() {
//     u32 t = *EMMC_INTERRUPT;
//     *EMMC_INTERRUPT = t;
//     return t;
// }

// //int key = 541256;
// struct ListNode buf_root;
// SpinLock buf_lock;
// SpinLock sd_lock;
// struct ListNode* nowPointer;
// u32 nowBlockNo;
// /*
//  * Initialize SD card and parse MBR.
//  * 1. The first partition should be FAT and is used for booting.
//  * 2. The second partition is used by our file system.
//  *
//  * See https://en.wikipedia.org/wiki/Master_boot_record
//  */


 
// static buf* get_first(){
//     if(_empty_list(&buf_root)) return NULL;
//     printk("4\n");
//     _acquire_spinlock(&buf_lock);
//     struct ListNode* listnode = nowPointer;
//     if(listnode == &buf_root) listnode = listnode->next;
//     struct buf* _buf;
    
//     while(1){
//         while(listnode == &buf_root){

//             nowBlockNo = 0;
//             listnode = listnode->next;
//         }
        
//         _buf = container_of(listnode, buf, node);

//         if(_buf->blockno < nowBlockNo) listnode = listnode->next;
//         else break;
//     }
//     nowBlockNo = _buf->blockno;
//     nowPointer = _buf->node.prev;
//     _release_spinlock(&buf_lock);
    
//     return _buf;
// }

// static void dequeue(buf* _buf){
//     _acquire_spinlock(&buf_lock);
//     _detach_from_list(&_buf->node);
//     _release_spinlock(&buf_lock);
// }

// static void enqueue(buf* _buf){
//     _acquire_spinlock(&buf_lock);
    

//     struct ListNode* node;
//     buf* list_buf;
//     for(node = buf_root.next; node != &buf_root; node = node->next){
//         list_buf = container_of(node, buf, node);
//         if(list_buf->blockno > _buf->blockno){
//             struct ListNode* prev = node->prev;
//             prev->next = &_buf->node;
//             _buf->node.prev = prev;
//             node->prev = &_buf->node;
//             _buf->node.next = node;
//             _release_spinlock(&buf_lock);
//             return;
//         }
//     }
//     if(node == &buf_root){
//         node = buf_root.prev;
//         node->next = &_buf->node;
//         _buf->node.prev = node->next;
//         buf_root.prev = &_buf->node;
//         _buf->node.next = &buf_root;
//     }
//             printk("}}}}}}}%llx,%d\n",(u64)container_of(buf_root.prev,buf,node),container_of(buf_root.prev,buf,node)->blockno);
//     _release_spinlock(&buf_lock);
// }

// // define_init(sd){
// //     sd_init();
// // }


// void sd_init() {
//     printk("sd_init\n");
//     if(!sdInit()) PANIC();

//     init_list_node(&buf_root);
//     init_spinlock(&buf_lock);
//     init_spinlock(&sd_lock);
//     nowPointer = &buf_root;
//     nowBlockNo = 0;

//     set_interrupt_handler(IRQ_SDIO, sd_intr);
//     set_interrupt_handler(IRQ_ARASANSDIO, sd_intr);

//     arch_dsb_sy();


//     buf mbr;
//     mbr.flags = 0;
//     mbr.blockno = 0;
//     //get_and_clear_EMMC_INTERRUPT();
//     //enqueue(&mbr);
    
    
//     _acquire_spinlock(&sd_lock);
//     int flag = mbr.flags;
//     enqueue(&mbr);
//     get_and_clear_EMMC_INTERRUPT();
//     sd_start(&mbr);
//     init_sem(&mbr.sl,0);
//     while(flag == mbr.flags){
//         wait_sem(&mbr.sl);
//     }
//     _release_spinlock(&sd_lock);
    
//     //sd_intr();
//     unsigned int first_LBA = *((unsigned int*)(&mbr.data[0x1CE + 0x8]));
//     unsigned int number_SEC = *((unsigned int*)(&mbr.data[0x1CE + 0xC]));
//     printk("mbr: flag:%d, nblock:%d, first_LBA:%d, number_SEC:%d\n", mbr.flags, mbr.blockno, first_LBA, number_SEC);
// }




// /* Start the request for b. Caller must hold sdlock. */
// static void sd_start(buf* b) {
//     // Address is different depending on the card type.
//     // HC pass address as block #.
//     // SC pass address straight through.
//     int bno =
//         sdCard.type == SD_TYPE_2_HC ? (int)b->blockno : (int)b->blockno << 9;
//     int write = b->flags & B_DIRTY;
//    printk("- sd start: cpu %d, flag 0x%x, bno %d, write=%d\n", cpuid(), b->flags, bno, write);
//     arch_dsb_sy();
//     // Ensure that any data operation has completed before doing the transfer.
//     if (*EMMC_INTERRUPT) {
//         printk("emmc interrupt flag should be empty: 0x%x. \n",
//                *EMMC_INTERRUPT);
//         PANIC();
//     }
//     arch_dsb_sy();
//     // Work out the status, interrupt and command values for the transfer.
//     int cmd = write ? IX_WRITE_SINGLE : IX_READ_SINGLE;
//     int resp;
//     *EMMC_BLKSIZECNT = 512;
//     if ((resp = sdSendCommandA(cmd, bno))) {
//         printk("* EMMC send command error.\n");
//         PANIC();
//     }
//     int done = 0;
//     u32* intbuf = (u32*)b->data;
//     if (!(((i64)b->data) & 0x03) == 0) {
//         printk("Only support word-aligned buffers. \n");
//         PANIC();
//     }
//     if (write) {
//         // Wait for ready interrupt for the next block.
//         if ((resp = sdWaitForInterrupt(INT_WRITE_RDY))) {
//             printk("* EMMC ERROR: Timeout waiting for ready to write\n");
//             PANIC();
//             // return sdDebugResponse(resp);
//         }
//         if (*EMMC_INTERRUPT) {
//             printk("%d\n", *EMMC_INTERRUPT);
//             PANIC();
//         }
//         while (done < 128)
//             *EMMC_DATA = intbuf[done++];
//     }
// }

// void sd_intr() {
//     //为 IRQ_SDIO,IRQ_ARASANSDIO 的中断处理函数。将队列头的 buf与磁盘上的 buf 同步。以 MMIO 的方式与外设进行交互
    
//     printk("\nintr\n");
//     buf* b = get_first();
//     int write = b->flags & B_DIRTY;
//     int read = (write == 0) && ((write && B_VALID)==0);
//     int resp;
//     int done = 0;
//     u32* intbuf = (u32*)b->data;
//     if(read){
//         if ((resp = sdWaitForInterrupt(INT_READ_RDY))) {
//             printk("* EMMC ERROR: Timeout waiting for ready to read\n");
//             PANIC();
//         }
//         if (*EMMC_INTERRUPT) {
//             printk("%d\n", *EMMC_INTERRUPT);
//             PANIC();
//         }
//         while (done < 128)
//             intbuf[done++] = get_EMMC_DATA();
//     }

//     if ((write | read) && (resp = sdWaitForInterrupt(INT_DATA_DONE))) {
//         printk("* EMMC ERROR: Timeout waiting for data done\n");
//         PANIC();
//     }
    
//     b->flags = 0;
//     if(write | read){ b->flags += B_VALID; }

//     dequeue(b);
    
//     buf* bb = get_first();
//     if(bb == NULL) {
//         post_sem(&b->sl);
//         return;
//     }
//     get_and_clear_EMMC_INTERRUPT();
//     sd_start(bb);
// }

// void sdrw(buf* b) {
//     int flag = b->flags;
    
//     _acquire_spinlock(&sd_lock);
    
    
//     if(_empty_list(&buf_root))
//     enqueue(b);
//     get_and_clear_EMMC_INTERRUPT();
//     sd_start(b);
//     _release_spinlock(&sd_lock);
//     init_sem(&b->sl,0);
//     while(flag == b->flags){
//         wait_sem(&b->sl);
//     }
// }

// /* SD card test and benchmark. */
// void sd_test() {
//     static struct buf b[1 << 11];
//     int n = sizeof(b) / sizeof(b[0]);
//     int mb = (n * BSIZE) >> 20;
//     // assert(mb);
//     if (!mb)
//         PANIC();
//     i64 f, t;
//     asm volatile("mrs %[freq], cntfrq_el0" : [freq] "=r"(f));
//     printk("- sd test: begin nblocks %d\n", n);

//     printk("- sd check rw...\n");
//     // Read/write test
//     for (int i = 1; i < n; i++) {
//         // Backup.
//         b[0].flags = 0;
//         b[0].blockno = (u32)i;
//         sdrw(&b[0]);
//         // Write some value.
//         b[i].flags = B_DIRTY;
//         b[i].blockno = (u32)i;
//         for (int j = 0; j < BSIZE; j++)
//             b[i].data[j] = (u8)((i * j) & 0xFF);
//         sdrw(&b[i]);

//         memset(b[i].data, 0, sizeof(b[i].data));
//         // Read back and check
//         b[i].flags = 0;
//         sdrw(&b[i]);
//         for (int j = 0; j < BSIZE; j++) {
//             //   assert(b[i].data[j] == (i * j & 0xFF));
//             if (b[i].data[j] != (i * j & 0xFF))
//                 PANIC();
//         }
//         // Restore previous value.
//         b[0].flags = B_DIRTY;
//         sdrw(&b[0]);
//     }

//     // Read benchmark
//     arch_dsb_sy();
//     t = (i64)get_timestamp();
//     arch_dsb_sy();
//     for (int i = 0; i < n; i++) {
//         b[i].flags = 0;
//         b[i].blockno = (u32)i;
//         sdrw(&b[i]);
//     }
//     arch_dsb_sy();
//     t = (i64)get_timestamp() - t;
//     arch_dsb_sy();
//     printk("- read %dB (%dMB), t: %lld cycles, speed: %lld.%lld MB/s\n",
//            n * BSIZE, mb, t, mb * f / t, (mb * f * 10 / t) % 10);

//     // Write benchmark
//     arch_dsb_sy();
//     t = (i64)get_timestamp();
//     arch_dsb_sy();
//     for (int i = 0; i < n; i++) {
//         b[i].flags = B_DIRTY;
//         b[i].blockno = (u32)i;
//         sdrw(&b[i]);
//     }
//     arch_dsb_sy();
//     t = (i64)get_timestamp() - t;
//     arch_dsb_sy();

//     printk("- write %dB (%dMB), t: %lld cycles, speed: %lld.%lld MB/s\n",
//            n * BSIZE, mb, t, mb * f / t, (mb * f * 10 / t) % 10);
// }