/*
   Copyright 2020 Northern.tech AS

   This file is part of CFEngine 3 - written and maintained by Northern.tech AS.

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

/*
   cf-keycrypt.c

   Copyright (C) 2017 cfengineers.net

   Written and maintained by Jon Henrik Bjornstad <jonhenrik@cfengineers.net>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

*/

#include <platform.h>
#include <openssl/err.h>

#include <lastseen.h>
#include <crypto.h>
#include <writer.h>
#include <man.h>
#include <conversion.h>
#include <hash.h>
#include <known_dirs.h>
#include <string_lib.h>
#include <file_lib.h>
#include <unistd.h>

#define BUFSIZE 1024

typedef enum {
    HOST_RSA_KEY_PRIVATE,
    HOST_RSA_KEY_PUBLIC,
} HostRSAKeyType;

static const char passphrase[] = "Cfengine passphrase";

//*******************************************************************
// DOCUMENTATION / GETOPT CONSTS:
//*******************************************************************

static const char *const CF_KEYCRYPT_SHORT_DESCRIPTION =
    "cf-keycrypt: Use CFEngine cryptographic keys to encrypt and decrypt files";

static const char *const CF_KEYCRYPT_MANPAGE_LONG_DESCRIPTION =
    "cf-keycrypt offers a simple way to encrypt or decrypt files using keys "
    "generated by cf-key. CFEngine uses asymmetric cryptography, and "
    "cf-keycrypt allows you to encrypt a file using a public key file. "
    "The encrypted file can only be decrypted on the host with the "
    "corresponding private key. Original author: Jon Henrik Bjornstad "
    "<jonhenrik@cfengineers.net>";

static const struct option OPTIONS[] =
{
    {"help",        no_argument,        0, 'h'},
    {"manpage",     no_argument,        0, 'M'},
    {"encrypt",     no_argument,        0, 'e'},
    {"decrypt",     no_argument,        0, 'd'},
    {"key",         required_argument,  0, 'k'},
    {"host",        required_argument,  0, 'H'},
    {"output",      required_argument,  0, 'o'},
    {NULL,          0,                  0, '\0'}
};

static const char *const HINTS[] =
{
    "Print the help message",
    "Print the man page",
    "Encrypt file",
    "Decrypt file",
    "Use key file",
    "Encrypt for host (get key from lastseen database)",
    "Output file",
    NULL
};

