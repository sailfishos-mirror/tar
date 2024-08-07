/* wordsplit - a word splitter
   Copyright (C) 2009-2018 Sergey Poznyakoff

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; either version 3 of the License, or (at your
   option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program. If not, see <http://www.gnu.org/licenses/>. */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <wordsplit.h>

#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <attribute.h>
#include <c-ctype.h>
#include <ialloc.h>

#if ENABLE_NLS
# include <gettext.h>
#else
# define gettext(msgid) msgid
#endif
#define _(msgid) gettext (msgid)
#define N_(msgid) msgid

#define ISWS(c) ((c) == ' ' || (c) == '\t' || (c) == '\n')
#define ISDELIM(ws, c) ((c) && strchr ((ws)->ws_delim, c))

#define ISVARBEG(c) (c_isalpha (c) || c == '_')
#define ISVARCHR(c) (c_isalnum (c) || c == '_')

#define WSP_RETURN_DELIMS(wsp) \
 ((wsp)->ws_flags & WRDSF_RETURN_DELIMS || ((wsp)->ws_options & WRDSO_MAXWORDS))

/* Set escape option F in WS for words (Q==0) or quoted strings (Q==1).  */
#define WRDSO_ESC_SET(ws,q,f) ((ws)->ws_options |= ((f) << 4*(q)))
/* Test WS for escape option F for words (Q==0) or quoted strings (Q==1).  */
#define WRDSO_ESC_TEST(ws,q,f) ((ws)->ws_options & ((f) << 4*(q)))

/* When printing diagnostics with %.*s, output at most this many bytes.
   Users typically don't want super-long strings in diagnostics,
   and anyway printf fails if it outputs more than INT_MAX bytes.
   printflen (LEN) returns LEN as an int, but at most PRINTMAX;
   printfdots (LEN) returns "..." if LEN exceeds PRINTMAX, "" otherwise.  */

enum { PRINTMAX = 10 * 1024 };

static int
printflen (idx_t len)
{
  return len <= PRINTMAX ? len : PRINTMAX;
}

static char const *
printfdots (idx_t len)
{
  return &"..."[3 * (len <= PRINTMAX)];
}


static void
_wsplt_alloc_die (struct wordsplit *wsp)
{
  wsp->ws_error ("%s", _("memory exhausted"));
  abort ();
}

static void ATTRIBUTE_FORMAT ((__printf__, 1, 2))
_wsplt_error (const char *fmt, ...)
{
  va_list ap;

  va_start (ap, fmt);
  vfprintf (stderr, fmt, ap);
  va_end (ap);
  fputc ('\n', stderr);
}

#ifdef _WORDSPLIT_EXTRAS
# define WORDSPLIT_EXTRAS_extern extern
#else
# define WORDSPLIT_EXTRAS_extern static
static void wordsplit_clearerr (struct wordsplit *);
static void wordsplit_free_envbuf (struct wordsplit *);
static void wordsplit_free_words (struct wordsplit *);
static void wordsplit_perror (struct wordsplit *);
#endif

static void wordsplit_free_nodes (struct wordsplit *);

static int
_wsplt_seterr (struct wordsplit *wsp, int ec)
{
  wsp->ws_errno = ec;
  if (wsp->ws_flags & WRDSF_SHOWERR)
    wordsplit_perror (wsp);
  return ec;
}

static int
_wsplt_nomem (struct wordsplit *wsp)
{
  errno = ENOMEM;
  wsp->ws_errno = WRDSE_NOSPACE;
  if (wsp->ws_flags & WRDSF_ENOMEMABRT)
    wsp->ws_alloc_die (wsp);
  if (wsp->ws_flags & WRDSF_SHOWERR)
    wordsplit_perror (wsp);
  if (!(wsp->ws_flags & WRDSF_REUSE))
    wordsplit_free (wsp);
  wordsplit_free_nodes (wsp);
  return wsp->ws_errno;
}

static int wordsplit_init (struct wordsplit *wsp, char const *input,
			   idx_t len, unsigned flags);
static int wordsplit_process_list (struct wordsplit *wsp, idx_t start);
static int wordsplit_finish (struct wordsplit *wsp);

static int
_wsplt_subsplit (struct wordsplit *wsp, struct wordsplit *wss,
		 char const *str, idx_t len,
		 unsigned flags, bool finalize)
{
  int rc;

  wss->ws_delim = wsp->ws_delim;
  wss->ws_debug = wsp->ws_debug;
  wss->ws_error = wsp->ws_error;
  wss->ws_alloc_die = wsp->ws_alloc_die;

  if (!(flags & WRDSF_NOVAR))
    {
      wss->ws_env = wsp->ws_env;
      wss->ws_getvar = wsp->ws_getvar;
      flags |= wsp->ws_flags & (WRDSF_ENV | WRDSF_ENV_KV | WRDSF_GETVAR);
    }
  if (!(flags & WRDSF_NOCMD))
    {
      wss->ws_command = wsp->ws_command;
    }

  if ((flags & (WRDSF_NOVAR|WRDSF_NOCMD)) != (WRDSF_NOVAR|WRDSF_NOCMD))
    {
      wss->ws_closure = wsp->ws_closure;
      flags |= wsp->ws_flags & WRDSF_CLOSURE;
    }

  wss->ws_options = wsp->ws_options;

  flags |= WRDSF_DELIM
         | WRDSF_ALLOC_DIE
         | WRDSF_ERROR
         | WRDSF_DEBUG
         | (wsp->ws_flags & (WRDSF_SHOWDBG | WRDSF_SHOWERR | WRDSF_OPTIONS));

  rc = wordsplit_init (wss, str, len, flags);
  if (rc)
    return rc;
  wss->ws_lvl++;
  rc = wordsplit_process_list (wss, 0);
  if (rc)
    {
      wordsplit_free_nodes (wss);
      return rc;
    }
  if (finalize)
    {
      rc = wordsplit_finish (wss);
      wordsplit_free_nodes (wss);
    }
  return rc;
}

static void
_wsplt_seterr_sub (struct wordsplit *wsp, struct wordsplit *wss)
{
  if (wsp->ws_errno == WRDSE_USERERR)
    free (wsp->ws_usererr);
  wsp->ws_errno = wss->ws_errno;
  if (wss->ws_errno == WRDSE_USERERR)
    {
      wsp->ws_usererr = wss->ws_usererr;
      wss->ws_errno = WRDSE_EOF;
      wss->ws_usererr = NULL;
    }
}

static void
wordsplit_init0 (struct wordsplit *wsp)
{
  if (wsp->ws_flags & WRDSF_REUSE)
    {
      if (!(wsp->ws_flags & WRDSF_APPEND))
	wordsplit_free_words (wsp);
      wordsplit_clearerr (wsp);
    }
  else
    {
      wsp->ws_wordv = NULL;
      wsp->ws_wordc = 0;
      wsp->ws_wordn = 0;
    }

  wsp->ws_errno = 0;
}

static char wordsplit_c_escape_tab[] = "\\\\\"\"a\ab\bf\fn\nr\rt\tv\v";

static int
wordsplit_init (struct wordsplit *wsp, char const *input, idx_t len,
		unsigned flags)
{
  wsp->ws_flags = flags;

  if (!(wsp->ws_flags & WRDSF_ALLOC_DIE))
    wsp->ws_alloc_die = _wsplt_alloc_die;
  if (!(wsp->ws_flags & WRDSF_ERROR))
    wsp->ws_error = _wsplt_error;

  if (!(wsp->ws_flags & WRDSF_NOVAR))
    {
      /* These will be initialized on first variable assignment */
      wsp->ws_envidx = wsp->ws_envsiz = 0;
      wsp->ws_envbuf = NULL;
    }

  if (!(wsp->ws_flags & WRDSF_NOCMD))
    {
      if (!wsp->ws_command)
	{
	  _wsplt_seterr (wsp, WRDSE_USAGE);
	  errno = EINVAL;
	  return wsp->ws_errno;
	}
    }

  if (wsp->ws_flags & WRDSF_SHOWDBG)
    {
      if (!(wsp->ws_flags & WRDSF_DEBUG))
	{
	  if (wsp->ws_flags & WRDSF_ERROR)
	    wsp->ws_debug = wsp->ws_error;
	  else if (wsp->ws_flags & WRDSF_SHOWERR)
	    wsp->ws_debug = _wsplt_error;
	  else
	    wsp->ws_flags &= ~WRDSF_SHOWDBG;
	}
    }

  wsp->ws_input = input;
  wsp->ws_len = len;

  if (!(wsp->ws_flags & WRDSF_DOOFFS))
    wsp->ws_offs = 0;

  if (!(wsp->ws_flags & WRDSF_DELIM))
    wsp->ws_delim = " \t\n";

  if (!(wsp->ws_flags & WRDSF_COMMENT))
    wsp->ws_comment = NULL;

  if (!(wsp->ws_flags & WRDSF_CLOSURE))
    wsp->ws_closure = NULL;

  if (!(wsp->ws_flags & WRDSF_OPTIONS))
    wsp->ws_options = 0;

  if (wsp->ws_flags & WRDSF_ESCAPE)
    {
      if (!wsp->ws_escape[WRDSX_WORD])
	wsp->ws_escape[WRDSX_WORD] = "";
      if (!wsp->ws_escape[WRDSX_QUOTE])
	wsp->ws_escape[WRDSX_QUOTE] = "";
    }
  else
    {
      if (wsp->ws_flags & WRDSF_CESCAPES)
	{
	  wsp->ws_escape[WRDSX_WORD] = wordsplit_c_escape_tab;
	  wsp->ws_escape[WRDSX_QUOTE] = wordsplit_c_escape_tab;
	  wsp->ws_options |= WRDSO_OESC_QUOTE | WRDSO_OESC_WORD
	                     | WRDSO_XESC_QUOTE | WRDSO_XESC_WORD;
	}
      else
	{
	  wsp->ws_escape[WRDSX_WORD] = "";
	  wsp->ws_escape[WRDSX_QUOTE] = "\\\\\"\"";
	  wsp->ws_options |= WRDSO_BSKEEP_QUOTE;
	}
    }

  wsp->ws_endp = 0;
  wsp->ws_wordi = 0;

  if (wsp->ws_flags & WRDSF_REUSE)
    wordsplit_free_nodes (wsp);
  wsp->ws_head = wsp->ws_tail = NULL;

  wordsplit_init0 (wsp);

  return WRDSE_OK;
}

