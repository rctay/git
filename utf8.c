#include "git-compat-util.h"
#include "strbuf.h"
#include "utf8.h"

/* This code is originally from http://www.cl.cam.ac.uk/~mgk25/ucs/ */

struct interval {
  int first;
  int last;
};

/* auxiliary function for binary search in interval table */
static int bisearch(ucs_char_t ucs, const struct interval *table, int max)
{
	int min = 0;
	int mid;

	if (ucs < table[0].first || ucs > table[max].last)
		return 0;
	while (max >= min) {
		mid = (min + max) / 2;
		if (ucs > table[mid].last)
			min = mid + 1;
		else if (ucs < table[mid].first)
			max = mid - 1;
		else
			return 1;
	}

	return 0;
}

/* The following two functions define the column width of an ISO 10646
 * character as follows:
 *
 *    - The null character (U+0000) has a column width of 0.
 *
 *    - Other C0/C1 control characters and DEL will lead to a return
 *      value of -1.
 *
 *    - Non-spacing and enclosing combining characters (general
 *      category code Mn or Me in the Unicode database) have a
 *      column width of 0.
 *
 *    - SOFT HYPHEN (U+00AD) has a column width of 1.
 *
 *    - Other format characters (general category code Cf in the Unicode
 *      database) and ZERO WIDTH SPACE (U+200B) have a column width of 0.
 *
 *    - Hangul Jamo medial vowels and final consonants (U+1160-U+11FF)
 *      have a column width of 0.
 *
 *    - Spacing characters in the East Asian Wide (W) or East Asian
 *      Full-width (F) category as defined in Unicode Technical
 *      Report #11 have a column width of 2.
 *
 *    - All remaining characters (including all printable
 *      ISO 8859-1 and WGL4 characters, Unicode control characters,
 *      etc.) have a column width of 1.
 *
 * This implementation assumes that ucs_char_t characters are encoded
 * in ISO 10646.
 */

