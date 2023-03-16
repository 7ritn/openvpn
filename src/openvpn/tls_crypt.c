/*
 *  OpenVPN -- An application to securely tunnel IP networks
 *             over a single TCP/UDP port, with support for SSL/TLS-based
 *             session authentication and key exchange,
 *             packet encryption, packet authentication, and
 *             packet compression.
 *
 *  Copyright (C) 2016-2021 Fox Crypto B.V. <openvpn@foxcrypto.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2
 *  as published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#elif defined(_MSC_VER)
#include "config-msvc.h"
#endif

#include "syshead.h"

#include "argv.h"
#include "base64.h"
#include "crypto.h"
#include "platform.h"
#include "run_command.h"
#include "session_id.h"
#include "ssl.h"

#include "tls_crypt.h"

const char *tls_crypt_v2_cli_pem_name = "OpenVPN tls-crypt-v2 client key";
const char *tls_crypt_v2_srv_pem_name = "OpenVPN tls-crypt-v2 server key";

/** Metadata contains user-specified data */
static const uint8_t TLS_CRYPT_METADATA_TYPE_USER           = 0x00;
/** Metadata contains a 64-bit unix timestamp in network byte order */
static const uint8_t TLS_CRYPT_METADATA_TYPE_TIMESTAMP      = 0x01;

static struct key_type
tls_crypt_kt(void)
{
    return create_kt("AES-256-CTR", "SHA256", "tls-crypt");
}

int
tls_crypt_buf_overhead(void)
{
    return packet_id_size(true) + TLS_CRYPT_TAG_SIZE + TLS_CRYPT_BLOCK_SIZE;
}

void
tls_crypt_init_key(struct key_ctx_bi *key, const char *key_file,
                   bool key_inline, bool tls_server)
{
    const int key_direction = tls_server ?
                              KEY_DIRECTION_NORMAL : KEY_DIRECTION_INVERSE;
    struct key_type kt = tls_crypt_kt();
    if (!kt.cipher || !kt.digest)
    {
        msg(M_FATAL, "ERROR: --tls-crypt not supported");
    }
    crypto_read_openvpn_key(&kt, key, key_file, key_inline, key_direction,
                            "Control Channel Encryption", "tls-crypt");
}

bool
tls_crypt_wrap(const struct buffer *src, struct buffer *dst,
               struct crypto_options *opt)
{
    const struct key_ctx *ctx = &opt->key_ctx_bi.encrypt;
    struct gc_arena gc;

    /* IV, packet-ID and implicit IV required for this mode. */
    ASSERT(ctx->cipher);
    ASSERT(ctx->hmac);
    ASSERT(packet_id_initialized(&opt->packet_id));
    ASSERT(hmac_ctx_size(ctx->hmac) == 256/8);

    gc_init(&gc);

    dmsg(D_PACKET_CONTENT, "TLS-CRYPT WRAP FROM: %s",
         format_hex(BPTR(src), BLEN(src), 80, &gc));

    /* Get packet ID */
    if (!packet_id_write(&opt->packet_id.send, dst, true, false))
    {
        msg(D_CRYPT_ERRORS, "TLS-CRYPT ERROR: packet ID roll over.");
        goto err;
    }

    dmsg(D_PACKET_CONTENT, "TLS-CRYPT WRAP AD: %s",
         format_hex(BPTR(dst), BLEN(dst), 0, &gc));

    /* Buffer overflow check */
    if (!buf_safe(dst, BLEN(src) + TLS_CRYPT_BLOCK_SIZE + TLS_CRYPT_TAG_SIZE))
    {
        msg(D_CRYPT_ERRORS, "TLS-CRYPT WRAP: buffer size error, "
            "sc=%d so=%d sl=%d dc=%d do=%d dl=%d", src->capacity, src->offset,
            src->len, dst->capacity, dst->offset, dst->len);
        goto err;
    }

    /* Calculate auth tag and synthetic IV */
    {
        uint8_t *tag = NULL;
        hmac_ctx_reset(ctx->hmac);
        hmac_ctx_update(ctx->hmac, BPTR(dst), BLEN(dst));
        hmac_ctx_update(ctx->hmac, BPTR(src), BLEN(src));

        ASSERT(tag = buf_write_alloc(dst, TLS_CRYPT_TAG_SIZE));
        hmac_ctx_final(ctx->hmac, tag);

        dmsg(D_PACKET_CONTENT, "TLS-CRYPT WRAP TAG: %s",
             format_hex(tag, TLS_CRYPT_TAG_SIZE, 0, &gc));

        /* Use the 128 most significant bits of the tag as IV */
        ASSERT(cipher_ctx_reset(ctx->cipher, tag));
    }