static int
alloc_space (struct wordsplit *wsp, idx_t count)
{
  idx_t offs_plus_count;
  if (ckd_add (&offs_plus_count, count,
	       wsp->ws_flags & WRDSF_DOOFFS ? wsp->ws_offs : 0))
    return _wsplt_nomem (wsp);

  idx_t wordn;
  char **wordv;

  if (!wsp->ws_wordv)
    {
      /* The default largest "small, fast" request for glibc malloc.  */
      enum { DEFAULT_MXFAST = 64 * sizeof (size_t) / 4 };

      enum { ALLOC_INIT = DEFAULT_MXFAST / sizeof *wordv };
      wordn = offs_plus_count <= ALLOC_INIT ? ALLOC_INIT : offs_plus_count;

      /* Use calloc so that the initial ws_offs words are zero.  */
      wordv = icalloc (wordn, sizeof *wordv);
    }
  else
    {
      idx_t wordroom = wsp->ws_wordn - wsp->ws_wordc;
      if (offs_plus_count <= wordroom)
	return WRDSE_OK;

      /* Grow the allocation by at least MININCR.  To avoid quadratic
	 behavior, also grow it by at least 50% if possible. */
      idx_t minincr = offs_plus_count - wordroom;
      idx_t halfn = wsp->ws_wordn >> 1;
      wordv = (halfn <= minincr || ckd_add (&wordn, wsp->ws_wordn, halfn)
	       ? nullptr
	       : ireallocarray (wsp->ws_wordv, wordn, sizeof *wordv));
      if (!wordv)
	wordv = (ckd_add (&wordn, wsp->ws_wordn, minincr)
		 ? nullptr
		 : ireallocarray (wsp->ws_wordv, wordn, sizeof *wordv));
    }

  if (!wordv)
    return _wsplt_nomem (wsp);
  wsp->ws_wordn = wordn;
  wsp->ws_wordv = wordv;
  return WRDSE_OK;
}


/* Node state flags */
#define _WSNF_NULL     0x01	/* null node (a noop) */
#define _WSNF_WORD     0x02	/* node contains word in v.word */
#define _WSNF_QUOTE    0x04	/* text is quoted */
#define _WSNF_NOEXPAND 0x08	/* text is not subject to expansion */
#define _WSNF_JOIN     0x10	/* node must be joined with the next node */
#define _WSNF_SEXP     0x20	/* is a sed expression */
#define _WSNF_DELIM    0x40     /* node is a delimiter */

#define _WSNF_EMPTYOK  0x0100	/* special flag indicating that
				   wordsplit_add_segm must add the
				   segment even if it is empty */

struct wordsplit_node
{
  struct wordsplit_node *prev;	/* Previous element */
  struct wordsplit_node *next;	/* Next element */
  unsigned flags;		/* Node flags */
  union
  {
    struct
    {
      idx_t beg;		/* Start of word in ws_input */
      idx_t end;		/* End of word in ws_input */
    } segm;
    char *word;
  } v;
};

static const char *
wsnode_flagstr (unsigned flags)
{
  static char retbuf[7];
  char *p = retbuf;

  if (flags & _WSNF_WORD)
    *p++ = 'w';
  else if (flags & _WSNF_NULL)
    *p++ = 'n';
  else
    *p++ = '-';
  if (flags & _WSNF_QUOTE)
    *p++ = 'q';
  else
    *p++ = '-';
  if (flags & _WSNF_NOEXPAND)
    *p++ = 'E';
  else
    *p++ = '-';
  if (flags & _WSNF_JOIN)
    *p++ = 'j';
  else
    *p++ = '-';
  if (flags & _WSNF_SEXP)
    *p++ = 's';
  else
    *p++ = '-';
  if (flags & _WSNF_DELIM)
    *p++ = 'd';
  else
    *p++ = '-';
  *p = '\0';
  return retbuf;
}

static const char *
wsnode_ptr (struct wordsplit *wsp, struct wordsplit_node *p)
{
  if (p->flags & _WSNF_NULL)
    return "";
  else if (p->flags & _WSNF_WORD)
    return p->v.word;
  else
    return wsp->ws_input + p->v.segm.beg;
}

static idx_t
wsnode_len (struct wordsplit_node *p)
{
  if (p->flags & _WSNF_NULL)
    return 0;
  else if (p->flags & _WSNF_WORD)
    return strlen (p->v.word);
  else
    return p->v.segm.end - p->v.segm.beg;
}

static struct wordsplit_node *
wsnode_new (struct wordsplit *wsp)
{
  struct wordsplit_node *node = calloc (1, sizeof *node);
  if (!node)
    _wsplt_nomem (wsp);
  return node;
}

static void
wsnode_free (struct wordsplit_node *p)
{
  if (p->flags & _WSNF_WORD)
    free (p->v.word);
  free (p);
}

static void
wsnode_append (struct wordsplit *wsp, struct wordsplit_node *node)
{
  node->next = NULL;
  node->prev = wsp->ws_tail;
  if (wsp->ws_tail)
    wsp->ws_tail->next = node;
  else
    wsp->ws_head = node;
  wsp->ws_tail = node;
}

static void
wsnode_remove (struct wordsplit *wsp, struct wordsplit_node *node)
{
  struct wordsplit_node *p;

  p = node->prev;
  if (p)
    {
      p->next = node->next;
      if (!node->next)
	p->flags &= ~_WSNF_JOIN;
    }
  else
    wsp->ws_head = node->next;

  p = node->next;
  if (p)
    p->prev = node->prev;
  else
    wsp->ws_tail = node->prev;

  node->next = node->prev = NULL;
}

static struct wordsplit_node *
wsnode_tail (struct wordsplit_node *p)
{
  while (p && p->next)
    p = p->next;
  return p;
}

static void
wsnode_insert (struct wordsplit *wsp, struct wordsplit_node *node,
	       struct wordsplit_node *anchor)
{
  if (!wsp->ws_head)
    {
      node->next = node->prev = NULL;
      wsp->ws_head = wsp->ws_tail = node;
    }
  else
    {
      struct wordsplit_node *p;
      struct wordsplit_node *tail = wsnode_tail (node);

      p = anchor->next;
      if (p)
	p->prev = tail;
      else
	wsp->ws_tail = tail;
      tail->next = p;
      node->prev = anchor;
      anchor->next = node;
    }
}

static bool
wordsplit_add_segm (struct wordsplit *wsp, idx_t beg, idx_t end, unsigned flg)
{
  if (end == beg && !(flg & _WSNF_EMPTYOK))
    return true;
  struct wordsplit_node *node = wsnode_new (wsp);
  if (!node)
    return false;
  node->flags = flg & ~(_WSNF_WORD | _WSNF_EMPTYOK);
  node->v.segm.beg = beg;
  node->v.segm.end = end;
  wsnode_append (wsp, node);
  return true;
}

static void
wordsplit_free_nodes (struct wordsplit *wsp)
{
  struct wordsplit_node *p;

  for (p = wsp->ws_head; p;)
    {
      struct wordsplit_node *next = p->next;
      wsnode_free (p);
      p = next;
    }
  wsp->ws_head = wsp->ws_tail = NULL;
}

static void
wordsplit_dump_nodes (struct wordsplit *wsp)
{
  intmax_t n = 0;
  for (struct wordsplit_node *p = wsp->ws_head; p; p = p->next, n++)
    {
      if (p->flags & _WSNF_WORD)
	wsp->ws_debug ("(%02td) %4jd: %p: %#04x (%s):%s;",
		       wsp->ws_lvl,
		       n, p, p->flags, wsnode_flagstr (p->flags), p->v.word);
      else
	{
	  idx_t seglen = p->v.segm.end - p->v.segm.beg;
	  wsp->ws_debug ("(%02td) %4jd: %p: %#04x (%s):%.*s%s;",
			 wsp->ws_lvl,
			 n, p, p->flags, wsnode_flagstr (p->flags),
			 printflen (seglen), wsp->ws_input + p->v.segm.beg,
			 printfdots (seglen));
	}
    }
}

