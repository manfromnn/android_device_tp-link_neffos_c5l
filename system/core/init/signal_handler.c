/*
 * Copyright (C) 2010 The Android Open Source Project
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

#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <cutils/sockets.h>
#include <cutils/android_reboot.h>
#include <cutils/list.h>
/* Added by yanwenlong for data_bk. (QL1700) 2014-7-28 begin */
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include "init_parser.h"
#include "property_service.h"
/* Added by yanwenlong for data_bk. (QL1700) 2014-7-28 end */

#include "init.h"
#include "util.h"
#include "log.h"

static int signal_fd = -1;
static int signal_recv_fd = -1;

static void sigchld_handler(int s)
{
    write(signal_fd, &s, 1);
}

#define CRITICAL_CRASH_THRESHOLD    4       /* if we crash >4 times ... */
#define CRITICAL_CRASH_WINDOW       (4*60)  /* ... in 4 minutes, goto recovery*/
/* Added by yanwenlong for mount tmpfs. (huawei) 2014-12-06 begin */
int emerg_umnt_data(void)
{
    int i = 0;
    int j = 0;
    int re = -1;

    while (i < 10)
    {
        stop_main_service();
        usleep(100);
        if (umount2("/data", MNT_DETACH))
        {
            ERROR("umount data failed\n");
        }
        else
        {
            ERROR("umount data success\n");
            return 0;
        }
        i++;
    }
    return -1;
}

#define QCOM_USER_DATA "/dev/block/bootdevice/by-name/userdata"
#define CRYPTO_TMPFS_OPTIONS "mode=0771,uid=1000,gid=1000"

int emerg_mount_data(void)
{
    int ret = 0;
    ret = mount(QCOM_USER_DATA, "/data_bk", "ext4",
            //MS_NOATIME | MS_NOSUID | MS_NODEV, CRYPTO_TMPFS_OPTIONS);
        MS_RDONLY, 0);
    if (ret != 0)
    {
        ret = mount(QCOM_USER_DATA, "/data_bk", "ext4",
                MS_NOATIME | MS_NOSUID | MS_NODEV, CRYPTO_TMPFS_OPTIONS);
    }
    ret = mount("tmpfs", "/data", "tmpfs",
            MS_NOATIME | MS_NOSUID | MS_NODEV, CRYPTO_TMPFS_OPTIONS);
    return ret;
}
/* Added by yanwenlong for mount tmpfs. (huawei) 2014-12-06 end */

