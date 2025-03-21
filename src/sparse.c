/* Functions for dealing with sparse files

   Copyright 2003-2025 Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; either version 3, or (at your option) any later
   version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
   Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include <system.h>
#include <c-ctype.h>
#include <inttostr.h>
#include <quotearg.h>
#include "common.h"

struct tar_sparse_file;
static bool sparse_select_optab (struct tar_sparse_file *file);

enum sparse_scan_state
  {
    scan_begin,
    scan_block,
    scan_end
  };

struct tar_sparse_optab
{
  bool (*init) (struct tar_sparse_file *);
  bool (*done) (struct tar_sparse_file *);
  bool (*sparse_member_p) (struct tar_sparse_file *);
  bool (*dump_header) (struct tar_sparse_file *);
  bool (*fixup_header) (struct tar_sparse_file *);
  bool (*decode_header) (struct tar_sparse_file *);
  bool (*scan_block) (struct tar_sparse_file *, enum sparse_scan_state,
		      void *);
  bool (*dump_region) (struct tar_sparse_file *, idx_t);
  bool (*extract_region) (struct tar_sparse_file *, idx_t);
};

struct tar_sparse_file
{
  int fd;                           /* File descriptor */
  bool seekable;                    /* Is fd seekable? */
  off_t offset;                     /* Current offset in fd if seekable==false.
				       Otherwise unused */
  off_t dumped_size;                /* Number of bytes actually written
				       to the archive */
  struct tar_stat_info *stat_info;  /* Information about the file */
  struct tar_sparse_optab const *optab; /* Operation table */
  void *closure;                    /* Any additional data optab calls might
				       require */
};

/* Dump zeros to file->fd until offset is reached. It is used instead of
   lseek if the output file is not seekable */
static bool
dump_zeros (struct tar_sparse_file *file, off_t offset)
{
  static char const zero_buf[BLOCKSIZE];

  if (offset < file->offset)
    {
      errno = EINVAL;
      return false;
    }

  while (file->offset < offset)
    {
      idx_t size = min (BLOCKSIZE, offset - file->offset);
      ssize_t wrbytes = write (file->fd, zero_buf, size);
      if (wrbytes <= 0)
	{
	  if (wrbytes == 0)
	    errno = EINVAL;
	  return false;
	}
      file->offset += wrbytes;
    }

  return true;
}

static bool
tar_sparse_member_p (struct tar_sparse_file *file)
{
  if (file->optab->sparse_member_p)
    return file->optab->sparse_member_p (file);
  return false;
}

static bool
tar_sparse_init (struct tar_sparse_file *file)
{
  memset (file, 0, sizeof *file);

  if (!sparse_select_optab (file))
    return false;

  if (file->optab->init)
    return file->optab->init (file);

  return true;
}

static bool
tar_sparse_done (struct tar_sparse_file *file)
{
  if (file->optab->done)
    return file->optab->done (file);
  return true;
}

static bool
tar_sparse_scan (struct tar_sparse_file *file, enum sparse_scan_state state,
		 void *block)
{
  if (file->optab->scan_block)
    return file->optab->scan_block (file, state, block);
  return true;
}

static bool
tar_sparse_dump_region (struct tar_sparse_file *file, idx_t i)
{
  if (file->optab->dump_region)
    return file->optab->dump_region (file, i);
  return false;
}

static bool
tar_sparse_extract_region (struct tar_sparse_file *file, idx_t i)
{
  if (file->optab->extract_region)
    return file->optab->extract_region (file, i);
  return false;
}

static bool
tar_sparse_dump_header (struct tar_sparse_file *file)
{
  if (file->optab->dump_header)
    return file->optab->dump_header (file);
  return false;
}

static bool
tar_sparse_decode_header (struct tar_sparse_file *file)
{
  if (file->optab->decode_header)
    return file->optab->decode_header (file);
  return true;
}

static bool
tar_sparse_fixup_header (struct tar_sparse_file *file)
{
  if (file->optab->fixup_header)
    return file->optab->fixup_header (file);
  return true;
}


static bool
lseek_or_error (struct tar_sparse_file *file, off_t offset)
{
  if (file->seekable
      ? lseek (file->fd, offset, SEEK_SET) < 0
      : ! dump_zeros (file, offset))
    {
      seek_diag_details (file->stat_info->orig_file_name, offset);
      return false;
    }
  return true;
}

/* Takes a blockful of data and basically cruises through it to see if
   it's made *entirely* of zeros, returning a 0 the instant it finds
   something that is a nonzero, i.e., useful data.  */
static bool
zero_block_p (char const *buffer, idx_t size)
{
  for (; size; size--)
    if (*buffer++)
      return false;
  return true;
}

static void
sparse_add_map (struct tar_stat_info *st, struct sp_array const *sp)
{
  struct sp_array *sparse_map = st->sparse_map;
  idx_t avail = st->sparse_map_avail;
  if (avail == st->sparse_map_size)
    st->sparse_map = sparse_map =
      xpalloc (sparse_map, &st->sparse_map_size, 1, -1, sizeof *sparse_map);
  sparse_map[avail] = *sp;
  st->sparse_map_avail = avail + 1;
}

