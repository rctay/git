#include "xinclude.h"
#include "xtypes.h"
#include "xdiff.h"

#define MAX_PTR	((1<<15)-1)
#define MAX_CNT	((1<<15)-1)

#define LINE_END(n) (line##n+count##n-1)
#define LINE_END_PTR(n) (*line##n+*count##n-1)

struct histindex {
	struct record {
		unsigned int ptr, cnt;
		struct record *next;
	} **records, /* an ocurrence */
	  **line_map; /* map of line to record chain */
	unsigned int records_size;
	unsigned int line_map_size;

	unsigned int max_chain_length,
		     key_shift,
		     ptr_shift;

	unsigned int cnt,
		     has_common;

	xdfenv_t *env;
	xpparam_t const *xpp;
};

struct region {
	unsigned int begin1, end1;
	unsigned int begin2, end2;
};

#define LINE_MAP(i, a) (i->line_map[(a) - i->ptr_shift])

static xdfile_t *map_side(struct histindex *index, int s)
{
	return s == 1 ? &index->env->xdf1 : &index->env->xdf2;
}

static int cmp(struct histindex *index,
	int side1, int line1, int side2, int line2)
{
	xrecord_t *r1 = (map_side(index, side1))->recs[line1 - 1],
		  *r2 = (map_side(index, side2))->recs[line2 - 1];
	return xdl_recmatch(r1->ptr, r1->size, r2->ptr, r2->size,
			    index->xpp->flags);
}

static unsigned int table_hash(struct histindex *index, int side, int line)
{
	return xdl_table_key((map_side(index, side))->recs[line - 1]->ha, index->key_shift);
}

static int scanA(struct histindex *index, int line1, int count1)
{
	unsigned int ptr, tbl_idx;
	unsigned int chain_len;
	unsigned int new_cnt;
	struct record **rec_chain, *rec, *new_rec;

	for (ptr = LINE_END(1); line1 <= ptr; ptr--) {
		tbl_idx = table_hash(index, 1, ptr);
		rec_chain = index->records + tbl_idx;
		rec = *rec_chain;

		chain_len = 0;
		while (rec) {
			if (cmp(index, 1, rec->ptr, 1, ptr)) {
				/* insert a new occurrence record */
				if (!(new_rec = (struct record *)
					xdl_malloc(sizeof(struct record))))
					return -1;
				new_rec->ptr = ptr;
				new_cnt = rec->cnt + 1;
				if (new_cnt > MAX_CNT)
					new_cnt = MAX_CNT;
				new_rec->cnt = new_cnt;

				new_rec->next = rec;
				*rec_chain = new_rec;
				LINE_MAP(index, ptr) = new_rec;
				goto continue_scan;
			}

			rec = rec->next;
			chain_len++;
		}

		if (chain_len == index->max_chain_length)
			return -1;

		/* first occurrence, create occurrence chain */
		if (!(new_rec = (struct record *)
			xdl_malloc(sizeof(struct record))))
			return -1;
		new_rec->ptr = ptr;
		new_rec->cnt = 1;
		new_rec->next = *rec_chain;
		LINE_MAP(index, ptr) = new_rec;
		*rec_chain = new_rec;

continue_scan:
		; /* no op */
	}

	return 0;
}

static int get_next_ptr(struct histindex *index, int ptr)
{
	struct record *rec = LINE_MAP(index, ptr)->next;
	return rec ? rec->ptr : 0;
}

static int get_cnt(struct histindex *index, int ptr)
{
	struct record *rec = LINE_MAP(index, ptr);
	return rec ? rec->cnt : 0;
}

