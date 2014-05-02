/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
  Copyright (C) 2011       Sebastian Pipping <sebastian@pipping.org>
  Copyright (C) 2013       Andy Sayler <www.andysayler.com>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.

*/

#define FUSE_USE_VERSION 30

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE

#include <fuse.h>
#include <ulockmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/xattr.h>
#include <sys/file.h>
#include <stdint.h>
#include <inttypes.h>

#include "aes-crypt.h"
#include "libcustos/custos_client.h"

typedef struct fuse_args fuse_args_t;
typedef struct fuse_bufvec fuse_bufvec_t;
typedef struct fuse_conn_info fuse_conn_info_t;
typedef struct fuse_file_info fuse_file_info_t;

typedef struct flock flock_t;
typedef struct stat stat_t;
typedef struct statvfs statvfs_t;
typedef struct timespec timespec_t;

#define DEBUG

#define TESTKEY "MySuperSecretKey"

#define RETURN_FAILURE -1
#define RETURN_SUCCESS 0

#define FHS_DIRTY 1
#define FHS_CLEAN 0
#define PATHBUFSIZE 1024
#define PATHDELIMINATOR '/'
#define NULLTERM '\0'
#define TEMPNAME_PRE  "._"
#define TEMPNAME_POST ".decrypt"
#define KEYBUFSIZE 1024

typedef struct enc_fhs {
    uint64_t encFH;
    uint64_t clearFH;
    char     clearPath[PATHBUFSIZE];
    char     dirty;
    char     padding[7];
} enc_fhs_t;

static inline enc_fhs_t* get_fhs(uint64_t fh) {
    return (enc_fhs_t*) fh;
}

static inline uint64_t put_fhs(enc_fhs_t* fhs) {
    return (uint64_t) fhs;
}

typedef struct enc_dirp {
    DIR *dp;
    struct dirent *entry;
    off_t offset;
} enc_dirp_t;

static inline enc_dirp_t* get_dirp(fuse_file_info_t* fi) {
    return (enc_dirp_t*) (uintptr_t) fi->fh;
}

typedef struct fsState {
    char* basePath;
} fsState_t;

#define GOOD_PSK "It's A Trap!"
#define UUID "1b4e28ba-2fa1-11d2-883f-b9a761bde3fb"
#define SERVER_URL "http://custos:5000"

static int getCustosKey(char* buf, size_t bufSize) {

    uuid_t uuid;
    custosReq_t*     req     = NULL;
    custosKey_t*     key     = NULL;
    custosKeyReq_t*  keyreq  = NULL;
    custosAttr_t*    attr    = NULL;
    custosAttrReq_t* attrreq = NULL;
    custosRes_t*     res     = NULL;

    /* Setup a new request */
    req = custos_createReq(SERVER_URL);
    if(!req) {
        fprintf(stderr, "ERROR getCustosKey: custos_createKeyReq() failed\n");
        return RETURN_FAILURE;
    }

    /* Add Key to Request */
    if(uuid_parse(UUID, uuid) < 0) {
        fprintf(stderr, "ERROR getCustosKey: uuid_parse() failed\n");
        return RETURN_FAILURE;
    }
    key = custos_createKey(uuid, 1, 0, NULL);
    if(!key) {
        fprintf(stderr, "ERROR getCustosKey: custos_createKey() failed\n");
        return RETURN_FAILURE;
    }
    keyreq = custos_createKeyReq(true);
    if(!keyreq) {
        fprintf(stderr, "ERROR getCustosKey: custos_createKeyReq() failed\n");
        return RETURN_FAILURE;
    }
    if(custos_updateKeyReqAddKey(keyreq, key) < 0) {
        fprintf(stderr, "ERROR getCustosKey: custos_updateKeyReqAddKey() failed\n");
        return RETURN_FAILURE;
    }
    if(custos_updateReqAddKeyReq(req, keyreq) < 0) {
        fprintf(stderr, "ERROR getCustosKey: custos_updateReqAddKeyReq() failed\n");
        return RETURN_FAILURE;
    }

    /* Add attr to request */
    attr = custos_createAttr(CUS_ATTRCLASS_EXPLICIT, CUS_ATTRTYPE_EXP_PSK, 0,
                             (strlen(GOOD_PSK) + 1), (uint8_t*) GOOD_PSK);
    if(!attr) {
        fprintf(stderr, "ERROR getCustosKey: custos_createAttr() failed\n");
        return RETURN_FAILURE;
    }
    attrreq = custos_createAttrReq(true);
    if(!attrreq) {
        fprintf(stderr, "ERROR getCustosKey: custos_createAttrReq() failed\n");
        return RETURN_FAILURE;
    }
    if(custos_updateAttrReqAddAttr(attrreq, attr) < 0) {
        fprintf(stderr, "ERROR getCustosKey: custos_updateAttrReqAddAttr() failed\n");
        return RETURN_FAILURE;
    }
    if(custos_updateReqAddAttrReq(req, attrreq) < 0) {
        fprintf(stderr, "ERROR getCustosKey: custos_updateReqAddAttrReq() failed\n");
        return RETURN_FAILURE;
    }

    /* Get Response */
    res = custos_getRes(req);
    if(!res) {
    	fprintf(stderr, "ERROR getCustosKey: custos_getRes() failed\n");
    	return RETURN_FAILURE;
    }

    /* Extract Key */
    if(res->status != CUS_RESSTAT_ACCEPTED) {
    	fprintf(stderr, "ERROR getCustosKey: Bad response status %d\n", res->status);
    	return RETURN_FAILURE;
    }
    if(res->num_keys != 1) {
    	fprintf(stderr, "ERROR getCustosKey: Bad number of keys: %zd\n", res->num_keys);
    	return RETURN_FAILURE;
    }
    if(!res->keys[0]) {
    	fprintf(stderr, "ERROR getCustosKey: Key response struct must not be NULL\n");
    	return RETURN_FAILURE;
    }
    if(res->keys[0]->status != CUS_KEYSTAT_ACCEPTED) {
    	fprintf(stderr, "ERROR getCustosKey: Bad key response status: %d\n", res->keys[0]->status);
    	return RETURN_FAILURE;
    }
    if(!res->keys[0]->key) {
    	fprintf(stderr, "ERROR getCustosKey: Key struct must not be NULL\n");
    	return RETURN_FAILURE;
    }
    if(!res->keys[0]->key->val) {
    	fprintf(stderr, "ERROR getCustosKey: Key value must not be NULL\n");
    	return RETURN_FAILURE;
    }
    if(res->keys[0]->key->size >= bufSize) {
    	fprintf(stderr, "ERROR getCustosKey: keySize %zd larger than bufSize %zd\n",
                res->keys[0]->key->size, bufSize);
    	return RETURN_FAILURE;
    }
    strncpy(buf, (char*) res->keys[0]->key->val, res->keys[0]->key->size);
    buf[res->keys[0]->key->size] = '\0';

    /* Free Response */
    if(custos_destroyRes(&res) < 0) {
        fprintf(stderr, "ERROR getCustosKey: custos_destroyRes() failed\n");
        return RETURN_FAILURE;
    }

    /* Free Request */
    if(custos_destroyReq(&req) < 0) {
        fprintf(stderr, "ERROR getCustosKey: custos_destroyReq() failed\n");
        return RETURN_FAILURE;
    }

    return RETURN_SUCCESS;

}

