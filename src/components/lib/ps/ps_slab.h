/***
 * Copyright 2011-2015 by Gabriel Parmer.  All rights reserved.
 *
 * Redistribution of this file is permitted under the GNU General
 * Public License v2.
 *
 * Author: Gabriel Parmer, gparmer@gwu.edu, 2011
 *
 * History:
 * - Initial slab allocator, 2011
 * - Adapted for parsec, 2015
 */

#ifndef  PS_SLAB_H
#define  PS_SLAB_H

#include <ps_list.h>
#include <ps_plat.h>
#include <ps_global.h>

#ifndef PS_REMOTE_BATCH
/* Needs to be a power of 2 */
#define PS_REMOTE_BATCH 128
#endif

/* #define PS_SLAB_DEBUG 1 */

/* The header for a slab. */
struct ps_slab {
	/*
	 * Read-only data.  coreid is read by _other_ cores, so we
	 * want it on a separate cache-line from the frequently
	 * modified stuff.
	 */
	void  *memory;		/* != NULL iff slab is separately allocated */
	ps_desc_t start, end;	/* A slab used as a namespace: min and max descriptor ids */
	size_t memsz;		/* size of backing memory */
	u16_t  coreid;		/* which is the home core for this slab? */
	char   pad[PS_CACHE_LINE-(sizeof(void *)+sizeof(size_t)+sizeof(u16_t)+sizeof(ps_desc_t)*2)];

	/* Frequently modified data on the owning core... */
	struct ps_mheader *freelist; /* free objs in this slab */
	struct ps_list     list;     /* freelist of slabs */
	size_t             nfree;    /* # allocations in freelist */
} PS_PACKED;


/*** Operations on the freelist of slabs ***/

/*
 * These functions should really must be statically computed for
 * efficiency (see macros below)...
 */
static inline unsigned int
__ps_slab_objmemsz(size_t obj_sz)
{ return PS_RNDUP(obj_sz + sizeof(struct ps_mheader), PS_WORD); }
static inline unsigned int
__ps_slab_max_nobjs(size_t obj_sz, size_t allocsz, int hintern)
{ return (allocsz - (hintern ? sizeof(struct ps_slab) : 0)) / __ps_slab_objmemsz(obj_sz); }
/* The offset of the given object in its slab */
static inline unsigned int
__ps_slab_objsoff(struct ps_slab *s, struct mheader *h, size_t obj_sz, int hintern)
{ return ((unsigned long)h - ((unsigned long)s->memory + (hintern ? sizeof(struct ps_slab) : 0))) / __ps_slab_objmemsz(obj_sz); }

static void
__slab_freelist_rem(struct ps_slab_freelist *fl, struct ps_slab *s)
{
	assert(s && fl);
	if (fl->list == s) {
		if (ps_list_empty(s, list)) fl->list = NULL;
		else                        fl->list = ps_list_first(s, list);
	}
	ps_list_rem(s, list);
}

static void
__slab_freelist_add(struct ps_slab_freelist *fl, struct ps_slab *s)
{
	assert(s && fl);
	assert(ps_list_empty(s, list));
	assert(s != fl->list);
	if (fl->list) ps_list_add(fl->list, s, list);
	fl->list = s;
	/* TODO: sort based on emptiness...just use N bins */
}

/*** Alloc and free ***/

/* Create function prototypes for cross-object usage */
#define PS_SLAB_CREATE_PROTOS(name)				\
void  *ps_slab_alloc_##name(void);			        \
void   ps_slab_free_##name(void *buf);				\
size_t ps_slab_objmem_##name(void);                             \
size_t ps_slab_nobjs_##name(void);

/* For non-internal slab headers */
PS_SLAB_CREATE_PROTOS(slabhead)

void __ps_slab_mem_remote_free(struct ps_mem_percore *percpu, struct ps_mheader *h, u16_t core_target);
void __ps_slab_mem_remote_process(struct ps_mem_percore *percpu, size_t obj_sz, size_t allocsz, int hintern);
void __ps_slab_init(struct ps_slab *s, void *mem, struct ps_slab_info *si, size_t obj_sz, int allocsz, int hintern);

