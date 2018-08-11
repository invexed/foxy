#ifndef FOXY_CLIENT_SESSION_IMPL_HPP_
#define FOXY_CLIENT_SESSION_IMPL_HPP_

#include "foxy/client_session.hpp"

namespace foxy
{
namespace detail
{

template <class ConnectHandler>
struct connect_op : boost::asio::coroutine
{
private:

  struct state
  {
    ::foxy::client_session&                      session;
    std::string                                  host;
    std::string                                  service;
    boost::asio::ip::tcp::resolver               resolver;
    boost::asio::ip::tcp::resolver::results_type results;
    boost::asio::ip::tcp::endpoint               endpoint;

    boost::asio::executor_work_guard<decltype(session.get_executor())> work;

    explicit state(
      ConnectHandler const&   handler,
      ::foxy::client_session& session_,
      std::string             host_,
      std::string             service_)
    : session(session_)
    , host(std::move(host_))
    , service(std::move(service_))
    , resolver(session.stream.get_executor().context())
    , work(session.get_executor())
    {
    }
  };

  boost::beast::handler_ptr<state, ConnectHandler> p_;

public:
  connect_op()                  = delete;
  connect_op(connect_op const&) = default;
  connect_op(connect_op&&)      = default;

  template <class DeducedHandler>
  connect_op(
    ::foxy::client_session& session,
    std::string             host,
    std::string             service,
    DeducedHandler&&        handler)
  : p_(
    std::forward<DeducedHandler>(handler),
    session, std::move(host), std::move(service))
  {
  }

  using executor_type = boost::asio::associated_executor_t<
    ConnectHandler,
    decltype(std::declval<::foxy::client_session&>().get_executor())
  >;

  using allocator_type = boost::asio::associated_allocator_t<ConnectHandler>;

  auto get_executor() const noexcept -> executor_type
  {
    return boost::asio::get_associated_executor(p_.handler(), p_->session.get_executor());
  }

  auto get_allocator() const noexcept -> allocator_type
  {
    return boost::asio::get_associated_allocator(p_.handler());
  }

  struct on_resolve_t {};
  struct on_connect_t {};

  auto operator()(
    on_resolve_t,
    boost::system::error_code                    ec,
    boost::asio::ip::tcp::resolver::results_type results) -> void
  {
    p_->results = std::move(results);
    (*this)(ec, 0);
  }

  auto operator()(
    on_connect_t,
    boost::system::error_code      ec,
    boost::asio::ip::tcp::endpoint endpoint) -> void
  {
    p_->endpoint = std::move(endpoint);
    (*this)(ec, 0);
  }

  auto operator()(
    boost::system::error_code ec,
    std::size_t const         bytes_transferred,
    bool const                is_continuation = true) -> void
  {
    using namespace std::placeholders;
    using boost::beast::bind_handler;

    auto& s = *p_;
    BOOST_ASIO_CORO_REENTER(*this)
    {
      if (s.session.stream.is_ssl()) {
        if (!SSL_set_tlsext_host_name(s.session.stream.ssl().native_handle(), s.host.c_str())) {
            ec.assign(static_cast<int>(::ERR_get_error()), boost::asio::error::get_ssl_category());
            goto upcall;
        }
      }

      BOOST_ASIO_CORO_YIELD
      s.resolver.async_resolve(
        s.host, s.service, bind_handler(std::move(*this), on_resolve_t{}, _1, _2));
      if (ec) { goto upcall; }

      BOOST_ASIO_CORO_YIELD
      boost::asio::async_connect(
        s.session.stream.tcp(), s.results,
        bind_handler(std::move(*this), on_connect_t{}, _1, _2));

      if (ec) { goto upcall; }

      {
        auto endpoint = std::move(s.endpoint);
        auto work     = std::move(s.work);
        return p_.invoke(boost::system::error_code(), std::move(endpoint));
      }

    upcall:
      if (!is_continuation) {
        BOOST_ASIO_CORO_YIELD
        boost::asio::post(boost::beast::bind_handler(std::move(*this), ec, 0));
      }
      auto work = std::move(s.work);
      p_.invoke(ec, boost::asio::ip::tcp::endpoint());
    }
  }
};

} // detail

template <class ConnectHandler>
auto
client_session::async_connect(
  std::string      host,
  std::string      service,
  ConnectHandler&& handler
) & -> BOOST_ASIO_INITFN_RESULT_TYPE(
  ConnectHandler, void(boost::system::error_code, boost::asio::ip::tcp::endpoint))
{
  boost::asio::async_completion<
    ConnectHandler, void(boost::system::error_code, boost::asio::ip::tcp::endpoint)
  >
  init(handler);

  detail::connect_op<
    BOOST_ASIO_HANDLER_TYPE(
      ConnectHandler, void(boost::system::error_code, boost::asio::ip::tcp::endpoint))
  >(
    *this, std::move(host), std::move(service), std::move(init.completion_handler)
  )({}, 0, false);

  return init.result.get();
}

namespace detail
{

template <class Request, class ResponseParser, class RequestHandler>
struct request_op : boost::asio::coroutine
{
private:

