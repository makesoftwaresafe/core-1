/*
   Copyright (C) CFEngine AS

   This file is part of CFEngine 3 - written and maintained by CFEngine AS.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; version 3.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of CFEngine, the applicable Commercial Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/

#include <generic_agent.h>

#include <lastseen.h>
#include <dir.h>
#include <scope.h>
#include <files_copy.h>
#include <files_interfaces.h>
#include <files_hashes.h>
#include <keyring.h>
#include <communication.h>
#include <eval_context.h>
#include <crypto.h>
#include <file_lib.h>

#include <cf-key-functions.h>

static const char *const passphrase = "Cfengine passphrase";

RSA* LoadPublicKey(const char* filename)
{
    unsigned long err;
    FILE* fp;
    RSA* key;

    fp = safe_fopen(filename, "r");
    if (fp == NULL)
    {
        Log(LOG_LEVEL_ERR, "Cannot open file '%s'. (fopen: %s)", filename, GetErrorStr());
        return NULL;
    };

    if ((key = PEM_read_RSAPublicKey(fp, NULL, NULL,
                                     (void *)passphrase)) == NULL)
    {
        err = ERR_get_error();
        Log(LOG_LEVEL_ERR, "Error reading public key. (PEM_read_RSAPublicKey: %s)",
            ERR_reason_error_string(err));
        fclose(fp);
        return NULL;
    };

    fclose(fp);

    if (BN_num_bits(key->e) < 2 || !BN_is_odd(key->e))
    {
        Log(LOG_LEVEL_ERR, "RSA Exponent in key '%s' too small or not odd. (BN_num_bits: %s)",
            filename, GetErrorStr());
        return NULL;
    };

    return key;
}

/** Return a string with the printed digest of the given key file,
    or NULL if an error occurred. */
char* GetPubkeyDigest(const char* pubkey)
{
    unsigned char digest[EVP_MAX_MD_SIZE + 1];
    RSA* key = NULL;
    char* buffer = xmalloc(EVP_MAX_MD_SIZE * 4);

    key = LoadPublicKey(pubkey);
    if (NULL == key)
    {
        return NULL;
    }

    HashPubKey(key, digest, CF_DEFAULT_DIGEST);
    HashPrintSafe(CF_DEFAULT_DIGEST, true, digest, buffer);
    return buffer;
}

/*****************************************************************************/

/** Print digest of the specified public key file.
    Return 0 on success and 1 on error. */
int PrintDigest(const char* pubkey)
{
    char *digeststr = GetPubkeyDigest(pubkey);

    if (NULL == digeststr)
    {
        return 1; /* ERROR exitcode */
    }

    fprintf(stdout, "%s\n", digeststr);
    free(digeststr);
    return 0; /* OK exitcode */
}

int TrustKey(const char* pubkey)
{
    char *digeststr = GetPubkeyDigest(pubkey);
    char outfilename[CF_BUFSIZE];
    bool ok;

    if (NULL == digeststr)
        return 1; /* ERROR exitcode */

    snprintf(outfilename, CF_BUFSIZE, "%s/ppkeys/root-%s.pub", CFWORKDIR, digeststr);
    free(digeststr);

    ok = CopyRegularFileDisk(pubkey, outfilename);

    return (ok? 0 : 1);
}

bool ShowHost(const char *hostkey, const char *address, bool incoming,
                     const KeyHostSeen *quality, void *ctx)
{
    int *count = ctx;
    char timebuf[26];

    char hostname[MAXHOSTNAMELEN];
    int ret = IPString2Hostname(hostname, address, sizeof(hostname));

    (*count)++;
    printf("%-10.10s %-40.40s %-25.25s %-26.26s %-s\n",
           incoming ? "Incoming" : "Outgoing",
           address, (ret != -1) ? hostname : "-",
           cf_strtimestamp_local(quality->lastseen, timebuf), hostkey);

    return true;
}

void ShowLastSeenHosts()
{
    int count = 0;

    printf("%-10.10s %-40.40s %-25.25s %-26.26s %-s\n", "Direction", "IP", "Name", "Last connection", "Key");

    if (!ScanLastSeenQuality(ShowHost, &count))
    {
        Log(LOG_LEVEL_ERR, "Unable to show lastseen database");
        return;
    }

    printf("Total Entries: %d\n", count);
}

/**
 * @brief removes all traces of entry 'input' from lastseen and filesystem
 *
 * @param[in] key digest (SHA/MD5 format) or free host name string
 * @param[in] must_be_coherent. false : delete if lastseen is incoherent, 
 *                              true :  don't if lastseen is incoherent
 * @retval 0 if entry was deleted, >0 otherwise
 */
