/*	$OpenBSD: conf.c,v 1.55 2003/06/03 14:28:16 ho Exp $	*/
/*	$EOM: conf.c,v 1.48 2000/12/04 02:04:29 angelos Exp $	*/

/*
 * Copyright (c) 1998, 1999, 2000, 2001 Niklas Hallqvist.  All rights reserved.
 * Copyright (c) 2000, 2001, 2002 H�kan Olsson.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * This code was written under funding by Ericsson Radio Systems.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/param.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <err.h>
#include <syslog.h>

#include "conffile.h"
#include "xlog.h"

#pragma GCC visibility push(hidden)

static void conf_load_defaults(void);
static char * conf_load(const char *path);
static int conf_set(int , const char *, const char *, const char *, 
	const char *, int , int );
static void conf_parse(int trans, char *buf, 
	char **section, char **subsection);

struct conf_trans {
	TAILQ_ENTRY (conf_trans) link;
	int trans;
	enum conf_op { CONF_SET, CONF_REMOVE, CONF_REMOVE_SECTION } op;
	char *section;
	char *arg;
	char *tag;
	char *value;
	int override;
	int is_default;
};

TAILQ_HEAD (conf_trans_head, conf_trans) conf_trans_queue;

/*
 * Radix-64 Encoding.
 */
#if 0
static const uint8_t bin2asc[]
  = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
#endif

static const uint8_t asc2bin[] =
{
  255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255, 255, 255, 255, 255, 255,
  255, 255, 255,  62, 255, 255, 255,  63,
   52,  53,  54,  55,  56,  57,  58,  59,
   60,  61, 255, 255, 255, 255, 255, 255,
  255,   0,   1,   2,   3,   4,   5,   6,
    7,   8,   9,  10,  11,  12,  13,  14,
   15,  16,  17,  18,  19,  20,  21,  22,
   23,  24,  25, 255, 255, 255, 255, 255,
  255,  26,  27,  28,  29,  30,  31,  32,
   33,  34,  35,  36,  37,  38,  39,  40,
   41,  42,  43,  44,  45,  46,  47,  48,
   49,  50,  51, 255, 255, 255, 255, 255
};

struct conf_binding {
  LIST_ENTRY (conf_binding) link;
  char *section;
  char *arg;
  char *tag;
  char *value;
  int is_default;
};

LIST_HEAD (conf_bindings, conf_binding) conf_bindings[256];

static __inline__ uint8_t
conf_hash(const char *s)
{
	uint8_t hash = 0;

	while (*s) {
		hash = ((hash << 1) | (hash >> 7)) ^ tolower (*s);
		s++;
	}
	return hash;
}

/*
 * Insert a tag-value combination from LINE (the equal sign is at POS)
 */
static int
conf_remove_now(const char *section, const char *tag)
{
	struct conf_binding *cb, *next;

	cb = LIST_FIRST(&conf_bindings[conf_hash (section)]);
	for (; cb; cb = next) {
		next = LIST_NEXT(cb, link);
		if (strcasecmp(cb->section, section) == 0
				&& strcasecmp(cb->tag, tag) == 0) {
			LIST_REMOVE(cb, link);
			xlog(LOG_INFO,"[%s]:%s->%s removed", section, tag, cb->value);
			free(cb->section);
			free(cb->arg);
			free(cb->tag);
			free(cb->value);
			free(cb);
			return 0;
		}
	}
	return 1;
}

static int
conf_remove_section_now(const char *section)
{
  struct conf_binding *cb, *next;
  int unseen = 1;

	cb = LIST_FIRST(&conf_bindings[conf_hash (section)]);
	for (; cb; cb = next) {
		next = LIST_NEXT(cb, link);
		if (strcasecmp(cb->section, section) == 0) {
			unseen = 0;
			LIST_REMOVE(cb, link);
			xlog(LOG_INFO, "[%s]:%s->%s removed", section, cb->tag, cb->value);
			free(cb->section);
			free(cb->arg);
			free(cb->tag);
			free(cb->value);
			free(cb);
			}
		}
	return unseen;
}

/*
 * Insert a tag-value combination from LINE (the equal sign is at POS)
 * into SECTION of our configuration database.
 */
