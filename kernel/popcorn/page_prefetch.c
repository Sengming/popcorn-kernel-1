/*
 * Popcorn page prefetching machanism implementation
 */
#include <linux/compiler.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/rmap.h>
#include <linux/mmu_notifier.h>
#include <linux/wait.h>
#include <linux/ptrace.h>
#include <linux/swap.h>
#include <linux/pagemap.h>
#include <linux/delay.h>
#include <linux/random.h>
#include <linux/radix-tree.h>

#include <asm/tlbflush.h>
#include <asm/cacheflush.h>
#include <asm/mmu_context.h>

#define PFPRINTK(...) printk(KERN_INFO __VA_ARGS__)
///////////////////////////// TODO test above
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/memcontrol.h>

#include "types.h"
#include "pgtable.h"
#include "wait_station.h"
#include "page_server.h"
#include "fh_action.h"

#include "page_prefetch.h"

#include <popcorn/debug.h>
//#include <popcorn/types.h>
//#include <popcorn/page_server.h>

#define PREFETCH_FAIL 0x0001
#define PREFETCH_SUCCESS 0x0002
#define PREFETCH_CONCURRENCY 0x0004

struct fault_handle {
    struct hlist_node list;

    unsigned long addr;
    unsigned long flags;

    unsigned int limit;
    pid_t pid;
    int ret;

    atomic_t pendings;
    atomic_t pendings_retry;
    wait_queue_head_t waits;
    wait_queue_head_t waits_retry;
    struct remote_context *rc;

    struct completion *complete;
};

static inline int __fault_hash_key(unsigned long address)
{
    return (address >> PAGE_SHIFT) % FAULTS_HASH;
}

#define PER_PAGE_INFO_SIZE \
        (sizeof(unsigned long) * BITS_TO_LONGS(MAX_POPCORN_NODES))
#define PAGE_INFO_PER_REGION (PAGE_SIZE / PER_PAGE_INFO_SIZE)
static inline void __get_page_info_key(unsigned long addr, unsigned long *key, unsigned long *offset)
{
    unsigned long paddr = addr >> PAGE_SHIFT;
    *key = paddr / PAGE_INFO_PER_REGION;
    *offset = (paddr % PAGE_INFO_PER_REGION) *
            (PER_PAGE_INFO_SIZE / sizeof(unsigned long));
}

static inline unsigned long *__get_page_info(struct mm_struct *mm, unsigned long addr)
{
    unsigned long key, offset;
    unsigned long *region;
    struct remote_context *rc = mm->remote;
    __get_page_info_key(addr, &key, &offset);

    region = radix_tree_lookup(&rc->pages, key);
    if (!region) return NULL;

    return region + offset;
}

static inline void set_page_owner(int nid, struct mm_struct *mm, unsigned long addr)
{
    unsigned long *pi = __get_page_info(mm, addr);
    set_bit(nid, pi);
}

#define PI_FLAG_DISTRIBUTED 63
static inline bool page_is_mine(struct mm_struct *mm, unsigned long addr)
{
    unsigned long *pi = __get_page_info(mm, addr);

    if (!pi || !test_bit(PI_FLAG_DISTRIBUTED, pi)) return true;
    return test_bit(my_nid, pi);
}

static pte_t *__get_pte_at(struct mm_struct *mm, unsigned long addr, pmd_t **ppmd, spinlock_t **ptlp)
{
    pgd_t *pgd;
    pud_t *pud;
    pmd_t *pmd;

    pgd = pgd_offset(mm, addr);
    if (!pgd || pgd_none(*pgd)) return NULL;

    pud = pud_offset(pgd, addr);
    if (!pud || pud_none(*pud)) return NULL;

    pmd = pmd_offset(pud, addr);
    if (!pmd || pmd_none(*pmd)) return NULL;

    *ppmd = pmd;
    *ptlp = pte_lockptr(mm, pmd);

    return pte_offset_map(pmd, addr);
}

static struct kmem_cache *__fault_handle_cache = NULL;
static struct fault_handle *__alloc_fault_handle(struct task_struct *tsk, unsigned long addr)
{
    struct fault_handle *fh =
            kmem_cache_alloc(__fault_handle_cache, GFP_ATOMIC);
    int fk = __fault_hash_key(addr);
    BUG_ON(!fh);

    INIT_HLIST_NODE(&fh->list);

    fh->addr = addr;
    fh->flags = 0;