static inline void
__ps_slab_check_consistency(struct ps_slab *s)
{
#ifdef PS_SLAB_DEBUG
	struct ps_mheader *h;
	unsigned int i;

	assert(s);
	h = s->freelist;
	for (i = 0 ; h ; i++) {
		assert(h->slab == s);
		assert(h->tsc_free != 0);
		h = h->next;
	}
	assert(i == s->nfree);
#else
	(void)s;
#endif
}

static inline void
__ps_slab_mem_free(void *buf, struct ps_mem_percore *percpu, coreid_t curr, size_t obj_sz, size_t allocsz, int hintern)
{
	struct ps_slab *s;
	struct ps_mheader *h, *next;
	unsigned int max_nobjs = __ps_slab_max_nobjs(obj_sz, allocsz, hintern);
	struct ps_slab_freelist *fl;
	u16_t coreid;
	assert(__ps_slab_objmemsz(obj_sz) + (hintern ? sizeof(struct ps_slab) : 0) <= allocsz);

	h = __ps_mhead_get(buf);
	assert(!__ps_mhead_isfree(h)); /* freeing freed memory? */
	s = h->slab;
	assert(s);

	coreid = s->coreid;
	if (unlikely(coreid != curr)) {
		__ps_slab_mem_remote_free(percpu, h, coreid);
		return;
	}

	__ps_mhead_setfree(h, 1);
	next        = s->freelist;
	s->freelist = h; 	/* TODO: should be atomic/locked */
	h->next     = next;
	s->nfree++;		/* TODO: ditto */
	__ps_slab_check_consistency(s);

	if (s->nfree == max_nobjs) {
		/* remove from the freelist */
		fl = &percpu[coreid].slab_info.fl;
		__slab_freelist_rem(fl, s);
	 	PS_SLAB_FREE(s->memory, s->memsz);
		if (!hintern) ps_slab_free_slabhead(s);
	} else if (s->nfree == 1) {
		fl = &percpu[coreid].slab_info.fl;
		/* add back onto the freelists */
		assert(ps_list_empty(s, list));
		__slab_freelist_add(fl, s);
	}

	return;
}

static inline void *
__ps_slab_mem_alloc(struct ps_mem_percore *percpu, size_t obj_sz, u32_t allocsz, int hintern)
{
	struct ps_slab      *s;
	struct ps_mheader   *h;
	struct ps_slab_info *si = &percpu->slab_info;
	void                *mem;
	assert(obj_sz + (hintern ? sizeof(struct ps_slab) : 0) <= allocsz);


	si->salloccnt++;
	if (unlikely(si->salloccnt % PS_REMOTE_BATCH == 0)) {
		__ps_slab_mem_remote_process(percpu, obj_sz, allocsz, hintern);
	}

	s = si->fl.list;
	if (unlikely(!s)) {
		s = mem = PS_SLAB_ALLOC(allocsz);
		if (unlikely(!mem)) return NULL;

		if (!hintern) {
			s = ps_slab_alloc_slabhead();
			if (unlikely(!s)) {
				PS_SLAB_FREE(mem, allocsz);
				return NULL;
			}
		}

		__ps_slab_init(s, mem, si, obj_sz, allocsz, hintern);
	}

	/* TODO: atomic modification to the freelist */
	h           = s->freelist;
	s->freelist = h->next;
	h->next     = NULL;
	s->nfree--;
	__ps_mhead_reset(h);
	__ps_slab_check_consistency(s);

	/* remove from the freelist */
	if (s->nfree == 0) {
		__slab_freelist_rem(&si->fl, s);
		assert(ps_list_empty(s, list));
	}
	assert(!__ps_mhead_isfree(h));

	return __ps_mhead_mem(h);
}

