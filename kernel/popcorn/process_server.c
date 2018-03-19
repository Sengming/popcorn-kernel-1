/**
 * @file process_server.c
 *
 * Popcorn Linux thread migration implementation
 * This work was an extension of David Katz MS Thesis, but totally rewritten 
 * by Sang-Hoon to support multithread environment.
 *
 * @author Sang-Hoon Kim, SSRG Virginia Tech 2017
 * @author Antonio Barbalace, SSRG Virginia Tech 2014-2016
 * @author Vincent Legout, Sharat Kumar Bath, Ajithchandra Saya, SSRG Virginia Tech 2014-2015
 * @author David Katz, Marina Sadini, SSRG Virginia 2013
 */

#include <linux/sched.h>
#include <linux/threads.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/ptrace.h>
#include <linux/mmu_context.h>
#include <linux/fs.h>
#include <linux/futex.h>

#include <asm/mmu_context.h>
#include <asm/kdebug.h>
#include <asm/uaccess.h>

#include <popcorn/types.h>
#include <popcorn/bundle.h>
#include <popcorn/cpuinfo.h>

#include "types.h"
#include "process_server.h"
#include "vma_server.h"
#include "page_server.h"
#include "wait_station.h"
#include "util.h"

static struct list_head remote_contexts[2];
static spinlock_t remote_contexts_lock[2];

enum {
	INDEX_OUTBOUND = 0,
	INDEX_INBOUND = 1,
};

/* Hold the correnponding remote_contexts_lock */
static struct remote_context *__lookup_remote_contexts_in(int nid, int tgid)
{
	struct remote_context *rc, *tmp;

	list_for_each_entry_safe(rc, tmp, remote_contexts + INDEX_INBOUND, list) {
		if (rc->remote_tgids[nid] == tgid) {
			return rc;
		}
	}
	return NULL;
}

#define __lock_remote_contexts(index) \
	spin_lock(remote_contexts_lock + index)
#define __lock_remote_contexts_in(nid) \
	__lock_remote_contexts(INDEX_INBOUND)
#define __lock_remote_contexts_out(nid) \
	__lock_remote_contexts(INDEX_OUTBOUND)

#define __unlock_remote_contexts(index) \
	spin_unlock(remote_contexts_lock + index)
#define __unlock_remote_contexts_in(nid) \
	__unlock_remote_contexts(INDEX_INBOUND)
#define __unlock_remote_contexts_out(nid) \
	__unlock_remote_contexts(INDEX_OUTBOUND)

#define __remote_contexts_in() remote_contexts[INDEX_INBOUND]
#define __remote_contexts_out() remote_contexts[INDEX_OUTBOUND]


inline struct remote_context *get_task_remote(struct task_struct *tsk)
{
	struct remote_context *rc = tsk->mm->remote;
	atomic_inc(&rc->count);

	return rc;
}

inline bool __put_task_remote(struct remote_context *rc)
{
	if (!atomic_dec_and_test(&rc->count)) return false;

	__lock_remote_contexts(rc->for_remote);
	BUG_ON(atomic_read(&rc->count));
	list_del(&rc->list);
	__unlock_remote_contexts(rc->for_remote);

	free_remote_context_pages(rc);
	kfree(rc);
	return true;
}

inline bool put_task_remote(struct task_struct *tsk)
{
	return __put_task_remote(tsk->mm->remote);
}

void free_remote_context(struct remote_context *rc)
{
	__put_task_remote(rc);
}

static struct remote_context *__alloc_remote_context(int nid, int tgid, bool remote)
{
	struct remote_context *rc = kmalloc(sizeof(*rc), GFP_KERNEL);
	int i;
	BUG_ON(!rc);

	INIT_LIST_HEAD(&rc->list);
	atomic_set(&rc->count, 1); /* Account for mm->remote in a near future */
	rc->mm = NULL;

	rc->tgid = tgid;
	rc->for_remote = remote;

	for (i = 0; i < FAULTS_HASH; i++) {
		INIT_HLIST_HEAD(&rc->faults[i]);
		spin_lock_init(&rc->faults_lock[i]);
	}

	INIT_LIST_HEAD(&rc->vmas);
	spin_lock_init(&rc->vmas_lock);

	rc->vma_worker_stop = false;

