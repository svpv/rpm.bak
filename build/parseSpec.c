/** \ingroup rpmbuild
 * \file build/parseSpec.c
 *  Top level dispatcher for spec file parsing.
 */

#include "system.h"

#include <errno.h>

#include <rpm/rpmtypes.h>
#include <rpm/rpmlib.h>		/* RPM_MACHTABLE & related */
#include <rpm/rpmds.h>
#include <rpm/rpmts.h>
#include <rpm/rpmlog.h>
#include <rpm/rpmfileutil.h>
#include "build/rpmbuild_internal.h"
#include "build/rpmbuild_misc.h"
#include "debug.h"
#include <libgen.h>

#define SKIPSPACE(s) { while (*(s) && risspace(*(s))) (s)++; }
#define SKIPNONSPACE(s) { while (*(s) && !risspace(*(s))) (s)++; }
#define ISMACRO(s,m) (rstreqn((s), (m), sizeof((m))-1) && !risalpha((s)[sizeof((m))-1]))
#define ISMACROWITHARG(s,m) (rstreqn((s), (m), sizeof((m))-1) && (risblank((s)[sizeof((m))-1]) || !(s)[sizeof((m))-1]))

#define LEN_AND_STR(_tag) (sizeof(_tag)-1), (_tag)

typedef struct OpenFileInfo {
    char * fileName;
    FILE *fp;
    int lineNum;
    char readBuf[BUFSIZ];
    const char * readPtr;
    struct OpenFileInfo * next;
} OFI_t;

static const struct PartRec {
    int part;
    size_t len;
    const char * token;
} partList[] = {
    { PART_PREAMBLE,      LEN_AND_STR("%package")},
    { PART_PREP,          LEN_AND_STR("%prep")},
    { PART_BUILD,         LEN_AND_STR("%build")},
    { PART_INSTALL,       LEN_AND_STR("%install")},
    { PART_CHECK,         LEN_AND_STR("%check")},
    { PART_CLEAN,         LEN_AND_STR("%clean")},
    { PART_PREUN,         LEN_AND_STR("%preun")},
    { PART_POSTUN,        LEN_AND_STR("%postun")},
    { PART_PRETRANS,      LEN_AND_STR("%pretrans")},
    { PART_POSTTRANS,     LEN_AND_STR("%posttrans")},
    { PART_PRE,           LEN_AND_STR("%pre")},
    { PART_POST,          LEN_AND_STR("%post")},
    { PART_FILES,         LEN_AND_STR("%files")},
    { PART_CHANGELOG,     LEN_AND_STR("%changelog")},
    { PART_DESCRIPTION,   LEN_AND_STR("%description")},
    { PART_TRIGGERPOSTUN, LEN_AND_STR("%triggerpostun")},
    { PART_TRIGGERPREIN,  LEN_AND_STR("%triggerprein")},
    { PART_TRIGGERUN,     LEN_AND_STR("%triggerun")},
    { PART_TRIGGERIN,     LEN_AND_STR("%triggerin")},
    { PART_TRIGGERIN,     LEN_AND_STR("%trigger")},
    { PART_VERIFYSCRIPT,  LEN_AND_STR("%verifyscript")},
    { PART_POLICIES,      LEN_AND_STR("%sepolicy")},
    {0, 0, 0}
};

static int isPartToken(const char *line, int wb)
{
    const struct PartRec *p;

    if (*line != '%')
	return PART_NONE;

    for (p = partList; p->token != NULL; p++) {
	char c;
	if (rstrncasecmp(line, p->token, p->len))
	    continue;
	c = *(line + p->len);
	if (c == '\0' || risspace(c))
	    break;
	if (wb && !(risalnum(c) || c == '_'))
	    break;
    }

    return (p->token ? p->part : PART_NONE);
}

int isPart(const char *line)
{
    return isPartToken(line, 0);
}

/* Check for conditional/include directives */
static int isCond(const char *s)
{
    if (ISMACROWITHARG(s, "%if") ||
	ISMACROWITHARG(s, "%ifarch") ||
	ISMACROWITHARG(s, "%ifnarch") ||
	ISMACROWITHARG(s, "%ifos") ||
	ISMACROWITHARG(s, "%ifnos"))
	    return 1;
    if (ISMACRO(s, "%else") ||
	ISMACRO(s, "%endif") ||
	ISMACRO(s, "%include"))
	    return 1;
    return 0;
}