/***
 * This macro is very important for high-performance.  It creates the
 * functions for allocation and deallocation passing in the freelist
 * directly, and size information for these objects, thus enabling the
 * compiler to do partial evaluation.  This avoids freelist lookups,
 * and relies on the compilers optimizations to generate specialized
 * code for the given sizes -- requires function inlining and constant
 * propagation.  Relying on these optimizations is better than putting
 * all of the code for allocation and deallocation in the macro due to
 * maintenance and readability.
 */
#define PS_SLAB_CREATE_FNS(name, size, allocsz, headintern)		       \
inline void *						                       \
ps_slab_alloc_##name(void)						       \
{									       \
        struct ps_mem_percore *fl = &__ps_slab_##name##_freelist[ps_coreid()]; \
	return __ps_slab_mem_alloc(fl, size, allocsz, headintern);             \
}									       \
inline void							               \
ps_slab_free_##name(void *buf)						       \
{									       \
        struct ps_mem_percore *fl = __ps_slab_##name##_freelist;	       \
	__ps_slab_mem_free(buf, fl, ps_coreid(), size, allocsz, headintern);   \
}									       \
inline void							               \
ps_slab_free_coreid_##name(void *buf, coreid_t curr)			       \
{									       \
        struct ps_mem_percore *fl = __ps_slab_##name##_freelist;	       \
	__ps_slab_mem_free(buf, fl, curr, size, allocsz, headintern);          \
}									       \
inline void *						                       \
ps_slabptr_alloc_##name(struct ps_mem *m)				       \
{									       \
        struct ps_mem_percore *fl = &(m->__ps_slab_freelist[ps_coreid()]);     \
	return __ps_slab_mem_alloc(fl, size, allocsz, headintern);             \
}									       \
inline void							               \
ps_slabptr_free_##name(struct ps_mem *m, void *buf)			       \
{									       \
        struct ps_mem_percore *fl = m->__ps_slab_freelist;		       \
	__ps_slab_mem_free(buf, fl, ps_coreid(), size, allocsz, headintern);   \
}									       \
inline void							               \
ps_slabptr_free_coreid_##name(struct ps_mem *m, void *buf, coreid_t curr)      \
{									       \
        struct ps_mem_percore *fl = m->__ps_slab_freelist;		       \
	__ps_slab_mem_free(buf, fl, curr, size, allocsz, headintern);          \
}									       \
inline size_t								       \
ps_slab_objmem_##name(void)						       \
{ return __ps_slab_objmemsz(size); }					       \
inline size_t								       \
ps_slab_nobjs_##name(void)						       \
{ return __ps_slab_max_nobjs(size, allocsz, headintern); }		       \
inline unsigned int							       \
ps_slab_objoff_##name(void *obj)					       \
{									       \
	struct mheader *h = __ps_mhead_get(obj);			       \
	__ps_slab_objsoff(h->slab, h, size, headintern);		       \
}

/*
 * allocsz is the size of the backing memory allocation, and
 * headintern is 0 or 1, should the ps_slab header be internally
 * allocated from that slab of memory, or from elsewhere.
 *
 * Note: if you use headintern == 1, then you must manually create
 * PS_SLAB_CREATE_DEF(meta, sizeof(struct ps_slab));
 */
#define PS_SLAB_CREATE(name, size, allocsz, headintern)	\
PS_MEM_CREATE_DATA(name)				\
PS_SLAB_CREATE_FNS(name, size, allocsz, headintern)

#define PS_SLAB_CREATE_DEF(name, size)		\
PS_SLAB_CREATE(name, size, PS_PAGE_SIZE, 1)

/* Create function prototypes for cross-object usage */
#define PS_SLAB_CREATE_PROTOS(name)				\
void  *ps_slab_alloc_##name(void);			        \
void   ps_slab_free_##name(void *buf);				\
size_t ps_slab_objmem_##name(void);                             \
size_t ps_slab_nobjs_##name(void);

#endif /* PS_SLAB_H */