static int buildPath(const char* path, char* buf, size_t bufSize) {

    size_t size = 0;
    fsState_t* state = NULL;

    fprintf(stderr, "DEBUG buildPath called\n");

    /* Input Checks */
    if(path == NULL) {
        fprintf(stderr, "ERROR buildPath: path must not be NULL\n");
        return -EINVAL;
    }
    if(buf == NULL) {
        fprintf(stderr, "ERROR buildPath: buf must not be NULL\n");
        return -EINVAL;
    }

    fprintf(stderr, "INFO buildPath: path = %s\n", path);

    /* Get State */
    state = (fsState_t*)(fuse_get_context()->private_data);
    if(state == NULL) {
        fprintf(stderr, "ERROR buildPath: state must not be NULL\n");
        return -EINVAL;
    }

    /* Concatenate in Buffer */
    size = snprintf(buf, bufSize, "%s%s", state->basePath, path);
    if(size > (bufSize - 1)) {
        fprintf(stderr, "ERROR buildPath: length too large for buffer\n");
        return -ENAMETOOLONG;
    }

    fprintf(stderr, "INFO buildPath: buf = %s\n", buf);

    return RETURN_SUCCESS;

}

static int buildTempPath(const char* fullPath, char* tempPath, size_t bufSize) {

    char* pFileName = NULL;
    char buf[PATHBUFSIZE];
    size_t length;

    fprintf(stderr, "DEBUG buildTempPath called\n");

    /* Input Checks */
    if(fullPath == NULL) {
        fprintf(stderr, "ERROR buildTempPath: fullPath must not be NULL\n");
        return -EINVAL;
    }
    if(tempPath == NULL) {
        fprintf(stderr, "ERROR buildTempPath: tempPath must not be NULL\n");
        return -EINVAL;
    }

    fprintf(stderr, "INFO buildTempPath: fullPath = %s\n", fullPath);

    /* Copy input path to buf */
    length = snprintf(buf, sizeof(buf), "%s", fullPath);
    if(length > (sizeof(buf) - 1)) {
        fprintf(stderr, "ERROR buildTempPath: Overflowed buf\n");
        return -ENAMETOOLONG;
    }

    /* Find start of file name */
    pFileName = strrchr(buf, PATHDELIMINATOR);
    if(pFileName == NULL) {
        fprintf(stderr, "ERROR buildTempPath: Could not find deliminator in path\n");
        return -EINVAL;
    }
    *pFileName = NULLTERM;

    /* Build Temp Path */
    length = snprintf(tempPath, bufSize, "%s%c%s%s%s",
                      buf, PATHDELIMINATOR, TEMPNAME_PRE, (pFileName + 1), TEMPNAME_POST);
    if(length > (bufSize - 1)) {
        fprintf(stderr, "ERROR buildTempPath: Overflowed tempPath\n");
        return -ENAMETOOLONG;
    }

    fprintf(stderr, "INFO buildTempPath: tempPath = %s\n", tempPath);

    return RETURN_SUCCESS;

}

