/**
 * collectd - src/rrdtool.c
 * Copyright (C) 2006-2008  Florian octo Forster
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; only version 2 of the License is applicable.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Authors:
 *   Florian octo Forster <octo at verplant.org>
 **/

#include "collectd.h"
#include "plugin.h"
#include "common.h"
#include "utils_avltree.h"
#include "utils_rrdcreate.h"

#include <rrd.h>

#if HAVE_PTHREAD_H
# include <pthread.h>
#endif

/*
 * Private types
 */
struct rrd_cache_s
{
	int    values_num;
	char **values;
	time_t first_value;
	time_t last_value;
	enum
	{
		FLAG_NONE   = 0x00,
		FLAG_QUEUED = 0x01
	} flags;
};
typedef struct rrd_cache_s rrd_cache_t;

enum rrd_queue_dir_e
{
  QUEUE_INSERT_FRONT,
  QUEUE_INSERT_BACK
};
typedef enum rrd_queue_dir_e rrd_queue_dir_t;

struct rrd_queue_s
{
	char *filename;
	struct rrd_queue_s *next;
};
typedef struct rrd_queue_s rrd_queue_t;

/*
 * Private variables
 */
static const char *config_keys[] =
{
	"CacheTimeout",
	"CacheFlush",
	"DataDir",
	"StepSize",
	"HeartBeat",
	"RRARows",
	"RRATimespan",
	"XFF"
};
static int config_keys_num = STATIC_ARRAY_SIZE (config_keys);

/* If datadir is zero, the daemon's basedir is used. If stepsize or heartbeat
 * is zero a default, depending on the `interval' member of the value list is
 * being used. */
static char   *datadir   = NULL;
static rrdcreate_config_t rrdcreate_config =
{
	/* stepsize = */ 0,
	/* heartbeat = */ 0,
	/* rrarows = */ 1200,
	/* xff = */ 0.1,

	/* timespans = */ NULL,
	/* timespans_num = */ 0,

	/* consolidation_functions = */ NULL,
	/* consolidation_functions_num = */ 0
};

/* XXX: If you need to lock both, cache_lock and queue_lock, at the same time,
 * ALWAYS lock `cache_lock' first! */
static int         cache_timeout = 0;
static int         cache_flush_timeout = 0;
static time_t      cache_flush_last;
static c_avl_tree_t *cache = NULL;
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;

static rrd_queue_t    *queue_head = NULL;
static rrd_queue_t    *queue_tail = NULL;
static pthread_t       queue_thread = 0;
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  queue_cond = PTHREAD_COND_INITIALIZER;

#if !HAVE_THREADSAFE_LIBRRD
static pthread_mutex_t librrd_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static int do_shutdown = 0;

#if HAVE_THREADSAFE_LIBRRD
static int srrd_update (char *filename, char *template,
		int argc, const char **argv)
{
	int status;

	optind = 0; /* bug in librrd? */
	rrd_clear_error ();

	status = rrd_update_r (filename, template, argc, (void *) argv);

	if (status != 0)
	{
		WARNING ("rrdtool plugin: rrd_update_r (%s) failed: %s",
				filename, rrd_get_error ());
	}

	return (status);
} /* int srrd_update */
/* #endif HAVE_THREADSAFE_LIBRRD */

#else /* !HAVE_THREADSAFE_LIBRRD */
static int srrd_update (char *filename, char *template,
		int argc, const char **argv)
{
	int status;

	int new_argc;
	char **new_argv;

	assert (template == NULL);

	new_argc = 2 + argc;
	new_argv = (char **) malloc ((new_argc + 1) * sizeof (char *));
	if (new_argv == NULL)
	{
		ERROR ("rrdtool plugin: malloc failed.");
		return (-1);
	}

	new_argv[0] = "update";
	new_argv[1] = filename;

	memcpy (new_argv + 2, argv, argc * sizeof (char *));
	new_argv[new_argc] = NULL;

	pthread_mutex_lock (&librrd_lock);
	optind = 0; /* bug in librrd? */
	rrd_clear_error ();

	status = rrd_update (new_argc, new_argv);
	pthread_mutex_unlock (&librrd_lock);

	if (status != 0)
	{
		WARNING ("rrdtool plugin: rrd_update_r failed: %s: %s",
				argv[1], rrd_get_error ());
	}

	sfree (new_argv);

	return (status);
} /* int srrd_update */
#endif /* !HAVE_THREADSAFE_LIBRRD */