	rc->vma_worker = NULL;
	INIT_LIST_HEAD(&rc->vma_works);
	spin_lock_init(&rc->vma_works_lock);
	init_completion(&rc->vma_works_ready);

	rc->remote_thread_spawner = NULL;
	INIT_LIST_HEAD(&rc->spawn_requests);
	spin_lock_init(&rc->spawn_requests_lock);
	init_completion(&rc->spawn_pended);

	memset(rc->remote_tgids, 0x00, sizeof(rc->remote_tgids));

	INIT_RADIX_TREE(&rc->pages, GFP_ATOMIC);

	barrier();

	return rc;
}

static void __build_task_comm(char *buffer, char *path)
{
	int i, ch;
	for (i = 0; (ch = *(path++)) != '\0';) {
		if (ch == '/')
			i = 0;
		else if (i < (TASK_COMM_LEN - 1))
			buffer[i++] = ch;
	}
	buffer[i] = '\0';
}


///////////////////////////////////////////////////////////////////////////////
// Distributed mutex
///////////////////////////////////////////////////////////////////////////////
long process_server_do_futex_at_remote(u32 __user *uaddr, int op, u32 val,
		bool valid_ts, struct timespec *ts,
		u32 __user *uaddr2,u32 val2, u32 val3)
{
	struct wait_station *ws = get_wait_station(current);
	remote_futex_request req = {
		.header = {
			.type = PCN_KMSG_TYPE_FUTEX_REQUEST,
			.prio = PCN_KMSG_PRIO_NORMAL,
		},
		.origin_pid = current->origin_pid,
		.remote_ws = ws->id,
		.op = op,
		.val = val,
		.ts = {
			.tv_sec = -1,
		},
		.uaddr = uaddr,
		.uaddr2 = uaddr2,
		.val2 = val2,
		.val3 = val3,
	};
	remote_futex_response *res;
	long ret;

	if (valid_ts) {
		req.ts = *ts;
	}

	/*
	printk(" f[%d] ->[%d/%d] 0x%x %p 0x%x\n", current->pid,
			current->origin_pid, current->origin_nid,
			op, uaddr, val);
	*/
	pcn_kmsg_send(current->origin_nid, &req, sizeof(req));
	res = wait_at_station(ws);
	ret = res->ret;
	/*
	printk(" f[%d] <-[%d/%d] 0x%x %p %ld\n", current->pid,
			current->origin_pid, current->origin_nid,
			op, uaddr, ret);
	*/

	put_wait_station(ws);
	pcn_kmsg_free_msg(res);
	return ret;
}

static int handle_remote_futex_response(struct pcn_kmsg_message *msg)
{
	remote_futex_response *res = (remote_futex_response *)msg;
	struct wait_station *ws = wait_station(res->remote_ws);

	ws->private = res;
	complete(&ws->pendings);
	return 0;
}

static void process_remote_futex_request(remote_futex_request *req)
{
	remote_futex_response res = {
		.header = {
			.type = PCN_KMSG_TYPE_FUTEX_RESPONSE,
			.prio = PCN_KMSG_PRIO_NORMAL,
		},
		.remote_ws = req->remote_ws,
	};
	ktime_t t, *tp = NULL;

	if (timespec_valid(&req->ts)) {
		t = timespec_to_ktime(req->ts);
		t = ktime_add_safe(ktime_get(), t);
		tp = &t;
	}

	/*
	printk(" f[%d] <-[%d/%d] 0x%x %p 0x%x\n", current->pid,
			current->remote_pid, current->remote_nid,
			req->op, req->uaddr, req->val);
	*/
	res.ret = do_futex(req->uaddr, req->op, req->val,
			tp, req->uaddr2, req->val2, req->val3);
	/*
	printk(" f[%d] ->[%d/%d] 0x%x %p %ld\n", current->pid,
			current->remote_pid, current->remote_nid,
			req->op, req->uaddr, res.ret);
	*/

	pcn_kmsg_send(current->remote_nid, &res, sizeof(res));
	pcn_kmsg_free_msg(req);
}