static int
coalesce_segment (struct wordsplit *wsp, struct wordsplit_node *node)
{
  struct wordsplit_node *p, *end;
  idx_t len = 0;
  char *buf, *cur;

  if (!(node->flags & _WSNF_JOIN))
    return WRDSE_OK;

  for (p = node; p && (p->flags & _WSNF_JOIN); p = p->next)
    {
      len += wsnode_len (p);
    }
  if (p)
    len += wsnode_len (p);
  end = p;

  buf = imalloc (len + 1);
  if (!buf)
    return _wsplt_nomem (wsp);
  cur = buf;

  p = node;
  for (;;)
    {
      struct wordsplit_node *next = p->next;
      const char *str = wsnode_ptr (wsp, p);
      idx_t slen = wsnode_len (p);

      memcpy (cur, str, slen);
      cur += slen;
      if (p != node)
	{
	  node->flags |= p->flags & _WSNF_QUOTE;
	  wsnode_remove (wsp, p);
	  if (p == end)
	    {
	      /* Call wsnode_free separately to work around GCC bug 106427.  */
	      wsnode_free (p);
	      break;
	    }
	  wsnode_free (p);
	}
      p = next;
    }

  *cur = '\0';

  node->flags &= ~_WSNF_JOIN;

  if (node->flags & _WSNF_WORD)
    free (node->v.word);
  else
    node->flags |= _WSNF_WORD;
  node->v.word = buf;
  return WRDSE_OK;
}

static void wordsplit_string_unquote_copy (struct wordsplit *ws, bool inquote,
					   char *dst, const char *src,
					   idx_t n);

static int
wsnode_quoteremoval (struct wordsplit *wsp)
{
  struct wordsplit_node *p;

  for (p = wsp->ws_head; p; p = p->next)
    {
      bool unquote = (wsp->ws_flags & WRDSF_QUOTE
		      && !(p->flags & _WSNF_NOEXPAND));

      if (unquote)
	{
	  const char *str = wsnode_ptr (wsp, p);
	  idx_t slen = wsnode_len (p);

	  if (!(p->flags & _WSNF_WORD))
	    {
	      char *newstr = imalloc (slen + 1);
	      if (!newstr)
		return _wsplt_nomem (wsp);
	      memcpy (newstr, str, slen);
	      newstr[slen] = '\0';
	      p->v.word = newstr;
	      p->flags |= _WSNF_WORD;
	    }

	  wordsplit_string_unquote_copy (wsp, !!(p->flags & _WSNF_QUOTE),
					 p->v.word, str, slen);
	}
    }
  return WRDSE_OK;
}

static bool
wsnode_coalesce (struct wordsplit *wsp)
{
  struct wordsplit_node *p;

  for (p = wsp->ws_head; p; p = p->next)
    {
      if (p->flags & _WSNF_JOIN)
	if (coalesce_segment (wsp, p))
	  return true;
    }
  return false;
}

static bool
wsnode_tail_coalesce (struct wordsplit *wsp, struct wordsplit_node *p)
{
  if (p->next)
    {
      struct wordsplit_node *np = p;
      while (np && np->next)
	{
	  np->flags |= _WSNF_JOIN;
	  np = np->next;
	}
      if (coalesce_segment (wsp, p))
	return true;
    }
  return false;
}

static idx_t skip_delim (struct wordsplit *wsp);

static int
wordsplit_finish (struct wordsplit *wsp)
{
  struct wordsplit_node *p;
  idx_t n;
  char delim;

  /* Postprocess delimiters. It would be rather simple, if it weren't for
     the incremental operation.

     Nodes of type _WSNF_DELIM get inserted to the node list if either
     WRDSF_RETURN_DELIMS flag or WRDSO_MAXWORDS option is set.

     The following cases should be distinguished:

     1. If both WRDSF_SQUEEZE_DELIMS and WRDSF_RETURN_DELIMS are set, compress
        any runs of similar delimiter nodes to a single node. The nodes are
	'similar' if they point to the same delimiter character.

	If WRDSO_MAXWORDS option is set, stop compressing when
	ws_wordi + 1 == ws_maxwords, and coalesce the rest of nodes into
	a single last node.

     2. If WRDSO_MAXWORDS option is set, but WRDSF_RETURN_DELIMS is not,
        remove any delimiter nodes. Stop operation when
	ws_wordi + 1 == ws_maxwords, and coalesce the rest of nodes into
	a single last node.

     3. If incremental operation is in progress, restart the loop any time
        a delimiter node is about to be returned, unless WRDSF_RETURN_DELIMS
	is set.
  */
 again:
  delim = '\0';      /* Delimiter being processed (if any) */
  n = 0;             /* Number of words processed so far */
  p = wsp->ws_head;  /* Current node */

  while (p)
    {
      struct wordsplit_node *next = p->next;
      if (p->flags & _WSNF_DELIM)
	{
	  if (wsp->ws_flags & WRDSF_RETURN_DELIMS)
	    {
	      if (wsp->ws_flags & WRDSF_SQUEEZE_DELIMS)
		{
		  char const *s = wsnode_ptr (wsp, p);
		  if (delim)
		    {
		      if (delim == *s)
			{
			  wsnode_remove (wsp, p);
			  p = next;
			  continue;
			}
		      else
			{
			  delim = '\0';
			  n++; /* Count this node; it will be returned */
			}
		    }
		  else
		    {
		      delim = *s;
		      p = next;
		      continue;
		    }
		}
	    }
	  else if (wsp->ws_options & WRDSO_MAXWORDS)
	    {
	      wsnode_remove (wsp, p);
	      p = next;
	      continue;
	    }
	}
      else
	{
	  if (delim)
	    {
	      /* Last node was a delimiter or a compressed run of delimiters;
		 Count it, and clear the delimiter marker */
	      n++;
	      delim = '\0';
	    }
	  if (wsp->ws_options & WRDSO_MAXWORDS)
	    {
	      if (wsp->ws_wordi + n + 1 == wsp->ws_maxwords)
		break;
	    }
	}
      n++;
      if (wsp->ws_flags & WRDSF_INCREMENTAL)
	p = NULL; /* Break the loop */
      else
	p = next;
    }

  if (p)
    {
      /* We're here if WRDSO_MAXWORDS is in effect and wsp->ws_maxwords
	 words have already been collected. Reconstruct a single final
	 node from the remaining nodes. */
      if (wsnode_tail_coalesce (wsp, p))
	return wsp->ws_errno;
      n++;
    }

  if (n == 0 && (wsp->ws_flags & WRDSF_INCREMENTAL))
    {
      /* The loop above have eliminated all nodes. Restart the
	 processing, if there's any input left. */
      if (wsp->ws_endp < wsp->ws_len)
	{
	  int rc;
	  if (wsp->ws_flags & WRDSF_SHOWDBG)
	    wsp->ws_debug (_("Restarting"));
	  rc = wordsplit_process_list (wsp, skip_delim (wsp));
	  if (rc)
	    return rc;
	}
      else
	{
	  wsp->ws_error = WRDSE_EOF;
	  return WRDSE_EOF;
	}
      goto again;
    }

  if (alloc_space (wsp, n + 1))
    return wsp->ws_errno;

  while (wsp->ws_head)
    {
      const char *str = wsnode_ptr (wsp, wsp->ws_head);
      idx_t slen = wsnode_len (wsp->ws_head);
      char *newstr = imalloc (slen + 1);

      /* Assign newstr first, even if it is NULL.  This way
         wordsplit_free will work even if we return
         nomem later. */
      wsp->ws_wordv[wsp->ws_offs + wsp->ws_wordc] = newstr;
      if (!newstr)
	return _wsplt_nomem (wsp);
      memcpy (newstr, str, slen);
      newstr[slen] = '\0';

      wsnode_remove (wsp, wsp->ws_head);

      wsp->ws_wordc++;
      wsp->ws_wordi++;

      if (wsp->ws_flags & WRDSF_INCREMENTAL)
	break;
    }
  wsp->ws_wordv[wsp->ws_offs + wsp->ws_wordc] = NULL;
  return WRDSE_OK;
}

#ifdef _WORDSPLIT_EXTRAS
int
wordsplit_append (wordsplit_t *wsp, int argc, char **argv)
{
  int rc;
  idx_t i;

  rc = alloc_space (wsp, wsp->ws_wordc + argc + 1);
  if (rc)
    return rc;
  for (i = 0; i < argc; i++)
    {
      char *newstr = strdup (argv[i]);
      if (!newstr)
	{
	  while (i > 0)
	    {
	      free (wsp->ws_wordv[wsp->ws_offs + wsp->ws_wordc + i - 1]);
	      wsp->ws_wordv[wsp->ws_offs + wsp->ws_wordc + i - 1] = NULL;
	      i--;
	    }
	  return _wsplt_nomem (wsp);
	}
      wsp->ws_wordv[wsp->ws_offs + wsp->ws_wordc + i] = newstr;
    }
  wsp->ws_wordc += i;
  wsp->ws_wordv[wsp->ws_offs + wsp->ws_wordc] = NULL;
  return WRDSE_OK;
}
#endif

/* Variable expansion */
static int
node_split_prefix (struct wordsplit *wsp,
		   struct wordsplit_node **ptail,
		   struct wordsplit_node *node,
		   idx_t beg, idx_t len, unsigned flg)
{
  if (len == 0)
    return 0;
  struct wordsplit_node *newnode = wsnode_new (wsp);
  if (!newnode)
    return 1;
  wsnode_insert (wsp, newnode, *ptail);
  if (node->flags & _WSNF_WORD)
    {
      const char *str = wsnode_ptr (wsp, node);
      char *newstr = imalloc (len + 1);
      if (!newstr)
	return _wsplt_nomem (wsp);
      memcpy (newstr, str + beg, len);
      newstr[len] = '\0';
      newnode->flags = _WSNF_WORD;
      newnode->v.word = newstr;
    }
  else
    {
      newnode->v.segm.beg = node->v.segm.beg + beg;
      newnode->v.segm.end = newnode->v.segm.beg + len;
    }
  newnode->flags |= flg;
  *ptail = newnode;
  return 0;
}

