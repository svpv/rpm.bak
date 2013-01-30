/** \ingroup rpmio
 * \file rpmio/argv.c
 */

#include "system.h"

#include <stdlib.h>
#include <rpm/argv.h>
#include <rpm/rpmstring.h>

#include "debug.h"

void argvPrint(const char * msg, ARGV_const_t argv, FILE * fp)
{
    ARGV_const_t av;

    if (fp == NULL) fp = stderr;

    if (msg)
	fprintf(fp, "===================================== %s\n", msg);

    if (argv)
    for (av = argv; *av; av++)
	fprintf(fp, "%s\n", *av);

}

ARGV_t argvNew(void)
{
    ARGV_t argv = xcalloc(1, sizeof(*argv));
    return argv;
}

ARGI_t argiFree(ARGI_t argi)
{
    if (argi) {
	argi->nvals = 0;
	free(argi->vals);
	free(argi);
    }
    return NULL;
}

ARGV_t argvFree(ARGV_t argv)
{
    if (argv) {
	for (ARGV_t av = argv; *av; av++)
	    free(*av);
	free(argv);
    }
    return NULL;
}

int argiCount(ARGI_const_t argi)
{
    int nvals = 0;
    if (argi)
	nvals = argi->nvals;
    return nvals;
}

ARGint_t argiData(ARGI_const_t argi)
{
    ARGint_t vals = NULL;
    if (argi && argi->nvals > 0)
	vals = argi->vals;
    return vals;
}

int argvCount(ARGV_const_t argv)
{
    ARGV_const_t argv_start = argv;
    if (argv)
    while (*argv != NULL)
	argv++;
    return argv - argv_start;
}

ARGV_t argvData(ARGV_t argv)
{
    return argv;
}

int argvCmp(const void * a, const void * b)
{
    const char *astr = *(ARGV_t)a;
    const char *bstr = *(ARGV_t)b;
    return strcmp(astr, bstr);
}

int argvSort(ARGV_t argv, int (*compar)(const void *, const void *))
{
    if (compar == NULL)
	compar = argvCmp;
    qsort(argv, argvCount(argv), sizeof(*argv), compar);
    return 0;
}

ARGV_t argvSearch(ARGV_const_t argv, const char *val,
		int (*compar)(const void *, const void *))
{
    if (argv == NULL)
	return NULL;
    if (compar == NULL)
	compar = argvCmp;
    return bsearch(&val, argv, argvCount(argv), sizeof(*argv), compar);
}

int argiAdd(ARGI_t * argip, int ix, int val)
{
    ARGI_t argi;

    if (argip == NULL)
	return -1;
    if (*argip == NULL)
	*argip = xcalloc(1, sizeof(**argip));
    argi = *argip;
    if (ix < 0)
	ix = argi->nvals;
    if (ix >= argi->nvals) {
	argi->vals = xrealloc(argi->vals, (ix + 1) * sizeof(*argi->vals));
	memset(argi->vals + argi->nvals, 0,
		(ix - argi->nvals) * sizeof(*argi->vals));
	argi->nvals = ix + 1;
    }
    argi->vals[ix] = val;
    return 0;
}

int argvAdd(ARGV_t * argvp, const char *val)
{
    ARGV_t argv;
    int argc;

    if (argvp == NULL)
	return -1;
    argc = argvCount(*argvp);
    *argvp = xrealloc(*argvp, (argc + 1 + 1) * sizeof(**argvp));
    argv = *argvp;
    argv[argc++] = xstrdup(val);
    argv[argc  ] = NULL;
    return 0;
}

int argvAddNum(ARGV_t *argvp, int val)
{
    char *valstr = NULL;
    int rc;
    rasprintf(&valstr, "%d", val);
    rc = argvAdd(argvp, valstr);
    free(valstr);
    return rc;
}

int argvAppend(ARGV_t * argvp, ARGV_const_t av)
{
    ARGV_t argv = *argvp;
    int argc = argvCount(argv);
    int ac = argvCount(av);
    int i;

    argv = xrealloc(argv, (argc + ac + 1) * sizeof(*argv));
    for (i = 0; i < ac; i++)
	argv[argc + i] = xstrdup(av[i]);
    argv[argc + ac] = NULL;
    *argvp = argv;
    return 0;
}

ARGV_t argvSplitString(const char * str, const char * seps, argvFlags flags)
{
    char *dest = NULL;
    ARGV_t argv;
    int argc = 1;
    const char * s;
    char * t;
    int c;

    if (str == NULL || seps == NULL)
	return NULL;

    dest = xmalloc(strlen(str) + 1);
    for (argc = 1, s = str, t = dest; (c = *s); s++, t++) {
	if (strchr(seps, c)) {
	    argc++;
	    c = '\0';
	}
	*t = c;
    }
    *t = '\0';

    argv = xmalloc( (argc + 1) * sizeof(*argv));

    for (c = 0, s = dest; s < t; s+= strlen(s) + 1) {
	if (*s == '\0' && (flags & ARGV_SKIPEMPTY))
	    continue;
	argv[c] = xstrdup(s);
	c++;
    }
    argv[c] = NULL;
    free(dest);
    return argv;
}

/* Backwards compatibility */
int argvSplit(ARGV_t * argvp, const char * str, const char * seps)
{
    if (argvp) {
	*argvp = argvSplitString(str, seps, ARGV_SKIPEMPTY);
    }
    return 0;
}

char *argvJoin(ARGV_const_t argv, const char *sep)
{
    int argc = argvCount(argv);
    if (argc < 1)
	return NULL;

    /* calculate lengths */
    size_t lenbuf[32], *lens = lenbuf;
    if (argc > 32)
	lens = xmalloc(argc * sizeof(*lens));
    size_t argvlen = 0;
    for (int i = 0; i < argc; i++) {
	size_t len = strlen(argv[i]);
	lens[i] = len;
	argvlen += len;
    }

    /* alloate destination buffer */
    size_t seplen = sep ? strlen(sep) : 0;
    char *dest = xmalloc(argvlen + seplen * (argc - 1) + 1);

    /* first element */
    size_t len = lens[0];
    char *p = memcpy(dest, argv[0], len);
    p += len;

    /* remaining elements, with separators */
    switch (seplen) {
    case 0:
	for (int i = 1; i < argc; i++) {
	    len = lens[i];
	    memcpy(p, argv[i], len);
	    p += len;
	}
	break;
    case 1:
	for (int i = 1; i < argc; i++) {
	    *p++ = *sep;
	    len = lens[i];
	    memcpy(p, argv[i], len);
	    p += len;
	}
	break;
    default:
	for (int i = 1; i < argc; i++) {
	    memcpy(p, sep, seplen);
	    p += seplen;
	    len = lens[i];
	    memcpy(p, argv[i], len);
	    p += len;
	}
	break;
    }

    *p = '\0';
    if (lens != lenbuf)
	free(lens);
    return dest;
}
