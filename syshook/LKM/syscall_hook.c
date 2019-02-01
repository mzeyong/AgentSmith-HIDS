/*******************************************************************
* Project:	AgentSmith-HIDS
* Author:	E_BWill,k2yk
* Year:		2018
* File:		syscall_hook.c
* Description:	Hook execve,connect,init_module,finit_module Syscall

* AgentSmith-HIDS is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 2 of the License, or
* (at your option) any later version.
*
* AgentSmith-HIDS is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* see <https://www.gnu.org/licenses/>.
*******************************************************************/

#include "syscall_hook.h"
#include <asm/syscall.h>
#include <linux/binfmts.h>
#include <linux/device.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/atomic.h>
#include <linux/fs_struct.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/netlink.h>
#include <linux/sched.h>
#include <linux/skbuff.h>
#include <linux/syscalls.h>
#include <linux/utsname.h>
#include <linux/version.h>
#include <net/inet_sock.h>
#include <net/sock.h>

#define NETLINK_USER 31
#define NETLINK 1
#define SHERE_MEM 2
#define SEND_TYPE 2
#define KERNEL_PRINT -1
#define DELAY_TEST -1
#define WRITE_INDEX_TRY_LOCK -1
#define WRITE_INDEX_TRY_LOCK_NUM 3
#define CONNECT_TIME_TEST 0
#define EXECVE_TIME_TEST -1
#define DATA_ALIGMENT -1
#define SAFE_EXIT 1
#define EXIT_PROTECT -1

#define MAX_SIZE 2097152
#define CHECK_READ_INDEX_THRESHOLD 524288
#define CHECK_WRITE_INDEX_THRESHOLD 32768

#define DEVICE_NAME "smith"
#define CLASS_NAME "smith"
#define EXECVE_TYPE "59"
#define CONNECT_TYPE "42"
#define INIT_MODULE_TYPE "175"
#define FINIT_MODULE_TYPE "313"

unsigned long **sys_call_table_ptr;
void *orig_sys_call_table[NR_syscalls];
struct filename *(*tmp_getname)(const char __user *filename);
void (*tmp_putname)(struct filename *name);
struct sockaddr_in *sad;
extern asmlinkage long monitor_stub_execve_hook(
    const char __user *, const char __user *const __user *,
    const char __user *const __user *);

typedef asmlinkage long (*func_execve)(const char __user *,
                                       const char __user *const __user *,
                                       const char __user *const __user *);
typedef unsigned short int uint16;
typedef unsigned long int uint32;

asmlinkage long (*orig_connect)(int fd, struct sockaddr __user *dirp,
                                int addrlen);
asmlinkage long (*orig_finit_module)(int fd, const char __user *uargs,
                                     int flags);
asmlinkage long (*orig_init_module)(void __user *umod, unsigned long len,
                                    const char __user *uargs);

static int flen = 256;
static int use_count = 0;
static int netlink_pid = -1;
static int share_mem_flag = -1;
static int pre_slot_len = 0;
static int write_index = 8;
static int check_read_index_flag = -1;
static char *pname_buf;
static char *init_module_buf;
static char *pathname;
static char *tmp_stdin_fd;
static char *tmp_stdout_fd;
static char *slot_len;
static char *list_head_char;
struct sock *nl_sk;
static struct class *class;
static struct device *device;
static int major;
static char *sh_mem = NULL;
static func_execve orig_stub_execve;
static DEFINE_MUTEX(mchar_mutex);

static rwlock_t _write_index_lock;
static rwlock_t _write_use_count_lock;

static int write_index_trylock(void)
{
    return write_trylock(&_write_index_lock);
}

static inline void write_index_lock(void)
{
    write_lock(&_write_index_lock);
}

static inline void write_index_unlock(void)
{
    write_unlock(&_write_index_lock);
}

static inline void write_use_count_lock(void)
{
    write_lock(&_write_use_count_lock);
}

static inline void write_use_count_unlock(void)
{
    write_unlock(&_write_use_count_lock);
}

struct user_arg_ptr
{
#ifdef CONFIG_COMPAT
    bool is_compat;
#endif
    union {
        const char __user *const __user *native;
#ifdef CONFIG_COMPAT
        const compat_uptr_t __user *compat;
#endif
    } ptr;
};

#define NIPQUAD(addr)                \
    ((unsigned char *)&addr)[0],     \
        ((unsigned char *)&addr)[1], \
        ((unsigned char *)&addr)[2], \
        ((unsigned char *)&addr)[3]

#define NIP6(addr)                  \
    ntohs((addr).s6_addr16[0]),     \
        ntohs((addr).s6_addr16[1]), \
        ntohs((addr).s6_addr16[2]), \
        ntohs((addr).s6_addr16[3]), \
        ntohs((addr).s6_addr16[4]), \
        ntohs((addr).s6_addr16[5]), \
        ntohs((addr).s6_addr16[6]), \
        ntohs((addr).s6_addr16[7])