int RemoveKeys(const char *input, bool must_be_coherent)
{
    int res = 0;
    char equivalent[CF_BUFSIZE];
    equivalent[0] = '\0';

    res = RemoveKeysFromLastSeen(input, must_be_coherent, equivalent);
    if (res!=0)
    {
        return res;
    }

    Log(LOG_LEVEL_INFO, "Removed corresponding entries from lastseen database.");

    int removed_input      = RemovePublicKey(input);
    int removed_equivalent = RemovePublicKey(equivalent);

    if ((removed_input == -1) || (removed_equivalent == -1))
    {
        Log(LOG_LEVEL_ERR, "Unable to remove keys for the entry %s", input);
        return 255;
    }
    else if (removed_input + removed_equivalent == 0)
    {
        Log(LOG_LEVEL_ERR, "No key file(s) for entry %s were found on the filesytem", input);
        return 1;
    }
    else
    {
        Log(LOG_LEVEL_INFO, "Removed %d corresponding key file(s) from filesystem.",
              removed_input + removed_equivalent);
        return 0;
    }

    return -1;
}

void KeepKeyPromises(const char *public_key_file, const char *private_key_file)
{
    unsigned long err;
#ifdef OPENSSL_NO_DEPRECATED
    RSA *pair = RSA_new();
    BIGNUM *rsa_bignum = BN_new();
#else
    RSA *pair;
#endif
    FILE *fp;
    struct stat statbuf;
    int fd;
    const EVP_CIPHER *cipher;
    char vbuff[CF_BUFSIZE];

    cipher = EVP_des_ede3_cbc();

    if (stat(public_key_file, &statbuf) != -1)
    {
        printf("A key file already exists at %s\n", public_key_file);
        return;
    }

    if (stat(private_key_file, &statbuf) != -1)
    {
        printf("A key file already exists at %s\n", private_key_file);
        return;
    }

    printf("Making a key pair for cfengine, please wait, this could take a minute...\n");

#ifdef OPENSSL_NO_DEPRECATED
    BN_set_word(rsa_bignum, 35);

    if (!RSA_generate_key_ex(pair, 2048, rsa_bignum, NULL))
#else
    pair = RSA_generate_key(2048, 35, NULL, NULL);

    if (pair == NULL)
#endif
    {
        err = ERR_get_error();
        Log(LOG_LEVEL_ERR, "Unable to generate key '%s'", ERR_reason_error_string(err));
        return;
    }

    fd = safe_open(private_key_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    if (fd < 0)
    {
        Log(LOG_LEVEL_ERR, "Open '%s' failed. (open: %s)", private_key_file, GetErrorStr());
        return;
    }

    if ((fp = fdopen(fd, "w")) == NULL)
    {
        Log(LOG_LEVEL_ERR, "Couldn't open private key '%s'. (fdopen: %s)", private_key_file, GetErrorStr());
        close(fd);
        return;
    }

    Log(LOG_LEVEL_VERBOSE, "Writing private key to '%s'", private_key_file);

    if (!PEM_write_RSAPrivateKey(fp, pair, cipher, (void *)passphrase,
                                 strlen(passphrase), NULL, NULL))
    {
        err = ERR_get_error();
        Log(LOG_LEVEL_ERR, "Couldn't write private key. (PEM_write_RSAPrivateKey: %s)", ERR_reason_error_string(err));
        return;
    }

    fclose(fp);

    fd = safe_open(public_key_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    if (fd < 0)
    {
        Log(LOG_LEVEL_ERR, "Unable to open public key '%s'. (open: %s)",
            public_key_file, GetErrorStr());
        return;
    }

    if ((fp = fdopen(fd, "w")) == NULL)
    {
        Log(LOG_LEVEL_ERR, "Open '%s' failed. (fdopen: %s)", public_key_file, GetErrorStr());
        close(fd);
        return;
    }

    Log(LOG_LEVEL_VERBOSE, "Writing public key to file '%s'", public_key_file);

    if (!PEM_write_RSAPublicKey(fp, pair))
    {
        err = ERR_get_error();
        Log(LOG_LEVEL_ERR, "Unable to write public key. (PEM_write_RSAPublicKey: %s)", ERR_reason_error_string(err));
        return;
    }

    fclose(fp);

    snprintf(vbuff, CF_BUFSIZE, "%s/randseed", CFWORKDIR);
    RAND_write_file(vbuff);
    chmod(vbuff, 0644);
}


ENTERPRISE_FUNC_1ARG_DEFINE_STUB(bool, LicenseInstall, ARG_UNUSED char *, path_source)
{
    Log(LOG_LEVEL_ERR, "License installation only applies to CFEngine Enterprise");

    return false;
}