static bool
find_closing_paren (char const *str, idx_t i, idx_t len, idx_t *poff,
		    char const *paren)
{
  enum { st_init, st_squote, st_dquote } state = st_init;
  idx_t level = 1;

  for (; i < len; i++)
    {
      switch (state)
	{
	case st_init:
	  switch (str[i])
	    {
	    default:
	      if (str[i] == paren[0])
		{
		  level++;
		  break;
		}
	      else if (str[i] == paren[1])
		{
		  if (--level == 0)
		    {
		      *poff = i;
		      return false;
		    }
		  break;
		}
	      break;

	    case '"':
	      state = st_dquote;
	      break;

	    case '\'':
	      state = st_squote;
	      break;
	    }
	  break;

	case st_squote:
	  if (str[i] == '\'')
	    state = st_init;
	  break;

	case st_dquote:
	  if (str[i] == '\\')
	    i++;
	  else if (str[i] == '"')
	    state = st_init;
	  break;
	}
    }
  return true;
}

static int
wordsplit_find_env (struct wordsplit *wsp, char const *name, idx_t len,
		    char const **ret)
{
  idx_t i;

  if (!(wsp->ws_flags & WRDSF_ENV))
    return WRDSE_UNDEF;

  if (wsp->ws_flags & WRDSF_ENV_KV)
    {
      /* A key-value pair environment */
      for (i = 0; wsp->ws_env[i]; i++)
	{
	  idx_t elen = strlen (wsp->ws_env[i]);
	  if (elen == len && memcmp (wsp->ws_env[i], name, elen) == 0)
	    {
	      *ret = wsp->ws_env[i + 1];
	      return WRDSE_OK;
	    }
	  /* Skip the value.  Break the loop if it is NULL. */
	  i++;
	  if (wsp->ws_env[i] == NULL)
	    break;
	}
    }
  else if (wsp->ws_env)
    {
      /* Usual (A=B) environment. */
      for (i = 0; wsp->ws_env[i]; i++)
	{
	  idx_t j;
	  const char *var = wsp->ws_env[i];

	  for (j = 0; j < len; j++)
	    if (name[j] != var[j])
	      break;
	  if (j == len && var[j] == '=')
	    {
	      *ret = var + j + 1;
	      return WRDSE_OK;
	    }
	}
    }
  return WRDSE_UNDEF;
}

/* Initial size for ws_env, if allocated automatically */
enum { WORDSPLIT_ENV_INIT = 16 };

static int
wsplt_assign_var (struct wordsplit *wsp, char const *name, idx_t namelen,
		  char *value)
{
  int n = (wsp->ws_flags & WRDSF_ENV_KV) ? 2 : 1;
  char *v;

  if (wsp->ws_envsiz - wsp->ws_envidx <= n)
    {
      idx_t sz;
      char **newenv;

      if (!wsp->ws_envbuf)
	{
	  if (wsp->ws_flags & WRDSF_ENV)
	    {
	      idx_t i = 0, j;

	      if (wsp->ws_env)
		{
		  for (; wsp->ws_env[i]; i++)
		    ;
		}

	      sz = i + n + 1;

	      newenv = icalloc (sz, sizeof *newenv);
	      if (!newenv)
		return _wsplt_nomem (wsp);

	      for (j = 0; j < i; j++)
		{
		  newenv[j] = strdup (wsp->ws_env[j]);
		  if (!newenv[j])
		    {
		      for (; j > 1; j--)
			free (newenv[j-1]);
		      free (newenv[j-1]);
		      free (newenv);
		      return _wsplt_nomem (wsp);
		    }
		}
	      newenv[j] = NULL;

	      wsp->ws_envbuf = newenv;
	      wsp->ws_envidx = i;
	      wsp->ws_envsiz = sz;
	      wsp->ws_env = (char const **) newenv;
	    }
	  else
	    {
	      newenv = calloc (WORDSPLIT_ENV_INIT, sizeof *newenv);
	      if (!newenv)
		return _wsplt_nomem (wsp);
	      wsp->ws_envbuf = newenv;
	      wsp->ws_envidx = 0;
	      wsp->ws_envsiz = WORDSPLIT_ENV_INIT;
	      wsp->ws_env = (char const **) newenv;
	      wsp->ws_flags |= WRDSF_ENV;
	    }
	}
      else
	{
	  idx_t envsiz, envsiz2;
	  if (ckd_add (&envsiz, wsp->ws_envidx, n + 1))
	    return _wsplt_nomem (wsp);
	  newenv = ((!ckd_add (&envsiz2, wsp->ws_envsiz, wsp->ws_envsiz >> 1)
		     && envsiz < envsiz2)
		    ? ireallocarray (wsp->ws_envbuf, envsiz2, sizeof *newenv)
		    : NULL);
	  if (newenv)
	    envsiz = envsiz2;
	  else
	    {
	      newenv = ireallocarray (wsp->ws_envbuf, envsiz, sizeof *newenv);
	      if (!newenv)
		return _wsplt_nomem (wsp);
	    }

	  wsp->ws_envbuf = newenv;
	  wsp->ws_envsiz = envsiz;
	  wsp->ws_env = (char const **) wsp->ws_envbuf;
	}
    }

  if (wsp->ws_flags & WRDSF_ENV_KV)
    {
      /* A key-value pair environment */
      char *p = imalloc (namelen + 1);
      if (!p)
	return _wsplt_nomem (wsp);
      memcpy (p, name, namelen);
      p[namelen] = '\0';

      v = strdup (value);
      if (!v)
	{
	  free (p);
	  return _wsplt_nomem (wsp);
	}
      wsp->ws_env[wsp->ws_envidx++] = p;
      wsp->ws_env[wsp->ws_envidx++] = v;
    }
  else
    {
      v = imalloc (namelen + strlen (value) + 2);
      if (!v)
	return _wsplt_nomem (wsp);
      memcpy (v, name, namelen);
      v[namelen++] = '=';
      strcpy (v + namelen, value);
      wsp->ws_env[wsp->ws_envidx++] = v;
    }
  wsp->ws_env[wsp->ws_envidx++] = NULL;
  return WRDSE_OK;
}

static int
expvar (struct wordsplit *wsp, char const *str, idx_t len,
	struct wordsplit_node **ptail, const char **pend, unsigned flg)
{
  idx_t i = 0;
  const char *defstr = NULL;
  char *value;
  const char *vptr;
  struct wordsplit_node *newnode;
  const char *start = str - 1;
  int rc;
  struct wordsplit ws;

  if (ISVARBEG (str[0]))
    {
      for (i = 1; i < len; i++)
	if (!ISVARCHR (str[i]))
	  break;
      *pend = str + i - 1;
    }
  else if (str[0] == '{')
    {
      str++;
      len--;
      for (i = 1; i < len; i++)
	{
	  if (str[i] == ':')
	    {
	      idx_t j;

	      defstr = str + i + 1;
	      if (find_closing_paren (str, i + 1, len, &j, "{}"))
		return _wsplt_seterr (wsp, WRDSE_CBRACE);
	      *pend = str + j;
	      break;
	    }
	  else if (str[i] == '}')
	    {
	      defstr = NULL;
	      *pend = str + i;
	      break;
	    }
	  else if (str[i] && strchr ("-+?=", str[i]))
	    {
	      idx_t j;

	      defstr = str + i;
	      if (find_closing_paren (str, i, len, &j, "{}"))
		return _wsplt_seterr (wsp, WRDSE_CBRACE);
	      *pend = str + j;
	      break;
	    }
	}
      if (i == len)
	return _wsplt_seterr (wsp, WRDSE_CBRACE);
    }
  else
    {
      newnode = wsnode_new (wsp);
      if (!newnode)
	return 1;
      wsnode_insert (wsp, newnode, *ptail);
      *ptail = newnode;
      newnode->flags = _WSNF_WORD | flg;
      newnode->v.word = malloc (3);
      if (!newnode->v.word)
	return _wsplt_nomem (wsp);
      newnode->v.word[0] = '$';
      newnode->v.word[1] = str[0];
      newnode->v.word[2] = '\0';
      *pend = str;
      return 0;
    }

  /* Actually expand the variable */
  /* str - start of the variable name
     i   - its length
     defstr - default replacement str */

  if (defstr && ! (defstr[0] && strchr ("-+?=", defstr[0])))
    {
      rc = WRDSE_UNDEF;
      defstr = NULL;
    }
  else
    {
      rc = wordsplit_find_env (wsp, str, i, &vptr);
      if (rc == WRDSE_OK)
	{
	  if (vptr)
	    {
	      value = strdup (vptr);
	      if (!value)
		rc = WRDSE_NOSPACE;
	    }
	  else
	    rc = WRDSE_UNDEF;
	}
      else if (wsp->ws_flags & WRDSF_GETVAR)
	rc = wsp->ws_getvar (&value, str, i, wsp->ws_closure);
      else
	rc = WRDSE_UNDEF;

      if (rc == WRDSE_OK
	  && !(value && *value)
	  && defstr && defstr[-1] == ':')
	{
	  free (value);
	  rc = WRDSE_UNDEF;
	}
    }

