/*
 * Copyright (c) 2019 Wiele Associates.
 * Copyright (c) 2020 Red Hat, Inc. All rights reserved.
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
#include <getopt.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "errors.h"
#include "lz4.h"
#include "murmur/MurmurHash3.h"
#include "uds.h"

#define BLOCK_SIZE 4096

struct query {
  LIST_ENTRY(query) query_list;
  struct uds_request request;
  ssize_t data_size;
  unsigned char data[BLOCK_SIZE];
};

static pthread_mutex_t list_mutex;
static pthread_cond_t  list_cond;

LIST_HEAD(query_list, query);
static struct query_list queries = LIST_HEAD_INITIALIZER(query);

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

static char *uds_index = NULL;
static bool use_sparse = false;
static bool compression_only = false;
static bool dedupe_only = false;
static bool reuse = false;
static bool mem_modified = false;
static bool verbose = false;
uds_memory_config_size_t mem_size;

/**
 * Gets a query from the lookaside list, or allocates one if possible.
 **/
static struct query *get_query(void)
{
  if (pthread_mutex_lock(&list_mutex)) {
    err(2, "Unable to lock the mutex");
  }
  struct query *query = NULL;
  do {
    query = LIST_FIRST(&queries);
    if (query) {
      LIST_REMOVE(query, query_list);
      break;
    }
    // If one was not immediately available, try to allocate one.
    if (concurrent_requests < high) {
      query = malloc(sizeof(*query));
      if (query) {
        concurrent_requests++;
        if (peak_requests < concurrent_requests) {
          peak_requests = concurrent_requests;
        }
        break;
      }
    }
    // If all else fails, wait for one to be available
    if (pthread_cond_wait(&list_cond, &list_mutex)) {
      err(2, "Unable to wait for a request");
    }
  } while (1);
  if (pthread_mutex_unlock(&list_mutex)) {
    err(2, "Unable to unlock the mutex");
  }
  return query;
}

/**
 * Puts a query on the lookaside list, or frees it if above the low
 * water mark.
 **/
static void put_query(struct query *query)
{
  if (pthread_mutex_lock(&list_mutex)) {
    err(2, "Unable to lock the mutex");
  }
  if (concurrent_requests > low) {
    free(query);
    --concurrent_requests;
  } else {
    LIST_INSERT_HEAD(&queries, query, query_list);
    pthread_cond_signal(&list_cond);
  }
  if (pthread_mutex_unlock(&list_mutex)) {
    err(2, "Unable to unlock the mutex");
  }
}

static void try_compression(struct query *query)
{
  char buf[BLOCK_SIZE/2];
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

static void chunk_callback(struct uds_request *request)
{
  if (request->status != UDS_SUCCESS) {
    errx(2, "Unsuccessful request %d", request->status);
  }
  struct query *query = container_of(request, struct query, request);
  // If not found, i. e., a never seen before block, compute its
  // compressability.
  if (!request->found && !dedupe_only)
    try_compression(query);
  else {
    if(compression_only)
      try_compression(query);
  }
  put_query(query);
  return;
}

static void scan(char *file, struct uds_index_session *session)
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

    ssize_t nread = read(fd, query->data, BLOCK_SIZE);
    if (nread < 0) {
      err(1, "Unable to read '%s'", file);
    }
    if (nread == 0) {
      put_query(query);
      break;                    /* EOF */
    }
    query->data_size = nread;
    total_bytes += nread;
    query->request = (struct uds_request) {.callback  = chunk_callback,
					   .session   = session,
					   .type      = UDS_POST,
    };
    MurmurHash3_x64_128 (query->data, nread, 0x62ea60be,
                         &query->request.chunk_name);
    int result = uds_start_chunk_operation(&query->request);
    if (result != UDS_SUCCESS) {
      errx(1, "Unable to start request");
    }
  } while (true);
  files_scanned++;
  close(fd);
}

