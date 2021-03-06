/*
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 *
 * openGauss is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 * -------------------------------------------------------------------------
 *
 * fetchmot.cpp
 *    Receives and writes the current MOT checkpoint.
 *
 * IDENTIFICATION
 *    src/bin/pg_ctl/fetchmot.cpp
 *
 * -------------------------------------------------------------------------
 */

#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include "postgres_fe.h"
#include "libpq/libpq-fe.h"
#include "utils/builtins.h"
#include "common/fe_memutils.h"

#define disconnect_and_exit(code) \
    {                             \
        if (conn != NULL)         \
            PQfinish(conn);       \
        exit(code);               \
    }

static uint64 totaldone = 0;

/*
 * Receive a tar format stream from the connection to the server, and unpack
 * the contents of it into a directory. Only files, directories and
 * symlinks are supported, no other kinds of special files.
 */
static void MotReceiveAndUnpackTarFile(
    const char* basedir, const char* chkptName, PGconn* conn, PGresult* res, int rownum, const char* progname)
{
    char current_path[MAXPGPATH];
    char filename[MAXPGPATH];
    int current_len_left;
    int current_padding = 0;
    char* copybuf = NULL;
    FILE* file = NULL;

    errno_t errorno = strncpy_s(current_path, sizeof(current_path), basedir, sizeof(current_path) - 1);
    securec_check_c(errorno, "", "");

    /*
     * Get the COPY data
     */
    res = PQgetResult(conn);
    if (PQresultStatus(res) != PGRES_COPY_OUT) {
        fprintf(stderr, "%s: could not get COPY data stream: %s", progname, PQerrorMessage(conn));
        disconnect_and_exit(1);
    }

    while (1) {
        int r;

        if (copybuf != NULL) {
            PQfreemem(copybuf);
            copybuf = NULL;
        }

        r = PQgetCopyData(conn, &copybuf, 0);
        if (r == -1) {
            /*
             * End of chunk
             */
            if (file != NULL) {
                fclose(file);
                file = NULL;
            }

            break;
        } else if (r == -2) {
            fprintf(stderr, "%s: could not read COPY data: %s", progname, PQerrorMessage(conn));
            disconnect_and_exit(1);
        }

        if (file == NULL) {
            /* new file */
            int filemode;

            /*
             * No current file, so this must be the header for a new file
             */
            if (r != 2560) {
                fprintf(stderr, "%s: invalid tar block header size: %d\n", progname, r);
                disconnect_and_exit(1);
            }
            totaldone += 2560;

            if (sscanf_s(copybuf + 1048, "%201o", &current_len_left) != 1) {
                fprintf(stderr, "%s: could not parse file size\n", progname);
                disconnect_and_exit(1);
            }

            /* Set permissions on the file */
            if (sscanf_s(&copybuf[1024], "%07o ", (unsigned int*)&filemode) != 1) {
                fprintf(stderr, "%s: could not parse file mode\n", progname);
                disconnect_and_exit(1);
            }

            /*
             * All files are padded up to 512 bytes
             */
            current_padding = ((current_len_left + 511) & ~511) - current_len_left;

            /*
             * First part of header is zero terminated filename.
             * when getting a checkpoint, file name can be either
             * the control file (written to base_dir), or a checkpoint
             * file (written to base_dir/chkpt_)
             */
            if (strstr(copybuf, "mot.ctrl")) {
                errorno =
                    snprintf_s(filename, sizeof(filename), sizeof(filename) - 1, "%s/%s", current_path, "mot.ctrl");
                securec_check_ss_c(errorno, "", "");
            } else {
                char* chkptOffset = strstr(copybuf, chkptName);
                if (chkptOffset) {
                    errorno = snprintf_s(
                        filename, sizeof(filename), sizeof(filename) - 1, "%s/%s", current_path, chkptOffset);
                    securec_check_ss_c(errorno, "", "");
                } else {
                    errorno =
                        snprintf_s(filename, sizeof(filename), sizeof(filename) - 1, "%s/%s", current_path, copybuf);
                    securec_check_ss_c(errorno, "", "");
                }
            }

            if (filename[strlen(filename) - 1] == '/') {
                /*
                 * Ends in a slash means directory or symlink to directory
                 */
                if (copybuf[1080] == '5') {
                    /*
                     * Directory
                     */
                    filename[strlen(filename) - 1] = '\0'; /* Remove trailing slash */
                    if (mkdir(filename, S_IRWXU) != 0) {
                        fprintf(stderr,
                            "%s: could not create directory \"%s\": %s\n",
                            progname,
                            filename,
                            strerror(errno));
                        disconnect_and_exit(1);
                    }
#ifndef WIN32
                    if (chmod(filename, (mode_t)filemode))
                        fprintf(stderr,
                            "%s: could not set permissions on directory \"%s\": %s\n",
                            progname,
                            filename,
                            strerror(errno));
#endif
                } else if (copybuf[1080] == '2') {
                    /*
                     * Symbolic link
                     */
                    filename[strlen(filename) - 1] = '\0'; /* Remove trailing slash */
                    if (symlink(&copybuf[1081], filename) != 0) {
                        fprintf(stderr,
                            "%s: could not create symbolic link from \"%s\" to \"%s\": %s\n",
                            progname,
                            filename,
                            &copybuf[1081],
                            strerror(errno));
                        disconnect_and_exit(1);
                    }
                } else {
                    fprintf(stderr, "%s: unrecognized link indicator \"%c\"\n", progname, copybuf[1080]);
                    disconnect_and_exit(1);
                }
                continue; /* directory or link handled */
            }

            canonicalize_path(filename);
            /*
             * regular file
             */
            file = fopen(filename, "wb");
            if (file == NULL) {
                fprintf(stderr, "%s: could not create file \"%s\": %s\n", progname, filename, strerror(errno));
                disconnect_and_exit(1);
            }

#ifndef WIN32
            if (chmod(filename, (mode_t)filemode))
                fprintf(
                    stderr, "%s: could not set permissions on file \"%s\": %s\n", progname, filename, strerror(errno));
#endif

            if (current_len_left == 0) {
                /*
                 * Done with this file, next one will be a new tar header
                 */
                fclose(file);
                file = NULL;
                continue;
            }
        } else {
            /*
             * Continuing blocks in existing file
             */
            if (current_len_left == 0 && r == current_padding) {
                /*
                 * Received the padding block for this file, ignore it and
                 * close the file, then move on to the next tar header.
                 */
                fclose(file);
                file = NULL;
                totaldone += r;
                continue;
            }

            if (fwrite(copybuf, r, 1, file) != 1) {
                fprintf(stderr, "%s: could not write to file \"%s\": %s\n", progname, filename, strerror(errno));
                fclose(file);
                file = NULL;
                disconnect_and_exit(1);
            }
            totaldone += r;

            current_len_left -= r;
            if (current_len_left == 0 && current_padding == 0) {
                /*
                 * Received the last block, and there is no padding to be
                 * expected. Close the file and move on to the next tar
                 * header.
                 */
                fclose(file);
                file = NULL;
                continue;
            }
        } /* continuing data in existing file */
    }     /* loop over all data blocks */

    if (file != NULL) {
        fprintf(stderr, "%s: COPY stream ended before last file was finished\n", progname);
        fclose(file);
        file = NULL;
        disconnect_and_exit(1);
    }

    if (copybuf != NULL) {
        PQfreemem(copybuf);
        copybuf = NULL;
    }
}