/* Check for %prep specials */
static int isPrep(const char *s)
{
    if (rstreqn(s, "%setup", sizeof("%setup") - 1) ||
	rstreqn(s, "%patch", sizeof("%patch") - 1))
	    return 1;
    return 0;
}

/* Handle undefined macros */
static void undefined(const char *fileName, int lineNum,
	const char *s, const char *f, const char *fe,
	const char *exp, int level, void *arg)
{
    rpmSpec spec = arg;
    assert(spec);

    /* effective part */
    int part = spec->parsePart;

    /* skip concatenated lines */
    const char *e;
    while ((e = strchr(exp, '\n')) != NULL) {
	int p = isPart(exp);
	if (p != PART_NONE)
	    part = p;
	exp = e + 1;
    }

    /* too much noise about comments */
    e = exp;
    SKIPSPACE(e);
    if (*e == '#')
	return;

    /* whether s is a simple %token */
    int token = (s + 1 == f);

    /* token at line start */
    if (token && *exp == '\0') {
	/* next part */
	if (isPart(s))
	    return;
	/* %prep special */
	if (part == PART_PREP && isPrep(s))
	    return;
    }

    /* conditional, possibly indented */
    if (token && isCond(s)) {
	e = exp;
	SKIPSPACE(e);
	if (*e == '\0')
	    return;
    }

    /* treat the rest of %part line as preamble */
    if (isPart(exp))
	part = PART_PREAMBLE;

    /* permit some tokens in some sections */
    switch (part) {
    case PART_CHANGELOG:
	if (token && isPartToken(s, 1))
	    return;
	/* fallthrough */
    case PART_PREP:
    case PART_BUILD:
    case PART_INSTALL:
    case PART_CHECK:
    case PART_CLEAN:
    case PART_FILES:
	if (token && isFileAttr(s))
	    return;
	break;
    }

    /* warning vs error */
    int rc = 0;
    switch (part) {
    case PART_PREAMBLE:
    case PART_PRE:
    case PART_POST:
    case PART_PREUN:
    case PART_POSTUN:
    case PART_PRETRANS:
    case PART_POSTTRANS:
    case PART_TRIGGERPREIN:
    case PART_TRIGGERIN:
    case PART_TRIGGERUN:
    case PART_TRIGGERPOSTUN:
	if (!(spec->flags & RPMSPEC_FORCE))
	    rc = 1;
	break;
    }

    /* XXX tolerate sloppy %global usage in preamble */
    if (part == PART_PREAMBLE && level > 1)
        rc = 0;

    /* wages of sin */
    rpmlog(rc ? RPMLOG_ERR : RPMLOG_WARNING,
	    _("%s:%d: Undefined macro %%%.*s\n"),
	    fileName, lineNum, (int)(fe - f), f);
    spec->errors += rc;
}

/**
 */
static int matchTok(const char *token, const char *line)
{
    const char *b, *be = line;
    size_t toklen = strlen(token);
    int rc = 0;

    while ( *(b = be) != '\0' ) {
	SKIPSPACE(b);
	be = b;
	SKIPNONSPACE(be);
	if (be == b)
	    break;
	if (toklen != (be-b) || rstrncasecmp(token, b, (be-b)))
	    continue;
	rc = 1;
	break;
    }

    return rc;
}

void handleComments(char *s)
{
    SKIPSPACE(s);
    if (*s == '#')
	*s = '\0';
}

/* Push a file to spec's file stack, return the newly pushed entry */
static OFI_t * pushOFI(rpmSpec spec, const char *fn)
{
    OFI_t *ofi = xcalloc(1, sizeof(*ofi));

    ofi->fp = NULL;
    ofi->fileName = xstrdup(fn);
    ofi->lineNum = 0;
    ofi->readBuf[0] = '\0';
    ofi->readPtr = NULL;
    ofi->next = spec->fileStack;

    spec->fileStack = ofi;
    return spec->fileStack;
}

/* Pop from spec's file stack */
static OFI_t * popOFI(rpmSpec spec)
{
    if (spec->fileStack) {
	OFI_t * ofi = spec->fileStack;

	spec->fileStack = ofi->next;
	if (ofi->fp)
	    fclose(ofi->fp);
	free(ofi->fileName);
	free(ofi);
    }
    return spec->fileStack;
}