#define BigLittleSwap16(A) ((((uint16)(A)&0xff00) >> 8) | \
                            (((uint16)(A)&0x10ff) << 8))

static const char __user *get_user_arg_ptr(struct user_arg_ptr argv, int nr)
{
    const char __user *native;

#ifdef CONFIG_COMPAT
    if (unlikely(argv.is_compat))
    {
        compat_uptr_t compat;

        if (get_user(compat, argv.ptr.compat + nr))
            return ERR_PTR(-EFAULT);

        return compat_ptr(compat);
    }
#endif

    if (get_user(native, argv.ptr.native + nr))
        return ERR_PTR(-EFAULT);

    return native;
}

static int count(struct user_arg_ptr argv, int max)
{
    int i = 0;
    if (argv.ptr.native != NULL)
    {
        for (;;)
        {
            const char __user *p = get_user_arg_ptr(argv, i);
            if (!p)
                break;
            if (IS_ERR(p))
                return -EFAULT;
            if (i >= max)
                return -E2BIG;
            ++i;
            if (fatal_signal_pending(current))
                return -ERESTARTNOHAND;
            cond_resched();
        }
    }
    return i;
}

int checkCPUendian(void)
{
    union {
        unsigned long int i;
        unsigned char s[4];
    } c;
    c.i = 0x12345678;
    return (0x12 == c.s[0]);
}

unsigned short int Ntohs(unsigned short int n)
{
    return checkCPUendian() ? n : BigLittleSwap16(n);
}

static void get_start_time(ktime_t *start)
{
    *start = ktime_get();
}

static char *get_timespec(void)
{
    char *res = NULL;
    struct timespec tmp_time;
    res = kzalloc(64, GFP_ATOMIC);
    tmp_time = current_kernel_time();
    snprintf(res, 64, "%lu.%lu", tmp_time.tv_sec, tmp_time.tv_nsec);
    return res;
}

static long get_time_interval(ktime_t starttime)
{
    return ktime_to_ns(ktime_sub(ktime_get(), starttime));
}

static void enable_write_protection(void)
{
    unsigned long cr0 = read_cr0();
    set_bit(16, &cr0);
    write_cr0(cr0);
}

static void disable_write_protection(void)
{
    unsigned long cr0 = read_cr0();
    clear_bit(16, &cr0);
    write_cr0(cr0);
}

static int get_data_alignment(int len)
{
    int tmp = 0;

#if (DATA_ALIGMENT == -1)
    return len;
#else
    tmp = len % 4;

    if (tmp == 0)
        return len;
    else
        return (len + (4 - tmp));
#endif
}

static void kzalloc_init(void)
{
    pname_buf = kzalloc(flen, GFP_ATOMIC);
    init_module_buf = kzalloc(256, GFP_ATOMIC);
    pathname = kzalloc(PATH_MAX, GFP_ATOMIC);
    tmp_stdin_fd = kzalloc(256, GFP_ATOMIC);
    tmp_stdout_fd = kzalloc(256, GFP_ATOMIC);
    slot_len = kzalloc(8, GFP_ATOMIC);
    list_head_char = kzalloc(8, GFP_ATOMIC);
}

static void lock_init(void)
{
    rwlock_init(&_write_index_lock);
    rwlock_init(&_write_use_count_lock);
}

static void kzalloc_exit(void)
{
    int i;
    if (pname_buf)
        kfree(pname_buf);

    if (init_module_buf)
        kfree(init_module_buf);

    if (pathname)
        kfree(pathname);

    if(tmp_stdin_fd)
        kfree(tmp_stdin_fd);

    if(tmp_stdout_fd)
        kfree(tmp_stdout_fd);

    if (slot_len)
        kfree(slot_len);

#if (SEND_TYPE == SHERE_MEM)
    if (sh_mem)
    {
        for (i = 0; i < MAX_SIZE; i += PAGE_SIZE)
            ClearPageReserved(virt_to_page(((unsigned long)sh_mem) + i));
        kfree(sh_mem);
    }
#endif

    if (list_head_char)
        kfree(list_head_char);
}

static char *str_replace(char *orig, char *rep, char *with)
{
    char *result, *ins, *tmp;
    int len_rep, len_with, len_front, count;

    if (!orig || !rep)
        return NULL;

    len_rep = strlen(rep);
    if (len_rep == 0)
        return NULL;
    if (!with)
        with = "";
    len_with = strlen(with);

    ins = orig;
    for (count = 0; (tmp = strstr(ins, rep)); ++count)
        ins = tmp + len_rep;

    tmp = result = kzalloc(strlen(orig) + (len_with - len_rep) * count + 1, GFP_ATOMIC);

    if (!result)
        return NULL;

    while (count--)
    {
        ins = strstr(orig, rep);
        len_front = ins - orig;
        tmp = strncpy(tmp, orig, len_front) + len_front;
        tmp = strcpy(tmp, with) + len_with;
        orig += len_front + len_rep;
    }

    strcpy(tmp, orig);
    return result;
}