static enc_fhs_t* createFilePair(const char* encPath, const char* clearPath,
                                 int flags, mode_t mode) {

    int ret;
    enc_fhs_t* fhs = NULL;

    fprintf(stderr, "DEBUG createFilePair called\n");

    fhs = malloc(sizeof(*fhs));
    if(!fhs) {
        fprintf(stderr, "ERROR createFilePair: malloc failed\n");
        perror("ERROR createFilePair");
        return NULL;
    }

    ret = open(encPath, flags, mode);
    if(ret < 0) {
        fprintf(stderr, "ERROR createFilePair: open(encPath) failed\n");
        perror("ERROR createFilePair");
        return NULL;
    }
    fhs->encFH = ret;

    ret = open(clearPath, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if(ret < 0) {
        fprintf(stderr, "ERROR createFilePair: open(clearPath) failed\n");
        perror("ERROR createFilePair");
        return NULL;
    }
    fhs->clearFH = ret;
    strncpy(fhs->clearPath, clearPath, PATHBUFSIZE);

    return fhs;

}

static enc_fhs_t* openFilePair(const char* encPath, const char* clearPath,
                               int flags) {

    int ret;
    int newflags;
    enc_fhs_t* fhs = NULL;

    fprintf(stderr, "DEBUG openFilePair called\n");

    if((flags & O_WRONLY) == O_WRONLY) {
        newflags = (flags & ~O_WRONLY) | O_RDWR;
        fprintf(stderr, "INFO openFilePair: upgrading O_WRONLY to O_RDWR: %X to %X\n",
                flags, newflags);
    }
    else {
        newflags = flags;
    }

    fhs = malloc(sizeof(*fhs));
    if(!fhs) {
        fprintf(stderr, "ERROR openFilePair: malloc failed\n");
        perror("ERROR openFilePair");
        return NULL;
    }

    ret = open(encPath, newflags);
    if(ret < 0) {
        fprintf(stderr, "ERROR openFilePair: open(encPath) failed\n");
        perror("ERROR openFilePair");
        return NULL;
    }
    fhs->encFH = ret;

    ret = open(clearPath, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if(ret < 0) {
        fprintf(stderr, "ERROR openFilePair: open(clearPath) failed\n");
        perror("ERROR openFilePair");
        return NULL;
    }
    fhs->clearFH = ret;
    strncpy(fhs->clearPath, clearPath, PATHBUFSIZE);

    return fhs;

}

static int closeFilePair(enc_fhs_t* fhs) {

    fprintf(stderr, "DEBUG closeFilePair called\n");

    if(!fhs) {
        fprintf(stderr, "ERROR closeFilePair: fhs must not be NULL\n");
        return -EINVAL;
    }

    if(close(fhs->encFH) < 0) {
        fprintf(stderr, "ERROR closeFilePair: close(encFH) failed\n");
        perror("ERROR enc_release");
        return -errno;
    }

    if(close(fhs->clearFH) < 0) {
        fprintf(stderr, "ERROR closeFilePair: close(clearFH) failed\n");
        perror("ERROR enc_release");
        return -errno;
    }

    free(fhs);

    return RETURN_SUCCESS;

}

static int removeFile(const char* filePath) {

    int ret;

    fprintf(stderr, "DEBUG removeFile called\n");

    fprintf(stderr, "INFO removeFile: function called on %s\n", filePath);

    ret = unlink(filePath);
    if(ret < 0) {
        fprintf(stderr, "ERROR removeFile: unlink failed\n");
        perror("ERROR removeTemp");
        return -errno;
    }

    return RETURN_SUCCESS;

}

static int decryptFH(const uint64_t encFH, const uint64_t clearFH) {

    int ret = RETURN_SUCCESS;
    int encFD;
    int clearFD;
    off_t encOffset;
    off_t clearOffset;
    FILE* encFP = NULL;
    FILE* clearFP = NULL;
    /* char key[KEYBUFSIZE]; */
    char* key = NULL;

    fprintf(stderr, "DEBUG decryptFH called\n");

    /* Get Custos Key */
    /* ret = getCustosKey(key, sizeof(key)); */
    /* if(ret  < 0) { */
    /*     fprintf(stderr, "ERROR decryptFH: getCustosKey failed\n"); */
    /*     goto CLEANUP_0; */
    /* } */
    key = TESTKEY;

    /* Save and Rewind Input Offset */
    encOffset = lseek(encFH, 0, SEEK_CUR);
    if(encOffset < 0) {
        perror("ERROR decryptFH");
        ret = -errno;
        fprintf(stderr,
		"ERROR decryptFH: Save enc lseek(%"PRIu64") failed with error %d\n",
                encFH, -ret);
        goto CLEANUP_0;
    }
    ret = lseek(encFH, 0, SEEK_SET);
    if(ret < 0) {
        perror("ERROR decryptFH");
        ret = -errno;
        fprintf(stderr,
		"ERROR decryptFH: Rewind enc lseek(%"PRIu64") failed with error %d\n",
                encFH, -ret);
        goto CLEANUP_0;
    }

    /* Save, Rewind, and Truncate Output */
    clearOffset = lseek(clearFH, 0, SEEK_CUR);
    if(clearOffset < 0) {
        perror("ERROR decryptFH");
        ret = -errno;
        fprintf(stderr,
		"ERROR decryptFH: Save clr lseek(%"PRIu64") failed with error %d\n",
                clearFH, -ret);
        goto CLEANUP_1;
    }
    ret = lseek(clearFH, 0, SEEK_SET);
    if(ret < 0) {
        perror("ERROR decryptFH");
        ret = -errno;
        fprintf(stderr,
		"ERROR decryptFH: Rewind clr lseek(%"PRIu64") failed with error %d\n",
                clearFH, -ret);
        goto CLEANUP_1;
    }
    ret = ftruncate(clearFH, 0);
    if(ret < 0) {
        perror("ERROR decryptFH");
        ret = -errno;
        fprintf(stderr,
		"ERROR decryptFH: clr ftruncate(%"PRIu64") failed with error %d\n",
                clearFH, -ret);
        goto CLEANUP_2;
    }

    /* Dup and get FILE* for encFH */
    encFD = dup(encFH);
    if(ret < 0) {
        perror("ERROR decryptFH");
        ret = -errno;
        fprintf(stderr,
		"ERROR decryptFH: encFH dup(%"PRIu64") failed with error %d\n",
                encFH, -ret);
        goto CLEANUP_3;
    }
    encFP = fdopen(encFD, "r");
    if(!encFP) {
        perror("ERROR decryptFH");
        ret = -errno;
        fprintf(stderr,
		"ERROR decryptFH: encFH fdopen(%d) failed with error %d\n",
                encFD, -ret);
        goto CLEANUP_4;
    }

    /* Dup and get FILE* for clearFH */
    clearFD = dup(clearFH);
    if(ret < 0) {
        perror("ERROR decryptFH");
        ret = -errno;
        fprintf(stderr,
		"ERROR decryptFH: clearFH dup(%"PRIu64") failed with error %d\n",
                clearFH, -ret);
        goto CLEANUP_5;
    }
    clearFP = fdopen(clearFD, "w");
    if(!clearFP) {
        perror("ERROR decryptFH");
        ret = -errno;
        fprintf(stderr,
		"ERROR decryptFH: clearFH fdopen(%d) failed with error %d\n",
                clearFD, -ret);
        goto CLEANUP_6;
    }

    /* Decrypt */
    ret = crypt_decrypt(encFP, clearFP, key);
    //ret = crypt_copy(encFP, clearFP);
    if(ret < 0) {
        fprintf(stderr,
		"ERROR decryptFH: crypt_decrypt() failed\n");
        goto CLEANUP_7;
    }

 CLEANUP_7:
    /* Cleanup clearFP */
    if(fclose(clearFP)) {
        perror("ERROR decryptFH");
        ret = -errno;
        fprintf(stderr,
                "ERROR decryptFH: fclose(clearFP) failed with error %d\n",
                -ret);
    }
    goto CLEANUP_5;

 CLEANUP_6:
    /* Cleanup Duped clearFD */
    if(close(clearFD)) {
        perror("ERROR decryptFH");
        ret = -errno;
        fprintf(stderr,
                "ERROR decryptFH: close(clearFD) failed with error %d\n",
                -ret);
    }

 CLEANUP_5:
    /* Cleanup encFP */
    if(fclose(encFP)) {
        perror("ERROR decryptFH");
        ret = -errno;
        fprintf(stderr,
                "ERROR decryptFH: fclose(encFP) failed with error %d\n",
                -ret);
    }
    goto CLEANUP_3;

 CLEANUP_4:
    /* Cleanup Duped encFD */
    if(close(encFD)) {
        perror("ERROR decryptFH");
        ret = -errno;
        fprintf(stderr,
                "ERROR decryptFH: close(encFD) failed with error %d\n",
                -ret);
    }

 CLEANUP_3:
    /* No Cleanup */

 CLEANUP_2:
    /* Restore Output Offset*/
    ret = lseek(clearFH, clearOffset, SEEK_SET);
    if(ret < 0) {
        perror("ERROR decryptFH");
        ret = -errno;
        fprintf(stderr,
                "ERROR decryptFH: Restore clr lseek(%"PRIu64") failed, error %d\n",
                encFH, -ret);
    }

 CLEANUP_1:
    /* Restore Input Offset*/
    ret = lseek(encFH, encOffset, SEEK_SET);
    if(ret < 0) {
        perror("ERROR decryptFH");
        ret = -errno;
        fprintf(stderr,
                "ERROR decryptFH: Restore enc lseek(%"PRIu64") failed, error %d\n",
                encFH, -ret);
    }

 CLEANUP_0:
    /* Return */
    return ret;

}

static int encryptFH(const uint64_t clearFH, const uint64_t encFH) {

    int ret = RETURN_SUCCESS;
    int clearFD;
    int encFD;
    off_t clearOffset;
    off_t encOffset;
    FILE* clearFP = NULL;
    FILE* encFP = NULL;
    /* char key[KEYBUFSIZE]; */
    char* key = NULL;

    fprintf(stderr, "DEBUG encryptFH called\n");

    /* Get Custos Key */
    /* ret = getCustosKey(key, sizeof(key)); */
    /* if(ret  < 0) { */
    /*     fprintf(stderr, "ERROR decryptFH: getCustosKey failed\n"); */
    /*     goto CLEANUP_0; */
    /* } */
    key = TESTKEY;

    /* Save and Rewind Input Offset */
    clearOffset = lseek(clearFH, 0, SEEK_CUR);
    if(clearOffset < 0) {
        perror("ERROR encryptFH");
        ret = -errno;
        fprintf(stderr,
		"ERROR encryptFH: Save clr lseek(%"PRIu64") failed with error %d\n",
                clearFH, -ret);
        goto CLEANUP_0;
    }
    ret = lseek(clearFH, 0, SEEK_SET);
    if(ret < 0) {
        perror("ERROR encryptFH");
        ret = -errno;
        fprintf(stderr,
		"ERROR encryptFH: Rewind clr lseek(%"PRIu64") failed with error %d\n",
                clearFH, -ret);
        goto CLEANUP_0;
    }

    /* Save, Rewind, and Truncate Output */
    encOffset = lseek(encFH, 0, SEEK_CUR);
    if(encOffset < 0) {
        perror("ERROR encryptFH");
        ret = -errno;
        fprintf(stderr,
		"ERROR encryptFH: Save enc lseek(%"PRIu64") failed with error %d\n",
                encFH, -ret);
        goto CLEANUP_1;
    }
    ret = lseek(encFH, 0, SEEK_SET);
    if(ret < 0) {
        perror("ERROR encryptFH");
        ret = -errno;
        fprintf(stderr,
		"ERROR encryptFH: Rewind enc lseek(%"PRIu64") failed with error %d\n",
                encFH, -ret);
        goto CLEANUP_1;
    }
    ret = ftruncate(encFH, 0);
    if(ret < 0) {
        perror("ERROR encryptFH");
        ret = -errno;
        fprintf(stderr,
		"ERROR encryptFH: enc ftruncate(%"PRIu64") failed with error %d\n",
                encFH, -ret);
        goto CLEANUP_2;
    }

    /* Dup and get FILE* for clearFH */
    clearFD = dup(clearFH);
    if(ret < 0) {
        perror("ERROR encryptFH");
        ret = -errno;
        fprintf(stderr,
		"ERROR encryptFH: clearFH dup(%"PRIu64") failed with error %d\n",
                clearFH, -ret);
        goto CLEANUP_3;
    }
    clearFP = fdopen(clearFD, "r");
    if(!clearFP) {
        perror("ERROR encryptFH");
        ret = -errno;
        fprintf(stderr,
		"ERROR encryptFH: clearFH fdopen(%d) failed with error %d\n",
                clearFD, -ret);
        goto CLEANUP_4;
    }

    /* Dup and get FILE* for encFH */
    encFD = dup(encFH);
    if(ret < 0) {
        perror("ERROR encryptFH");
        ret = -errno;
        fprintf(stderr,
		"ERROR encryptFH: encFH dup(%"PRIu64") failed with error %d\n",
                encFH, -ret);
        goto CLEANUP_5;
    }
    encFP = fdopen(encFD, "w");
    if(!encFP) {
        perror("ERROR encryptFH");
        ret = -errno;
        fprintf(stderr,
		"ERROR encryptFH: encFH fdopen(%d) failed with error %d\n",
                encFD, -ret);
        goto CLEANUP_6;
    }

    /* Encrypt */
    ret = crypt_encrypt(clearFP, encFP, key);
    //ret = crypt_copy(clearFP, encFP);
    if(ret < 0) {
        fprintf(stderr,
		"ERROR encryptFH: crypt_encrypt() failed\n");
        goto CLEANUP_7;
    }

 CLEANUP_7:
    /* Cleanup encFP */
    if(fclose(encFP)) {
        perror("ERROR decryptFH");
        ret = -errno;
        fprintf(stderr,
                "ERROR decryptFH: fclose(encFP) failed with error %d\n",
                -ret);
    }
    goto CLEANUP_5;

 CLEANUP_6:
    /* Cleanup Duped encFD */
    if(close(encFD)) {
        perror("ERROR decryptFH");
        ret = -errno;
        fprintf(stderr,
                "ERROR decryptFH: close(encFD) failed with error %d\n",
                -ret);
    }

 CLEANUP_5:
    /* Cleanup clearFP */
    if(fclose(clearFP)) {
        perror("ERROR decryptFH");
        ret = -errno;
        fprintf(stderr,
                "ERROR decryptFH: fclose(clearFP) failed with error %d\n",
                -ret);
    }
    goto CLEANUP_3;

 CLEANUP_4:
    /* Cleanup Duped clearFD */
    if(close(clearFD)) {
        perror("ERROR decryptFH");
        ret = -errno;
        fprintf(stderr,
                "ERROR decryptFH: close(clearFD) failed with error %d\n",
                -ret);
    }

 CLEANUP_3:
    /* No Cleanup */

 CLEANUP_2:
    /* Restore Output Offset*/
    ret = lseek(encFH, encOffset, SEEK_SET);
    if(ret < 0) {
        perror("ERROR decryptFH");
        ret = -errno;
        fprintf(stderr,
                "ERROR decryptFH: Restore enc lseek(%"PRIu64") failed, error %d\n",
                encFH, -ret);
    }

 CLEANUP_1:
    /* Restore Input Offset*/
    ret = lseek(clearFH, clearOffset, SEEK_SET);
    if(ret < 0) {
        perror("ERROR decryptFH");
        ret = -errno;
        fprintf(stderr,
                "ERROR decryptFH: Restore clr lseek(%"PRIu64") failed, error %d\n",
                encFH, -ret);
    }

 CLEANUP_0:
    /* Return */
    return ret;

}

static int enc_getattr(const char* path, stat_t* stbuf) {

    int ret;
    char fullPath[PATHBUFSIZE];
    char tempPath[PATHBUFSIZE];
    enc_fhs_t* fhs;
    stat_t stTemp;

    ret = buildPath(path, fullPath, sizeof(fullPath));
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_getattr: buildPath failed\n");
        return ret;
    }
    path = NULL;

    ret = lstat(fullPath, stbuf);
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_getattr: lstat(fullPath) failed\n");
        perror("ERROR enc_getattr");
        return -errno;
    }

    if(S_ISREG(stbuf->st_mode)) {

        ret = buildTempPath(fullPath, tempPath, sizeof(tempPath));
        if(ret < 0) {
            fprintf(stderr, "ERROR enc_getattr: buildTempPath failed\n");
            return ret;
        }

        // TODO:  Make re-entrant/parallel safe - currently overwrites/erases temp

        fhs = openFilePair(fullPath, tempPath, O_RDONLY);
        if(!fhs) {
            fprintf(stderr, "ERROR enc_getattr: openFilePair failed\n");
            return RETURN_FAILURE;
        }

        ret = decryptFH(fhs->encFH, fhs->clearFH);
        if(ret < 0) {
            fprintf(stderr, "ERROR enc_getattr: decryptFH failed\n");
            return ret;
        }

        ret = fstat(fhs->clearFH, &stTemp);
        if(ret < 0) {
            fprintf(stderr, "ERROR enc_getattr: lstat(tempPath) failed\n");
            perror("ERROR enc_getattr");
            return -errno;
        }

        ret = closeFilePair(fhs);
        if(ret < 0) {
            fprintf(stderr, "ERROR enc_getattr: closeFilePair failed\n");
            return ret;
        }

        /* Copy over select fields */
        stbuf->st_size = stTemp.st_size;
        stbuf->st_blksize = stTemp.st_blksize;
        stbuf->st_blocks = stTemp.st_blocks;

        ret = removeFile(tempPath);
        if(ret < 0) {
            fprintf(stderr, "ERROR enc_getattr: removeFile failed\n");
            return ret;
        }
    }

    return RETURN_SUCCESS;

}

