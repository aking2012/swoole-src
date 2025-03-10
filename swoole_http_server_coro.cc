/*
  +----------------------------------------------------------------------+
  | Swoole                                                               |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.0 of the Apache license,    |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.apache.org/licenses/LICENSE-2.0.html                      |
  | If you did not receive a copy of the Apache2.0 license and are unable|
  | to obtain it through the world-wide-web, please send a note to       |
  | license@swoole.com so we can mail you a copy immediately.            |
  +----------------------------------------------------------------------+
  | Author: Tianfeng Han  <mikan.tenny@gmail.com>                        |
  +----------------------------------------------------------------------+
*/

#include "swoole_http_server.h"

#include "http.h"
#ifdef SW_USE_HTTP2
#include "http2.h"
#endif

#include <string>
#include <map>
#include <algorithm>

using namespace std;
using namespace swoole;
using swoole::coroutine::Socket;
using swoole::coroutine::System;

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_void, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_http_server_coro_construct, 0, 0, 1)
    ZEND_ARG_INFO(0, host)
    ZEND_ARG_INFO(0, port)
    ZEND_ARG_INFO(0, ssl)
    ZEND_ARG_INFO(0, reuse_port)
ZEND_END_ARG_INFO()


ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_http_server_coro_handle, 0, 0, 2)
    ZEND_ARG_INFO(0, pattern)
    ZEND_ARG_CALLABLE_INFO(0, callback, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_swoole_http_server_coro_set, 0, 0, 1)
    ZEND_ARG_ARRAY_INFO(0, settings, 0)
ZEND_END_ARG_INFO()

static zend_class_entry *swoole_http_server_coro_ce;
static zend_object_handlers swoole_http_server_coro_handlers;

static bool http_context_send_data(http_context* ctx, const char *data, size_t length);
static bool http_context_sendfile(http_context* ctx, const char *file, uint32_t l_file, off_t offset, size_t length);
static bool http_context_disconnect(http_context* ctx);

#ifdef SW_USE_HTTP2
static void http2_server_onRequest(http2_session *session, http2_stream *stream);
#endif

class http_server
{
public:
    Socket *socket;
    map<string, php_swoole_fci *> handlers;
    php_swoole_fci *default_handler;
    bool running;
    std::list<Socket *> receivers;

    /* options */
#ifdef SW_HAVE_ZLIB
    bool websocket_compression;
#endif

    http_server(enum swSocket_type type)
    {
        socket = new Socket(type);
        default_handler = nullptr;
        running = true;

#ifdef SW_HAVE_ZLIB
        websocket_compression = false;
#endif
    }

    void set_handler(string pattern, php_swoole_fci *fci)
    {
        if (pattern == "/")
        {
            if (default_handler)
            {
                sw_zend_fci_cache_discard(&default_handler->fci_cache);
                efree(default_handler);
            }
            default_handler = fci;
        }
        else
        {
            auto find_fci = handlers.find(pattern);
            if (find_fci != handlers.end())
            {
                sw_zend_fci_cache_discard(&find_fci->second->fci_cache);
                efree(find_fci->second);
            }
            handlers[pattern] = fci;
        }
        sw_zend_fci_cache_persist(&fci->fci_cache);
    }

    php_swoole_fci* get_handler(http_context *ctx)
    {
        for (auto i = handlers.begin(); i != handlers.end(); i++)
        {
            if (swoole_strcasect(ctx->request.path, ctx->request.path_len, i->first.c_str(), i->first.length()))
            {
                return i->second;
            }
        }
        return default_handler;
    }

