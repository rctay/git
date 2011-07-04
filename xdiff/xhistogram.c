#include "xinclude.h"
#include "xtypes.h"
#include "xdiff.h"

#define MAX_PTR	((1<<15)-1)
#define MAX_CNT	((1<<15)-1)

#define LINE_END(n) (line##n+count##n-1)
#define LINE_END_PTR(n) (*line##n+*count##n-1)

struct histindex {
	struct record {
		long ptr, cnt;
	} *line_map; /* map of line to record chain */
	long line_map_size;

	unsigned int max_chain_length,
		     has_common;

	long ptr_shift;
	long cnt;

	xdfenv_t *env;
	xpparam_t const *xpp;
};

struct region {
	long begin1, end1;
	long begin2, end2;
};

#define LINE_MAP(i, a) (i->line_map[(a) - i->ptr_shift])

static xdfile_t *map_env_side(xdfenv_t *env, int s)
{
	return s == 1 ? &env->xdf1 : &env->xdf2;
}

static xdfile_t *map_side(struct histindex *index, int s)
{
	return map_env_side(index->env, s);
}

static int cmp_env(xpparam_t const *xpp, xdfenv_t *env,
	int side1, long line1, int side2, long line2)
{
	xrecord_t *r1 = (map_env_side(env, side1))->recs[line1 - 1],
		  *r2 = (map_env_side(env, side2))->recs[line2 - 1];
	return xdl_recmatch(r1->ptr, r1->size, r2->ptr, r2->size,
			    xpp->flags);
}

static int cmp(struct histindex *index,
	int side1, long line1, int side2, long line2)
{
	return cmp_env(index->xpp, index->env, side1, line1, side2, line2);
}

static xrecord_t *table_hash(struct histindex *index, int side, unsigned long ha)
{
	xdfile_t *a = map_side(index, side);
	return a->rhash[(long) XDL_HASHLONG(ha, a->hbits)];
}

static int scanA(struct histindex *index, long line1, long count1)
{
	xdfile_t *a = map_side(index, 1);
	long i;
	long chain_len;
	xrecord_t **rec_chain, *curr, *prev, *tail;

	i = 1 << a->hbits;
	rec_chain = a->rhash;
	while (i--) {
		tail = *rec_chain;

		while (tail) {
			while (tail->previous
				&& tail->previous->head == tail->head
				&& tail->previous->line_number <= LINE_END(1))
				tail = tail->previous;
			while (tail->line_number > LINE_END(1)
				&& tail->next
				&& tail->next->head == tail->head)
				tail = tail->next;

			curr = tail;
			prev = NULL;
			chain_len = 0;
			while (curr && line1 <= curr->line_number && curr->line_number <= LINE_END(1)) {
				LINE_MAP(index, curr->line_number).ptr = prev ? prev->line_number : 0;
				LINE_MAP(index, curr->line_number).cnt = tail->count - curr->count + 1;

				chain_len++;
				if (!curr->next ||
					curr->next->head != curr->head ||
					curr->next->line_number < line1)
					break;
				prev = curr;
				curr = curr->next;
			}

			if (chain_len >= index->max_chain_length)
				return -1;

			tail = tail->head->next;
		}

		rec_chain++;
	}

	return 0;
}

static long get_next_ptr(struct histindex *index, long ptr)
{
	return LINE_MAP(index, ptr).ptr;
}

static long get_cnt(struct histindex *index, long ptr)
{
	return LINE_MAP(index, ptr).cnt;
}

