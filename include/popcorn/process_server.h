#ifndef __POPCORN_PROCESS_SERVER_H
#define __POPCORN_PROCESS_SERVER_H

struct task_struct;

int process_server_do_migration(struct task_struct* tsk, unsigned int dst_nid, void __user *uregs);
int process_server_task_exit(struct task_struct *tsk);
int update_frame_pointer(void);

long process_server_do_futex_at_remote(u32 __user *uaddr, int op, u32 val,
		bool valid_ts, struct timespec *ts,
		u32 __user *uaddr2, u32 val2, u32 val3);

#endif /* __POPCORN_PROCESS_SERVER_H */
