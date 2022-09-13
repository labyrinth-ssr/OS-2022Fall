#include <aarch64/intrinsic.h>
#include <kernel/init.h>
#include <driver/uart.h>
#include <common/string.h>
#include <common/defines.h>

#define HELLO_SIZE 16

static char hello[HELLO_SIZE];
extern char edata[],end[];

void init_bss(){
    memset(edata,0,(usize)(end-edata));
}
    

void define_early_init() {

    strncpy (hello,"hello world!",HELLO_SIZE);
    
}

void define_init(){
    for (int i = 0; i <HELLO_SIZE; i++){
        uart_put_char (hello[i]);
    }
}


NO_RETURN void main()
{
    if (cpuid()==0){
        do_early_init();
        do_init(); 
        init_bss();
        define_early_init();
        define_init();
    }
    
    arch_stop_cpu();

}