static int restoreFirstChar(rpmSpec spec)
{
    /* Restore 1st char in (possible) next line */
    if (spec->nextline != NULL && spec->nextpeekc != '\0') {
	*spec->nextline = spec->nextpeekc;
	spec->nextpeekc = '\0';
	return 1;
    }
    return 0;
}

/* Return zero on success, 1 if we need to read more and -1 on errors. */
static int copyNextLineFromOFI(rpmSpec spec, OFI_t *ofi)
{
    /* Expand next line from file into line buffer */
    if (!(spec->nextline && *spec->nextline)) {
	const char *from = ofi->readPtr;
	const char *end = strchr(from, '\n');
	size_t len;
	if (end == NULL) {
	    len = strlen(from);
	    end = from + len;
	}
	else {
	    end++; /* grab newline */
	    len = end - from;
	}
	if (len + 1 > spec->lbufSize - spec->lbufOff) {
	    spec->lbufSize += BUFSIZ + len + 1;
	    spec->lbuf = xrealloc(spec->lbuf, spec->lbufSize);
	}
	memcpy(spec->lbuf + spec->lbufOff, from, len);
	spec->lbufOff += len;
	spec->lbuf[spec->lbufOff] = '\0';
	ofi->readPtr = end;

	/* Check if we need another line before expanding the buffer. */
	int pc = 0, bc = 0, nc = 0;
	/* Also check if we need to expand macros */
	int needExpand = 0;
	for (const char *p = spec->lbuf, *set = "\\%";
		(p = strpbrk(p, set)) != NULL; p++)
	    switch (*p) {
		case '\\':
		    switch (*(p+1)) {
			case '\n': p++, nc = 1; set = "\\\n%{}()"; break;
			case '\0': break;
			case '%': needExpand = 1; break;
			default: p++; break;
		    }
		    break;
		case '\n': nc = 0; break;
		case '%':
		    needExpand = 1;
		    switch (*(p+1)) {
			case '{': p++, bc++; set = "\\\n%{}()"; break;
			case '(': p++, pc++; set = "\\\n%{}()"; break;
			case '%': p++; break;
		    }
		    break;
		case '{': if (bc > 0) bc++; break;
		case '}': if (bc > 0) bc--; break;
		case '(': if (pc > 0) pc++; break;
		case ')': if (pc > 0) pc--; break;
	    }
	
	/* If it doesn't, ask for one more line. */
	if (pc || bc || nc ) {
	    spec->nextline = "";
	    return 1;
	}
	spec->lbufOff = 0;

	/* Don't expand macros (eg. %define) in false branch of %if clause */
	if (needExpand && spec->readStack->reading) {
	    char *exp = rpmExpandMacros(spec->macros, spec->lbuf,
				basename(ofi->fileName), ofi->lineNum,
				undefined, spec);
	    if (exp == NULL)
		return -1;
	    size_t len = strlen(exp);
	    if (len < spec->lbufSize) {
		memcpy(spec->lbuf, exp, len + 1);
		free(exp);
	    } else {
		free(spec->lbuf);
		spec->lbuf = exp;
		spec->lbufSize = len + 1;
	    }
	}
	spec->nextline = spec->lbuf;
    }
    return 0;
}

/* Logical chunking into lines after macro expansion */
static void copyNextLineFinish(rpmSpec spec, int strip)
{
    /* This line is to be processed */
    spec->line = spec->nextline;

    /* Next line is after newline character */
    char *end = strchr(spec->line, '\n');
    if (end == NULL) {
	/* Last line */
	size_t len = strlen(spec->line);
	end = spec->line + len;
	spec->nextline = end;
    }
    else {
	spec->nextline = end + 1;
	/* Save 1st char of next line in order to terminate current line */
	spec->nextpeekc = *spec->nextline;
	*spec->nextline = '\0';
    }
    
    if (strip & STRIP_COMMENTS)
	handleComments(spec->line);
    
    if (strip & STRIP_TRAILINGSPACE) {
	while (end > spec->line && risspace(end[-1]))
	    end--;
	*end = '\0';
    }
}