/* Scan the sparse file byte-by-byte and create its map. */
static bool
sparse_scan_file_raw (struct tar_sparse_file *file)
{
  struct tar_stat_info *st = file->stat_info;
  int fd = file->fd;
  char buffer[BLOCKSIZE];
  off_t offset = 0;
  struct sp_array sp = {0, 0};

  st->archive_file_size = 0;

  if (!tar_sparse_scan (file, scan_begin, NULL))
    return false;

  while (true)
    {
      idx_t count = blocking_read (fd, buffer, sizeof buffer);
      if (count < sizeof buffer)
	{
	  if (errno)
	    read_diag_details (st->orig_file_name, offset, sizeof buffer);
	  if (count == 0)
	    break;
	}

      /* Analyze the block.  */
      if (zero_block_p (buffer, count))
        {
          if (sp.numbytes)
            {
              sparse_add_map (st, &sp);
              sp.numbytes = 0;
              if (!tar_sparse_scan (file, scan_block, NULL))
                return false;
            }
        }
      else
        {
          if (sp.numbytes == 0)
            sp.offset = offset;
          sp.numbytes += count;
          st->archive_file_size += count;
          if (!tar_sparse_scan (file, scan_block, buffer))
            return false;
        }

      offset += count;
      if (count < sizeof buffer)
	break;
    }

  /* save one more sparse segment of length 0 to indicate that
     the file ends with a hole */
  if (sp.numbytes == 0)
    sp.offset = offset;

  sparse_add_map (st, &sp);
  return tar_sparse_scan (file, scan_end, NULL);
}

static bool
sparse_scan_file_wholesparse (struct tar_sparse_file *file)
{
  struct tar_stat_info *st = file->stat_info;
  struct sp_array sp = {0, 0};

  /* Note that this function is called only for truly sparse files of size >= 1
     block size (checked via ST_IS_SPARSE before).  See the thread
     http://www.mail-archive.com/bug-tar@gnu.org/msg04209.html for more info */
  if (ST_NBLOCKS (st->stat) == 0)
    {
      st->archive_file_size = 0;
      sp.offset = st->stat.st_size;
      sparse_add_map (st, &sp);
      return true;
    }

  return false;
}

#ifdef SEEK_HOLE
/* Try to engage SEEK_HOLE/SEEK_DATA feature. */
static bool
sparse_scan_file_seek (struct tar_sparse_file *file)
{
  struct tar_stat_info *st = file->stat_info;
  int fd = file->fd;
  struct sp_array sp = {0, 0};
  off_t offset = 0;
  off_t data_offset;
  off_t hole_offset;

  st->archive_file_size = 0;

  for (;;)
    {
      /* locate first chunk of data */
      data_offset = lseek (fd, offset, SEEK_DATA);

      if (data_offset < 0)
        /* ENXIO == EOF; error otherwise */
        {
          if (errno == ENXIO)
            {
              /* file ends with hole, add one more empty chunk of data */
              sp.numbytes = 0;
              sp.offset = st->stat.st_size;
              sparse_add_map (st, &sp);
              return true;
            }
          return false;
        }

      hole_offset = lseek (fd, data_offset, SEEK_HOLE);

      /* according to specs, if FS does not fully support
	 SEEK_DATA/SEEK_HOLE it may just implement kind of "wrapper" around
	 classic lseek() call.  We must detect it here and try to use other
	 hole-detection methods. */
      if (offset == 0 /* first loop */
          && data_offset == 0
          && hole_offset == st->stat.st_size)
        {
          lseek (fd, 0, SEEK_SET);
          return false;
        }

      sp.offset = data_offset;
      sp.numbytes = hole_offset - data_offset;
      sparse_add_map (st, &sp);

      st->archive_file_size += sp.numbytes;
      offset = hole_offset;
    }
}
#endif

static bool
sparse_scan_file (struct tar_sparse_file *file)
{
  /* always check for completely sparse files */
  if (sparse_scan_file_wholesparse (file))
    return true;

  switch (hole_detection)
    {
    case HOLE_DETECTION_DEFAULT:
    case HOLE_DETECTION_SEEK:
#ifdef SEEK_HOLE
      if (sparse_scan_file_seek (file))
        return true;
#else
      if (hole_detection == HOLE_DETECTION_SEEK)
	paxwarn (0, _("\"seek\" hole detection is not supported, using \"raw\"."));
      /* fall back to "raw" for this and all other files */
      hole_detection = HOLE_DETECTION_RAW;
#endif
      FALLTHROUGH;
    case HOLE_DETECTION_RAW:
      if (sparse_scan_file_raw (file))
	return true;
    }

  return false;
}

static struct tar_sparse_optab const oldgnu_optab;
static struct tar_sparse_optab const star_optab;
static struct tar_sparse_optab const pax_optab;