/**
 * @brief Receives and writes the current MOT checkpoint.
 * @param the directory in which to save the files in.
 * @param the connection to use in order to fetch.
 * @param the caller program name (for logging).
 * @param controls verbose output
 * @return Boolean value denoting success or failure.
 */
void FetchMotCheckpoint(const char* basedir, PGconn* fetchConn, const char* progname, bool verbose)
{
    PGresult* res = NULL;
    const char* fetchQuery = "FETCH_MOT_CHECKPOINT";
    const char* chkptPrefix = "chkpt_";
    char dirName[MAXPGPATH] = {0};
    char chkptName[MAXPGPATH] = {0};
    errno_t errorno = EOK;
    struct stat fileStat;

    if (fetchConn == NULL) {
        fprintf(stderr, "%s: FetchMotCheckpoint: bad connection\n", progname);
        exit(1);
    }

    if (PQsendQuery(fetchConn, fetchQuery) == 0) {
        fprintf(stderr,
            "%s: could not send fetch mot checkpoint cmd \"%s\": %s",
            progname,
            fetchQuery,
            PQerrorMessage(fetchConn));
        exit(1);
    }

    res = PQgetResult(fetchConn);
    ExecStatusType curStatus = PQresultStatus(res);
    if (curStatus != PGRES_TUPLES_OK) {
        if (curStatus == PGRES_COMMAND_OK) {
            if (verbose) {
                fprintf(stderr, "%s: no mot checkpoint exists\n", progname);
            }
            return;
        } else {
            fprintf(stderr, "%s: could not fetch mot checkpoint info: %s", progname, PQerrorMessage(fetchConn));
            exit(1);
        }
    }

    if (PQntuples(res) == 1) {
        char* chkptLine = PQgetvalue(res, 0, 0);
        if (NULL == chkptLine) {
            fprintf(stderr, "%s: cmd failed\n", progname);
            exit(1);
        }

        char* prefixStart = strstr(chkptLine, chkptPrefix);
        if (prefixStart == NULL) {
            fprintf(stderr, "%s: unable to parse checkpoint location\n", progname);
            exit(1);
        }

        errorno = strncpy_s(chkptName, sizeof(chkptName), prefixStart, sizeof(chkptName) - 1);
        securec_check_c(errorno, "", "");
        errorno = snprintf_s(dirName, sizeof(dirName), sizeof(dirName) - 1, "%s/%s", basedir, chkptName);
        securec_check_ss_c(errorno, "", "");

        if (verbose) {
            fprintf(stderr, "%s: mot checkpoint directory: %s\n", progname, dirName);
        }

        if (stat(dirName, &fileStat) < 0) {
            if (pg_mkdir_p(dirName, S_IRWXU) == -1) {
                fprintf(stderr, "%s: could not create directory \"%s\": %s\n", progname, dirName, strerror(errno));
                exit(1);
            }
        } else {
            fprintf(stderr, "%s: directory \"%s\" already exists, please remove it and try again\n", progname, dirName);
            exit(1);
        }

        MotReceiveAndUnpackTarFile(basedir, chkptName, fetchConn, res, 1, progname);
        if (verbose) {
            fprintf(stderr, "%s: finished fetching mot checkpoint\n", progname);
        }
    } else {
        if (verbose) {
            fprintf(stderr, "%s: mot checkpoint does not exist\n", progname);
        }
    }

    PQclear(res);
}

