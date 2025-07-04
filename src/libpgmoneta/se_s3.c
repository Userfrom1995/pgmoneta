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
#include <pgmoneta.h>
#include <http.h>
#include <logging.h>
#include <security.h>
#include <utils.h>
#include <workflow.h>

/* system */
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* s3_storage_name(void);
static int s3_storage_setup(char*, struct art*);
static int s3_storage_execute(char*, struct art*);
static int s3_storage_teardown(char*, struct art*);

static int s3_upload_files(char* local_root, char* s3_root, char* relative_path);
static int s3_send_upload_request(char* local_root, char* s3_root, char* relative_path);

static char* s3_get_host(void);
static char* s3_get_basepath(int server, char* identifier);

struct workflow*
pgmoneta_storage_create_s3(void)
{
   struct workflow* wf = NULL;

   wf = (struct workflow*)malloc(sizeof(struct workflow));

   wf->name = &s3_storage_name;
   wf->setup = &s3_storage_setup;
   wf->execute = &s3_storage_execute;
   wf->teardown = &s3_storage_teardown;
   wf->next = NULL;

   return wf;
}

static char*
s3_storage_name(void)
{
   return "S3";
}

static int
s3_storage_setup(char* name __attribute__((unused)), struct art* nodes)
{
   int server = -1;
   char* label = NULL;
   struct main_configuration* config;

   config = (struct main_configuration*)shmem;

#ifdef DEBUG
   if (pgmoneta_log_is_enabled(PGMONETA_LOGGING_LEVEL_DEBUG1))
   {
      char* a = NULL;
      a = pgmoneta_art_to_string(nodes, FORMAT_TEXT, NULL, 0);
      pgmoneta_log_debug("(Tree)\n%s", a);
      free(a);
   }
   assert(nodes != NULL);
   assert(pgmoneta_art_contains_key(nodes, NODE_SERVER_ID));
   assert(pgmoneta_art_contains_key(nodes, NODE_LABEL));
#endif

   server = (int)pgmoneta_art_search(nodes, NODE_SERVER_ID);
   label = (char*)pgmoneta_art_search(nodes, NODE_LABEL);

   pgmoneta_log_debug("S3 storage engine (setup): %s/%s", config->common.servers[server].name, label);

   return 0;
}

static int
s3_storage_execute(char* name __attribute__((unused)), struct art* nodes)
{
   int server = -1;
   char* label = NULL;
   struct timespec start_t;
   struct timespec end_t;
   double remote_s3_elapsed_time;
   char* local_root = NULL;
   char* base_dir = NULL;
   char* s3_root = NULL;
   struct main_configuration* config;
   struct backup* temp_backup = NULL;
#ifdef HAVE_FREEBSD
   clock_gettime(CLOCK_MONOTONIC_FAST, &start_t);
#else
   clock_gettime(CLOCK_MONOTONIC_RAW, &start_t);
#endif

   config = (struct main_configuration*)shmem;

#ifdef DEBUG
   if (pgmoneta_log_is_enabled(PGMONETA_LOGGING_LEVEL_DEBUG1))
   {
      char* a = NULL;
      a = pgmoneta_art_to_string(nodes, FORMAT_TEXT, NULL, 0);
      pgmoneta_log_debug("(Tree)\n%s", a);
      free(a);
   }
   assert(nodes != NULL);
   assert(pgmoneta_art_contains_key(nodes, NODE_SERVER_ID));
   assert(pgmoneta_art_contains_key(nodes, NODE_LABEL));
#endif

   server = (int)pgmoneta_art_search(nodes, NODE_SERVER_ID);
   label = (char*)pgmoneta_art_search(nodes, NODE_LABEL);

   pgmoneta_log_debug("S3 storage engine (execute): %s/%s", config->common.servers[server].name, label);

   local_root = pgmoneta_get_server_backup_identifier(server, label);
   base_dir = pgmoneta_get_server_backup(server);
   s3_root = s3_get_basepath(server, label);

   if (s3_upload_files(local_root, s3_root, ""))
   {
      goto error;
   }

#ifdef HAVE_FREEBSD
   clock_gettime(CLOCK_MONOTONIC_FAST, &end_t);
#else
   clock_gettime(CLOCK_MONOTONIC_RAW, &end_t);
#endif

   remote_s3_elapsed_time = pgmoneta_compute_duration(start_t, end_t);

   if (pgmoneta_load_info(base_dir, label, &temp_backup))
   {
      goto error;
   }
   temp_backup->remote_s3_elapsed_time = remote_s3_elapsed_time;
   if (pgmoneta_save_info(base_dir, temp_backup))
   {
      pgmoneta_log_error("Unable to save backup info for directory %s", base_dir);
      goto error;
   }

   free(temp_backup);
   free(local_root);
   free(s3_root);

   return 0;

error:
   free(temp_backup);
   free(local_root);
   free(s3_root);

   return 1;
}