static bool
sparse_select_optab (struct tar_sparse_file *file)
{
  switch (current_format == DEFAULT_FORMAT ? archive_format : current_format)
    {
    case V7_FORMAT:
    case USTAR_FORMAT:
      return false;

    case OLDGNU_FORMAT:
    case GNU_FORMAT: /*FIXME: This one should disappear? */
      file->optab = &oldgnu_optab;
      break;

    case POSIX_FORMAT:
      file->optab = &pax_optab;
      break;

    case STAR_FORMAT:
      file->optab = &star_optab;
      break;

    default:
      return false;
    }
  return true;
}

static bool
sparse_dump_region (struct tar_sparse_file *file, idx_t i)
{
  off_t bytes_left = file->stat_info->sparse_map[i].numbytes;

  if (!lseek_or_error (file, file->stat_info->sparse_map[i].offset))
    return false;

  while (bytes_left > 0)
    {
      union block *blk = find_next_block ();
      idx_t avail = available_space_after (blk);
      idx_t bufsize = min (bytes_left, avail);
      idx_t bytes_read = full_read (file->fd, blk->buffer, bufsize);
      if (bytes_read < BLOCKSIZE)
	memset (blk->buffer + bytes_read, 0, BLOCKSIZE - bytes_read);
      bytes_left -= bytes_read;
      file->dumped_size += bytes_read;

      if (bytes_read < bufsize)
	{
	  off_t current_offset = (file->stat_info->sparse_map[i].offset
				  + file->stat_info->sparse_map[i].numbytes
				  - bytes_left);
	  if (errno != 0)
	    read_diag_details (file->stat_info->orig_file_name,
			       current_offset, bufsize - bytes_read);
	  else
	    {
	      off_t cursize = current_offset;
	      struct stat st;
	      if (fstat (file->fd, &st) == 0 && st.st_size < cursize)
		cursize = st.st_size;
	      intmax_t n = file->stat_info->stat.st_size - cursize;
	      warnopt (WARN_FILE_SHRANK, 0,
		       ngettext ("%s: File shrank by %jd byte; padding with zeros",
				 "%s: File shrank by %jd bytes; padding with zeros",
				 n),
		       quotearg_colon (file->stat_info->orig_file_name),
		       n);
	    }
	  if (! ignore_failed_read_option)
	    set_exit_status (TAREXIT_DIFFERS);
	  return false;
	}

      set_next_block_after (blk + ((bufsize - 1) >> LG_BLOCKSIZE));
    }

  return true;
}

static bool
sparse_extract_region (struct tar_sparse_file *file, idx_t i)
{
  off_t write_size;

  if (!lseek_or_error (file, file->stat_info->sparse_map[i].offset))
    return false;

  write_size = file->stat_info->sparse_map[i].numbytes;

  if (write_size == 0)
    {
      /* Last block of the file is a hole */
      if (file->seekable && sys_truncate (file->fd))
	truncate_warn (file->stat_info->orig_file_name);
    }
  else while (write_size > 0)
    {
      union block *blk = find_next_block ();
      if (!blk)
	{
	  paxerror (0, _("Unexpected EOF in archive"));
	  return false;
	}
      idx_t avail = available_space_after (blk);
      idx_t wrbytes = min (write_size, avail);
      set_next_block_after (blk + ((wrbytes - 1) >> LG_BLOCKSIZE));
      file->dumped_size += avail;
      idx_t count = blocking_write (file->fd, blk->buffer, wrbytes);
      write_size -= count;
      mv_size_left (file->stat_info->archive_file_size - file->dumped_size);
      file->offset += count;
      if (count != wrbytes)
	{
	  write_error_details (file->stat_info->orig_file_name,
			       count, wrbytes);
	  return false;
	}
    }
  return true;
}



/* Interface functions */
enum dump_status
sparse_dump_file (int fd, struct tar_stat_info *st)
{
  bool rc;
  struct tar_sparse_file file;

  if (!tar_sparse_init (&file))
    return dump_status_not_implemented;

  file.stat_info = st;
  file.fd = fd;
  file.seekable = true; /* File *must* be seekable for dump to work */

  rc = sparse_scan_file (&file);
  if (rc && file.optab->dump_region)
    {
      tar_sparse_dump_header (&file);

      if (fd >= 0)
	{
	  mv_begin_write (file.stat_info->file_name,
		          file.stat_info->stat.st_size,
		          file.stat_info->archive_file_size - file.dumped_size);
	  for (idx_t i = 0; rc && i < file.stat_info->sparse_map_avail; i++)
	    rc = tar_sparse_dump_region (&file, i);
	}
    }

  pad_archive (file.stat_info->archive_file_size - file.dumped_size);
  return (tar_sparse_done (&file) && rc) ? dump_status_ok : dump_status_short;
}

bool
sparse_member_p (struct tar_stat_info *st)
{
  struct tar_sparse_file file;

  if (!tar_sparse_init (&file))
    return false;
  file.stat_info = st;
  return tar_sparse_member_p (&file);
}

bool
sparse_fixup_header (struct tar_stat_info *st)
{
  struct tar_sparse_file file;

  if (!tar_sparse_init (&file))
    return false;
  file.stat_info = st;
  return tar_sparse_fixup_header (&file);
}

