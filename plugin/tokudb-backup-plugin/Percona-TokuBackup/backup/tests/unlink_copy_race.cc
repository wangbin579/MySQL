/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
/*======
This file is part of Percona TokuBackup.

Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    Percona TokuBackup is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

     Percona TokuBackup is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Percona TokuBackup.  If not, see <http://www.gnu.org/licenses/>.

----------------------------------------

    Percona TokuBackup is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License, version 3,
    as published by the Free Software Foundation.

    Percona TokuBackup is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with Percona TokuBackup.  If not, see <http://www.gnu.org/licenses/>.
======= */

#ident "$Id$"

#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

#include "backup.h"
#include "backup_internal.h"
#include "backup_test_helpers.h"
#include "backup_debug.h"

const int N = 1;
const int SIZE = 100;
const char BEFORE = 'a';

static void create_n_files(void)
{
    const char *src = get_src();
    for (int i = 0; i < N; ++i) {
        int fd = openf(O_RDWR | O_CREAT, 0777, "%s/my_%d.data", src, i);
        printf("Created file with fd = %d ", fd);
        assert(fd > 0);
        char buf[SIZE] = {BEFORE};
        int pwrite_r = pwrite(fd, buf, SIZE, 0);
        check(pwrite_r == SIZE);
        int close_r = close(fd);
        check(close_r == 0);
    }

    free((void*)src);
}

static int unlink_verify(void)
{
    int result = 0;
    char *source_scratch = get_src();
    char *destination_scratch = get_dst();
    char source_file[SIZE];
    char destination_file[SIZE];

    // Verify the first N were unlinked.
    for (int i = 0; i < N; ++i) {
        snprintf(source_file, SIZE, "%s/my_%d.data", source_scratch, i);
        struct stat blah;
        int stat_r = stat(source_file, &blah);
        check(stat_r != 0);
        int error = errno;
        if (error != ENOENT) {
            printf("source file : %s should not exist.\n", source_file);
            result = -1;
            check(stat_r != 0);
        }

        snprintf(destination_file, SIZE,  "%s/my_%d.data", destination_scratch, i);
        stat_r = stat(destination_file, &blah);
        if (stat_r == 0) {
            error = errno;
            perror("");
            printf("destination file : %s should not exist.\n", destination_file);
            result = -1;
            check(stat_r != 0);
        }
    }

    free((void*)destination_scratch);
    free((void*)source_scratch);
    return result;
}

static void my_unlink(int i)
{
    int r = 0;
    const char * free_me = get_src();
    char name[PATH_MAX] = {0};
    snprintf(name, PATH_MAX, "%s/my_%d.data", free_me, i);
    r = unlink(name);
    check(r == 0);
    free((void*) free_me);
}

static int unlink_test(void)
{
    int result = 0;
    // Create one files.
    create_n_files();
    // Prevent copy from finishing.
    HotBackup::toggle_pause_point(HotBackup::COPIER_AFTER_OPEN_SOURCE);
    pthread_t thread;
    start_backup_thread(&thread);
    // Perform unlink between file ops.
    sleep(3);
    my_unlink(0);
    HotBackup::toggle_pause_point(HotBackup::COPIER_AFTER_OPEN_SOURCE);
    finish_backup_thread(thread);

    result = unlink_verify();
    if (result == 0) {
        pass();
    } else {
        fail();
    }

    return result;
}

int test_main(int argc __attribute__((__unused__)), const char *argv[] __attribute__((__unused__))) {
    int result = 0;
    setup_source();
    setup_destination();
    result = unlink_test();
    cleanup_dirs();
    return result;
}