static int
s3_storage_teardown(char* name __attribute__((unused)), struct art* nodes)
{
   int server = -1;
   char* label = NULL;
   char* root = NULL;
   struct main_configuration* config;

   config = (struct main_configuration*)shmem;

#ifdef DEBUG
   if (pgmoneta_log_is_enabled(PGMONETA_LOGGING_LEVEL_DEBUG1))
   {
      char* a = NULL;
      a = pgmoneta_art_to_string(nodes, FORMAT_TEXT, NULL, 0);
      pgmoneta_log_debug("(Tree)\n%s", a);
      free(a);
   }
   assert(nodes != NULL);
   assert(pgmoneta_art_contains_key(nodes, NODE_SERVER_ID));
   assert(pgmoneta_art_contains_key(nodes, NODE_LABEL));
#endif

   server = (int)pgmoneta_art_search(nodes, NODE_SERVER_ID);
   label = (char*)pgmoneta_art_search(nodes, NODE_LABEL);

   pgmoneta_log_debug("S3 storage engine (teardown): %s/%s", config->common.servers[server].name, label);

   root = pgmoneta_get_server_backup_identifier_data(server, label);

   pgmoneta_delete_directory(root);

   free(root);

   return 0;
}

static int
s3_upload_files(char* local_root, char* s3_root, char* relative_path)
{
   char* local_path = NULL;
   char* relative_file;
   DIR* dir;
   struct dirent* entry;

   local_path = pgmoneta_append(local_path, local_root);
   local_path = pgmoneta_append(local_path, relative_path);

   if (!(dir = opendir(local_path)))
   {
      goto error;
   }

   while ((entry = readdir(dir)) != NULL)
   {
      if (entry->d_type == DT_DIR)
      {
         char relative_dir[1024];

         if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
         {
            continue;
         }

         snprintf(relative_dir, sizeof(relative_dir), "%s/%s", relative_path, entry->d_name);

         s3_upload_files(local_root, s3_root, relative_dir);
      }
      else
      {
         relative_file = NULL;

         relative_file = pgmoneta_append(relative_file, relative_path);
         relative_file = pgmoneta_append(relative_file, "/");
         relative_file = pgmoneta_append(relative_file, entry->d_name);

         if (s3_send_upload_request(local_root, s3_root, relative_file))
         {
            free(relative_file);
            goto error;
         }

         free(relative_file);
      }
   }

   closedir(dir);

   free(local_path);

   return 0;

error:

   closedir(dir);

   free(local_path);

   return 1;
}