    init_waitqueue_head(&fh->waits);
    init_waitqueue_head(&fh->waits_retry);
    atomic_set(&fh->pendings, 1);
    atomic_set(&fh->pendings_retry, 0);
    fh->limit = 0;
    fh->ret = 0;
    fh->rc = get_task_remote(tsk);
    fh->pid = tsk->pid;
    fh->complete = NULL;

    hlist_add_head(&fh->list, &fh->rc->faults[fk]);
    return fh;
}

static bool __finish_fault_handling(struct fault_handle *fh)
{
    unsigned long flags;
    bool last = false;
    int fk = __fault_hash_key(fh->addr);

    spin_lock_irqsave(&fh->rc->faults_lock[fk], flags);
    if (atomic_dec_return(&fh->pendings)) {
        PGPRINTK(" >[%d] %lx %p\n", fh->pid, fh->addr, fh);
#ifndef CONFIG_POPCORN_DEBUG_PAGE_SERVER
        wake_up_all(&fh->waits);
#else
        wake_up(&fh->waits);
#endif
    } else {
        PGPRINTK(">>[%d] %lx %p\n", fh->pid, fh->addr, fh);
        if (fh->complete) {
            complete(fh->complete);
        } else {
            hlist_del(&fh->list);
            last = true;
        }
    }
    spin_unlock_irqrestore(&fh->rc->faults_lock[fk], flags);

    if (last) {
        __put_task_remote(fh->rc);
        if (atomic_read(&fh->pendings_retry)) {
            wake_up_all(&fh->waits_retry);
        } else {
            kmem_cache_free(__fault_handle_cache, fh);
        }
    }
    return last;
}

inline struct prefetch_list *alloc_prefetch_list(void)
{
	return kzalloc(sizeof(struct prefetch_list), GFP_KERNEL);
}

inline void free_prefetch_list(struct prefetch_list* pf_list)
{
	if (pf_list) kfree(pf_list);
}

inline void add_pf_list_at(struct prefetch_list* pf_list,
							unsigned long addr, 
							struct fault_handle* fh,
							int slot_num)
{
	struct prefetch_body *list_ptr = (struct prefetch_body*)pf_list;
	(list_ptr + slot_num)->addr = addr;
	(list_ptr + slot_num)->fh = fh;
}


/*
 * Decide prefetched pages
 */
#define SKIP_NUM_OF_PAGES 1	// 0 = myself
#define PREFETCH_NUM_OF_PAGES 10
#define PREFETCH_DURATION 1
void prefetch_policy(struct prefetch_list* pf_list, unsigned long fault_addr)
{
	static uint8_t cnt = 0;
	struct prefetch_body *list_ptr;
	cnt++;
	if (cnt >= PREFETCH_DURATION) {
		int i = 0;
		list_ptr = (struct prefetch_body*)pf_list;
		for(i = 0; i < PREFETCH_NUM_OF_PAGES; i++) {
			list_ptr->addr = fault_addr + ((i + SKIP_NUM_OF_PAGES) * PAGE_SIZE);
			list_ptr++;
		}
		cnt = 0;
    }
}

/*
 * Select prefetched pages
 * 		peek existing preftech_list
 * 		return a new preftechlist
 */
struct prefetch_list *select_prefetch_pages(
        struct prefetch_list* pf_list, struct mm_struct *mm)
{
    int slot = 0;
    struct prefetch_list *new_pf_list = NULL;
    struct prefetch_body *list_ptr = (struct prefetch_body*)pf_list;
	
	//PFPRINTK("%s(): 0 %lx\n", __func__, list_ptr->addr);
    if (!(list_ptr->addr)) goto out;

