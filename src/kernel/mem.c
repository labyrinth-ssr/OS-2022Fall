#include <common/rc.h>
#include <kernel/init.h>
#include <kernel/mem.h>
#include <driver/memlayout.h>
#include <common/list.h>
#include <aarch64/mmu.h>

#define MAX_ORDER 10 
#define PAGE_ADDR_SIZE 12

extern char end[];
static SpinLock list_lock;

define_early_init(alloc_page_lock)
{
    init_spinlock(&list_lock);
}

int pow(int x,int n){
    int res=1;
    for (int i = 0; i < n; i++)
    {
        res *= x;
    }
    return res;
}

// typedef struct FreeList {
//     int order;
//     ListNode* head;
// }FreeList;

ListNode* freeArea[MAX_ORDER];
// FreeList listMaxOrder;
ListNode* head=P2K(EXTMEM);

void init_page_list(){
    for ( int i = 0; i < MAX_ORDER; i++)
    {
        freeArea[i]=NULL;
    }
    
    u32 p=(u32)head;
    while (p<=(u32)(P2K(PHYSTOP))){
        p+= PAGE_ADDR_SIZE * pow(2,MAX_ORDER-1);
        insert_into_list(list_lock,&head,(ListNode*)P2K(p));
    }
    freeArea[MAX_ORDER-1]=head;

}

RefCount alloc_page_cnt;

define_early_init(alloc_page_cnt)
{
    init_rc(&alloc_page_cnt);
}


void* kalloc_page()
{
    _increment_rc(&alloc_page_cnt);
    // TODO
    init_page_list();
    int order=1;
    int i=order;
    while (i<=MAX_ORDER)
    {
        if (freeArea[i-1]==NULL)
        {
            i++;
        } else {
            u32 fetchHead=(u32)&freeArea[i-1];
            detach_from_list(list_lock,freeArea[i-1]);
            while (i!=order)
            {
                i--;
                insert_into_list (list_lock,&freeArea[i-1],(ListNode*)fetchHead+PAGE_ADDR_SIZE * pow(2,i));
            }
            return (void*)fetchHead;
        }
    }
    return NULL;
}

void kfree_page(void* p)
{
    _decrement_rc(&alloc_page_cnt);
    // TODO
    int i=0;
    while (i<=MAX_ORDER)
    {
        if ((u32)p+PAGE_ADDR_SIZE * pow(2,i)==(u32)freeArea[i] || (u32)p-PAGE_ADDR_SIZE * pow(2,i)==(u32)freeArea[i] )
        {
           p=MIN(freeArea[i],p);
           i++;
           continue;
        } 
        else 
        {
            auto node=freeArea[i];
             _for_in_list(node,&freeArea[i]){
                if (node==(u32)p+PAGE_ADDR_SIZE * pow(2,i) || node==(u32)p-PAGE_ADDR_SIZE * pow(2,i))
                {
                    i++;
                    continue;   
                }
            }
            break;
        }
    }
}

// TODO: kalloc kfree
void* kalloc(isize size){

}

void kfree(void* p){

}