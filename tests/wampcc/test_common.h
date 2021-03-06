#ifndef WAMPCC_TEST_COMMON_H
#define WAMPCC_TEST_COMMON_H

#include "wampcc/kernel.h"
#include "wampcc/wamp_session.h"
#include "wampcc/tcp_socket.h"
#include "wampcc/websocket_protocol.h"
#include "wampcc/rawsocket_protocol.h"
#include "wampcc/wamp_router.h"
#include "wampcc/event_loop.h"
#include "3rdparty/apache/base64.h"

#include <iostream>
#include <chrono>
#include <string.h>

#include <openssl/evp.h>

#undef NDEBUG

#include <assert.h>

#define TLOG(X) std::cout << X << std::endl

//#define TSTART() std::cout << "==> " << __PRETTY_FUNCTION__ << std::endl

#ifndef TSTART
# define TSTART() while(false){}
#endif

namespace wampcc
{

logger debug_logger()
{
  return logger::stream(logger::lockable_cout, -1, true);
}


struct socket_listener
{
  void io_on_read(char*, size_t n)
  {
    std::cout << "socket_listener: io_on_read, n=" << n << std::endl;
  }

  void io_on_error(uverr ec)
  {
    std::cout << "socket_listener: io_on_error, err=" << ec << std::endl;
  }

  void start_listening(std::shared_ptr<tcp_socket> sock)
  {
    sock->start_read([this](char* s, size_t n) { this->io_on_read(s, n); },
                     [this](uverr e) { this->io_on_error(e); });
  }

  void start_listening(tcp_socket& sock)
  {
    sock.start_read([this](char* s, size_t n) { this->io_on_read(s, n); },
                    [this](uverr e) { this->io_on_error(e); });
  }
};

enum test_outcome { e_expected, e_unexpected };

class internal_server
{
public:
  internal_server(logger log = logger::nolog())
    : m_kernel(new kernel({}, log)),
      m_router(new wamp_router(m_kernel.get(), nullptr)),
      m_port(0),
      m_user_password("secret2"),
      m_salt{"saltxx",32, 1500}
  {
    std::random_device rd;  // used for seed
    std::mt19937 gen(rd()); 
    std::uniform_int_distribution<> dis(-1000, 1000);

    m_salt.iterations += dis(gen);
  }

  ~internal_server()
  {
    m_router.reset();
    m_kernel.reset();
  }

  /** Call to setup salting on the authentication.  Should be called before
   * start(). */
  void enable_salting()
  {
    std::vector<unsigned char> derived_key_bytes(m_salt.keylen, {});

    // generate the derived key
    if (PKCS5_PBKDF2_HMAC(m_user_password.c_str(), m_user_password.size(),
                          (const unsigned char *) m_salt.salt.c_str(), m_salt.salt.size(),
                          m_salt.iterations,
                          EVP_sha256(), /* sha256 is used by Autobahn */
                          m_salt.keylen, derived_key_bytes.data()) == 0)
      throw std::runtime_error("PKCS5_PBKDF2_HMAC failed");

    // convert derived key to base64
    char b64[50] = {}; // 256 bits / 6 bits
    ap_base64encode(b64, (char*)derived_key_bytes.data(), derived_key_bytes.size());
    m_derived_key = b64;
  }

  int start(int starting_port_number)
  {
    auth_provider server_auth;
    server_auth.provider_name = [](const std::string) { return "programdb"; };
    server_auth.policy =
        [](const std::string& /*user*/, const std::string& /*realm*/) {
      std::set<std::string> methods{"wampcra"};
      return std::make_tuple(auth_provider::mode::authenticate,
                             std::move(methods));
    };

    if (m_derived_key.empty()) {
      server_auth.user_secret =
        [this](const std::string& /*user*/,
               const std::string& /*realm*/) { return m_user_password; };
    }
    else {
      server_auth.user_secret =
        [this](const std::string& /*user*/,
               const std::string& /*realm*/) { return m_derived_key; };

      server_auth.cra_salt = [this](const std::string& /*realm*/,
                                    const std::string& /*user*/) {
        return m_salt;
      };
    }

    for (int port = starting_port_number; port < 65535; port++) {
      std::future<uverr> fut_listen_err = m_router->listen(std::string("127.0.0.1"), std::to_string(port), server_auth);
      std::future_status status =
          fut_listen_err.wait_for(std::chrono::milliseconds(100));
      if (status == std::future_status::ready) {
        wampcc::uverr err = fut_listen_err.get();
        if (err == 0)
          return m_port = port;
      }
    }

    throw std::runtime_error(
        "failed to find an available port number for listen socket");
    return 0;
  }

  int start(int starting_port_number,
            int allowed_protocols,
            int allowed_serialisers)
  {
    auth_provider server_auth;
    server_auth.provider_name = [](const std::string) { return "programdb"; };
    server_auth.policy =
        [](const std::string& /*user*/, const std::string& /*realm*/) {
      std::set<std::string> methods{"wampcra"};
      return std::make_tuple(auth_provider::mode::authenticate,
                             std::move(methods));
    };

    if (m_derived_key.empty()) {
      server_auth.user_secret =
        [this](const std::string& /*user*/,
               const std::string& /*realm*/) { return m_user_password; };
    }
    else {
      server_auth.user_secret =
        [this](const std::string& /*user*/,
               const std::string& /*realm*/) { return m_derived_key; };

      server_auth.cra_salt = [this](const std::string& /*realm*/,
                                    const std::string& /*user*/) {
        return m_salt;
      };
    }

    wamp_router::listen_options opts;
    opts.protocols = allowed_protocols;
    opts.serialisers = allowed_serialisers;
    opts.node = "127.0.0.1";
    for (int port = starting_port_number; port < 65535; port++) {
      opts.service = std::to_string(port);
      std::future<uverr> fut_listen_err = m_router->listen(server_auth, opts);
      std::future_status status =
          fut_listen_err.wait_for(std::chrono::milliseconds(100));
      if (status == std::future_status::ready) {
        wampcc::uverr err = fut_listen_err.get();
        if (err == 0)
          return m_port = port;
      }
    }

    throw std::runtime_error(
        "failed to find an available port number for listen socket");
    return 0;
  }