  struct state
  {
    ::foxy::client_session& session;
    Request&                request;
    ResponseParser&         parser;

    boost::asio::executor_work_guard<decltype(session.get_executor())> work;

    explicit state(
      RequestHandler const&   handler,
      ::foxy::client_session& session_,
      Request&                request_,
      ResponseParser&         parser_)
    : session(session_)
    , request(request_)
    , parser(parser_)
    , work(session.get_executor())
    {
    }
  };

  boost::beast::handler_ptr<state, RequestHandler> p_;

public:
  request_op()                  = delete;
  request_op(request_op const&) = default;
  request_op(request_op&&)      = default;

  template <class DeducedHandler>
  request_op(
    ::foxy::client_session& session,
    Request&                request,
    ResponseParser&         parser,
    DeducedHandler&&        handler)
  : p_(
    std::forward<DeducedHandler>(handler),
    session, request, parser)
  {
  }

  using executor_type = boost::asio::associated_executor_t<
    RequestHandler,
    decltype((std::declval<::foxy::client_session&>().get_executor()))
  >;

  using allocator_type = boost::asio::associated_allocator_t<RequestHandler>;

  auto get_executor() const noexcept -> executor_type
  {
    return boost::asio::get_associated_executor(p_.handler(), p_->session.get_executor());
  }

  auto get_allocator() const noexcept -> allocator_type
  {
    return boost::asio::get_associated_allocator(p_.handler());
  }

  auto operator()(
    boost::system::error_code ec,
    std::size_t const         bytes_transferred,
    bool const                is_continuation = true) -> void
  {
    using namespace std::placeholders;
    using boost::beast::bind_handler;

    namespace http = boost::beast::http;

    auto& s = *p_;
    BOOST_ASIO_CORO_REENTER(*this)
    {
      BOOST_ASIO_CORO_YIELD
      http::async_write(s.session.stream, s.request, std::move(*this));
      if (ec) { goto upcall; }

      BOOST_ASIO_CORO_YIELD
      http::async_read(s.session.stream, s.session.buffer, s.parser, std::move(*this));
      if (ec) { goto upcall; }

      {
        auto work = std::move(s.work);
        return p_.invoke(boost::system::error_code());
      }

    upcall:
      if (!is_continuation) {
        BOOST_ASIO_CORO_YIELD
        boost::asio::post(bind_handler(std::move(*this), ec, 0));
      }
      auto work = std::move(s.work);
      p_.invoke(ec);
    }
  }
};

} // detail

template <class Request, class ResponseParser, class RequestHandler>
auto
client_session::async_request(
  Request&         request,
  ResponseParser&  parser,
  RequestHandler&& handler
) & -> BOOST_ASIO_INITFN_RESULT_TYPE(RequestHandler, void(boost::system::error_code))
{
  boost::asio::async_completion<
    RequestHandler, void(boost::system::error_code)
  >
  init(handler);

  detail::request_op<
    Request, ResponseParser,
    BOOST_ASIO_HANDLER_TYPE(RequestHandler, void(boost::system::error_code))
  >(*this, request, parser, std::move(init.completion_handler))({}, 0, false);

  return init.result.get();
}

} // foxy

#endif // FOXY_CLIENT_SESSION_IMPL_HPP_