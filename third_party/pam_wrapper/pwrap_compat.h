/*
 * Copyright (c) 2015 Andreas Schneider <asn@samba.org>
 * Copyright (c) 2015 Jakub Hrozek <jakub.hrozek@posteo.se>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_OPENPAM
#include <security/openpam.h>
#endif

/* OpenPAM doesn't define PAM_BAD_ITEM */
#ifndef PAM_BAD_ITEM
#define PAM_BAD_ITEM	PAM_SYSTEM_ERR
#endif /* PAM_BAD_ITEM */

#ifndef ENODATA
#define ENODATA EPIPE
#endif