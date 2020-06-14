#ifndef LIBASYIK_ASYIK_HTTP_HPP
#define LIBASYIK_ASYIK_HTTP_HPP
#include <string>
#include <regex>
#include "boost/fiber/all.hpp"
#include "aixlog.hpp"
#include "boost/asio.hpp"
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include "service.hpp"
#include "boost/algorithm/string/predicate.hpp"
#include "internal/asio_internal.hpp"

namespace fibers = boost::fibers;
using fiber = boost::fibers::fiber;
using tcp = boost::asio::ip::tcp;
namespace asio = boost::asio;
namespace ssl = asio::ssl;
namespace beast = boost::beast;
namespace ip = boost::asio::ip;
namespace websocket = beast::websocket;

namespace asyik
{
  void _TEST_invoke_http();

  class http_connection;
  using http_connection_ptr = std::shared_ptr<http_connection>;
  using http_connection_wptr = std::weak_ptr<http_connection>;

  class websocket;
  using websocket_ptr = std::shared_ptr<websocket>;

  using http_route_args = std::vector<std::string>;

  class http_server;
  using http_server_ptr = std::shared_ptr<http_server>;
  using http_server_wptr = std::weak_ptr<http_server>;

  class http_request;
  using http_request_ptr = std::shared_ptr<http_request>;
  using http_request_wptr = std::weak_ptr<http_request>;
  using http_result = uint16_t;

  using http_beast_request = boost::beast::http::request<boost::beast::http::string_body>;
  using http_beast_response = boost::beast::http::response<boost::beast::http::string_body>;
  using http_request_headers = http_beast_request::header_type;
  using http_request_body = http_beast_request::body_type::value_type;
  using http_response_headers = http_beast_response::header_type;
  using http_response_body = http_beast_response::body_type::value_type;

  using http_route_callback = std::function<void(http_request_ptr, const http_route_args &)>;
  using http_route_tuple = std::tuple<std::string, std::regex, http_route_callback>;

  using websocket_route_callback = std::function<void(websocket_ptr, const http_route_args &)>;
  using websocket_route_tuple = std::tuple<std::string, std::regex, websocket_route_callback>;

  using websocket_close_code = boost::beast::websocket::close_code;

  http_server_ptr make_http_server(service_ptr as, string_view addr, uint16_t port = 80);
  http_connection_ptr make_http_connection(service_ptr as, string_view addr, string_view port);
  websocket_ptr make_websocket_connection(service_ptr as,
                                          string_view url,
                                          int timeout = 10);

  http_request_ptr http_easy_request(service_ptr as, 
                                     string_view method, string_view url);
  template<typename D>
  http_request_ptr http_easy_request(service_ptr as, 
                                     string_view method, string_view url, D &&data);

  template<typename D>
  http_request_ptr http_easy_request(service_ptr as, 
                                     string_view method, string_view url, D &&data,
                                     const std::map<string_view, string_view> &headers);

  struct http_url_scheme
  {
    bool is_ssl;
    std::string host;
    uint16_t port;
    std::string target;
  };

  bool http_analyze_url(string_view url, http_url_scheme &scheme);

  namespace internal
  {
    std::string route_spec_to_regex(string_view route_spc);
  }

  class http_server : public std::enable_shared_from_this<http_server>
  {
  private:
    struct private_
    {
    };

  public:
    ~http_server(){};
    http_server &operator=(const http_server &) = delete;
    http_server() = delete;
    http_server(const http_server &) = delete;
    http_server(http_server &&) = default;
    http_server &operator=(http_server &&) = default;

    http_server(struct private_ &&, service_ptr as, string_view addr, uint16_t port);

    template <typename T>
    void on_http_request(string_view route_spec, T &&cb)
    {
      std::regex re(internal::route_spec_to_regex(route_spec));
      on_http_request_regex(re, "", std::forward<T>(cb));
    }

    template <typename T>
    void on_http_request(string_view route_spec, string_view method, T &&cb)
    {
      std::regex re(internal::route_spec_to_regex(route_spec));
      on_http_request_regex(re, method, std::forward<T>(cb));
    }

    template <typename R, typename M, typename T>
    void on_http_request_regex(R &&r, M &&m, T &&cb)
    {
      http_routes.push_back({std::string{std::forward<M>(m)}, std::forward<R>(r), std::forward<T>(cb)});
    }

    template <typename T>
    void on_websocket(string_view route_spec, T &&cb)
    {
      std::regex re(internal::route_spec_to_regex(route_spec));
      on_websocket_regex(re, std::forward<T>(cb));
    }

    template <typename R, typename T>
    void on_websocket_regex(R &&r, T &&cb)
    {
      ws_routes.push_back({"", std::forward<R>(r), std::forward<T>(cb)});
    }