static void exit_protect_action(void)
{
    preempt_disable();
    atomic_inc(&THIS_MODULE->refcnt);
    preempt_enable();
}

static void update_use_count(void)
{
    write_use_count_lock();

    if (use_count == 0)
    {
        preempt_disable();
        atomic_inc(&THIS_MODULE->refcnt);
        preempt_enable();
    }

    use_count = use_count + 1;
    write_use_count_unlock();
}

static void del_use_count(void)
{
    write_use_count_lock();
    use_count = use_count - 1;

    if (use_count == 0)
    {
        preempt_disable();
        atomic_dec(&THIS_MODULE->refcnt);
        preempt_enable();
    }

    write_use_count_unlock();
}

static struct socket *sockfd_lookup_light(int fd, int *err, int *fput_needed)
{
    struct file *file;
    struct socket *sock;

    *err = -EBADF;
    file = fget(fd);
    if (file)
    {
        sock = sock_from_file(file, err);
        if (sock)
            return sock;
        fput_light(file, *fput_needed);
    }
    return NULL;
}

static int device_mmap(struct file *filp, struct vm_area_struct *vma)
{
    int ret = 0;
    struct page *page = NULL;
    unsigned long size = (unsigned long)(vma->vm_end - vma->vm_start);

    vma->vm_flags |= 0;

    if (size > MAX_SIZE)
    {
        ret = -EINVAL;
        goto out;
    }

    page = virt_to_page((unsigned long)sh_mem + (vma->vm_pgoff << PAGE_SHIFT));
    ret = remap_pfn_range(vma, vma->vm_start, page_to_pfn(page), size, vma->vm_page_prot);
    if (ret != 0)
    {
        goto out;
    }

out:
    return ret;
}

static void init_share_mem(int type)
{
    struct sh_mem_list_head _init_list_head = {0, -1};
    if (type == 1)
        _init_list_head.next = 8;
    memcpy(sh_mem, &_init_list_head, 8);
}

static int device_open(struct inode *inode, struct file *file)
{
    if (!mutex_trylock(&mchar_mutex))
    {
        return -1;
    }
    else
    {
        share_mem_flag = 1;
        write_index_lock();
        pre_slot_len = 0;
        write_index_unlock();
        write_index = 8;
        memset(sh_mem, '\0', MAX_SIZE);
        init_share_mem(0);
    }
    return 0;
}

static int device_close(struct inode *indoe, struct file *file)
{
    mutex_unlock(&mchar_mutex);
    share_mem_flag = -1;
    return 0;
}

static const struct file_operations mchar_fops = {
    .owner = THIS_MODULE,
    .open = device_open,
    .release = device_close,
    .mmap = device_mmap,
};

static int get_read_index(void)
{
    int i;
    struct sh_mem_list_head *_list_head;

    for (i = 0; i < 8; i++)
        list_head_char[i] = sh_mem[i];

    _list_head = (struct sh_mem_list_head *)list_head_char;
    return _list_head->read_index;
}

static int get_write_index(void)
{
    return write_index;
}

static void fix_write_index(int index)
{
    write_index = index;
}

static struct msg_slot get_solt(int len, int next)
{
    struct msg_slot new_msg_slot = {len, next};
    return new_msg_slot;
}