///////////////////////////////////////////////////////////////////////////////
// Handle process/task exit
///////////////////////////////////////////////////////////////////////////////
static void __terminate_peers(struct remote_context *rc)
{
	int nid;
	origin_task_exit_t req = {
		.header.type = PCN_KMSG_TYPE_TASK_EXIT_ORIGIN,
		.header.prio = PCN_KMSG_PRIO_NORMAL,

		.origin_pid = current->pid,
		.exit_code = current->exit_code,
	};

	/* Take down peer vma workers */
	for (nid = 0; nid < MAX_POPCORN_NODES; nid++) {
		if (nid == my_nid || rc->remote_tgids[nid] == 0) continue;
		PSPRINTK("TERMINATE [%d/%d] with 0x%d\n",
				rc->remote_tgids[nid], nid, req.exit_code);

		req.remote_pid = rc->remote_tgids[nid];
		pcn_kmsg_send(nid, &req, sizeof(req));
	}
}

static int __exit_origin_task(struct task_struct *tsk)
{
	struct remote_context *rc = tsk->mm->remote;

	if (tsk->remote) {
		put_task_remote(tsk);
	}
	tsk->remote = NULL;
	tsk->origin_nid = tsk->origin_pid = -1;

	/**
	 * Trigger peer termination if this is the last user thread
	 * referring to this mm.
	 */
	if (atomic_read(&tsk->mm->mm_users) == 1) {
		__terminate_peers(rc);
	}

	return 0;
}

static int __exit_remote_task(struct task_struct *tsk)
{
	if (tsk->exit_code == TASK_PARKED) {
		/* Skip notifying for back-migrated threads */
	} else {
		/* Something went south. Notify the origin. */
		if (!get_task_remote(tsk)->vma_worker_stop) {
			remote_task_exit_t req = {
				.header.type = PCN_KMSG_TYPE_TASK_EXIT_REMOTE,
				.header.prio = PCN_KMSG_PRIO_NORMAL,

				.origin_pid = tsk->origin_pid,
				.remote_pid = tsk->pid,
				.exit_code = tsk->exit_code,
			};
			pcn_kmsg_send(tsk->origin_nid, &req, sizeof(req));
		}
		put_task_remote(tsk);
	}

	put_task_remote(tsk);
	tsk->remote = NULL;
	tsk->origin_nid = tsk->origin_pid = -1;

	return 0;
}

int process_server_task_exit(struct task_struct *tsk)
{
	WARN_ON(tsk != current);

	if (!distributed_process(tsk)) return -ESRCH;

	PSPRINTK("EXITED [%d] %s%s / 0x%x\n", tsk->pid,
			tsk->at_remote ? "remote" : "local",
			tsk->is_vma_worker ? " worker": "",
			tsk->exit_code);

	// show_regs(task_pt_regs(tsk));

	if (tsk->is_vma_worker) return 0;

	if (tsk->at_remote) {
		return __exit_remote_task(tsk);
	} else {
		return __exit_origin_task(tsk);
	}
}


/**
 * Handle the notification of the task kill at the remote.
 */
static void process_remote_task_exit(remote_task_exit_t *req)
{
	struct task_struct *tsk = current;
	int exit_code = req->exit_code;

	if (tsk->remote_pid != req->remote_pid) {
		printk(KERN_INFO"%s: pid mismatch %d != %d\n", __func__,
				tsk->remote_pid, req->remote_pid);
		pcn_kmsg_free_msg(req);
		return;
	}

	PSPRINTK("%s [%d] 0x%x\n", __func__, tsk->pid, req->exit_code);

	tsk->remote = NULL;
	tsk->remote_nid = -1;
	tsk->remote_pid = -1;
	put_task_remote(tsk);

	exit_code = req->exit_code;
	pcn_kmsg_free_msg(req);

	if (exit_code & 0xff) {
		force_sig(exit_code & 0xff, tsk);
	}
	do_exit(exit_code);
}

static int handle_origin_task_exit(struct pcn_kmsg_message *msg)
{
	origin_task_exit_t *req = (origin_task_exit_t *)msg;
	struct task_struct *tsk;
	struct remote_context *rc;

	tsk = __get_task_struct(req->remote_pid);
	if (!tsk) {
		printk(KERN_INFO"%s: task %d not found\n", __func__, req->remote_pid);
		goto out;
	}
	PSPRINTK("\nTERMINATE [%d] with 0x%x\n", tsk->pid, req->exit_code);
	BUG_ON(!tsk->is_vma_worker);
	tsk->exit_code = req->exit_code;

	rc = get_task_remote(tsk);
	rc->vma_worker_stop = true;
	complete(&rc->vma_works_ready);
	complete(&rc->spawn_pended);

	__put_task_remote(rc);
	put_task_struct(tsk);
out:
	pcn_kmsg_free_msg(req);
	return 0;
}


