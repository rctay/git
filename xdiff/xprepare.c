/*
 *  LibXDiff by Davide Libenzi ( File Differential Library )
 *  Copyright (C) 2003  Davide Libenzi
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  Davide Libenzi <davidel@xmailserver.org>
 *
 */

#include "xinclude.h"


#define XDL_KPDIS_RUN 4
#define XDL_MAX_EQLIMIT 1024
#define XDL_SIMSCAN_WINDOW 100
#define XDL_TRIM_BLK 128


typedef struct s_xdlclass {
	struct s_xdlclass *next;
	unsigned long ha;
	char const *line;
	long size;
	long idx;
} xdlclass_t;

typedef struct s_xdlclassifier {
	unsigned int hbits;
	long hsize;
	xdlclass_t **rchash;
	chastore_t ncha;
	long count;
	long flags;
} xdlclassifier_t;




static int xdl_init_classifier(xdlclassifier_t *cf, long size, long flags);
static void xdl_free_classifier(xdlclassifier_t *cf);
static int xdl_classify_record(xdlclassifier_t *cf, xrecord_t **rhash, unsigned int hbits,
			       xrecord_t *rec);
static int xdl_prepare_ctx(mmfile_t *mf, long narec, xpparam_t const *xpp,
			   xdlclassifier_t *cf, xdfile_t *xdf,
			   xrecord_t **arec, long *ntail);
static void xdl_free_ctx(xdfile_t *xdf);
static int xdl_clean_mmatch(char const *dis, long i, long s, long e);
static int xdl_cleanup_records(xdfile_t *xdf1, xdfile_t *xdf2);
static int xdl_optimize_ctxs(xdfile_t *xdf1, xdfile_t *xdf2);




static int xdl_init_classifier(xdlclassifier_t *cf, long size, long flags) {
	cf->flags = flags;

	cf->hbits = xdl_hashbits((unsigned int) size);
	cf->hsize = 1L + (1 << cf->hbits);

	if (xdl_cha_init(&cf->ncha, sizeof(xdlclass_t), size / 4 + 1) < 0) {

		return -1;
	}
	if (!(cf->rchash = (xdlclass_t **) xdl_malloc(cf->hsize * sizeof(xdlclass_t *)))) {

		xdl_cha_free(&cf->ncha);
		return -1;
	}
	memset(cf->rchash, 0, cf->hsize * sizeof(xdlclass_t *));

	cf->count = 0;

	return 0;
}


static void xdl_free_classifier(xdlclassifier_t *cf) {

	xdl_free(cf->rchash);
	xdl_cha_free(&cf->ncha);
}


static int xdl_classify_record(xdlclassifier_t *cf, xrecord_t **rhash, unsigned int hbits,
			       xrecord_t *rec) {
	long hi;
	char const *line;
	xdlclass_t *rcrec;

	line = rec->ptr;
	hi = rec->ha == 0 ? 0 : 1L + (long) XDL_HASHLONG(rec->ha, cf->hbits);
	for (rcrec = cf->rchash[hi]; rcrec; rcrec = rcrec->next)
		if (rec->ha == 0 ||
			(rcrec->ha == rec->ha &&
				xdl_recmatch(rcrec->line, rcrec->size,
					rec->ptr, rec->size, cf->flags)))
			break;

	if (!rcrec) {
		if (!(rcrec = xdl_cha_alloc(&cf->ncha))) {

			return -1;
		}
		rcrec->idx = cf->count++;
		rcrec->line = line;
		rcrec->size = rec->size;
		rcrec->ha = rec->ha;
		rcrec->next = cf->rchash[hi];
		cf->rchash[hi] = rcrec;
	}

	rec->ha = (unsigned long) rcrec->idx;

	hi = rec->ha == 0 ? 0 : 1L + (long) XDL_HASHLONG(rec->ha, hbits);
	rec->next = rhash[hi];
	rhash[hi] = rec;

	return 0;
}