    /* Encrypt src */
    {
        int outlen = 0;
        ASSERT(cipher_ctx_update(ctx->cipher, BEND(dst), &outlen,
                                 BPTR(src), BLEN(src)));
        ASSERT(buf_inc_len(dst, outlen));
        ASSERT(cipher_ctx_final(ctx->cipher, BPTR(dst), &outlen));
        ASSERT(buf_inc_len(dst, outlen));
    }

    dmsg(D_PACKET_CONTENT, "TLS-CRYPT WRAP TO: %s",
         format_hex(BPTR(dst), BLEN(dst), 80, &gc));

    gc_free(&gc);
    return true;

err:
    crypto_clear_error();
    dst->len = 0;
    gc_free(&gc);
    return false;
}

bool
tls_crypt_unwrap(const struct buffer *src, struct buffer *dst,
                 struct crypto_options *opt)
{
    static const char error_prefix[] = "tls-crypt unwrap error";
    const struct key_ctx *ctx = &opt->key_ctx_bi.decrypt;
    struct gc_arena gc;

    gc_init(&gc);

    ASSERT(opt);
    ASSERT(src->len > 0);
    ASSERT(ctx->cipher);
    ASSERT(packet_id_initialized(&opt->packet_id)
           || (opt->flags & CO_IGNORE_PACKET_ID));

    dmsg(D_PACKET_CONTENT, "TLS-CRYPT UNWRAP FROM: %s",
         format_hex(BPTR(src), BLEN(src), 80, &gc));

    if (buf_len(src) < TLS_CRYPT_OFF_CT)
    {
        CRYPT_ERROR("packet too short");
    }

    /* Decrypt cipher text */
    {
        int outlen = 0;

        /* Buffer overflow check (should never fail) */
        if (!buf_safe(dst, BLEN(src) - TLS_CRYPT_OFF_CT + TLS_CRYPT_BLOCK_SIZE))
        {
            CRYPT_ERROR("potential buffer overflow");
        }

        if (!cipher_ctx_reset(ctx->cipher, BPTR(src) + TLS_CRYPT_OFF_TAG))
        {
            CRYPT_ERROR("cipher reset failed");
        }
        if (!cipher_ctx_update(ctx->cipher, BPTR(dst), &outlen,
                               BPTR(src) + TLS_CRYPT_OFF_CT, BLEN(src) - TLS_CRYPT_OFF_CT))
        {
            CRYPT_ERROR("cipher update failed");
        }
        ASSERT(buf_inc_len(dst, outlen));
        if (!cipher_ctx_final(ctx->cipher, BPTR(dst), &outlen))
        {
            CRYPT_ERROR("cipher final failed");
        }
        ASSERT(buf_inc_len(dst, outlen));
    }

    /* Check authentication */
    {
        const uint8_t *tag = BPTR(src) + TLS_CRYPT_OFF_TAG;
        uint8_t tag_check[TLS_CRYPT_TAG_SIZE] = { 0 };

        dmsg(D_PACKET_CONTENT, "TLS-CRYPT UNWRAP AD: %s",
             format_hex(BPTR(src), TLS_CRYPT_OFF_TAG, 0, &gc));
        dmsg(D_PACKET_CONTENT, "TLS-CRYPT UNWRAP TO: %s",
             format_hex(BPTR(dst), BLEN(dst), 80, &gc));

        hmac_ctx_reset(ctx->hmac);
        hmac_ctx_update(ctx->hmac, BPTR(src), TLS_CRYPT_OFF_TAG);
        hmac_ctx_update(ctx->hmac, BPTR(dst), BLEN(dst));
        hmac_ctx_final(ctx->hmac, tag_check);

        if (memcmp_constant_time(tag, tag_check, sizeof(tag_check)))
        {
            dmsg(D_CRYPTO_DEBUG, "tag      : %s",
                 format_hex(tag, sizeof(tag_check), 0, &gc));
            dmsg(D_CRYPTO_DEBUG, "tag_check: %s",
                 format_hex(tag_check, sizeof(tag_check), 0, &gc));
            CRYPT_ERROR("packet authentication failed");
        }
    }

    /* Check replay */
    if (!(opt->flags & CO_IGNORE_PACKET_ID))
    {
        struct packet_id_net pin;
        struct buffer tmp = *src;
        ASSERT(buf_advance(&tmp, TLS_CRYPT_OFF_PID));
        ASSERT(packet_id_read(&pin, &tmp, true));
        if (!crypto_check_replay(opt, &pin, error_prefix, &gc))
        {
            CRYPT_ERROR("packet replay");
        }
    }

    gc_free(&gc);
    return true;

error_exit:
    crypto_clear_error();
    dst->len = 0;
    gc_free(&gc);
    return false;
}