///////////////////////////////////////////////////////////////////////////////
// handling back migration
///////////////////////////////////////////////////////////////////////////////
static void bring_back_remote_thread(back_migration_request_t *req)
{
	if (current->remote_pid != req->remote_pid) {
		printk(KERN_INFO"%s: pid mismatch during back migration (%d != %d)\n",
				__func__, current->remote_pid, req->remote_pid);
		goto out_free;
	}

	PRINTK("### BACKMIG [%d] from [%d/%d]\n",
			current->pid, req->remote_pid, req->remote_nid);

	/* Welcome home */

	current->remote = NULL;
	current->remote_nid = -1;
	current->remote_pid = -1;
	put_task_remote(current);

	current->personality = req->personality;

	/* XXX signals */

	restore_thread_info(&req->arch, false);

out_free:
	pcn_kmsg_free_msg(req);
}


/*
 * Send a message to <dst_nid> for migrating back a task <task>.
 * This is a back migration
 *  => <task> must already been migrated to <dst_nid>.
 * It returns -1 in error case.
 */
static int do_back_migration(struct task_struct *tsk, int dst_nid, void __user *uregs)
{
	back_migration_request_t *req;
	int ret;

	might_sleep();

	BUG_ON(tsk->origin_nid == -1 && tsk->origin_pid == -1);

	req = kmalloc(sizeof(*req), GFP_KERNEL);
	BUG_ON(!req);

	req->header.type = PCN_KMSG_TYPE_TASK_MIGRATE_BACK;
	req->header.prio = PCN_KMSG_PRIO_NORMAL;

	req->origin_pid = tsk->origin_pid;
	req->remote_nid = my_nid;
	req->remote_pid = tsk->pid;

	req->personality = tsk->personality;

	/*
	req->remote_blocked = tsk->blocked;
	req->remote_real_blocked = tsk->real_blocked;
	req->remote_saved_sigmask = tsk->saved_sigmask;
	req->remote_pending = tsk->pending;
	req->sas_ss_sp = tsk->sas_ss_sp;
	req->sas_ss_size = tsk->sas_ss_size;
	memcpy(req->action, tsk->sighand->action, sizeof(req->action));
	*/

	ret = copy_from_user(&req->arch.regsets, uregs,
			regset_size(get_popcorn_node_arch(dst_nid)));
	BUG_ON(ret != 0);

	save_thread_info(&req->arch);

	ret = pcn_kmsg_send(dst_nid, req, sizeof(*req));

	kfree(req);
	do_exit(TASK_PARKED);
}


///////////////////////////////////////////////////////////////////////////////
// Remote thread
///////////////////////////////////////////////////////////////////////////////
static int handle_remote_task_pairing(struct pcn_kmsg_message *msg)
{
	remote_task_pairing_t *req = (remote_task_pairing_t *)msg;
	struct task_struct *tsk;
	int ret = 0;

	tsk = __get_task_struct(req->your_pid);
	if (!tsk) {
		ret = -ESRCH;
		goto out;
	}
	BUG_ON(tsk->at_remote);
	BUG_ON(!tsk->remote);

	tsk->remote_nid = req->my_nid;
	tsk->remote_pid = req->my_pid;
	tsk->remote->remote_tgids[req->my_nid] = req->my_tgid;

	put_task_struct(tsk);
out:
	pcn_kmsg_free_msg(req);
	return 0;
}

static int __pair_remote_task(void)
{
	remote_task_pairing_t req = {
		.header = {
			.type = PCN_KMSG_TYPE_TASK_PAIRING,
			.prio = PCN_KMSG_PRIO_NORMAL,
		},
		.my_nid = my_nid,
		.my_tgid = current->tgid,
		.my_pid = current->pid,
		.your_pid = current->origin_pid,
	};
	return pcn_kmsg_send(current->origin_nid, &req, sizeof(req));
}


struct shadow_params {
	clone_request_t *req;
};