static int send_msg_to_user_memshare(char *msg, int kfree_flag)
{
    int i;
    int raw_data_len = 0;
    int write_index_lock_flag = 0;
    int curr_write_index = -1;
    int now_write_index = -1;
    int now_read_index = -1;
    struct msg_slot new_msg_slot;

    if (share_mem_flag == 1)
    {

#if (WRITE_INDEX_TRY_LOCK == 1)
        for (i = 0; i < WRITE_INDEX_TRY_LOCK_NUM; i++)
        {
            if (write_index_trylock())
            {
                write_index_lock_flag = 1;
                break;
            }
        }
#else
        write_index_lock();
        write_index_lock_flag = 1;
#endif

        if (write_index_lock_flag)
        {

            raw_data_len = strlen(msg);

            if (raw_data_len == 0)
            {
                write_index_unlock();
                if (msg && kfree_flag == 1)
                    kfree(msg);

                return 0;
            }

            curr_write_index = get_write_index();

            if (pre_slot_len != 0)
                now_write_index = curr_write_index + 1;
            else
                now_write_index = curr_write_index;

            if (check_read_index_flag == 1)
            {
                now_read_index = get_read_index();
                if (now_read_index > curr_write_index + 1)
                {
                    if ((curr_write_index + 1024 + raw_data_len) > now_read_index)
                        goto out;
                }
            }

            if ((curr_write_index + CHECK_WRITE_INDEX_THRESHOLD) >= MAX_SIZE)
            {
                now_read_index = get_read_index();
                if (now_read_index <= CHECK_READ_INDEX_THRESHOLD)
                {
#if (KERNEL_PRINT == 1)
                    printk("READ IS TOO SLOW!! READ_INDEX:%d\n", now_read_index);
#endif
                    check_read_index_flag = 1;
                }
                else
                    check_read_index_flag = -1;

                new_msg_slot = get_solt(raw_data_len, 1);
                memcpy(&sh_mem[now_write_index], &new_msg_slot, 8);
                memcpy(&sh_mem[now_write_index + 8], msg, raw_data_len);
                fix_write_index(7);

#if (KERNEL_PRINT == 1)
                printk("curr_write_index:%d pre_slot_len:%d now_write_index:%d now_read_index:%d\n",
                       curr_write_index, pre_slot_len, now_write_index, now_read_index);
#endif
            }
            else
            {
                new_msg_slot = get_solt(raw_data_len, -1);
                memcpy(&sh_mem[now_write_index], &new_msg_slot, 8);
                memcpy(&sh_mem[now_write_index + 8], msg, raw_data_len);
                fix_write_index(now_write_index + 8 + raw_data_len);
            }

            pre_slot_len = raw_data_len + 8;
            write_index_unlock();
        }
    }

    if (msg && kfree_flag == 1)
        kfree(msg);

    return 0;

out:
    write_index_unlock();
    if (msg && kfree_flag == 1)
        kfree(msg);
    return 0;
}

static int send_msg_to_user_netlink(char *msg, int kfree_flag)
{
    int msg_size;
    struct nlmsghdr *nlh;
    struct sk_buff *skb_out;
    msg_size = strlen(msg);

    if (netlink_pid == -1)
        goto err;

    if (msg_size == 0)
        goto err;

    skb_out = nlmsg_new(msg_size, GFP_KERNEL);
    if (!skb_out)
        goto err;

    nlh = nlmsg_put(skb_out, 0, 0, NLMSG_DONE, msg_size, 0);
    NETLINK_CB(skb_out).dst_group = 0;
    strncpy(nlmsg_data(nlh), msg, msg_size);
    nlmsg_unicast(nl_sk, skb_out, netlink_pid);

    if (msg && kfree_flag == 1)
        kfree(msg);
    return 0;

err:
    if (msg && kfree_flag == 1)
        kfree(msg);
    return -1;
}

static int send_msg_to_user(int type, char *msg, int kfree_flag)
{
#if (KERNEL_PRINT == 2)
    printk("%s\n", msg);
#endif

#if (DELAY_TEST == 1)
    if (kfree_flag == 1)
    {
        if (type == SHERE_MEM)
            send_msg_to_user_memshare(get_timespec(), 1);
        else if (type == NETLINK)
            send_msg_to_user_netlink(get_timespec(), 1);

        if (msg)
            kfree(msg);
        return 0;
    }
#endif

#if (SEND_TYPE == NETLINK)
    return send_msg_to_user_netlink(msg, kfree_flag);
#elif (SEND_TYPE == SHERE_MEM)
    return send_msg_to_user_memshare(msg, kfree_flag);
#else
    if (msg && kfree_flag == 1)
        kfree(msg);
#endif

    return 0;
}

static void nl_recv_msg(struct sk_buff *skb)
{
    int recv_msg_len;
    char *recv_msg;
    struct nlmsghdr *nlh;
    nlh = (struct nlmsghdr *)skb->data;
    netlink_pid = nlh->nlmsg_pid;
    recv_msg_len = strlen((char *)nlmsg_data(nlh)) + 1;
    recv_msg = kzalloc(recv_msg_len, GFP_ATOMIC);
    if (recv_msg)
    {
        snprintf(recv_msg, recv_msg_len, (char *)nlmsg_data(nlh));
        if (strcmp("NIDS_AGENT_UP", recv_msg) != 0)
        {
            send_msg_to_user(NETLINK, "DOWN_SUCCESS", 0);
            netlink_pid = -1;
        }
        else
            send_msg_to_user(NETLINK, "UP_SUCCESS", 0);
        kfree(recv_msg);
    }
}