enum dump_status
sparse_extract_file (int fd, struct tar_stat_info *st, off_t *size)
{
  bool rc = true;
  struct tar_sparse_file file;

  if (!tar_sparse_init (&file))
    {
      *size = st->stat.st_size;
      return dump_status_not_implemented;
    }

  file.stat_info = st;
  file.fd = fd;
  file.seekable = lseek (fd, 0, SEEK_SET) == 0;
  file.offset = 0;

  rc = tar_sparse_decode_header (&file);
  for (idx_t i = 0; rc && i < file.stat_info->sparse_map_avail; i++)
    rc = tar_sparse_extract_region (&file, i);
  *size = file.stat_info->archive_file_size - file.dumped_size;
  return (tar_sparse_done (&file) && rc) ? dump_status_ok : dump_status_short;
}

enum dump_status
sparse_skim_file (struct tar_stat_info *st, bool must_copy)
{
  bool rc = true;
  struct tar_sparse_file file;

  if (!tar_sparse_init (&file))
    return dump_status_not_implemented;

  file.stat_info = st;
  file.fd = -1;

  rc = tar_sparse_decode_header (&file);
  skim_file (file.stat_info->archive_file_size - file.dumped_size, must_copy);
  return (tar_sparse_done (&file) && rc) ? dump_status_ok : dump_status_short;
}


static bool
check_sparse_region (struct tar_sparse_file *file, off_t beg, off_t end)
{
  if (!lseek_or_error (file, beg))
    return false;

  while (beg < end)
    {
      idx_t rdsize = min (end - beg, BLOCKSIZE);
      char diff_buffer[BLOCKSIZE];

      idx_t bytes_read = full_read (file->fd, diff_buffer, rdsize);
      if (bytes_read < rdsize)
	{
	  if (errno)
	    read_diag_details (file->stat_info->orig_file_name, beg, rdsize);
	  else
	    report_difference (file->stat_info, _("Size differs"));
	  return false;
	}

      if (!zero_block_p (diff_buffer, bytes_read))
	{
	  char begbuf[INT_BUFSIZE_BOUND (off_t)];
 	  report_difference (file->stat_info,
			     _("File fragment at %s is not a hole"),
			     offtostr (beg, begbuf));
	  return false;
	}

      beg += bytes_read;
    }

  return true;
}

static bool
check_data_region (struct tar_sparse_file *file, idx_t i)
{
  off_t size_left;

  if (!lseek_or_error (file, file->stat_info->sparse_map[i].offset))
    return false;
  size_left = file->stat_info->sparse_map[i].numbytes;
  mv_size_left (file->stat_info->archive_file_size - file->dumped_size);

  while (size_left > 0)
    {
      char diff_buffer[BLOCKSIZE];

      union block *blk = find_next_block ();
      if (!blk)
	{
	  paxerror (0, _("Unexpected EOF in archive"));
	  return false;
	}
      set_next_block_after (blk);
      file->dumped_size += BLOCKSIZE;
      idx_t rdsize = min (size_left, BLOCKSIZE);
      idx_t bytes_read = full_read (file->fd, diff_buffer, rdsize);
      size_left -= bytes_read;
      mv_size_left (file->stat_info->archive_file_size - file->dumped_size);
      if (memcmp (blk->buffer, diff_buffer, bytes_read) != 0)
	{
	  report_difference (file->stat_info, _("Contents differ"));
	  return false;
	}

      if (bytes_read < rdsize)
	{
	  if (errno != 0)
	    read_diag_details (file->stat_info->orig_file_name,
			       (file->stat_info->sparse_map[i].offset
				+ file->stat_info->sparse_map[i].numbytes
				- size_left),
			       rdsize - bytes_read);
	  else
	    report_difference (&current_stat_info, _("Size differs"));
	  return false;
	}
    }
  return true;
}

bool
sparse_diff_file (int fd, struct tar_stat_info *st)
{
  bool rc = true;
  struct tar_sparse_file file;
  off_t offset = 0;

  if (!tar_sparse_init (&file))
    return false;

  file.stat_info = st;
  file.fd = fd;
  file.seekable = true; /* File *must* be seekable for compare to work */

  rc = tar_sparse_decode_header (&file);
  mv_begin_read (st);
  for (idx_t i = 0; rc && i < file.stat_info->sparse_map_avail; i++)
    {
      rc = check_sparse_region (&file,
				offset, file.stat_info->sparse_map[i].offset)
	    && check_data_region (&file, i);
      offset = file.stat_info->sparse_map[i].offset
	        + file.stat_info->sparse_map[i].numbytes;
    }

  if (!rc)
    skim_file (file.stat_info->archive_file_size - file.dumped_size, false);
  mv_end ();

  tar_sparse_done (&file);
  return rc;
}


/* Old GNU Format. The sparse file information is stored in the
   oldgnu_header in the following manner:

   The header is marked with type 'S'. Its 'size' field contains
   the cumulative size of all non-empty blocks of the file. The
   actual file size is stored in 'realsize' member of oldgnu_header.

   The map of the file is stored in a list of 'struct sparse'.
   Each struct contains offset to the block of data and its
   size (both as octal numbers). The first file header contains
   at most 4 such structs (SPARSES_IN_OLDGNU_HEADER). If the map
   contains more structs, then the field 'isextended' of the main
   header is set to 1 (binary) and the 'struct sparse_header'
   header follows, containing at most 21 following structs
   (SPARSES_IN_SPARSE_HEADER). If more structs follow, 'isextended'
   field of the extended header is set and next  next extension header
   follows, etc... */

