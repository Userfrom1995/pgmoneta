/*
 * Copyright (C) 2025 The pgmoneta community
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list
 * of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this
 * list of conditions and the following disclaimer in the documentation and/or other
 * materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may
 * be used to endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 * TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* pgmoneta */
#include "server.h"
#include <pgmoneta.h>
#include <deque.h>
#include <extension.h>
#include <logging.h>
#include <message.h>
#include <network.h>
#include <security.h>
#include <utils.h>

/* system */
#include <ev.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <sys/stat.h>
#include <sys/types.h>

static int get_primary(SSL* ssl, int socket, bool* primary);
static int get_wal_level(SSL* ssl, int socket, bool* replica);
static int get_wal_size(SSL* ssl, int socket, int* ws);
static int get_checksums(SSL* ssl, int socket, bool* checksums);
static int get_segment_size(SSL* ssl, int socket, size_t* segsz);
static int get_block_size(SSL* ssl, int socket, size_t* blocksz);
static int get_summarize_wal(SSL* ssl, int socket, bool* sw);
static int process_server_parameters(int server, struct deque* server_parameters);

static bool is_valid_response(struct query_response* response);

void
pgmoneta_server_info(int srv, SSL* ssl, int socket)
{
   bool primary = false;
   bool replica = false;
   bool checksums = false;
   int ws = 0;
   size_t blocksz = 0;
   size_t segsz = 0;
   bool sw = false;
   struct main_configuration* config;
   struct deque* server_parameters = NULL;

   config = (struct main_configuration*)shmem;

   if (ssl == NULL && socket < 0)
   {
      pgmoneta_log_error("Unable to connect to server %s", config->common.servers[srv].name);
      goto done;
   }

   config->common.servers[srv].online = true;
   config->common.servers[srv].valid = false;
   config->common.servers[srv].checksums = false;

   if (pgmoneta_extract_server_parameters(&server_parameters))
   {
      pgmoneta_log_error("Unable to extract server parameters for %s", config->common.servers[srv].name);
      goto done;
   }

   if (process_server_parameters(srv, server_parameters))
   {
      pgmoneta_log_error("Unable to process server_parameters for %s", config->common.servers[srv].name);
      goto done;
   }

   pgmoneta_log_debug("%s/version %d.%d", config->common.servers[srv].name,
                      config->common.servers[srv].version, config->common.servers[srv].minor_version);

   if (get_primary(ssl, socket, &primary))
   {
      pgmoneta_log_error("Unable to get primary information for %s", config->common.servers[srv].name);
      config->common.servers[srv].primary = false;
      goto done;
   }
   else
   {
      config->common.servers[srv].primary = primary;
   }

   if (get_wal_level(ssl, socket, &replica))
   {
      pgmoneta_log_error("Unable to get wal_level for %s", config->common.servers[srv].name);
      config->common.servers[srv].valid = false;
      goto done;
   }
   else
   {
      config->common.servers[srv].valid = replica;
   }

   pgmoneta_log_debug("%s/wal_level %s", config->common.servers[srv].name, config->common.servers[srv].valid ? "Yes" : "No");

   if (get_checksums(ssl, socket, &checksums))
   {
      pgmoneta_log_error("Unable to get data_checksums for %s", config->common.servers[srv].name);
      config->common.servers[srv].checksums = false;
      goto done;
   }
   else
   {
      config->common.servers[srv].checksums = checksums;
   }

   pgmoneta_log_debug("%s/data_checksums %s", config->common.servers[srv].name, config->common.servers[srv].checksums ? "Yes" : "No");

   if (get_wal_size(ssl, socket, &ws))
   {
      pgmoneta_log_error("Unable to get wal_segment_size for %s", config->common.servers[srv].name);
      config->common.servers[srv].valid = false;
      goto done;
   }
   else
   {
      config->common.servers[srv].wal_size = ws;
   }
   pgmoneta_log_debug("%s/wal_segment_size %d", config->common.servers[srv].name, config->common.servers[srv].wal_size);

   if (pgmoneta_detect_server_extensions(srv))
   {
      pgmoneta_log_warn("Unable to detect extensions in server %s", config->common.servers[srv].name);
   }

   pgmoneta_log_debug("%s has_extension: %s, ext_version: %s",
                      config->common.servers[srv].name,
                      config->common.servers[srv].has_extension ? "true" : "false",
                      config->common.servers[srv].has_extension ? config->common.servers[srv].ext_version : "N/A");
   if (get_segment_size(ssl, socket, &segsz))
   {
      pgmoneta_log_error("Unable to get segment_size for %s", config->common.servers[srv].name);
      config->common.servers[srv].valid = false;
      goto done;
   }
   else
   {
      config->common.servers[srv].segment_size = segsz;
   }
   pgmoneta_log_debug("%s/segment_size %d", config->common.servers[srv].name, config->common.servers[srv].segment_size);

   if (get_block_size(ssl, socket, &blocksz))
   {
      pgmoneta_log_error("Unable to get block_size for %s", config->common.servers[srv].name);
      config->common.servers[srv].valid = false;
      goto done;
   }
   else
   {
      config->common.servers[srv].block_size = blocksz;
   }
   pgmoneta_log_debug("%s/block_size %d", config->common.servers[srv].name, config->common.servers[srv].block_size);

   config->common.servers[srv].relseg_size = config->common.servers[srv].segment_size / config->common.servers[srv].block_size;

   if (config->common.servers[srv].version >= 17)
   {
      if (get_summarize_wal(ssl, socket, &sw))
      {
         pgmoneta_log_error("Unable to get summarize_wal for %s", config->common.servers[srv].name);
         config->common.servers[srv].summarize_wal = false;
         goto done;
      }
      else
      {
         config->common.servers[srv].summarize_wal = sw;
      }
   }
   pgmoneta_log_debug("%s/summarize_wal %d", config->common.servers[srv].name, config->common.servers[srv].summarize_wal);

done:

   pgmoneta_deque_destroy(server_parameters);

   if (!config->common.servers[srv].valid)
   {
      pgmoneta_log_error("Server %s need wal_level at replica or logical", config->common.servers[srv].name);
   }
}

