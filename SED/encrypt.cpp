/*
* PSP Software Development Kit - http://www.pspdev.org
* -----------------------------------------------------------------------
* Licensed under the BSD license, see LICENSE in PSPSDK root for details.
*
* encrypt.c - Encryption routines using sceChnnlsv
*
* Copyright (c) 2005 Jim Paris <jim@jtan.com>
* Coypright (c) 2005 psp123
*
* $Id: encrypt.c 1560 2005-12-10 01:16:32Z jim $
*/

#include "sed.h"
#include "hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline int align16(unsigned int v)
{
    return ((v + 0xF) >> 4) << 4;
}

int fopen_getsize(const char *filename, FILE **fd, size_t *size)
{
    if ((*fd = fopen(filename, "rb")) == NULL) {
        return -1;
    }

    fseek(*fd, 0, SEEK_END);
    *size = ftell(*fd);
    fseek(*fd, 0, SEEK_SET);

    if (*size <= 0) {
        fclose(*fd);
        return -2;
    }

    return 0;
}

/* Do the actual hardware encryption.
mode is 3 for saves with a cryptkey, or 1 otherwise
data, dataLen, and cryptkey must be multiples of 0x10.
cryptkey is NULL if mode == 1.
*/
int encrypt_data(unsigned int mode,
                 unsigned char *data,
                 size_t *dataLen,
                 int *alignedLen,
                 unsigned char *hash,
                 unsigned char *cryptkey)
{
    pspChnnlsvContext1 ctx1;
    pspChnnlsvContext2 ctx2;

    /* Make room for the IV in front of the data. */
    memmove(data + 0x10, data, *alignedLen);

    /* Set up buffers */
    memset(&ctx1, 0, sizeof(pspChnnlsvContext1));
    memset(&ctx2, 0, sizeof(pspChnnlsvContext2));
    memset(hash, 0, 0x10);
    memset(data, 0, 0x10);

    /* Build the 0x10-byte IV and setup encryption */
    if (sceSdCreateList_(ctx2, mode, 1, data, cryptkey) < 0) {
        return -1;
    }
    if (sceSdSetIndex_(ctx1, mode) < 0) {
        return -2;
    }
    if (sceSdRemoveValue_(ctx1, data, 0x10) < 0) {
        return -3;
    }
    if (sceSdSetMember_(ctx2, data + 0x10, *alignedLen) < 0) {
        return -4;
    }

    /* Clear any extra bytes left from the previous steps */
    memset(data + 0x10 + *dataLen, 0, *alignedLen - *dataLen);

    /* Encrypt the data */
    if (sceSdRemoveValue_(ctx1, data + 0x10, *alignedLen) < 0) {
        return -5;
    }

    /* Verify encryption */
    if (sceChnnlsv_21BE78B4_(ctx2) < 0) {
        return -6;
    }

    /* Build the file hash from this PSP */
    if (sceSdGetLastIndex_(ctx1, hash, cryptkey) < 0) {
        return -7;
    }

    /* Adjust sizes to account for IV */
    *alignedLen += 0x10;
    *dataLen += 0x10;

    /* All done */
    return 0;
}

/* Encrypt the given plaintext file, and update the message
authentication hashes in the param.sfo.  The data_filename is
usually the final component of encrypted_filename, e.g. "DATA.BIN".
See main.c for an example of usage. */
int Savedata::Encrypt(const char *plaintext_filename,
                      const char *encrypted_filename,
                      const char *data_filename,
                      const char *paramsfo_filename,
                      const unsigned char *gamekey)
{
    FILE *in = NULL, *out = NULL, *sfo = NULL;
    unsigned char *data = NULL, *cryptkey = NULL, *hash = NULL;
    unsigned char paramsfo[0x1330];
    size_t len, tmp;
    int aligned_len;
    int retval = 0;

    /* Open plaintext and param.sfo files and get size */

    if (fopen_getsize(plaintext_filename, &in, &len) < 0) {
        retval = FILE_IO_ERROR;
        goto out;
    }

    //fopen_getsize(paramsfo_filename, &sfo, &tmp);

    if (fopen_getsize(paramsfo_filename, &sfo, &tmp) < 0) {
        retval = FILE_IO_ERROR;
        goto out;
    }

    /* Verify size of param.sfo; all known saves use this size */

    if (tmp != 0x1330) {
        retval = INVALID_SFO_ERROR;
        goto out;
    }


    /* Allocate buffers.  data has 0x10 bytes extra for the IV. */

    aligned_len = align16(len);

    if ((data =
                (unsigned char *) aligned_alloc(0x10, aligned_len + 0x10)) == NULL) {
        retval = MEMORY_ERROR;
        goto out;
    }

    if ((cryptkey = (unsigned char *) aligned_alloc(0x10, 0x10)) == NULL) {
        retval = MEMORY_ERROR;
        goto out;
    }

    if ((hash = (unsigned char *) aligned_alloc(0x10, 0x10)) == NULL) {
        retval = MEMORY_ERROR;
        goto out;
    }

    /* Fill buffers. */

    memset(data + len, 0, aligned_len - len);
    if (fread(data, 1, len, in) != len) {
        retval = FILE_IO_ERROR;
        goto out;
    }

    if (fread(paramsfo, 1, 0x1330, sfo) != 0x1330) {
        retval = FILE_IO_ERROR;
        goto out;
    }

    if (gamekey != NULL) {
        memcpy(cryptkey, gamekey, 0x10);
    }

    /* Do the encryption */

    if ((retval = encrypt_data( gamekey ? (5) : 1,
                                data,
                                &len, &aligned_len,
                                hash,
                                gamekey ? cryptkey : NULL)) < 0) {
        retval += ENCRYPT_ERROR;
        goto out;
    }

    /* Update the param.sfo hashes */

    if ((retval = update_hashes(paramsfo, 0x1330,
                                data_filename, hash,
                                paramsfo[0x11b0]>>4/*gamekey ? 3 : 1*/)) < 0) {
        retval += HASH_ERROR;
        goto out;
    }

    /* Write the data to the file.  encrypt_data has already set len. */

    if ((out = fopen(encrypted_filename, "wb")) == NULL) {
        retval = FILE_IO_ERROR;
        goto out;
    }

    if (fwrite(data, 1, len, out) != len) {
        retval = FILE_IO_ERROR;
        goto out;
    }

    /* Reopen param.sfo, and write the updated copy out. */

    fclose(sfo);
    if ((sfo = fopen(paramsfo_filename, "wb")) == NULL) {
        retval = FILE_IO_ERROR;
        goto out;
    }

    if (fwrite(paramsfo, 1, 0x1330, sfo) != 0x1330) {
        retval = FILE_IO_ERROR;
        goto out;
    }

    /* All done.  Return file length. */

    retval = len;

out:
    if(out) {
        fclose(out);
    }
    if(hash) {
        aligned_free(hash);
    }
    if(cryptkey) {
        aligned_free(cryptkey);
    }
    if(data) {
        aligned_free(data);
    }
    if(sfo) {
        fclose(sfo);
    }
    if(in) {
        fclose(in);
    }

    return retval;
}