enum oldgnu_add_status
  {
    add_ok,
    add_finish,
    add_fail
  };

static bool
oldgnu_sparse_member_p (MAYBE_UNUSED struct tar_sparse_file *file)
{
  return current_header->header.typeflag == GNUTYPE_SPARSE;
}

/* Add a sparse item to the sparse file and its obstack */
static enum oldgnu_add_status
oldgnu_add_sparse (struct tar_sparse_file *file, struct sparse *s)
{
  struct sp_array sp;

  if (s->numbytes[0] == '\0')
    return add_finish;
  sp.offset = OFF_FROM_HEADER (s->offset);
  sp.numbytes = OFF_FROM_HEADER (s->numbytes);
  off_t size;
  if (sp.offset < 0 || sp.numbytes < 0
      || ckd_add (&size, sp.offset, sp.numbytes)
      || file->stat_info->stat.st_size < size
      || file->stat_info->archive_file_size < 0)
    return add_fail;

  sparse_add_map (file->stat_info, &sp);
  return add_ok;
}

static bool
oldgnu_fixup_header (struct tar_sparse_file *file)
{
  /* NOTE! st_size was initialized from the header
     which actually contains archived size. The following fixes it */
  off_t realsize = OFF_FROM_HEADER (current_header->oldgnu_header.realsize);
  file->stat_info->archive_file_size = file->stat_info->stat.st_size;
  file->stat_info->stat.st_size = max (0, realsize);
  return 0 <= realsize;
}

/* Convert old GNU format sparse data to internal representation */
static bool
oldgnu_get_sparse_info (struct tar_sparse_file *file)
{
  union block *h = current_header;
  enum oldgnu_add_status rc;

  file->stat_info->sparse_map_avail = 0;
  for (idx_t i = 0; i < SPARSES_IN_OLDGNU_HEADER; i++)
    {
      rc = oldgnu_add_sparse (file, &h->oldgnu_header.sp[i]);
      if (rc != add_ok)
	break;
    }

  for (char ext_p = h->oldgnu_header.isextended;
       rc == add_ok && ext_p; ext_p = h->sparse_header.isextended)
    {
      h = find_next_block ();
      if (!h)
	{
	  paxerror (0, _("Unexpected EOF in archive"));
	  return false;
	}
      set_next_block_after (h);
      for (idx_t i = 0; i < SPARSES_IN_SPARSE_HEADER && rc == add_ok; i++)
	rc = oldgnu_add_sparse (file, &h->sparse_header.sp[i]);
    }

  if (rc == add_fail)
    {
      paxerror (0, _("%s: invalid sparse archive member"),
		file->stat_info->orig_file_name);
      return false;
    }
  return true;
}

static void
oldgnu_store_sparse_info (struct tar_sparse_file *file, idx_t *pindex,
			  struct sparse *sp, idx_t sparse_size)
{
  for (; *pindex < file->stat_info->sparse_map_avail
	 && sparse_size > 0; sparse_size--, sp++, ++*pindex)
    {
      OFF_TO_CHARS (file->stat_info->sparse_map[*pindex].offset,
		    sp->offset);
      OFF_TO_CHARS (file->stat_info->sparse_map[*pindex].numbytes,
		    sp->numbytes);
    }
}

static bool
oldgnu_dump_header (struct tar_sparse_file *file)
{
  off_t block_ordinal = current_block_ordinal ();
  union block *blk;

  blk = start_header (file->stat_info);
  blk->header.typeflag = GNUTYPE_SPARSE;
  if (file->stat_info->sparse_map_avail > SPARSES_IN_OLDGNU_HEADER)
    blk->oldgnu_header.isextended = 1;

  /* Store the real file size */
  OFF_TO_CHARS (file->stat_info->stat.st_size, blk->oldgnu_header.realsize);
  /* Store the effective (shrunken) file size */
  OFF_TO_CHARS (file->stat_info->archive_file_size, blk->header.size);

  idx_t i = 0;
  oldgnu_store_sparse_info (file, &i,
			    blk->oldgnu_header.sp,
			    SPARSES_IN_OLDGNU_HEADER);
  blk->oldgnu_header.isextended = i < file->stat_info->sparse_map_avail;
  finish_header (file->stat_info, blk, block_ordinal);

  while (i < file->stat_info->sparse_map_avail)
    {
      blk = find_next_block ();
      memset (blk->buffer, 0, BLOCKSIZE);
      oldgnu_store_sparse_info (file, &i,
				blk->sparse_header.sp,
				SPARSES_IN_SPARSE_HEADER);
      if (i < file->stat_info->sparse_map_avail)
	blk->sparse_header.isextended = 1;
      set_next_block_after (blk);
    }
  return true;
}