static int value_list_to_string (char *buffer, int buffer_len,
		const data_set_t *ds, const value_list_t *vl)
{
	int offset;
	int status;
	int i;

	memset (buffer, '\0', buffer_len);

	status = ssnprintf (buffer, buffer_len, "%u", (unsigned int) vl->time);
	if ((status < 1) || (status >= buffer_len))
		return (-1);
	offset = status;

	for (i = 0; i < ds->ds_num; i++)
	{
		if ((ds->ds[i].type != DS_TYPE_COUNTER)
				&& (ds->ds[i].type != DS_TYPE_GAUGE))
			return (-1);

		if (ds->ds[i].type == DS_TYPE_COUNTER)
			status = ssnprintf (buffer + offset, buffer_len - offset,
					":%llu", vl->values[i].counter);
		else
			status = ssnprintf (buffer + offset, buffer_len - offset,
					":%lf", vl->values[i].gauge);

		if ((status < 1) || (status >= (buffer_len - offset)))
			return (-1);

		offset += status;
	} /* for ds->ds_num */

	return (0);
} /* int value_list_to_string */

static int value_list_to_filename (char *buffer, int buffer_len,
		const data_set_t *ds, const value_list_t *vl)
{
	int offset = 0;
	int status;

	if (datadir != NULL)
	{
		status = ssnprintf (buffer + offset, buffer_len - offset,
				"%s/", datadir);
		if ((status < 1) || (status >= buffer_len - offset))
			return (-1);
		offset += status;
	}

	status = ssnprintf (buffer + offset, buffer_len - offset,
			"%s/", vl->host);
	if ((status < 1) || (status >= buffer_len - offset))
		return (-1);
	offset += status;

	if (strlen (vl->plugin_instance) > 0)
		status = ssnprintf (buffer + offset, buffer_len - offset,
				"%s-%s/", vl->plugin, vl->plugin_instance);
	else
		status = ssnprintf (buffer + offset, buffer_len - offset,
				"%s/", vl->plugin);
	if ((status < 1) || (status >= buffer_len - offset))
		return (-1);
	offset += status;

	if (strlen (vl->type_instance) > 0)
		status = ssnprintf (buffer + offset, buffer_len - offset,
				"%s-%s.rrd", vl->type, vl->type_instance);
	else
		status = ssnprintf (buffer + offset, buffer_len - offset,
				"%s.rrd", vl->type);
	if ((status < 1) || (status >= buffer_len - offset))
		return (-1);
	offset += status;

	return (0);
} /* int value_list_to_filename */

static void *rrd_queue_thread (void *data)
{
	while (42)
	{
		rrd_queue_t *queue_entry;
		rrd_cache_t *cache_entry;
		char **values;
		int    values_num;
		int    i;

		/* XXX: If you need to lock both, cache_lock and queue_lock, at
		 * the same time, ALWAYS lock `cache_lock' first! */

		/* wait until an entry is available */
		pthread_mutex_lock (&queue_lock);
		while ((queue_head == NULL) && (do_shutdown == 0))
			pthread_cond_wait (&queue_cond, &queue_lock);

		/* We're in the shutdown phase */
		if (queue_head == NULL)
		{
			pthread_mutex_unlock (&queue_lock);
			break;
		}

		/* Dequeue the first entry */
		queue_entry = queue_head;
		if (queue_head == queue_tail)
			queue_head = queue_tail = NULL;
		else
			queue_head = queue_head->next;

		/* Unlock the queue again */
		pthread_mutex_unlock (&queue_lock);

		/* We now need the cache lock so the entry isn't updated while
		 * we make a copy of it's values */
		pthread_mutex_lock (&cache_lock);

		c_avl_get (cache, queue_entry->filename, (void *) &cache_entry);

		values = cache_entry->values;
		values_num = cache_entry->values_num;

		cache_entry->values = NULL;
		cache_entry->values_num = 0;
		cache_entry->flags = FLAG_NONE;

		pthread_mutex_unlock (&cache_lock);

		/* Write the values to the RRD-file */
		srrd_update (queue_entry->filename, NULL,
				values_num, (const char **)values);
		DEBUG ("rrdtool plugin: queue thread: Wrote %i values to %s",
				values_num, queue_entry->filename);

		for (i = 0; i < values_num; i++)
		{
			sfree (values[i]);
		}
		sfree (values);
		sfree (queue_entry->filename);
		sfree (queue_entry);
	} /* while (42) */

	pthread_mutex_lock (&cache_lock);
	c_avl_destroy (cache);
	cache = NULL;
	pthread_mutex_unlock (&cache_lock);

	pthread_exit ((void *) 0);
	return ((void *) 0);
} /* void *rrd_queue_thread */

