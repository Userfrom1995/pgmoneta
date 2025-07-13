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
 *
 */

#include <pgmoneta.h>
#include <http.h>
#include <logging.h>
#include <tsclient.h>
#include <stdio.h> 
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>


int
pgmoneta_tsclient_execute_http()
{
   int status;
   struct http* h = NULL;

   pgmoneta_init_logging();

   const char* hostname = "localhost";
   int port = 80;
   bool secure = false;

   printf("[tsclient] Connecting to %s:%d (IPv4, HTTP)\n", hostname, port);

   if (pgmoneta_http_connect((char*)hostname, port, secure, &h))
   {
      printf("[tsclient] Failed to connect to %s:%d (IPv4, HTTP)\n", hostname, port);
      return 1;
   }

   // Log local (ephemeral) port
   struct sockaddr_in local_addr;
   socklen_t addr_len = sizeof(local_addr);
   if (getsockname(h->socket, (struct sockaddr*)&local_addr, &addr_len) == 0)
   {
      printf("[tsclient] Local request sent from %s:%d\n",
             inet_ntoa(local_addr.sin_addr), ntohs(local_addr.sin_port));
   }
   else
   {
      perror("[tsclient] getsockname failed");
   }

   status = pgmoneta_http_get(h, (char*)hostname, "/get");

   pgmoneta_http_disconnect(h);
   pgmoneta_http_destroy(h);

   return (status == 0) ? 0 : 1;
}

int
pgmoneta_tsclient_execute_https()
{
   int status;
   struct http* h = NULL;

   pgmoneta_init_logging();

   const char* hostname = "localhost";
   int port = 80;
   bool secure = false;

   printf("[tsclient] Connecting to %s:%d (IPv4, HTTPS)\n", hostname, port);

   if (pgmoneta_http_connect((char*)hostname, port, secure, &h))
   {
      printf("[tsclient] Failed to connect to %s:%d (IPv4, HTTPS)\n", hostname, port);
      return 1;
   }

   // Log local (ephemeral) port
   struct sockaddr_in local_addr;
   socklen_t addr_len = sizeof(local_addr);
   if (getsockname(h->socket, (struct sockaddr*)&local_addr, &addr_len) == 0)
   {
      printf("[tsclient] Local request sent from %s:%d\n",
             inet_ntoa(local_addr.sin_addr), ntohs(local_addr.sin_port));
   }
   else
   {
      perror("[tsclient] getsockname failed");
   }

   status = pgmoneta_http_get(h, (char*)hostname, "/get");

   pgmoneta_http_disconnect(h);
   pgmoneta_http_destroy(h);

   return (status == 0) ? 0 : 1;
}

int
pgmoneta_tsclient_execute_http_post()
{
   int status;
   struct http* h = NULL;

   pgmoneta_init_logging();

   const char* hostname = "localhost";
   int port = 80;
   bool secure = false;
   const char* test_data = "name=pgmoneta&version=1.0";

   printf("[tsclient] Connecting to %s:%d (IPv4, HTTP POST)\n", hostname, port);

   if (pgmoneta_http_connect((char*)hostname, port, secure, &h))
   {
      printf("[tsclient] Failed to connect to %s:%d (IPv4, HTTP POST)\n", hostname, port);
      return 1;
   }

   // Log local (ephemeral) port
   struct sockaddr_in local_addr;
   socklen_t addr_len = sizeof(local_addr);
   if (getsockname(h->socket, (struct sockaddr*)&local_addr, &addr_len) == 0)
   {
      printf("[tsclient] Local request sent from %s:%d\n",
             inet_ntoa(local_addr.sin_addr), ntohs(local_addr.sin_port));
   }
   else
   {
      perror("[tsclient] getsockname failed");
   }

   status = pgmoneta_http_post(h, (char*)hostname, "/post", (char*)test_data, strlen(test_data));

   pgmoneta_http_disconnect(h);
   pgmoneta_http_destroy(h);

   return (status == 0) ? 0 : 1;
}

int
pgmoneta_tsclient_execute_http_put()
{
   int status;
   struct http* h = NULL;

   pgmoneta_init_logging();

   const char* hostname = "localhost";
   int port = 80;
   bool secure = false;
   const char* test_data = "This is a test file content for PUT request";

   printf("[tsclient] Connecting to %s:%d (IPv4, HTTP PUT)\n", hostname, port);

   if (pgmoneta_http_connect((char*)hostname, port, secure, &h))
   {
      printf("[tsclient] Failed to connect to %s:%d (IPv4, HTTP PUT)\n", hostname, port);
      return 1;
   }

   // Log local (ephemeral) port
   struct sockaddr_in local_addr;
   socklen_t addr_len = sizeof(local_addr);
   if (getsockname(h->socket, (struct sockaddr*)&local_addr, &addr_len) == 0)
   {
      printf("[tsclient] Local request sent from %s:%d\n",
             inet_ntoa(local_addr.sin_addr), ntohs(local_addr.sin_port));
   }
   else
   {
      perror("[tsclient] getsockname failed");
   }

   status = pgmoneta_http_put(h, (char*)hostname, "/put", (void*)test_data, strlen(test_data));

   pgmoneta_http_disconnect(h);
   pgmoneta_http_destroy(h);

   return (status == 0) ? 0 : 1;
}

int
pgmoneta_tsclient_execute_http_put_file()
{
   int status;
   struct http* h = NULL;
   FILE* temp_file = NULL;

   pgmoneta_init_logging();

   const char* hostname = "localhost";
   int port = 80;
   bool secure = false;
   const char* test_data = "This is a test file content for PUT file request\nSecond line of test data\nThird line with some numbers: 12345";
   size_t data_len = strlen(test_data);

   temp_file = tmpfile();
   if (temp_file == NULL)
   {
      printf("[tsclient] Failed to create temp file for PUT file\n");
      return 1;
   }

   if (fwrite(test_data, 1, data_len, temp_file) != data_len)
   {
      printf("[tsclient] Failed to write to temp file for PUT file\n");
      fclose(temp_file);
      return 1;
   }

   rewind(temp_file);

   printf("[tsclient] Connecting to %s:%d (IPv4, HTTP PUT FILE)\n", hostname, port);

   if (pgmoneta_http_connect((char*)hostname, port, secure, &h))
   {
      printf("[tsclient] Failed to connect to %s:%d (IPv4, HTTP PUT FILE)\n", hostname, port);
      fclose(temp_file);
      return 1;
   }

   // Log local (ephemeral) port
   struct sockaddr_in local_addr;
   socklen_t addr_len = sizeof(local_addr);
   if (getsockname(h->socket, (struct sockaddr*)&local_addr, &addr_len) == 0)
   {
      printf("[tsclient] Local request sent from %s:%d\n",
             inet_ntoa(local_addr.sin_addr), ntohs(local_addr.sin_port));
   }
   else
   {
      perror("[tsclient] getsockname failed");
   }

   status = pgmoneta_http_put_file(h, (char*)hostname, "/put", temp_file, data_len, "text/plain");

   pgmoneta_http_disconnect(h);
   pgmoneta_http_destroy(h);
   fclose(temp_file);

   return (status == 0) ? 0 : 1;
}