static int readLineFromOFI(rpmSpec spec, OFI_t *ofi)
{
retry:
    /* Make sure the current file is open */
    if (ofi->fp == NULL) {
	ofi->fp = fopen(ofi->fileName, "r");
	if (ofi->fp == NULL) {
	    rpmlog(RPMLOG_ERR, _("Unable to open %s: %s\n"),
		     ofi->fileName, strerror(errno));
	    return PART_ERROR;
	}
	spec->lineNum = ofi->lineNum = 0;
    }

    /* Make sure we have something in the read buffer */
    if (!(ofi->readPtr && *(ofi->readPtr))) {
	if (!fgets(ofi->readBuf, BUFSIZ, ofi->fp)) {
	    /* EOF, remove this file from the stack */
	    ofi = popOFI(spec);

	    /* only on last file do we signal EOF to caller */
	    if (ofi == NULL)
		return 1;

	    /* otherwise, go back and try the read again. */
	    goto retry;
	}
	ofi->readPtr = ofi->readBuf;
	ofi->lineNum++;
	spec->lineNum = ofi->lineNum;
    }
    return 0;
}

#define ARGMATCH(s,token,match) \
do { \
    char *os = s; \
    char *exp = rpmExpand(token, NULL); \
    while(*s && !risblank(*s)) s++; \
    while(*s && risblank(*s)) s++; \
    if (!*s) { \
	rpmlog(RPMLOG_ERR, _("%s:%d: Argument expected for %s\n"), ofi->fileName, ofi->lineNum, os); \
	free(exp); \
	return PART_ERROR; \
    } \
    match = matchTok(exp, s); \
    free(exp); \
} while (0)


int readLine(rpmSpec spec, int strip)
{
    char *s;
    int match;
    struct ReadLevelEntry *rl;
    OFI_t *ofi = spec->fileStack;
    int rc;
    int startLine = 0;

    if (!restoreFirstChar(spec)) {
    retry:
	if ((rc = readLineFromOFI(spec, ofi)) != 0) {
	    if (spec->readStack->next) {
		rpmlog(RPMLOG_ERR, _("line %d: Unclosed %%if\n"),
			spec->readStack->lineNum);
		rc = PART_ERROR;
	    } else if (startLine > 0) {
		rpmlog(RPMLOG_ERR,
		    _("line %d: unclosed macro or bad line continuation\n"),
		    startLine);
		rc = PART_ERROR;
	    }
	    return rc;
	}
	ofi = spec->fileStack;

	/* Copy next file line into the spec line buffer */
	rc = copyNextLineFromOFI(spec, ofi);
	if (rc > 0) {
	    if (startLine == 0)
		startLine = spec->lineNum;
	    goto retry;
	} else if (rc < 0) {
	    return PART_ERROR;
	}
    }

    copyNextLineFinish(spec, strip);

    s = spec->line;
    SKIPSPACE(s);

    match = -1;
    if (!spec->readStack->reading && ISMACROWITHARG(s, "%if")) {
	match = 0;
    } else if (ISMACROWITHARG(s, "%ifarch")) {
	ARGMATCH(s, "%{_target_cpu}", match);
    } else if (ISMACROWITHARG(s, "%ifnarch")) {
	ARGMATCH(s, "%{_target_cpu}", match);
	match = !match;
    } else if (ISMACROWITHARG(s, "%ifos")) {
	ARGMATCH(s, "%{_target_os}", match);
    } else if (ISMACROWITHARG(s, "%ifnos")) {
	ARGMATCH(s, "%{_target_os}", match);
	match = !match;
    } else if (ISMACROWITHARG(s, "%if")) {
	s += 3;
        match = parseExpressionBoolean(spec, s);
	if (match < 0) {
	    rpmlog(RPMLOG_ERR,
			_("%s:%d: bad %%if condition\n"),
			ofi->fileName, ofi->lineNum);
	    return PART_ERROR;
	}
    } else if (ISMACRO(s, "%else")) {
	if (! spec->readStack->next) {
	    /* Got an else with no %if ! */
	    rpmlog(RPMLOG_ERR,
			_("%s:%d: Got a %%else with no %%if\n"),
			ofi->fileName, ofi->lineNum);
	    return PART_ERROR;
	}
	spec->readStack->reading =
	    spec->readStack->next->reading && ! spec->readStack->reading;
	spec->line[0] = '\0';
    } else if (ISMACRO(s, "%endif")) {
	if (! spec->readStack->next) {
	    /* Got an end with no %if ! */
	    rpmlog(RPMLOG_ERR,
			_("%s:%d: Got a %%endif with no %%if\n"),
			ofi->fileName, ofi->lineNum);
	    return PART_ERROR;
	}
	rl = spec->readStack;
	spec->readStack = spec->readStack->next;
	free(rl);
	spec->line[0] = '\0';
    } else if (spec->readStack->reading && ISMACROWITHARG(s, "%include")) {
	char *fileName, *endFileName, *p;

	fileName = s+8;
	SKIPSPACE(fileName);
	endFileName = fileName;
	SKIPNONSPACE(endFileName);
	p = endFileName;
	SKIPSPACE(p);
	if (*fileName == '\0' || *p != '\0') {
	    rpmlog(RPMLOG_ERR, _("%s:%d: malformed %%include statement\n"),
				ofi->fileName, ofi->lineNum);
	    return PART_ERROR;
	}
	*endFileName = '\0';

	ofi = pushOFI(spec, fileName);
	goto retry;
    }

    if (match != -1) {
	rl = xmalloc(sizeof(*rl));
	rl->reading = spec->readStack->reading && match;
	rl->next = spec->readStack;
	rl->lineNum = ofi->lineNum;
	spec->readStack = rl;
	spec->line[0] = '\0';
    }

    if (! spec->readStack->reading) {
	spec->line[0] = '\0';
    }

    /* Collect parsed line */
    if (spec->parsed == NULL)
	spec->parsed = newStringBuf();
    appendStringBufAux(spec->parsed, spec->line,(strip & STRIP_TRAILINGSPACE));

    /* FIX: spec->readStack->next should be dependent */
    return 0;
}