asmlinkage long monitor_execve_hook(const char __user *filename, const char __user *const __user *argv, const char __user *const __user *envp)
{

    char *result_str;
    int result_str_len;
    int argv_len = 0, argv_res_len = 0, i = 0, len = 0, offset = 0;
    struct filename *path;
    const char __user *native;
    const char *abs_path = NULL;
    char *argv_res = NULL;
    char *argv_res_tmp = NULL;
    char *pname = NULL;
    ktime_t stime;
    char *ktime_result_str = NULL;
    char *tmp_stdin = NULL;
    char *tmp_stdout = NULL;
    struct fdtable *files;
    struct user_arg_ptr argv_ptr = {.ptr.native = argv};

    if (netlink_pid == -1 && share_mem_flag == -1)
        return 0;

#if (EXECVE_TIME_TEST == 1)
    get_start_time(&stime);
#endif

    files = files_fdtable(current->files);

    if (!tmp_stdin_fd)
        tmp_stdin_fd = kzalloc(256, GFP_ATOMIC);

    if (!tmp_stdout_fd)
        tmp_stdout_fd = kzalloc(256, GFP_ATOMIC);

    if(files->fd[0] != NULL)
        tmp_stdin = d_path(&(files->fd[0]->f_path), tmp_stdin_fd, 256);
    else
        tmp_stdin = "-1";

    if(files->fd[1] != NULL)
        tmp_stdout = d_path(&(files->fd[1]->f_path), tmp_stdout_fd, 256);
    else
        tmp_stdout = "-1";

    path = tmp_getname(filename);
    if (!IS_ERR(path))
        abs_path = path->name;
    else
        abs_path = "-1";

    if (pname_buf)
    {
        pname_buf = memset(pname_buf, '\0', flen);
        pname = dentry_path_raw(current->fs->pwd.dentry, pname_buf, flen - 1);
    }
    else
        pname_buf = kzalloc(flen, GFP_ATOMIC);

    argv_len = count(argv_ptr, MAX_ARG_STRINGS);
    if (argv_len < 0)
        goto err;

    argv_res = kzalloc(argv_res_len + 16 * argv_len, GFP_ATOMIC);
    if (!argv_res)
        goto err;

    for (i = 0; i < argv_len; i++)
    {
        native = get_user_arg_ptr(argv_ptr, i);
        if (IS_ERR(native))
            goto err;

        len = strnlen_user(native, MAX_ARG_STRLEN);
        if (!len)
            goto err;

        if (offset + len > argv_res_len + 16 * argv_len)
            break;

        if (copy_from_user(argv_res + offset, native, len))
            goto err;

        offset += len - 1;
        *(argv_res + offset) = ' ';
        offset += 1;
    }

    argv_res_tmp = str_replace(argv_res, "\n", " ");

    result_str_len =
        get_data_alignment(strlen(argv_res_tmp) + strlen(pname) +
                           strlen(abs_path) +
                           strlen(current->nsproxy->uts_ns->name.nodename) + 1024);

    if(result_str_len > 10240)
        goto err;

    result_str = kzalloc(result_str_len, GFP_ATOMIC);

    snprintf(result_str, result_str_len,
             "%d%s%s%s%s%s%s%s%s%s%d%s%d%s%d%s%d%s%s%s%s%s%s%s%s",
             current->real_cred->uid.val, "\n", EXECVE_TYPE, "\n", pname, "\n",
             abs_path, "\n", argv_res_tmp, "\n", current->pid, "\n",
             current->real_parent->pid, "\n", pid_vnr(task_pgrp(current)),
             "\n", current->tgid, "\n", current->comm, "\n",
             current->nsproxy->uts_ns->name.nodename,"\n",tmp_stdin,"\n",tmp_stdout);

    send_msg_to_user(SEND_TYPE, result_str, 1);

    tmp_stdin_fd = memset(tmp_stdin_fd, '\0', 256);
    tmp_stdout_fd = memset(tmp_stdout_fd, '\0', 256);

    if (argv_res)
        kfree(argv_res);

    if (argv_res_tmp)
        kfree(argv_res_tmp);

    tmp_putname(path);

#if (EXECVE_TIME_TEST == 1)
    ktime_result_str = kzalloc(16, GFP_ATOMIC);
    snprintf(ktime_result_str, 16, "%ld", get_time_interval(stime));
    send_msg_to_user(SEND_TYPE, ktime_result_str, 1);
#endif

    return 0;

err:
    if (argv_res)
        kfree(argv_res);

    if (argv_res_tmp)
        kfree(argv_res_tmp);

    tmp_putname(path);

    tmp_stdin_fd = memset(tmp_stdin_fd, '\0', 256);
    tmp_stdout_fd = memset(tmp_stdout_fd, '\0', 256);

    return -1;
}