static int
conf_set_now(const char *section, const char *arg, const char *tag, 
	const char *value, int override, int is_default)
{
	struct conf_binding *node = 0;

	if (override)
		conf_remove_now(section, tag);
	else if (conf_get_section(section, arg, tag)) {
		if (!is_default) {
			xlog(LOG_INFO, "conf_set: duplicate tag [%s]:%s, ignoring...\n", 
				section, tag);
		}
		return 1;
	}
	node = calloc(1, sizeof *node);
	if (!node) {
		xlog_warn("conf_set: calloc (1, %lu) failed", (unsigned long)sizeof *node);
		return 1;
	}
	node->section = strdup(section);
	if (arg)
		node->arg = strdup(arg);
	node->tag = strdup(tag);
	node->value = strdup(value);
	node->is_default = is_default;
	LIST_INSERT_HEAD(&conf_bindings[conf_hash (section)], node, link);
	return 0;
}

/*
 * Parse the line LINE of SZ bytes.  Skip Comments, recognize section
 * headers and feed tag-value pairs into our configuration database.
 */
static void
conf_parse_line(int trans, char *line, int lineno, char **section, char **subsection)
{
	char *val, *ptr;

	/* Ignore blank lines */
	if (*line == '\0')
		return;

	/* Strip off any leading blanks */
	while (isblank(*line)) 
		line++;

	/* Lines starting with '#' or ';' are comments.  */
	if (*line == '#' || *line == ';')
		return;

	/* '[section]' parsing...  */
	if (*line == '[') {
		line++;

		if (*section) {
			free(*section);
			*section = NULL;
		}
		if (*subsection) {
			free(*subsection);
			*subsection = NULL;
		}

		/* Strip off any blanks after '[' */
		while (isblank(*line)) 
			line++;

		/* find the closing ] */
		ptr = strchr(line, ']');
		if (ptr == NULL) {
			xlog_warn("config file error: line %d: "
 				"non-matched ']', ignoring until next section", lineno);
			return;
		}

		/* just ignore everything after the closing ] */
		*(ptr--) = '\0';

		/* Strip off any blanks before ']' */
		while (ptr >= line && isblank(*ptr)) 
			*(ptr--)='\0';

		/* look for an arg to split from the section name */
		val = strchr(line, '"');
		if (val != NULL) {
			ptr = val - 1;
			*(val++) = '\0';

			/* trim away any whitespace before the " */
			while (ptr > line && isblank(*ptr))
				*(ptr--)='\0';
		}

		/* copy the section name */
		*section = strdup(line);
		if (!*section) {
			xlog_warn("conf_parse_line: %d: malloc failed", lineno);
			return;
		}

		/* there is no arg, we are done */
		if (val == NULL) return;

		/* check for the closing " */
		ptr = strchr(val, '"');
		if (ptr == NULL) {
			xlog_warn("config file error: line %d: "
 				"non-matched '\"', ignoring until next section", lineno);
			return;
		}
		*ptr = '\0';
		*subsection = strdup(val);
		if (!*subsection) 
			xlog_warn("conf_parse_line: %d: malloc arg failed", lineno);
		return;
	}

	/* Deal with assignments.  */
	ptr = strchr(line, '=');

	/* not an assignment line */
	if (ptr == NULL) {
		/* Other non-empty lines are weird.  */
		if (line[strspn(line, " \t")])
			xlog_warn("config file error: line %d: "
				"line not empty and not an assignment", lineno);
		return;
	}

	/* If no section, we are ignoring the line.  */
	if (!*section) {
		xlog_warn("config file error: line %d: "
			"ignoring line due to no section", lineno);
		return;
	}

	val = ptr + 1;
	*(ptr--) = '\0';

	/* strip spaces before and after the = */
	while (ptr >= line && isblank(*ptr))
		*(ptr--)='\0';
	while (*val != '\0' && isblank(*val))
		val++;

	if (*val == '"') {
		val++;
		ptr = strchr(val, '"');
		if (ptr == NULL) {
			xlog_warn("config file error: line %d: "
				"unmatched quotes", lineno);
			return;
		}
		*ptr = '\0';
	} else
	if (*val == '\'') {
		val++;
		ptr = strchr(val, '\'');
		if (ptr == NULL) {
			xlog_warn("config file error: line %d: "
				"unmatched quotes", lineno);
			return;
		}
		*ptr = '\0';
	} else {
		/* Trim any trailing spaces and comments */
		if ((ptr=strchr(val, '#'))!=NULL)
			*ptr = '\0';
		if ((ptr=strchr(val, ';'))!=NULL)
			*ptr = '\0';

		ptr = val + strlen(val) - 1;
		while (ptr > val && isspace(*ptr))
			*(ptr--) = '\0';
	}

	if (*line == '\0') {
		xlog_warn("config file error: line %d: "
			"missing tag in assignment", lineno);
		return;
	}
	if (*val == '\0') {
		xlog_warn("config file error: line %d: "
			"missing value in assignment", lineno);
		return;
	}

	if (strcasecmp(line, "include")==0) {
		/* load and parse subordinate config files */
		char * subconf = conf_load(val);
		if (subconf == NULL) {
			xlog_warn("config file error: line %d: "
			"error loading included config", lineno);
			return;
		}

		/* copy the section data so the included file can inherit it
		 * without accidentally changing it for us */
		char * inc_section = NULL;
		char * inc_subsection = NULL;
		if (*section != NULL) {
			inc_section = strdup(*section);
			if (*subsection != NULL)
				inc_subsection = strdup(*subsection);
		}

		conf_parse(trans, subconf, &inc_section, &inc_subsection);

		if (inc_section) free(inc_section);
		if (inc_subsection) free(inc_subsection);
		free(subconf);
	} else {
		/* XXX Perhaps should we not ignore errors?  */
		conf_set(trans, *section, *subsection, line, val, 0, 0);
	}
}