    new_pf_list = alloc_prefetch_list();
    while (list_ptr->addr) {
		int fk;
		pte_t *pte;
		pmd_t *pmd;
		spinlock_t *ptl;
		bool found = false;
		unsigned long flags;
		struct fault_handle *fh;
        struct remote_context *rc;
        unsigned long addr = list_ptr->addr;
        struct vm_area_struct *vma = find_vma(mm, addr);
        if (!vma || vma->vm_start > addr) { /* ask this causes origin bug_on */
            PFPRINTK("local unselect: %lx no vma/out bound\n", addr);
			PFPRINTK("\n\n\n\n\n\n\n\n\n\t\t\t\t\t\tI wanna see this \t\t\t\t\t\n\n\n\n\n\n\n\n\n");
            list_ptr++;
            continue;
        }

        pte = __get_pte_at(mm, addr, &pmd, &ptl);
		if (!pte) {
			PFPRINTK("local unselect: %lx no pte\n", addr);
			list_ptr++;
			continue;
		}

        if(!spin_trylock(ptl)) {
			list_ptr++;
			pte_unmap(pte);
			PFPRINTK("local unselect: %lx pte locked\n", addr);
			continue;
		}

		rc = get_task_remote(current);
        fk = __fault_hash_key(addr);

		/* fault lock will stop next pte acess as well */
    	//spin_lock_irqsave(&rc->faults_lock[fk], flags); /* TODO - use try lock */
		if(!spin_trylock_irqsave(&rc->faults_lock[fk], flags)) {
			spin_unlock(ptl);
        	list_ptr++;
			pte_unmap(pte);
			PFPRINTK("local unselect: %lx fh locked\n", addr);
			continue;
		}
        spin_unlock(ptl);
		pte_unmap(pte);

		hlist_for_each_entry(fh, &rc->faults[fk], list) {
			if (fh->addr == addr) {
				found = true;
				break;
			}
		}

        if (!found && !page_is_mine(mm, addr)) { //leader
			// remotefault | at origin | read
			fh = __alloc_fault_handle(current, addr);
			add_pf_list_at(new_pf_list, addr, fh, slot);
			PFPRINTK("select: [%d] %lx [%d]\n", slot, addr, current->pid);
			slot++;
        } else { // follower
			/* TODO - leave? or be a follower, which requires leader to wail it up */
			//PFPRINTK("unselect %lx %d %d\n", addr, found, page_is_mine(mm, addr));
		}
        list_ptr++;
		spin_unlock_irqrestore(&rc->faults_lock[fk], flags);
    }
out:
    free_prefetch_list(pf_list);
	/* TODO - counter */
	if (!slot) {
		free_prefetch_list(new_pf_list);
		return 0;
	}
    return new_pf_list;
}

static unsigned long *__lookup_region(struct remote_context *rc, unsigned long key)
{
    unsigned long *region = radix_tree_lookup(&rc->pages, key);
    if (!region) {
        int ret;
        struct page *page = alloc_page(GFP_ATOMIC | __GFP_ZERO);
        BUG_ON(!page);
        set_page_private(page, key);

        region = kmap(page);
        ret = radix_tree_insert(&rc->pages, key, region);
        BUG_ON(ret);
    }
    return region;
}
static inline void SetPageDistributed(struct mm_struct *mm, unsigned long addr)
{
    unsigned long key, offset;
    unsigned long *region;
    struct remote_context *rc = mm->remote;
    __get_page_info_key(addr, &key, &offset);

    region = __lookup_region(rc, key);
    set_bit(PI_FLAG_DISTRIBUTED, region + offset);
}
static void __make_pte_valid(struct mm_struct *mm,
        struct vm_area_struct *vma, unsigned long addr,
        unsigned long fault_flags, pte_t *pte)
{
    pte_t entry;

    entry = ptep_clear_flush(vma, addr, pte);
    entry = pte_make_valid(entry);

    if (fault_for_write(fault_flags)) {
        entry = pte_mkwrite(entry);
        entry = pte_mkdirty(entry);
    } else {
        entry = pte_wrprotect(entry);
    }

    set_pte_at_notify(mm, addr, pte, entry);
    update_mmu_cache(vma, addr, pte);
    // flush_tlb_page(vma, addr);

    SetPageDistributed(mm, addr);
    set_page_owner(my_nid, mm, addr);
}
enum {
    FAULT_HANDLE_WRITE = 0x01,
    FAULT_HANDLE_INVALIDATE = 0x02,
    FAULT_HANDLE_REMOTE = 0x04,
};
#define TRANSFER_PAGE_WITH_RDMA \
        pcn_kmsg_has_features(PCN_KMSG_FEATURE_RDMA)