static int wait_for_one_process(int block)
{
    pid_t pid;
    int status;
    struct service *svc;
    struct socketinfo *si;
    time_t now;
    struct listnode *node;
    struct command *cmd;
    /* add luohao for data_bk. (701) 20150312 begin */
    struct service *svc_zygote;
    char mnt_tmpfs[PROP_VALUE_MAX] = {0};
    char dev_boot_complete[PROP_VALUE_MAX] = {0};
    char sys_boot_complete[PROP_VALUE_MAX] = {0};
    /* add luohao for data_bk. (701) 20150312 end */

    while ( (pid = waitpid(-1, &status, block ? 0 : WNOHANG)) == -1 && errno == EINTR );
    if (pid <= 0) return -1;
    INFO("waitpid returned pid %d, status = %08x\n", pid, status);

    svc = service_find_by_pid(pid);
    if (!svc) {
        if (WIFEXITED(status)) {
            ERROR("untracked pid %d exited with status %d\n", pid, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            ERROR("untracked pid %d killed by signal %d\n", pid, WTERMSIG(status));
        } else if (WIFSTOPPED(status)) {
            ERROR("untracked pid %d stopped by signal %d\n", pid, WSTOPSIG(status));
        } else {
            ERROR("untracked pid %d state changed\n", pid);
        }
        return 0;
    }

    NOTICE("process '%s', pid %d exited\n", svc->name, pid);

    if (!(svc->flags & SVC_ONESHOT) || (svc->flags & SVC_RESTART)) {
        kill(-pid, SIGKILL);
        NOTICE("process '%s' killing any children in process group\n", svc->name);
    }

    /* remove any sockets we may have created */
    for (si = svc->sockets; si; si = si->next) {
        char tmp[128];
        snprintf(tmp, sizeof(tmp), ANDROID_SOCKET_DIR"/%s", si->name);
        unlink(tmp);
    }

    svc->pid = 0;
    svc->flags &= (~SVC_RUNNING);

        /* oneshot processes go into the disabled state on exit,
         * except when manually restarted. */
    if ((svc->flags & SVC_ONESHOT) && !(svc->flags & SVC_RESTART)) {
        svc->flags |= SVC_DISABLED;
    }

        /* disabled and reset processes do not get restarted automatically */
    if (svc->flags & (SVC_DISABLED | SVC_RESET) )  {
        notify_service_state(svc->name, "stopped");
        return 0;
    }

    now = gettime();
    /* Added by yanwenlong for data_bk. (QL1700) 2014-12-12 begin */
    property_get("persist.sys.data_reco_mt_tfs", mnt_tmpfs);
    ++svc->nr_crashed;
    if (mnt_tmpfs[0] == '\0' || (strcmp(mnt_tmpfs, "0") == 0)) {
        if (((svc->flags & SVC_EMERGENCY) || (svc->flags & SVC_CRITICAL))
                && !(svc->flags & SVC_RESTART)){
            if (svc->time_crashed + CRITICAL_CRASH_WINDOW >= now) {
                if (svc->nr_crashed > CRITICAL_CRASH_THRESHOLD) {
                    ERROR("emergency/critical process '%s' exited %d times in %d minutes; "
                            "try data recovery\n", svc->name,
                            CRITICAL_CRASH_THRESHOLD, CRITICAL_CRASH_WINDOW / 60);
                    // begin mount tmpfs
                    //if (emerg_mount_data() != 0)
                    //{
                        if(emerg_umnt_data() == 0)
                        {
                            emerg_mount_data();
                        }
                    //}
                    property_set("persist.sys.littlecore", "1");
                    action_for_each_trigger("post-fs-data", action_add_queue_tail);
                    // end mount tmpfs
                    if (!strcmp(svc->name, "zygote")) {
                        svc_zygote = service_find_by_name("zygote");
                        svc_zygote->flags &= (~SVC_RUNNING);
                        service_start(svc_zygote, NULL);
                        /* Execute all onrestart commands for this service. */
                        list_for_each(node, &svc_zygote->onrestart.commands) {
                            cmd = node_to_item(node, struct command, clist);
                            cmd->func(cmd->nargs, cmd->args);
                        }
                    } // restart framework/zygote
                }
            } else {
                svc->time_crashed = now;
                svc->nr_crashed = 1;
            }
        }
    }
    else if ((svc->flags & SVC_CRITICAL) && !(svc->flags & SVC_RESTART)) {
    /* Added by yanwenlong for data_bk. (QL1700) 2014-12-12 end */
        if (svc->time_crashed + CRITICAL_CRASH_WINDOW >= now) {
            if (svc->nr_crashed > CRITICAL_CRASH_THRESHOLD) {//modify by yanwenlong for data_bk. 8909 20150310
                ERROR("critical process '%s' exited %d times in %d minutes; "
                      "rebooting into recovery mode\n", svc->name,
                      CRITICAL_CRASH_THRESHOLD, CRITICAL_CRASH_WINDOW / 60);
                android_reboot(ANDROID_RB_RESTART2, 0, "recovery");
                return 0;
            }
        } else {
            svc->time_crashed = now;
            svc->nr_crashed = 1;
        }
    }

    svc->flags &= (~SVC_RESTART);
    svc->flags |= SVC_RESTARTING;

    /* Execute all onrestart commands for this service. */
    list_for_each(node, &svc->onrestart.commands) {
        cmd = node_to_item(node, struct command, clist);
        cmd->func(cmd->nargs, cmd->args);
    }
    notify_service_state(svc->name, "restarting");
    return 0;
}

void handle_signal(void)
{
    char tmp[32];

    /* we got a SIGCHLD - reap and restart as needed */
    read(signal_recv_fd, tmp, sizeof(tmp));
    while (!wait_for_one_process(0))
        ;
}

void signal_init(void)
{
    int s[2];

    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = sigchld_handler;
    act.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &act, 0);

    /* create a signalling mechanism for the sigchld handler */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0) {
        signal_fd = s[0];
        signal_recv_fd = s[1];
        fcntl(s[0], F_SETFD, FD_CLOEXEC);
        fcntl(s[0], F_SETFL, O_NONBLOCK);
        fcntl(s[1], F_SETFD, FD_CLOEXEC);
        fcntl(s[1], F_SETFL, O_NONBLOCK);
    }

    handle_signal();
}

int get_signal_fd()
{
    return signal_recv_fd;
}