/* Parse the mapped configuration file.  */
static void
conf_parse(int trans, char *buf, char **section, char **subsection)
{
	char *cp = buf;
	char *bufend = NULL;
	char *line;
	int lineno = 0;

	line = cp;
	bufend = buf + strlen(buf);
	while (cp < bufend) {
		if (*cp == '\n') {
			/* Check for escaped newlines.  */
			if (cp > buf && *(cp - 1) == '\\')
				*(cp - 1) = *cp = ' ';
			else {
				*cp = '\0';
				lineno++;
				conf_parse_line(trans, line, lineno, section, subsection);
				line = cp + 1;
			}
		}
		cp++;
	}
	if (cp != line)
		xlog_warn("conf_parse: last line non-terminated, ignored.");
}

static void
conf_load_defaults(void)
{
	/* No defaults */
	return;
}

static char *
conf_load(const char *path)
{
	struct stat sb;
	if ((stat (path, &sb) == 0) || (errno != ENOENT)) {
		char *new_conf_addr = NULL;
		size_t sz = sb.st_size;
		int fd = open (path, O_RDONLY, 0);

		if (fd == -1) {
			xlog_warn("conf_reinit: open (\"%s\", O_RDONLY) failed", path);
			return NULL;
		}

		new_conf_addr = malloc(sz+1);
		if (!new_conf_addr) {
			xlog_warn("conf_reinit: malloc (%lu) failed", (unsigned long)sz);
			goto fail;
		}

		/* XXX I assume short reads won't happen here.  */
		if (read (fd, new_conf_addr, sz) != (int)sz) {
			xlog_warn("conf_reinit: read (%d, %p, %lu) failed",
				fd, new_conf_addr, (unsigned long)sz);
			goto fail;
		}
		close(fd);

		/* XXX Should we not care about errors and rollback?  */
		new_conf_addr[sz] = '\0';
		return new_conf_addr;
	fail:
		close(fd);
		if (new_conf_addr) free(new_conf_addr);
	}
	return NULL;
}

/* remove and free up any existing config state */
static void conf_free_bindings(void)
{
	unsigned int i;
	for (i = 0; i < sizeof conf_bindings / sizeof conf_bindings[0]; i++) {
		struct conf_binding *cb, *next;

		cb = LIST_FIRST(&conf_bindings[i]);
		for (; cb; cb = next) {
			next = LIST_NEXT(cb, link);
			LIST_REMOVE(cb, link);
			free(cb->section);
			free(cb->arg);
			free(cb->tag);
			free(cb->value);
			free(cb);
		}
		LIST_INIT(&conf_bindings[i]);
	}
}