static struct tar_sparse_optab const oldgnu_optab = {
  NULL,  /* No init function */
  NULL,  /* No done function */
  oldgnu_sparse_member_p,
  oldgnu_dump_header,
  oldgnu_fixup_header,
  oldgnu_get_sparse_info,
  NULL,  /* No scan_block function */
  sparse_dump_region,
  sparse_extract_region,
};


/* Star */

static bool
star_sparse_member_p (MAYBE_UNUSED struct tar_sparse_file *file)
{
  return current_header->header.typeflag == GNUTYPE_SPARSE;
}

static bool
star_fixup_header (struct tar_sparse_file *file)
{
  /* NOTE! st_size was initialized from the header
     which actually contains archived size. The following fixes it */
  off_t realsize = OFF_FROM_HEADER (current_header->star_in_header.realsize);
  file->stat_info->archive_file_size = file->stat_info->stat.st_size;
  file->stat_info->stat.st_size = max (0, realsize);
  return 0 <= realsize;
}

/* Convert STAR format sparse data to internal representation */
static bool
star_get_sparse_info (struct tar_sparse_file *file)
{
  union block *h = current_header;
  char ext_p;
  enum oldgnu_add_status rc = add_ok;

  file->stat_info->sparse_map_avail = 0;

  if (h->star_in_header.prefix[0] == '\0'
      && h->star_in_header.sp[0].offset[10] != '\0')
    {
      /* Old star format */
      for (idx_t i = 0; i < SPARSES_IN_STAR_HEADER; i++)
	{
	  rc = oldgnu_add_sparse (file, &h->star_in_header.sp[i]);
	  if (rc != add_ok)
	    break;
	}
      ext_p = h->star_in_header.isextended;
    }
  else
    ext_p = 1;

  for (; rc == add_ok && ext_p; ext_p = h->star_ext_header.isextended)
    {
      h = find_next_block ();
      if (!h)
	{
	  paxerror (0, _("Unexpected EOF in archive"));
	  return false;
	}
      set_next_block_after (h);
      for (idx_t i = 0; i < SPARSES_IN_STAR_EXT_HEADER && rc == add_ok; i++)
	rc = oldgnu_add_sparse (file, &h->star_ext_header.sp[i]);
      file->dumped_size += BLOCKSIZE;
    }

  if (rc == add_fail)
    {
      paxerror (0, _("%s: invalid sparse archive member"),
		file->stat_info->orig_file_name);
      return false;
    }
  return true;
}


static struct tar_sparse_optab const star_optab = {
  NULL,  /* No init function */
  NULL,  /* No done function */
  star_sparse_member_p,
  NULL,
  star_fixup_header,
  star_get_sparse_info,
  NULL,  /* No scan_block function */
  NULL, /* No dump region function */
  sparse_extract_region,
};


/* GNU PAX sparse file format. There are several versions:

   * 0.0

   The initial version of sparse format used by tar 1.14-1.15.1.
   The sparse file map is stored in x header:

   GNU.sparse.size      Real size of the stored file
   GNU.sparse.numblocks Number of blocks in the sparse map
   repeat numblocks time
     GNU.sparse.offset    Offset of the next data block
     GNU.sparse.numbytes  Size of the next data block
   end repeat

   This has been reported as conflicting with the POSIX specs. The reason is
   that offsets and sizes of non-zero data blocks were stored in multiple
   instances of GNU.sparse.offset/GNU.sparse.numbytes variables, whereas
   POSIX requires the latest occurrence of the variable to override all
   previous occurrences.

   To avoid this incompatibility two following versions were introduced.

   * 0.1

   Used by tar 1.15.2 -- 1.15.91 (alpha releases).

   The sparse file map is stored in
   x header:

   GNU.sparse.size      Real size of the stored file
   GNU.sparse.numblocks Number of blocks in the sparse map
   GNU.sparse.map       Map of non-null data chunks. A string consisting
                       of comma-separated values "offset,size[,offset,size]..."

   The resulting GNU.sparse.map string can be *very* long. While POSIX does not
   impose any limit on the length of a x header variable, this can confuse some
   tars.

   * 1.0

   Starting from this version, the exact sparse format version is specified
   explicitly in the header using the following variables:

   GNU.sparse.major     Major version
   GNU.sparse.minor     Minor version

   X header keeps the following variables:

   GNU.sparse.name      Real file name of the sparse file
   GNU.sparse.realsize  Real size of the stored file (corresponds to the old
                        GNU.sparse.size variable)

   The name field of the ustar header is constructed using the pattern
   "%d/GNUSparseFile.%p/%f".

   The sparse map itself is stored in the file data block, preceding the actual
   file data. It consists of a series of octal numbers of arbitrary length,
   delimited by newlines. The map is padded with nulls to the nearest block
   boundary.

   The first number gives the number of entries in the map. Following are map
   entries, each one consisting of two numbers giving the offset and size of
   the data block it describes.

   The format is designed in such a way that non-posix aware tars and tars not
   supporting GNU.sparse.* keywords will extract each sparse file in its
   condensed form with the file map attached and will place it into a separate
   directory. Then, using a simple program it would be possible to expand the
   file to its original form even without GNU tar.

   Bu default, v.1.0 archives are created. To use other formats,
   --sparse-version option is provided. Additionally, v.0.0 can be obtained
   by deleting GNU.sparse.map from 0.1 format: --sparse-version 0.1
   --pax-option delete=GNU.sparse.map
*/