static void xdl_trim_head(mmfile_t *mf1, mmfile_t *mf2, xpparam_t const *xpp,
			  xdfile_t *xdf1, xdfile_t *xdf2) {

	long trimmed = 0, recovered = 0;
	long smaller = XDL_MIN(mf1->size, mf2->size);
	char const *p1, *p2;
	char const *cur, *top;

	/* part 1: blocks */
	p1 = mf1->ptr;
	p2 = mf2->ptr;

	top = p1 + smaller;

	while (XDL_TRIM_BLK + trimmed <= smaller && !memcmp(p1, p2, XDL_TRIM_BLK)) {
		trimmed += XDL_TRIM_BLK;
		p1 += XDL_TRIM_BLK;
		p2 += XDL_TRIM_BLK;
	}

	while (recovered < trimmed)
		if (*(p1 - recovered++) == '\n')
			break;
	p1 -= recovered;
	p2 -= recovered;

	/* part 2: line based */
	cur = p1;
	while (cur < top
		&& (cur = memchr(cur, '\n', top - cur))
		&& !memcmp(p1, p2, ++cur - p1)) {
		p2 += cur - p1;
		p1 = cur;
	}

	xdf1->rstart = p1;
	xdf2->rstart = p2;
}


static void xdl_trim_tail(mmfile_t *mf1, mmfile_t *mf2, xpparam_t const *xpp,
			  xdfile_t *xdf1, xdfile_t *xdf2) {

	long trimmed = 0, recovered = 0;
	long smaller = XDL_MIN(mf1->size, mf2->size);
	long blk;
	char const *p1 = mf1->ptr + mf1->size;
	char const *p2 = mf2->ptr + mf2->size;
	char const *cur, *top = p1;

	while (XDL_TRIM_BLK + trimmed <= smaller && !memcmp(p1 - XDL_TRIM_BLK, p2 - XDL_TRIM_BLK, XDL_TRIM_BLK)) {
		trimmed += XDL_TRIM_BLK;
		p1 -= XDL_TRIM_BLK;
		p2 -= XDL_TRIM_BLK;
	}

	while (recovered < trimmed) {
		if (p1[recovered] == '\n')
			break;
		recovered++;
	}
	p1 += recovered;
	p2 += recovered;

	top = p1 - (smaller - (top - p1));
	cur = p1;

	while (cur >= top
		&& (cur = memrchr(top, '\n', cur - top))) {
		blk = p1 - cur;
		if (memcmp(p1 - blk, p2 - blk, blk))
			break;
		p1 -= blk;
		p2 -= blk;
	}

	/* ensure that rend >= rstart */
	blk = 0;
	if (p1 < xdf1->rstart)
		blk = xdf1->rstart - p1;
	else if (p2 < xdf2->rstart)
		blk = xdf2->rstart - p2;

	xdf1->rend = p1 + blk;
	xdf2->rend = p2 + blk;
}


static void xdl_set_dend(xdfile_t *xdf, xpparam_t const *xpp,
			 xdlclassifier_t *cf, long lim, long ntail)
{
	long i, ha;
	xrecord_t *rec;

	xdf->dend = xdf->nrec - XDL_MIN(lim - xdf->dstart, ntail) - 1;

	i = xdf->nrec - ntail;
	while (i < xdf->nrec) {
		rec = xdf->recs[i];
		rec->ha = xdl_hash_record(&rec->ptr, rec->ptr + rec->size, xpp->flags);
		rec->ptr -= rec->size;

		xdl_classify_record(cf, xdf->rhash, xdf->hbits, rec);

		i++;
	}
}