static inline void
tls_crypt_v2_load_client_key(struct key_ctx_bi *key, const struct key2 *key2,
                             bool tls_server)
{
    const int key_direction = tls_server ?
                              KEY_DIRECTION_NORMAL : KEY_DIRECTION_INVERSE;
    struct key_type kt = tls_crypt_kt();
    if (!kt.cipher || !kt.digest)
    {
        msg(M_FATAL, "ERROR: --tls-crypt-v2 not supported");
    }
    init_key_ctx_bi(key, key2, key_direction, &kt,
                    "Control Channel Encryption");
}

void
tls_crypt_v2_init_client_key(struct key_ctx_bi *key, struct buffer *wkc_buf,
                             const char *key_file, bool key_inline)
{
    struct buffer client_key = alloc_buf(TLS_CRYPT_V2_CLIENT_KEY_LEN
                                         + TLS_CRYPT_V2_MAX_WKC_LEN);

    if (!read_pem_key_file(&client_key, tls_crypt_v2_cli_pem_name,
                           key_file, key_inline))
    {
        msg(M_FATAL, "ERROR: invalid tls-crypt-v2 client key format");
    }

    struct key2 key2;
    if (!buf_read(&client_key, &key2.keys, sizeof(key2.keys)))
    {
        msg(M_FATAL, "ERROR: not enough data in tls-crypt-v2 client key");
    }

    tls_crypt_v2_load_client_key(key, &key2, false);
    secure_memzero(&key2, sizeof(key2));

    *wkc_buf = client_key;
}

void
tls_crypt_v2_init_server_key(struct key_ctx *key_ctx, bool encrypt,
                             const char *key_file, bool key_inline)
{
    struct key srv_key;
    struct buffer srv_key_buf;

    buf_set_write(&srv_key_buf, (void *)&srv_key, sizeof(srv_key));
    if (!read_pem_key_file(&srv_key_buf, tls_crypt_v2_srv_pem_name,
                           key_file, key_inline))
    {
        msg(M_FATAL, "ERROR: invalid tls-crypt-v2 server key format");
    }

    struct key_type kt = tls_crypt_kt();
    if (!kt.cipher || !kt.digest)
    {
        msg(M_FATAL, "ERROR: --tls-crypt-v2 not supported");
    }
    init_key_ctx(key_ctx, &srv_key, &kt, encrypt, "tls-crypt-v2 server key");
    secure_memzero(&srv_key, sizeof(srv_key));
}

static bool
tls_crypt_v2_wrap_client_key(struct buffer *wkc,
                             const struct key2 *src_key,
                             const struct buffer *src_metadata,
                             struct key_ctx *server_key, struct gc_arena *gc)
{
    cipher_ctx_t *cipher_ctx = server_key->cipher;
    struct buffer work = alloc_buf_gc(TLS_CRYPT_V2_MAX_WKC_LEN
                                      + cipher_ctx_block_size(cipher_ctx), gc);

    /* Calculate auth tag and synthetic IV */
    uint8_t *tag = buf_write_alloc(&work, TLS_CRYPT_TAG_SIZE);
    if (!tag)
    {
        msg(M_WARN, "ERROR: could not write tag");
        return false;
    }
    uint16_t net_len = htons(sizeof(src_key->keys) + BLEN(src_metadata)
                             + TLS_CRYPT_V2_TAG_SIZE + sizeof(uint16_t));
    hmac_ctx_t *hmac_ctx = server_key->hmac;
    hmac_ctx_reset(hmac_ctx);
    hmac_ctx_update(hmac_ctx, (void *)&net_len, sizeof(net_len));
    hmac_ctx_update(hmac_ctx, (void *)src_key->keys, sizeof(src_key->keys));
    hmac_ctx_update(hmac_ctx, BPTR(src_metadata), BLEN(src_metadata));
    hmac_ctx_final(hmac_ctx, tag);

    dmsg(D_CRYPTO_DEBUG, "TLS-CRYPT WRAP TAG: %s",
         format_hex(tag, TLS_CRYPT_TAG_SIZE, 0, gc));

    /* Use the 128 most significant bits of the tag as IV */
    ASSERT(cipher_ctx_reset(cipher_ctx, tag));

    /* Overflow check (OpenSSL requires an extra block in the dst buffer) */
    if (buf_forward_capacity(&work) < (sizeof(src_key->keys)
                                       + BLEN(src_metadata)
                                       + sizeof(net_len)
                                       + cipher_ctx_block_size(cipher_ctx)))
    {
        msg(M_WARN, "ERROR: could not crypt: insufficient space in dst");
        return false;
    }

