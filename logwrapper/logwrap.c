/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>

#include <logwrap/logwrap.h>
#include "private/android_filesystem_config.h"
#include "cutils/log.h"

static int fatal(const char *msg) {
    fprintf(stderr, "%s", msg);
    ALOG(LOG_ERROR, "logwrapper", "%s", msg);
    return -1;
}

void parent(const char *tag, int parent_read, int *chld_sts) {
    int status;
    char buffer[4096];

    int a = 0;  // start index of unprocessed data
    int b = 0;  // end index of unprocessed data
    int sz;

    char *btag = basename(tag);
    if (!btag) btag = (char*) tag;

    while ((sz = read(parent_read, &buffer[b], sizeof(buffer) - 1 - b)) > 0) {

        sz += b;
        // Log one line at a time
        for (b = 0; b < sz; b++) {
            if (buffer[b] == '\r') {
                buffer[b] = '\0';
            } else if (buffer[b] == '\n') {
                buffer[b] = '\0';
                ALOG(LOG_INFO, btag, "%s", &buffer[a]);
                a = b + 1;
            }
        }

        if (a == 0 && b == sizeof(buffer) - 1) {
            // buffer is full, flush
            buffer[b] = '\0';
            ALOG(LOG_INFO, btag, "%s", &buffer[a]);
            b = 0;
        } else if (a != b) {
            // Keep left-overs
            b -= a;
            memmove(buffer, &buffer[a], b);
            a = 0;
        } else {
            a = 0;
            b = 0;
        }

    }
    // Flush remaining data
    if (a != b) {
        buffer[b] = '\0';
        ALOG(LOG_INFO, btag, "%s", &buffer[a]);
    }

    if (wait(&status) != -1) {  // Wait for child
        if (WIFEXITED(status) && WEXITSTATUS(status))
            ALOG(LOG_INFO, "logwrapper", "%s terminated by exit(%d)", tag,
                    WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            ALOG(LOG_INFO, "logwrapper", "%s terminated by signal %d", tag,
                    WTERMSIG(status));
        else if (WIFSTOPPED(status))
            ALOG(LOG_INFO, "logwrapper", "%s stopped by signal %d", tag,
                    WSTOPSIG(status));
        if (chld_sts != NULL)
            *chld_sts = status;
    } else
        ALOG(LOG_INFO, "logwrapper", "%s wait() failed: %s (%d)", tag,
                strerror(errno), errno);
}

void child(int argc, char* argv[]) {
    // create null terminated argv_child array
    char* argv_child[argc + 1];
    memcpy(argv_child, argv, argc * sizeof(char *));
    argv_child[argc] = NULL;

    if (execvp(argv_child[0], argv_child)) {
        ALOG(LOG_ERROR, "logwrapper",
            "executing %s failed: %s\n", argv_child[0], strerror(errno));
        exit(-1);
    }
}

int logwrap(int argc, char* argv[], int *chld_sts) {
    pid_t pid;

    int parent_ptty;
    int child_ptty;
    char *child_devname = NULL;

    /* Use ptty instead of socketpair so that STDOUT is not buffered */
    parent_ptty = open("/dev/ptmx", O_RDWR);
    if (parent_ptty < 0) {
        return fatal("Cannot create parent ptty\n");
    }

    if (grantpt(parent_ptty) || unlockpt(parent_ptty) ||
            ((child_devname = (char*)ptsname(parent_ptty)) == 0)) {
        return fatal("Problem with /dev/ptmx\n");
    }

    pid = fork();
    if (pid < 0) {
        close(parent_ptty);
        return fatal("Failed to fork\n");
    } else if (pid == 0) {
        child_ptty = open(child_devname, O_RDWR);
        close(parent_ptty);
        if (child_ptty < 0) {
            return fatal("Problem with child ptty\n");
        }

        // redirect stdout and stderr
        dup2(child_ptty, 1);
        dup2(child_ptty, 2);
        close(child_ptty);

        child(argc - 1, &argv[1]);

    } else {
        // switch user and group to "log"
        // this may fail if we are not root, 
        // but in that case switching user/group is unnecessary 
        setgid(AID_LOG);
        setuid(AID_LOG);

        parent(argv[1], parent_ptty, chld_sts);
    }

    return 0;
}