  private:
    void start_accept(asio::io_context &io_service);

    template <typename RouteType, typename ReqType>
    const RouteType &find_route(const std::vector<RouteType> &routeList, const ReqType &req, http_route_args &a) const
    {
      auto it = find_if(routeList.begin(), routeList.end(),
                        [&req, &a](const RouteType &tuple) -> bool {
                          std::smatch m;
                          auto method = std::get<0>(tuple);
                          if (!method.compare("") || boost::iequals(method, req.method_string()))
                          {
                            std::string s{req.target()};
                            if (std::regex_search(s, m, std::get<1>(tuple)))
                            {
                              a.clear();
                              std::for_each(m.begin(), m.end(),
                                            [&a](auto item) {
                                              a.push_back(item.str());
                                            });
                              return true;
                            }
                            else
                              return false;
                          }
                          else
                            return false;
                        });
      if (it != routeList.end())
        return *it;
      else
        throw std::make_exception_ptr(std::runtime_error("route not found"));
    }

    template <typename ReqType>
    const websocket_route_tuple &find_websocket_route(const ReqType &req, http_route_args &a) const
    {
      return find_route(ws_routes, req, a);
    }

    template <typename ReqType>
    const http_route_tuple &find_http_route(const ReqType &req, http_route_args &a) const
    {
      return find_route(http_routes, req, a);
    }

    std::shared_ptr<ip::tcp::acceptor> acceptor;
    service_wptr service;

    std::vector<http_route_tuple> http_routes;
    std::vector<websocket_route_tuple> ws_routes;

    friend class http_connection;
    friend http_server_ptr make_http_server(service_ptr, string_view , uint16_t);
  };

  class http_connection : public std::enable_shared_from_this<http_connection>
  {
  private:
    struct private_
    {
    };

  public:
    ~http_connection(){};
    http_connection &operator=(const http_connection &) = delete;
    http_connection() = delete;
    http_connection(const http_connection &) = delete;
    http_connection(http_connection &&) = default;
    http_connection &operator=(http_connection &&) = default;

    // constructor for server connection
    template <typename executor_type>
    http_connection(struct private_ &&, const executor_type &io_service, http_server_ptr server)
        : http_server(http_server_wptr(server)),
          socket(io_service),
          is_websocket(false),
          is_server_connection(true){};

    // constructor for client connection
    template <typename executor_type>
    http_connection(struct private_ &&, const executor_type &io_service)
        : http_server(nullptr),
          socket(io_service),
          is_websocket(false),
          is_server_connection(false){};

    ip::tcp::socket &get_socket() { return socket; };

  private:
    void start();

    http_server_wptr http_server;
    ip::tcp::socket socket;
    bool is_websocket;
    bool is_server_connection;

    friend class http_server;
    friend http_connection_ptr make_http_connection(service_ptr as, string_view addr, string_view port);
  };

  class http_request : public std::enable_shared_from_this<http_request>
  {
  private:
    struct private_
    {
    };

  public:
    ~http_request(){};
    http_request &operator=(const http_request &) = delete;
    http_request(const http_request &) = delete;
    http_request(http_request &&) = default;
    http_request &operator=(http_request &&) = default;

    http_request()
        : beast_request(),
          headers(beast_request.base()),
          body(beast_request.body()),
          response(){};

    http_beast_request beast_request;
    http_request_headers &headers;
    http_request_body &body;

    string_view target() const
    {
      return beast_request.target();
    };

    void target(string_view t)
    {
      beast_request.target(t);
    };

    string_view method() const
    {
      return beast_request.method_string();
    };

    void method(string_view verb)
    {
      beast_request.method(beast::http::string_to_verb(verb));
    };

    struct response
    {
      response() : beast_response(),
                   headers(beast_response.base()),
                   body(beast_response.body()){};
      http_beast_response beast_response;
      http_response_headers &headers;
      http_response_body &body;
      http_result result() const
      {
        return beast_response.result_int();
      };
      void result(http_result res)
      {
        beast_response.result(res);
      };
    } response;

    private:
    boost::beast::flat_buffer buffer;

    friend class http_connection;

    template<typename D>
    friend http_request_ptr http_easy_request(service_ptr as, 
                                              string_view method, string_view url, D &&data,
                                              const std::map<string_view, string_view> &headers);
  };

  
  class websocket : public std::enable_shared_from_this<websocket>
  {
  public:
    virtual ~websocket(){};
    websocket &operator=(const websocket &) = delete;
    websocket() = delete;
    websocket(const websocket &) = delete;
    websocket(websocket &&) = default;
    websocket &operator=(websocket &&) = default;

  protected:
    websocket(string_view host_, string_view port_, string_view path_)
        : host(host_),
          port(port_),
          path(path_),
          is_server_connection(false){};