static bool
pax_sparse_member_p (struct tar_sparse_file *file)
{
  return file->stat_info->sparse_map_avail > 0
          || file->stat_info->sparse_major > 0;
}

/* Start a header that uses the effective (shrunken) file size.  */
static union block *
pax_start_header (struct tar_stat_info *st)
{
  off_t realsize = st->stat.st_size;
  union block *blk;
  st->stat.st_size = st->archive_file_size;
  blk = start_header (st);
  st->stat.st_size = realsize;
  return blk;
}

static bool
pax_dump_header_0 (struct tar_sparse_file *file)
{
  off_t block_ordinal = current_block_ordinal ();
  union block *blk;
  char nbuf[UINTMAX_STRSIZE_BOUND];
  struct sp_array *map = file->stat_info->sparse_map;
  char *save_file_name = NULL;

  /* Store the real file size */
  xheader_store ("GNU.sparse.size", file->stat_info, NULL);
  xheader_store ("GNU.sparse.numblocks", file->stat_info, NULL);

  if (xheader_keyword_deleted_p ("GNU.sparse.map")
      || tar_sparse_minor == 0)
    {
      for (idx_t i = 0; i < file->stat_info->sparse_map_avail; i++)
	{
	  xheader_store ("GNU.sparse.offset", file->stat_info, &i);
	  xheader_store ("GNU.sparse.numbytes", file->stat_info, &i);
	}
    }
  else
    {
      xheader_store ("GNU.sparse.name", file->stat_info, NULL);
      save_file_name = file->stat_info->file_name;
      file->stat_info->file_name = xheader_format_name (file->stat_info,
					       "%d/GNUSparseFile.%p/%f", 0);

      xheader_string_begin (&file->stat_info->xhdr);
      for (idx_t i = 0; i < file->stat_info->sparse_map_avail; i++)
	{
	  if (i)
	    xheader_string_add (&file->stat_info->xhdr, ",");
	  xheader_string_add (&file->stat_info->xhdr,
			      umaxtostr (map[i].offset, nbuf));
	  xheader_string_add (&file->stat_info->xhdr, ",");
	  xheader_string_add (&file->stat_info->xhdr,
			      umaxtostr (map[i].numbytes, nbuf));
	}
      if (!xheader_string_end (&file->stat_info->xhdr,
			       "GNU.sparse.map"))
	{
	  free (file->stat_info->file_name);
	  file->stat_info->file_name = save_file_name;
	  return false;
	}
    }
  blk = pax_start_header (file->stat_info);
  finish_header (file->stat_info, blk, block_ordinal);
  if (save_file_name)
    {
      free (file->stat_info->file_name);
      file->stat_info->file_name = save_file_name;
    }
  return true;
}

/* An output block BLOCK, and a pointer PTR into it.  */
struct block_ptr
{
  union block *block;
  char *ptr;
};

/* Append to BP the contents of the string SRC, followed by a newline.
   If the string doesn't fit, put any overflow into the succeeding blocks.
   Return the updated BP.  */
static struct block_ptr
dump_str_nl (struct block_ptr bp, char const *str)
{
  char *endp = bp.block->buffer + BLOCKSIZE;
  char c;
  do
    {
      c = *str++;
      if (bp.ptr == endp)
	{
	  set_next_block_after (bp.block);
	  bp.block = find_next_block ();
	  bp.ptr = bp.block->buffer;
	  endp = bp.block->buffer + BLOCKSIZE;
	}
      *bp.ptr++ = c ? c : '\n';
    }
  while (c);

  return bp;
}

/* Return the floor of the log base 10 of N.  If N is 0, return 0.  */
static int
floorlog10 (uintmax_t n)
{
  for (int f = 0; ; f++)
    if ((n /= 10) == 0)
      return f;
}