static int
s3_send_upload_request(char* local_root, char* s3_root, char* relative_path)
{
   char short_date[SHORT_TIME_LENGTH];
   char long_date[LONG_TIME_LENGTH];
   char* canonical_request = NULL;
   char* auth_value = NULL;
   char* string_to_sign = NULL;
   char* s3_host = NULL;
   char* s3_url = NULL;
   char* s3_path = NULL;
   char s3_put_path[MAX_PATH];
   char* file_sha256 = NULL;
   char* canonical_request_sha256 = NULL;
   char* key = NULL;
   char* local_path = NULL;
   unsigned char* date_key_hmac = NULL;
   unsigned char* date_region_key_hmac = NULL;
   unsigned char* date_region_service_key_hmac = NULL;
   unsigned char* signing_key_hmac = NULL;
   unsigned char* signature_hmac = NULL;
   unsigned char* signature_hex = NULL;
   int hmac_length = 0;
   FILE* file = NULL;
   struct stat file_info;
   int res = -1;
   struct http* http = NULL;
   struct main_configuration* config;

   config = (struct main_configuration*)shmem;

   local_path = pgmoneta_append(local_path, local_root);
   local_path = pgmoneta_append(local_path, relative_path);

   s3_path = pgmoneta_append(s3_path, s3_root);
   s3_path = pgmoneta_append(s3_path, relative_path);

   memset(&short_date[0], 0, sizeof(short_date));
   memset(&long_date[0], 0, sizeof(long_date));

   if (pgmoneta_get_timestamp_ISO8601_format(short_date, long_date))
   {
      goto error;
   }

   pgmoneta_create_sha256_file(local_path, &file_sha256);

   s3_host = s3_get_host();

   // Construct canonical request.
   canonical_request = pgmoneta_append(canonical_request, "PUT\n/");
   canonical_request = pgmoneta_append(canonical_request, s3_path);
   canonical_request = pgmoneta_append(canonical_request, "\n\nhost:");
   canonical_request = pgmoneta_append(canonical_request, s3_host);
   canonical_request = pgmoneta_append(canonical_request, "\nx-amz-content-sha256:");
   canonical_request = pgmoneta_append(canonical_request, file_sha256);
   canonical_request = pgmoneta_append(canonical_request, "\nx-amz-date:");
   canonical_request = pgmoneta_append(canonical_request, long_date);
   canonical_request = pgmoneta_append(canonical_request, "\nx-amz-storage-class:REDUCED_REDUNDANCY\n\nhost;x-amz-content-sha256;x-amz-date;x-amz-storage-class\n");
   canonical_request = pgmoneta_append(canonical_request, file_sha256);

   pgmoneta_generate_string_sha256_hash(canonical_request, &canonical_request_sha256);

   // Construct string to sign.
   string_to_sign = pgmoneta_append(string_to_sign, "AWS4-HMAC-SHA256\n");
   string_to_sign = pgmoneta_append(string_to_sign, long_date);
   string_to_sign = pgmoneta_append(string_to_sign, "\n");
   string_to_sign = pgmoneta_append(string_to_sign, short_date);
   string_to_sign = pgmoneta_append(string_to_sign, "/");
   string_to_sign = pgmoneta_append(string_to_sign, config->s3_aws_region);
   string_to_sign = pgmoneta_append(string_to_sign, "/s3/aws4_request\n");
   string_to_sign = pgmoneta_append(string_to_sign, canonical_request_sha256);

   key = pgmoneta_append(key, "AWS4");
   key = pgmoneta_append(key, config->s3_secret_access_key);

   if (pgmoneta_generate_string_hmac_sha256_hash(key, strlen(key), short_date, SHORT_TIME_LENGTH - 1, &date_key_hmac, &hmac_length))
   {
      goto error;
   }

   if (pgmoneta_generate_string_hmac_sha256_hash((char*)date_key_hmac, hmac_length, config->s3_aws_region, strlen(config->s3_aws_region), &date_region_key_hmac, &hmac_length))
   {
      goto error;
   }

   if (pgmoneta_generate_string_hmac_sha256_hash((char*)date_region_key_hmac, hmac_length, "s3", strlen("s3"), &date_region_service_key_hmac, &hmac_length))
   {
      goto error;
   }

   if (pgmoneta_generate_string_hmac_sha256_hash((char*)date_region_service_key_hmac, hmac_length, "aws4_request", strlen("aws4_request"), &signing_key_hmac, &hmac_length))
   {
      goto error;
   }

   if (pgmoneta_generate_string_hmac_sha256_hash((char*)signing_key_hmac, hmac_length, string_to_sign, strlen(string_to_sign), &signature_hmac, &hmac_length))
   {
      goto error;
   }

   pgmoneta_convert_base32_to_hex(signature_hmac, hmac_length, &signature_hex);

   auth_value = pgmoneta_append(auth_value, "AWS4-HMAC-SHA256 Credential=");
   auth_value = pgmoneta_append(auth_value, config->s3_access_key_id);
   auth_value = pgmoneta_append(auth_value, "/");
   auth_value = pgmoneta_append(auth_value, short_date);
   auth_value = pgmoneta_append(auth_value, "/");
   auth_value = pgmoneta_append(auth_value, config->s3_aws_region);
   auth_value = pgmoneta_append(auth_value, "/s3/aws4_request,SignedHeaders=host;x-amz-content-sha256;x-amz-date;x-amz-storage-class,Signature=");
   auth_value = pgmoneta_append(auth_value, (char*)signature_hex);

   // Connect to S3 with our custom HTTP implementation
   if (pgmoneta_http_connect(s3_host, 443, true, &http))
   {
      goto error;
   }

   // Add headers
   pgmoneta_http_add_header(http, "Authorization", auth_value);
   pgmoneta_http_add_header(http, "x-amz-content-sha256", file_sha256);
   pgmoneta_http_add_header(http, "x-amz-date", long_date);
   pgmoneta_http_add_header(http, "x-amz-storage-class", "REDUCED_REDUNDANCY");

   // Construct path for PUT request
   snprintf(s3_put_path, sizeof(s3_put_path), "/%s", s3_path);

   file = fopen(local_path, "rb");
   if (file == NULL)
   {
      goto error;
   }

   if (fstat(fileno(file), &file_info) != 0)
   {
      goto error;
   }

   // Perform the PUT request with file
   res = pgmoneta_http_put_file(http, s3_host, s3_put_path, file, file_info.st_size, "application/octet-stream");
   if (res == 0)
   {
      int status_code = 0;
      if (http->headers && sscanf(http->headers, "HTTP/1.1 %d", &status_code) == 1)
      {
         pgmoneta_log_info("S3 HTTP status code: %d", status_code);
         if (status_code >= 200 && status_code < 300)
         {
            pgmoneta_log_info("Successfully uploaded file to S3: %s", s3_path);
            pgmoneta_log_info("Object URL: https://%s/%s", s3_host, s3_path);
         }
         else
         {
            pgmoneta_log_error("S3 upload failed with status code: %d. Failed to upload: %s to S3 path: %s. Response headers: %s. Response body: %s",
                               status_code, s3_put_path, s3_path,
                               http->headers, http->body ? http->body : "None");
            goto error;
         }
      }
      else
      {
         pgmoneta_log_error("Failed to parse HTTP status code from response. Response headers: %s",
                            http->headers ? http->headers : "None");
         goto error;
      }
   }
   else
   {
      pgmoneta_log_error("pgmoneta_http_put_file() failed");
      goto error;
   }

   free(s3_url);
   free(s3_host);
   free(file_sha256);
   free(signature_hex);
   free(signature_hmac);
   free(signing_key_hmac);
   free(date_region_service_key_hmac);
   free(date_region_key_hmac);
   free(date_key_hmac);
   free(key);
   free(local_path);
   free(s3_path);
   free(canonical_request_sha256);
   free(canonical_request);
   free(string_to_sign);
   free(auth_value);

   pgmoneta_http_disconnect(http);
   pgmoneta_http_destroy(http);

   fclose(file);
   return 0;

error:

   if (s3_url != NULL)
   {
      free(s3_url);
   }

   if (s3_host != NULL)
   {
      free(s3_host);
   }

   if (signature_hex != NULL)
   {
      free(signature_hex);
   }

   if (signature_hmac != NULL)
   {
      free(signature_hmac);
   }

   if (signing_key_hmac != NULL)
   {
      free(signing_key_hmac);
   }

   if (date_region_service_key_hmac != NULL)
   {
      free(date_region_service_key_hmac);
   }

   if (date_region_key_hmac != NULL)
   {
      free(date_region_key_hmac);
   }

   if (date_key_hmac != NULL)
   {
      free(date_region_key_hmac);
   }

   if (key != NULL)
   {
      free(key);
   }

   if (local_path != NULL)
   {
      free(local_path);
   }

   if (s3_path != NULL)
   {
      free(s3_path);
   }

   if (canonical_request_sha256 != NULL)
   {
      free(canonical_request_sha256);
   }

   if (file_sha256 != NULL)
   {
      free(file_sha256);
   }

   if (canonical_request != NULL)
   {
      free(canonical_request);
   }

   if (string_to_sign != NULL)
   {
      free(string_to_sign);
   }

   if (auth_value != NULL)
   {
      free(auth_value);
   }

   if (http != NULL)
   {
      pgmoneta_http_disconnect(http);
      pgmoneta_http_destroy(http);
   }

   if (file != NULL)
   {
      fclose(file);
   }

   return 1;
}

static char*
s3_get_host(void)
{
   char* host = NULL;
   struct main_configuration* config;

   config = (struct main_configuration*)shmem;

   host = pgmoneta_append(host, config->s3_bucket);
   host = pgmoneta_append(host, ".s3.");
   host = pgmoneta_append(host, config->s3_aws_region);
   host = pgmoneta_append(host, ".amazonaws.com");

   return host;
}

static char*
s3_get_basepath(int server, char* identifier)
{
   char* d = NULL;
   struct main_configuration* config;

   config = (struct main_configuration*)shmem;

   d = pgmoneta_append(d, config->s3_base_dir);
   if (!pgmoneta_ends_with(config->s3_base_dir, "/"))
   {
      d = pgmoneta_append(d, "/");
   }
   d = pgmoneta_append(d, config->common.servers[server].name);
   d = pgmoneta_append(d, "/backup/");
   d = pgmoneta_append(d, identifier);

   return d;
}