/* Open the config file and map it into our address space, then parse it.  */
static void
conf_reinit(const char *conf_file)
{
	int trans;
	char * conf_data;

	trans = conf_begin();
	conf_data = conf_load(conf_file);

	if (conf_data == NULL)
		return;

	/* Load default configuration values.  */
	conf_load_defaults();

	/* Parse config contents into the transaction queue */
	char *section = NULL;
	char *subsection = NULL;
	conf_parse(trans, conf_data, &section, &subsection);
	if (section) free(section);
	if (subsection) free(subsection);
	free(conf_data);

	/* Free potential existing configuration.  */
	conf_free_bindings();

	/* Apply the new configuration values */
	conf_end(trans, 1);
	return;
}

void
conf_init (const char *conf_file)
{
	unsigned int i;

	for (i = 0; i < sizeof conf_bindings / sizeof conf_bindings[0]; i++)
		LIST_INIT (&conf_bindings[i]);

	TAILQ_INIT (&conf_trans_queue);

	if (conf_file == NULL) conf_file=NFS_CONFFILE;
	conf_reinit(conf_file);
}

/* 
 * Empty the config and free up any used memory 
 */
void
conf_cleanup(void)
{
	conf_free_bindings();

	struct conf_trans *node, *next;
	for (node = TAILQ_FIRST(&conf_trans_queue); node; node = next) {
		next = TAILQ_NEXT(node, link);
		TAILQ_REMOVE (&conf_trans_queue, node, link);
		if (node->section) free(node->section);
		if (node->arg) free(node->arg);
		if (node->tag) free(node->tag);
		if (node->value) free(node->value);
		free (node);
	}
	TAILQ_INIT(&conf_trans_queue);
}

/*
 * Return the numeric value denoted by TAG in section SECTION or DEF
 * if that tag does not exist.
 */
int
conf_get_num(const char *section, const char *tag, int def)
{
	char *value = conf_get_str(section, tag);

	if (value)
		return atoi(value);

	return def;
}

/*
 * Return the Boolean value denoted by TAG in section SECTION, or DEF
 * if that tags does not exist.
 * FALSE is returned for case-insensitve comparisons with 0, f, false, n, no, off
 * TRUE is returned for 1, t, true, y, yes, on
 * A failure to match one of these results in DEF
 */
_Bool
conf_get_bool(const char *section, const char *tag, _Bool def)
{
	char *value = conf_get_str(section, tag);

	if (!value)
		return def;
	if (strcasecmp(value, "1") == 0 ||
	    strcasecmp(value, "t") == 0 ||
	    strcasecmp(value, "true") == 0 ||
	    strcasecmp(value, "y") == 0 ||
	    strcasecmp(value, "yes") == 0 ||
	    strcasecmp(value, "on") == 0)
		return true;

	if (strcasecmp(value, "0") == 0 ||
	    strcasecmp(value, "f") == 0 ||
	    strcasecmp(value, "false") == 0 ||
	    strcasecmp(value, "n") == 0 ||
	    strcasecmp(value, "no") == 0 ||
	    strcasecmp(value, "off") == 0)
		return false;
	return def;
}

/* Validate X according to the range denoted by TAG in section SECTION.  */
int
conf_match_num(const char *section, const char *tag, int x)
{
	char *value = conf_get_str (section, tag);
	int val, min, max, n;

	if (!value)
		return 0;
	n = sscanf (value, "%d,%d:%d", &val, &min, &max);
	switch (n) {
	case 1:
		xlog(LOG_INFO, "conf_match_num: %s:%s %d==%d?", section, tag, val, x);
		return x == val;
	case 3:
		xlog(LOG_INFO, "conf_match_num: %s:%s %d<=%d<=%d?", section, 
			tag, min, x, max);
		return min <= x && max >= x;
	default:
		xlog(LOG_INFO, "conf_match_num: section %s tag %s: invalid number spec %s",
			section, tag, value);
	}
	return 0;
}

/* Return the string value denoted by TAG in section SECTION.  */
char *
conf_get_str(const char *section, const char *tag)
{
	return conf_get_section(section, NULL, tag);
}

/* Return the string value denoted by TAG in section SECTION,
 * unless it is not set, in which case return def
 */
char *
conf_get_str_with_def(const char *section, const char *tag, char *def)
{
	char * result = conf_get_section(section, NULL, tag);
	if (!result) 
		return def;
	return result;
}