static int enc_fgetattr(const char* path, stat_t* stbuf,
                        fuse_file_info_t* fi) {

    (void) path;

    int ret;
    enc_fhs_t* fhs;
    stat_t stTemp;

    fhs = get_fhs(fi->fh);

    ret = fstat(fhs->encFH, stbuf);
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_fgetattr: fstat(encFH) failed");
        perror("ERROR enc_fgetattr");
        return -errno;
    }

    if(S_ISREG(stbuf->st_mode)) {

        ret = fstat(fhs->clearFH, &stTemp);
        if(ret < 0) {
            fprintf(stderr, "ERROR enc_fgetattr: fstat(clearFH) failed");
            perror("ERROR enc_fgetattr");
            return -errno;
        }

        /* Copy over select fields */
        stbuf->st_size = stTemp.st_size;
        stbuf->st_blksize = stTemp.st_blksize;
        stbuf->st_blocks = stTemp.st_blocks;

    }

    return RETURN_SUCCESS;

}

static int enc_access(const char* path, int mask) {

    int ret;
    char fullPath[PATHBUFSIZE];

    ret = buildPath(path, fullPath, sizeof(fullPath));
    if(ret < 0){
        fprintf(stderr, "ERROR enc_access: buildPath failed\n");
        return ret;
    }
    path = NULL;

    ret = access(fullPath, mask);
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_access: access failed\n");
        perror("ERROR enc_access");
        return -errno;
    }

    return RETURN_SUCCESS;

}

