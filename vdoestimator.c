/*
 * Copyright (c) 2019 Wiele Associates.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *
 * $Id$
 */

/**
 * Space saving estimator for VDO
 **/

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "errors.h"
#include "lz4.h"
#include "murmur/MurmurHash3.h"
#include "uds.h"
#include "uds-block.h"

struct query {
  LIST_ENTRY(query) query_list;
  struct udsRequest request;
  ssize_t data_size;
  unsigned char data[4096];
};

static pthread_mutex_t list_mutex;
static pthread_cond_t  list_cond;

LIST_HEAD(query_list, query);
static struct query_list queries= LIST_HEAD_INITIALIZER(query);

#define DEFAULT_HIGH 2000
#define DEFAULT_LOW  2000
static unsigned int high = DEFAULT_HIGH;
static unsigned int low =  DEFAULT_LOW;
static unsigned int concurrent_requests = 0;
static unsigned int peak_requests = 0;

static uint64_t files_scanned = 0;
static uint64_t files_skipped = 0;
static uint64_t total_bytes = 0;
static uint64_t compressed_bytes = 0;
static uint64_t bytes_used = 0;

static void chunk_callback(struct udsRequest *request)
{
  char buf[4096/2];
  if (request->status != UDS_SUCCESS) {
    errx(2, "Unsuccessful request %d", request->status);
  }
  struct query *query = container_of(request, struct query, request);
  if (pthread_mutex_lock(&list_mutex)) {
    err(2, "Unable to lock the mutex");
  }
  if (!request->found) {
    int compressed_size = LZ4_compress_default((char *)query->data, buf,
					       (int)query->data_size,
					       sizeof(buf));
    if (compressed_size && compressed_size < query->data_size) {
      compressed_bytes += query->data_size - compressed_size;
      bytes_used += compressed_size;
    } else {
      bytes_used += query->data_size;
    }
  }
  if (concurrent_requests <= high) {
    LIST_INSERT_HEAD(&queries, query, query_list);
    pthread_cond_signal(&list_cond);
  } else if (concurrent_requests > low) {
    free(query);
    --concurrent_requests;
  }
  if (pthread_mutex_unlock(&list_mutex)) {
    err(2, "Unable to unlock the mutex");
  }
  return;
}

static struct query *get_query(void)
{
  if (pthread_mutex_lock(&list_mutex)) {
    err(2, "Unable to lock the mutex");
  }
  struct query *query = LIST_FIRST(&queries);
  if (!query) {
    if (concurrent_requests < high) {
      query = malloc(sizeof(*query));
      if (!query) {
	err(2, "Unable to allocate request");
      }
      concurrent_requests++;
      if (peak_requests < concurrent_requests) {
	peak_requests = concurrent_requests;
      }
    } else {
      // Wait for one to be available
      if (pthread_cond_wait(&list_cond, &list_mutex)) {
	err(2, "Unable to wait for a request");
      }
    }
  } else {
    LIST_REMOVE(query, query_list);
  }
  if (pthread_mutex_unlock(&list_mutex)) {
    err(2, "Unable to unlock the mutex");
  }
  return query;
}

static void scan(char *file, UdsBlockContext context)
{
  //printf("scanning %s\n", file);
  int fd = open(file, O_RDONLY);
  if (fd < 0) {
    if (errno == EACCES || errno == EWOULDBLOCK || errno == EPERM
	|| errno == ENOENT || errno == ENFILE || errno == ENAMETOOLONG
	|| errno == EMFILE) {
      files_skipped++;
      return;
    }
    err(1, "Unable to open '%s'", file);
  }

  do {
    struct query *query = get_query();
    if (!query) {
      err(2, "Unable to allocate request");
    }

    ssize_t nread = read(fd, query->data, 4096);
    if (nread < 0) {
      err(1, "Unable to read '%s'", file);
    }
    if (nread == 0) {
      break;			/* EOF */
    }
    query->data_size = nread;
    total_bytes += nread;
    query->request = (struct udsRequest) {.callback  = chunk_callback,
					  .context   = context,
					  .type      = UDS_POST,
    };

    MurmurHash3_x64_128 (query->data, nread, 0x62ea60be,
			 &query->request.chunkName);
    int result = udsStartChunkOperation(&query->request);
    if (result != UDS_SUCCESS) {
      errx(1, "Unable to start request");
    }
  } while (true);
  files_scanned++;
  close(fd);
}