/*
 * Find a section that may or may not have an argument
 */
char *
conf_get_section(const char *section, const char *arg, const char *tag)
{
	struct conf_binding *cb;
retry:
	cb = LIST_FIRST (&conf_bindings[conf_hash (section)]);
	for (; cb; cb = LIST_NEXT (cb, link)) {
		if (strcasecmp(section, cb->section) != 0)
			continue;
		if (arg && strcasecmp(arg, cb->arg) != 0)
			continue;
		if (strcasecmp(tag, cb->tag) != 0)
			continue;
		if (cb->value[0] == '$') {
			/* expand $name from [environment] section,
			 * or from environment
			 */
			char *env = getenv(cb->value+1);
			if (env && *env)
				return env;
			section = "environment";
			tag = cb->value + 1;
			goto retry;
		}
		return cb->value;
	}
	return 0;
}

/*
 * Build a list of string values out of the comma separated value denoted by
 * TAG in SECTION.
 */
struct conf_list *
conf_get_list(const char *section, const char *tag)
{
	char *liststr = 0, *p, *field, *t;
	struct conf_list *list = 0;
	struct conf_list_node *node;

	list = malloc (sizeof *list);
	if (!list)
		goto cleanup;
	TAILQ_INIT (&list->fields);
	list->cnt = 0;
	liststr = conf_get_str(section, tag);
	if (!liststr)
		goto cleanup;
	liststr = strdup (liststr);
	if (!liststr)
		goto cleanup;
	p = liststr;
	while ((field = strsep (&p, ",")) != NULL) {
		/* Skip leading whitespace */
		while (isspace (*field))
			field++;
		/* Skip trailing whitespace */
		if (p) {
			for (t = p - 1; t > field && isspace (*t); t--)
				*t = '\0';
		}
		if (*field == '\0') {
			xlog(LOG_INFO, "conf_get_list: empty field, ignoring...");
			continue;
		}
		list->cnt++;
		node = calloc (1, sizeof *node);
		if (!node)
			goto cleanup;
		node->field = strdup (field);
		if (!node->field) {
			free(node);
			goto cleanup;
		}
		TAILQ_INSERT_TAIL (&list->fields, node, link);
	}
	free (liststr);
	return list;

cleanup:
	if (list)
		conf_free_list(list);
	if (liststr)
		free(liststr);
	return 0;
}

struct conf_list *
conf_get_tag_list(const char *section, const char *arg)
{
	struct conf_list *list = 0;
	struct conf_list_node *node;
	struct conf_binding *cb;

	list = malloc(sizeof *list);
	if (!list)
		goto cleanup;
	TAILQ_INIT(&list->fields);
	list->cnt = 0;
	cb = LIST_FIRST(&conf_bindings[conf_hash (section)]);
	for (; cb; cb = LIST_NEXT(cb, link)) {
		if (strcasecmp (section, cb->section) == 0) {
			if (arg != NULL && strcasecmp(arg, cb->arg) != 0)
				continue;
			list->cnt++;
			node = calloc(1, sizeof *node);
			if (!node)
				goto cleanup;
			node->field = strdup(cb->tag);
			if (!node->field) {
				free(node);
				goto cleanup;
			}
			TAILQ_INSERT_TAIL(&list->fields, node, link);
		}
	}
	return list;

cleanup:
	if (list)
		conf_free_list(list);
	return 0;
}

/* Decode a PEM encoded buffer.  */
int
conf_decode_base64 (uint8_t *out, uint32_t *len, const unsigned char *buf)
{
	uint32_t c = 0;
	uint8_t c1, c2, c3, c4;

	while (*buf) {
		if (*buf > 127 || (c1 = asc2bin[*buf]) == 255)
			return 0;

		buf++;
		if (*buf > 127 || (c2 = asc2bin[*buf]) == 255)
			return 0;

		buf++;
		if (*buf == '=') {
			c3 = c4 = 0;
			c++;

			/* Check last four bit */
			if (c2 & 0xF)
				return 0;

			if (strcmp((char *)buf, "==") == 0)
				buf++;
			else
				return 0;
		} else if (*buf > 127 || (c3 = asc2bin[*buf]) == 255)
			return 0;
		else {
			if (*++buf == '=') {
				c4 = 0;
				c += 2;

				/* Check last two bit */
				if (c3 & 3)
					return 0;

			if (strcmp((char *)buf, "="))
				return 0;
			} else if (*buf > 127 || (c4 = asc2bin[*buf]) == 255)
				return 0;
			else
				c += 3;
		}

		buf++;
		*out++ = (c1 << 2) | (c2 >> 4);
		*out++ = (c2 << 4) | (c3 >> 2);
		*out++ = (c3 << 6) | c4;
	}

	*len = c;
	return 1;
}