  void reset_kernel() { m_kernel.reset(); }

  void reset_dealer() { m_router.reset(); }

  kernel* get_kernel() { return m_kernel.get(); }

  wamp_router* router() { return m_router.get(); }

  int port() const { return m_port; }

private:
  std::unique_ptr<kernel> m_kernel;
  std::shared_ptr<wamp_router> m_router;
  int m_port;

  std::string m_user_password;
  auth_provider::cra_salt_params m_salt;
  std::string m_derived_key;
};


enum class callback_status_t {
  not_invoked,
  close_with_sp,
  open_with_sp,
} callback_status;


std::unique_ptr< std::promise<callback_status_t> > sessioncb_promise;
std::future<callback_status_t> reset_callback_result()
{
  sessioncb_promise.reset( new std::promise<callback_status_t>());
  return sessioncb_promise->get_future();
}




void session_cb(wamp_session& ws, bool is_open)
{
  if (is_open == false) {
    callback_status = callback_status_t::close_with_sp;
  }
  else
  {
    callback_status = callback_status_t::open_with_sp;
  }

  if (sessioncb_promise)
    sessioncb_promise->set_value(callback_status);
}


std::unique_ptr<tcp_socket> tcp_connect(kernel& k, int port)
{
  std::unique_ptr<tcp_socket> sock{new tcp_socket(&k)};

  auto fut = sock->connect("127.0.0.1", port);
  auto status = fut.wait_for(std::chrono::milliseconds(100));

  if (status == std::future_status::timeout)
    throw std::runtime_error("timeout during connect");

  auto err = fut.get();
  if (err)
    throw std::runtime_error(err.message());

  if (sock->is_connected() == false)
    throw std::runtime_error("expected to be connected");

  return sock;
}


std::shared_ptr<wamp_session> establish_session(
  std::unique_ptr<kernel>& the_kernel, int port,
  int protocols = wampcc::all_protocols,
  int serialisers = wampcc::all_serialisers)
{
  static int count = 0;
  count++;

  std::unique_ptr<tcp_socket> sock(new tcp_socket(the_kernel.get()));

  auto fut = sock->connect("127.0.0.1", port);

  auto connect_status = fut.wait_for(std::chrono::milliseconds(100));
  if (connect_status == std::future_status::timeout) {
    return std::shared_ptr<wamp_session>();
  }

  if (fut.get())
    throw std::runtime_error("tcp connect failed during establish_session()");

  websocket_protocol::options ws_opts;
  ws_opts.serialisers = serialisers;

  rawsocket_protocol::options rs_opts;
  rs_opts.serialisers = serialisers;


  /* attempt to create a session */
  std::shared_ptr<wamp_session> session;
  switch(protocols)
  {
    case static_cast<int>(protocol_type::websocket) : {
      session = wamp_session::create<websocket_protocol>(
        the_kernel.get(), std::move(sock), session_cb, ws_opts);
      break;
    }
    case static_cast<int>(protocol_type::rawsocket) : {
      session = wamp_session::create<rawsocket_protocol>(
        the_kernel.get(), std::move(sock), session_cb, rs_opts);
      break;
    }
    case wampcc::all_protocols : {
      if (count % 2)
        session = wamp_session::create<rawsocket_protocol>(
          the_kernel.get(), std::move(sock), session_cb, rs_opts);
      else
        session = wamp_session::create<websocket_protocol>(
          the_kernel.get(), std::move(sock), session_cb, ws_opts);
      break;
    }
  }

  return session;
}


void perform_realm_logon(std::shared_ptr<wamp_session>&session,
                         std::string realm="default_realm")
{
  if (!session)
    throw std::runtime_error("perform_realm_logon: null session");

  auto fut = reset_callback_result();

  wampcc::client_credentials credentials;
  credentials.realm  = realm;
  credentials.authid = "peter";
  credentials.authmethods = {"wampcra"};
  credentials.secret_fn =  [=]() -> std::string { return "secret2"; };

  session->hello(credentials);

  auto long_time = std::chrono::milliseconds(200);

  if (fut.wait_for(long_time) != std::future_status::ready)
    throw std::runtime_error("timeout waiting for realm logon");

  if (fut.get() != callback_status_t::open_with_sp)
    throw std::runtime_error("realm logon failed");
}


enum class rpc_result_expect {nocheck, success, fail };
result_info sync_rpc_all(std::shared_ptr<wamp_session>&session,
                              const char* rpc_name,
                              wamp_args call_args,
                              rpc_result_expect expect)
{
  if (!session)
    throw std::runtime_error("sync_rpc_all: null session");

  std::promise<result_info> result_prom;
  std::future<result_info> result_fut = result_prom.get_future();
  session->call(rpc_name, {}, call_args,
                [&result_prom](wamp_session&, result_info r) {
                  result_prom.set_value(r);
                });

  auto long_time = std::chrono::milliseconds(200);

  if (result_fut.wait_for(long_time) != std::future_status::ready)
    throw std::runtime_error("timeout waiting for RPC reply");

  result_info result = result_fut.get();

  if (expect==rpc_result_expect::success && result.was_error==true)
    throw std::runtime_error("expected call to succeed");

  if (expect==rpc_result_expect::fail && result.was_error==false)
    throw std::runtime_error("expected call to fail");

  return result;
}


}

#endif