    /* Encrypt */
    int outlen = 0;
    ASSERT(cipher_ctx_update(cipher_ctx, BEND(&work), &outlen,
                             (void *)src_key->keys, sizeof(src_key->keys)));
    ASSERT(buf_inc_len(&work, outlen));
    ASSERT(cipher_ctx_update(cipher_ctx, BEND(&work), &outlen,
                             BPTR(src_metadata), BLEN(src_metadata)));
    ASSERT(buf_inc_len(&work, outlen));
    ASSERT(cipher_ctx_final(cipher_ctx, BEND(&work), &outlen));
    ASSERT(buf_inc_len(&work, outlen));
    ASSERT(buf_write(&work, &net_len, sizeof(net_len)));

    return buf_copy(wkc, &work);
}

static bool
tls_crypt_v2_plugin_wrap_client_key(struct buffer *wkc, const struct key2 *src_key, const struct buffer *src_metadata,
                                    const struct plugin_list *plugins, struct env_set *es, struct gc_arena *gc)
{
    bool ret = false;
    int data_len = TLS_CRYPT_V2_CLIENT_KEY_LEN + BLEN(src_metadata);
    char data[data_len];
    memcpy(data, (uint8_t *) src_key->keys, TLS_CRYPT_V2_CLIENT_KEY_LEN);
    memcpy(data + TLS_CRYPT_V2_CLIENT_KEY_LEN, BPTR(src_metadata), BLEN(src_metadata));

    // Prepare unwrapped key for plugin
    struct argv av = argv_new();
    char *b64_key = NULL;
    ASSERT(openvpn_base64_encode(data, data_len, &b64_key) >= 0);
    ASSERT(argv_printf(&av, "%s %s", "wrap", b64_key) == true);

    // Prepare response structure
    struct plugin_return pr;
    plugin_return_init(&pr);

    // Call the plugin
    int plug_ret = plugin_call(plugins, OPENVPN_PLUGIN_CLIENT_KEY_WRAPPING, &av, &pr, es);
    ASSERT(plug_ret == OPENVPN_PLUGIN_FUNC_SUCCESS);
    free(b64_key);

    // Handle return
    struct plugin_return wrapped_return;
    plugin_return_get_column(&pr, &wrapped_return, "wrapping result");
    ASSERT(plugin_return_defined(&wrapped_return));

    for (int i = 0; i < wrapped_return.n; ++i)
    {
        if (wrapped_return.list[i] && wrapped_return.list[i]->value)
        {
            char *b64_return = wrapped_return.list[i]->value;
            char wrapped_data[TLS_CRYPT_V2_MAX_WKC_LEN];
            int wrapped_len = openvpn_base64_decode(b64_return, wrapped_data, (int) strlen(b64_return));
            ASSERT(wrapped_len <= TLS_CRYPT_V2_MAX_WKC_LEN);
            ASSERT(buf_write(wkc, wrapped_data, wrapped_len));
            ret = true;
            break;
        }
    }
    argv_free(&av);
    plugin_return_free(&pr);
    return ret;
}

static bool
tls_crypt_v2_plugin_unwrap_client_key(struct key2 *client_key, struct buffer *metadata,
                                      struct buffer wrapped_client_key,
                                      const struct plugin_list *plugins, struct env_set *es)
{
    const char *error_prefix = __func__;
    bool ret = false;
    struct gc_arena gc = gc_new();

    uint8_t plaintext_data[TLS_CRYPT_V2_MAX_WKC_LEN];

    dmsg(D_TLS_DEBUG_MED, "%s: unwrapping client key (len=%d): %s", __func__,
         BLEN(&wrapped_client_key), format_hex(BPTR(&wrapped_client_key),
                                               BLEN(&wrapped_client_key),
                                               0, &gc));

    if (TLS_CRYPT_V2_MAX_WKC_LEN < BLEN(&wrapped_client_key))
    {
        CRYPT_ERROR("wrapped client key too big");
    }

    // Prepare unwrapped key for plugin
    struct argv av = argv_new();
    char *b64_key;
    ASSERT(openvpn_base64_encode(BPTR(&wrapped_client_key), BLEN(&wrapped_client_key), &b64_key) >= 0);
    ASSERT(argv_printf(&av, "%s %s", "unwrap", b64_key) == true);
    free(b64_key);

    // Prepare response structure
    struct plugin_return pr;
    plugin_return_init(&pr);

    // Call the plugin
    int plug_ret = plugin_call(plugins, OPENVPN_PLUGIN_CLIENT_KEY_WRAPPING, &av, &pr, es);
    if (plug_ret != OPENVPN_PLUGIN_FUNC_SUCCESS)
    {
        CRYPT_ERROR("unwrap client key plugin failed");
    }

    // Handle return
    struct plugin_return unwrapped_return;
    plugin_return_get_column(&pr, &unwrapped_return, "wrapping result");
    if (!plugin_return_defined(&unwrapped_return))
    {
        CRYPT_ERROR("plugin send no unwrapped data");
    }

    for (int i = 0; i < unwrapped_return.n; ++i)
    {
        if (unwrapped_return.list[i] && unwrapped_return.list[i]->value)
        {
            char *b64_return = unwrapped_return.list[i]->value;
            int b64_len = (int) strlen(b64_return);
            int unwrapped_len = openvpn_base64_decode(b64_return, plaintext_data, b64_len);
            int expected_key_len = sizeof(client_key->keys);
            if (unwrapped_len < expected_key_len)
            {
                CRYPT_ERROR("failed to read client key");
            }
            memcpy(&client_key->keys, plaintext_data, expected_key_len);
            if (!buf_write(metadata, plaintext_data + expected_key_len, unwrapped_len - expected_key_len))
            {
                CRYPT_ERROR("metadata too large for supplied buffer");
            }
            ret = true;
            break;
        }
    }
error_exit:
    if (!ret)
    {
        secure_memzero(client_key, sizeof(*client_key));
    }
    secure_memzero(plaintext_data, TLS_CRYPT_V2_MAX_WKC_LEN);
    plugin_return_free(&pr);
    gc_free(&gc);
    argv_free(&av);
    return ret;
}

