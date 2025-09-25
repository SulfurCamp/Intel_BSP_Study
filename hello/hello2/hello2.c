// hello.c
#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>

static char *who = (char *)"world";
module_param(who, charp, 0644);
MODULE_PARM_DESC(who, "Greet target");

static int count = 1;
module_param(count, int, 0644);
MODULE_PARM_DESC(count, "How many times to greet");

static int __init hello_init(void)
{
    int i;
    for (i = 0; i < count; ++i)
        pr_info("Hello, %s! (%d/%d)\n", who, i+1, count);
    pr_debug("hello_init: debug-level message (use dynamic debug to see me)\n");
    return 0;
}

static void __exit hello_exit(void)
{
    pr_info("Goodbye, %s!\n", who);
}

module_init(hello_init);
module_exit(hello_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("진영");
MODULE_DESCRIPTION("Hello World kernel module for Ubuntu insmod/rmmod/dmesg practice");
