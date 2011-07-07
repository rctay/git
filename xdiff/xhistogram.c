#include "xinclude.h"
#include "xtypes.h"
#include "xdiff.h"

#define REC_NEXT_SHIFT	(12 + 6)
#define REC_PTR_SHIFT	(6)
#define REC_PTR_MASK	((1 << 12) - 1)
#define REC_CNT_MASK	((1 << 6) - 1)
#define MAX_PTR		REC_PTR_MASK
#define MAX_CNT		REC_CNT_MASK

#define REC_CREATE(n, p, c) \
	(((unsigned long) n << REC_NEXT_SHIFT) \
	| ((unsigned long) p << REC_PTR_SHIFT) \
	| c)
#define REC_NEXT(r) ((unsigned int) (r >> REC_NEXT_SHIFT))
#define REC_PTR(r) \
	(((unsigned int) (r >> REC_PTR_SHIFT)) & REC_PTR_MASK)
#define REC_CNT(a) (((unsigned int) a) & REC_CNT_MASK)

#define LINE_END(n) (line##n+count##n-1)
#define LINE_END_PTR(n) (*line##n+*count##n-1)

struct histindex {
	unsigned int *table, table_bits, table_size;

	unsigned long *recs;
	unsigned int recs_size;

	unsigned int *next, *rec_idxs, size;

	unsigned int max_chain_length,
		     key_shift,
		     ptr_shift;

	unsigned int recs_count,
		     cnt,
		     has_common;

	xdfenv_t *env;
	xpparam_t const *xpp;
};

struct region {
	unsigned int begin1, end1;
	unsigned int begin2, end2;
};

#define INDEX_NEXT(i, a) (i->next[(a) - i->ptr_shift])
#define INDEX_REC_IDXS(i, a) (i->rec_idxs[(a) - i->ptr_shift])

