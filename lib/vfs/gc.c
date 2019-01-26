/*
   Virtual File System garbage collection code

   Copyright (C) 2003-2019
   Free Software Foundation, Inc.

   Written by:
   Miguel de Icaza, 1995
   Jakub Jelinek, 1995
   Pavel Machek, 1998
   Pavel Roskin, 2003

   This file is part of the Midnight Commander.

   The Midnight Commander is free software: you can redistribute it
   and/or modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation, either version 3 of the License,
   or (at your option) any later version.

   The Midnight Commander is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * \file
 * \brief Source: Virtual File System: garbage collection code
 * \author Miguel de Icaza
 * \author Jakub Jelinek
 * \author Pavel Machek
 * \author Pavel Roskin
 * \date 1995, 1998, 2003
 */


#include <config.h>

#include <stdlib.h>             /* For atol() */
#include <sys/types.h>
#include <sys/time.h>           /* gettimeofday() */

#include "lib/global.h"
#include "lib/event.h"

#include "vfs.h"
#include "utilvfs.h"

#include "gc.h"

/*
 * The garbage collection mechanism is based on "stamps".
 *
 * A stamp is a record that says "I'm a filesystem which is no longer in
 * use. Free me when you get a chance."
 *
 * This file contains a set of functions used for managing this stamp. You
 * should use them when you write your own filesystem. Here are some rules
 * of thumb:
 *
 * (1) When the last open file in your filesystem gets closed, conditionally
 *     create a stamp. You do this with vfs_stamp_create(). (The meaning
 *     of "conditionaly" is explained below.)
 *
 * (2) When a file in your filesystem is opened, delete the stamp. You do
 *     this with vfs_rmstamp().
 *
 * (3) When a path inside your filesystem is invoked, call vfs_stamp() to
 *     postpone the free'ing of your filesystem a bit. (This simply updates
 *     a timestamp variable inside the stamp.)
 *
 * Additionally, when a user navigates to a new directory in a panel (or a
 * programmer uses mc_chdir()), a stamp is conditionally created for the
 * previous directory's filesystem. This ensures that that filesystem is
 * free'ed. (see: _do_panel_cd() -> vfs_release_path(); mc_chdir()).
 *
 * We've spoken here of "conditionally creating" a stamp. What we mean is
 * that vfs_stamp_create() is to be used: this function creates a stamp
 * only if no directories are open (aka "active") in your filesystem. (If
 * there _are_ directories open, it means that the filesystem is in use, in
 * which case we don't want to free it.)
 */

/*** global variables ****************************************************************************/

int vfs_timeout = 60;           /* VFS timeout in seconds */

/*** file scope macro definitions ****************************************************************/

#define VFS_STAMPING(a) ((struct vfs_stamping *)(a))

/*** file scope type declarations ****************************************************************/

struct vfs_stamping
{
    struct vfs_class *v;
    vfsid id;
    struct timeval time;
};

/*** file scope variables ************************************************************************/

static GSList *stamps = NULL;

/* --------------------------------------------------------------------------------------------- */
/*** file scope functions ************************************************************************/
/* --------------------------------------------------------------------------------------------- */

/** Compare two timeval structures.  Return 0 is t1 is less than t2. */
static inline int
timeoutcmp (const struct timeval *t1, const struct timeval *t2)
{
    return ((t1->tv_sec < t2->tv_sec)
            || ((t1->tv_sec == t2->tv_sec) && (t1->tv_usec <= t2->tv_usec)));
}

/* --------------------------------------------------------------------------------------------- */

static gint
vfs_stamp_compare (gconstpointer a, gconstpointer b)
{
    const struct vfs_stamping *vsa = (const struct vfs_stamping *) a;
    const struct vfs_stamping *vsb = (const struct vfs_stamping *) b;

    return (vsa->v == vsb->v && vsa->id == vsb->id) ? 0 : 1;
}

/* --------------------------------------------------------------------------------------------- */

static void
vfs_stamp_free (gpointer data, gpointer user_data)
{
    struct vfs_stamping *stamp = VFS_STAMPING (data);

    (void) user_data;

    if (stamp->v->free != NULL)
        stamp->v->free (stamp->id);
}

/* --------------------------------------------------------------------------------------------- */

static void
vfs_stamp_expire (gpointer data, gpointer user_data)
{
    struct vfs_stamping *stamp = VFS_STAMPING (data);

    if (user_data == NULL || timeoutcmp (&(stamp->time), (struct timeval *) user_data) != 0)
    {
        vfs_stamp_free (data, NULL);
        vfs_rmstamp (stamp->v, stamp->id);
    }
}

/* --------------------------------------------------------------------------------------------- */

