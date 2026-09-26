// SPDX-License-Identifier: GPL-2.0
/*
 * hello_misc.c - 最小的内核字符设备驱动：/dev/hello
 *
 * 演示内核驱动最基本的骨架：
 *   module_init/module_exit   加载/卸载入口
 *   file_operations           把 open/read/write/ioctl 系统调用接到驱动函数
 *   copy_to_user/from_user    内核和用户空间之间搬数据（不能直接解引用用户指针）
 *   mutex                     多个进程同时读写时的并发保护
 *
 * 用 misc 设备而不是 register_chrdev + class_create + device_create：
 * misc 框架自动分配次设备号、自动在 /dev 下建节点，是写"单个字符设备"最省事的方式。
 *
 *   make && sudo insmod hello_misc.ko
 *   echo "hi kernel" | sudo tee /dev/hello
 *   sudo cat /dev/hello              → hi kernel
 *   dmesg | tail                     → 看 pr_info 输出
 *   sudo rmmod hello_misc
 */
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>

#define BUF_SIZE 256

static char buf[BUF_SIZE];
static size_t buf_len;
static DEFINE_MUTEX(buf_lock);

static ssize_t hello_read(struct file *f, char __user *ubuf, size_t count, loff_t *ppos)
{
	ssize_t ret;

	mutex_lock(&buf_lock);
	/* simple_read_from_buffer 处理了 *ppos、越界和 copy_to_user，cat 读到 EOF 会自动停 */
	ret = simple_read_from_buffer(ubuf, count, ppos, buf, buf_len);
	mutex_unlock(&buf_lock);
	return ret;
}

static ssize_t hello_write(struct file *f, const char __user *ubuf, size_t count, loff_t *ppos)
{
	ssize_t ret;

	if (count > BUF_SIZE)
		return -EINVAL;
	mutex_lock(&buf_lock);
	ret = simple_write_to_buffer(buf, BUF_SIZE, ppos, ubuf, count);
	if (ret > 0)
		buf_len = *ppos;
	mutex_unlock(&buf_lock);
	pr_info("hello: %zd bytes written by pid %d (%s)\n", ret, current->pid, current->comm);
	return ret;
}

static const struct file_operations hello_fops = {
	.owner = THIS_MODULE,
	.read  = hello_read,
	.write = hello_write,
	.llseek = default_llseek,
};

static struct miscdevice hello_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "hello",
	.fops  = &hello_fops,
	.mode  = 0666,
};

static int __init hello_init(void)
{
	int ret = misc_register(&hello_dev);

	if (!ret)
		pr_info("hello: /dev/%s registered\n", hello_dev.name);
	return ret;
}

static void __exit hello_exit(void)
{
	misc_deregister(&hello_dev);
	pr_info("hello: unloaded\n");
}

module_init(hello_init);
module_exit(hello_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Minimal misc character device demo");