static int git_wcwidth(ucs_char_t ch)
{
	/*
	 * Sorted list of non-overlapping intervals of non-spacing characters,
	 * generated by
	 *   "uniset +cat=Me +cat=Mn +cat=Cf -00AD +1160-11FF +200B c".
	 */
	static const struct interval combining[] = {
		{ 0x0300, 0x0357 }, { 0x035D, 0x036F }, { 0x0483, 0x0486 },
		{ 0x0488, 0x0489 }, { 0x0591, 0x05A1 }, { 0x05A3, 0x05B9 },
		{ 0x05BB, 0x05BD }, { 0x05BF, 0x05BF }, { 0x05C1, 0x05C2 },
		{ 0x05C4, 0x05C4 }, { 0x0600, 0x0603 }, { 0x0610, 0x0615 },
		{ 0x064B, 0x0658 }, { 0x0670, 0x0670 }, { 0x06D6, 0x06E4 },
		{ 0x06E7, 0x06E8 }, { 0x06EA, 0x06ED }, { 0x070F, 0x070F },
		{ 0x0711, 0x0711 }, { 0x0730, 0x074A }, { 0x07A6, 0x07B0 },
		{ 0x0901, 0x0902 }, { 0x093C, 0x093C }, { 0x0941, 0x0948 },
		{ 0x094D, 0x094D }, { 0x0951, 0x0954 }, { 0x0962, 0x0963 },
		{ 0x0981, 0x0981 }, { 0x09BC, 0x09BC }, { 0x09C1, 0x09C4 },
		{ 0x09CD, 0x09CD }, { 0x09E2, 0x09E3 }, { 0x0A01, 0x0A02 },
		{ 0x0A3C, 0x0A3C }, { 0x0A41, 0x0A42 }, { 0x0A47, 0x0A48 },
		{ 0x0A4B, 0x0A4D }, { 0x0A70, 0x0A71 }, { 0x0A81, 0x0A82 },
		{ 0x0ABC, 0x0ABC }, { 0x0AC1, 0x0AC5 }, { 0x0AC7, 0x0AC8 },
		{ 0x0ACD, 0x0ACD }, { 0x0AE2, 0x0AE3 }, { 0x0B01, 0x0B01 },
		{ 0x0B3C, 0x0B3C }, { 0x0B3F, 0x0B3F }, { 0x0B41, 0x0B43 },
		{ 0x0B4D, 0x0B4D }, { 0x0B56, 0x0B56 }, { 0x0B82, 0x0B82 },
		{ 0x0BC0, 0x0BC0 }, { 0x0BCD, 0x0BCD }, { 0x0C3E, 0x0C40 },
		{ 0x0C46, 0x0C48 }, { 0x0C4A, 0x0C4D }, { 0x0C55, 0x0C56 },
		{ 0x0CBC, 0x0CBC }, { 0x0CBF, 0x0CBF }, { 0x0CC6, 0x0CC6 },
		{ 0x0CCC, 0x0CCD }, { 0x0D41, 0x0D43 }, { 0x0D4D, 0x0D4D },
		{ 0x0DCA, 0x0DCA }, { 0x0DD2, 0x0DD4 }, { 0x0DD6, 0x0DD6 },
		{ 0x0E31, 0x0E31 }, { 0x0E34, 0x0E3A }, { 0x0E47, 0x0E4E },
		{ 0x0EB1, 0x0EB1 }, { 0x0EB4, 0x0EB9 }, { 0x0EBB, 0x0EBC },
		{ 0x0EC8, 0x0ECD }, { 0x0F18, 0x0F19 }, { 0x0F35, 0x0F35 },
		{ 0x0F37, 0x0F37 }, { 0x0F39, 0x0F39 }, { 0x0F71, 0x0F7E },
		{ 0x0F80, 0x0F84 }, { 0x0F86, 0x0F87 }, { 0x0F90, 0x0F97 },
		{ 0x0F99, 0x0FBC }, { 0x0FC6, 0x0FC6 }, { 0x102D, 0x1030 },
		{ 0x1032, 0x1032 }, { 0x1036, 0x1037 }, { 0x1039, 0x1039 },
		{ 0x1058, 0x1059 }, { 0x1160, 0x11FF }, { 0x1712, 0x1714 },
		{ 0x1732, 0x1734 }, { 0x1752, 0x1753 }, { 0x1772, 0x1773 },
		{ 0x17B4, 0x17B5 }, { 0x17B7, 0x17BD }, { 0x17C6, 0x17C6 },
		{ 0x17C9, 0x17D3 }, { 0x17DD, 0x17DD }, { 0x180B, 0x180D },
		{ 0x18A9, 0x18A9 }, { 0x1920, 0x1922 }, { 0x1927, 0x1928 },
		{ 0x1932, 0x1932 }, { 0x1939, 0x193B }, { 0x200B, 0x200F },
		{ 0x202A, 0x202E }, { 0x2060, 0x2063 }, { 0x206A, 0x206F },
		{ 0x20D0, 0x20EA }, { 0x302A, 0x302F }, { 0x3099, 0x309A },
		{ 0xFB1E, 0xFB1E }, { 0xFE00, 0xFE0F }, { 0xFE20, 0xFE23 },
		{ 0xFEFF, 0xFEFF }, { 0xFFF9, 0xFFFB }, { 0x1D167, 0x1D169 },
		{ 0x1D173, 0x1D182 }, { 0x1D185, 0x1D18B },
		{ 0x1D1AA, 0x1D1AD }, { 0xE0001, 0xE0001 },
		{ 0xE0020, 0xE007F }, { 0xE0100, 0xE01EF }
	};

	/* test for 8-bit control characters */
	if (ch == 0)
		return 0;
	if (ch < 32 || (ch >= 0x7f && ch < 0xa0))
		return -1;

	/* binary search in table of non-spacing characters */
	if (bisearch(ch, combining, sizeof(combining)
				/ sizeof(struct interval) - 1))
		return 0;

	/*
	 * If we arrive here, ch is neither a combining nor a C0/C1
	 * control character.
	 */

	return 1 +
		(ch >= 0x1100 &&
                    /* Hangul Jamo init. consonants */
		 (ch <= 0x115f ||
		  ch == 0x2329 || ch == 0x232a ||
                  /* CJK ... Yi */
		  (ch >= 0x2e80 && ch <= 0xa4cf &&
		   ch != 0x303f) ||
		  /* Hangul Syllables */
		  (ch >= 0xac00 && ch <= 0xd7a3) ||
		  /* CJK Compatibility Ideographs */
		  (ch >= 0xf900 && ch <= 0xfaff) ||
		  /* CJK Compatibility Forms */
		  (ch >= 0xfe30 && ch <= 0xfe6f) ||
		  /* Fullwidth Forms */
		  (ch >= 0xff00 && ch <= 0xff60) ||
		  (ch >= 0xffe0 && ch <= 0xffe6) ||
		  (ch >= 0x20000 && ch <= 0x2fffd) ||
		  (ch >= 0x30000 && ch <= 0x3fffd)));
}