static int enc_readlink(const char* path, char* buf, size_t size) {

    int ret;
    char fullPath[PATHBUFSIZE];

    ret = buildPath(path, fullPath, sizeof(fullPath));
    if(ret < 0){
        fprintf(stderr, "ERROR enc_readlink: buildPath failed\n");
        return ret;
    }
    path = NULL;

    /* ToDo: Should this operate on the plain or encrypted file? */

    ret = readlink(fullPath, buf, (size-1));
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_readlink: readlink failed\n");
        perror("ERROR enc_readlink");
        return -errno;
    }

    buf[ret] = '\0';

    return RETURN_SUCCESS;

}

static int enc_opendir(const char* path, fuse_file_info_t* fi) {

    int ret;
    enc_dirp_t* d = NULL;
    char fullPath[PATHBUFSIZE];

    d = malloc(sizeof(*d));
    if(d == NULL) {
        fprintf(stderr, "ERROR enc_opendir: malloc failed\n");
        perror("ERROR enc_opendir");
        return -errno;
    }

    ret = buildPath(path, fullPath, sizeof(fullPath));
    if(ret < 0){
        fprintf(stderr, "ERROR enc_opendir: buildPath failed\n");
        return ret;
    }
    path = NULL;

    d->dp = opendir(fullPath);
    if(d->dp == NULL) {
        fprintf(stderr, "ERROR enc_opendir: opendir failed\n");
        perror("ERROR enc_opendir");
        ret = -errno;
        free(d);
        return ret;
    }
    d->offset = 0;
    d->entry = NULL;

    fi->fh = (unsigned long) d;

    return RETURN_SUCCESS;

}

static int enc_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                       off_t offset, fuse_file_info_t* fi) {

    (void) path;

    enc_dirp_t* d = NULL;

    d = get_dirp(fi);

    if (offset != d->offset) {
        seekdir(d->dp, offset);
        d->entry = NULL;
        d->offset = offset;
    }
    while (1) {
        stat_t st;
        off_t nextoff;

        if (!d->entry) {
            d->entry = readdir(d->dp);
            if (!d->entry)
                break;
        }

        memset(&st, 0, sizeof(st));
        st.st_ino = d->entry->d_ino;
        st.st_mode = d->entry->d_type << 12;
        nextoff = telldir(d->dp);
        if (filler(buf, d->entry->d_name, &st, nextoff))
            break;

        d->entry = NULL;
        d->offset = nextoff;
    }

    return RETURN_SUCCESS;

}

static int enc_releasedir(const char* path, fuse_file_info_t* fi) {

    (void) path;

    enc_dirp_t* d = NULL;

    d = get_dirp(fi);
    closedir(d->dp);
    free(d);

    return RETURN_SUCCESS;

}

static int enc_mknod(const char* path, mode_t mode, dev_t rdev) {

    int ret;
    char fullPath[PATHBUFSIZE];

    ret = buildPath(path, fullPath, sizeof(fullPath));
    if(ret < 0){
        fprintf(stderr, "ERROR enc_mknod: buildPath failed\n");
        return ret;
    }
    path = NULL;

    if(S_ISFIFO(mode)) {
        ret = mkfifo(fullPath, mode);
    }
    else {
        ret = mknod(fullPath, mode, rdev);
    }
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_mknod: mkfifo/mknode failed\n");
        perror("ERROR enc_mknod");
        return -errno;
    }

    return RETURN_SUCCESS;

}

static int enc_mkdir(const char* path, mode_t mode) {

    int ret;
    char fullPath[PATHBUFSIZE];

    ret = buildPath(path, fullPath, sizeof(fullPath));
    if(ret < 0){
        fprintf(stderr, "ERROR enc_mkdir: buildPath failed\n");
        return ret;
    }
    path = NULL;

    ret = mkdir(fullPath, mode);
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_mkdir: mkdir failed\n");
        perror("ERROR enc_mkdir");
        return -errno;
    }

    return RETURN_SUCCESS;

}

static int enc_unlink(const char* path) {

    int ret;
    char fullPath[PATHBUFSIZE];

    ret = buildPath(path, fullPath, sizeof(fullPath));
    if(ret < 0){
        fprintf(stderr, "ERROR enc_unlink: buildPath failed\n");
        return ret;
    }
    path = NULL;

    ret = unlink(fullPath);
    if(ret < 0){
        fprintf(stderr, "ERROR enc_unlink: unlink failed\n");
        perror("ERROR enc_unlink");
        return -errno;
    }

    return RETURN_SUCCESS;

}

