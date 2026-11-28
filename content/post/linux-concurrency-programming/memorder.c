#include <stdatomic.h>

_Atomic long g;

void st_relaxed(void) { atomic_store_explicit(&g, 1, memory_order_relaxed); }
void st_release(void) { atomic_store_explicit(&g, 1, memory_order_release); }
void st_seq(void)     { atomic_store_explicit(&g, 1, memory_order_seq_cst); }
long ld_relaxed(void) { return atomic_load_explicit(&g, memory_order_relaxed); }
long ld_acquire(void) { return atomic_load_explicit(&g, memory_order_acquire); }
long ld_seq(void)     { return atomic_load_explicit(&g, memory_order_seq_cst); }
void rmw_seq(void)    { atomic_fetch_add_explicit(&g, 1, memory_order_seq_cst); }
long xchg_acqrel(void) { return atomic_exchange_explicit(&g, 1, memory_order_acq_rel); }

void pub_relaxed(_Atomic long *d, _Atomic long *f)
{
	atomic_store_explicit(d, 1, memory_order_relaxed);
	atomic_store_explicit(f, 1, memory_order_relaxed);
}

void pub_release(_Atomic long *d, _Atomic long *f)
{
	atomic_store_explicit(d, 1, memory_order_relaxed);
	atomic_store_explicit(f, 1, memory_order_release);
}

void pub_seq(_Atomic long *d, _Atomic long *f)
{
	atomic_store_explicit(d, 1, memory_order_seq_cst);
	atomic_store_explicit(f, 1, memory_order_seq_cst);
}