int prefetch_at_origin(remote_page_request_t *req)
{
	int from_nid = PCN_KMSG_FROM_NID(req);
	struct mm_struct *mm;
	struct task_struct *tsk;
    struct remote_context *rc;
	struct prefetch_body *list_ptr = (struct prefetch_body*)&req->pf_list;
	//if (!req->is_prefetch) return -1; //problem: msg size
	//PFPRINTK("%s(): 0 %lx\n", __func__, list_ptr->addr);

	if(!list_ptr->addr) return -1;
	
	tsk = __get_task_struct(req->remote_pid);
	if (!tsk) return -1;
	mm = get_task_mm(tsk);
	rc = get_task_remote(tsk);
	down_read(&mm->mmap_sem);

	while(list_ptr->addr) { //TODO - check list boundry (also check in other place)
		int fk;
		pmd_t *pmd;
		pte_t *pte;
		void *paddr;
		int res_size;
		spinlock_t *ptl;
		struct page *page;
		unsigned long flags;
		struct fault_handle *fh = NULL;
		remote_prefetch_response_t *res;
		bool found = false, leader = false;
        unsigned long addr = list_ptr->addr;
		struct vm_area_struct *vma = find_vma(mm, addr);

		if (TRANSFER_PAGE_WITH_RDMA) {
			res = pcn_kmsg_get(sizeof(remote_page_response_short_t));
		} else {
			res = pcn_kmsg_get(sizeof(*res));
		}

		//BUG_ON(!vma || vma->vm_start > addr);
		if(!vma || vma->vm_start > addr) {
			PFPRINTK("origin unselect %lx pte locked\n", addr);
			res->result = PREFETCH_FAIL;
			res_size = sizeof(remote_prefetch_fail_t);
			goto out;
		}
		
	
        pte = __get_pte_at(mm, addr, &pmd, &ptl);
        //spin_lock(ptl); // opt - best-effort
        if(!spin_trylock(ptl)) { // opt - relax
			PFPRINTK("origin unselect %lx pte locked\n", addr);
			res->result = PREFETCH_FAIL;
			res_size = sizeof(remote_prefetch_fail_t);
			goto out_post;
		}

        fk = __fault_hash_key(addr);

		/* fault lock will stop next pte acess as well */
        //spin_lock_irqsave(&rc->faults_lock[fk], flags); // opt - best-effort
		if(!spin_trylock_irqsave(&rc->faults_lock[fk], flags)) { // opt - relax
			spin_unlock(ptl);
			PFPRINTK("origin unselect %lx fh locked\n", addr);
			res->result = PREFETCH_FAIL;
			res_size = sizeof(remote_prefetch_fail_t);
			goto out_post;
		}
        spin_unlock(ptl);

		hlist_for_each_entry(fh, &rc->faults[fk], list) {
			if (fh->addr == addr) {
				found = true;
				break;
			}
		}

		if (found) {
			/* confliction - follwer case */
			res->result = PREFETCH_FAIL;
			res_size = sizeof(remote_prefetch_fail_t);
			/* choose1 A - TODO take care of followers */
			/* TODO */
			/* choose1 B - Stop execution(B) is more safe for now */
		} else if (!found && page_is_mine(mm, addr)) {
			/* no conflict and owner */
			leader = true;
			res->result = PREFETCH_SUCCESS;
			res_size = sizeof(remote_prefetch_response_t);

			/* remotefault | at remote | read */
			/* choose1 A - */
			//fh = __alloc_fault_handle(tsk, addr);
			//fh->flags |= FAULT_HANDLE_REMOTE;
			/* choose1 B - instatead of creating a fh addr,
									send msg out immediately */
			/* none for B */
        } else if (!found && !page_is_mine(mm, addr)){
			/* send a remote page request TODO: NOT supported yet */
			res->result = PREFETCH_FAIL;
			res_size = sizeof(remote_prefetch_fail_t);
		} else {
			BUG();
		}
		/* choose1 A - will not block other addresses */
		//spin_unlock_irqrestore(&rc->faults_lock[fk], flags);
			
		if (leader) {
			pte_t entry;
			spin_lock(ptl);
			SetPageDistributed(mm, addr);
			set_page_owner(from_nid, mm, addr);

			entry = ptep_clear_flush(vma, addr, pte);

			/* remotefault | read */
            entry = pte_make_valid(entry); /* For remote-claimed case */
            entry = pte_wrprotect(entry);
            set_page_owner(my_nid, mm, addr);

			set_pte_at_notify(mm, addr, pte, entry);
			update_mmu_cache(vma, addr, pte);
			spin_unlock(ptl);

			/* copy page to msg */
			page = get_normal_page(vma, addr, pte);
			flush_cache_page(vma, addr, page_to_pfn(page)); // ???
			paddr = kmap_atomic(page);
			copy_from_user_page(vma, page, addr, res->page, paddr, PAGE_SIZE);
			kunmap_atomic(paddr);
		}

		/* choose1 A - will not block other addresses */
		//__finish_fault_handling(fh); //TODO - check
		/* choose1 B - will not casue a resend for the same address */
		spin_unlock_irqrestore(&rc->faults_lock[fk], flags);
out_post:
		pte_unmap(pte);
out:
		PFPRINTK("handled pf:\t%lx %s\n", addr,
				res->result & PREFETCH_SUCCESS ? "(O)" : "(X)");
		/* msg */
		res->addr = addr;
		res->fh = list_ptr->fh;
		res->remote_pid = req->remote_pid;
		res->origin_pid = req->origin_pid;

		pcn_kmsg_post(PCN_KMSG_TYPE_REMOTE_PREFETCH_RESPONSE,
							PCN_KMSG_FROM_NID(req), res, res_size);

		list_ptr++;
    }

	up_read(&mm->mmap_sem);
	mmput(mm);
	put_task_struct(tsk);
	return 0;
}