void closeSpec(rpmSpec spec)
{
    while (popOFI(spec)) {};
}

static const rpmTagVal sourceTags[] = {
    RPMTAG_NAME,
    RPMTAG_VERSION,
    RPMTAG_RELEASE,
    RPMTAG_EPOCH,
    RPMTAG_SUMMARY,
    RPMTAG_DESCRIPTION,
    RPMTAG_PACKAGER,
    RPMTAG_DISTRIBUTION,
    RPMTAG_DISTURL,
    RPMTAG_VENDOR,
    RPMTAG_LICENSE,
    RPMTAG_GROUP,
    RPMTAG_OS,
    RPMTAG_ARCH,
    RPMTAG_CHANGELOGTIME,
    RPMTAG_CHANGELOGNAME,
    RPMTAG_CHANGELOGTEXT,
    RPMTAG_URL,
    RPMTAG_BUGURL,
    RPMTAG_HEADERI18NTABLE,
    0
};

static void initSourceHeader(rpmSpec spec)
{
    struct Source *srcPtr;

    if (spec->sourceHeader)
	return;

    spec->sourceHeader = headerNew();
    /* Only specific tags are added to the source package header */
    headerCopyTags(spec->packages->header, spec->sourceHeader, sourceTags);

    /* Add the build restrictions */
    {
	HeaderIterator hi = headerInitIterator(spec->buildRestrictions);
	struct rpmtd_s td;
	while (headerNext(hi, &td)) {
	    if (rpmtdCount(&td) > 0) {
		(void) headerPut(spec->sourceHeader, &td, HEADERPUT_DEFAULT);
	    }
	    rpmtdFreeData(&td);
	}
	headerFreeIterator(hi);
    }

    if (spec->BANames && spec->BACount > 0) {
	headerPutStringArray(spec->sourceHeader, RPMTAG_BUILDARCHS,
		  spec->BANames, spec->BACount);
    }

    /* Add tags for sources and patches */
    for (srcPtr = spec->sources; srcPtr != NULL; srcPtr = srcPtr->next) {
	if (srcPtr->flags & RPMBUILD_ISSOURCE) {
	    headerPutString(spec->sourceHeader, RPMTAG_SOURCE, srcPtr->source);
	    if (srcPtr->flags & RPMBUILD_ISNO) {
		headerPutUint32(spec->sourceHeader, RPMTAG_NOSOURCE,
				&srcPtr->num, 1);
	    }
	}
	if (srcPtr->flags & RPMBUILD_ISPATCH) {
	    headerPutString(spec->sourceHeader, RPMTAG_PATCH, srcPtr->source);
	    if (srcPtr->flags & RPMBUILD_ISNO) {
		headerPutUint32(spec->sourceHeader, RPMTAG_NOPATCH,
				&srcPtr->num, 1);
	    }
	}
    }
}