static int try_lcs(struct histindex *index, struct region *lcs, int b_ptr,
	long line1, long count1, long line2, long count2)
{
	unsigned int b_next = b_ptr + 1;
	xrecord_t *brec = index->env->xdf2.recs[b_ptr - 1];
	xrecord_t *rec = table_hash(index, 1, brec->ha);
	unsigned int as, ae, bs, be, np, rc;
	int should_break;

	while (rec
		&& rec->ha != brec->ha
		&& !xdl_recmatch(rec->ptr, rec->size,
				 brec->ptr, brec->size, index->xpp->flags)) {
		rec = rec->head->next;
	}

	if (!rec)
		return b_next;

	rec = rec->head;
	while (rec
		&& rec->line_number < line1
		&& rec->previous
		&& rec->previous->head == rec->head)
		rec = rec->previous;

	if (rec && rec->line_number < line1)
		return b_next;

	while (rec && rec->line_number <= LINE_END(1)) {
		if (get_cnt(index, rec->line_number) > index->cnt) {
			if (!index->has_common)
				index->has_common = cmp(index, 1, rec->line_number, 2, b_ptr);
			goto continue_try_lcs;
		}

		as = rec->line_number;
		if (!cmp(index, 1, as, 2, b_ptr))
			goto continue_try_lcs;

		index->has_common = 1;
		for (;;) {
			should_break = 0;
			np = get_next_ptr(index, as);
			bs = b_ptr;
			ae = as;
			be = bs;
			rc = get_cnt(index, rec->line_number);

			while (line1 < as && line2 < bs
				&& cmp(index, 1, as-1, 2, bs-1)) {
				as--;
				bs--;
				if (1 < rc)
					rc = XDL_MIN(rc, get_cnt(index, as));
			}
			while (ae < LINE_END(1) && be < LINE_END(2)
				&& cmp(index, 1, ae+1, 2, be+1)) {
				ae++;
				be++;
				if (1 < rc)
					rc = XDL_MIN(rc, get_cnt(index, ae));
			}

			if (b_next <= be)
				b_next = be+1;
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
				np = get_next_ptr(index, np);
				if (np == 0) {
					should_break = 1;
					break;
				}
			}

			if (should_break)
				break;

			as = np;
		}
continue_try_lcs:
		if (!rec->previous
			|| rec->previous->head != rec->head)
			break;
		rec = rec->previous;
	}
	return b_next;
}

static int find_lcs(struct histindex *index, struct region *lcs,
	long line1, long count1, long line2, long count2) {
	int b_ptr;

	if (scanA(index, line1, count1))
		return -1;

	index->cnt = index->max_chain_length + 1;

	for (b_ptr = line2; b_ptr <= LINE_END(2); )
		b_ptr = try_lcs(index, lcs, b_ptr, line1, count1, line2, count2);

	return index->has_common && index->max_chain_length < index->cnt;
}

static void reduce_common_start_end(xpparam_t const *xpp, xdfenv_t *env,
	long *line1, long *count1, long *line2, long *count2)
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
		long line1, long count1, long line2, long count2)
{
	xpparam_t xpp;
	xpp.flags = index->xpp->flags & ~XDF_HISTOGRAM_DIFF;

	return xdl_fall_back_diff(index->env, &xpp,
				  line1, count1, line2, count2);
}

static int histogram_diff(xpparam_t const *xpp, xdfenv_t *env,
	long line1, long count1, long line2, long count2)
{
	struct histindex index;
	struct region lcs;
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

	index.env = env;
	index.xpp = xpp;

	index.max_chain_length = 64;
	index.ptr_shift = line1;
	index.has_common = 0;
	index.cnt = 0;

	index.line_map_size = count1;
	if (!(index.line_map = (struct record *)
		xdl_malloc(count1 * sizeof(struct record))))
		goto cleanup_line_map;

	memset(index.line_map, 0, index.line_map_size * sizeof(struct record));

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
			result = result || histogram_diff(xpp, env,
				line1, lcs.begin1 - line1,
				line2, lcs.begin2 - line2);
			result = result || histogram_diff(xpp, env,
				lcs.end1 + 1, LINE_END(1) - lcs.end1,
				lcs.end2 + 1, LINE_END(2) - lcs.end2);
			result *= -1;
		}
	}

	xdl_free(index.line_map);
cleanup_line_map:
	return result;
}

int xdl_do_histogram_diff(mmfile_t *file1, mmfile_t *file2,
	xpparam_t const *xpp, xdfenv_t *env)
{
	long line1, line2, count1, count2;

	if (xdl_prepare_env(file1, file2, xpp, env) < 0)
		return -1;

	line1 = line2 = 1;
	count1 = env->xdf1.nrec;
	count2 = env->xdf2.nrec;

 	reduce_common_start_end(xpp, env, &line1, &count1, &line2, &count2);

	return histogram_diff(xpp, env, line1, count1, line2, count2);
}
