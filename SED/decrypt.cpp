#include "sed.h"


/* Do the actual hardware decryption.
mode is 3 for saves with a cryptkey, or 1 otherwise
data, dataLen, and cryptkey must be multiples of 0x10.
cryptkey is NULL if mode == 1.
*/
int decrypt_data(unsigned int mode,
                 unsigned char *data,
                 size_t *dataLen,
                 int *alignedLen,
                 unsigned char *cryptkey)
{
    pspChnnlsvContext1 ctx1;
    pspChnnlsvContext2 ctx2;

    /* Need a 16-byte IV plus some data */
    if (*alignedLen <= 0x10) {
        return -1;
    }
    *dataLen -= 0x10;
    *alignedLen -= 0x10;

    /* Set up buffers */
    memset(&ctx1, 0, sizeof(pspChnnlsvContext1));
    memset(&ctx2, 0, sizeof(pspChnnlsvContext2));

    /* Perform the magic */
    if (sceSdSetIndex_(ctx1, mode) < 0) {
        return -2;
    }
    if (sceSdCreateList_(ctx2, mode, 2, data, cryptkey) < 0) {
        return -3;
    }
    if (sceSdRemoveValue_(ctx1, data, 0x10) < 0) {
        return -4;
    }
    if (sceSdRemoveValue_(ctx1, data + 0x10, *alignedLen) < 0) {
        return -5;
    }
    if (sceSdSetMember_(ctx2, data + 0x10, *alignedLen) < 0) {
        return -6;
    }

    /* Verify that it decrypted correctly */
    if (sceChnnlsv_21BE78B4_(ctx2) < 0) {
        return -7;
    }

    // If desired, a new file hash from this PSP can be computed now:
    //if (sceSdGetLastIndex_(ctx1, newhash, cryptkey) < 0)
    //   	return -8;
    //

    /* The decrypted data starts at data + 0x10, so shift it back. */
    memmove(data, data + 0x10, *dataLen);

    /* All done */
    return 0;
}

unsigned int align16(unsigned int v)
{
    return ((v + 0xF) >> 4) << 4;
}

/* Read, decrypt, and write a savedata file */
int Savedata::Decrypt(const char *decrypted_filename,
                      const char *encrypted_filename,
                      const unsigned char *gamekey)
{
    FILE *in, *out;
    size_t len;
    int aligned_len;
    unsigned char *data, *cryptkey;
    int retval;

    /* Open file and get size */

    if ((in = fopen(encrypted_filename, "rb")) == NULL) {
        retval = FILE_IO_ERROR;
        goto out;
    }

    fseek(in, 0, SEEK_END);
    len = ftell(in);
    fseek(in, 0, SEEK_SET);

    if (len <= 0) {
        retval = FILE_IO_ERROR;
        goto out1;
    }

    /* Allocate buffers */

    aligned_len = align16(len);

    if ((data = (unsigned char *) aligned_alloc(0x10, aligned_len)) == NULL) {
        retval = MEMORY_ERROR;
        goto out1;
    }

    if ((cryptkey = (unsigned char *) aligned_alloc(0x10, 0x10)) == NULL) {
        retval = MEMORY_ERROR;
        goto out2;
    }

    /* Fill buffers */

    if (gamekey != NULL) {
        memcpy(cryptkey, gamekey, 0x10);
    }

    memset(data + len, 0, aligned_len - len);
    if (fread(data, 1, len, in) != len) {
        retval = FILE_IO_ERROR;
        goto out3;
    }

    /* Do the decryption */

    if ((retval = decrypt_data( gamekey ? (6 >= 4 ? 5 : 3) : 1, // 5 for sdk >= 4, else 3
                                data, &len, &aligned_len,
                                gamekey ? cryptkey : NULL)) < 0) {
        retval += DECRYPT_ERROR;
        goto out3;
    }

    /* Write the data out.  decrypt_data has set len correctly. */

    if ((out = fopen(decrypted_filename, "wb")) == NULL) {
        retval = FILE_IO_ERROR;
        goto out3;
    }

    if (fwrite(data, 1, len, out) != len) {
        retval = FILE_IO_ERROR;
        goto out4;
    }

    /* All done.  Return file length. */
    retval = len;
out4:
    fclose(out);
out3:
    aligned_free(cryptkey);
out2:
    aligned_free(data);
out1:
    fclose(in);
out:
    return retval;
}