static int enc_rmdir(const char* path) {

    int ret;
    char fullPath[PATHBUFSIZE];

    ret = buildPath(path, fullPath, sizeof(fullPath));
    if(ret < 0){
        fprintf(stderr, "ERROR enc_rmdir: buildPath failed\n");
        return ret;
    }
    path = NULL;

    ret = rmdir(fullPath);
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_rmdir: rmdir failed\n");
        perror("ERROR enc_rmdir");
        return -errno;
    }

    return RETURN_SUCCESS;

}

static int enc_symlink(const char* from, const char* to) {

    /* ToDo: Are both from and to in the fuse FS? */

    char fullFrom[PATHBUFSIZE];
    char fullTo[PATHBUFSIZE];

    if(buildPath(from, fullFrom, sizeof(fullFrom)) < 0){
        fprintf(stderr, "ERROR enc_symlink: buildPath failed on from\n");
        return RETURN_FAILURE;
    }
    from = NULL;

    if(buildPath(to, fullTo, sizeof(fullTo)) < 0){
        fprintf(stderr, "ERROR enc_symlink: buildPath failed on to\n");
        return RETURN_FAILURE;
    }
    to = NULL;

    if(symlink(fullFrom, fullTo)) {
        return -errno;
    }

    return RETURN_SUCCESS;

}

static int enc_link(const char* from, const char* to) {

    /* ToDo: Are both from and to in the fuse FS? */

    char fullFrom[PATHBUFSIZE];
    char fullTo[PATHBUFSIZE];

    if(buildPath(from, fullFrom, sizeof(fullFrom)) < 0){
        fprintf(stderr, "ERROR enc_link: buildPath failed on from\n");
        return RETURN_FAILURE;
    }
    from = NULL;

    if(buildPath(to, fullTo, sizeof(fullTo)) < 0){
        fprintf(stderr, "ERROR enc_link: buildPath failed on to\n");
        return RETURN_FAILURE;
    }
    to = NULL;

    if(link(fullFrom, fullTo) < 0) {
        return -errno;
    }

    return RETURN_SUCCESS;

}

static int enc_rename(const char* from, const char* to) {

    int ret;
    char fullFrom[PATHBUFSIZE];
    char fullTo[PATHBUFSIZE];

    ret = buildPath(from, fullFrom, sizeof(fullFrom));
    if(ret < 0){
        fprintf(stderr, "ERROR enc_rename: buildPath(from) failed\n");
        return ret;
    }
    from = NULL;

    ret = buildPath(to, fullTo, sizeof(fullTo));
    if(ret < 0){
        fprintf(stderr, "ERROR enc_rename: buildPath(to) failed\n");
        return ret;
    }
    to = NULL;

    ret = rename(fullFrom, fullTo);
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_rename: rename failed\n");
        perror("ERROR enc_rename");
        return -errno;
    }

    return RETURN_SUCCESS;

}

static int enc_chmod(const char* path, mode_t mode) {

    int ret;
    char fullPath[PATHBUFSIZE];

    ret = buildPath(path, fullPath, sizeof(fullPath));
    if(ret < 0){
        fprintf(stderr, "ERROR enc_chmod: buildPath failed\n");
        return ret;
    }
    path = NULL;

    ret = chmod(fullPath, mode);
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_chmod: chmod failed\n");
        perror("ERROR enc_chmod");
        return -errno;
    }

    return RETURN_SUCCESS;

}

static int enc_chown(const char* path, uid_t uid, gid_t gid) {

    int ret;
    char fullPath[PATHBUFSIZE];

    ret = buildPath(path, fullPath, sizeof(fullPath));
    if(ret < 0){
        fprintf(stderr, "ERROR enc_chown: buildPath failed\n");
        return ret;
    }
    path = NULL;

    ret = lchown(fullPath, uid, gid);
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_chown: lchown failed\n");
        perror("ERROR enc_chown");
        return -errno;
    }

    return RETURN_SUCCESS;

}

static int enc_truncate(const char* path, off_t size) {

    int ret;
    char fullPath[PATHBUFSIZE];
    char tempPath[PATHBUFSIZE];
    enc_fhs_t* fhs;

    ret = buildPath(path, fullPath, sizeof(fullPath));
    if(ret < 0){
        fprintf(stderr, "ERROR enc_truncate: buildPath failed\n");
        return ret;
    }
    path = NULL;

    ret = buildTempPath(fullPath, tempPath, sizeof(tempPath));
    if(ret < 0){
        fprintf(stderr, "ERROR enc_truncate: buildTempPath failed\n");
        return ret;
    }

    fhs = openFilePair(fullPath, tempPath, O_RDWR);
    if(!fhs) {
        fprintf(stderr, "ERROR enc_truncate: openFilePair failed\n");
        return RETURN_FAILURE;
    }

    ret = ftruncate(fhs->clearFH, size);
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_truncate: ftruncate failed\n");
        perror("ERROR enc_truncate");
        return -errno;
    }

    ret = encryptFH(fhs->clearFH, fhs->encFH);
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_turncate: encryptFH failed\n");
        return ret;
    }

    ret = closeFilePair(fhs);
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_getattr: closeFilePair failed\n");
        return ret;
    }

    ret = removeFile(tempPath);
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_truncate: removeFile failed\n");
        return ret;
    }

    return RETURN_SUCCESS;

}

static int enc_ftruncate(const char* path, off_t size,
                         fuse_file_info_t* fi) {

    (void) path;

    int ret;
    enc_fhs_t* fhs;

    fhs = get_fhs(fi->fh);

    ret = ftruncate(fhs->clearFH, size);
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_truncate: ftruncate failed\n");
        perror("ERROR enc_truncate");
        return -errno;
    }

    return RETURN_SUCCESS;

}

static int enc_utimens(const char* path, const timespec_t ts[2]) {

    int ret;
    char fullPath[PATHBUFSIZE];

    ret = buildPath(path, fullPath, sizeof(fullPath));
    if(ret < 0){
        fprintf(stderr, "ERROR enc_utimens: buildPath failed\n");
        return ret;
    }
    path = NULL;

    /* don't use utime/utimes since they follow symlinks */
    ret = utimensat(0, fullPath, ts, AT_SYMLINK_NOFOLLOW);
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_utimens: utimensat failed\n");
        perror("ERROR enc_utimens");
        return -errno;
    }

    return RETURN_SUCCESS;

}

