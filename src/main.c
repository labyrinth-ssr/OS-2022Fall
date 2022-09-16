#include <aarch64/intrinsic.h>
#include <kernel/init.h>
#include <driver/uart.h>
#include <common/string.h>
#include <common/defines.h>


static char hello[16];
extern char edata[],end[];

void init_bss(){
    memset(edata,0,(usize)(end-edata));
}
    

define_early_init (hello){

    strncpy (hello,"hello world!",16);
    
}

define_init (print){
    for (char* p=hello; *p; p++)
    {
        uart_put_char(*p);
    }
    
}


NO_RETURN void main()
{
    if (cpuid()==0){
        init_bss();
        do_early_init();
        do_init(); 
    }
    
    arch_stop_cpu();

}