static int remote_thread_main(void *_args)
{
	struct shadow_params *params = _args;
	clone_request_t *req = params->req;

#ifdef CONFIG_POPCORN_DEBUG_VERBOSE
	PSPRINTK("%s [%d] started for [%d/%d]\n", __func__,
			current->pid, req->origin_pid, req->origin_nid);
#endif

	current->flags &= ~PF_KTHREAD;	/* Drop to user */
	current->origin_nid = req->origin_nid;
	current->origin_pid = req->origin_pid;
	current->at_remote = true;
	current->remote = get_task_remote(current);

	set_fs(USER_DS);

	/* Inject thread info here */
	restore_thread_info(&req->arch, true);

	/* XXX: Skip restoring signals and handlers for now
	sigorsets(&current->blocked, &current->blocked, &req->remote_blocked);
	sigorsets(&current->real_blocked,
			&current->real_blocked, &req->remote_real_blocked);
	sigorsets(&current->saved_sigmask,
			&current->saved_sigmask, &req->remote_saved_sigmask);
	current->pending = req->remote_pending;
	current->sas_ss_sp = req->sas_ss_sp;
	current->sas_ss_size = req->sas_ss_size;
	memcpy(current->sighand->action, req->action, sizeof(req->action));
	*/

	__pair_remote_task();

	PSPRINTK("\n####### MIGRATED - [%d/%d] from [%d/%d]\n",
			current->pid, my_nid, current->origin_pid, current->origin_nid);

	kfree(params);
	pcn_kmsg_free_msg(req);

	return 0;
	/* Returning from here makes this thread jump into the user-space */
}


static void __kick_remote_thread_spawner(struct remote_context *rc, struct pcn_kmsg_work *work)
{
	/* Exploit the list_head in work_struct */
	struct list_head *entry = &((struct work_struct *)work)->entry;

	INIT_LIST_HEAD(entry);
	spin_lock(&rc->spawn_requests_lock);
	list_add(entry, &rc->spawn_requests);
	spin_unlock(&rc->spawn_requests_lock);

	complete(&rc->spawn_pended);
}


int remote_thread_spawner(void *_args)
{
	struct remote_context *rc = get_task_remote(current);

	PSPRINTK("%s [%d] started\n", __func__, current->pid);

	current->is_vma_worker = true;
	rc->remote_thread_spawner = current;

	while (!rc->vma_worker_stop) {
		struct work_struct *work = NULL;
		struct shadow_params *params;

		if (!wait_for_completion_interruptible_timeout(&rc->spawn_pended, HZ))
			continue;

		spin_lock(&rc->spawn_requests_lock);
		if (!list_empty(&rc->spawn_requests)) {
			work = list_first_entry(
					&rc->spawn_requests, struct work_struct, entry);
			list_del(&work->entry);
		}
		spin_unlock(&rc->spawn_requests_lock);

		if (!work) continue;

		params = kmalloc(sizeof(*params), GFP_KERNEL);
		params->req = ((struct pcn_kmsg_work *)work)->msg;

		/* Following loop deals with signals between concurrent migration */
		while (kernel_thread(remote_thread_main, params,
						CLONE_THREAD | CLONE_SIGHAND | SIGCHLD) < 0) {
			schedule();
		}
		kfree(work);
	}

	PSPRINTK("%s [%d] exiting\n", __func__, current->pid);

	put_task_remote(current);
	do_exit(0);
}


static int __construct_mm(clone_request_t *req, struct remote_context *rc)
{
	struct mm_struct *mm;
	struct file *f;

	mm = mm_alloc();
	if (!mm) {
		BUG_ON(!mm);
		return -ENOMEM;
	}

	arch_pick_mmap_layout(mm);

	f = filp_open(req->exe_path, O_RDONLY | O_LARGEFILE | O_EXCL, 0);
	if (IS_ERR(f)) {
		PCNPRINTK_ERR("cannot open executable from %s\n", req->exe_path);
		return -EINVAL;
	}
	set_mm_exe_file(mm, f);
	filp_close(f, NULL);

	mm->task_size = req->task_size;
	mm->start_stack = req->stack_start;
	mm->start_brk = req->start_brk;
	mm->brk = req->brk;
	mm->env_start = req->env_start;
	mm->env_end = req->env_end;
	mm->arg_start = req->arg_start;
	mm->arg_end = req->arg_end;
	mm->start_code = req->start_code;
	mm->end_code = req->end_code;
	mm->start_data = req->start_data;
	mm->end_data = req->end_data;
	mm->def_flags = req->def_flags;

	use_mm(mm);

	rc->mm = mm;  /* No need to increase mm_users due to mm_alloc() */
	mm->remote = rc;

	return 0;
}