static int rrd_queue_cache_entry (const char *filename, rrd_queue_dir_t dir)
{
  rrd_queue_t *queue_entry;

  queue_entry = (rrd_queue_t *) malloc (sizeof (rrd_queue_t));
  if (queue_entry == NULL)
    return (-1);

  queue_entry->filename = strdup (filename);
  if (queue_entry->filename == NULL)
  {
    free (queue_entry);
    return (-1);
  }

  queue_entry->next = NULL;

  pthread_mutex_lock (&queue_lock);
  if (dir == QUEUE_INSERT_FRONT)
  {
    queue_entry->next = queue_head;
    queue_head = queue_entry;
    if (queue_tail == NULL)
      queue_tail = queue_head;
  }
  else /* (dir == QUEUE_INSERT_BACK) */
  {
    if (queue_tail == NULL)
      queue_head = queue_entry;
    else
      queue_tail->next = queue_entry;
    queue_tail = queue_entry;
  }
  pthread_cond_signal (&queue_cond);
  pthread_mutex_unlock (&queue_lock);

  DEBUG ("rrdtool plugin: Put `%s' into the update queue", filename);

  return (0);
} /* int rrd_queue_cache_entry */

static int rrd_queue_move_to_front (const char *filename)
{
  rrd_queue_t *this;
  rrd_queue_t *prev;

  this = NULL;
  prev = NULL;
  pthread_mutex_lock (&queue_lock);
  for (this = queue_head; this != NULL; this = this->next)
  {
    if (strcmp (this->filename, filename) == 0)
      break;
    prev = this;
  }

  /* Check if we found the entry and if it is NOT the first entry. */
  if ((this != NULL) && (prev != NULL))
  {
    prev->next = this->next;
    this->next = queue_head;
    queue_head = this;
  }
  pthread_mutex_unlock (&queue_lock);

  return (0);
} /* int rrd_queue_move_to_front */

static void rrd_cache_flush (int timeout)
{
	rrd_cache_t *rc;
	time_t       now;

	char **keys = NULL;
	int    keys_num = 0;

	char *key;
	c_avl_iterator_t *iter;
	int i;

	DEBUG ("rrdtool plugin: Flushing cache, timeout = %i", timeout);

	now = time (NULL);

	/* Build a list of entries to be flushed */
	iter = c_avl_get_iterator (cache);
	while (c_avl_iterator_next (iter, (void *) &key, (void *) &rc) == 0)
	{
		if (rc->flags == FLAG_QUEUED)
			continue;
		else if ((now - rc->first_value) < timeout)
			continue;
		else if (rc->values_num > 0)
		{
			if (rrd_queue_cache_entry (key, QUEUE_INSERT_BACK) == 0)
				rc->flags = FLAG_QUEUED;
		}
		else /* ancient and no values -> waste of memory */
		{
			char **tmp = (char **) realloc ((void *) keys,
					(keys_num + 1) * sizeof (char *));
			if (tmp == NULL)
			{
				char errbuf[1024];
				ERROR ("rrdtool plugin: "
						"realloc failed: %s",
						sstrerror (errno, errbuf,
							sizeof (errbuf)));
				c_avl_iterator_destroy (iter);
				sfree (keys);
				return;
			}
			keys = tmp;
			keys[keys_num] = key;
			keys_num++;
		}
	} /* while (c_avl_iterator_next) */
	c_avl_iterator_destroy (iter);
	
	for (i = 0; i < keys_num; i++)
	{
		if (c_avl_remove (cache, keys[i], (void *) &key, (void *) &rc) != 0)
		{
			DEBUG ("rrdtool plugin: c_avl_remove (%s) failed.", keys[i]);
			continue;
		}

		assert (rc->values == NULL);
		assert (rc->values_num == 0);

		sfree (rc);
		sfree (key);
		keys[i] = NULL;
	} /* for (i = 0..keys_num) */

	sfree (keys);

	cache_flush_last = now;
} /* void rrd_cache_flush */

