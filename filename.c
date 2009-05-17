/*
 * Filename character set conversion
 */
#include "cache.h"
#include "dir.h"

#if defined(__x86_64__) || defined(__i386__)
#define FAST_UNALIGNED
#endif

/*
 * The "common" case that requires no conversion: all 7-bit ASCII.
 *
 * Return how many character were trivially converted, negative on
 * error (result wouldn't fit in buffer even trivially).
 */
static int convert_path_common(const char *path, int plen, char *result, int resultlen)
{
	int retval;

	if (plen+1 > resultlen)
		return -1;
	retval = 0;
#ifdef FAST_UNALIGNED
	while (plen >= sizeof(unsigned long)) {
		unsigned long x = *(unsigned long *)path;
		if (x & (unsigned long) 0x8080808080808080ull)
			break;
		*(unsigned long *)result = x;
		path += sizeof(unsigned long);
		result += sizeof(unsigned long);
		plen -= sizeof(unsigned long);
		retval += sizeof(unsigned long);
	}
#endif
	while (plen > 0) {
		unsigned char x = *path;
		if (x & 0x80)
			break;
		*result = x;
		path++;
		result++;
		plen--;
		retval++;
	}
	*result = 0;
	return retval;
}

int convert_path_to_git(const char *path, int plen, char *result, int resultlen)
{
	int retval;

	retval = convert_path_common(path, plen, result, resultlen);
	/* Absolute failure, or total success.. */
	if (retval < 0 || retval == plen)
		return retval;

	/* Skip the part we already did trivially */
	result += retval;
	path += retval;
	plen -= retval;

	/* This is where we should get fancy. Some day */
	memcpy(result, path, plen+1);
	return retval + plen;
}

int convert_git_to_path(const char *path, int plen, char *result, int resultlen)
{
	int retval;

	retval = convert_path_common(path, plen, result, resultlen);
	/* Absolute failure, or total success.. */
	if (retval < 0 || retval == plen)
		return retval;

	/* Skip the part we already did trivially */
	result += retval;
	path += retval;
	plen -= retval;

	/* This is where we should get fancy. Some day */
	memcpy(result, path, plen+1);
	return retval + plen;
}