static int enc_create(const char* path, mode_t mode, fuse_file_info_t* fi) {

    int ret;
    enc_fhs_t* fhs;
    char fullPath[PATHBUFSIZE];
    char tempPath[PATHBUFSIZE];

    ret = buildPath(path, fullPath, sizeof(fullPath));
    if(ret < 0){
        fprintf(stderr, "ERROR enc_create: buildPath failed\n");
        return ret;
    }
    path = NULL;

    ret = buildTempPath(fullPath, tempPath, sizeof(tempPath));
    if(ret < 0){
        fprintf(stderr, "ERROR enc_create: buildTempPath failed\n");
        return ret;
    }

    fhs = createFilePair(fullPath, tempPath, fi->flags, mode);
    if(!fhs) {
        fprintf(stderr, "ERROR enc_create: createFilePair failed\n");
        return RETURN_FAILURE;
    }

    ret = encryptFH(fhs->clearFH, fhs->encFH);
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_create: encryptFH failed\n");
        return ret;
    }

    fhs->dirty = FHS_CLEAN;
    fi->fh = put_fhs(fhs);

    return RETURN_SUCCESS;

}

static int enc_open(const char* path, fuse_file_info_t* fi) {

    int ret;
    enc_fhs_t* fhs;
    char fullPath[PATHBUFSIZE];
    char tempPath[PATHBUFSIZE];

    ret = buildPath(path, fullPath, sizeof(fullPath));
    if(ret < 0){
        fprintf(stderr, "ERROR enc_open: buildPath failed\n");
        return ret;
    }
    path = NULL;

    ret = buildTempPath(fullPath, tempPath, sizeof(tempPath));
    if(ret < 0){
        fprintf(stderr, "ERROR enc_open: buildTempPath failed\n");
        return ret;
    }

    fhs = openFilePair(fullPath, tempPath, fi->flags);
    if(!fhs) {
        fprintf(stderr, "ERROR enc_open: openFilePair failed\n");
        return RETURN_FAILURE;
    }

    ret = decryptFH(fhs->encFH, fhs->clearFH);
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_open: decryptFH failed\n");
        return ret;
    }

    fhs->dirty = FHS_CLEAN;
    fi->fh = put_fhs(fhs);

    return RETURN_SUCCESS;

}

static int enc_read(const char* path, char* buf, size_t size, off_t offset,
                    fuse_file_info_t* fi) {

    (void) path;

    int ret;
    enc_fhs_t* fhs;

    fhs = get_fhs(fi->fh);

    ret = pread(fhs->clearFH, buf, size, offset);
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_read: pread failed\n");
        perror("ERROR enc_read");
        ret = -errno;
    }

    return ret;

}

static int enc_write(const char* path, const char* buf, size_t size,
		     off_t offset, fuse_file_info_t* fi) {

    (void) path;

    int ret;
    enc_fhs_t* fhs;

    fhs = get_fhs(fi->fh);
    fhs->dirty = FHS_DIRTY;

    ret = pwrite(fhs->clearFH, buf, size, offset);
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_write: pwrite failed\n");
        perror("ERROR enc_write");
        ret = -errno;
    }

    return ret;

}

static int enc_statfs(const char* path, statvfs_t* stbuf) {

    int ret;
    char fullPath[PATHBUFSIZE];

    ret = buildPath(path, fullPath, sizeof(fullPath));
    if(ret < 0){
        fprintf(stderr, "ERROR enc_statfs: buildPath failed\n");
        return ret;
    }
    path = NULL;

    ret = statvfs(fullPath, stbuf);
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_statfs: statvfs failed\n");
        perror("ERROR enc_statfs");
        return -errno;
    }

    return RETURN_SUCCESS;

}

static int enc_flush(const char* path, fuse_file_info_t* fi) {

    /* This is called from every close on an open file, so call the
       close on the underlying filesystem. But since flush may be
       called multiple times for an open file, this must not really
       close the file.  This is important if used on a network
       filesystem like NFS which flush the data/metadata on close() */

    (void) path;

    int ret;
    enc_fhs_t* fhs;

    if(!fi) {
        fprintf(stderr, "ERROR enc_flush: fi is NULL");
        return -EINVAL;
    }

    fhs = get_fhs(fi->fh);


    if(fhs->dirty == FHS_DIRTY) {

        ret = encryptFH(fhs->clearFH, fhs->encFH);
        if(ret < 0) {
            fprintf(stderr, "ERROR enc_flush: encryptFH failed\n");
            return ret;
        }
        fhs->dirty = FHS_CLEAN;

    }

    ret = dup(fhs->clearFH);
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_flush: dup(clearFH) failed\n");
        perror("ERROR enc_flush");
        return -errno;
    }
    ret = close(ret);
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_flush: close(dup(clearFH)) failed\n");
        perror("ERROR enc_flush");
        return -errno;
    }

    ret = dup(fhs->encFH);
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_flush: dup(encFH) failed\n");
        perror("ERROR enc_flush");
        return -errno;
    }
    ret = close(ret);
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_flush: close(dup(encFH)) failed\n");
        perror("ERROR enc_flush");
        return -errno;
    }

    return RETURN_SUCCESS;

}

static int enc_fsync(const char* path, int isdatasync,
                     fuse_file_info_t* fi) {

    (void) path;

    int ret;
    enc_fhs_t* fhs;

    if(!fi) {
        fprintf(stderr, "ERROR enc_fsync: fi is NULL");
        return -EINVAL;
    }

    fhs = get_fhs(fi->fh);

    if(fhs->dirty == FHS_DIRTY) {

        ret = encryptFH(fhs->clearFH, fhs->encFH);
        if(ret < 0) {
            fprintf(stderr, "ERROR enc_fsync: encryptFH failed\n");
            return ret;
        }
        fhs->dirty = FHS_CLEAN;

    }

    if(isdatasync) {
        ret = fdatasync(fhs->encFH);
    }
    else {
        ret = fsync(fhs->encFH);
    }

    if(ret < 0) {
        fprintf(stderr, "ERROR enc_fsync: fdatasync/fsync failed\n");
        perror("ERROR enc_fsync");
        return -errno;
    }

    return RETURN_SUCCESS;

}

static int enc_release(const char* path, fuse_file_info_t* fi) {

    (void) path;

    int ret;
    enc_fhs_t* fhs;

    if(!fi) {
        fprintf(stderr, "ERROR enc_release: fi is NULL");
        return -EINVAL;
    }

    fhs = get_fhs(fi->fh);

    if(fhs->dirty == FHS_DIRTY) {

        ret = encryptFH(fhs->clearFH, fhs->encFH);
        if(ret < 0) {
            fprintf(stderr, "ERROR enc_release: encryptFH failed\n");
            return ret;
        }

    }

    ret = removeFile(fhs->clearPath);
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_release: removeFile failed\n");
        return ret;
    }

    ret = closeFilePair(fhs);
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_release: closeFilePair failed\n");
        return ret;
    }

    return RETURN_SUCCESS;

}

static int enc_lock(const char* path, fuse_file_info_t* fi, int cmd,
                    flock_t* lock) {

    (void) path;

    int ret;
    enc_fhs_t* fhs;

    fhs = get_fhs(fi->fh);

    ret = ulockmgr_op(fhs->clearFH, cmd, lock, &fi->lock_owner,
                      sizeof(fi->lock_owner));
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_lock: ulockmgr_op failed\n");
        return ret;
    }

    return ret;

}