static void walk(char *root, struct uds_index_session *session)
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
    if (verbose)
      printf("Scanning file %s\n", linep);
    scan(linep, session);
    free(linep);
    linep = NULL;
    count = 0;
  } while(true);
  int wstatus;
  if (waitpid(child, &wstatus, 0) != child) {
    err(2, "Unable to finish subprocess");
  }
}

/**
 * Prints a usage string.
 **/
static void usage(char *prog)
{
  printf("Usage: %s [OPTION]... PATH\n"
         "Estimate the storage savings that would be obtained by putting\n"
         "the contents of a directory tree or device at PATH on a VDO\n"
         "device.\n"
         "\n"
         "Options:\n"
         "  --compressionOnly Calculate compression savings only\n"
         "  --dedupeOnly      Calculate deduplication savings only\n"
         "  --help            Print this help message and exit\n"
         "  --index           Specify the location and name of the UDS index file\n"
         "  --memorySize      Specifies the amount of UDS server memory in gigabytes;\n"
         "                    the default size is 0.25 GB.\n"
         "                    The special decimal values 0.25, 0.5, 0.75 can be used\n"
         "                    as can any positive integer up to 1024.\n"
         "  --reuse           Reuse index file\n"
         "  --sparse          Use a sparse index\n"
         "  --verbose         Verbose run\n",
         prog);
}

static void parse_args(int argc, char *argv[])
{
  static const char *optstring = "cdhi:m:rsv";
  static const struct option longopts[]
    = {
       {"compressionOnly",  no_argument,       0,    'c'},
       {"dedupeOnly",       no_argument,       0,    'd'},
       {"help",             no_argument,       0,    'h'},
       {"index",            required_argument, 0,    'i'},
       {"memorySize",       required_argument, 0,    'm'},
       {"reuse",            no_argument,       0,    'r'},
       {"sparse",           no_argument,       0,    's'},
       {"verbose",          no_argument,       0,    'v'},
       {0,                  0,                 0,     0 }
  };
  int opt;
  mem_size = UDS_MEMORY_CONFIG_256MB;

  while ((opt = getopt_long(argc, argv, optstring, longopts, NULL)) != -1) {
    switch(opt) {
    case 'c':
      compression_only=true;
      break;
    case 'd':
      dedupe_only=true;
      break;
    case 'h':
      usage(argv[0]);
      _exit(0);
      break;
    case 'i':
      uds_index = optarg;
      break;
    case 'm':
      mem_modified = true;
      if (strcmp("0.25", optarg) == 0)
        mem_size = UDS_MEMORY_CONFIG_256MB;
      else if (strcmp("0.5", optarg) == 0)
        mem_size = UDS_MEMORY_CONFIG_512MB;
      else if (strcmp("0.75", optarg) == 0)
        mem_size = UDS_MEMORY_CONFIG_768MB;
      else {
        char *endptr = NULL;
        unsigned long n = strtoul(optarg, &endptr, 10);
        if (*endptr != '\0' || n == 0 || n > UDS_MEMORY_CONFIG_MAX) {
          errx(1, "Illegal memory size, valid value: 1..1024, 0.25, 0.5, 0.75");
          _exit(2);
        }
        mem_size = (uds_memory_config_size_t)n;
      }
      break;
    case 'r':
      reuse = true;
      break;
    case 's':
      use_sparse = true;
      break;
    case 'v':
      verbose = true;
      break;
    default:
      usage(argv[0]);
      _exit(2);
      break;
    }
  }
  if (optind != argc - 1) {
    fprintf(stderr, "Exactly one PATH argument is required.\n");
    usage(argv[0]);
    _exit(2);
  }
  if (uds_index == NULL) {
    printf("Index file is required\n");
    usage(argv[0]);
    _exit(2);
  }
  if (compression_only && dedupe_only) {
    printf("--compressOnly and --dedupeOnly may not be used together\n");
    _exit(2);
  }
  if (reuse && (mem_modified || use_sparse)) {
    printf("--reuse may not be combined with --memorySize or --sparse\n");
    _exit(2);
  }
}