static inline void *GetIPAddress(struct sockaddr *sa)
{
    assert(sa != NULL);
    if (sa->sa_family == AF_INET)
    {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }
    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

/**
 * Get path of the RSA key for the given host.
 */
static char *GetHostRSAKey(const char *host, HostRSAKeyType type)
{
    const char *key_ext = NULL;
    if (type == HOST_RSA_KEY_PRIVATE)
    {
        key_ext = ".priv";
    }
    else
    {
        key_ext = ".pub";
    }

    struct addrinfo *result;
    int error = getaddrinfo(host, NULL, NULL, &result);
    if (error != 0)
    {
        Log(LOG_LEVEL_ERR, "Failed to get IP from host (getaddrinfo: %s)",
            gai_strerror(error));
        return NULL;
    }

    char *buffer = malloc(BUFSIZE);
    char hash[CF_HOSTKEY_STRING_SIZE];
    char ipaddress[64];
    bool found = false;
    for (struct addrinfo *res = result; !found && (res != NULL); res = res->ai_next)
    {
        inet_ntop(res->ai_family,
                  GetIPAddress((struct sockaddr *) res->ai_addr),
                  ipaddress, sizeof(ipaddress));
        if (StringStartsWith(ipaddress, "127.") || StringSafeEqual(ipaddress, "::1"))
        {
            found = true;
            snprintf(buffer, BUFSIZE, "%s/ppkeys/localhost%s", WORKDIR, key_ext);
            return buffer;
        }
        found = Address2Hostkey(hash, sizeof(hash), ipaddress);
    }
    if (found)
    {
        snprintf(buffer, BUFSIZE, "%s/ppkeys/root-%s%s", WORKDIR, hash, key_ext);
        freeaddrinfo(result);
        return buffer;
    }
    else
    {
        for (struct addrinfo *res = result; res != NULL; res = res->ai_next)
        {
            inet_ntop(res->ai_family,
                      GetIPAddress((struct sockaddr *) res->ai_addr),
                      ipaddress, sizeof(ipaddress));
            snprintf(buffer, BUFSIZE, "%s/ppkeys/root-%s%s", WORKDIR, ipaddress, key_ext);
            if (access(buffer, F_OK) == 0)
            {
                freeaddrinfo(result);
                return buffer;
            }
        }
    }
    return NULL;
}

static RSA *ReadPrivateKey(const char *privkey_path)
{
    FILE *fp = safe_fopen(privkey_path,"r");

    if (fp == NULL)
    {
        Log(LOG_LEVEL_ERR, "Could not open private key '%s'", privkey_path);
        return NULL;
    }
    RSA *privkey = PEM_read_RSAPrivateKey(fp, (RSA **) NULL, NULL, (void *) passphrase);
    if (privkey == NULL)
    {
        unsigned long err = ERR_get_error();
        Log(LOG_LEVEL_ERR, "Could not read private key '%s': %s",
            privkey_path, ERR_reason_error_string(err));
    }
    fclose(fp);
    return privkey;
}

static RSA *ReadPublicKey(const char *pubkey_path)
{
    FILE *fp = safe_fopen(pubkey_path, "r");

    if (fp == NULL)
    {
        Log(LOG_LEVEL_ERR, "Could not open public key '%s'", pubkey_path);
        return NULL;
    }

    RSA *pubkey = PEM_read_RSAPublicKey(fp, NULL, NULL, (void *) passphrase);
    if (pubkey == NULL)
    {
        unsigned long err = ERR_get_error();
        Log(LOG_LEVEL_ERR, "Could not read public key '%s': %s",
            pubkey_path, ERR_reason_error_string(err));
    }
    fclose(fp);
    return pubkey;
}

static bool RSAEncrypt(const char *pubkey_path, const char *input_path, const char *output_path)
{
    RSA* pubkey = ReadPublicKey(pubkey_path);
    if (pubkey == NULL)
    {
        return false;
    }

    FILE *input_file = safe_fopen(input_path, "r");
    if (input_file == NULL)
    {
        Log(LOG_LEVEL_ERR, "Could not open input file '%s'", input_path);
        RSA_free(pubkey);
        return false;
    }

    FILE *output_file = safe_fopen(output_path, "w");
    if (output_file == NULL)
    {
        Log(LOG_LEVEL_ERR, "Could not create output file '%s'", output_path);
        fclose(input_file);
        RSA_free(pubkey);
        return false;
    }

    const int key_size = RSA_size(pubkey);
    char tmp_ciphertext[key_size], tmp_plaintext[key_size];

    srand(time(NULL));
    unsigned long len = 0;
    bool error = false;
    while (!error && !feof(input_file))
    {
        len = fread(tmp_plaintext, 1, key_size - RSA_PKCS1_PADDING_SIZE, input_file);
        if (len <= 0 || ferror(input_file) != 0)
        {
            Log(LOG_LEVEL_ERR, "Could not read file '%s'", input_path);
            error = true;
            break;
        }
        unsigned long size = RSA_public_encrypt(len, tmp_plaintext, tmp_ciphertext,
                                                pubkey, RSA_PKCS1_PADDING);
        if (size == -1)
        {
            Log(LOG_LEVEL_ERR, "Failed to encrypt data: %s",
                ERR_error_string(ERR_get_error(), NULL));
            error = true;
            break;
        }
        fwrite(tmp_ciphertext, 1, key_size, output_file);
        if (ferror(output_file) != 0)
        {
            Log(LOG_LEVEL_ERR, "Could not write file '%s'", output_path);
            error = true;
            break;
        }
    }
    OPENSSL_cleanse(tmp_plaintext, key_size);
    fclose(input_file);
    fclose(output_file);
    RSA_free(pubkey);
    return !error;
}

static bool RSADecrypt(const char *privkey_path, const char *input_path, const char *output_path)
{
    RSA *privkey = ReadPrivateKey(privkey_path);
    if (privkey == NULL)
    {
        return false;
    }

    FILE *input_file = safe_fopen(input_path, "r");
    if (input_file == NULL)
    {
        Log(LOG_LEVEL_ERR, "Cannot open input file '%s'", input_path);
        RSA_free(privkey);
        return false;
    }

    FILE *output_file  = safe_fopen(output_path, "w");
    if (output_file == NULL)
    {
        Log(LOG_LEVEL_ERR, "Cannot open output file '%s'", output_path);
        fclose(input_file);
        RSA_free(privkey);
        return false;
    }

    const int key_size = RSA_size(privkey);
    char tmp_ciphertext[key_size], tmp_plaintext[key_size];

    bool error = false;
    while (!error && !feof(input_file))
    {
        unsigned long int len = fread(tmp_ciphertext, 1, key_size, input_file);
        if (ferror(input_file) != 0)
        {
            Log(LOG_LEVEL_ERR, "Could not read from '%s'", input_path);
            error = true;
        }
        else if (len > 0)
        {
            int size = RSA_private_decrypt(key_size, tmp_ciphertext, tmp_plaintext,
                                           privkey, RSA_PKCS1_PADDING);
            if (size == -1)
            {
                Log(LOG_LEVEL_ERR, "Failed to decrypt data: %s",
                    ERR_error_string(ERR_get_error(), NULL));
                error = true;
            }
            else
            {
                fwrite(tmp_plaintext, 1, size, output_file);
                if (ferror(output_file) != 0)
                {
                    Log(LOG_LEVEL_ERR, "Could not write to '%s'", output_path);
                    error = true;
                }
            }
        }
    }
    OPENSSL_cleanse(tmp_plaintext, key_size);
    fclose(input_file);
    fclose(output_file);
    RSA_free(privkey);
    return !error;
}

static void CFKeyCryptHelp()
{
    Writer *w = FileWriter(stdout);
    WriterWriteHelp(w, "cf-keycrypt", OPTIONS, HINTS, false, NULL);
    FileWriterDetach(w);
}

void CFKeyCryptMan()
{
    Writer *out = FileWriter(stdout);
    ManPageWrite(out, "cf-keycrypt", time(NULL),
                 CF_KEYCRYPT_SHORT_DESCRIPTION,
                 CF_KEYCRYPT_MANPAGE_LONG_DESCRIPTION,
                 OPTIONS, HINTS, true);
    FileWriterDetach(out);
}

int main(int argc, char *argv[])
{
    if (argc == 1)
    {
        CFKeyCryptHelp();
        exit(EXIT_FAILURE);
    }

    opterr = 0;
    char *key_path = NULL;
    char *input_path = NULL;
    char *output_path = NULL;
    char *host = NULL;
    bool encrypt = false;
    bool decrypt = false;

    int c = 0;
    while ((c = getopt_long(argc, argv, "hMedk:o:H:", OPTIONS, NULL)) != -1)
    {
        switch (c)
        {
            case 'h':
                CFKeyCryptHelp();
                exit(EXIT_SUCCESS);
                break;
            case 'M':
                CFKeyCryptMan();
                exit(EXIT_SUCCESS);
                break;
            case 'e':
                encrypt = true;
                break;
            case 'd':
                decrypt = true;
                break;
            case 'k':
                key_path = optarg;
                break;
            case 'o':
                output_path = optarg;
                break;
            case 'H':
                host = optarg;
                break;
            default:
                Log(LOG_LEVEL_ERR, "Unknown option '-%c'", optopt);
                CFKeyCryptHelp();
                exit(EXIT_FAILURE);
        }
    }

    input_path = argv[optind];
    optind += 1;
    if (optind < argc)
    {
        Log(LOG_LEVEL_ERR, "Unexpected non-option argument: '%s'", argv[optind]);
        exit(EXIT_FAILURE);
    }

    // Check for argument errors:
    if ((encrypt && decrypt) || (!encrypt && !decrypt))
    {
        Log(LOG_LEVEL_ERR, "Must specify either encrypt or decrypt (and not both)");
        exit(EXIT_FAILURE);
    }

    if ((host != NULL) && (key_path != NULL))
    {
        Log(LOG_LEVEL_ERR,
            "--host/-H is used to specify a public key and cannot be used with --key/-k");
        exit(EXIT_FAILURE);
    }

    if (input_path == NULL)
    {
        Log(LOG_LEVEL_ERR, "No input file specified (Use -h for help)");
        exit(EXIT_FAILURE);
    }
    if (output_path == NULL)
    {
        Log(LOG_LEVEL_ERR, "No output file specified (Use -h for help)");
        exit(EXIT_FAILURE);
    }

    CryptoInitialize();
    if (host != NULL)
    {
        HostRSAKeyType key_type = encrypt ? HOST_RSA_KEY_PUBLIC : HOST_RSA_KEY_PRIVATE;
        key_path = GetHostRSAKey(host, key_type);
        if (!key_path)
        {
            Log(LOG_LEVEL_ERR, "Unable to locate key for host '%s'", host);
            exit(EXIT_FAILURE);
        }
    }
    assert (key_path != NULL);

    // Encrypt or decrypt
    bool success;
    if (encrypt)
    {
        success = RSAEncrypt(key_path, input_path, output_path);
        if (!success)
        {
            Log(LOG_LEVEL_ERR, "Encryption failed");
            exit(EXIT_FAILURE);
        }
    }
    else if (decrypt)
    {
        success = RSADecrypt(key_path, input_path, output_path);
        if (!success)
        {
            Log(LOG_LEVEL_ERR, "Decryption failed");
            exit(EXIT_FAILURE);
        }
    }
    else
    {
        ProgrammingError("Unexpected error in cf-keycrypt");
        exit(EXIT_FAILURE);
    }

    return 0;
}