    http_context* create_context(Socket *conn, zval *zconn)
    {
        http_context *ctx = swoole_http_context_new(conn->get_fd());
        ctx->parse_body = 1;
        ctx->parse_cookie = 1;
#ifdef SW_HAVE_COMPRESSION
        ctx->enable_compression = 1;
        ctx->compression_level = SW_Z_BEST_SPEED;
#endif
#ifdef SW_HAVE_ZLIB
        ctx->websocket_compression = websocket_compression;
#endif
        ctx->private_data = conn;
        ctx->co_socket = 1;
        ctx->send = http_context_send_data;
        ctx->sendfile = http_context_sendfile;
        ctx->close = http_context_disconnect;
        ctx->upload_tmp_dir = "/tmp";

        swoole_http_parser *parser = &ctx->parser;
        parser->data = ctx;
        swoole_http_parser_init(parser, PHP_HTTP_REQUEST);

        zend_update_property(swoole_http_response_ce, ctx->response.zobject, ZEND_STRL("socket"), zconn);

        return ctx;
    }

#ifdef SW_USE_HTTP2
    void recv_http2_frame(http_context *ctx)
    {
        Socket *sock = (Socket *) ctx->private_data;
        swHttp2_send_setting_frame(&sock->protocol, sock->socket);

        sock->open_length_check = true;
        sock->protocol.package_length_size = SW_HTTP2_FRAME_HEADER_SIZE;
        sock->protocol.package_length_offset = 0;
        sock->protocol.package_body_offset = 0;
        sock->protocol.get_package_length = swHttp2_get_frame_length;

        http2_session session(ctx->fd);
        session.default_ctx = ctx;
        session.handle = http2_server_onRequest;
        session.private_data = this;

        while (true)
        {
            auto buffer = sock->get_read_buffer();
            ssize_t retval = sock->recv_packet();
            if (sw_unlikely(retval <= 0))
            {
                break;
            }
            swoole_http2_server_parse(&session, buffer->str);
        }

        /* default_ctx does not blong to session object */
        session.default_ctx = nullptr;

        ctx->detached = 1;
        zval_dtor(ctx->request.zobject);
        zval_dtor(ctx->response.zobject);
    }
#endif
};

typedef struct
{
    http_server *server;
    zend_object std;
} http_server_coro_t;

static PHP_METHOD(swoole_http_server_coro, __construct);
static PHP_METHOD(swoole_http_server_coro, set);
static PHP_METHOD(swoole_http_server_coro, handle);
static PHP_METHOD(swoole_http_server_coro, start);
static PHP_METHOD(swoole_http_server_coro, shutdown);
static PHP_METHOD(swoole_http_server_coro, onAccept);
static PHP_METHOD(swoole_http_server_coro, __destruct);