struct vma_worker_params {
	struct pcn_kmsg_work *work;
	struct remote_context *rc;
	char comm[TASK_COMM_LEN];
};

static void __terminate_remote_threads(struct remote_context *rc)
{
	struct task_struct *tsk;

	/* Terminate userspace threads. Tried to use do_group_exit() but it
	 * didn't work */
	rcu_read_lock();
	for_each_thread(current, tsk) {
		if (tsk->is_vma_worker) continue;
		force_sig(SIGKILL, tsk);
		break;
	}
	rcu_read_unlock();
}

static int start_vma_worker_remote(void *_data)
{
	struct vma_worker_params *params = (struct vma_worker_params *)_data;
	struct pcn_kmsg_work *work = params->work;
	clone_request_t *req = work->msg;
	struct remote_context *rc = params->rc;
	struct cred *new;

	might_sleep();
	kfree(params);

	PSPRINTK("%s: [%d] for [%d/%d]\n", __func__,
			current->pid, req->origin_tgid, req->origin_nid);
	PSPRINTK("%s: [%d] %s\n", __func__,
			current->pid, req->exe_path);

	current->flags &= ~PF_RANDOMIZE;	/* Disable ASLR for now*/
	current->personality = req->personality;
	current->is_vma_worker = true;
	current->at_remote = true;
	current->origin_nid = req->origin_nid;
	current->origin_pid = req->origin_pid;

	set_user_nice(current, 0);
	new = prepare_kernel_cred(current);
	commit_creds(new);

	if (__construct_mm(req, rc) != 0) {
		BUG();
		return -EINVAL;
	}

	get_task_remote(current);
	rc->tgid = current->tgid;
	smp_mb();

	/* Create the shadow spawner */
	kernel_thread(remote_thread_spawner, rc, CLONE_THREAD | CLONE_SIGHAND | SIGCHLD);

	/*
	 * Drop to user here to access mm using get_task_mm() in vma_worker routine.
	 * This should be done after forking remote_thread_spawner otherwise
	 * kernel_thread() will consider this as a user thread fork() which
	 * will end up an inproper instruction pointer (see copy_thread_tls()).
	 */
	current->flags &= ~PF_KTHREAD;

	vma_worker_remote(rc);
	__terminate_remote_threads(rc);

	put_task_remote(current);
	return 0;
}


static void clone_remote_thread(struct work_struct *_work)
{
	struct pcn_kmsg_work *work = (struct pcn_kmsg_work *)_work;
	clone_request_t *req = work->msg;
	int nid_from = req->origin_nid;
	int tgid_from = req->origin_tgid;
	struct remote_context *rc;
	struct remote_context *rc_new =
			__alloc_remote_context(nid_from, tgid_from, true);

	BUG_ON(!rc_new);

	__lock_remote_contexts_in(nid_from);
	rc = __lookup_remote_contexts_in(nid_from, tgid_from);
	if (!rc) {
		struct vma_worker_params *params;

		rc = rc_new;
		rc->remote_tgids[nid_from] = tgid_from;
		list_add(&rc->list, &__remote_contexts_in());
		__unlock_remote_contexts_in(nid_from);

		params = kmalloc(sizeof(*params), GFP_KERNEL);
		BUG_ON(!params);

		params->rc = rc;
		params->work = work;
		__build_task_comm(params->comm, req->exe_path);
		smp_mb();

		rc->vma_worker =
				kthread_run(start_vma_worker_remote, params, params->comm);
	} else {
		__unlock_remote_contexts_in(nid_from);
		kfree(rc_new);
	}

	/* Kick the spawner */
	__kick_remote_thread_spawner(rc, work);
	return;
}

static int handle_clone_request(struct pcn_kmsg_message *msg)
{
	clone_request_t *req = (clone_request_t *)msg;
	struct pcn_kmsg_work *work = kmalloc(sizeof(*work), GFP_ATOMIC);
	BUG_ON(!work);

	work->msg = req;
	INIT_WORK((struct work_struct *)work, clone_remote_thread);
	queue_work(popcorn_wq, (struct work_struct *)work);

	return 0;
}


