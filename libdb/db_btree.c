/*
 * db_btree.c: low level btree interface routines for man.
 *
 * Copyright (C) 1994, 1995 Graeme W. Wilford. (Wilf.)
 * Copyright (C) 2002 Colin Watson.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Mon Aug  8 20:35:30 BST 1994  Wilf. (G.Wilford@ee.surrey.ac.uk)
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif /* HAVE_CONFIG_H */

/* below this line are routines only useful for the BTREE interface */
#ifdef BTREE

#include <stdio.h>
#include <errno.h>

#if HAVE_SYS_FILE_H
#  include <sys/file.h> /* for flock() */
#endif

#include <sys/types.h> /* for open() */
#include <sys/stat.h>

#if HAVE_FCNTL_H
#  include <fcntl.h>
#endif

#ifdef HAVE_UNISTD_H
#  include <unistd.h>
#endif /* HAVE_UNISTD_H */

#include "manconfig.h"
#include "lib/error.h"
#include "lib/flock.h"
#include "lib/hashtable.h"
#include "mydbm.h"
#include "db_storage.h"

struct hashtable *loop_check_hash;

/* the Berkeley database libraries do nothing to arbitrate between concurrent
   database accesses, so we do a simple flock(). If the db is opened in
   anything but O_RDONLY mode, an exclusive lock is enabled. Otherwise, the
   lock is shared. A file cannot have both locks at once, and the non
   blocking method is used ": Try again". This adopts GNU dbm's approach. */

#ifdef FAST_BTREE
#define B_FLAGS		R_DUP 	/* R_DUP: allow duplicates in tree */

void test_insert (int line, const datum key, const datum cont)
{
	fprintf (stderr, "(%d) key: \"%s\", cont: \"%.40s\"\n", line, key.dptr,
		 cont.dptr);
}
#else /* !FAST_BTREE */
#define B_FLAGS		0	/* do not allow any duplicate keys */
#endif /* FAST_BTREE */

/* release the lock and close the database */
int btree_close (DB *dbf)
{
	(void) flock ((dbf->fd) (dbf), LOCK_UN);
	return (dbf->close) (dbf);
}

/* open a btree type database, with file locking. */
DB *btree_flopen (char *filename, int flags, int mode)
{
	DB *dbf;
	BTREEINFO b;
	int lock_op;
	int lock_failed;

	b.flags = B_FLAGS;

	b.cachesize = 0;	/* default size */
	b.maxkeypage = 0;	/* default */
	b.minkeypage = 0;	/* default */
	b.psize = 0;		/* default page size (2048?) */
	b.compare = NULL;	/* builtin compare() function */
	b.prefix = NULL;	/* builtin function */
	b.lorder = 0;		/* byte order of host */

	if (flags & ~O_RDONLY) {
		/* flags includes O_RDWR or O_WRONLY, need an exclusive lock */
		lock_op = LOCK_EX | LOCK_NB;
	} else {
		lock_op = LOCK_SH | LOCK_NB;
	}

	if (!(flags & O_CREAT)) {
		/* Berkeley DB thinks that a zero-length file means that
		 * somebody else is writing it, and sleeps for a few
		 * seconds to give the writer a chance. All very well, but
		 * the common case is that the database is just zero-length
		 * because mandb was interrupted or ran out of disk space or
		 * something like that - so we check for this case by hand
		 * and ignore the database if it's zero-length.
		 */
		struct stat iszero;
		if (stat (filename, &iszero) < 0)
			return NULL;
		if (iszero.st_size == 0) {
			errno = EINVAL;
			return NULL;
		}
	}

	if (flags & O_TRUNC) {
		/* opening the db is destructive, need to lock first */
		int fd;

		dbf = NULL;
		lock_failed = 1;
		fd = open (filename, flags & ~O_TRUNC, mode);
		if (fd != -1) {
			if (!(lock_failed = flock (fd, lock_op)))
				dbf = dbopen (filename, flags, mode,
					      DB_BTREE, &b);
			close (fd);
		}
	} else {
		dbf = dbopen (filename, flags, mode, DB_BTREE, &b);
		if (dbf)
			lock_failed = flock ((dbf->fd) (dbf), lock_op);
	}

	if (!dbf)
		return NULL;

	if (lock_failed) {
		gripe_lock (filename);
		btree_close (dbf);
		return NULL;
	}

	return dbf;
}

/* do a replace when we have the duplicate flag set on the database -
   we must do a del and insert, as a direct insert will not wipe out the
   old entry */
int btree_replace (DB *dbf, datum key, datum cont)
{
#ifdef FAST_BTREE
	test_insert (__LINE__, key, cont);
	return (dbf->put) (dbf, (DBT *) &key, (DBT *) &cont, R_CURSOR);
#else /* normal BTREE */
	return (dbf->put) (dbf, (DBT *) &key, (DBT *) &cont, 0);
#endif /* FAST_BTREE */
}

int btree_insert (DB *dbf, datum key, datum cont)
{
	return (dbf->put) (dbf, (DBT *) &key, (DBT *) &cont, R_NOOVERWRITE);
}

/* generic fetch routine for the btree database */
datum btree_fetch (DB *dbf, datum key)
{
	datum data;

	if ((dbf->get) (dbf, (DBT *) &key, (DBT *) &data, 0)) {
		data.dptr = NULL;
		data.dsize = 0;
		return data;
	}

	return copy_datum (data);
}

/* return 1 if the key exists, 0 otherwise */
int btree_exists (DB *dbf, datum key)
{
	datum data;
	return ((dbf->get) (dbf, (DBT *) &key, (DBT *) &data, 0) ? 0 : 1);
}