  switch (rc)
    {
    case WRDSE_OK:
      if (defstr && *defstr == '+')
	{
	  idx_t size = *pend - ++defstr;

	  rc = _wsplt_subsplit (wsp, &ws, defstr, size,
				WRDSF_NOSPLIT | WRDSF_WS | WRDSF_QUOTE |
				(wsp->ws_flags &
				 (WRDSF_NOVAR | WRDSF_NOCMD)),
				true);
	  if (rc)
	    return rc;
	  free (value);
	  value = ws.ws_wordv[0];
	  ws.ws_wordv[0] = NULL;
	  wordsplit_free (&ws);
	}
      break;

    case WRDSE_UNDEF:
      if (defstr)
	{
	  idx_t size;
	  if (*defstr == '-' || *defstr == '=')
	    {
	      size = *pend - ++defstr;

	      rc = _wsplt_subsplit (wsp, &ws, defstr, size,
				    WRDSF_NOSPLIT | WRDSF_WS | WRDSF_QUOTE |
				    (wsp->ws_flags &
				     (WRDSF_NOVAR | WRDSF_NOCMD)),
				    true);
	      if (rc)
		return rc;

	      value = ws.ws_wordv[0];
	      ws.ws_wordv[0] = NULL;
	      wordsplit_free (&ws);

	      if (defstr[-1] == '=')
		wsplt_assign_var (wsp, str, i, value);
	    }
	  else
	    {
	      if (*defstr == '?')
		{
		  size = *pend - ++defstr;
		  if (size == 0)
		    wsp->ws_error (_("%.*s%s: variable null or not set"),
				   printflen (i), str, printfdots (i));
		  else
		    {
		      rc = _wsplt_subsplit (wsp, &ws, defstr, size,
					    WRDSF_NOSPLIT | WRDSF_WS |
					    WRDSF_QUOTE |
					    (wsp->ws_flags &
					     (WRDSF_NOVAR | WRDSF_NOCMD)),
					    true);
		      if (rc == 0)
			wsp->ws_error ("%.*s%s: %s",
				       printflen (i), str, printfdots (i),
				       ws.ws_wordv[0]);
		      else
			wsp->ws_error ("%.*s%s: %.*s%s",
				       printflen (i), str, printfdots (i),
				       printflen (size), defstr,
				       printfdots (size));
		      wordsplit_free (&ws);
		    }
		}
	      value = NULL;
	    }
	}
      else if (wsp->ws_flags & WRDSF_UNDEF)
	{
	  _wsplt_seterr (wsp, WRDSE_UNDEF);
	  return 1;
	}
      else
	{
	  if (wsp->ws_flags & WRDSF_WARNUNDEF)
	    wsp->ws_error (_("warning: undefined variable '%.*s%s'"),
			   printflen (i), str, printfdots (i));
	  if (wsp->ws_flags & WRDSF_KEEPUNDEF)
	    value = NULL;
	  else
	    {
	      value = strdup ("");
	      if (!value)
		return _wsplt_nomem (wsp);
	    }
	}
      break;

    case WRDSE_NOSPACE:
      return _wsplt_nomem (wsp);

    case WRDSE_USERERR:
      if (wsp->ws_errno == WRDSE_USERERR)
	free (wsp->ws_usererr);
      wsp->ws_usererr = value;
      FALLTHROUGH;
    default:
      _wsplt_seterr (wsp, rc);
      return 1;
    }

  if (value)
    {
      if (flg & _WSNF_QUOTE)
	{
	  newnode = wsnode_new (wsp);
	  if (!newnode)
	    {
	      free (value);
	      return 1;
	    }
	  wsnode_insert (wsp, newnode, *ptail);
	  *ptail = newnode;
	  newnode->flags = _WSNF_WORD | _WSNF_NOEXPAND | flg;
	  newnode->v.word = value;
	}
      else if (!*value)
	{
	  free (value);
	  /* Empty string is a special case */
	  newnode = wsnode_new (wsp);
	  if (!newnode)
	    return 1;
	  wsnode_insert (wsp, newnode, *ptail);
	  *ptail = newnode;
	  newnode->flags = _WSNF_NULL;
	}
      else
	{
	  struct wordsplit ws;
	  int rc;

	  rc = _wsplt_subsplit (wsp, &ws, value, strlen (value),
				(WRDSF_NOVAR | WRDSF_NOCMD | WRDSF_QUOTE
				 | (WSP_RETURN_DELIMS (wsp)
				    ? WRDSF_RETURN_DELIMS : 0)),
				false);
	  free (value);
	  if (rc)
	    {
	      _wsplt_seterr_sub (wsp, &ws);
	      wordsplit_free (&ws);
	      return 1;
	    }
	  wsnode_insert (wsp, ws.ws_head, *ptail);
	  *ptail = ws.ws_tail;
	  ws.ws_head = ws.ws_tail = NULL;
	  wordsplit_free (&ws);
	}
    }
  else if (wsp->ws_flags & WRDSF_KEEPUNDEF)
    {
      idx_t size = *pend - start + 1;

      newnode = wsnode_new (wsp);
      if (!newnode)
	return 1;
      wsnode_insert (wsp, newnode, *ptail);
      *ptail = newnode;
      newnode->flags = _WSNF_WORD | _WSNF_NOEXPAND | flg;
      newnode->v.word = imalloc (size + 1);
      if (!newnode->v.word)
	return _wsplt_nomem (wsp);
      memcpy (newnode->v.word, start, size);
      newnode->v.word[size] = '\0';
    }
  else
    {
      newnode = wsnode_new (wsp);
      if (!newnode)
	return 1;
      wsnode_insert (wsp, newnode, *ptail);
      *ptail = newnode;
      newnode->flags = _WSNF_NULL;
    }
  return 0;
}

static bool
begin_var_p (char c)
{
  return c == '{' || ISVARBEG (c);
}

static bool
node_expand (struct wordsplit *wsp, struct wordsplit_node *node,
	     bool (*beg_p) (char),
	     int (*ws_exp_fn) (struct wordsplit *wsp,
			       char const *str, idx_t len,
			       struct wordsplit_node **ptail,
			       const char **pend,
			       unsigned flg))
{
  const char *str = wsnode_ptr (wsp, node);
  idx_t slen = wsnode_len (node);
  const char *end = str + slen;
  const char *p;
  idx_t off = 0;
  struct wordsplit_node *tail = node;

  for (p = str; p < end; p++)
    {
      if (*p == '\\')
	{
	  p++;
	  continue;
	}
      if (*p == '$' && beg_p (p[1]))
	{
	  idx_t n = p - str;

	  if (tail != node)
	    tail->flags |= _WSNF_JOIN;
	  if (node_split_prefix (wsp, &tail, node, off, n, _WSNF_JOIN))
	    return false;
	  p++;
	  if (ws_exp_fn (wsp, p, slen - n, &tail, &p,
			 node->flags & (_WSNF_JOIN | _WSNF_QUOTE)))
	    return false;
	  off += p - str + 1;
	  str = p + 1;
	}
    }
  if (p > str)
    {
      if (tail != node)
	tail->flags |= _WSNF_JOIN;
      if (node_split_prefix (wsp, &tail, node, off, p - str,
			     node->flags & (_WSNF_JOIN|_WSNF_QUOTE)))
	return false;
    }
  if (tail != node)
    {
      wsnode_remove (wsp, node);
      wsnode_free (node);
    }
  return true;
}

/* Remove NULL nodes from the list */
static void
wsnode_nullelim (struct wordsplit *wsp)
{
  struct wordsplit_node *p;

  for (p = wsp->ws_head; p;)
    {
      struct wordsplit_node *next = p->next;
      if (p->flags & _WSNF_DELIM && p->prev)
	p->prev->flags &= ~_WSNF_JOIN;
      if (p->flags & _WSNF_NULL)
	{
	  wsnode_remove (wsp, p);
	  wsnode_free (p);
	}
      p = next;
    }
}

static int
wordsplit_varexp (struct wordsplit *wsp)
{
  struct wordsplit_node *p;

  for (p = wsp->ws_head; p;)
    {
      struct wordsplit_node *next = p->next;
      if (!(p->flags & (_WSNF_NOEXPAND|_WSNF_DELIM)))
	if (!node_expand (wsp, p, begin_var_p, expvar))
	  return 1;
      p = next;
    }

  wsnode_nullelim (wsp);
  return 0;
}

static bool
begin_cmd_p (char c)
{
  return c == '(';
}

static int
expcmd (struct wordsplit *wsp, char const *str, idx_t len,
	struct wordsplit_node **ptail, const char **pend, unsigned flg)
{
  int rc;
  idx_t j;
  char *value;
  struct wordsplit_node *newnode;

  str++;
  len--;

  if (find_closing_paren (str, 0, len, &j, "()"))
    {
      _wsplt_seterr (wsp, WRDSE_PAREN);
      return 1;
    }

  *pend = str + j;
  if (wsp->ws_options & WRDSO_ARGV)
    {
      struct wordsplit ws;

      rc = _wsplt_subsplit (wsp, &ws, str, j, WRDSF_WS | WRDSF_QUOTE, true);
      if (rc)
	{
	  _wsplt_seterr_sub (wsp, &ws);
	  wordsplit_free (&ws);
	  return 1;
	}
      rc = wsp->ws_command (&value, str, j, ws.ws_wordv, wsp->ws_closure);
      wordsplit_free (&ws);
    }
  else
    rc = wsp->ws_command (&value, str, j, NULL, wsp->ws_closure);