/* prefetch response event handler */
static void process_remote_prefetch_response(struct work_struct *work)
{
	START_KMSG_WORK(remote_prefetch_response_t, res, work);
	//int ret = 0;
    struct page *page;
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    struct task_struct *tsk = __get_task_struct(res->origin_pid);
    if (!tsk) {
        PGPRINTK("%s: no such process %d %d pf_addr %lx\n", __func__,
                res->origin_pid, res->remote_pid, res->addr); // TODO - chekc original side sending info is correct?
        goto out;
    }

	mm = get_task_mm(tsk);
	//BUG_ON(page_is_mine(mm, res->addr)); /* if, concurrency problem */
	if(page_is_mine(mm, res->addr)) {
		res->result = PREFETCH_CONCURRENCY;
		goto out_free;
	}
	down_read(&mm->mmap_sem);
	vma = find_vma(mm, res->addr);
    BUG_ON(!vma || vma->vm_start > res->addr);

    /* get a page frame for the vma page if needed */
	if (res->result & PREFETCH_SUCCESS) {
		pte_t *pte;
		pmd_t *pmd;
        void *paddr;
		spinlock_t *ptl;
		bool populated = false;
		struct mem_cgroup *memcg;
		pte = __get_pte_at(mm, res->addr, &pmd, &ptl);
		if (!pte) {
			PGPRINTK("  [%d] No PTE!!\n", tsk->pid);
			BUG();
			//goto out_free_all;
			//ret = VM_FAULT_OOM;
		}

        if (pte_none(*pte) ||
            !(page = vm_normal_page(vma, res->addr, *pte))) {
            page = alloc_page_vma(GFP_HIGHUSER_MOVABLE, vma, res->addr);
            mem_cgroup_try_charge(page, mm, GFP_KERNEL, &memcg);
			populated = true;
        }
        get_page(page);

		/* load page - not support RDMA now */
		paddr = kmap(page);
		copy_to_user_page(vma, page, res->addr, paddr, res->page, PAGE_SIZE);
        kunmap(page);
        __SetPageUptodate(page);

		/* update pte */
		spin_lock(ptl);
        if (populated) {
            do_set_pte(vma, res->addr, page, pte, false, true);
            mem_cgroup_commit_charge(page, memcg, false);
            lru_cache_add_active_or_unevictable(page, vma);
        } else {
			unsigned fault_flags = 0; /* just for for read */
            __make_pte_valid(mm, vma, res->addr, fault_flags, pte);
        }
		spin_unlock(ptl);

		pte_unmap_unlock(pte, ptl);
		put_page(page);
    } else if (res->result & PREFETCH_FAIL) {
		;
	} else { // detour PREFETCH_CONCURRENCY
		/* optimication for granted page case?
			VM_FAULT_CONTINUE is consider a PREFETCH_FAIL */
		printk("%lx\n", res->result);
		BUG();
	}


//out_free_all:
	up_read(&mm->mmap_sem);
out_free:
	mmput(mm);
out:
	/* TODO - counter 3 state */
	PFPRINTK("recv:\t\t>%lx %s %p\n", res->addr,
				res->result&PREFETCH_SUCCESS?"(O)":"(X)", res->fh);
	if (res->fh)
		__finish_fault_handling(res->fh);
    END_KMSG_WORK(res);
	//return ret;
}

DEFINE_KMSG_WQ_HANDLER(remote_prefetch_response);
int __init page_prefetch_init(void)
{
    REGISTER_KMSG_WQ_HANDLER(
		PCN_KMSG_TYPE_REMOTE_PREFETCH_RESPONSE, remote_prefetch_response);

    __fault_handle_cache = kmem_cache_create("fault_handle",
	            sizeof(struct fault_handle), 0, 0, NULL);
    return 0;
}