static int rrd_cache_flush_identifier (int timeout, const char *identifier)
{
  rrd_cache_t *rc;
  time_t now;
  int status;
  char key[2048];

  if (identifier == NULL)
  {
    rrd_cache_flush (timeout);
    return (0);
  }

  now = time (NULL);

  if (datadir == NULL)
	  snprintf (key, sizeof (key), "%s.rrd",
			  identifier);
  else
	  snprintf (key, sizeof (key), "%s/%s.rrd",
			  datadir, identifier);
  key[sizeof (key) - 1] = 0;

  status = c_avl_get (cache, key, (void *) &rc);
  if (status != 0)
  {
    WARNING ("rrdtool plugin: rrd_cache_flush_identifier: "
	"c_avl_get (%s) failed. Does that file really exist?",
	key);
    return (status);
  }

  if (rc->flags == FLAG_QUEUED)
    status = rrd_queue_move_to_front (key);
  else if ((now - rc->first_value) < timeout)
    status = 0;
  else if (rc->values_num > 0)
  {
    status = rrd_queue_cache_entry (key, QUEUE_INSERT_FRONT);
    if (status == 0)
      rc->flags = FLAG_QUEUED;
  }

  return (status);
} /* int rrd_cache_flush_identifier */

static int rrd_cache_insert (const char *filename,
		const char *value, time_t value_time)
{
	rrd_cache_t *rc = NULL;
	int new_rc = 0;
	char **values_new;

	pthread_mutex_lock (&cache_lock);

	c_avl_get (cache, filename, (void *) &rc);

	if (rc == NULL)
	{
		rc = (rrd_cache_t *) malloc (sizeof (rrd_cache_t));
		if (rc == NULL)
			return (-1);
		rc->values_num = 0;
		rc->values = NULL;
		rc->first_value = 0;
		rc->last_value = 0;
		rc->flags = FLAG_NONE;
		new_rc = 1;
	}

	if (rc->last_value >= value_time)
	{
		pthread_mutex_unlock (&cache_lock);
		WARNING ("rrdtool plugin: (rc->last_value = %u) >= (value_time = %u)",
				(unsigned int) rc->last_value,
				(unsigned int) value_time);
		return (-1);
	}

	values_new = (char **) realloc ((void *) rc->values,
			(rc->values_num + 1) * sizeof (char *));
	if (values_new == NULL)
	{
		char errbuf[1024];
		void *cache_key = NULL;

		sstrerror (errno, errbuf, sizeof (errbuf));

		c_avl_remove (cache, filename, &cache_key, NULL);
		pthread_mutex_unlock (&cache_lock);

		ERROR ("rrdtool plugin: realloc failed: %s", errbuf);

		sfree (cache_key);
		sfree (rc->values);
		sfree (rc);
		return (-1);
	}
	rc->values = values_new;

	rc->values[rc->values_num] = strdup (value);
	if (rc->values[rc->values_num] != NULL)
		rc->values_num++;

	if (rc->values_num == 1)
		rc->first_value = value_time;
	rc->last_value = value_time;

	/* Insert if this is the first value */
	if (new_rc == 1)
	{
		void *cache_key = strdup (filename);

		if (cache_key == NULL)
		{
			char errbuf[1024];
			sstrerror (errno, errbuf, sizeof (errbuf));

			pthread_mutex_unlock (&cache_lock);

			ERROR ("rrdtool plugin: strdup failed: %s", errbuf);

			sfree (rc->values[0]);
			sfree (rc->values);
			sfree (rc);
			return (-1);
		}

		c_avl_insert (cache, cache_key, rc);
	}

	DEBUG ("rrdtool plugin: rrd_cache_insert: file = %s; "
			"values_num = %i; age = %lu;",
			filename, rc->values_num,
			(unsigned long)(rc->last_value - rc->first_value));

	if ((rc->last_value - rc->first_value) >= cache_timeout)
	{
		/* XXX: If you need to lock both, cache_lock and queue_lock, at
		 * the same time, ALWAYS lock `cache_lock' first! */
		if (rc->flags != FLAG_QUEUED)
		{
			if (rrd_queue_cache_entry (filename, QUEUE_INSERT_BACK) == 0)
				rc->flags = FLAG_QUEUED;
		}
		else
		{
			DEBUG ("rrdtool plugin: `%s' is already queued.", filename);
		}
	}

	if ((cache_timeout > 0) &&
			((time (NULL) - cache_flush_last) > cache_flush_timeout))
		rrd_cache_flush (cache_flush_timeout);


	pthread_mutex_unlock (&cache_lock);

	return (0);
} /* int rrd_cache_insert */

