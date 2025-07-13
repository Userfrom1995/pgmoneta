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

#include <stdio.h>
#include <tsclient.h>
#include "pgmoneta_test_2.h"
#include <unistd.h> 
static int call_count = 0;

// test backup
START_TEST(test_pgmoneta_backup)
{
   
   call_count++;
   printf("[DEBUG] test_pgmoneta_backup: call #%d - starting\n", call_count);

   int found = 0;
   found = !pgmoneta_tsclient_execute_backup("primary", NULL);

   printf("[DEBUG] test_pgmoneta_backup: call #%d - after execute_backup, found=%d\n", call_count, found);

   ck_assert_msg(found, "success status not found");

   printf("[DEBUG] test_pgmoneta_backup: call #%d - finished\n", call_count);
   
   sleep(2);
}
END_TEST

// test restore
START_TEST(test_pgmoneta_restore)
{
   printf("[DEBUG] test_pgmoneta_restore: starting\n");
   int found = 0;
   found = !pgmoneta_tsclient_execute_restore("primary", "newest", "current");
   printf("[DEBUG] test_pgmoneta_restore: after execute_restore, found=%d\n", found);
   ck_assert_msg(found, "success status not found");
   printf("[DEBUG] test_pgmoneta_restore: finished\n");
}
END_TEST

// test delete
START_TEST(test_pgmoneta_delete)
{
   printf("[DEBUG] test_pgmoneta_delete: starting\n");
   int found = 0;
   found = !pgmoneta_tsclient_execute_delete("primary", "oldest");
   printf("[DEBUG] test_pgmoneta_delete: after execute_delete, found=%d\n", found);
   ck_assert_msg(found, "success status not found");
   printf("[DEBUG] test_pgmoneta_delete: finished\n");
}
END_TEST

Suite*
pgmoneta_test2_suite()
{
   Suite* s;
   TCase* tc_core;
   s = suite_create("pgmoneta_test2");

   tc_core = tcase_create("Core");

   tcase_set_timeout(tc_core, 60);
   tcase_add_test(tc_core, test_pgmoneta_backup);
   tcase_add_test(tc_core, test_pgmoneta_backup);
   tcase_add_test(tc_core, test_pgmoneta_delete);
   tcase_add_test(tc_core, test_pgmoneta_restore);

   suite_add_tcase(s, tc_core);

   return s;
}