bool
pgmoneta_server_valid(int srv)
{
   struct main_configuration* config;

   config = (struct main_configuration*)shmem;

   if (!config->common.servers[srv].valid)
   {
      return false;
   }

   if (config->common.servers[srv].version == 0)
   {
      return false;
   }

   if (config->common.servers[srv].wal_size == 0)
   {
      return false;
   }

   if (config->common.servers[srv].segment_size == 0 || config->common.servers[srv].block_size == 0)
   {
      return false;
   }

   return true;
}

bool
pgmoneta_server_verify_connection(int srv)
{
   int fd = -1;
   struct main_configuration* config;

   config = (struct main_configuration*)shmem;

   if (pgmoneta_connect(config->common.servers[srv].host,
                        config->common.servers[srv].port,
                        &fd))
   {
      pgmoneta_log_debug("No connection to %s:%d",
                         config->common.servers[srv].host,
                         config->common.servers[srv].port);
   }
   else
   {
      pgmoneta_disconnect(fd);
      return true;
   }

   return false;
}

static int
get_wal_size(SSL* ssl, int socket, int* ws)
{
   int q = 0;
   bool mb = true;
   int ret;
   char wal_size[MISC_LENGTH];
   struct message* query_msg = NULL;
   struct query_response* response = NULL;

   ret = pgmoneta_create_query_message("SHOW wal_segment_size;", &query_msg);
   if (ret != MESSAGE_STATUS_OK)
   {
      goto error;
   }

q:

   pgmoneta_query_execute(ssl, socket, query_msg, &response);

   if (!is_valid_response(response))
   {
      pgmoneta_free_query_response(response);
      response = NULL;

      SLEEP(5000000L);

      q++;

      if (q < 5)
      {
         goto q;
      }
      else
      {
         goto error;
      }
   }

   memset(&wal_size[0], 0, sizeof(wal_size));

   snprintf(&wal_size[0], sizeof(wal_size), "%s", response->tuples->data[0]);

   if (pgmoneta_ends_with(&wal_size[0], "MB"))
   {
      mb = true;
   }
   else
   {
      mb = false;
   }

   wal_size[strlen(wal_size)] = '\0';
   wal_size[strlen(wal_size)] = '\0';

   *ws = pgmoneta_atoi(wal_size);

   if (mb)
   {
      *ws = *ws * 1024 * 1024;
   }
   else
   {
      *ws = *ws * 1024 * 1024 * 1024;
   }

   pgmoneta_free_query_response(response);
   pgmoneta_free_message(query_msg);

   return 0;
error:

   pgmoneta_log_error("Error getting wal_segment_size");

   pgmoneta_query_response_debug(response);
   pgmoneta_free_query_response(response);
   pgmoneta_free_message(query_msg);
   return 1;
}

static int
get_primary(SSL* ssl, int socket, bool* primary)
{
   bool p = false;
   int status;
   size_t size = 40;
   signed char state;
   char is_recovery[size];
   struct message qmsg;
   struct message* tmsg = NULL;

   *primary = false;

   memset(&qmsg, 0, sizeof(struct message));
   memset(&is_recovery, 0, size);

   pgmoneta_write_byte(&is_recovery, 'Q');
   pgmoneta_write_int32(&(is_recovery[1]), size - 1);
   pgmoneta_write_string(&(is_recovery[5]), "SELECT * FROM pg_is_in_recovery();");

   qmsg.kind = 'Q';
   qmsg.length = size;
   qmsg.data = &is_recovery;

   status = pgmoneta_write_message(ssl, socket, &qmsg);
   if (status != MESSAGE_STATUS_OK)
   {
      goto error;
   }

   status = pgmoneta_read_block_message(ssl, socket, &tmsg);
   if (status != MESSAGE_STATUS_OK)
   {
      goto error;
   }

   /* Read directly from the D message fragment */
   state = pgmoneta_read_byte(tmsg->data + 54);

   pgmoneta_clear_message();

   if (state == 'f')
   {
      p = true;
   }
   else
   {
      p = false;
   }

   *primary = p;

   return 0;

error:
   pgmoneta_clear_message();

   return 1;
}