    template <typename req_type>
    websocket(req_type &&req)
        : request(std::forward<req_type>(req)),
          is_server_connection(true){};

    //API
  public:
    virtual std::string get_string(){};
    virtual void send_string(string_view s){};
    void close(websocket_close_code code)
    {
      close(code, "NORMAL");
    }
    virtual void close(websocket_close_code code, string_view reason){};
    http_request_ptr request;

  protected:
    std::string host;
    std::string port;
    std::string path;

    bool is_server_connection;
  };
  
  template <typename StreamType>
  class websocket_impl : public websocket
  {
  private:
    struct private_
    {
    };

  public:
    virtual ~websocket_impl(){};
    websocket_impl &operator=(const websocket_impl &) = delete;
    websocket_impl() = delete;
    websocket_impl(const websocket_impl &) = delete;
    websocket_impl(websocket_impl &&) = default;
    websocket_impl &operator=(websocket_impl &&) = default;


    template <typename executor_type>
    websocket_impl(struct private_ &&, const executor_type &io_service, string_view host_, string_view port_, string_view path_)
        : websocket(host_, port_, path_){};

    template <typename executor_type, typename ws_type, typename req_type>
    websocket_impl(struct private_ &&, const executor_type &io_service, ws_type &&ws, req_type &&req)
        : websocket(std::forward<req_type>(req)),
          ws(std::forward<ws_type>(ws)){};

    //API
    virtual std::string get_string()
    {
      std::string message;
      auto buffer = asio::dynamic_buffer(message);

      internal::websocket::async_read(*ws, buffer).get();
      
      return message;
    }

    virtual void send_string(string_view s)
    {
      internal::websocket::async_write(*ws, asio::buffer(s.data(), s.length())).get();
    }

    virtual void close(websocket_close_code code, string_view reason)
    {
      const boost::beast::websocket::close_reason cr(code, reason);
      internal::websocket::async_close(*ws, cr);
    }

  private:
    friend class http_connection;
    friend websocket_ptr make_websocket_connection(service_ptr as,
                                                   string_view url,
                                                   int timeout);

    std::shared_ptr<StreamType> ws;
  };

  template<typename D>
  http_request_ptr http_easy_request(service_ptr as, 
                                     string_view method, string_view url, D &&data,
                                     const std::map<string_view, string_view> &headers)
  {
    bool result;
    http_url_scheme scheme;
    
    if(http_analyze_url(url, scheme))
    {
      auto req=std::make_shared<http_request>();

      if(scheme.port==80 || scheme.port==443)
        req->headers.set("Host", scheme.host);
      else
        req->headers.set("Host", scheme.host+":"+std::to_string(scheme.port));
      req->headers.set("User-Agent", LIBASYIK_VERSION_STRING);
      req->headers.set("Content-Type", "text/html");

      // user-overidden headers
      std::for_each(headers.cbegin(), headers.cend(),
        [&req](const auto &item)
        {
          req->headers.set(item.first, item.second);
        });
    
      req->body=std::forward<D>(data);
      req->method(method);
      req->target(scheme.target);
      req->beast_request.keep_alive(false);

      req->beast_request.prepare_payload();
      
      tcp::resolver resolver(as->get_io_service().get_executor());
      tcp::resolver::results_type results = 
        internal::socket::async_resolve(resolver, scheme.host, std::to_string(scheme.port)).get();

      if(scheme.is_ssl)
      {
        // The SSL context is required, and holds certificates
        ssl::context ctx(ssl::context::tlsv12_client);

        // This holds the root certificate used for verification
        //load_root_certificates(ctx);

        ctx.set_default_verify_paths();
        ctx.set_verify_mode(ssl::verify_peer);
        

        beast::ssl_stream<beast::tcp_stream> stream(as->get_io_service().get_executor(), ctx);

        internal::socket::async_connect(beast::get_lowest_layer(stream), results).get();
        internal::ssl::async_handshake(stream, ssl::stream_base::client).get();
        internal::http::async_write(stream, req->beast_request);
        internal::http::async_read(stream, req->buffer, req->response.beast_response).get();

        internal::ssl::async_shutdown(stream).get();

        return req;
      }else
      {
        beast::tcp_stream stream(as->get_io_service().get_executor());
      
        internal::socket::async_connect(stream, results).get();
        internal::http::async_write(stream, req->beast_request);
        internal::http::async_read(stream, req->buffer, req->response.beast_response).get();

        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);

        if(ec && ec != beast::errc::not_connected)
          throw beast::system_error{ec};

        return req;
      }
    }else 
      return nullptr;
  };

  template<typename D>
  http_request_ptr http_easy_request(service_ptr as, 
                                     string_view method, string_view url, D &&data)
  {
    return http_easy_request(as, method, url, std::forward<D>(data), {});
  };

} // namespace asyik

#endif