#define get_rec(env, s, l) \
	(env->xdf##s.recs[l-1])

static int cmp_recs(xpparam_t const *xpp,
	xrecord_t *r1, xrecord_t *r2)
{
	return r1->ha == r2->ha &&
		xdl_recmatch(r1->ptr, r1->size, r2->ptr, r2->size,
			    xpp->flags);
}

#define cmp_env(xpp, env, s1, l1, s2, l2) \
	(cmp_recs(xpp, get_rec(env, s1, l1), get_rec(env, s2, l2)))

#define cmp(i, s1, l1, s2, l2) \
	(cmp_env(i->xpp, i->env, s1, l1, s2, l2))

#define table_hash(index, side, line) \
	XDL_HASHLONG((get_rec(index->env, side, line))->ha, index->table_bits)

static int scanA(struct histindex *index, int line1, int count1)
{
	unsigned int ptr, rec_idx, tbl_idx;
	unsigned int chain_len;
	unsigned int new_cnt;
	unsigned long rec;

	for (ptr = LINE_END(1); line1 <= ptr; ptr--) {
		tbl_idx = table_hash(index, 1, ptr);

		chain_len = 0;
		for (rec_idx = index->table[tbl_idx]; rec_idx != 0; ) {
			rec = index->recs[rec_idx];
			if (cmp(index, 1, REC_PTR(rec), 1, ptr)) {
				new_cnt = REC_CNT(rec) + 1;
				if (new_cnt > MAX_CNT)
					new_cnt = MAX_CNT;
				index->recs[rec_idx] = REC_CREATE(REC_NEXT(rec), ptr, new_cnt);
				INDEX_NEXT(index, ptr) = REC_PTR(rec);
				INDEX_REC_IDXS(index, ptr) = rec_idx;
				goto continue_scan;
			}

			rec_idx = REC_NEXT(rec);
			chain_len++;
		}

		if (chain_len == index->max_chain_length)
			return -1;

		if (index->recs_count >= MAX_PTR)
			return -1;
		rec_idx = ++index->recs_count;
		if (rec_idx == index->recs_size) {
			index->recs_size = XDL_MIN(index->recs_size << 1, 1 + count1);
			if (!(index->recs = (unsigned long *) xdl_realloc(index->recs, sizeof(*index->recs) * index->recs_size)))
				return -1;
		}

		index->recs[rec_idx] = REC_CREATE(index->table[tbl_idx], ptr, 1);
		INDEX_REC_IDXS(index, ptr) = rec_idx;
		index->table[tbl_idx] = rec_idx;

continue_scan:
		; /* no op */
	}

	return 0;
}

static int try_lcs(struct histindex *index, struct region *lcs, int b_ptr,
	int line1, int count1, int line2, int count2)
{
	unsigned int b_next = b_ptr + 1;
	unsigned int rec_idx = index->table[table_hash(index, 2, b_ptr)];
	unsigned long rec;
	unsigned int as, ae, bs, be, np, rc;
	int should_break;

	for (; rec_idx != 0; rec_idx = REC_NEXT(rec)) {
		rec = index->recs[rec_idx];

		if (REC_CNT(rec) > index->cnt) {
			if (!index->has_common)
				index->has_common = cmp(index, 1, REC_PTR(rec), 2, b_ptr);
			continue;
		}

		as = REC_PTR(rec);
		if (!cmp(index, 1, as, 2, b_ptr))
			continue;

		index->has_common = 1;
		for (;;) {
			should_break = 0;
			np = INDEX_NEXT(index, as);
			bs = b_ptr;
			ae = as;
			be = bs;
			rc = REC_CNT(rec);

			while (line1 < as && line2 < bs
				&& cmp(index, 1, as-1, 2, bs-1)) {
				as--;
				bs--;
				if (1 < rc)
					rc = XDL_MIN(rc, REC_CNT(index->recs[INDEX_REC_IDXS(index, as)]));
			}
			while (ae < LINE_END(1) && be < LINE_END(2)
				&& cmp(index, 1, ae+1, 2, be+1)) {
				ae++;
				be++;
				if (1 < rc)
					rc = XDL_MIN(rc, REC_CNT(index->recs[INDEX_REC_IDXS(index, ae)]));
			}

			if (b_next < be)
				b_next = be;
			if (lcs->end1 - lcs->begin1 + 1 < ae - as || rc < index->cnt) {
				lcs->begin1 = as;
				lcs->begin2 = bs;
				lcs->end1 = ae;
				lcs->end2 = be;
				index->cnt = rc;
			}

			if (np == 0)
				break;

			while (np < ae) {
				np = INDEX_NEXT(index, np);
				if (np == 0) {
					should_break = 1;
					break;
				}
			}

			if (should_break)
				break;

			as = np;
		}
	}
	return b_next;
}

static int find_lcs(struct histindex *index, struct region *lcs,
	int line1, int count1, int line2, int count2) {
	int b_ptr;

	if (scanA(index, line1, count1))
		return -1;

	index->cnt = index->max_chain_length + 1;

	for (b_ptr = line2; b_ptr <= LINE_END(2); )
		b_ptr = try_lcs(index, lcs, b_ptr, line1, count1, line2, count2);

	return index->has_common && index->max_chain_length < index->cnt;
}

static void reduce_common_start_end(xpparam_t const *xpp, xdfenv_t *env,
	int *line1, int *count1, int *line2, int *count2)
{
	if (*count1 <= 1 || *count2 <= 1)
		return;
	while (*count1 > 1 && *count2 > 1 && cmp_env(xpp, env, 1, *line1, 2, *line2)) {
		(*line1)++;
		(*count1)--;
		(*line2)++;
		(*count2)--;
	}
	while (*count1 > 1 && *count2 > 1 && cmp_env(xpp, env, 1, LINE_END_PTR(1), 2, LINE_END_PTR(2))) {
		(*count1)--;
		(*count2)--;
	}
}

static int fall_back_to_classic_diff(struct histindex *index,
		int line1, int count1, int line2, int count2)
{
	xpparam_t xpp;
	xpp.flags = index->xpp->flags & ~XDF_HISTOGRAM_DIFF;

	return xdl_fall_back_diff(index->env, &xpp,
				  line1, count1, line2, count2);
}

static int histogram_diff(xpparam_t const *xpp, xdfenv_t *env,
	int line1, int count1, int line2, int count2)
{
	struct histindex index;
	struct region lcs;
	int sz;
	int result = -1;

	if (count1 <= 0 && count2 <= 0)
		return 0;

	if (LINE_END(1) >= MAX_PTR)
		return -1;

	if (!count1) {
		while(count2--)
			env->xdf2.rchg[line2++ - 1] = 1;
		return 0;
	} else if (!count2) {
		while(count1--)
			env->xdf1.rchg[line1++ - 1] = 1;
		return 0;
	}

	memset(&index, 0, sizeof(index));

	index.env = env;
	index.xpp = xpp;

	index.table = NULL;
	index.recs = NULL;
	index.next = NULL;
	index.rec_idxs = NULL;

	index.table_bits = xdl_hashbits(count1);
	sz = index.table_size = 1 << index.table_bits;
	sz *= sizeof(int);
	if (!(index.table = xdl_malloc(sz)))
		goto cleanup;
	memset(index.table, 0, sz);

	sz = index.recs_size = XDL_MAX(4, count1 >> 3);
	sz *= sizeof(long);
	if (!(index.recs = xdl_malloc(sz)))
		goto cleanup;
	memset(index.recs, 0, sz);

	sz = index.size = count1;
	sz *= sizeof(int);
	if (!(index.next = xdl_malloc(sz)))
		goto cleanup;
	memset(index.next, 0, sz);

	if (!(index.rec_idxs = xdl_malloc(sz)))
		goto cleanup;
	memset(index.rec_idxs, 0, sz);

	index.ptr_shift = line1;
	index.max_chain_length = 64;

	memset(&lcs, 0, sizeof(lcs));
	if (find_lcs(&index, &lcs, line1, count1, line2, count2))
		result = fall_back_to_classic_diff(&index, line1, count1, line2, count2);
	else {
		result = 0;
		if (lcs.begin1 == 0 && lcs.begin2 == 0) {
			int ptr;
			for (ptr = 0; ptr < count1; ptr++)
				env->xdf1.rchg[line1 + ptr - 1] = 1;
			for (ptr = 0; ptr < count2; ptr++)
				env->xdf2.rchg[line2 + ptr - 1] = 1;
		} else {
			result = histogram_diff(xpp, env,
				line1, lcs.begin1 - line1,
				line2, lcs.begin2 - line2);
			result = histogram_diff(xpp, env,
				lcs.end1 + 1, LINE_END(1) - lcs.end1,
				lcs.end2 + 1, LINE_END(2) - lcs.end2);
			result *= -1;
		}
	}

cleanup:
	xdl_free(index.table);
	xdl_free(index.recs);
	xdl_free(index.next);
	xdl_free(index.rec_idxs);

	return result;
}

int xdl_do_histogram_diff(mmfile_t *file1, mmfile_t *file2,
	xpparam_t const *xpp, xdfenv_t *env)
{
	int line1, line2, count1, count2;

	if (xdl_prepare_env(file1, file2, xpp, env) < 0)
		return -1;

	line1 = line2 = 1;
	count1 = env->xdf1.nrec;
	count2 = env->xdf2.nrec;

	reduce_common_start_end(xpp, env, &line1, &count1, &line2, &count2);

	return histogram_diff(xpp, env, line1, count1, line2, count2);
}