static bool
tls_crypt_v2_local_unwrap_client_key(struct key2 *client_key, struct buffer *metadata,
                                     struct buffer wrapped_client_key,
                                     struct key_ctx *server_key)
{
    const char *error_prefix = __func__;
    bool ret = false;
    struct gc_arena gc = gc_new();
    /* The crypto API requires one extra cipher block of buffer head room when
     * decrypting, which nicely matches the tag size of WKc.  So
     * TLS_CRYPT_V2_MAX_WKC_LEN is always large enough for the plaintext. */
    uint8_t plaintext_buf_data[TLS_CRYPT_V2_MAX_WKC_LEN] = { 0 };
    struct buffer plaintext = { 0 };

    dmsg(D_TLS_DEBUG_MED, "%s: unwrapping client key (len=%d): %s", __func__,
         BLEN(&wrapped_client_key), format_hex(BPTR(&wrapped_client_key),
                                               BLEN(&wrapped_client_key),
                                               0, &gc));

    if (TLS_CRYPT_V2_MAX_WKC_LEN < BLEN(&wrapped_client_key))
    {
        CRYPT_ERROR("wrapped client key too big");
    }

    /* Decrypt client key and metadata */
    uint16_t net_len = 0;
    const uint8_t *tag = BPTR(&wrapped_client_key);

    if (BLEN(&wrapped_client_key) < sizeof(net_len))
    {
        CRYPT_ERROR("failed to read length");
    }
    memcpy(&net_len, BEND(&wrapped_client_key) - sizeof(net_len),
           sizeof(net_len));

    if (ntohs(net_len) != BLEN(&wrapped_client_key))
    {
        dmsg(D_TLS_DEBUG_LOW, "%s: net_len=%u, BLEN=%i", __func__,
             ntohs(net_len), BLEN(&wrapped_client_key));
        CRYPT_ERROR("invalid length");
    }

    buf_inc_len(&wrapped_client_key, -(int)sizeof(net_len));

    if (!buf_advance(&wrapped_client_key, TLS_CRYPT_TAG_SIZE))
    {
        CRYPT_ERROR("failed to read tag");
    }

    if (!cipher_ctx_reset(server_key->cipher, tag))
    {
        CRYPT_ERROR("failed to initialize IV");
    }
    buf_set_write(&plaintext, plaintext_buf_data, sizeof(plaintext_buf_data));
    int outlen = 0;
    if (!cipher_ctx_update(server_key->cipher, BPTR(&plaintext), &outlen,
                           BPTR(&wrapped_client_key),
                           BLEN(&wrapped_client_key)))
    {
        CRYPT_ERROR("could not decrypt client key");
    }
    ASSERT(buf_inc_len(&plaintext, outlen));

    if (!cipher_ctx_final(server_key->cipher, BEND(&plaintext), &outlen))
    {
        CRYPT_ERROR("cipher final failed");
    }
    ASSERT(buf_inc_len(&plaintext, outlen));

    /* Check authentication */
    uint8_t tag_check[TLS_CRYPT_TAG_SIZE] = { 0 };
    hmac_ctx_reset(server_key->hmac);
    hmac_ctx_update(server_key->hmac, (void *)&net_len, sizeof(net_len));
    hmac_ctx_update(server_key->hmac, BPTR(&plaintext),
                    BLEN(&plaintext));
    hmac_ctx_final(server_key->hmac, tag_check);