static int
get_wal_level(SSL* ssl, int socket, bool* replica)
{
   int q = 0;
   int ret;
   char wal_level[MISC_LENGTH];
   struct message* query_msg = NULL;
   struct query_response* response = NULL;

   *replica = false;

   ret = pgmoneta_create_query_message("SHOW wal_level;", &query_msg);
   if (ret != MESSAGE_STATUS_OK)
   {
      goto error;
   }

q:

   pgmoneta_query_execute(ssl, socket, query_msg, &response);

   if (!is_valid_response(response))
   {
      pgmoneta_free_query_response(response);
      response = NULL;

      SLEEP(5000000L);

      q++;

      if (q < 5)
      {
         goto q;
      }
      else
      {
         goto error;
      }
   }

   memset(&wal_level[0], 0, sizeof(wal_level));

   snprintf(&wal_level[0], sizeof(wal_level), "%s", response->tuples->data[0]);

   if (!strcmp("replica", wal_level) || !strcmp("logical", wal_level))
   {
      *replica = true;
   }

   pgmoneta_free_query_response(response);
   pgmoneta_free_message(query_msg);

   return 0;
error:

   pgmoneta_log_error("Error getting wal_level");

   pgmoneta_query_response_debug(response);
   pgmoneta_free_query_response(response);
   pgmoneta_free_message(query_msg);
   return 1;
}

static int
get_checksums(SSL* ssl, int socket, bool* checksums)
{
   int q = 0;
   int ret;
   char data_checksums[MISC_LENGTH];
   struct message* query_msg = NULL;
   struct query_response* response = NULL;

   *checksums = false;

   ret = pgmoneta_create_query_message("SHOW data_checksums;", &query_msg);
   if (ret != MESSAGE_STATUS_OK)
   {
      goto error;
   }

q:

   pgmoneta_query_execute(ssl, socket, query_msg, &response);

   if (!is_valid_response(response))
   {
      pgmoneta_free_query_response(response);
      response = NULL;

      SLEEP(5000000L);

      q++;

      if (q < 5)
      {
         goto q;
      }
      else
      {
         goto error;
      }
   }

   memset(&data_checksums[0], 0, sizeof(data_checksums));

   snprintf(&data_checksums[0], sizeof(data_checksums), "%s", response->tuples->data[0]);

   if (!strcmp("on", data_checksums))
   {
      *checksums = true;
   }

   pgmoneta_free_query_response(response);
   pgmoneta_free_message(query_msg);

   return 0;
error:

   pgmoneta_log_error("Error getting data_checksums");

   pgmoneta_query_response_debug(response);
   pgmoneta_free_query_response(response);
   pgmoneta_free_message(query_msg);
   return 1;
}

static int
get_segment_size(SSL* ssl, int socket, size_t* segsz)
{
   int q = 0;
   bool mb = true;
   int ret;
   char seg_size[MISC_LENGTH];
   struct message* query_msg = NULL;
   struct query_response* response = NULL;

   ret = pgmoneta_create_query_message("SHOW segment_size;", &query_msg);
   if (ret != MESSAGE_STATUS_OK)
   {
      goto error;
   }

q:

   pgmoneta_query_execute(ssl, socket, query_msg, &response);

   if (!is_valid_response(response))
   {
      pgmoneta_free_query_response(response);
      response = NULL;

      SLEEP(5000000L);

      q++;

      if (q < 5)
      {
         goto q;
      }
      else
      {
         goto error;
      }
   }

   memset(&seg_size[0], 0, sizeof(seg_size));

   snprintf(&seg_size[0], sizeof(seg_size), "%s", response->tuples->data[0]);

   if (pgmoneta_ends_with(&seg_size[0], "MB"))
   {
      mb = true;
   }
   else
   {
      mb = false;
   }

   seg_size[strlen(seg_size) - 2] = '\0';

   *segsz = pgmoneta_atoi(seg_size);

   if (mb)
   {
      *segsz = *segsz * 1024 * 1024;
   }
   else
   {
      *segsz = *segsz * 1024 * 1024 * 1024;
   }

   pgmoneta_free_query_response(response);
   pgmoneta_free_message(query_msg);

   return 0;
error:

   pgmoneta_log_error("Error getting segment_size");

   pgmoneta_query_response_debug(response);
   pgmoneta_free_query_response(response);
   pgmoneta_free_message(query_msg);
   return 1;
}

