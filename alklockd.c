/* alklockd -- Al Klimov's alarm clock daemon
 *
 * Copyright (C) 2015  Alexander A. Klimov
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// C99 && POSIX

#include <stdio.h>
/* fputs()
 * vfprintf()
 * fprintf()
 * stdin
 * stdout
 * stderr
 * fflush()
 * FILE
 */

#include <stdarg.h>
/* va_list
 * va_start()
 * va_end()
 */

#include <stdlib.h>
/* exit()
 * _Exit()
 * EXIT_*
 */

#include <stdint.h>
/* intmax_t
 * uintmax_t
 */

#include <time.h>
/* time_t
 * tm
 * time()
 * localtime()
 */

//#include <signal.h>
/* signal()
 * SIGTERM
 * SIG_ERR
 */

#include <string.h>
// strerror()

#include <stdbool.h>
/* bool
 * true
 * false
 */

#include <unistd.h>
/* fork()
 * setsid()
 * chdir()
 * dup2()
 * close()
 */

#include <errno.h>
// errno

#include <sys/types.h>
// pid_t

#include <sys/wait.h>
/* waitpid()
 * WIFEXITED()
 * WEXITSTATUS()
 * WIFSIGNALED()
 * WTERMSIG()
 */

#include <sys/stat.h>
// umask()

#include <fcntl.h>
// open()

#include <syslog.h>
/* openlog()
 * syslog()
 */

#include <postgresql/libpq-fe.h>
/* PGconn
 * CONNECTION_*
 * PGresult
 * PGRES_*
 * PQ*()
 */

typedef char const * const ccstr;

void eputs(char const * restrict);
void eprintf(char const * restrict, ...);
void efail(char const * restrict);
//void closeConn(int);

//PGconn* pgconn = NULL;