static void walk(char *root, UdsBlockContext context)
{
  int fds[2];
  int result = pipe(fds);
  if (result) {
    err(2, "Unable to create pipe to subprocess");
  }
  pid_t child = fork();
  if (child == -1) {
    err(2, "Unable to create subprocess");
  }
  else if (child == 0) {
    char *argv[7];
    argv[0] = "/usr/bin/find";
    argv[1] = root;
    argv[2] = (char *)"-xdev";
    argv[3] = "-type";
    argv[4] = "f";
    argv[5] = (char *)"-print";
    argv[6] = NULL;
    close(fds[0]);
    close(0);
    if (!dup2(fds[1], 1)) {
      err(2, "Unable to direct output to parent");
    }
    if (!execv(argv[0], argv)) {
      err(2, "Unable to run 'find'");
    }
    close(fds[0]);
    _exit(0);
  }

  /* Parent process */
  close(fds[1]);
  FILE* foundlist = fdopen(fds[0], "r");
  if (foundlist == NULL) {
    err(2, "Unable to get output of subprocess");
  }
  do {
    char *linep = NULL;
    size_t count = 0;
    ssize_t actual = getline(&linep, &count, foundlist);
    if (actual == -1) {
      /* Check for eof */
      if (!feof(foundlist)) {
	perror("Unable to read from subprocess");
      }
      fclose(foundlist);
      break;
    }
    if (linep[actual - 1] == '\n')
      linep[actual - 1] = '\000';
    scan(linep, context);
    free(linep);
    linep = NULL;
    count = 0;
  } while(true);
  int wstatus;
  if (waitpid(child, &wstatus, 0) != child) {
    err(2, "Unable to finish subprocess");
  }
}

int main(int argc, char *argv[])
{
  UdsConfiguration conf;
  int result = udsInitializeConfiguration(&conf, UDS_MEMORY_CONFIG_256MB);
  if (result != UDS_SUCCESS) {
    errx(1, "Unable to initialize configuration");
  }
  UdsIndexSession session;
  result = udsCreateLocalIndex("scan", conf, &session);
  if (result != UDS_SUCCESS) {
    errx(1, "Unable to create local index");
  }

  UdsBlockContext context;
  result = udsOpenBlockContext(session, 16, &context);
  if (result != UDS_SUCCESS) {
    errx(1, "Unable to create block context");
  }

  if (argc < 2) {
    errx(1, "At least one argument required");
  }

  pthread_mutex_init(&list_mutex, NULL);
  pthread_cond_init(&list_cond, NULL);
  
  walk(argv[1], context);

  result = udsFlushBlockContext(context);
  if (result != UDS_SUCCESS) {
    errx(1, "Unable to flush context");
  }

  struct udsContextStats cstats;
  result = udsGetBlockContextStats(context, &cstats);
  if (result != UDS_SUCCESS) {
    errx(1, "Unable to get context stats");
  }

  UdsIndexStats stats;
  result = udsGetIndexStats(session, &stats);
  if (result != UDS_SUCCESS) {
    errx(1, "Unable to get index stats");
  }

  printf("Files scanned: %lu\n", files_scanned);
  printf("Files skipped: %lu\n", files_skipped);
  printf("Bytes scanned: %lu\n", total_bytes);
  printf("Entries indexed: %ld\n", stats.entriesIndexed);
  printf("Posts found: %ld\n", cstats.postsFound);
  printf("Posts not found: %ld\n", cstats.postsNotFound);
  printf("Bytes used: %lu\n", bytes_used);
  double saved
     = ((double)total_bytes - (double)bytes_used) / (double)total_bytes;
  printf("Percent saved dedupe: %2.3f\n", saved * 100.0);
  printf("Compressed bytes: %lu\n", compressed_bytes);
  saved = (double)compressed_bytes / (double)total_bytes;
  printf("Percent saved compression: %2.3f\n", saved * 100.0);
  
  printf("Peak concurrent requests: %u\n", peak_requests);

  result = udsCloseBlockContext(context);
  if (result != UDS_SUCCESS) {
    errx(1, "Unable to close context");
  }

  result = udsCloseIndexSession(session);
  if (result != UDS_SUCCESS) {
    errx(1, "Unable to close index");
  }
  pthread_mutex_destroy(&list_mutex);
  return 0;
 }