  if (rc == WRDSE_NOSPACE)
    return _wsplt_nomem (wsp);
  else if (rc)
    {
      if (rc == WRDSE_USERERR)
	{
	  if (wsp->ws_errno == WRDSE_USERERR)
	    free (wsp->ws_usererr);
	  wsp->ws_usererr = value;
	}
      _wsplt_seterr (wsp, rc);
      return 1;
    }

  if (value)
    {
      if (flg & _WSNF_QUOTE)
	{
	  newnode = wsnode_new (wsp);
	  if (!newnode)
	    return 1;
	  wsnode_insert (wsp, newnode, *ptail);
	  *ptail = newnode;
	  newnode->flags = _WSNF_WORD | _WSNF_NOEXPAND | flg;
	  newnode->v.word = value;
	}
      else if (!*value)
	{
	  free (value);
	  /* Empty string is a special case */
	  newnode = wsnode_new (wsp);
	  if (!newnode)
	    return 1;
	  wsnode_insert (wsp, newnode, *ptail);
	  *ptail = newnode;
	  newnode->flags = _WSNF_NULL;
	}
      else
	{
	  struct wordsplit ws;
	  int rc;

	  rc = _wsplt_subsplit (wsp, &ws, value, strlen (value),
				(WRDSF_NOVAR | WRDSF_NOCMD | WRDSF_WS
				 | WRDSF_QUOTE
				 | (WSP_RETURN_DELIMS (wsp)
				    ? WRDSF_RETURN_DELIMS : 0)),
				false);
	  free (value);
	  if (rc)
	    {
	      _wsplt_seterr_sub (wsp, &ws);
	      wordsplit_free (&ws);
	      return 1;
	    }
	  wsnode_insert (wsp, ws.ws_head, *ptail);
	  *ptail = ws.ws_tail;
	  ws.ws_head = ws.ws_tail = NULL;
	  wordsplit_free (&ws);
	}
    }
  else
    {
      newnode = wsnode_new (wsp);
      if (!newnode)
	return 1;
      wsnode_insert (wsp, newnode, *ptail);
      *ptail = newnode;
      newnode->flags = _WSNF_NULL;
    }
  return 0;
}

static int
wordsplit_cmdexp (struct wordsplit *wsp)
{
  struct wordsplit_node *p;

  for (p = wsp->ws_head; p;)
    {
      struct wordsplit_node *next = p->next;
      if (!(p->flags & _WSNF_NOEXPAND))
	if (!node_expand (wsp, p, begin_cmd_p, expcmd))
	  return 1;
      p = next;
    }

  wsnode_nullelim (wsp);
  return 0;
}

/* Strip off any leading and trailing whitespace.  This function is called
   right after the initial scanning, therefore it assumes that every
   node in the list is a text reference node. */
static int
wordsplit_trimws (struct wordsplit *wsp)
{
  struct wordsplit_node *p;

  for (p = wsp->ws_head; p; p = p->next)
    {
      idx_t n;

      if (!(p->flags & _WSNF_QUOTE))
	{
	  /* Skip leading whitespace: */
	  for (n = p->v.segm.beg; n < p->v.segm.end && ISWS (wsp->ws_input[n]);
	       n++)
	    ;
	  p->v.segm.beg = n;
	}

      while (p->next && (p->flags & _WSNF_JOIN))
	p = p->next;

      if (p->flags & _WSNF_QUOTE)
	continue;

      /* Trim trailing whitespace */
      for (n = p->v.segm.end;
	   n > p->v.segm.beg && ISWS (wsp->ws_input[n - 1]); n--);
      p->v.segm.end = n;
      if (p->v.segm.beg == p->v.segm.end)
	p->flags |= _WSNF_NULL;
    }

  wsnode_nullelim (wsp);
  return 0;
}

static int
wordsplit_tildexpand (struct wordsplit *wsp)
{
  struct wordsplit_node *p;
  char *uname = NULL;
  idx_t usize = 0;

  for (p = wsp->ws_head; p; p = p->next)
    {
      const char *str;

      if (p->flags & _WSNF_QUOTE)
	continue;

      str = wsnode_ptr (wsp, p);
      if (str[0] == '~')
	{
	  idx_t i;
	  idx_t slen = wsnode_len (p);
	  struct passwd *pw;

	  for (i = 1; i < slen && str[i] != '/'; i++)
	    ;
	  if (i == slen)
	    continue;
	  if (i > 1)
	    {
	      if (i > usize)
		{
		  if (ckd_add (&usize, usize, usize >> 1) || usize < i)
		    usize = i;
		  char *p = irealloc (uname, usize);
		  if (!p)
		    {
		      free (uname);
		      return _wsplt_nomem (wsp);
		    }
		  uname = p;
		}
	      --i;
	      memcpy (uname, str + 1, i);
	      uname[i] = '\0';
	      pw = getpwnam (uname);
	    }
	  else
	    pw = getpwuid (getuid ());

	  if (!pw)
	    continue;

	  idx_t dlen = strlen (pw->pw_dir);
	  idx_t size = slen - i + dlen;
	  char *newstr = imalloc (size);
	  if (!newstr)
	    {
	      free (uname);
	      return _wsplt_nomem (wsp);
	    }
	  --size;

	  memcpy (newstr, pw->pw_dir, dlen);
	  memcpy (newstr + dlen, str + i + 1, slen - i - 1);
	  newstr[size] = '\0';
	  if (p->flags & _WSNF_WORD)
	    free (p->v.word);
	  p->v.word = newstr;
	  p->flags |= _WSNF_WORD;
	}
    }
  free (uname);
  return 0;
}

static bool
isglob (char const *s, idx_t l)
{
  for (ptrdiff_t i = l; i--; )
    {
      char c = *s++;
      if (c && strchr ("*?[", c))
	return true;
    }
  return false;
}

static int
wordsplit_pathexpand (struct wordsplit *wsp)
{
  struct wordsplit_node *p, *next;
  idx_t slen;
  int flags = 0;

#ifdef GLOB_PERIOD
  if (wsp->ws_options & WRDSO_DOTGLOB)
    flags = GLOB_PERIOD;
#endif

  for (p = wsp->ws_head; p; p = next)
    {
      const char *str;

      next = p->next;

      if (p->flags & _WSNF_QUOTE)
	continue;

      str = wsnode_ptr (wsp, p);
      slen = wsnode_len (p);

      if (isglob (str, slen))
	{
	  int i;
	  glob_t g;
	  struct wordsplit_node *prev;

	  char *pattern = imalloc (slen + 1);
	  if (!pattern)
	    return _wsplt_nomem (wsp);
	  memcpy (pattern, str, slen);
	  pattern[slen] = '\0';

	  switch (glob (pattern, flags, NULL, &g))
	    {
	    case 0:
	      free (pattern);
	      break;

	    case GLOB_NOSPACE:
	      free (pattern);
	      return _wsplt_nomem (wsp);

	    case GLOB_NOMATCH:
	      if (wsp->ws_options & WRDSO_NULLGLOB)
		{
		  wsnode_remove (wsp, p);
		  wsnode_free (p);
		}
	      else if (wsp->ws_options & WRDSO_FAILGLOB)
		{
		  if (wsp->ws_errno == WRDSE_USERERR)
		    free (wsp->ws_usererr);
		  char const *msg = _("no files match pattern ");
		  idx_t msglen = strlen (msg);
		  char *usererr = irealloc (pattern, msglen + slen + 1);
		  if (!usererr)
		    {
		      free (pattern);
		      return _wsplt_nomem (wsp);
		    }
		  memmove (usererr + msglen, usererr, slen + 1);
		  wsp->ws_usererr = memcpy (usererr, msg, msglen);
		  return _wsplt_seterr (wsp, WRDSE_USERERR);
		}
	      free (pattern);
	      continue;

	    default:
	      free (pattern);
	      return _wsplt_seterr (wsp, WRDSE_GLOBERR);
	    }

	  prev = p;
	  for (i = 0; i < g.gl_pathc; i++)
	    {
	      struct wordsplit_node *newnode = wsnode_new (wsp);
	      char *newstr;

	      if (!newnode)
		return 1;
	      newstr = strdup (g.gl_pathv[i]);
	      if (!newstr)
		{
		  wsnode_free (newnode);
		  return _wsplt_nomem (wsp);
		}
	      newnode->v.word = newstr;
	      newnode->flags |= _WSNF_WORD|_WSNF_QUOTE;
	      wsnode_insert (wsp, newnode, prev);
	      prev = newnode;
	    }
	  globfree (&g);

	  wsnode_remove (wsp, p);
	  wsnode_free (p);
	}
    }
  return 0;
}

static int
skip_sed_expr (char const *command, idx_t i, idx_t len)
{
  int state;

  do
    {
      char delim;

      if (command[i] == ';')
	i++;
      if (!(command[i] == 's' && i + 3 < len && c_ispunct (command[i + 1])))
	break;

      delim = command[++i];
      state = 1;
      for (i++; i < len; i++)
	{
	  if (state == 3)
	    {
	      if (command[i] == delim || !c_isalnum (command[i]))
		break;
	    }
	  else if (command[i] == '\\')
	    i++;
	  else if (command[i] == delim)
	    state++;
	}
    }
  while (state == 3 && i < len && command[i] == ';');
  return i;
}