    if (memcmp_constant_time(tag, tag_check, sizeof(tag_check)))
    {
        dmsg(D_CRYPTO_DEBUG, "tag      : %s",
             format_hex(tag, sizeof(tag_check), 0, &gc));
        dmsg(D_CRYPTO_DEBUG, "tag_check: %s",
             format_hex(tag_check, sizeof(tag_check), 0, &gc));
        CRYPT_ERROR("client key authentication error");
    }

    if (buf_len(&plaintext) < sizeof(client_key->keys))
    {
        CRYPT_ERROR("failed to read client key");
    }
    memcpy(&client_key->keys, BPTR(&plaintext), sizeof(client_key->keys));
    ASSERT(buf_advance(&plaintext, sizeof(client_key->keys)));

    if (!buf_copy(metadata, &plaintext))
    {
        CRYPT_ERROR("metadata too large for supplied buffer");
    }

    ret = true;
error_exit:
    if (!ret)
    {
        secure_memzero(client_key, sizeof(*client_key));
    }
    buf_clear(&plaintext);
    gc_free(&gc);
    return ret;
}

static bool
tls_crypt_v2_unwrap_client_key(struct key2 *client_key, struct buffer *metadata,
                               struct buffer wrapped_client_key, struct key_ctx *server_key,
                               const struct plugin_list *plugins, struct env_set *es)
{
    if (plugin_defined(plugins, OPENVPN_PLUGIN_CLIENT_KEY_WRAPPING) & !server_key->cipher)
    {
        return tls_crypt_v2_plugin_unwrap_client_key(client_key, metadata, wrapped_client_key, plugins, es);

    }
    else
    {
        return tls_crypt_v2_local_unwrap_client_key(client_key, metadata, wrapped_client_key, server_key);
    }
}

static bool
tls_crypt_v2_verify_metadata(const struct tls_wrap_ctx *ctx,
                             const struct tls_options *opt)
{
    bool ret = false;
    struct gc_arena gc = gc_new();
    const char *tmp_file = NULL;
    struct buffer metadata = ctx->tls_crypt_v2_metadata;
    int metadata_type = buf_read_u8(&metadata);
    if (metadata_type < 0)
    {
        msg(M_WARN, "ERROR: no metadata type");
        goto cleanup;
    }

    tmp_file = platform_create_temp_file(opt->tmp_dir, "tls_crypt_v2_metadata_",
                                         &gc);
    if (!tmp_file || !buffer_write_file(tmp_file, &metadata))
    {
        msg(M_WARN, "ERROR: could not write metadata to file");
        goto cleanup;
    }

    char metadata_type_str[4] = { 0 }; /* Max value: 255 */
    openvpn_snprintf(metadata_type_str, sizeof(metadata_type_str),
                     "%i", metadata_type);
    struct env_set *es = env_set_create(NULL);
    setenv_str(es, "script_type", "tls-crypt-v2-verify");
    setenv_str(es, "metadata_type", metadata_type_str);
    setenv_str(es, "metadata_file", tmp_file);

    struct argv argv = argv_new();
    argv_parse_cmd(&argv, opt->tls_crypt_v2_verify_script);
    argv_msg_prefix(D_TLS_DEBUG, &argv, "Executing tls-crypt-v2-verify");

    ret = openvpn_run_script(&argv, es, 0, "--tls-crypt-v2-verify");

    argv_free(&argv);
    env_set_destroy(es);

    if (!platform_unlink(tmp_file))
    {
        msg(M_WARN, "WARNING: failed to remove temp file '%s", tmp_file);
    }

    if (ret)
    {
        msg(D_HANDSHAKE, "TLS CRYPT V2 VERIFY SCRIPT OK");
    }
    else
    {
        msg(D_HANDSHAKE, "TLS CRYPT V2 VERIFY SCRIPT ERROR");
    }

cleanup:
    gc_free(&gc);
    return ret;
}