static int enc_flock(const char* path, fuse_file_info_t* fi, int op) {

    (void) path;

    int ret;
    enc_fhs_t* fhs;

    fhs = get_fhs(fi->fh);

    ret = flock(fhs->clearFH, op);
    if(ret < 0) {
        fprintf(stderr, "ERROR enc_flock: flock failed\n");
        perror("ERROR enc_flock");
        return -errno;
    }

    return RETURN_SUCCESS;

}

/* xattr operations are optional and can safely be left unimplemented */
/* static int enc_setxattr(const char* path, const char* name, const char* value, */
/*                         size_t size, int flags) { */

/*     int ret; */
/*     char fullPath[PATHBUFSIZE]; */

/*     ret = buildPath(path, fullPath, sizeof(fullPath)); */
/*     if(ret < 0){ */
/*         fprintf(stderr, "ERROR enc_setxattr: buildPath failed\n"); */
/*         return ret; */
/*     } */
/*     path = NULL; */

/*     ret = lsetxattr(fullPath, name, value, size, flags); */
/*     if(ret < 0) { */
/*         fprintf(stderr, "ERROR enc_setxattr: lsetxattr failed\n"); */
/*         perror("ERROR enc_setxattr"); */
/*         return -errno; */
/*     } */

/*     return RETURN_SUCCESS; */

/* } */

/* static int enc_getxattr(const char* path, const char* name, char* value, */
/*                         size_t size) { */

/*     int ret; */
/*     char fullPath[PATHBUFSIZE]; */

/*     ret = buildPath(path, fullPath, sizeof(fullPath)); */
/*     if(ret < 0){ */
/*         fprintf(stderr, "ERROR enc_getxattr: buildPath failed\n"); */
/*         return ret; */
/*     } */
/*     path = NULL; */

/*     ret = lgetxattr(fullPath, name, value, size); */
/*     if(ret < 0) { */
/*         fprintf(stderr, "ERROR enc_getxattr: lgetxattr failed\n"); */
/*         perror("ERROR enc_getxattr"); */
/*         return -errno; */
/*     } */

/*     return ret; */

/* } */

/* static int enc_listxattr(const char* path, char* list, size_t size) { */

/*     int ret; */
/*     char fullPath[PATHBUFSIZE]; */

/*     ret = buildPath(path, fullPath, sizeof(fullPath)); */
/*     if(ret < 0){ */
/*         fprintf(stderr, "ERROR enc_listxattr: buildPath failed\n"); */
/*         return ret; */
/*     } */
/*     path = NULL; */

/*     ret = llistxattr(fullPath, list, size); */
/*     if(ret < 0) { */
/*         fprintf(stderr, "ERROR enc_listxattr: llistxattr failed\n"); */
/*         perror("ERROR enc_listxattr"); */
/*         return -errno; */
/*     } */

/*     return ret; */

/* } */

/* static int enc_removexattr(const char* path, const char* name) { */

/*     int ret; */
/*     char fullPath[PATHBUFSIZE]; */

/*     ret = buildPath(path, fullPath, sizeof(fullPath)); */
/*     if(ret < 0){ */
/*         fprintf(stderr, "ERROR enc_removexattr: buildPath failed\n"); */
/*         return RETURN_FAILURE; */
/*     } */
/*     path = NULL; */

/*     ret = lremovexattr(fullPath, name); */
/*     if(ret < 0) { */
/*         fprintf(stderr, "ERROR enc_removexattr: lremovexattr failed\n"); */
/*         perror("ERROR enc_removexattr"); */
/*         return -errno; */
/*     } */

/*     return RETURN_SUCCESS; */

/* } */

static struct fuse_operations enc_oper = {

    /* Access Control */
    .access     = enc_access,       /* Check File Permissions */
    .lock       = enc_lock,         /* Lock File */
    .flock      = enc_flock,        /* Lock Open File */

    /* Metadata */
    .chmod      = enc_chmod,        /* Change File Permissions */
    .chown      = enc_chown,        /* Change File Owner */
    .getattr    = enc_getattr,      /* Get File Attributes */
    .fgetattr   = enc_fgetattr,     /* Get Open File Attributes  */
    .statfs     = enc_statfs,       /* Get File System Statistics */
    .utimens    = enc_utimens,      /* Change the Times of a File*/

    /* Create and Delete */
    .create     = enc_create,       /* Create and Open a Regular File */
    .mkdir      = enc_mkdir,        /* Create a Directory */
    .mknod      = enc_mknod,        /* Create a Non-Regular File Node */
    .link       = enc_link,         /* Create a Hard Link */
    .symlink    = enc_symlink,      /* Create a Symbolic Link */
    .rmdir      = enc_rmdir,        /* Remove a Directory */
    .unlink     = enc_unlink,       /* Remove a File */

    /* Open and Close */
    .open       = enc_open,         /* Open a File */
    .opendir    = enc_opendir,      /* Open a Directory */
    .release    = enc_release,      /* Release an Open File */
    .releasedir = enc_releasedir,   /* Release an Open Directory */

    /* Read and Write */
    .read        = enc_read,        /* Read a File */
    .readdir     = enc_readdir,     /* Read a Directory */
    .readlink    = enc_readlink,    /* Read the Target of a Symbolic Link */
    .write       = enc_write,       /* Write a File*/

    /* Modify */
    .rename      = enc_rename,      /* Rename a File */
    .truncate    = enc_truncate,    /* Change the Size of a File */
    .ftruncate   = enc_ftruncate,   /* Change the Size of an Open File*/

    /* Buffering */
    .flush       = enc_flush,       /* Flush Cached Data */
    .fsync       = enc_fsync,       /* Synch Open File Contents */

    /* Extended Attributes */
    /* .setxattr    = enc_setxattr,    /\* Set XATTR *\/ */
    /* .getxattr    = enc_getxattr,    /\* Get XATTR *\/ */
    /* .listxattr   = enc_listxattr,   /\* List XATTR *\/ */
    /* .removexattr = enc_removexattr, /\* Remove XATTR *\/ */

    /* Flags */
    .flag_nullpath_ok   = 1,
    .flag_utime_omit_ok = 1,

};

int main(int argc, char *argv[]) {

    fuse_args_t args = FUSE_ARGS_INIT(0, NULL);
    fsState_t state;
    int i;

    if(argc < 3){
	fprintf(stderr,
		"Usage:\n %s <Mount Point> <Mirrored Directory>\n",
		argv[0]);
	exit(EXIT_FAILURE);
    }

    for(i = 0; i < argc; i++) {
	if (i == 2)
	    state.basePath = realpath(argv[i], NULL);
	else
	    fuse_opt_add_arg(&args, argv[i]);
    }

    umask(0);

    return fuse_main(args.argc, args.argv, &enc_oper, &state);

}