static const zend_function_entry swoole_http_server_coro_methods[] =
{
    PHP_ME(swoole_http_server_coro, __construct, arginfo_swoole_http_server_coro_construct, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_server_coro, __destruct, arginfo_swoole_void, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_server_coro, set, arginfo_swoole_http_server_coro_set, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_server_coro, handle, arginfo_swoole_http_server_coro_handle, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_server_coro, onAccept, arginfo_swoole_void, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_server_coro, start, arginfo_swoole_void, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_server_coro, shutdown, arginfo_swoole_void, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static zend_object *php_swoole_http_server_coro_create_object(zend_class_entry *ce)
{
    http_server_coro_t *hsc = (http_server_coro_t *) ecalloc(1, sizeof(http_server_coro_t) + zend_object_properties_size(ce));
    zend_object_std_init(&hsc->std, ce);
    object_properties_init(&hsc->std, ce);
    hsc->std.handlers = &swoole_http_server_coro_handlers;
    return &hsc->std;
}

static sw_inline http_server_coro_t* php_swoole_http_server_coro_fetch_object(zend_object *obj)
{
    return (http_server_coro_t *) ((char *) obj - swoole_http_server_coro_handlers.offset);
}

static sw_inline http_server* http_server_get_object(zend_object *obj)
{
    return php_swoole_http_server_coro_fetch_object(obj)->server;
}

static inline void http_server_set_error(zval *zobject, Socket *sock)
{
    zend_update_property_long(swoole_http_server_coro_ce, zobject, ZEND_STRL("errCode"), sock->errCode);
    zend_update_property_string(swoole_http_server_coro_ce, zobject, ZEND_STRL("errMsg"), sock->errMsg);
}

static bool http_context_send_data(http_context* ctx, const char *data, size_t length)
{
    Socket *sock = (Socket *) ctx->private_data;
    return sock->send_all(data, length) == (ssize_t) length;
}

static bool http_context_sendfile(http_context* ctx, const char *file, uint32_t l_file, off_t offset, size_t length)
{
    Socket *sock = (Socket *) ctx->private_data;
    return sock->sendfile(file, offset, length);
}

static bool http_context_disconnect(http_context* ctx)
{
    Socket *sock = (Socket *) ctx->private_data;
    return sock->close();
}

static void php_swoole_http_server_coro_free_object(zend_object *object)
{
    http_server_coro_t *hsc = php_swoole_http_server_coro_fetch_object(object);
    if (hsc->server)
    {
        http_server *hs = hsc->server;
        if (hs->default_handler)
        {
            sw_zend_fci_cache_discard(&hs->default_handler->fci_cache);
            efree(hs->default_handler);
        }
        for (auto i = hs->handlers.begin(); i != hs->handlers.end(); i++)
        {
            sw_zend_fci_cache_discard(&i->second->fci_cache);
            efree(i->second);
        }
        delete hs;
    }
    zend_object_std_dtor(&hsc->std);
}

void php_swoole_http_server_coro_minit(int module_number)
{
    SW_INIT_CLASS_ENTRY(swoole_http_server_coro, "Swoole\\Coroutine\\Http\\Server", NULL, "Co\\Http\\Server", swoole_http_server_coro_methods);
    SW_SET_CLASS_SERIALIZABLE(swoole_http_server_coro, zend_class_serialize_deny, zend_class_unserialize_deny);
    SW_SET_CLASS_CLONEABLE(swoole_http_server_coro, sw_zend_class_clone_deny);
    SW_SET_CLASS_UNSET_PROPERTY_HANDLER(swoole_http_server_coro, sw_zend_class_unset_property_deny);
    SW_SET_CLASS_CREATE_WITH_ITS_OWN_HANDLERS(swoole_http_server_coro);
    SW_SET_CLASS_CUSTOM_OBJECT(swoole_http_server_coro, php_swoole_http_server_coro_create_object, php_swoole_http_server_coro_free_object, http_server_coro_t, std);
    swoole_http_server_coro_ce->ce_flags |= ZEND_ACC_FINAL;

    zend_declare_property_long(swoole_http_server_coro_ce, ZEND_STRL("fd"), -1, ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_http_server_coro_ce, ZEND_STRL("host"), ZEND_ACC_PUBLIC);
    zend_declare_property_long(swoole_http_server_coro_ce, ZEND_STRL("port"), -1, ZEND_ACC_PUBLIC);
    zend_declare_property_bool(swoole_http_server_coro_ce, ZEND_STRL("ssl"), 0, ZEND_ACC_PUBLIC);
    zend_declare_property_null(swoole_http_server_coro_ce, ZEND_STRL("settings"), ZEND_ACC_PUBLIC);
    zend_declare_property_long(swoole_http_server_coro_ce, ZEND_STRL("errCode"), 0, ZEND_ACC_PUBLIC);
    zend_declare_property_string(swoole_http_server_coro_ce, ZEND_STRL("errMsg"), "", ZEND_ACC_PUBLIC);
}

static PHP_METHOD(swoole_http_server_coro, __construct)
{
    char *host;
    size_t l_host;
    zend_long port = 0;
    zend_bool ssl = 0;
    zend_bool reuse_port = 0;

    ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 1, 4)
        Z_PARAM_STRING(host, l_host)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(port)
        Z_PARAM_BOOL(ssl)
        Z_PARAM_BOOL(reuse_port)
    ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

    zend_update_property_stringl(swoole_http_server_coro_ce, ZEND_THIS, ZEND_STRL("host"), host, l_host);
    zend_update_property_bool(swoole_http_server_coro_ce, ZEND_THIS, ZEND_STRL("ssl"), ssl);

    // check host
    if (l_host == 0)
    {
        zend_throw_exception_ex(swoole_exception_ce, EINVAL, "host is empty");
        RETURN_FALSE;
    }

    http_server_coro_t *hsc = php_swoole_http_server_coro_fetch_object(Z_OBJ_P(ZEND_THIS));
    string host_str(host, l_host);
    hsc->server = new http_server(Socket::convert_to_type(host_str));
    Socket *sock = hsc->server->socket;

#ifdef SO_REUSEPORT
    if (reuse_port)
    {
        sock->set_option(SOL_SOCKET, SO_REUSEPORT, 1);
    }
#endif
    if (!sock->bind(host_str, port))
    {
        http_server_set_error(ZEND_THIS, sock);
        zend_throw_exception_ex(swoole_exception_ce, sock->errCode, "bind(%s:%d) failed", host, (int) port);
        RETURN_FALSE;
    }
    //check ssl
    if (ssl)
    {
#ifndef SW_USE_OPENSSL
        zend_throw_exception_ex(
            swoole_exception_ce,
            EPROTONOSUPPORT, "you must configure with `--enable-openssl` to support ssl connection when compiling Swoole"
        );
        RETURN_FALSE;
#else
        /* we have to call ssl_check_context after user setProtocols */
        zval *zsettings = sw_zend_read_and_convert_property_array(swoole_http_server_coro_ce, ZEND_THIS, ZEND_STRL("settings"), 0);
        add_assoc_bool(zsettings, "open_ssl", 1);
#endif
    }
    if (!sock->listen())
    {
        http_server_set_error(ZEND_THIS, sock);
        zend_throw_exception_ex(swoole_exception_ce, sock->errCode, "listen() failed");
        RETURN_FALSE;
    }

    zend_update_property_long(swoole_http_server_coro_ce, ZEND_THIS, ZEND_STRL("fd"), sock->get_fd());
    zend_update_property_long(swoole_http_server_coro_ce, ZEND_THIS, ZEND_STRL("port"), sock->get_bind_port());
}

static PHP_METHOD(swoole_http_server_coro, handle)
{
    char *pattern;
    size_t pattern_len;

    http_server *hs = http_server_get_object(Z_OBJ_P(ZEND_THIS));
    php_swoole_fci *fci = (php_swoole_fci *) ecalloc(1, sizeof(php_swoole_fci));

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STRING(pattern, pattern_len)
        Z_PARAM_FUNC(fci->fci, fci->fci_cache)
    ZEND_PARSE_PARAMETERS_END();

    string key(pattern, pattern_len);
    hs->set_handler(key, fci);
}

static PHP_METHOD(swoole_http_server_coro, set)
{
    zval *zset;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ARRAY(zset)
    ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

    if (php_swoole_array_length(zset) == 0)
    {
        RETURN_FALSE;
    }
    else
    {
        zval *zsettings = sw_zend_read_and_convert_property_array(swoole_http_server_coro_ce, ZEND_THIS, ZEND_STRL("settings"), 0);
        php_array_merge(Z_ARRVAL_P(zsettings), Z_ARRVAL_P(zset));
        RETURN_TRUE;
    }
}

static PHP_METHOD(swoole_http_server_coro, start)
{
    http_server *hs = http_server_get_object(Z_OBJ_P(ZEND_THIS));
    Socket *sock = hs->socket;

    /* get callback fci cache */
    char *func_name = NULL;
    zend_fcall_info_cache fci_cache;
    zval zcallback;
    ZVAL_STRING(&zcallback, "onAccept");
    if (!sw_zend_is_callable_ex(&zcallback, ZEND_THIS, 0, &func_name, NULL, &fci_cache, NULL))
    {
        php_swoole_fatal_error(E_CORE_ERROR, "function '%s' is not callable", func_name);
        return;
    }
    efree(func_name);

    /* check settings */
    zval *zsettings = sw_zend_read_and_convert_property_array(swoole_http_server_coro_ce, ZEND_THIS, ZEND_STRL("settings"), 0);
    php_swoole_socket_set_protocol(hs->socket, zsettings);
#ifdef SW_HAVE_ZLIB
    HashTable *vht = Z_ARRVAL_P(zsettings);
    zval *ztmp;
    if (php_swoole_array_get_value(vht, "websocket_compression", ztmp))
    {
        hs->websocket_compression = zval_is_true(ztmp);
    }
#endif

    php_swoole_http_server_init_global_variant();

    while (hs->running)
    {
        auto conn = sock->accept();
        if (conn)
        {
            zval zsocket;
            php_swoole_init_socket_object(&zsocket, conn);
            long cid = PHPCoroutine::create(&fci_cache, 1, &zsocket);
            zval_dtor(&zsocket);
            if (cid < 0)
            {
                goto _wait_1s;
            }
        }
        else
        {
            /*
             * Too many connection, wait 1s
             */
            if (sock->errCode == EMFILE || sock->errCode == ENFILE)
            {
                _wait_1s: System::sleep(SW_ACCEPT_RETRY_TIME);
            }
            else if (sock->errCode == ETIMEDOUT || sock->errCode == SW_ERROR_SSL_BAD_CLIENT)
            {
                continue;
            }
            else if (sock->errCode == ECANCELED)
            {
                http_server_set_error(ZEND_THIS, sock);
                break;
            }
            else
            {
                http_server_set_error(ZEND_THIS, sock);
                php_swoole_fatal_error(E_WARNING, "accept failed, Error: %s[%d]", sock->errMsg, sock->errCode);
                break;
            }
        }
    }

    zval_dtor(&zcallback);

    RETURN_TRUE;
}

static PHP_METHOD(swoole_http_server_coro, __destruct)
{

}

static PHP_METHOD(swoole_http_server_coro, onAccept)
{
    http_server *hs = http_server_get_object(Z_OBJ_P(ZEND_THIS));
    zval *zconn;

    ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 1, 1)
        Z_PARAM_OBJECT(zconn)
    ZEND_PARSE_PARAMETERS_END_EX(RETURN_FALSE);

    Socket *sock = php_swoole_get_socket(zconn);

#ifdef SW_USE_OPENSSL
    if (sock->open_ssl)
    {
        if (!sock->ssl_handshake())
        {
            return;
        }
    }
#endif

    swString* buffer = sock->get_read_buffer();
    size_t total_bytes = 0;
    http_context *ctx = nullptr;

    while (true)
    {
        ssize_t retval;
        if (ctx != nullptr || total_bytes == 0)
        {
            hs->receivers.push_front(sock);
            auto receiver = hs->receivers.begin();
            retval = sock->recv(buffer->str + total_bytes, buffer->size - total_bytes);
            hs->receivers.erase(receiver);

            if (sw_unlikely(retval <= 0))
            {
                break;
            }

            if (!ctx)
            {
                ctx = hs->create_context(sock, zconn);
            }

            if (total_bytes + retval > sock->protocol.package_max_length)
            {
                ctx->response.status = SW_HTTP_REQUEST_ENTITY_TOO_LARGE;
                break;
            }
        }
        else
        {
            /* redundant data from previous packet */
            retval = total_bytes;
            total_bytes = 0;

            if (!ctx)
            {
                ctx = hs->create_context(sock, zconn);
            }
        }

        size_t parsed_n = swoole_http_requset_parse(ctx, buffer->str + total_bytes, retval);
        size_t total_parsed_n = total_bytes + parsed_n;
        total_bytes += retval;

        swTraceLog(SW_TRACE_CO_HTTP_SERVER, "parsed_n=%ld, retval=%ld, total_bytes=%ld, completed=%d", parsed_n, retval, total_bytes, ctx->completed);

        if (!ctx->completed)
        {
            if (ctx->parser.state == s_dead)
            {
                ctx->response.status = SW_HTTP_BAD_REQUEST;
                break;
            }
            if (total_bytes == buffer->size)
            {
                if (swString_extend(buffer, buffer->size * 2) != SW_OK)
                {
                    ctx->response.status = SW_HTTP_SERVICE_UNAVAILABLE;
                    break;
                }
            }
            continue;
        }

#ifdef SW_USE_HTTP2
        if (ctx->parser.method == PHP_HTTP_NOT_IMPLEMENTED
                && memcmp(buffer->str, SW_HTTP2_PRI_STRING, sizeof(SW_HTTP2_PRI_STRING) - 1) == 0)
        {
            buffer->length = total_bytes - (sizeof(SW_HTTP2_PRI_STRING) - 1);
            buffer->offset = buffer->length == 0 ? 0 : (sizeof(SW_HTTP2_PRI_STRING) - 1);
            hs->recv_http2_frame(ctx);
            return;
        }
#endif

        ZVAL_STRINGL(&ctx->request.zdata, buffer->str, total_parsed_n);

        /* handle more packages */
        if ((size_t) retval > parsed_n)
        {
            total_bytes = retval - parsed_n;
            memmove(buffer->str, buffer->str + total_parsed_n, total_bytes);
            if (ctx->websocket)
            {
                /* for recv_packet */
                buffer->length = total_bytes;
            }
        }
        else
        {
            total_bytes = 0;
        }

        zval *zserver = ctx->request.zserver;
        add_assoc_long(zserver, "server_port", hs->socket->get_bind_port());
        add_assoc_long(zserver, "remote_port", (zend_long) sock->get_port());
        add_assoc_string(zserver, "remote_addr", (char *) sock->get_ip());

        php_swoole_fci *fci = hs->get_handler(ctx);
        zval args[2] = {*ctx->request.zobject, *ctx->response.zobject};
        bool keep_alive = swoole_http_should_keep_alive(&ctx->parser) && !ctx->websocket;

        if (fci)
        {
            if (UNEXPECTED(!zend::function::call(&fci->fci_cache, 2, args, NULL, 0)))
            {
                php_swoole_error(E_WARNING, "handler error");
            }
        }
        else
        {
            ctx->response.status = SW_HTTP_NOT_FOUND;
        }

        zval_dtor(&args[0]);
        zval_dtor(&args[1]);
        ctx = nullptr;

        if (!hs->running || !keep_alive)
        {
            break;
        }
    }

    if (ctx)
    {
        zval_dtor(ctx->request.zobject);
        zval_dtor(ctx->response.zobject);
    }
}