/* Add extra provides to package.  */
static void addPackageProvides(Header h)
{
    const char *arch, *name;
    char *evr, *isaprov;
    rpmsenseFlags pflags = RPMSENSE_EQUAL;

    /* <name> = <evr> provide */
    name = headerGetString(h, RPMTAG_NAME);
    arch = headerGetString(h, RPMTAG_ARCH);
    evr = headerGetAsString(h, RPMTAG_EVR);
    headerPutString(h, RPMTAG_PROVIDENAME, name);
    headerPutString(h, RPMTAG_PROVIDEVERSION, evr);
    headerPutUint32(h, RPMTAG_PROVIDEFLAGS, &pflags, 1);

    /*
     * <name>(<isa>) = <evr> provide
     * FIXME: noarch needs special casing for now as BuildArch: noarch doesn't
     * cause reading in the noarch macros :-/ 
     */
    isaprov = rpmExpand(name, "%{?_isa}", NULL);
    if (!rstreq(arch, "noarch") && !rstreq(name, isaprov)) {
	headerPutString(h, RPMTAG_PROVIDENAME, isaprov);
	headerPutString(h, RPMTAG_PROVIDEVERSION, evr);
	headerPutUint32(h, RPMTAG_PROVIDEFLAGS, &pflags, 1);
    }
    free(isaprov);
    free(evr);
}

static void addTargets(Package Pkgs)
{
    char *platform = rpmExpand("%{_target_platform}", NULL);
    char *arch = rpmExpand("%{_target_cpu}", NULL);
    char *os = rpmExpand("%{_target_os}", NULL);
    char *optflags = rpmExpand("%{optflags}", NULL);

    for (Package pkg = Pkgs; pkg != NULL; pkg = pkg->next) {
	headerPutString(pkg->header, RPMTAG_OS, os);
	/* noarch subpackages already have arch set here, leave it alone */
	if (!headerIsEntry(pkg->header, RPMTAG_ARCH)) {
	    headerPutString(pkg->header, RPMTAG_ARCH, arch);
	}
	headerPutString(pkg->header, RPMTAG_PLATFORM, platform);
	headerPutString(pkg->header, RPMTAG_OPTFLAGS, optflags);

	pkg->ds = rpmdsThis(pkg->header, RPMTAG_REQUIRENAME, RPMSENSE_EQUAL);
	addPackageProvides(pkg->header);
    }
    free(platform);
    free(arch);
    free(os);
    free(optflags);
}