int main(int argc, char *argv[])
{
  parse_args(argc, argv);
  time_t start_time = time(0);

  struct uds_configuration *conf;

  int result = uds_initialize_configuration(&conf, mem_size);
  if (result != UDS_SUCCESS) {
    errx(1, "Unable to initialize configuration");
  }

  uds_configuration_set_sparse(conf, use_sparse);

  struct uds_index_session *session;
  result = uds_create_index_session(&session);
  if (result != UDS_SUCCESS) {
    errx(1, "Unable to create an index session");
  }

  const struct uds_parameters params = UDS_PARAMETERS_INITIALIZER;

  result = uds_open_index(reuse ? UDS_LOAD : UDS_CREATE,
			  uds_index, &params, conf, session);
  if (result != UDS_SUCCESS) {
    errx(1, "Unable to open the index");
  }
  
  pthread_mutex_init(&list_mutex, NULL);
  pthread_cond_init(&list_cond, NULL);
  
  while (optind < argc) {
    char *path = argv[optind++];
    struct stat statbuf;
    if (stat(path, &statbuf)) {
      err(1, "Unable to stat %s\n", path);
    }
    if (S_ISBLK(statbuf.st_mode)) {
      scan(path, session);
    } else if (S_ISDIR(statbuf.st_mode)) {
      walk(path, session);
    } else {
      errx(2, "Argument must be a directory or block device");
    }
  }

  result = uds_flush_index_session(session);
  if (result != UDS_SUCCESS) {
    errx(1, "Unable to flush the index session");
  }

  struct uds_context_stats cstats;
  result = uds_get_index_session_stats(session, &cstats);
  if (result != UDS_SUCCESS) {
    errx(1, "Unable to get context stats");
  }

  struct uds_index_stats stats;
  result = uds_get_index_stats(session, &stats);
  if (result != UDS_SUCCESS) {
    errx(1, "Unable to get index stats");
  }

  time_t time_passed = cstats.current_time - start_time;
  printf("Duration: %ldh:%ldm:%lds\n",
         time_passed/3600, (time_passed%3600)/60, time_passed%60);
  printf("Sparse Index: %d\n", uds_configuration_get_sparse(conf));
  printf("Files Scanned: %llu\n", files_scanned);
  printf("Files Skipped: %llu\n", files_skipped);
  printf("Bytes Scanned: %llu\n", total_bytes);
  printf("Entries Indexed: %llu\n", stats.entries_indexed);
  printf("Dedupe Request Posts Found: %llu\n", cstats.posts_found);
  printf("Dedupe Request Posts Not Found: %llu\n", cstats.posts_not_found);
  printf("Dedupe Percentage: %2.3f%%\n",
         ((double)cstats.posts_found/(double)cstats.requests) * 100);
  double saved
     = (double)compressed_bytes / (double)total_bytes;
  printf("Compressed Bytes: %llu\n", compressed_bytes);
  printf("Percent Saved Compression: %2.3f%%\n", saved * 100.0);
  printf("Total Bytes Used: %llu\n", bytes_used);
  saved = ((double)total_bytes - (double)bytes_used) / (double)total_bytes; 
  printf("Total Percent Saved: %2.3f%%\n", saved * 100.0);
  printf("Peak Concurrent Requests: %u\n", peak_requests);
#if 0
  // uds does not return the corrent index size
  printf("Estimate Index Size: %luM\n", stats.diskUsed/(1024*1024));
#endif
  result = uds_resume_index_session(session);
  if (result != UDS_SUCCESS) {
    errx(1, "Unable to resume the index");
  }
  result = uds_close_index(session);
  if (result != UDS_SUCCESS) {
    errx(1, "Unable to close the index");
  }
  pthread_mutex_destroy(&list_mutex);
  return 0;
 }