static bool
pax_dump_header_1 (struct tar_sparse_file *file)
{
  off_t block_ordinal = current_block_ordinal ();
  char nbuf[UINTMAX_STRSIZE_BOUND];
  struct sp_array *map = file->stat_info->sparse_map;
  char *save_file_name = file->stat_info->file_name;

  /* Compute stored file size */
  off_t size = floorlog10 (file->stat_info->sparse_map_avail) + 2;
  for (idx_t i = 0; i < file->stat_info->sparse_map_avail; i++)
    {
      size += floorlog10 (map[i].offset) + 2;
      size += floorlog10 (map[i].numbytes) + 2;
    }
  size = (size + BLOCKSIZE - 1) & ~(BLOCKSIZE - 1);
  file->stat_info->archive_file_size += size;
  file->dumped_size += size;

  /* Store sparse file identification */
  xheader_store ("GNU.sparse.major", file->stat_info, NULL);
  xheader_store ("GNU.sparse.minor", file->stat_info, NULL);
  xheader_store ("GNU.sparse.name", file->stat_info, NULL);
  xheader_store ("GNU.sparse.realsize", file->stat_info, NULL);

  file->stat_info->file_name =
    xheader_format_name (file->stat_info, "%d/GNUSparseFile.%p/%f", 0);
  /* Make sure the created header name is shorter than NAME_FIELD_SIZE: */
  if (strlen (file->stat_info->file_name) > NAME_FIELD_SIZE)
    file->stat_info->file_name[NAME_FIELD_SIZE] = 0;

  struct block_ptr bp;
  bp.block = pax_start_header (file->stat_info);
  finish_header (file->stat_info, bp.block, block_ordinal);
  free (file->stat_info->file_name);
  file->stat_info->file_name = save_file_name;

  bp.block = find_next_block ();
  bp.ptr = bp.block->buffer;
  bp = dump_str_nl (bp, umaxtostr (file->stat_info->sparse_map_avail, nbuf));
  for (idx_t i = 0; i < file->stat_info->sparse_map_avail; i++)
    {
      bp = dump_str_nl (bp, umaxtostr (map[i].offset, nbuf));
      bp = dump_str_nl (bp, umaxtostr (map[i].numbytes, nbuf));
    }
  memset (bp.ptr, 0, BLOCKSIZE - (bp.ptr - bp.block->buffer));
  set_next_block_after (bp.block);
  return true;
}

static bool
pax_dump_header (struct tar_sparse_file *file)
{
  file->stat_info->sparse_major = tar_sparse_major;
  file->stat_info->sparse_minor = tar_sparse_minor;

  return (file->stat_info->sparse_major == 0) ?
           pax_dump_header_0 (file) : pax_dump_header_1 (file);
}

/* A success flag OK, a computed integer N, and block + ptr BP.  */
struct ok_n_block_ptr
{
  bool ok;
  uintmax_t n;
  struct block_ptr bp;
};

static struct ok_n_block_ptr
decode_num (struct block_ptr bp, uintmax_t nmax, struct tar_sparse_file *file)
{
  char *endp = bp.block->buffer + BLOCKSIZE;
  uintmax_t n = 0;
  bool digit_seen = false, nondigit_seen = false, overflow = false;
  while (true)
    {
      if (bp.ptr == endp)
	{
	  set_next_block_after (bp.block);
	  bp.block = find_next_block ();
	  if (!bp.block)
	    paxfatal (0, _("Unexpected EOF in archive"));
	  bp.ptr = bp.block->buffer;
	  endp = bp.block->buffer + BLOCKSIZE;
	}
      char c = *bp.ptr++;
      if (c == '\n')
	break;
      if (c_isdigit (c))
	{
	  digit_seen = true;
	  overflow |= ckd_mul (&n, n, 10);
	  overflow |= ckd_add (&n, n, c - '0');
	}
      else
	nondigit_seen = true;
    }

  overflow |= nmax < n;
  char const *msgid
    = (!digit_seen | nondigit_seen ? N_("%s: malformed sparse archive member")
       : overflow ? N_("%s: numeric overflow in sparse archive member")
       : NULL);
  if (msgid)
    paxerror (0, gettext (msgid), file->stat_info->orig_file_name);
  return (struct ok_n_block_ptr) { .ok = !msgid, .n = n, .bp = bp };
}

static bool
pax_decode_header (struct tar_sparse_file *file)
{
  if (file->stat_info->sparse_major > 0)
    {
      off_t start = current_block_ordinal ();
      set_next_block_after (current_header);
      struct block_ptr bp;
      bp.block = find_next_block ();
      if (!bp.block)
	paxfatal (0, _("Unexpected EOF in archive"));
      bp.ptr = bp.block->buffer;
      struct ok_n_block_ptr onbp = decode_num (bp, SIZE_MAX, file);
      if (!onbp.ok)
	return false;
      bp = onbp.bp;
      file->stat_info->sparse_map_size = onbp.n;
      file->stat_info->sparse_map
	= xicalloc (file->stat_info->sparse_map_size,
		    sizeof *file->stat_info->sparse_map);
      file->stat_info->sparse_map_avail = 0;
      for (idx_t i = 0; i < file->stat_info->sparse_map_size; i++)
	{
	  struct sp_array sp;
	  onbp = decode_num (bp, file->stat_info->stat.st_size, file);
	  if (!onbp.ok)
	    return false;
	  sp.offset = onbp.n;
	  off_t numbytes_max = file->stat_info->stat.st_size - sp.offset;
	  onbp = decode_num (onbp.bp, numbytes_max, file);
	  if (!onbp.ok)
	    return false;
	  sp.numbytes = onbp.n;
	  bp = onbp.bp;
	  sparse_add_map (file->stat_info, &sp);
	}
      set_next_block_after (bp.block);

      file->dumped_size += BLOCKSIZE * (current_block_ordinal () - start);
    }

  return true;
}

static struct tar_sparse_optab const pax_optab = {
  NULL,  /* No init function */
  NULL,  /* No done function */
  pax_sparse_member_p,
  pax_dump_header,
  NULL,
  pax_decode_header,
  NULL,  /* No scan_block function */
  sparse_dump_region,
  sparse_extract_region,
};
