/*
 * xsigaction.c: sigaction() with EINTR checking
 * Copyright (C) 2007 Colin Watson.
 *
 * This file is part of man-db.
 *
 * man-db is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * man-db is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with man-db; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef MAN_XSIGACTION_H
#define MAN_XSIGACTION_H

#include <signal.h>
#include <errno.h>

int xsigaction (int signum, const struct sigaction *act,
		struct sigaction *oldact)
{
	for (;;) {
		int ret = sigaction (signum, act, oldact);
		if (ret >= 0 || errno != EINTR)
			return ret;
	}
}

#endif /* MAN_XSIGACTION_H */