static void TrimValue(char* value)
{
    if (value == NULL) {
        return;
    }

    const char* tmp = value;
    do {
        while (*tmp == ' ' || *tmp == '\'' || *tmp == '\"' || *tmp == '=' || *tmp == '\n' || *tmp == '\t') {
            ++tmp;
        }
    } while ((*value++ = *tmp++));
}

/**
 * @brief Parses a certain value for an option from a file.
 * @param the file to parse from.
 * @param option to parse.
 * @return the option's value as a malloc'd buffer or NULL if
 * it was not found.
 */
char* GetOptionValueFromFile(const char* fileName, const char* option)
{
    FILE* file = NULL;
    char* line = NULL;
    char* ret = NULL;
    size_t len = 0;
    ssize_t read = 0;

    if (fileName == NULL || option == NULL) {
        return ret;
    }

    if ((file = fopen(fileName, "r")) == NULL) {
        fprintf(stderr, "could not open file \"%s\": %s\n", fileName, strerror(errno));
        return ret;
    }

    while ((read = getline(&line, &len, file)) != -1) {
        if (strncmp(line, option, strlen(option)) == 0) {
            char* offset = line + strlen(option);
            TrimValue(offset);
            ret = strdup(offset);
        }
    }

    fclose(file);
    return ret;
}