static int try_lcs(struct histindex *index, struct region *lcs, int b_ptr,
	int line1, int count1, int line2, int count2)
{
	unsigned int b_next = b_ptr + 1;
	struct record *rec = index->records[table_hash(index, 2, b_ptr)];
	unsigned int as, ae, bs, be, np, rc;
	int should_break;

	for (; rec; rec = rec->next) {
		if (rec->cnt > index->cnt) {
			if (!index->has_common)
				index->has_common = cmp(index, 1, rec->ptr, 2, b_ptr);
			continue;
		}

		as = rec->ptr;
		if (!cmp(index, 1, as, 2, b_ptr))
			continue;

		index->has_common = 1;
		for (;;) {
			should_break = 0;
			np = get_next_ptr(index, as);
			bs = b_ptr;
			ae = as;
			be = bs;
			rc = rec->cnt;

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

static void reduce_common_start_end(struct histindex *index,
	int *line1, int *count1, int *line2, int *count2)
{
	if (*count1 <= 1 || *count2 <= 1)
		return;
	while (*count1 > 1 && *count2 > 1 && cmp(index, 1, *line1, 2, *line2)) {
		(*line1)++;
		(*count1)--;
		(*line2)++;
		(*count2)--;
	}
	while (*count1 > 1 && *count2 > 1 && cmp(index, 1, LINE_END_PTR(1), 2, LINE_END_PTR(2))) {
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

static int histogram_diff(struct histindex *index,
	int line1, int count1, int line2, int count2)
{
	struct region lcs;
	int result;

	if (count1 <= 0 && count2 <= 0)
		return 0;

	if (LINE_END(1) >= MAX_PTR)
		return -1;

	if (!count1) {
		while(count2--)
			index->env->xdf2.rchg[line2++ - 1] = 1;
		return 0;
	} else if (!count2) {
		while(count1--)
			index->env->xdf1.rchg[line1++ - 1] = 1;
		return 0;
	}

	index->ptr_shift = line1;
	index->has_common = 0;
	index->cnt = 0;

	memset(index->records, 0, index->records_size * sizeof(struct record *));
	memset(index->line_map, 0, index->line_map_size * sizeof(struct record *));

	memset(&lcs, 0, sizeof(lcs));
	if (find_lcs(index, &lcs, line1, count1, line2, count2))
		result = fall_back_to_classic_diff(index, line1, count1, line2, count2);
	else {
		result = 0;
		if (lcs.begin1 == 0 && lcs.begin2 == 0) {
			int ptr;
			for (ptr = 0; ptr < count1; ptr++)
				index->env->xdf1.rchg[line1 + ptr - 1] = 1;
			for (ptr = 0; ptr < count2; ptr++)
				index->env->xdf2.rchg[line2 + ptr - 1] = 1;
		} else {
			result = result || histogram_diff(index,
				line1, lcs.begin1 - line1,
				line2, lcs.begin2 - line2);
			result = result || histogram_diff(index,
				lcs.end1 + 1, LINE_END(1) - lcs.end1,
				lcs.end2 + 1, LINE_END(2) - lcs.end2);
			result *= -1;
		}
	}

	return result;
}

int xdl_do_histogram_diff(mmfile_t *file1, mmfile_t *file2,
	xpparam_t const *xpp, xdfenv_t *env)
{
	struct histindex index;
	int sz, tbits;
	int line1, line2, count1, count2;
	int result = -1;

	if (xdl_prepare_env(file1, file2, xpp, env) < 0)
		goto cleanup;

	index.env = env;
	index.xpp = xpp;

	line1 = line2 = 1;
	count1 = env->xdf1.nrec;
	count2 = env->xdf2.nrec;

 	reduce_common_start_end(&index, &line1, &count1, &line2, &count2);

	tbits = xdl_table_bits(count1);

	index.key_shift = 32 - tbits;
	index.max_chain_length = 64;

	sz = index.records_size = 1 << tbits;
	if (!(index.records = (struct record **)
		xdl_malloc(sz * sizeof(struct record *))))
		goto cleanup_records;

	sz = index.line_map_size = count1;
	if (!(index.line_map = (struct record **)
		xdl_malloc(sz * sizeof(struct record *))))
		goto cleanup_line_map;

	result = histogram_diff(&index,
		line1, count1, line2, count2);

	xdl_free(index.line_map);
cleanup_line_map:
	xdl_free(index.records);
cleanup:
cleanup_records:
	; /* no-op */

	return result;
}