static int xdl_prepare_ctx(mmfile_t *mf, long narec, xpparam_t const *xpp,
			   xdlclassifier_t *cf, xdfile_t *xdf,
			   xrecord_t **arec, long *ntail) {
	unsigned int hbits;
	long nrec, hsize, bsize;
	unsigned long hav;
	char const *blk, *cur, *top, *prev;
	xrecord_t *crec;
	xrecord_t **recs, **rrecs;
	xrecord_t **rhash;
	unsigned long *ha;
	char *rchg;
	long *rindex;

	if (xdl_cha_init(&xdf->rcha, sizeof(xrecord_t), narec / 4 + 1) < 0) {

		return -1;
	}
	if (!(recs = (xrecord_t **) xdl_malloc(narec * sizeof(xrecord_t *)))) {

		xdl_cha_free(&xdf->rcha);
		return -1;
	}

	hbits = xdl_hashbits((unsigned int) narec);
	hsize = 1L + (1 << hbits);
	if (!(rhash = (xrecord_t **) xdl_malloc(hsize * sizeof(xrecord_t *)))) {

		xdl_free(recs);
		xdl_cha_free(&xdf->rcha);
		return -1;
	}
	memset(rhash, 0, hsize * sizeof(xrecord_t *));

	nrec = 0;
	xdf->dstart = *ntail = 0;
	if ((cur = blk = xdl_mmfile_first(mf, &bsize)) != NULL) {
		for (top = blk + bsize; cur && cur < top; ) {
			prev = cur;
			if (cur < xdf->rstart) {
				if (arec)
					cur += (arec++)[0]->size;
				else {
					cur = memchr(cur, '\n', top - cur);
					cur = cur ? cur + 1 : top;
				}
				hav = 0;
				xdf->dstart++;
			} else if (cur > xdf->rend) {
				cur = memchr(cur, '\n', top - cur);
				cur = cur ? cur + 1 : top;
				hav = 0;
				(*ntail)++;
			} else
				hav = xdl_hash_record(&cur, top, xpp->flags);
			if (nrec >= narec) {
				narec *= 2;
				if (!(rrecs = (xrecord_t **) xdl_realloc(recs, narec * sizeof(xrecord_t *)))) {

					xdl_free(rhash);
					xdl_free(recs);
					xdl_cha_free(&xdf->rcha);
					return -1;
				}
				recs = rrecs;
			}
			if (!(crec = xdl_cha_alloc(&xdf->rcha))) {

				xdl_free(rhash);
				xdl_free(recs);
				xdl_cha_free(&xdf->rcha);
				return -1;
			}
			crec->ptr = prev;
			crec->size = (long) (cur - prev);
			crec->ha = hav;
			recs[nrec++] = crec;

			if (!*ntail && xdl_classify_record(cf, rhash, hbits, crec) < 0) {

				xdl_free(rhash);
				xdl_free(recs);
				xdl_cha_free(&xdf->rcha);
				return -1;
			}
		}
	}

	if (!(rchg = (char *) xdl_malloc((nrec + 2) * sizeof(char)))) {

		xdl_free(rhash);
		xdl_free(recs);
		xdl_cha_free(&xdf->rcha);
		return -1;
	}
	memset(rchg, 0, (nrec + 2) * sizeof(char));

	if (!(rindex = (long *) xdl_malloc((nrec + 1) * sizeof(long)))) {

		xdl_free(rchg);
		xdl_free(rhash);
		xdl_free(recs);
		xdl_cha_free(&xdf->rcha);
		return -1;
	}
	if (!(ha = (unsigned long *) xdl_malloc((nrec + 1) * sizeof(unsigned long)))) {

		xdl_free(rindex);
		xdl_free(rchg);
		xdl_free(rhash);
		xdl_free(recs);
		xdl_cha_free(&xdf->rcha);
		return -1;
	}

	xdf->nrec = nrec;
	xdf->recs = recs;
	xdf->hbits = hbits;
	xdf->rhash = rhash;
	xdf->rchg = rchg + 1;
	xdf->rindex = rindex;
	xdf->nreff = 0;
	xdf->ha = ha;

	return 0;
}


static void xdl_free_ctx(xdfile_t *xdf) {

	xdl_free(xdf->rhash);
	xdl_free(xdf->rindex);
	xdl_free(xdf->rchg - 1);
	xdl_free(xdf->ha);
	xdl_free(xdf->recs);
	xdl_cha_free(&xdf->rcha);
}