asmlinkage long monitor_init_module_hook(void __user *umod, unsigned long len, const char __user *uargs)
{
    char *result_str;
    int i = 0;
    int result_str_len;
    struct files_struct *current_files;
    struct fdtable *files_table;
    struct path files_path;
    char *cwd;
    current_files = current->files;
    files_table = files_fdtable(current_files);

    if (netlink_pid == -1 && share_mem_flag == -1)
        return orig_init_module(umod, len, uargs);

    if (init_module_buf)
        init_module_buf = memset(init_module_buf, '\0', 256);
    else
        init_module_buf = kzalloc(256, GFP_ATOMIC);

    while (files_table->fd[i] != NULL)
        i++;

    files_path = files_table->fd[i - 1]->f_path;
    cwd = d_path(&files_path, init_module_buf, 256);
    result_str_len = get_data_alignment(strlen(cwd) + 4);
    result_str = kzalloc(result_str_len, GFP_ATOMIC);
    snprintf(result_str, result_str_len, "%d%s%s%s%s%s%d%s%d%s%d%s%d%s%s%s%s",
             current->real_cred->uid.val, "\n", INIT_MODULE_TYPE, "\n", cwd,
             "\n", current->pid, "\n", current->real_parent->pid, "\n",
             pid_vnr(task_pgrp(current)), "\n", current->tgid, "\n",
             current->comm, "\n", current->nsproxy->uts_ns->name.nodename);

    send_msg_to_user(SEND_TYPE, result_str, 1);
    return orig_init_module(umod, len, uargs);
}

asmlinkage long monitor_finit_module_hook(int fd, const char __user *uargs, int flags)
{
    char *result_str;
    int result_str_len;
    int i = 0;
    struct files_struct *current_files;
    struct fdtable *files_table;
    struct path files_path;
    char *cwd;
    current_files = current->files;
    files_table = files_fdtable(current_files);

    if (netlink_pid == -1 && share_mem_flag == -1)
        return orig_finit_module(fd, uargs, flags);

    if (init_module_buf)
        init_module_buf = memset(init_module_buf, '\0', 256);
    else
        init_module_buf = kzalloc(256, GFP_ATOMIC);

    while (files_table->fd[i] != NULL)
        i++;

    files_path = files_table->fd[i - 1]->f_path;
    cwd = d_path(&files_path, init_module_buf, 256);
    result_str_len = get_data_alignment(strlen(cwd) + 4);
    result_str = kzalloc(result_str_len, GFP_ATOMIC);
    snprintf(result_str, result_str_len, "%d%s%s%s%s%s%d%s%d%s%d%s%d%s%s%s%s",
             current->real_cred->uid.val, "\n", FINIT_MODULE_TYPE, "\n", cwd,
             "\n", current->pid, "\n", current->real_parent->pid, "\n",
             pid_vnr(task_pgrp(current)), "\n", current->tgid, "\n",
             current->comm, "\n", current->nsproxy->uts_ns->name.nodename);

    send_msg_to_user(SEND_TYPE, result_str, 1);
    return orig_finit_module(fd, uargs, flags);
}

asmlinkage long monitor_connect_no_hook_time_test(int fd, struct sockaddr __user *dirp, int addrlen)
{
    ktime_t stime;
    char *result_str = NULL;
    struct sockaddr tmp_dirp;
    long ori_connect_syscall_res;
    int copy_res = copy_from_user(&tmp_dirp, dirp, 16);
    get_start_time(&stime);
    ori_connect_syscall_res = orig_connect(fd, dirp, addrlen);
    result_str = kzalloc(16, GFP_ATOMIC);
    snprintf(result_str, 16, "%ld", get_time_interval(stime));
    if (copy_res == 0)
    {
        if (tmp_dirp.sa_family == AF_INET)
            send_msg_to_user(SEND_TYPE, result_str, 1);
    }

    return ori_connect_syscall_res;
}