static int
get_block_size(SSL* ssl, int socket, size_t* blocksz)
{
   int q = 0;
   int ret;
   char block_size[MISC_LENGTH];
   struct message* query_msg = NULL;
   struct query_response* response = NULL;

   ret = pgmoneta_create_query_message("SHOW block_size;", &query_msg);
   if (ret != MESSAGE_STATUS_OK)
   {
      goto error;
   }

q:

   pgmoneta_query_execute(ssl, socket, query_msg, &response);

   if (!is_valid_response(response))
   {
      pgmoneta_free_query_response(response);
      response = NULL;

      SLEEP(5000000L);

      q++;

      if (q < 5)
      {
         goto q;
      }
      else
      {
         goto error;
      }
   }

   memset(&block_size[0], 0, sizeof(block_size));

   snprintf(&block_size[0], sizeof(block_size), "%s", response->tuples->data[0]);

   *blocksz = pgmoneta_atoi(block_size);

   pgmoneta_free_query_response(response);
   pgmoneta_free_message(query_msg);

   return 0;
error:

   pgmoneta_log_error("Error getting block_size");

   pgmoneta_query_response_debug(response);
   pgmoneta_free_query_response(response);
   pgmoneta_free_message(query_msg);
   return 1;
}

static int
get_summarize_wal(SSL* ssl, int socket, bool* sw)
{
   int q = 0;
   int ret;
   char summarize_wal[MISC_LENGTH];
   struct message* query_msg = NULL;
   struct query_response* response = NULL;

   *sw = false;

   ret = pgmoneta_create_query_message("SHOW summarize_wal;", &query_msg);
   if (ret != MESSAGE_STATUS_OK)
   {
      goto error;
   }

q:

   pgmoneta_query_execute(ssl, socket, query_msg, &response);

   if (!is_valid_response(response))
   {
      pgmoneta_free_query_response(response);
      response = NULL;

      SLEEP(5000000L);

      q++;

      if (q < 5)
      {
         goto q;
      }
      else
      {
         goto error;
      }
   }

   memset(&summarize_wal[0], 0, sizeof(summarize_wal));

   snprintf(&summarize_wal[0], sizeof(summarize_wal), "%s", response->tuples->data[0]);

   if (!strcmp("on", summarize_wal))
   {
      *sw = true;
   }

   pgmoneta_free_query_response(response);
   pgmoneta_free_message(query_msg);

   return 0;

error:

   pgmoneta_log_error("Error getting summarize_wal");

   pgmoneta_query_response_debug(response);
   pgmoneta_free_query_response(response);
   pgmoneta_free_message(query_msg);
   return 1;
}

static bool
is_valid_response(struct query_response* response)
{
   struct tuple* tuple = NULL;

   if (response == NULL)
   {
      return false;
   }

   if (response->number_of_columns == 0 || response->tuples == NULL)
   {
      return false;
   }

   tuple = response->tuples;
   while (tuple != NULL)
   {
      for (int i = 0; i < response->number_of_columns; i++)
      {
         /* First column must be defined */
         if (tuple->data[0] == NULL)
         {
            return false;
         }
      }
      tuple = tuple->next;
   }

   return true;
}

static int
process_server_parameters(int server, struct deque* server_parameters)
{
   int status = 0;
   int major = 0;
   int minor = 0;
   struct deque_iterator* iter = NULL;
   struct main_configuration* config;

   config = (struct main_configuration*)shmem;

   config->common.servers[server].version = 0;
   config->common.servers[server].minor_version = 0;

   pgmoneta_deque_iterator_create(server_parameters, &iter);
   while (pgmoneta_deque_iterator_next(iter))
   {
      pgmoneta_log_debug("%s/process server_parameter '%s'", config->common.servers[server].name, iter->tag);
      if (!strcmp("server_version", iter->tag))
      {
         char* server_version = pgmoneta_value_to_string(iter->value, FORMAT_TEXT, NULL, 0);
         if (sscanf(server_version, "%d.%d", &major, &minor) == 2)
         {
            config->common.servers[server].version = major;
            config->common.servers[server].minor_version = minor;
         }
         else if ((major = atoi(server_version)) > 0)
         {
            config->common.servers[server].version = major;
            config->common.servers[server].minor_version = 0;
         }
         else
         {
            pgmoneta_log_error("Unable to parse server_version '%s' for %s",
                               server_version, config->common.servers[server].name);
            config->common.servers[server].valid = false;
            status = 1;
         }
         free(server_version);
      }
   }

   pgmoneta_deque_iterator_destroy(iter);
   return status;
}