///////////////////////////////////////////////////////////////////////////////
// Handle remote works at the origin
///////////////////////////////////////////////////////////////////////////////
int request_remote_work(pid_t pid, struct pcn_kmsg_message *req)
{
	struct task_struct *tsk = __get_task_struct(pid);
	if (!tsk) {
		printk(KERN_INFO"%s: invalid origin task %d for remote work %d\n",
				__func__, pid, req->header.type);
		pcn_kmsg_free_msg(req);
		return -ESRCH;
	}

	if (tsk->at_remote) {
		struct remote_context *rc = get_task_remote(tsk);
		struct pcn_kmsg_work *work = kmalloc(sizeof(*work), GFP_ATOMIC);
		struct list_head *entry = &((struct work_struct *)work)->entry;

		BUG_ON(!tsk->is_vma_worker);

		work->msg = req;
		INIT_LIST_HEAD(entry);
		spin_lock(&rc->vma_works_lock);
		list_add(entry, &rc->vma_works);
		spin_unlock(&rc->vma_works_lock);
		complete(&rc->vma_works_ready);

		put_task_remote(tsk);
	} else {
		BUG_ON(tsk->remote_work);
		tsk->remote_work = req;
		complete(&tsk->remote_work_pended);
	}

	put_task_struct(tsk);
	return 0;
}

static int __process_remote_works(void)
{
	bool run = true;
	BUG_ON(current->at_remote);

	while (run) {
		struct pcn_kmsg_message *req;
		long ret;
		ret = wait_for_completion_interruptible_timeout(
				&current->remote_work_pended, HZ);
		if (ret == 0) continue; /* timeout */
		if (ret == -ERESTARTSYS) break;

		req = (struct pcn_kmsg_message *)current->remote_work;
		current->remote_work = NULL;
		smp_wmb();

		switch (req->header.type) {
		case PCN_KMSG_TYPE_REMOTE_PAGE_REQUEST:
			WARN_ON_ONCE("Not implemented yet!");
			break;
		case PCN_KMSG_TYPE_VMA_OP_REQUEST:
			process_vma_op_request((vma_op_request_t *)req);
			break;
		case PCN_KMSG_TYPE_VMA_INFO_REQUEST:
			process_vma_info_request((vma_info_request_t *)req);
			break;
		case PCN_KMSG_TYPE_FUTEX_REQUEST:
			process_remote_futex_request((remote_futex_request *)req);
			break;
		case PCN_KMSG_TYPE_TASK_EXIT_REMOTE:
			process_remote_task_exit((remote_task_exit_t *)req);
			run = false;
			break;
		case PCN_KMSG_TYPE_TASK_MIGRATE_BACK:
			bring_back_remote_thread((back_migration_request_t *)req);
			run = false;
			break;
		default:
			if (WARN_ON("Receive unsupported message")) {
				printk("  Type: %d\n", req->header.type);
			}
		}
	}
	return 0;
}


/**
 * Send a message to <dst_nid> for migrating a task <task>.
 * This function will ask the remote node to create a thread to host the task.
 * It returns <0 in error case.
 */