static int rrd_compare_numeric (const void *a_ptr, const void *b_ptr)
{
	int a = *((int *) a_ptr);
	int b = *((int *) b_ptr);

	if (a < b)
		return (-1);
	else if (a > b)
		return (1);
	else
		return (0);
} /* int rrd_compare_numeric */

static int rrd_write (const data_set_t *ds, const value_list_t *vl)
{
	struct stat  statbuf;
	char         filename[512];
	char         values[512];
	int          status;

	if (0 != strcmp (ds->type, vl->type)) {
		ERROR ("rrdtool plugin: DS type does not match value list type");
		return -1;
	}

	if (value_list_to_filename (filename, sizeof (filename), ds, vl) != 0)
		return (-1);

	if (value_list_to_string (values, sizeof (values), ds, vl) != 0)
		return (-1);

	if (stat (filename, &statbuf) == -1)
	{
		if (errno == ENOENT)
		{
			status = cu_rrd_create_file (filename,
					ds, vl, &rrdcreate_config);
			if (status != 0)
				return (-1);
		}
		else
		{
			char errbuf[1024];
			ERROR ("stat(%s) failed: %s", filename,
					sstrerror (errno, errbuf,
						sizeof (errbuf)));
			return (-1);
		}
	}
	else if (!S_ISREG (statbuf.st_mode))
	{
		ERROR ("stat(%s): Not a regular file!",
				filename);
		return (-1);
	}

	status = rrd_cache_insert (filename, values, vl->time);

	return (status);
} /* int rrd_write */

static int rrd_flush (int timeout, const char *identifier)
{
	pthread_mutex_lock (&cache_lock);

	if (cache == NULL) {
		pthread_mutex_unlock (&cache_lock);
		return (0);
	}

	rrd_cache_flush_identifier (timeout, identifier);

	pthread_mutex_unlock (&cache_lock);
	return (0);
} /* int rrd_flush */

static int rrd_config (const char *key, const char *value)
{
	if (strcasecmp ("CacheTimeout", key) == 0)
	{
		int tmp = atoi (value);
		if (tmp < 0)
		{
			fprintf (stderr, "rrdtool: `CacheTimeout' must "
					"be greater than 0.\n");
			return (1);
		}
		cache_timeout = tmp;
	}
	else if (strcasecmp ("CacheFlush", key) == 0)
	{
		int tmp = atoi (value);
		if (tmp < 0)
		{
			fprintf (stderr, "rrdtool: `CacheFlush' must "
					"be greater than 0.\n");
			return (1);
		}
		cache_flush_timeout = tmp;
	}
	else if (strcasecmp ("DataDir", key) == 0)
	{
		if (datadir != NULL)
			free (datadir);
		datadir = strdup (value);
		if (datadir != NULL)
		{
			int len = strlen (datadir);
			while ((len > 0) && (datadir[len - 1] == '/'))
			{
				len--;
				datadir[len] = '\0';
			}
			if (len <= 0)
			{
				free (datadir);
				datadir = NULL;
			}
		}
	}
	else if (strcasecmp ("StepSize", key) == 0)
	{
		int temp = atoi (value);
		if (temp > 0)
			rrdcreate_config.stepsize = temp;
	}
	else if (strcasecmp ("HeartBeat", key) == 0)
	{
		int temp = atoi (value);
		if (temp > 0)
			rrdcreate_config.heartbeat = temp;
	}
	else if (strcasecmp ("RRARows", key) == 0)
	{
		int tmp = atoi (value);
		if (tmp <= 0)
		{
			fprintf (stderr, "rrdtool: `RRARows' must "
					"be greater than 0.\n");
			return (1);
		}
		rrdcreate_config.rrarows = tmp;
	}
	else if (strcasecmp ("RRATimespan", key) == 0)
	{
		char *saveptr = NULL;
		char *dummy;
		char *ptr;
		char *value_copy;
		int *tmp_alloc;

		value_copy = strdup (value);
		if (value_copy == NULL)
			return (1);

		dummy = value_copy;
		while ((ptr = strtok_r (dummy, ", \t", &saveptr)) != NULL)
		{
			dummy = NULL;
			
			tmp_alloc = realloc (rrdcreate_config.timespans,
					sizeof (int) * (rrdcreate_config.timespans_num + 1));
			if (tmp_alloc == NULL)
			{
				fprintf (stderr, "rrdtool: realloc failed.\n");
				free (value_copy);
				return (1);
			}
			rrdcreate_config.timespans = tmp_alloc;
			rrdcreate_config.timespans[rrdcreate_config.timespans_num] = atoi (ptr);
			if (rrdcreate_config.timespans[rrdcreate_config.timespans_num] != 0)
				rrdcreate_config.timespans_num++;
		} /* while (strtok_r) */

		qsort (/* base = */ rrdcreate_config.timespans,
				/* nmemb  = */ rrdcreate_config.timespans_num,
				/* size   = */ sizeof (rrdcreate_config.timespans[0]),
				/* compar = */ rrd_compare_numeric);

		free (value_copy);
	}
	else if (strcasecmp ("XFF", key) == 0)
	{
		double tmp = atof (value);
		if ((tmp < 0.0) || (tmp >= 1.0))
		{
			fprintf (stderr, "rrdtool: `XFF' must "
					"be in the range 0 to 1 (exclusive).");
			return (1);
		}
		rrdcreate_config.xff = tmp;
	}
	else
	{
		return (-1);
	}
	return (0);
} /* int rrd_config */