asmlinkage long monitor_connect_hook(int fd, struct sockaddr __user *dirp, int addrlen)
{
    char dip[64];
    char dport[16];
    char sip[64] = "-1";
    char sport[16] = "-1";
    char *final_path = NULL;
    char *result_str = NULL;
    char *ktime_result_str = NULL;
    int flag = 0;
    int sa_family = 0;
    int err, fput_needed;
    int result_str_len;
    ktime_t stime;
    struct sockaddr tmp_dirp;
    struct sockaddr_in *sin;
    struct sockaddr_in6 *sin6;
    struct socket *sock;
    struct sockaddr_in source_addr;
    struct sockaddr_in6 source_addr6;
    int len = sizeof(source_addr);
    int copy_res = copy_from_user(&tmp_dirp, dirp, 16);
    long ori_connect_syscall_res = 0;

#if (SAFE_EXIT == 1)
    update_use_count();
#endif

    ori_connect_syscall_res = orig_connect(fd, dirp, addrlen);

    if (netlink_pid == -1 && share_mem_flag == -1)
    {
        del_use_count();
        return ori_connect_syscall_res;
    }

    if (copy_res == 0)
    {

#if (CONNECT_TIME_TEST == 2)
        get_start_time(&stime);
#endif

        if (tmp_dirp.sa_family == AF_INET)
        {
            flag = 1;
            sa_family = 4;
            sock = sockfd_lookup_light(fd, &err, &fput_needed);
            if (sock)
            {
                kernel_getsockname(sock, (struct sockaddr *)&source_addr, &len);
                snprintf(sport, 16, "%d", Ntohs(source_addr.sin_port));
                snprintf(sip, 64, "%d.%d.%d.%d", NIPQUAD(source_addr.sin_addr));
                fput_light(sock->file, fput_needed);
            }
            sin = (struct sockaddr_in *)&tmp_dirp;
            snprintf(dip, 64, "%d.%d.%d.%d", NIPQUAD(sin->sin_addr.s_addr));
            snprintf(dport, 16, "%d", Ntohs(sin->sin_port));
        }
        else if (tmp_dirp.sa_family == AF_INET6)
        {
            flag = 1;
            sa_family = 6;
            sock = sockfd_lookup_light(fd, &err, &fput_needed);
            if (sock)
            {
                kernel_getsockname(sock, (struct sockaddr *)&source_addr6, &len);
                snprintf(sport, 16, "%d", Ntohs(source_addr6.sin6_port));
                snprintf(sip, 64, "%d:%d:%d:%d:%d:%d:%d:%d", NIP6(source_addr6.sin6_addr));
                fput_light(sock->file, fput_needed);
            }
            sin6 = (struct sockaddr_in6 *)&tmp_dirp;
            snprintf(dip, 64, "%d:%d:%d:%d:%d:%d:%d:%d", NIP6(sin6->sin6_addr));
            snprintf(dport, 16, "%d", Ntohs(sin6->sin6_port));
        }

        if (flag == 1)
        {
            if (current->active_mm)
            {
                if (current->mm->exe_file)
                {
                    if (pathname)
                    {
                        pathname = memset(pathname, '\0', PATH_MAX);
                        final_path = d_path(&current->mm->exe_file->f_path, pathname, PATH_MAX);
                    }
                    else
                    {
                        pathname = kzalloc(PATH_MAX, GFP_ATOMIC);
                    }
                }
            }
            result_str_len =
                get_data_alignment(strlen(current->comm) +
                                   strlen(current->nsproxy->uts_ns->name.nodename) +
                                   strlen(current->comm) + strlen(final_path) + 172);
            result_str = kzalloc(result_str_len, GFP_ATOMIC);
            snprintf(result_str, result_str_len,
                     "%d%s%s%s%d%s%d%s%s%s%s%s%s%s%d%s%d%s%d%s%d%s%s%s%s%s%s%s%s",
                     current->real_cred->uid.val, "\n", CONNECT_TYPE, "\n", sa_family,
                     "\n", fd, "\n", dport, "\n", dip, "\n", final_path, "\n",
                     current->pid, "\n", current->real_parent->pid, "\n",
                     pid_vnr(task_pgrp(current)), "\n", current->tgid, "\n",
                     current->comm, "\n", current->nsproxy->uts_ns->name.nodename, "\n",
                     sip, "\n", sport);

            send_msg_to_user(SEND_TYPE, result_str, 1);

#if (CONNECT_TIME_TEST == 2)
            ktime_result_str = kzalloc(16, GFP_ATOMIC);
            snprintf(ktime_result_str, 16, "%ld", get_time_interval(stime));
            send_msg_to_user(SEND_TYPE, ktime_result_str, 1);
#endif
        }
    }

#if (SAFE_EXIT == 1)
    del_use_count();
#endif

    return ori_connect_syscall_res;
}

unsigned long **find_sys_call_table(void)
{
    unsigned long ptr;
    unsigned long *p;
    for (ptr = (unsigned long)sys_close;
         ptr < (unsigned long)&loops_per_jiffy;
         ptr += sizeof(void *))
    {
        p = (unsigned long *)ptr;
        if (p[__NR_close] == (unsigned long)sys_close)
            return (unsigned long **)p;
    }
    return NULL;
}

static int lkm_init(void)
{
    int i = 0;

    if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 0, 0))
    {
        pr_err("KERNEL_VERSION_DON'T_SUPPORT\n");
        return -1;
    }