void
conf_free_list(struct conf_list *list)
{
	struct conf_list_node *node = TAILQ_FIRST(&list->fields);

	while (node) {
		TAILQ_REMOVE(&list->fields, node, link);
		if (node->field)
			free(node->field);
		free (node);
		node = TAILQ_FIRST(&list->fields);
	}
	free (list);
}

int
conf_begin(void)
{
  static int seq = 0;

  return ++seq;
}

static struct conf_trans *
conf_trans_node(int transaction, enum conf_op op)
{
	struct conf_trans *node;

	node = calloc (1, sizeof *node);
	if (!node) {
		xlog_warn("conf_trans_node: calloc (1, %lu) failed",
		(unsigned long)sizeof *node);
		return 0;
	}
	node->trans = transaction;
	node->op = op;
	TAILQ_INSERT_TAIL (&conf_trans_queue, node, link);
	return node;
}

/* Queue a set operation.  */
static int
conf_set(int transaction, const char *section, const char *arg,
	const char *tag, const char *value, int override, int is_default)
{
	struct conf_trans *node;

	if (!value || !*value)
		return 0;
	node = conf_trans_node(transaction, CONF_SET);
	if (!node)
		return 1;
	node->section = strdup(section);
	if (!node->section) {
		xlog_warn("conf_set: strdup(\"%s\") failed", section);
		goto fail;
	}
	/* Make Section names case-insensitive */
	upper2lower(node->section);

	if (arg) {
		node->arg = strdup(arg);
		if (!node->arg) {
			xlog_warn("conf_set: strdup(\"%s\") failed", arg);
			goto fail;
		}
	} else
		node->arg = NULL;

	node->tag = strdup(tag);
	if (!node->tag) {
		xlog_warn("conf_set: strdup(\"%s\") failed", tag);
		goto fail;
	}
	node->value = strdup(value);
	if (!node->value) {
		xlog_warn("conf_set: strdup(\"%s\") failed", value);
		goto fail;
	}
	node->override = override;
	node->is_default = is_default;
	return 0;

fail:
	if (node->tag)
		free(node->tag);
	if (node->section)
		free(node->section);
	if (node)
		free(node);
	return 1;
}

/* Queue a remove operation.  */
int
conf_remove(int transaction, const char *section, const char *tag)
{
	struct conf_trans *node;

	node = conf_trans_node(transaction, CONF_REMOVE);
	if (!node)
		goto fail;
	node->section = strdup(section);
	if (!node->section) {
		xlog_warn("conf_remove: strdup(\"%s\") failed", section);
		goto fail;
	}
	node->tag = strdup(tag);
	if (!node->tag) {
		xlog_warn("conf_remove: strdup(\"%s\") failed", tag);
		goto fail;
	}
	return 0;

fail:
	if (node && node->section)
		free (node->section);
	if (node)
		free (node);
	return 1;
}

/* Queue a remove section operation.  */
int
conf_remove_section(int transaction, const char *section)
{
	struct conf_trans *node;

	node = conf_trans_node(transaction, CONF_REMOVE_SECTION);
	if (!node)
		goto fail;
	node->section = strdup(section);
	if (!node->section) {
		xlog_warn("conf_remove_section: strdup(\"%s\") failed", section);
		goto fail;
	}
	return 0;

fail:
	if (node)
		free(node);
	return 1;
}