static PHP_METHOD(swoole_http_server_coro, shutdown)
{
    http_server *hs = http_server_get_object(Z_OBJ_P(ZEND_THIS));
    hs->running = false;
    hs->socket->cancel(SW_EVENT_READ);
    while (!hs->receivers.empty())
    {
        hs->receivers.back()->close();
    }
}

#ifdef SW_USE_HTTP2
static void http2_server_onRequest(http2_session *session, http2_stream *stream)
{
    http_context *ctx = stream->ctx;
    http_server *hs = (http_server*) session->private_data;
    Socket *sock = (Socket *) ctx->private_data;
    zval *zserver = ctx->request.zserver;

    add_assoc_long(zserver, "request_time", time(NULL));
    add_assoc_double(zserver, "request_time_float", swoole_microtime());
    add_assoc_long(zserver, "server_port", hs->socket->get_bind_port());
    add_assoc_long(zserver, "remote_port", sock->get_port());
    add_assoc_string(zserver, "remote_addr", (char * ) sock->get_ip());
    add_assoc_string(zserver, "server_protocol", (char * ) "HTTP/2");

    php_swoole_fci *fci = hs->get_handler(ctx);
    zval args[2] = {*ctx->request.zobject, *ctx->response.zobject};

    if (fci)
    {
        if (UNEXPECTED(!zend::function::call(&fci->fci_cache, 2, args, NULL, SwooleG.enable_coroutine)))
        {
            stream->reset(SW_HTTP2_ERROR_INTERNAL_ERROR);
            php_swoole_error(E_WARNING, "%s->onRequest[v2] handler error", ZSTR_VAL(swoole_http_server_ce->name));
        }
    }
    else
    {
        ctx->response.status = SW_HTTP_NOT_FOUND;
    }

    zval_ptr_dtor(&args[0]);
    zval_ptr_dtor(&args[1]);
}
#endif