/* wsp->ws_endp points to a delimiter character. If RETURN_DELIMS
   is true, return its value, otherwise return the index past it. */
static idx_t
skip_delim_internal (struct wordsplit *wsp, bool return_delims)
{
  return wsp->ws_endp + !return_delims;
}

static idx_t
skip_delim (struct wordsplit *wsp)
{
  return skip_delim_internal (wsp, WSP_RETURN_DELIMS (wsp));
}

static idx_t
skip_delim_real (struct wordsplit *wsp)
{
  return skip_delim_internal (wsp, !!(wsp->ws_flags & WRDSF_RETURN_DELIMS));
}

#define _WRDS_EOF   0
#define _WRDS_OK    1
#define _WRDS_ERR   2

static bool
scan_qstring (struct wordsplit *wsp, idx_t start, idx_t *end)
{
  idx_t j;
  const char *command = wsp->ws_input;
  idx_t len = wsp->ws_len;
  char q = command[start];

  for (j = start + 1; j < len && command[j] != q; j++)
    if (q == '"' && command[j] == '\\')
      j++;
  if (j < len && command[j] == q)
    {
      unsigned flags = _WSNF_QUOTE | _WSNF_EMPTYOK;
      if (q == '\'')
	flags |= _WSNF_NOEXPAND;
      if (!wordsplit_add_segm (wsp, start + 1, j, flags))
	return false;
      *end = j;
    }
  else
    {
      wsp->ws_endp = start;
      _wsplt_seterr (wsp, WRDSE_QUOTE);
      return false;
    }
  return true;
}

static int
scan_word (struct wordsplit *wsp, idx_t start, bool consume_all)
{
  idx_t len = wsp->ws_len;
  const char *command = wsp->ws_input;
  const char *comment = wsp->ws_comment;
  bool join = false;
  unsigned flags = 0;
  struct wordsplit_node *np = wsp->ws_tail;

  idx_t i = start;

  if (i >= len)
    {
      wsp->ws_errno = WRDSE_EOF;
      return _WRDS_EOF;
    }

  start = i;

  if (wsp->ws_flags & WRDSF_SED_EXPR
      && command[i] == 's' && i + 3 < len && c_ispunct (command[i + 1]))
    {
      flags = _WSNF_SEXP;
      i = skip_sed_expr (command, i, len);
    }
  else if (consume_all || !ISDELIM (wsp, command[i]))
    {
      while (i < len)
	{
	  if (comment && command[i] && strchr (comment, command[i]) != NULL)
	    {
	      idx_t j;
	      for (j = i + 1; j < len && command[j] != '\n'; j++)
		;
	      if (!wordsplit_add_segm (wsp, start, i, 0))
		return _WRDS_ERR;
	      wsp->ws_endp = j;
	      return _WRDS_OK;
	    }

	  if (wsp->ws_flags & WRDSF_QUOTE)
	    {
	      if (command[i] == '\\')
		{
		  if (++i == len)
		    break;
		  i++;
		  continue;
		}

	      if (((wsp->ws_flags & WRDSF_SQUOTE) && command[i] == '\'') ||
		  ((wsp->ws_flags & WRDSF_DQUOTE) && command[i] == '"'))
		{
		  if (join && wsp->ws_tail)
		    wsp->ws_tail->flags |= _WSNF_JOIN;
		  if (!wordsplit_add_segm (wsp, start, i, _WSNF_JOIN))
		    return _WRDS_ERR;
		  if (!scan_qstring (wsp, i, &i))
		    return _WRDS_ERR;
		  start = i + 1;
		  join = true;
		}
	    }

	  if (command[i] == '$')
	    {
	      if (!(wsp->ws_flags & WRDSF_NOVAR)
		  && command[i+1] == '{'
		  && !find_closing_paren (command, i + 2, len, &i, "{}"))
		continue;
	      if (!(wsp->ws_flags & WRDSF_NOCMD)
		  && command[i+1] == '('
		  && !find_closing_paren (command, i + 2, len, &i, "()"))
		continue;
	    }

	  if (!consume_all && ISDELIM (wsp, command[i]))
	    break;
	  else
	    i++;
	}
    }
  else if (WSP_RETURN_DELIMS (wsp))
    {
      i++;
      flags |= _WSNF_DELIM;
    }
  else if (!(wsp->ws_flags & WRDSF_SQUEEZE_DELIMS))
    flags |= _WSNF_EMPTYOK;

  if (join && i > start && wsp->ws_tail)
    wsp->ws_tail->flags |= _WSNF_JOIN;
  if (!wordsplit_add_segm (wsp, start, i, flags))
    return _WRDS_ERR;
  wsp->ws_endp = i;
  if (wsp->ws_flags & WRDSF_INCREMENTAL)
    return _WRDS_EOF;

  if (consume_all)
    {
      if (!np)
	np = wsp->ws_head;
      while (np)
	{
	  np->flags |= _WSNF_QUOTE;
	  np = np->next;
	}
    }

  return _WRDS_OK;
}

static int
xtonum (char *pval, char const *src, int base, int cnt)
{
  int i;
  unsigned char val = 0;

  /* The maximum value that a prefix of a number can represent.
     This is 31 if base is 8 and UCHAR_MAX == 255,
     so that "\400" is treated as "\40" followed by "0", not as "\000".  */
  unsigned char max_prefix = UCHAR_MAX / base;

  for (i = 0; i < cnt && val <= max_prefix; i++)
    {
      unsigned char c = src[i];
      unsigned char digit;

      if (c_isdigit (c))
	digit = c - '0';
      else if (c_isxdigit (c))
	digit = c_toupper (c) - 'A' + 10;
      else
	break;

      if (base <= digit)
	break;
      val = val * base + digit;
    }
  *pval = val;
  return i;
}

#ifdef _WORDSPLIT_EXTRAS
idx_t
wordsplit_c_quoted_length (const char *str, bool quote_hex, bool *quote)
{
  idx_t len = 0;

  *quote = false;
  for (; *str; str++)
    {
      if (strchr (" \"", *str))
	*quote = true;

      if (*str == ' ')
	len++;
      else if (*str == '"')
	len += 2;
      else if (*str != '\t' && *str != '\\' && c_isprint (*str))
	len++;
      else if (quote_hex)
	len += 3;
      else
	{
	  if (wordsplit_c_quote_char (*str))
	    len += 2;
	  else
	    len += 4;
	}
    }
  return len;
}
#endif

static char
wsplt_unquote_char (const char *transtab, char c)
{
  while (*transtab && transtab[1])
    {
      if (*transtab++ == c)
	return *transtab;
      ++transtab;
    }
  return '\0';
}

#ifdef _WORDSPLIT_EXTRAS
static char
wsplt_quote_char (const char *transtab, char c)
{
  for (; *transtab && transtab[1]; transtab += 2)
    {
      if (transtab[1] == c)
	return *transtab;
    }
  return '\0';
}

char
wordsplit_c_unquote_char (char c)
{
  return wsplt_unquote_char (wordsplit_c_escape_tab, c);
}

char
wordsplit_c_quote_char (char c)
{
  return wsplt_quote_char (wordsplit_c_escape_tab, c);
}
#endif

void
wordsplit_string_unquote_copy (struct wordsplit *ws, bool inquote,
			       char *dst, char const *src, idx_t n)
{
  for (idx_t i = 0; i < n; )
    {
      if (src[i] == '\\')
	{
	  char c;
	  ++i;
	  if (WRDSO_ESC_TEST (ws, inquote, WRDSO_XESC)
	      && (src[i] == 'x' || src[i] == 'X'))
	    {
	      if (n - i < 2)
		{
		  *dst++ = '\\';
		  *dst++ = src[i++];
		}
	      else
		{
		  int off = xtonum (&c, src + i + 1, 16, 2);
		  if (off == 0)
		    {
		      *dst++ = '\\';
		      *dst++ = src[i++];
		    }
		  else
		    {
		      *dst++ = c;
		      i += off + 1;
		    }
		}
	    }
	  else if (WRDSO_ESC_TEST (ws, inquote, WRDSO_OESC)
		   && c_isdigit (src[i]))
	    {
	      if (n - i < 1)
		{
		  *dst++ = '\\';
		  *dst++ = src[i++];
		}
	      else
		{
		  int off = xtonum (&c, src + i, 8, 3);
		  if (off == 0)
		    {
		      *dst++ = '\\';
		      *dst++ = src[i++];
		    }
		  else
		    {
		      *dst++ = c;
		      i += off;
		    }
		}
	    }
	  else if ((c = wsplt_unquote_char (ws->ws_escape[inquote], src[i])))
	    {
	      *dst++ = c;
	      ++i;
	    }
	  else
	    {
	      if (WRDSO_ESC_TEST (ws, inquote, WRDSO_BSKEEP))
		*dst++ = '\\';
	      *dst++ = src[i++];
	    }
	}
      else
	*dst++ = src[i++];
    }
  *dst = '\0';
}