static void
vfs_addstamp (struct vfs_class *v, vfsid id)
{
    if ((v->flags & VFSF_LOCAL) == 0 && id != NULL && !vfs_stamp (v, id))
    {
        struct vfs_stamping *stamp;

        stamp = g_new (struct vfs_stamping, 1);
        stamp->v = v;
        stamp->id = id;
        gettimeofday (&(stamp->time), NULL);

        stamps = g_slist_append (stamps, stamp);
    }
}

/* --------------------------------------------------------------------------------------------- */
/*** public functions ****************************************************************************/
/* --------------------------------------------------------------------------------------------- */

gboolean
vfs_stamp (struct vfs_class *v, vfsid id)
{
    struct vfs_stamping what = {
        .v = v,
        .id = id
    };
    GSList *stamp;
    gboolean ret = FALSE;

    stamp = g_slist_find_custom (stamps, &what, vfs_stamp_compare);
    if (stamp != NULL)
    {
        gettimeofday (&(VFS_STAMPING (stamp->data)->time), NULL);
        ret = TRUE;
    }

    return ret;
}

/* --------------------------------------------------------------------------------------------- */

void
vfs_rmstamp (struct vfs_class *v, vfsid id)
{
    struct vfs_stamping what = {
        .v = v,
        .id = id
    };
    GSList *stamp;

    stamp = g_slist_find_custom (stamps, &what, vfs_stamp_compare);
    if (stamp != NULL)
    {
        g_free (stamp->data);
        stamps = g_slist_delete_link (stamps, stamp);
    }
}

/* --------------------------------------------------------------------------------------------- */

void
vfs_stamp_path (const char *path)
{
    vfsid id;
    vfs_path_t *vpath;
    const vfs_path_element_t *path_element;

    vpath = vfs_path_from_str (path);
    path_element = vfs_path_get_by_index (vpath, -1);

    id = vfs_getid (vpath);
    vfs_addstamp (path_element->class, id);
    vfs_path_free (vpath);
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Create a new timestamp item by VFS class and VFS id.
 */

void
vfs_stamp_create (struct vfs_class *vclass, vfsid id)
{
    vfsid nvfsid;

    ev_vfs_stamp_create_t event_data = { vclass, id, FALSE };
    const vfs_path_t *vpath;
    const vfs_path_element_t *path_element;

    /* There are three directories we have to take care of: current_dir,
       current_panel->cwd and other_panel->cwd. Athough most of the time either
       current_dir and current_panel->cwd or current_dir and other_panel->cwd are the
       same, it's possible that all three are different -- Norbert */

    if (!mc_event_present (MCEVENT_GROUP_CORE, "vfs_timestamp"))
        return;

    vpath = vfs_get_raw_current_dir ();
    path_element = vfs_path_get_by_index (vpath, -1);

    nvfsid = vfs_getid (vpath);
    vfs_rmstamp (path_element->class, nvfsid);

    if (!(id == NULL || (path_element->class == vclass && nvfsid == id)))
    {
        mc_event_raise (MCEVENT_GROUP_CORE, "vfs_timestamp", (gpointer) & event_data);

        if (!event_data.ret && vclass != NULL && vclass->nothingisopen != NULL
            && vclass->nothingisopen (id) != 0)
            vfs_addstamp (vclass, id);
    }
}

/* --------------------------------------------------------------------------------------------- */
/** This is called from timeout handler with now = 0, or can be called
   with now = 1 to force freeing all filesystems that are not in use */

void
vfs_expire (gboolean now)
{
    static gboolean locked = FALSE;

    /* Avoid recursive invocation, e.g. when one of the free functions
       calls message */
    if (locked)
        return;

    locked = TRUE;

    if (now)
        g_slist_foreach (stamps, vfs_stamp_expire, NULL);
    else
    {
        struct timeval lc_time;

        gettimeofday (&lc_time, NULL);
        lc_time.tv_sec -= vfs_timeout;

        g_slist_foreach (stamps, vfs_stamp_expire, &lc_time);
    }

    locked = FALSE;
}

/* --------------------------------------------------------------------------------------------- */
/*
 * Return the number of seconds remaining to the vfs timeout.
 * FIXME: The code should be improved to actually return the number of
 * seconds until the next item times out.
 */

int
vfs_timeouts (void)
{
    return stamps != NULL ? 10 : 0;
}

/* --------------------------------------------------------------------------------------------- */

void
vfs_timeout_handler (void)
{
    vfs_expire (FALSE);
}

/* --------------------------------------------------------------------------------------------- */

void
vfs_release_path (const vfs_path_t * vpath)
{
    const vfs_path_element_t *path_element;

    path_element = vfs_path_get_by_index (vpath, -1);
    vfs_stamp_create (path_element->class, vfs_getid (vpath));
}

/* --------------------------------------------------------------------------------------------- */
/* Free all data */

void
vfs_gc_done (void)
{
    g_slist_foreach (stamps, vfs_stamp_free, NULL);
    g_slist_free_full (stamps, g_free);
    stamps = NULL;
}

/* --------------------------------------------------------------------------------------------- */