#if (SEND_TYPE == SHERE_MEM)
    major = register_chrdev(0, DEVICE_NAME, &mchar_fops);

    if (major < 0)
    {
        pr_err("REGISTER_CHRDEV_ERROR\n");
        return -1;
    }

    class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(class))
    {
        unregister_chrdev(major, DEVICE_NAME);
        pr_err("CLASS_CREATE_ERROR");
        return -1;
    }

    device = device_create(class, NULL, MKDEV(major, 0), NULL, DEVICE_NAME);
    if (IS_ERR(device))
    {
        class_destroy(class);
        unregister_chrdev(major, DEVICE_NAME);
        pr_err("DEVICE_CREATE_ERROR");
        return -1;
    }

    sh_mem = kzalloc(MAX_SIZE, GFP_KERNEL);
    if (sh_mem == NULL)
    {
        device_destroy(class, MKDEV(major, 0));
        class_destroy(class);
        unregister_chrdev(major, DEVICE_NAME);
        pr_err("SHMEM_INIT_ERROR\n");
        return -ENOMEM;
    }
    else
    {
        for (i = 0; i < MAX_SIZE; i += PAGE_SIZE)
            SetPageReserved(virt_to_page(((unsigned long)sh_mem) + i));
    }
    mutex_init(&mchar_mutex);
#else
    struct netlink_kernel_cfg cfg = {
        .input = nl_recv_msg,
    };

    nl_sk = netlink_kernel_create(&init_net, NETLINK_USER, &cfg);
    if (!nl_sk)
    {
        if (SEND_TYPE == SHERE_MEM)
        {
            device_destroy(class, MKDEV(major, 0));
            class_destroy(class);
            unregister_chrdev(major, DEVICE_NAME);
        }
        pr_err(KERN_ALERT "HIDS_ERROR_CREATING_SOCKET\n");
        return -1;
    }
#endif

    if (!(sys_call_table_ptr = find_sys_call_table()))
    {
        if (SEND_TYPE == SHERE_MEM)
        {
            device_destroy(class, MKDEV(major, 0));
            class_destroy(class);
            unregister_chrdev(major, DEVICE_NAME);
        }
        pr_err("GET_SYS_CALL_TABLE_FAILED\n");
        return -1;
    }

    kzalloc_init();
    lock_init();

    tmp_getname = (void *)kallsyms_lookup_name("getname");
    tmp_putname = (void *)kallsyms_lookup_name("putname");

    if (!tmp_getname)
    {
        if (SEND_TYPE == SHERE_MEM)
        {
            mutex_destroy(&mchar_mutex);
            device_destroy(class, MKDEV(major, 0));
            class_destroy(class);
            unregister_chrdev(major, DEVICE_NAME);
        }
        pr_err("UNKONW_SYMBOL_GETNAME\n");
        return -1;
    }

    for (i = 0; i < NR_syscalls - 1; i++)
        orig_sys_call_table[i] = sys_call_table_ptr[i];
    disable_write_protection();
    orig_connect = (void *)(sys_call_table_ptr[__NR_connect]);

#if (CONNECT_TIME_TEST == 1)
    sys_call_table_ptr[__NR_connect] = (void *)monitor_connect_no_hook_time_test;
#else
    sys_call_table_ptr[__NR_connect] = (void *)monitor_connect_hook;
#endif

#if (CONNECT_TIME_TEST == 0)
    orig_finit_module = (void *)(sys_call_table_ptr[__NR_finit_module]);
    sys_call_table_ptr[__NR_finit_module] = (void *)monitor_finit_module_hook;

    orig_init_module = (void *)(sys_call_table_ptr[__NR_init_module]);
    sys_call_table_ptr[__NR_init_module] = (void *)monitor_init_module_hook;

    orig_stub_execve = (void *)(sys_call_table_ptr[__NR_execve]);
    sys_call_table_ptr[__NR_execve] = (void *)monitor_stub_execve_hook;
#endif

    enable_write_protection();

#if (EXIT_PROTECT == 1)
    exit_protect_action()
#endif

        return 0;
}

static void lkm_exit(void)
{
    netlink_kernel_release(nl_sk);
    disable_write_protection();
    sys_call_table_ptr[__NR_connect] = (void *)orig_connect;

#if (CONNECT_TIME_TEST == 0)
    sys_call_table_ptr[__NR_finit_module] = (void *)orig_finit_module;
    sys_call_table_ptr[__NR_init_module] = (void *)orig_init_module;
    sys_call_table_ptr[__NR_execve] = (void *)orig_stub_execve;
#endif

    sys_call_table_ptr = NULL;
    enable_write_protection();

#if (SEND_TYPE == SHERE_MEM)
    device_destroy(class, MKDEV(major, 0));
    class_unregister(class);
    class_destroy(class);
    unregister_chrdev(major, DEVICE_NAME);
#endif

    kzalloc_exit();
}

module_init(lkm_init);
module_exit(lkm_exit);

MODULE_LICENSE("GPL v2");
MODULE_VERSION("0.1.4");
MODULE_AUTHOR("k2yk <mzeyong@gmail.com>");
MODULE_DESCRIPTION("Monitor Syscall: execve,connect,init_module,finit_module");