/*
 * Pick one ucs character starting from the location *start points at,
 * and return it, while updating the *start pointer to point at the
 * end of that character.  When remainder_p is not NULL, the location
 * holds the number of bytes remaining in the string that we are allowed
 * to pick from.  Otherwise we are allowed to pick up to the NUL that
 * would eventually appear in the string.  *remainder_p is also reduced
 * by the number of bytes we have consumed.
 *
 * If the string was not a valid UTF-8, *start pointer is set to NULL
 * and the return value is undefined.
 */
ucs_char_t pick_one_utf8_char(const char **start, size_t *remainder_p)
{
	unsigned char *s = (unsigned char *)*start;
	ucs_char_t ch;
	size_t remainder, incr;

	/*
	 * A caller that assumes NUL terminated text can choose
	 * not to bother with the remainder length.  We will
	 * stop at the first NUL.
	 */
	remainder = (remainder_p ? *remainder_p : 999);

	if (remainder < 1) {
		goto invalid;
	} else if (*s < 0x80) {
		/* 0xxxxxxx */
		ch = *s;
		incr = 1;
	} else if ((s[0] & 0xe0) == 0xc0) {
		/* 110XXXXx 10xxxxxx */
		if (remainder < 2 ||
		    (s[1] & 0xc0) != 0x80 ||
		    (s[0] & 0xfe) == 0xc0)
			goto invalid;
		ch = ((s[0] & 0x1f) << 6) | (s[1] & 0x3f);
		incr = 2;
	} else if ((s[0] & 0xf0) == 0xe0) {
		/* 1110XXXX 10Xxxxxx 10xxxxxx */
		if (remainder < 3 ||
		    (s[1] & 0xc0) != 0x80 ||
		    (s[2] & 0xc0) != 0x80 ||
		    /* overlong? */
		    (s[0] == 0xe0 && (s[1] & 0xe0) == 0x80) ||
		    /* surrogate? */
		    (s[0] == 0xed && (s[1] & 0xe0) == 0xa0) ||
		    /* U+FFFE or U+FFFF? */
		    (s[0] == 0xef && s[1] == 0xbf &&
		     (s[2] & 0xfe) == 0xbe))
			goto invalid;
		ch = ((s[0] & 0x0f) << 12) |
			((s[1] & 0x3f) << 6) | (s[2] & 0x3f);
		incr = 3;
	} else if ((s[0] & 0xf8) == 0xf0) {
		/* 11110XXX 10XXxxxx 10xxxxxx 10xxxxxx */
		if (remainder < 4 ||
		    (s[1] & 0xc0) != 0x80 ||
		    (s[2] & 0xc0) != 0x80 ||
		    (s[3] & 0xc0) != 0x80 ||
		    /* overlong? */
		    (s[0] == 0xf0 && (s[1] & 0xf0) == 0x80) ||
		    /* > U+10FFFF? */
		    (s[0] == 0xf4 && s[1] > 0x8f) || s[0] > 0xf4)
			goto invalid;
		ch = ((s[0] & 0x07) << 18) | ((s[1] & 0x3f) << 12) |
			((s[2] & 0x3f) << 6) | (s[3] & 0x3f);
		incr = 4;
	} else {
invalid:
		*start = NULL;
		return 0;
	}

	*start += incr;
	if (remainder_p)
		*remainder_p = remainder - incr;
	return ch;
}

/*
 * This function returns the number of columns occupied by the character
 * pointed to by the variable start. The pointer is updated to point at
 * the next character. When remainder_p is not NULL, it points at the
 * location that stores the number of remaining bytes we can use to pick
 * a character (see pick_one_utf8_char() above).
 */
int utf8_width(const char **start, size_t *remainder_p)
{
	ucs_char_t ch = pick_one_utf8_char(start, remainder_p);
	if (!*start)
		return 0;
	return git_wcwidth(ch);
}

/*
 * Returns the total number of columns required by a null-terminated
 * string, assuming that the string is utf8.  Returns strlen() instead
 * if the string does not look like a valid utf8 string.
 */
int utf8_strwidth(const char *string)
{
	int width = 0;
	const char *orig = string;

	while (1) {
		if (!string)
			return strlen(orig);
		if (!*string)
			return width;
		width += utf8_width(&string, NULL);
	}
}

int is_utf8(const char *text)
{
	while (*text) {
		if (*text == '\n' || *text == '\t' || *text == '\r') {
			text++;
			continue;
		}
		utf8_width(&text, NULL);
		if (!text)
			return 0;
	}
	return 1;
}

