#include <linux/kernel.h>
#include <linux/syscalls.h>

SYSCALL_DEFINE3(mysyscall, int, arg1, char __user *, arg2, size_t, arg3)
{
    printk(KERN_INFO "Hello from mysyscall! arg1=%d\n", arg1);
    
    // Your implementation here
    
    return 0;
}