static rpmSpec parseSpec(const char *specFile, rpmSpecFlags flags,
			 const char *buildRoot, int recursing)
{
    int initialPackage = 1;
    rpmSpec spec;
    
    /* Set up a new Spec structure with no packages. */
    spec = newSpec();
#define parsePart (spec->parsePart)
    parsePart = PART_PREAMBLE;

    spec->specFile = rpmGetPath(specFile, NULL);
    pushOFI(spec, spec->specFile);
    /* If buildRoot not specified, use default %{buildroot} */
    if (buildRoot) {
	spec->buildRoot = xstrdup(buildRoot);
    } else {
	spec->buildRoot = rpmGetPath("%{?buildroot:%{buildroot}}", NULL);
    }
    addMacro(NULL, "_docdir", NULL, "%{_defaultdocdir}", RMIL_SPEC);
    addMacro(NULL, "_licensedir", NULL, "%{_defaultlicensedir}", RMIL_SPEC);
    spec->recursing = recursing;
    spec->flags = flags;

    /* All the parse*() functions expect to have a line pre-read */
    /* in the spec's line buffer.  Except for parsePreamble(),   */
    /* which handles the initial entry into a spec file.         */
    
    while (parsePart != PART_NONE) {
	int goterror = 0;
	switch (parsePart) {
	case PART_ERROR: /* fallthrough */
	default:
	    goterror = 1;
	    break;
	case PART_PREAMBLE:
	    parsePart = parsePreamble(spec, initialPackage);
	    initialPackage = 0;
	    break;
	case PART_PREP:
	    parsePart = parsePrep(spec);
	    break;
	case PART_BUILD:
	case PART_INSTALL:
	case PART_CHECK:
	case PART_CLEAN:
	    parsePart = parseBuildInstallClean(spec, parsePart);
	    break;
	case PART_CHANGELOG:
	    parsePart = parseChangelog(spec);
	    break;
	case PART_DESCRIPTION:
	    parsePart = parseDescription(spec);
	    break;

	case PART_PRE:
	case PART_POST:
	case PART_PREUN:
	case PART_POSTUN:
	case PART_PRETRANS:
	case PART_POSTTRANS:
	case PART_VERIFYSCRIPT:
	case PART_TRIGGERPREIN:
	case PART_TRIGGERIN:
	case PART_TRIGGERUN:
	case PART_TRIGGERPOSTUN:
	    parsePart = parseScript(spec, parsePart);
	    break;

	case PART_FILES:
	    parsePart = parseFiles(spec);
	    break;

	case PART_POLICIES:
	    parsePart = parsePolicies(spec);
	    break;

	case PART_NONE:		/* XXX avoid gcc whining */
	case PART_LAST:
	case PART_BUILDARCHITECTURES:
	    break;
	}

	if (goterror || parsePart >= PART_LAST) {
	    goto errxit;
	}

	if (parsePart == PART_BUILDARCHITECTURES) {
	    int index;
	    int x;

	    closeSpec(spec);

	    spec->BASpecs = xcalloc(spec->BACount, sizeof(*spec->BASpecs));
	    index = 0;
	    if (spec->BANames != NULL)
	    for (x = 0; x < spec->BACount; x++) {

		/* Skip if not arch is not compatible. */
		if (!rpmMachineScore(RPM_MACHTABLE_BUILDARCH, spec->BANames[x]))
		    continue;
		addMacro(NULL, "_target_cpu", NULL, spec->BANames[x], RMIL_RPMRC);
		spec->BASpecs[index] = parseSpec(specFile, flags, buildRoot, 1);
		if (spec->BASpecs[index] == NULL) {
			spec->BACount = index;
			goto errxit;
		}
		delMacro(NULL, "_target_cpu");
		index++;
	    }

	    spec->BACount = index;
	    if (! index) {
		rpmlog(RPMLOG_ERR,
			_("No compatible architectures found for build\n"));
		goto errxit;
	    }

	    /*
	     * Return the 1st child's fully parsed Spec structure.
	     * The restart of the parse when encountering BuildArch
	     * causes problems for "rpm -q --specfile". This is
	     * still a hack because there may be more than 1 arch
	     * specified (unlikely but possible.) There's also the
	     * further problem that the macro context, particularly
	     * %{_target_cpu}, disagrees with the info in the header.
	     */
	    if (spec->BACount >= 1) {
		rpmSpec nspec = spec->BASpecs[0];
		spec->BASpecs = _free(spec->BASpecs);
		rpmSpecFree(spec);
		spec = nspec;
	    }

	    goto exit;
	}
    }

    if (spec->clean == NULL) {
	char *body = rpmExpand("%{?buildroot: %{__rm} -rf %{buildroot}}", NULL);
	spec->clean = newStringBuf();
	appendLineStringBuf(spec->clean, body);
	free(body);
    }

    /* Check for description in each package */
    for (Package pkg = spec->packages; pkg != NULL; pkg = pkg->next) {
	if (!headerIsEntry(pkg->header, RPMTAG_DESCRIPTION)) {
	    rpmlog(RPMLOG_ERR, _("Package has no %%description: %s\n"),
		   headerGetString(pkg->header, RPMTAG_NAME));
	    spec->errors++;
	}
    }

    if (spec->errors)
	goto errxit;

    /* Add arch, os and platform, self-provides etc for each package */
    addTargets(spec->packages);

    closeSpec(spec);
exit:
    /* Assemble source header from parsed components */
    initSourceHeader(spec);

    return spec;

errxit:
    rpmSpecFree(spec);
    return NULL;
}

rpmSpec rpmSpecParse(const char *specFile, rpmSpecFlags flags,
		     const char *buildRoot)
{
    return parseSpec(specFile, flags, buildRoot, 0);
}