/* initiate a sequential access */
static __inline__ datum btree_findkey (DB *dbf, u_int flags)
{
	datum key, data;

	if (flags == R_FIRST) {
		if (loop_check_hash) {
			hash_free (loop_check_hash);
			loop_check_hash = NULL;
		}
	}
	if (!loop_check_hash)
		loop_check_hash = hash_create (&plain_hash_free);

	if (((dbf->seq) (dbf, (DBT *) &key, (DBT *) &data, flags))) {
		key.dptr = NULL;
		key.dsize = 0;
		return key;
	}

	if (hash_lookup (loop_check_hash, key.dptr, key.dsize)) {
		/* We've seen this key already, which is broken. Return NULL
		 * so the caller doesn't go round in circles.
		 */
		if (debug)
			fprintf (stderr, "Corrupt database! Already seen %*s. "
					 "Attempting to recover ...\n",
				 (int) key.dsize, key.dptr);
		key.dptr = NULL;
		key.dsize = 0;
		return key;
	}

	hash_install (loop_check_hash, key.dptr, key.dsize, NULL);

	return copy_datum (key);
}

/* return the first key in the db */
datum btree_firstkey (DB *dbf)
{
	return btree_findkey (dbf, R_FIRST);
}

/* return the next key in the db. NB. This routine only works if the cursor
   has been previously set by btree_firstkey() since it was last opened. So
   if we close/reopen a db mid search, we have to manually set up the
   cursor again. */
datum btree_nextkey (DB *dbf)
{
	return btree_findkey (dbf, R_NEXT);
}

/* compound nextkey routine, initialising key and content */
int btree_nextkeydata (DB *dbf, datum *key, datum *cont)
{
	int status;

	if ((status = (dbf->seq) (dbf, (DBT *) key, (DBT *) cont,
				  R_NEXT)) != 0)
		return status;

	*key = copy_datum (*key);
	*cont = copy_datum (*cont);

	return 0;
}

#ifdef FAST_BTREE

/* EXTREMELY experimental and broken code, leave well alone for the time
   being */

#define free(x)

void gripe_get (int lineno)
{
	error (0, 0, "Oops, bad fetch, line %d ", lineno);
}

/* the simplified storage routine */
int dbstore (struct mandata *in, char *basename)
{
	datum key, cont;
	int status;

 	key.dsize = strlen (basename) + 1;

 	if (key.dsize == 1) {
 		if (debug)
 			dbprintf (in);
 		return 2;
 	}

	key.dptr = basename;

	/* initialise the cursor to (possibly) our key/cont */
	status = (dbf->seq) (dbf, (DBT *) &key, (DBT *) &cont, R_CURSOR);

	if (status == -1)
		gripe_get (__LINE__);

	/* either nothing was found or the key was not an exact match */
	else if (status == 1 || !STREQ (key.dptr, basename)) {
		cont = make_content (in);
		key.dptr = basename;
		key.dsize = strlen (basename) + 1;
		test_insert (__LINE__, key, cont);
		status = (dbf->put) (dbf, (DBT *) &key, (DBT *) &cont, 0);
		free (cont.dptr);

	/* There is already a key with this name */
	} else {
		/* find an exact match and see if it needs replacing or put
		   our new key in */
		while (1) {
			struct mandata old;

			cont = copy_datum (cont);
			split_content (cont.dptr, &old);
			if (STREQ (in->ext, old.ext)) {
				cont = make_content (in);
				status = replace_if_necessary (in, &old,
							       key, cont);
				free (cont.dptr);
				free_mandata_elements (&old);
				break;
			}
			free_mandata_elements (&old);
			status = (dbf->seq) (dbf, (DBT *) &key, (DBT *) &cont,
					     R_NEXT);
			if (!STREQ (key.dptr, basename)) {
				key.dptr = basename;
				key.dsize = strlen (basename) + 1;
				cont = make_content (in);
				test_insert (__LINE__, key, cont);
				status = (dbf->put) (dbf, (DBT *) &key,
						     (DBT *) &cont, 0);
				free (cont.dptr);
				break;
			}
		}
	}

	return status;
}

/* FIXME: I'm broken as I don't return properly */
static struct mandata *dblookup (char *page, char *section, int flags)
{
	struct mandata *info, *ret = NULL, **null_me;
	datum key, cont;
	int status;

	key.dptr = page;
	key.dsize = strlen (page) + 1;

	/* initialise the cursor to (possibly) our key/cont */
	status = (dbf->seq) (dbf, (DBT *) &key, (DBT *) &cont, R_CURSOR);

	/* either nothing was found or the key was not an exact match */
	if (status == 1 || !STREQ (page, key.dptr))
		return NULL;
	if (status == -1)
		gripe_get (__LINE__);

	ret = info = infoalloc ();
	null_me = &(info->next);

	do {
		cont = copy_datum (cont);
		split_content (cont.dptr, info);

		if (!(section == NULL ||
		    STRNEQ (section, info->ext,
		    	    flags & EXACT ? strlen (info->ext)
					  : strlen (section)))) {
			free_mandata_elements (info);
		} else {
			null_me = &(info->next);
			info = info->next = infoalloc ();
		}

		/* get next in the db */
		status = (dbf->seq) (dbf, (DBT *) &key, (DBT *) &cont, R_NEXT);

		if (status == -1)
			gripe_get (__LINE__);

		/* run out of identical keys */
	} while (!(status == 1 || !STREQ (page, key.dptr)));

	free (info);
	*null_me = NULL;
	return ret;
}

#endif /* FAST_BTREE */
#endif /* BTREE */