bool
tls_crypt_v2_extract_client_key(struct buffer *buf,
                                struct tls_wrap_ctx *ctx,
                                const struct tls_options *opt)
{
    if (!ctx->tls_crypt_v2_server_key.cipher && !plugin_defined(opt->plugins, OPENVPN_PLUGIN_CLIENT_KEY_WRAPPING))
    {
        msg(D_TLS_ERRORS,
            "Client wants tls-crypt-v2, but no server key present.");
        return false;
    }

    msg(D_HANDSHAKE, "Control Channel: using tls-crypt-v2 key");

    struct buffer wrapped_client_key = *buf;
    uint16_t net_len = 0;

    if (BLEN(&wrapped_client_key) < sizeof(net_len))
    {
        msg(D_TLS_ERRORS, "Can not read tls-crypt-v2 client key length");
        return false;
    }
    memcpy(&net_len, BEND(&wrapped_client_key) - sizeof(net_len),
           sizeof(net_len));

    size_t wkc_len = ntohs(net_len);
    if (!buf_advance(&wrapped_client_key, BLEN(&wrapped_client_key) - wkc_len))
    {
        msg(D_TLS_ERRORS, "Can not locate tls-crypt-v2 client key");
        return false;
    }

    struct key2 client_key = { 0 };
    ctx->tls_crypt_v2_metadata = alloc_buf(TLS_CRYPT_V2_MAX_METADATA_LEN);
    if (!tls_crypt_v2_unwrap_client_key(&client_key,
                                        &ctx->tls_crypt_v2_metadata,
                                        wrapped_client_key,
                                        &ctx->tls_crypt_v2_server_key,
                                        opt->plugins, opt->es))
    {
        msg(D_TLS_ERRORS, "Can not unwrap tls-crypt-v2 client key");
        secure_memzero(&client_key, sizeof(client_key));
        return false;
    }

    /* Load the decrypted key */
    ctx->mode = TLS_WRAP_CRYPT;
    ctx->cleanup_key_ctx = true;
    ctx->opt.flags |= CO_PACKET_ID_LONG_FORM;
    memset(&ctx->opt.key_ctx_bi, 0, sizeof(ctx->opt.key_ctx_bi));
    tls_crypt_v2_load_client_key(&ctx->opt.key_ctx_bi, &client_key, true);
    secure_memzero(&client_key, sizeof(client_key));

    /* Remove client key from buffer so tls-crypt code can unwrap message */
    ASSERT(buf_inc_len(buf, -(BLEN(&wrapped_client_key))));

    if (opt && opt->tls_crypt_v2_verify_script)
    {
        return tls_crypt_v2_verify_metadata(ctx, opt);
    }

    return true;
}

void
tls_crypt_v2_write_server_key_file(const char *filename)
{
    write_pem_key_file(filename, tls_crypt_v2_srv_pem_name);
}

void
tls_crypt_v2_send_plugin_server_key(const char *filename, const struct plugin_list *plugins, struct env_set *es)
{
    struct argv av = argv_new();
    struct key server_key = { 0 };
    struct key_type server_kt = tls_crypt_kt();
    ASSERT(server_kt.cipher && server_kt.digest);
    ASSERT(rand_bytes((void *)&server_key, sizeof(server_key)));
    char *b64_aes_key = NULL;
    char *b64_hmac_key = NULL;
    ASSERT(openvpn_base64_encode(server_key.cipher, sizeof(server_key.cipher), &b64_aes_key) >= sizeof(server_key.cipher));
    ASSERT(openvpn_base64_encode(server_key.hmac, sizeof(server_key.hmac), &b64_hmac_key) >= sizeof(server_key.hmac));
    ASSERT(argv_printf(&av, "%s %s %s", "import", b64_aes_key, b64_hmac_key));

    ASSERT(plugin_call(plugins, OPENVPN_PLUGIN_CLIENT_KEY_WRAPPING, &av, NULL, es) == OPENVPN_PLUGIN_FUNC_SUCCESS);

    if (filename && strcmp(filename, "") != 0)
    {
        struct gc_arena gc = gc_new();
        struct buffer server_key_buf = clear_buf();
        struct buffer server_key_pem = clear_buf();

        buf_set_read(&server_key_buf, (void *)&server_key, sizeof(server_key));
        ASSERT(crypto_pem_encode(tls_crypt_v2_srv_pem_name, &server_key_pem,
                               &server_key_buf, &gc));

        ASSERT(buffer_write_file(filename, &server_key_pem));
    }
    secure_memzero(b64_aes_key, strlen(b64_aes_key));
    free(b64_aes_key);
    secure_memzero(b64_hmac_key, strlen(b64_hmac_key));
    free(b64_hmac_key);

    // Securely erase argv
    if (av.argc)
    {
        for (size_t i = 0; i < av.argc; ++i)
        {
            string_clear(av.argv[i]);
        }
    }
    argv_free(&av);
}