static int __request_clone_remote(int dst_nid, struct task_struct *tsk, void __user *uregs)
{
	struct mm_struct *mm = get_task_mm(tsk);
	clone_request_t *req;
	int ret;

	might_sleep();

	req = kmalloc(sizeof(*req), GFP_KERNEL);
	BUG_ON(!req);

	/* Build request */
	req->header.type = PCN_KMSG_TYPE_TASK_MIGRATE;
	req->header.prio = PCN_KMSG_PRIO_NORMAL;

	/* struct mm_struct */
	if (get_file_path(mm->exe_file, req->exe_path, sizeof(req->exe_path))) {
		printk("%s: cannot get path to exe binary\n", __func__);
		ret = -ESRCH;
		goto out;
	}

	req->task_size = mm->task_size;
	req->stack_start = mm->start_stack;
	req->start_brk = mm->start_brk;
	req->brk = mm->brk;
	req->env_start = mm->env_start;
	req->env_end = mm->env_end;
	req->arg_start = mm->arg_start;
	req->arg_end = mm->arg_end;
	req->start_code = mm->start_code;
	req->end_code = mm->end_code;
	req->start_data = mm->start_data;
	req->end_data = mm->end_data;
	req->def_flags = mm->def_flags;

	/* struct tsk_struct */
	req->origin_nid = my_nid;
	req->origin_tgid = tsk->tgid;
	req->origin_pid = tsk->pid;

	req->personality = tsk->personality;

	/* Signals and handlers
	req->remote_blocked = tsk->blocked;
	req->remote_real_blocked = tsk->real_blocked;
	req->remote_saved_sigmask = tsk->saved_sigmask;
	req->remote_pending = tsk->pending;
	req->sas_ss_sp = tsk->sas_ss_sp;
	req->sas_ss_size = tsk->sas_ss_size;
	memcpy(req->action, tsk->sighand->action, sizeof(req->action));
	*/

	/* Register sets from userspace */
	ret = copy_from_user(&req->arch.regsets, uregs,
			regset_size(get_popcorn_node_arch(dst_nid)));
	BUG_ON(ret != 0);

	save_thread_info(&req->arch);

	ret = pcn_kmsg_send(dst_nid, req, sizeof(*req));

out:
	kfree(req);
	mmput(mm);

	return ret;
}

int do_migration(struct task_struct *tsk, int dst_nid, void __user *uregs)
{
	int ret;
	struct remote_context *rc;

	might_sleep();

	/* Won't to allocate this object in a spinlock-ed area */
	rc = __alloc_remote_context(my_nid, tsk->tgid, false);

	if (!cmpxchg(&tsk->mm->remote, 0, rc) == 0) {
		kfree(rc);
	} else {
		/*
		 * This process is becoming a distributed one if it was not yet.
		 * The first thread gets migrated attaches the remote context to
		 * mm->remote, which indicates some threads in this process is
		 * distributed.
		 */
		rc->mm = tsk->mm;
		rc->remote_tgids[my_nid] = tsk->tgid;

		__lock_remote_contexts_out(dst_nid);
		list_add(&rc->list, &__remote_contexts_out());
		__unlock_remote_contexts_out(dst_nid);
	}
	/*
	 * tsk->remote != NULL implies this task is a distributed one.
	 */
	tsk->remote = get_task_remote(tsk);
	tsk->at_remote = false;

	ret = __request_clone_remote(dst_nid, tsk, uregs);
	if (ret < 0) return ret;
	return __process_remote_works();
}


/**
 * Migrate the specified task <task> to node <dst_nid>
 * Currently, this function will put the specified task to sleep,
 * and push its info over to the remote node.
 * The remote node will then create a new thread and import that
 * info into its new context.
 */
int process_server_do_migration(struct task_struct *tsk, unsigned int dst_nid, void __user *uregs)
{
	int ret = 0;

	if (tsk->origin_nid == dst_nid) {
		ret = do_back_migration(tsk, dst_nid, uregs);
	} else {
		ret = do_migration(tsk, dst_nid, uregs);
		if (ret < 0) {
			tsk->remote = NULL;
			tsk->remote_pid = tsk->remote_nid = -1;
			put_task_remote(tsk);
		}
	}

	return ret;
}


DEFINE_KMSG_RW_HANDLER(remote_task_exit, remote_task_exit_t, origin_pid);
DEFINE_KMSG_RW_HANDLER(back_migration, back_migration_request_t, origin_pid);
DEFINE_KMSG_RW_HANDLER(remote_futex_request, remote_futex_request, origin_pid);

/**
 * Initialize the process server.
 */
int __init process_server_init(void)
{
	INIT_LIST_HEAD(&remote_contexts[0]);
	INIT_LIST_HEAD(&remote_contexts[1]);

	spin_lock_init(&remote_contexts_lock[0]);
	spin_lock_init(&remote_contexts_lock[1]);

	/* Register handlers */
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_TASK_MIGRATE, clone_request);
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_TASK_MIGRATE_BACK, back_migration);
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_TASK_PAIRING, remote_task_pairing);

	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_TASK_EXIT_REMOTE, remote_task_exit);
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_TASK_EXIT_ORIGIN, origin_task_exit);

	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_FUTEX_REQUEST, remote_futex_request);
	REGISTER_KMSG_HANDLER(PCN_KMSG_TYPE_FUTEX_RESPONSE, remote_futex_response);

	return 0;
}