#ifdef _WORDSPLIT_EXTRAS
void
wordsplit_c_quote_copy (char *dst, const char *src, bool quote_hex)
{
  for (; *src; src++)
    {
      if (*src == '"')
	{
	  *dst++ = '\\';
	  *dst++ = *src;
	}
      else if (*src != '\t' && *src != '\\' && c_isprint (*src))
	*dst++ = *src;
      else
	{
	  unsigned char uc = *src;

	  if (quote_hex)
	    {
	      static char const hexdigit[16] = "0123456789ABCDEF";
	      *dst++ = '%';
	      for (int i = 4; 0 <= i; i -= 4)
		*dst++ = hexdigit[(uc >> i) & 0xf];
	    }
	  else
	    {
	      char c = wordsplit_c_quote_char (*src);
	      *dst++ = '\\';
	      if (c)
		*dst++ = c;
	      else
		for (int i = 6; 0 <= i; i -= 3)
		  *dst++ = '0' + ((uc >> i) & 7);
	    }
	}
    }
}
#endif


/* This structure describes a single expansion phase */
struct exptab
{
  char const *descr; /* Textual description (for debugging) */
  int flag;          /* WRDSF_ bit that controls this phase */
  int opt;           /* Entry-specific options (see EXPOPT_ flags below */
  int (*expansion) (struct wordsplit *wsp); /* expansion function */
};

/* The following options control expansions: */
/* Normally the exptab entry is run if its flag bit is set in struct
   wordsplit.  The EXPOPT_NEG option negates this test so that expansion
   is performed if its associated flag bit is not set in struct wordsplit. */
#define EXPOPT_NEG      0x01
/* All bits in flag must be set in order for entry to match */
#define EXPORT_ALLOF    0x02
/* Coalesce the input list before running the expansion. */
#define EXPOPT_COALESCE 0x04

static struct exptab exptab[] = {
  { N_("WS trimming"),          WRDSF_WS,         0,
    wordsplit_trimws },
  { N_("command substitution"), WRDSF_NOCMD,      EXPOPT_NEG|EXPOPT_COALESCE,
    wordsplit_cmdexp },
  { N_("coalesce list"),        0,                EXPOPT_NEG|EXPOPT_COALESCE,
    NULL },
  { N_("tilde expansion"),      WRDSF_PATHEXPAND, 0,
    wordsplit_tildexpand },
  { N_("variable expansion"),   WRDSF_NOVAR,      EXPOPT_NEG,
    wordsplit_varexp },
  { N_("quote removal"),        0,                EXPOPT_NEG,
    wsnode_quoteremoval },
  { N_("coalesce list"),        0,                EXPOPT_NEG|EXPOPT_COALESCE,
    NULL },
  { N_("path expansion"),       WRDSF_PATHEXPAND, 0,
    wordsplit_pathexpand },
  { NULL }
};

static bool
exptab_matches (struct exptab *p, struct wordsplit *wsp)
{
  int result;

  result = (wsp->ws_flags & p->flag);
  if (p->opt & EXPORT_ALLOF)
    result = result == p->flag;
  if (p->opt & EXPOPT_NEG)
    result = !result;

  return !!result;
}

static int
wordsplit_process_list (struct wordsplit *wsp, idx_t start)
{
  struct exptab *p;

  if (wsp->ws_flags & WRDSF_SHOWDBG)
    wsp->ws_debug (_("(%02td) Input:%.*s%s;"),
		   wsp->ws_lvl, printflen (wsp->ws_len), wsp->ws_input,
		   printfdots (wsp->ws_len));

  if ((wsp->ws_flags & WRDSF_NOSPLIT)
      || ((wsp->ws_options & WRDSO_MAXWORDS)
	  && wsp->ws_wordi + 1 == wsp->ws_maxwords))
    {
      /* Treat entire input as a single word */
      if (scan_word (wsp, start, true) == _WRDS_ERR)
	return wsp->ws_errno;
    }
  else
    {
      int rc;

      while ((rc = scan_word (wsp, start, false)) == _WRDS_OK)
	start = skip_delim (wsp);
      /* Make sure tail element is not joinable */
      if (wsp->ws_tail)
	wsp->ws_tail->flags &= ~_WSNF_JOIN;
      if (rc == _WRDS_ERR)
	return wsp->ws_errno;
    }

  if (wsp->ws_flags & WRDSF_SHOWDBG)
    {
      wsp->ws_debug ("(%02td) %s", wsp->ws_lvl, _("Initial list:"));
      wordsplit_dump_nodes (wsp);
    }

  for (p = exptab; p->descr; p++)
    {
      if (exptab_matches (p, wsp))
	{
	  if (p->opt & EXPOPT_COALESCE)
	    {
	      if (wsnode_coalesce (wsp))
		break;
	      if (wsp->ws_flags & WRDSF_SHOWDBG)
		{
		  wsp->ws_debug ("(%02td) %s", wsp->ws_lvl,
				 _("Coalesced list:"));
		  wordsplit_dump_nodes (wsp);
		}
	    }
	  if (p->expansion)
	    {
	      if (p->expansion (wsp))
		break;
	      if (wsp->ws_flags & WRDSF_SHOWDBG)
		{
		  wsp->ws_debug ("(%02td) %s", wsp->ws_lvl, _(p->descr));
		  wordsplit_dump_nodes (wsp);
		}
	    }
	}
    }
  return wsp->ws_errno;
}

WORDSPLIT_EXTRAS_extern
int
wordsplit_len (char const *command, idx_t length, struct wordsplit *wsp,
	       unsigned flags)
{
  int rc;
  idx_t start;

  if (!command)
    {
      if (!(flags & WRDSF_INCREMENTAL))
	return _wsplt_seterr (wsp, WRDSE_USAGE);

      if (wsp->ws_head)
	return wordsplit_finish (wsp);

      start = skip_delim_real (wsp);
      if (wsp->ws_endp == wsp->ws_len)
	return _wsplt_seterr (wsp, WRDSE_NOINPUT);

      wsp->ws_flags |= WRDSF_REUSE;
      wordsplit_init0 (wsp);
    }
  else
    {
      start = 0;
      rc = wordsplit_init (wsp, command, length, flags);
      if (rc)
	return rc;
      wsp->ws_lvl = 0;
    }

  rc = wordsplit_process_list (wsp, start);
  if (rc)
    return rc;
  return wordsplit_finish (wsp);
}

int
wordsplit (const char *command, struct wordsplit *ws, unsigned flags)
{
  return wordsplit_len (command, command ? strlen (command) : 0, ws, flags);
}

WORDSPLIT_EXTRAS_extern
void
wordsplit_free_words (struct wordsplit *ws)
{
  idx_t i;

  for (i = 0; i < ws->ws_wordc; i++)
    {
      char *p = ws->ws_wordv[ws->ws_offs + i];
      if (p)
	{
	  free (p);
	  ws->ws_wordv[ws->ws_offs + i] = NULL;
	}
    }
  ws->ws_wordc = 0;
}

WORDSPLIT_EXTRAS_extern
void
wordsplit_free_envbuf (struct wordsplit *ws)
{
  if (ws->ws_flags & WRDSF_NOCMD)
    return;
  if (ws->ws_envbuf)
    {
      idx_t i;

      for (i = 0; ws->ws_envbuf[i]; i++)
	free (ws->ws_envbuf[i]);
      free (ws->ws_envbuf);
      ws->ws_envidx = ws->ws_envsiz = 0;
      ws->ws_envbuf = NULL;
    }
}

WORDSPLIT_EXTRAS_extern
void
wordsplit_clearerr (struct wordsplit *ws)
{
  if (ws->ws_errno == WRDSE_USERERR)
    free (ws->ws_usererr);
  ws->ws_usererr = NULL;
  ws->ws_errno = WRDSE_OK;
}

void
wordsplit_free (struct wordsplit *ws)
{
  wordsplit_free_nodes (ws);
  wordsplit_free_words (ws);
  free (ws->ws_wordv);
  ws->ws_wordv = NULL;
  wordsplit_free_envbuf (ws);
}

#ifdef _WORDSPLIT_EXTRAS
void
wordsplit_get_words (struct wordsplit *ws, idx_t *wordc, char ***wordv)
{
  /* Tell the memory manager that ws->ws_wordv can be shrunk.  */
  char **p = irealloc (ws->ws_wordv,
		       (ws->ws_wordc + 1) * sizeof (ws->ws_wordv[0]));
  *wordv = p ? p : ws->ws_wordv;
  *wordc = ws->ws_wordc;

  ws->ws_wordv = NULL;
  ws->ws_wordc = 0;
  ws->ws_wordn = 0;
}
#endif

static char const *const wordsplit_errstr[] = {
  N_("no error"),
  N_("missing closing quote"),
  N_("memory exhausted"),
  N_("invalid wordsplit usage"),
  N_("unbalanced curly brace"),
  N_("undefined variable"),
  N_("input exhausted"),
  N_("unbalanced parenthesis"),
  N_("globbing error")
};
enum { wordsplit_nerrs = sizeof wordsplit_errstr / sizeof *wordsplit_errstr };

const char *
wordsplit_strerror (struct wordsplit const *ws)
{
  if (ws->ws_errno == WRDSE_USERERR)
    return ws->ws_usererr;
  if (ws->ws_errno < wordsplit_nerrs)
    return wordsplit_errstr[ws->ws_errno];
  return N_("unknown error");
}

WORDSPLIT_EXTRAS_extern
void
wordsplit_perror (struct wordsplit *wsp)
{
  switch (wsp->ws_errno)
    {
    case WRDSE_QUOTE:
      wsp->ws_error (_("missing closing %c (start near #%td)"),
		     wsp->ws_input[wsp->ws_endp],
		     wsp->ws_endp);
      break;

    default:
      wsp->ws_error ("%s", wordsplit_strerror (wsp));
    }
}