int main(int argc, char** argv) {
    if (argc < 4)
        eputs("Too few arguments\n");

    {
        pid_t child = fork();
        switch (child) {
            case -1:
                efail("1st fork()");
            case 0:
                break;
            default: {
                int status;
                if (-1 == waitpid(child, &status, 0))
                    efail("waitpid()");
                if (WIFEXITED(status))
                    return WEXITSTATUS(status);
                if (WIFSIGNALED(status))
                    eprintf(
                        "The child process (w/ PID %jd) created by the 1st fork()"
                        " was terminated by signal %ju\n",
                        (intmax_t)child,
                        (uintmax_t)WTERMSIG(status)
                    );
                eprintf(
                    "The child process (w/ PID %jd) created by the 1st fork()"
                    " didn't terminate normally\n",
                    (intmax_t)child
                );
            }
        }
    }

    if (-1 == setsid() && errno != EPERM)
        efail("setsid()");
    if (-1 == chdir("/"))
        efail("chdir(\"/\")");
    umask(18); // 022

    {
        pid_t child = fork();
        switch (child) {
            case -1:
                efail("2nd fork()");
            case 0:
                break;
            default:
                fprintf(
                    stderr,
                    "Successfully started daemon w/ PID %jd\n",
                    (intmax_t)child
                );
                return EXIT_SUCCESS;
        }
    }

    openlog(*(ccstr*)argv, LOG_CONS | LOG_NOWAIT | LOG_PID, LOG_USER);

    {
        ccstr stdstr[3] = {"stdin", "stdout", "stderr"};
        ccstr* curstr = stdstr;
        FILE* stdio[3] = {stdin, stdout, stderr};
        FILE** curio = stdio;
        for (int fd, stdfd = 0; stdfd < 3; ++curstr) {
            if (fflush(*curio++)) {
                syslog(LOG_CRIT, "Failed at fflush()ing %s: %m", *curstr);
                return EXIT_FAILURE;
            }

            if (stdfd == 2)
                fd = 1;
            else if (-1 == (fd = open("/dev/null", (stdfd ? O_WRONLY : O_RDONLY) | O_NONBLOCK))) {
                syslog(LOG_CRIT, "Failed at open()ing `/dev/null' while getting a replacement for %s: %m", *curstr);
                return EXIT_FAILURE;
            }
            if (-1 == dup2(fd, stdfd++)) {
                syslog(LOG_CRIT, "Failed at dup2() while replacing %s w/ `/dev/null': %m", *curstr);
                if (fd != 1 && -1 == close(fd))
                    syslog(LOG_CRIT, "Failed at close()ing `/dev/null' while cleaning up: %m");
                return EXIT_FAILURE;
            }
            if (fd != 1 && -1 == close(fd)) {
                syslog(LOG_CRIT, "Failed at close()ing an unneeded file descriptor after dup2()ing it: %m");
                return EXIT_FAILURE;
            }
        }
    }

    /*if (SIG_ERR == signal(SIGTERM, &closeConn)) {
        syslog(LOG_CRIT, "Failed at setting a signal handler for SIGTERM: %m");
        return EXIT_FAILURE;
    }*/

    PGconn* pgconn = PQconnectdb((char const *)argv[1]);
    if (pgconn == NULL) {
        syslog(LOG_CRIT, "Failed at PQconnectdb(): out of memory");
        return EXIT_FAILURE;
    }
    if (CONNECTION_OK != PQstatus(pgconn)) {
        syslog(LOG_ERR, "Failed at PQconnectdb(): %s", (char const *)PQerrorMessage(pgconn));
        return EXIT_FAILURE;
    }
    syslog(LOG_INFO, "Successfully set up a DB connection");

    time_t now;
    struct tm* lnow;
    bool retry;
    PGresult* res;
    int nrows, ncols, status, childStat;
    ccstr playExec = argv[2],
        * music,
        * const musicStart = (ccstr*)argv + 3,
        * const musicStop = (ccstr*)argv + argc;
    pid_t child;

    for (;;) {
        if ((time_t)-1 == time(&now)) {
            syslog(LOG_CRIT, "Failed at getting the current system time: %m");
            break;
        }
        if (NULL == (lnow = localtime(&now))) {
            syslog(LOG_CRIT, "Failed at analyzing the current system time w/ localtime()");
            break;
        }
        if (sleep(300 - (lnow->tm_min * 60 + lnow->tm_sec) % 300))
            continue;

        /* if (CONNECTION_OK != PQstatus(pgconn)) { // Doesn't work as expected.. :/
            PQreset(pgconn);
            if (CONNECTION_OK != PQstatus(pgconn)) {
                syslog(LOG_ERR, "Failed at PQreset()ing a broken DB connection");
                continue;
            }
        } */

        retry = true;
        while (PGRES_TUPLES_OK != PQresultStatus(res = PQexec(
            pgconn, "SELECT alarm_now FROM alklockd_now;"
        ))) {
            if (!retry) {
                syslog(LOG_ERR, "Failed at PQexec(): %s", (char const *)PQresultErrorMessage(res));
                goto ClearRes;
            }
            PQclear(res);
            PQreset(pgconn);
            retry = false;
        }
        if (1 != (ncols = PQnfields(res))) {
            syslog(LOG_ERR, "Failed at querying: expected a resultset of 1 column -- got %d", ncols);
            goto ClearRes;
        }
        if (1 != (nrows = PQntuples(res))) {
            syslog(LOG_ERR, "Failed at querying: expected a resultset of 1 row -- got %d", nrows);
            goto ClearRes;
        }

        switch ((char const)*PQgetvalue(res, 0, 0)) {
            case 't':
            case 'T':
                for (music = musicStart; music < musicStop; ++music)
                    switch (child = fork()) {
                        case -1:
                            syslog(
                                LOG_ERR,
                                "Failed at fork() while playing music: %s",
                                strerror(errno)
                            );
                            break;
                        case 0: {
                            char * const envp[1] = {NULL};
                            execle(
                                playExec, playExec, *music,
                                (char const *)NULL,
                                envp
                            );
                            return EXIT_FAILURE;
                        }
                        default:
                            if (-1 == waitpid(child, &status, 0))
                                syslog(
                                    LOG_ERR,
                                    "Failed at waitpid(%jd) while playing music",
                                    (intmax_t)child
                                );
                            else if (WIFEXITED(status)) {
                                if ((childStat = WEXITSTATUS(status)))
                                    syslog(
                                        LOG_ERR,
                                        "The music-playing child process (w/ PID %jd) returned status %d",
                                        (intmax_t)child,
                                        childStat
                                    );
                            } else if (WIFSIGNALED(status)) {
                                syslog(
                                    LOG_ERR,
                                    "The music-playing child process (w/ PID %jd) was terminated by signal %ju",
                                    (intmax_t)child,
                                    (uintmax_t)WTERMSIG(status)
                                );
                                if (WTERMSIG(status) == 9u)
                                    goto StopMusic;
                            } else
                                syslog(
                                    LOG_ERR,
                                    "The music-playing child process (w/ PID %jd) didn't terminate normally",
                                    (intmax_t)child
                                );
                            sleep(15u);
                    }
                StopMusic:;
        }

        ClearRes:
        PQclear(res);
    }
    return EXIT_FAILURE;
}

void eputs(char const * restrict str) {
    fputs(str, stderr);
    exit(EXIT_FAILURE);
}

void eprintf(char const * restrict format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    exit(EXIT_FAILURE);
}

void efail(char const * restrict subject) {
    eprintf("Failed at %s: %s\n", subject, strerror(errno));
}

/*void closeConn(int) {
    if (pgconn != NULL)
        PQfinish(pgconn);
    _Exit(EXIT_SUCCESS);
}*/