static int rrd_shutdown (void)
{
	pthread_mutex_lock (&cache_lock);
	rrd_cache_flush (-1);
	pthread_mutex_unlock (&cache_lock);

	pthread_mutex_lock (&queue_lock);
	do_shutdown = 1;
	pthread_cond_signal (&queue_cond);
	pthread_mutex_unlock (&queue_lock);

	/* Wait for all the values to be written to disk before returning. */
	if (queue_thread != 0)
	{
		pthread_join (queue_thread, NULL);
		queue_thread = 0;
		DEBUG ("rrdtool plugin: queue_thread exited.");
	}

	return (0);
} /* int rrd_shutdown */

static int rrd_init (void)
{
	int status;

	if (rrdcreate_config.stepsize < 0)
		rrdcreate_config.stepsize = 0;
	if (rrdcreate_config.heartbeat <= 0)
		rrdcreate_config.heartbeat = 2 * rrdcreate_config.stepsize;

	if ((rrdcreate_config.heartbeat > 0)
			&& (rrdcreate_config.heartbeat < interval_g))
		WARNING ("rrdtool plugin: Your `heartbeat' is "
				"smaller than your `interval'. This will "
				"likely cause problems.");
	else if ((rrdcreate_config.stepsize > 0)
			&& (rrdcreate_config.stepsize < interval_g))
		WARNING ("rrdtool plugin: Your `stepsize' is "
				"smaller than your `interval'. This will "
				"create needlessly big RRD-files.");

	/* Set the cache up */
	pthread_mutex_lock (&cache_lock);

	cache = c_avl_create ((int (*) (const void *, const void *)) strcmp);
	if (cache == NULL)
	{
		ERROR ("rrdtool plugin: c_avl_create failed.");
		return (-1);
	}

	cache_flush_last = time (NULL);
	if (cache_timeout < 2)
	{
		cache_timeout = 0;
		cache_flush_timeout = 0;
	}
	else if (cache_flush_timeout < cache_timeout)
		cache_flush_timeout = 10 * cache_timeout;

	pthread_mutex_unlock (&cache_lock);

	status = pthread_create (&queue_thread, NULL, rrd_queue_thread, NULL);
	if (status != 0)
	{
		ERROR ("rrdtool plugin: Cannot create queue-thread.");
		return (-1);
	}

	DEBUG ("rrdtool plugin: rrd_init: datadir = %s; stepsize = %i;"
			" heartbeat = %i; rrarows = %i; xff = %lf;",
			(datadir == NULL) ? "(null)" : datadir,
			rrdcreate_config.stepsize,
			rrdcreate_config.heartbeat,
			rrdcreate_config.rrarows,
			rrdcreate_config.xff);

	return (0);
} /* int rrd_init */

void module_register (void)
{
	plugin_register_config ("rrdtool", rrd_config,
			config_keys, config_keys_num);
	plugin_register_init ("rrdtool", rrd_init);
	plugin_register_write ("rrdtool", rrd_write);
	plugin_register_flush ("rrdtool", rrd_flush);
	plugin_register_shutdown ("rrdtool", rrd_shutdown);
}