int xdl_prepare_env(mmfile_t *mf1, mmfile_t *mf2, xpparam_t const *xpp,
		    xdfenv_t *xe) {
	long lim;
	long enl1, enl2;
	long ntail1, ntail2;
	xdlclassifier_t cf;

	enl1 = xdl_guess_lines(mf1) + 1;
	enl2 = xdl_guess_lines(mf2) + 1;

	if (xdl_init_classifier(&cf, enl1 + enl2 + 1, xpp->flags) < 0) {

		return -1;
	}

	xdl_trim_head(mf1, mf2, xpp, &xe->xdf1, &xe->xdf2);
	xdl_trim_tail(mf1, mf2, xpp, &xe->xdf1, &xe->xdf2);

	if (xdl_prepare_ctx(mf1, enl1, xpp, &cf, &xe->xdf1, NULL, &ntail1) < 0) {

		xdl_free_classifier(&cf);
		return -1;
	}
	if (xdl_prepare_ctx(mf2, enl2, xpp, &cf, &xe->xdf2, xe->xdf1.recs, &ntail2) < 0) {

		xdl_free_ctx(&xe->xdf1);
		xdl_free_classifier(&cf);
		return -1;
	}

	lim = XDL_MIN(xe->xdf1.nrec, xe->xdf2.nrec);
	xdl_set_dend(&xe->xdf1, xpp, &cf, lim, ntail1);
	xdl_set_dend(&xe->xdf2, xpp, &cf, lim, ntail2);

	xdl_free_classifier(&cf);

	if (!(xpp->flags & XDF_PATIENCE_DIFF) &&
			xdl_optimize_ctxs(&xe->xdf1, &xe->xdf2) < 0) {

		xdl_free_ctx(&xe->xdf2);
		xdl_free_ctx(&xe->xdf1);
		return -1;
	}

	return 0;
}


void xdl_free_env(xdfenv_t *xe) {

	xdl_free_ctx(&xe->xdf2);
	xdl_free_ctx(&xe->xdf1);
}


static int xdl_clean_mmatch(char const *dis, long i, long s, long e) {
	long r, rdis0, rpdis0, rdis1, rpdis1;

	/*
	 * Limits the window the is examined during the similar-lines
	 * scan. The loops below stops when dis[i - r] == 1 (line that
	 * has no match), but there are corner cases where the loop
	 * proceed all the way to the extremities by causing huge
	 * performance penalties in case of big files.
	 */
	if (i - s > XDL_SIMSCAN_WINDOW)
		s = i - XDL_SIMSCAN_WINDOW;
	if (e - i > XDL_SIMSCAN_WINDOW)
		e = i + XDL_SIMSCAN_WINDOW;

	/*
	 * Scans the lines before 'i' to find a run of lines that either
	 * have no match (dis[j] == 0) or have multiple matches (dis[j] > 1).
	 * Note that we always call this function with dis[i] > 1, so the
	 * current line (i) is already a multimatch line.
	 */
	for (r = 1, rdis0 = 0, rpdis0 = 1; (i - r) >= s; r++) {
		if (!dis[i - r])
			rdis0++;
		else if (dis[i - r] == 2)
			rpdis0++;
		else
			break;
	}
	/*
	 * If the run before the line 'i' found only multimatch lines, we
	 * return 0 and hence we don't make the current line (i) discarded.
	 * We want to discard multimatch lines only when they appear in the
	 * middle of runs with nomatch lines (dis[j] == 0).
	 */
	if (rdis0 == 0)
		return 0;
	for (r = 1, rdis1 = 0, rpdis1 = 1; (i + r) <= e; r++) {
		if (!dis[i + r])
			rdis1++;
		else if (dis[i + r] == 2)
			rpdis1++;
		else
			break;
	}
	/*
	 * If the run after the line 'i' found only multimatch lines, we
	 * return 0 and hence we don't make the current line (i) discarded.
	 */
	if (rdis1 == 0)
		return 0;
	rdis1 += rdis0;
	rpdis1 += rpdis0;

	return rpdis1 * XDL_KPDIS_RUN < (rpdis1 + rdis1);
}


