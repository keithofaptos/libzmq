/*
    Copyright (c) 2007-2017 Contributors as noted in the AUTHORS file

    This file is part of libzmq, the ZeroMQ core engine in C++.

    libzmq is free software; you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License (LGPL) as published
    by the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    As a special exception, the Contributors give you permission to link
    this library with independent modules to produce an executable,
    regardless of the license terms of these independent modules, and to
    copy and distribute the resulting executable under terms of your choice,
    provided that you also meet, for each linked independent module, the
    terms and conditions of the license of that module. An independent
    module is a module which is not derived from or based on this library.
    If you modify this library, you must extend this exception to your
    version of the library.

    libzmq is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
    License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "testutil.hpp"
#include "testutil_security.hpp"
#if defined(ZMQ_HAVE_WINDOWS)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdexcept>
#define close closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif
#include <unity.h>

#include "../src/tweetnacl.h"
#include "../src/curve_client_tools.hpp"
#include "../src/random.hpp"

char error_message_buffer[256];

#if defined(_MSC_VER) && _MSC_VER < 1900
#define snprintf _snprintf
#endif

const char *zmq_errno_message ()
{
    snprintf (error_message_buffer, sizeof (error_message_buffer),
              "errno=%i (%s)", zmq_errno (), zmq_strerror (zmq_errno ()));
    return error_message_buffer;
}

#define TEST_ASSERT_ZMQ_ERRNO(condition)                                       \
    TEST_ASSERT_MESSAGE ((condition), zmq_errno_message ())

void *ctx;
void *handler;
void *zap_thread;
void *server;
void *server_mon;
char my_endpoint[MAX_SOCKET_STRING];

void setUp ()
{
    setup_context_and_server_side (&ctx, &handler, &zap_thread, &server,
                                   &server_mon, my_endpoint);
}

void tearDown ()
{
    shutdown_context_and_server_side (ctx, zap_thread, server, server_mon,
                                      handler);
}

const int timeout = 250;

const char large_routing_id[] = "0123456789012345678901234567890123456789"
                                "0123456789012345678901234567890123456789"
                                "0123456789012345678901234567890123456789"
                                "0123456789012345678901234567890123456789"
                                "0123456789012345678901234567890123456789"
                                "0123456789012345678901234567890123456789"
                                "012345678901234";

static void zap_handler_large_routing_id (void *ctx)
{
    zap_handler_generic (ctx, zap_ok, large_routing_id);
}

void expect_new_client_curve_bounce_fail (void *ctx,
                                          char *server_public,
                                          char *client_public,
                                          char *client_secret,
                                          char *my_endpoint,
                                          void *server,
                                          void **client_mon = NULL,
                                          int expected_client_event = 0,
                                          int expected_client_value = 0)
{
    curve_client_data_t curve_client_data = {server_public, client_public,
                                             client_secret};
    expect_new_client_bounce_fail (
      ctx, my_endpoint, server, socket_config_curve_client, &curve_client_data,
      client_mon, expected_client_event, expected_client_value);
}

void test_null_key (void *ctx,
                    void *server,
                    void *server_mon,
                    char *my_endpoint,
                    char *server_public,
                    char *client_public,
                    char *client_secret)
{
    expect_new_client_curve_bounce_fail (ctx, server_public, client_public,
                                         client_secret, my_endpoint, server);

#ifdef ZMQ_BUILD_DRAFT_API
    int handshake_failed_encryption_event_count =
      expect_monitor_event_multiple (server_mon,
                                     ZMQ_EVENT_HANDSHAKE_FAILED_PROTOCOL,
                                     ZMQ_PROTOCOL_ERROR_ZMTP_CRYPTOGRAPHIC);

    // handshake_failed_encryption_event_count should be at least two because
    // expect_bounce_fail involves two exchanges
    // however, with valgrind we see only one event (maybe the next one takes
    // very long, or does not happen at all because something else takes very
    // long)

    fprintf (stderr,
             "count of "
             "ZMQ_EVENT_HANDSHAKE_FAILED_PROTOCOL/"
             "ZMQ_PROTOCOL_ERROR_ZMTP_CRYPTOGRAPHIC events: %i\n",
             handshake_failed_encryption_event_count);
#endif
}

void test_curve_security_with_valid_credentials ()
{
    curve_client_data_t curve_client_data = {
      valid_server_public, valid_client_public, valid_client_secret};
    void *client_mon;
    void *client =
      create_and_connect_client (ctx, my_endpoint, socket_config_curve_client,
                                 &curve_client_data, &client_mon);
    bounce (server, client);
    int rc = zmq_close (client);
    TEST_ASSERT_ZMQ_ERRNO (rc == 0);

#ifdef ZMQ_BUILD_DRAFT_API
    int event = get_monitor_event_with_timeout (server_mon, NULL, NULL, -1);
    assert (event == ZMQ_EVENT_HANDSHAKE_SUCCEEDED);

    assert_no_more_monitor_events_with_timeout (server_mon, timeout);

    event = get_monitor_event_with_timeout (client_mon, NULL, NULL, -1);
    assert (event == ZMQ_EVENT_HANDSHAKE_SUCCEEDED);

    assert_no_more_monitor_events_with_timeout (client_mon, timeout);

    rc = zmq_close (client_mon);
    TEST_ASSERT_ZMQ_ERRNO (rc == 0);
#endif
}

void test_curve_security_with_bogus_client_credentials ()
{
    //  This must be caught by the ZAP handler
    char bogus_public[41];
    char bogus_secret[41];
    zmq_curve_keypair (bogus_public, bogus_secret);

    expect_new_client_curve_bounce_fail (ctx, valid_server_public, bogus_public,
                                         bogus_secret, my_endpoint, server,
                                         NULL,
#ifdef ZMQ_BUILD_DRAFT_API
                                         ZMQ_EVENT_HANDSHAKE_FAILED_AUTH, 400
#else
                                         0, 0
#endif
    );

    int server_event_count = 0;
#ifdef ZMQ_BUILD_DRAFT_API
    server_event_count = expect_monitor_event_multiple (
      server_mon, ZMQ_EVENT_HANDSHAKE_FAILED_AUTH, 400);
    TEST_ASSERT_LESS_OR_EQUAL_INT (1, server_event_count);
#endif

    // there may be more than one ZAP request due to repeated attempts by the client
    TEST_ASSERT (0 == server_event_count
                 || 1 <= zmq_atomic_counter_value (zap_requests_handled));
}

void expect_zmtp_mechanism_mismatch (void *client,
                                     char *my_endpoint,
                                     void *server,
                                     void *server_mon)
{
    //  This must be caught by the curve_server class, not passed to ZAP
    int rc = zmq_connect (client, my_endpoint);
    TEST_ASSERT_ZMQ_ERRNO (rc == 0);
    expect_bounce_fail (server, client);
    close_zero_linger (client);

#ifdef ZMQ_BUILD_DRAFT_API
    expect_monitor_event_multiple (server_mon,
                                   ZMQ_EVENT_HANDSHAKE_FAILED_PROTOCOL,
                                   ZMQ_PROTOCOL_ERROR_ZMTP_MECHANISM_MISMATCH);
#endif

    TEST_ASSERT_EQUAL_INT (0, zmq_atomic_counter_value (zap_requests_handled));
}

void test_curve_security_with_null_client_credentials ()
{
    void *client = zmq_socket (ctx, ZMQ_DEALER);
    TEST_ASSERT_NOT_NULL (client);

    expect_zmtp_mechanism_mismatch (client, my_endpoint, server, server_mon);
}

void test_curve_security_with_plain_client_credentials ()
{
    void *client = zmq_socket (ctx, ZMQ_DEALER);
    TEST_ASSERT_NOT_NULL (client);
    int rc = zmq_setsockopt (client, ZMQ_PLAIN_USERNAME, "admin", 5);
    TEST_ASSERT_ZMQ_ERRNO (rc == 0);
    rc = zmq_setsockopt (client, ZMQ_PLAIN_PASSWORD, "password", 8);
    TEST_ASSERT_ZMQ_ERRNO (rc == 0);

    expect_zmtp_mechanism_mismatch (client, my_endpoint, server, server_mon);
}

int connect_vanilla_socket (char *my_endpoint)
{
    int s;
    struct sockaddr_in ip4addr;

    unsigned short int port;
    int rc = sscanf (my_endpoint, "tcp://127.0.0.1:%hu", &port);
    TEST_ASSERT_EQUAL_INT (1, rc);

    ip4addr.sin_family = AF_INET;
    ip4addr.sin_port = htons (port);
#if defined(ZMQ_HAVE_WINDOWS) && (_WIN32_WINNT < 0x0600)
    ip4addr.sin_addr.s_addr = inet_addr ("127.0.0.1");
#else
    inet_pton (AF_INET, "127.0.0.1", &ip4addr.sin_addr);
#endif

    s = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
    rc = connect (s, (struct sockaddr *) &ip4addr, sizeof (ip4addr));
    TEST_ASSERT_GREATER_THAN_INT (-1, rc);
    return s;
}

void test_curve_security_unauthenticated_message ()
{
    // Unauthenticated messages from a vanilla socket shouldn't be received
    int s = connect_vanilla_socket (my_endpoint);
    // send anonymous ZMTP/1.0 greeting
    send (s, "\x01\x00", 2, 0);
    // send sneaky message that shouldn't be received
    send (s, "\x08\x00sneaky\0", 9, 0);

    zmq_setsockopt (server, ZMQ_RCVTIMEO, &timeout, sizeof (timeout));
    char *buf = s_recv (server);
    TEST_ASSERT_NULL_MESSAGE (buf, "Received unauthenticated message");
    close (s);
}

void send_all (int fd, const char *data, size_t size)
{
    while (size > 0) {
        int res = send (fd, data, size, 0);
        TEST_ASSERT_GREATER_THAN_INT (0, res);
        size -= res;
        data += res;
    }
}

template <size_t N> void send (int fd, const char (&data)[N])
{
    send_all (fd, data, N - 1);
}

void send_greeting (int s)
{
    send (s, "\xff\0\0\0\0\0\0\0\0\x7f");            // signature
    send (s, "\x03\x00");                            // version 3.0
    send (s, "CURVE\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"); // mechanism CURVE
    send (s, "\0");                                  // as-server == false
    send (s, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0");
}

void test_curve_security_invalid_hello_wrong_length ()
{
    int s = connect_vanilla_socket (my_endpoint);

    // send GREETING
    send_greeting (s);

    // send CURVE HELLO of wrong size
    send (s, "\x04\x06\x05HELLO");

#ifdef ZMQ_BUILD_DRAFT_API
    expect_monitor_event_multiple (
      server_mon, ZMQ_EVENT_HANDSHAKE_FAILED_PROTOCOL,
      ZMQ_PROTOCOL_ERROR_ZMTP_MALFORMED_COMMAND_HELLO);
#endif

    close (s);
}

const size_t hello_length = 200;
const size_t welcome_length = 168;

zmq::curve_client_tools_t make_curve_client_tools ()
{
    uint8_t valid_client_secret_decoded[32];
    uint8_t valid_client_public_decoded[32];

    zmq_z85_decode (valid_client_public_decoded, valid_client_public);
    zmq_z85_decode (valid_client_secret_decoded, valid_client_secret);

    uint8_t valid_server_public_decoded[32];
    zmq_z85_decode (valid_server_public_decoded, valid_server_public);

    return zmq::curve_client_tools_t (valid_client_public_decoded,
                                      valid_client_secret_decoded,
                                      valid_server_public_decoded);
}

#ifndef htonll
uint64_t htonll (uint64_t value)
{
    // The answer is 42
    static const int num = 42;

    // Check the endianness
    if (*reinterpret_cast<const char *> (&num) == num) {
        const uint32_t high_part = htonl (static_cast<uint32_t> (value >> 32));
        const uint32_t low_part =
          htonl (static_cast<uint32_t> (value & 0xFFFFFFFFLL));

        return (static_cast<uint64_t> (low_part) << 32) | high_part;
    } else {
        return value;
    }
}
#endif

template <size_t N> void send_command (int s, char (&command)[N])
{
    if (N < 256) {
        send (s, "\x04");
        char len = (char) N;
        send_all (s, &len, 1);
    } else {
        send (s, "\x06");
        uint64_t len = htonll (N);
        send_all (s, (char *) &len, 8);
    }
    send_all (s, command, N);
}

void test_curve_security_invalid_hello_command_name ()
{
    int s = connect_vanilla_socket (my_endpoint);

    send_greeting (s);

    zmq::curve_client_tools_t tools = make_curve_client_tools ();

    // send CURVE HELLO with a misspelled command name (but otherwise correct)
    char hello[hello_length];
    int rc = tools.produce_hello (hello, 0);
    TEST_ASSERT_ZMQ_ERRNO (rc == 0);
    hello[5] = 'X';

    send_command (s, hello);

#ifdef ZMQ_BUILD_DRAFT_API
    expect_monitor_event_multiple (server_mon,
                                   ZMQ_EVENT_HANDSHAKE_FAILED_PROTOCOL,
                                   ZMQ_PROTOCOL_ERROR_ZMTP_UNEXPECTED_COMMAND);
#endif

    close (s);
}

void test_curve_security_invalid_hello_version ()
{
    int s = connect_vanilla_socket (my_endpoint);

    send_greeting (s);

    zmq::curve_client_tools_t tools = make_curve_client_tools ();

    // send CURVE HELLO with a wrong version number (but otherwise correct)
    char hello[hello_length];
    int rc = tools.produce_hello (hello, 0);
    TEST_ASSERT_ZMQ_ERRNO (rc == 0);
    hello[6] = 2;

    send_command (s, hello);

#ifdef ZMQ_BUILD_DRAFT_API
    expect_monitor_event_multiple (
      server_mon, ZMQ_EVENT_HANDSHAKE_FAILED_PROTOCOL,
      ZMQ_PROTOCOL_ERROR_ZMTP_MALFORMED_COMMAND_HELLO);
#endif

    close (s);
}

void flush_read (int fd)
{
    int res;
    char buf[256];

    while ((res = recv (fd, buf, 256, 0)) == 256) {
    }
    TEST_ASSERT_NOT_EQUAL (-1, res);
}

void recv_all (int fd, uint8_t *data, size_t len)
{
    size_t received = 0;
    while (received < len) {
        int res = recv (fd, (char *) data, len, 0);
        TEST_ASSERT_GREATER_THAN_INT (0, res);

        data += res;
        received += res;
    }
}

void recv_greeting (int fd)
{
    uint8_t greeting[64];
    recv_all (fd, greeting, 64);
    //  TODO assert anything about the greeting received from the server?
}

int connect_exchange_greeting_and_send_hello (char *my_endpoint,
                                              zmq::curve_client_tools_t &tools)
{
    int s = connect_vanilla_socket (my_endpoint);

    send_greeting (s);
    recv_greeting (s);

    // send valid CURVE HELLO
    char hello[hello_length];
    int rc = tools.produce_hello (hello, 0);
    TEST_ASSERT_ZMQ_ERRNO (rc == 0);

    send_command (s, hello);
    return s;
}

void test_curve_security_invalid_initiate_wrong_length ()
{
    zmq::curve_client_tools_t tools = make_curve_client_tools ();

    int s = connect_exchange_greeting_and_send_hello (my_endpoint, tools);

    // receive but ignore WELCOME
    flush_read (s);

#ifdef ZMQ_BUILD_DRAFT_API
    int res = get_monitor_event_with_timeout (server_mon, NULL, NULL, timeout);
    TEST_ASSERT_EQUAL_INT (-1, res);
#else
    LIBZMQ_UNUSED (timeout);
#endif

    send (s, "\x04\x09\x08INITIATE");

#ifdef ZMQ_BUILD_DRAFT_API
    expect_monitor_event_multiple (
      server_mon, ZMQ_EVENT_HANDSHAKE_FAILED_PROTOCOL,
      ZMQ_PROTOCOL_ERROR_ZMTP_MALFORMED_COMMAND_INITIATE);
#endif

    close (s);
}

int connect_exchange_greeting_and_hello_welcome (
  char *my_endpoint,
  void *server_mon,
  int timeout,
  zmq::curve_client_tools_t &tools)
{
    int s = connect_exchange_greeting_and_send_hello (my_endpoint, tools);

    // receive but ignore WELCOME
    uint8_t welcome[welcome_length + 2];
    recv_all (s, welcome, welcome_length + 2);

    uint8_t cn_precom[crypto_box_BEFORENMBYTES];
    int res = tools.process_welcome (welcome + 2, welcome_length, cn_precom);
    TEST_ASSERT_ZMQ_ERRNO (res == 0);

#ifdef ZMQ_BUILD_DRAFT_API
    res = get_monitor_event_with_timeout (server_mon, NULL, NULL, timeout);
    TEST_ASSERT_EQUAL_INT (-1, res);
#endif

    return s;
}

void test_curve_security_invalid_initiate_command_name ()
{
    zmq::curve_client_tools_t tools = make_curve_client_tools ();
    int s = connect_exchange_greeting_and_hello_welcome (
      my_endpoint, server_mon, timeout, tools);

    char initiate[257];
    tools.produce_initiate (initiate, 257, 1, NULL, 0);
    // modify command name
    initiate[5] = 'X';

    send_command (s, initiate);

#ifdef ZMQ_BUILD_DRAFT_API
    expect_monitor_event_multiple (server_mon,
                                   ZMQ_EVENT_HANDSHAKE_FAILED_PROTOCOL,
                                   ZMQ_PROTOCOL_ERROR_ZMTP_UNEXPECTED_COMMAND);
#endif

    close (s);
}

void test_curve_security_invalid_initiate_command_encrypted_cookie ()
{
    zmq::curve_client_tools_t tools = make_curve_client_tools ();
    int s = connect_exchange_greeting_and_hello_welcome (
      my_endpoint, server_mon, timeout, tools);

    char initiate[257];
    tools.produce_initiate (initiate, 257, 1, NULL, 0);
    // make garbage from encrypted cookie
    initiate[30] = !initiate[30];

    send_command (s, initiate);

#ifdef ZMQ_BUILD_DRAFT_API
    expect_monitor_event_multiple (server_mon,
                                   ZMQ_EVENT_HANDSHAKE_FAILED_PROTOCOL,
                                   ZMQ_PROTOCOL_ERROR_ZMTP_CRYPTOGRAPHIC);
#endif

    close (s);
}

void test_curve_security_invalid_initiate_command_encrypted_content ()
{
    zmq::curve_client_tools_t tools = make_curve_client_tools ();
    int s = connect_exchange_greeting_and_hello_welcome (
      my_endpoint, server_mon, timeout, tools);

    char initiate[257];
    tools.produce_initiate (initiate, 257, 1, NULL, 0);
    // make garbage from encrypted content
    initiate[150] = !initiate[150];

    send_command (s, initiate);

#ifdef ZMQ_BUILD_DRAFT_API
    expect_monitor_event_multiple (server_mon,
                                   ZMQ_EVENT_HANDSHAKE_FAILED_PROTOCOL,
                                   ZMQ_PROTOCOL_ERROR_ZMTP_CRYPTOGRAPHIC);
#endif

    close (s);
}

void test_curve_security_invalid_keysize (void *ctx)
{
    //  Check return codes for invalid buffer sizes
    void *client = zmq_socket (ctx, ZMQ_DEALER);
    TEST_ASSERT_NOT_NULL (client);
    errno = 0;
    int rc =
      zmq_setsockopt (client, ZMQ_CURVE_SERVERKEY, valid_server_public, 123);
    assert (rc == -1 && errno == EINVAL);
    errno = 0;
    rc = zmq_setsockopt (client, ZMQ_CURVE_PUBLICKEY, valid_client_public, 123);
    assert (rc == -1 && errno == EINVAL);
    errno = 0;
    rc = zmq_setsockopt (client, ZMQ_CURVE_SECRETKEY, valid_client_secret, 123);
    assert (rc == -1 && errno == EINVAL);
    rc = zmq_close (client);
    TEST_ASSERT_ZMQ_ERRNO (rc == 0);
}

// TODO why isn't this const?
char null_key[] = "0000000000000000000000000000000000000000";

void test_null_server_key ()
{
    //  Check CURVE security with a null server key
    //  This will be caught by the curve_server class, not passed to ZAP
    test_null_key (ctx, server, server_mon, my_endpoint, null_key,
                   valid_client_public, valid_client_secret);
}

void test_null_client_public_key ()
{
    //  Check CURVE security with a null client public key
    //  This will be caught by the curve_server class, not passed to ZAP
    test_null_key (ctx, server, server_mon, my_endpoint, valid_server_public,
                   null_key, valid_client_secret);
}

void test_null_client_secret_key ()
{
    //  Check CURVE security with a null client public key
    //  This will be caught by the curve_server class, not passed to ZAP
    test_null_key (ctx, server, server_mon, my_endpoint, valid_server_public,
                   valid_client_public, null_key);
}


int main (void)
{
    if (!zmq_has ("curve")) {
        printf ("CURVE encryption not installed, skipping test\n");
        return 0;
    }

    zmq::random_open ();

    setup_testutil_security_curve ();


    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_curve_security_with_valid_credentials);
    RUN_TEST (test_null_server_key);
    RUN_TEST (test_null_client_public_key);
    RUN_TEST (test_null_client_secret_key);
    RUN_TEST (test_curve_security_with_bogus_client_credentials);
    RUN_TEST (test_curve_security_with_null_client_credentials);
    RUN_TEST (test_curve_security_with_plain_client_credentials);
    RUN_TEST (test_curve_security_unauthenticated_message);

    //  tests with misbehaving CURVE client
    RUN_TEST (test_curve_security_invalid_hello_wrong_length);
    RUN_TEST (test_curve_security_invalid_hello_command_name);
    RUN_TEST (test_curve_security_invalid_hello_version);
    RUN_TEST (test_curve_security_invalid_initiate_wrong_length);
    RUN_TEST (test_curve_security_invalid_initiate_command_name);
    RUN_TEST (test_curve_security_invalid_initiate_command_encrypted_cookie);
    RUN_TEST (test_curve_security_invalid_initiate_command_encrypted_content);

    // TODO this requires a deviating test setup, must be moved to a separate executable/fixture
    //  test with a large routing id (resulting in large metadata)
    fprintf (stderr,
             "test_curve_security_with_valid_credentials (large routing id)\n");
    setup_context_and_server_side (
      &ctx, &handler, &zap_thread, &server, &server_mon, my_endpoint,
      &zap_handler_large_routing_id, &socket_config_curve_server,
      &valid_server_secret, large_routing_id);
    test_curve_security_with_valid_credentials ();
    shutdown_context_and_server_side (ctx, zap_thread, server, server_mon,
                                      handler);

    ctx = zmq_ctx_new ();
    test_curve_security_invalid_keysize (ctx);
    int rc = zmq_ctx_term (ctx);
    TEST_ASSERT_ZMQ_ERRNO (rc == 0);

    zmq::random_close ();

    return UNITY_END ();
}