void
tls_crypt_v2_write_client_key_file(const char *filename, const char *b64_metadata,
                                   const char *server_key_file, bool server_key_inline,
                                   const struct plugin_list *plugins, struct env_set *es)
{
    struct gc_arena gc = gc_new();
    struct key_ctx server_key = { 0 };
    struct buffer client_key_pem = { 0 };
    struct buffer dst = alloc_buf_gc(TLS_CRYPT_V2_CLIENT_KEY_LEN
                                     + TLS_CRYPT_V2_MAX_WKC_LEN, &gc);
    struct key2 client_key = { 2 };

    if (!rand_bytes((void *)client_key.keys, sizeof(client_key.keys)))
    {
        msg(M_FATAL, "ERROR: could not generate random key");
        goto cleanup;
    }
    ASSERT(buf_write(&dst, client_key.keys, sizeof(client_key.keys)));

    struct buffer metadata;
    if (b64_metadata)
    {
        size_t b64_length = strlen(b64_metadata);
        metadata = alloc_buf_gc(OPENVPN_BASE64_DECODED_LENGTH(b64_length) + 1, &gc);
        ASSERT(buf_write(&metadata, &TLS_CRYPT_METADATA_TYPE_USER, 1));
        int decoded_len = openvpn_base64_decode(b64_metadata, BEND(&metadata),
                                                BCAP(&metadata));
        if (decoded_len < 0)
        {
            msg(M_FATAL, "ERROR: failed to base64 decode provided metadata");
            goto cleanup;
        }
        if (decoded_len > TLS_CRYPT_V2_MAX_METADATA_LEN - 1)
        {
            msg(M_FATAL,
                "ERROR: metadata too long (%d bytes, max %u bytes)",
                decoded_len, TLS_CRYPT_V2_MAX_METADATA_LEN - 1);
            goto cleanup;
        }
        ASSERT(buf_inc_len(&metadata, decoded_len));
    }
    else
    {
        metadata = alloc_buf_gc(1 + sizeof(int64_t), &gc);
        int64_t timestamp = htonll((uint64_t)now);
        ASSERT(buf_write(&metadata, &TLS_CRYPT_METADATA_TYPE_TIMESTAMP, 1));
        ASSERT(buf_write(&metadata, &timestamp, sizeof(timestamp)));
    }
    if (plugin_defined(plugins, OPENVPN_PLUGIN_CLIENT_KEY_WRAPPING) && !server_key_file) {
        tls_crypt_v2_plugin_wrap_client_key(&dst, &client_key, &metadata, plugins, es, NULL);
    } 
    else
    {
        tls_crypt_v2_init_server_key(&server_key, true, server_key_file,
                                     server_key_inline);
        if (!tls_crypt_v2_wrap_client_key(&dst, &client_key, &metadata, &server_key,
                                          &gc))
        {
            msg(M_FATAL, "ERROR: could not wrap generated client key");
            goto cleanup;
        }
    }

    /* PEM-encode Kc || WKc */
    if (!crypto_pem_encode(tls_crypt_v2_cli_pem_name, &client_key_pem, &dst,
                           &gc))
    {
        msg(M_FATAL, "ERROR: could not PEM-encode client key");
        goto cleanup;
    }

    const char *client_file = filename;
    bool client_inline = false;

    if (!filename || streq(filename, ""))
    {
        printf("%.*s\n", BLEN(&client_key_pem), BPTR(&client_key_pem));
        client_file = (const char *)BPTR(&client_key_pem);
        client_inline = true;
    }
    else if (!buffer_write_file(filename, &client_key_pem))
    {
        msg(M_FATAL, "ERROR: could not write client key file");
        goto cleanup;
    }

    /* Sanity check: load client key (as "client") */
    struct key_ctx_bi test_client_key;
    struct buffer test_wrapped_client_key;
    msg(D_GENKEY, "Testing client-side key loading...");
    tls_crypt_v2_init_client_key(&test_client_key, &test_wrapped_client_key,
                                 client_file, client_inline);
    free_key_ctx_bi(&test_client_key);

    /* Sanity check: unwrap and load client key (as "server") */
    msg(D_GENKEY, "Testing server-side key loading...");
    struct buffer test_metadata = alloc_buf_gc(TLS_CRYPT_V2_MAX_METADATA_LEN,
                                               &gc);
    struct key2 test_client_key2 = { 0 };
    if (!plugin_defined(plugins, OPENVPN_PLUGIN_CLIENT_KEY_WRAPPING) && !server_key_file)
    {
        free_key_ctx(&server_key);
        tls_crypt_v2_init_server_key(&server_key, false, server_key_file,
                                    server_key_inline);
    }
    ASSERT(tls_crypt_v2_unwrap_client_key(&test_client_key2, &test_metadata,
                                          test_wrapped_client_key, &server_key, plugins, es));
    secure_memzero(&test_client_key2, sizeof(test_client_key2));
    free_buf(&test_wrapped_client_key);

cleanup:
    secure_memzero(&client_key, sizeof(client_key));
    free_key_ctx(&server_key);
    buf_clear(&client_key_pem);
    buf_clear(&dst);

    gc_free(&gc);
}