/*
 * Try to reduce the problem complexity, discard records that have no
 * matches on the other file. Also, lines that have multiple matches
 * might be potentially discarded if they happear in a run of discardable.
 */
static int xdl_cleanup_records(xdfile_t *xdf1, xdfile_t *xdf2) {
	long i, nm, rhi, nreff, mlim;
	unsigned long hav;
	xrecord_t **recs;
	xrecord_t *rec;
	char *dis, *dis1, *dis2;

	if (!(dis = (char *) xdl_malloc(xdf1->nrec + xdf2->nrec + 2))) {

		return -1;
	}
	memset(dis, 0, xdf1->nrec + xdf2->nrec + 2);
	dis1 = dis;
	dis2 = dis1 + xdf1->nrec + 1;

	if ((mlim = xdl_bogosqrt(xdf1->nrec)) > XDL_MAX_EQLIMIT)
		mlim = XDL_MAX_EQLIMIT;
	for (i = xdf1->dstart, recs = &xdf1->recs[xdf1->dstart]; i <= xdf1->dend; i++, recs++) {
		hav = (*recs)->ha;
		rhi = hav == 0 ? 0 : 1L + ((long) XDL_HASHLONG(hav, xdf2->hbits));
		for (nm = 0, rec = xdf2->rhash[rhi]; rec; rec = rec->next)
			if (rec->ha == hav && ++nm == mlim)
				break;
		dis1[i] = (nm == 0) ? 0: (nm >= mlim) ? 2: 1;
	}

	if ((mlim = xdl_bogosqrt(xdf2->nrec)) > XDL_MAX_EQLIMIT)
		mlim = XDL_MAX_EQLIMIT;
	for (i = xdf2->dstart, recs = &xdf2->recs[xdf2->dstart]; i <= xdf2->dend; i++, recs++) {
		hav = (*recs)->ha;
		rhi = hav == 0 ? 0 : 1L + (long) XDL_HASHLONG(hav, xdf1->hbits);
		for (nm = 0, rec = xdf1->rhash[rhi]; rec; rec = rec->next)
			if (rec->ha == hav && ++nm == mlim)
				break;
		dis2[i] = (nm == 0) ? 0: (nm >= mlim) ? 2: 1;
	}

	for (nreff = 0, i = xdf1->dstart, recs = &xdf1->recs[xdf1->dstart];
	     i <= xdf1->dend; i++, recs++) {
		if (dis1[i] == 1 ||
		    (dis1[i] == 2 && !xdl_clean_mmatch(dis1, i, xdf1->dstart, xdf1->dend))) {
			xdf1->rindex[nreff] = i;
			xdf1->ha[nreff] = (*recs)->ha;
			nreff++;
		} else
			xdf1->rchg[i] = 1;
	}
	xdf1->nreff = nreff;

	for (nreff = 0, i = xdf2->dstart, recs = &xdf2->recs[xdf2->dstart];
	     i <= xdf2->dend; i++, recs++) {
		if (dis2[i] == 1 ||
		    (dis2[i] == 2 && !xdl_clean_mmatch(dis2, i, xdf2->dstart, xdf2->dend))) {
			xdf2->rindex[nreff] = i;
			xdf2->ha[nreff] = (*recs)->ha;
			nreff++;
		} else
			xdf2->rchg[i] = 1;
	}
	xdf2->nreff = nreff;

	xdl_free(dis);

	return 0;
}


static int xdl_optimize_ctxs(xdfile_t *xdf1, xdfile_t *xdf2) {

	if (xdl_cleanup_records(xdf1, xdf2) < 0) {

		return -1;
	}

	return 0;
}