static inline void strbuf_write(struct strbuf *sb, const char *buf, int len)
{
	if (sb)
		strbuf_insert(sb, sb->len, buf, len);
	else
		fwrite(buf, len, 1, stdout);
}

static void print_spaces(struct strbuf *buf, int count)
{
	static const char s[] = "                    ";
	while (count >= sizeof(s)) {
		strbuf_write(buf, s, sizeof(s) - 1);
		count -= sizeof(s) - 1;
	}
	strbuf_write(buf, s, count);
}

static void strbuf_add_indented_text(struct strbuf *buf, const char *text,
				     int indent, int indent2)
{
	if (indent < 0)
		indent = 0;
	while (*text) {
		const char *eol = strchrnul(text, '\n');
		if (*eol == '\n')
			eol++;
		print_spaces(buf, indent);
		strbuf_write(buf, text, eol - text);
		text = eol;
		indent = indent2;
	}
}

/*
 * Wrap the text, if necessary. The variable indent is the indent for the
 * first line, indent2 is the indent for all other lines.
 * If indent is negative, assume that already -indent columns have been
 * consumed (and no extra indent is necessary for the first line).
 */
int strbuf_add_wrapped_text(struct strbuf *buf,
		const char *text, int indent, int indent2, int width)
{
	int w = indent, assume_utf8 = is_utf8(text);
	const char *bol = text, *space = NULL;

	if (width <= 0) {
		strbuf_add_indented_text(buf, text, indent, indent2);
		return 1;
	}

	if (indent < 0) {
		w = -indent;
		space = text;
	}

	for (;;) {
		char c = *text;
		if (!c || isspace(c)) {
			if (w < width || !space) {
				const char *start = bol;
				if (!c && text == start)
					return w;
				if (space)
					start = space;
				else
					print_spaces(buf, indent);
				strbuf_write(buf, start, text - start);
				if (!c)
					return w;
				space = text;
				if (c == '\t')
					w |= 0x07;
				else if (c == '\n') {
					space++;
					if (*space == '\n') {
						strbuf_write(buf, "\n", 1);
						goto new_line;
					}
					else if (!isalnum(*space))
						goto new_line;
					else
						strbuf_write(buf, " ", 1);
				}
				w++;
				text++;
			}
			else {
new_line:
				strbuf_write(buf, "\n", 1);
				text = bol = space + isspace(*space);
				space = NULL;
				w = indent = indent2;
			}
			continue;
		}
		if (assume_utf8)
			w += utf8_width(&text, NULL);
		else {
			w++;
			text++;
		}
	}
}

int print_wrapped_text(const char *text, int indent, int indent2, int width)
{
	return strbuf_add_wrapped_text(NULL, text, indent, indent2, width);
}

int is_encoding_utf8(const char *name)
{
	if (!name)
		return 1;
	if (!strcasecmp(name, "utf-8") || !strcasecmp(name, "utf8"))
		return 1;
	return 0;
}

/*
 * Given a buffer and its encoding, return it re-encoded
 * with iconv.  If the conversion fails, returns NULL.
 */
#ifndef NO_ICONV
#if defined(OLD_ICONV) || (defined(__sun__) && !defined(_XPG6))
	typedef const char * iconv_ibp;
#else
	typedef char * iconv_ibp;
#endif
char *reencode_string(const char *in, const char *out_encoding, const char *in_encoding)
{
	iconv_t conv;
	size_t insz, outsz, outalloc;
	char *out, *outpos;
	iconv_ibp cp;

	if (!in_encoding)
		return NULL;
	conv = iconv_open(out_encoding, in_encoding);
	if (conv == (iconv_t) -1)
		return NULL;
	insz = strlen(in);
	outsz = insz;
	outalloc = outsz + 1; /* for terminating NUL */
	out = xmalloc(outalloc);
	outpos = out;
	cp = (iconv_ibp)in;

	while (1) {
		size_t cnt = iconv(conv, &cp, &insz, &outpos, &outsz);

		if (cnt == -1) {
			size_t sofar;
			if (errno != E2BIG) {
				free(out);
				iconv_close(conv);
				return NULL;
			}
			/* insz has remaining number of bytes.
			 * since we started outsz the same as insz,
			 * it is likely that insz is not enough for
			 * converting the rest.
			 */
			sofar = outpos - out;
			outalloc = sofar + insz * 2 + 32;
			out = xrealloc(out, outalloc);
			outpos = out + sofar;
			outsz = outalloc - sofar - 1;
		}
		else {
			*outpos = '\0';
			break;
		}
	}
	iconv_close(conv);
	return out;
}
#endif