/* Execute all queued operations for this transaction.  Cleanup.  */
int
conf_end(int transaction, int commit)
{
	struct conf_trans *node, *next;

	for (node = TAILQ_FIRST(&conf_trans_queue); node; node = next) {
		next = TAILQ_NEXT(node, link);
		if (node->trans == transaction) {
			if (commit) {
				switch (node->op) {
				case CONF_SET:
					conf_set_now(node->section, node->arg, 
						node->tag, node->value, node->override, 
						node->is_default);
					break;
				case CONF_REMOVE:
					conf_remove_now(node->section, node->tag);
					break;
				case CONF_REMOVE_SECTION:
					conf_remove_section_now(node->section);
					break;
				default:
					xlog(LOG_INFO, "conf_end: unknown operation: %d", node->op);
				}
			}
			TAILQ_REMOVE (&conf_trans_queue, node, link);
			if (node->section)
				free(node->section);
			if (node->tag)
				free(node->tag);
			if (node->value)
				free(node->value);
			free (node);
		}
	}
	return 0;
}

/*
 * Dump running configuration upon SIGUSR1.
 * Configuration is "stored in reverse order", so reverse it again.
 */
struct dumper {
	char *s, *v;
	struct dumper *next;
};

static void
conf_report_dump(struct dumper *node)
{
	/* Recursive, cleanup when we're done.  */
	if (node->next)
		conf_report_dump(node->next);

	if (node->v)
		xlog(LOG_INFO, "%s=\t%s", node->s, node->v);
	else if (node->s) {
		xlog(LOG_INFO, "%s", node->s);
		if (strlen(node->s) > 0)
			free(node->s);
	}

	free (node);
}

void
conf_report (void)
{
	struct conf_binding *cb, *last = 0;
	unsigned int i, len, diff_arg = 0;
	char *current_section = (char *)0;
	char *current_arg = (char *)0;
	struct dumper *dumper, *dnode;

	dumper = dnode = (struct dumper *)calloc(1, sizeof *dumper);
	if (!dumper)
		goto mem_fail;

	xlog(LOG_INFO, "conf_report: dumping running configuration");

	for (i = 0; i < sizeof conf_bindings / sizeof conf_bindings[0]; i++)
		for (cb = LIST_FIRST(&conf_bindings[i]); cb; cb = LIST_NEXT(cb, link)) {
			if (!cb->is_default) {
				/* Make sure the Section arugment is the same */
				if (current_arg && current_section && cb->arg) {
					if (strcmp(cb->section, current_section) == 0 &&
						strcmp(cb->arg, current_arg) != 0)
					diff_arg = 1;
				}
				/* Dump this entry.  */
				if (!current_section || strcmp(cb->section, current_section) 
							|| diff_arg) {
					if (current_section || diff_arg) {
						len = strlen (current_section) + 3;
						if (current_arg)
							len += strlen(current_arg) + 3;
						dnode->s = malloc(len);
						if (!dnode->s)
							goto mem_fail;

						if (current_arg)
							snprintf(dnode->s, len, "[%s \"%s\"]", 
								current_section, current_arg);
						else
							snprintf(dnode->s, len, "[%s]", current_section);

						dnode->next = 
							(struct dumper *)calloc(1, sizeof (struct dumper));
						dnode = dnode->next;
						if (!dnode)
							goto mem_fail;

						dnode->s = "";
						dnode->next = 
							(struct dumper *)calloc(1, sizeof (struct dumper));
						dnode = dnode->next;
						if (!dnode)
						goto mem_fail;
					}
					current_section = cb->section;
					current_arg = cb->arg;
					diff_arg = 0;
				}
				dnode->s = cb->tag;
				dnode->v = cb->value;
				dnode->next = (struct dumper *)calloc (1, sizeof (struct dumper));
				dnode = dnode->next;
				if (!dnode)
					goto mem_fail;
				last = cb;
		}
	}

	if (last) {
		len = strlen(last->section) + 3;
		if (last->arg)
			len += strlen(last->arg) + 3;
		dnode->s = malloc(len);
		if (!dnode->s)
			goto mem_fail;
		if (last->arg)
			snprintf(dnode->s, len, "[%s \"%s\"]", last->section, last->arg);
		else
			snprintf(dnode->s, len, "[%s]", last->section);
	}
	conf_report_dump(dumper);
	return;

mem_fail:
	xlog_warn("conf_report: malloc/calloc failed");
	while ((dnode = dumper) != 0) {
		dumper = dumper->next;
		if (dnode->s)
			free(dnode->s);
		free(dnode);
	}
	return;
}
