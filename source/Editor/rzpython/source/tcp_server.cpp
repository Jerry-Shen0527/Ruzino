#include <spdlog/spdlog.h>

#include <asio.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/use_awaitable.hpp>
#include <atomic>
#include <memory>
#include <rzpython/rzpython.hpp>
#include <rzpython/tcp_server.hpp>

RUZINO_NAMESPACE_OPEN_SCOPE

namespace python {

class PythonTcpServer::Impl : public std::enable_shared_from_this<Impl> {
   public:
    Impl(int port)
        : port_(port),
          running_(false),
          io_context_(),
          acceptor_(io_context_)
    {
    }

    ~Impl()
    {
        stop();
    }

    void set_execute_callback(ExecuteCallback callback)
    {
        execute_callback_ = std::move(callback);
    }

    void start()
    {
        if (running_.exchange(true)) {
            spdlog::warn("TCP server already running on port {}", port_);
            return;
        }

        try {
            asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), port_);
            acceptor_.open(endpoint.protocol());
            acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
            acceptor_.bind(endpoint);
            acceptor_.listen();

            asio::co_spawn(io_context_, listener(), asio::detached);

            thread_ = std::thread([this]() {
                try {
                    io_context_.run();
                }
                catch (const std::exception& e) {
                    spdlog::error("IO context error: {}", e.what());
                }
            });

            spdlog::info("Python TCP server started on port {}", port_);
        }
        catch (const std::exception& e) {
            running_ = false;
            spdlog::error("Failed to start TCP server: {}", e.what());
        }
    }

    void stop()
    {
        if (!running_.exchange(false))
            return;

        io_context_.stop();

        if (thread_.joinable()) {
            thread_.join();
        }

        std::error_code ec;
        acceptor_.close(ec);
        spdlog::info("Python TCP server stopped");
    }

    bool is_running() const
    {
        return running_;
    }
    int get_port() const
    {
        return port_;
    }

   private:
    asio::awaitable<void> listener()
    {
        auto executor = co_await asio::this_coro::executor;

        while (running_) {
            try {
                auto socket = std::make_shared<asio::ip::tcp::socket>(executor);
                co_await acceptor_.async_accept(*socket, asio::use_awaitable);

                auto endpoint = socket->remote_endpoint();
                spdlog::info(
                    "Client connected from {}:{}",
                    endpoint.address().to_string(),
                    endpoint.port());

                asio::co_spawn(
                    executor,
                    handle_session(std::move(*socket)),
                    asio::detached);
            }
            catch (const std::system_error& e) {
                if (running_) {
                    spdlog::error("Accept error: {}", e.what());
                }
            }
            catch (const std::exception& e) {
                if (running_) {
                    spdlog::error("Accept error: {}", e.what());
                }
            }
        }
    }

    asio::awaitable<void> handle_session(asio::ip::tcp::socket socket)
    {
        std::string accumulated_code;
        std::array<char, 4096> buffer;

        PyGILState_STATE gstate = PyGILState_Ensure();

        try {
            while (running_) {
                std::size_t bytes_read = co_await socket.async_read_some(
                    asio::buffer(buffer), asio::use_awaitable);

                accumulated_code.append(buffer.data(), bytes_read);

                std::size_t exec_pos;
                while ((exec_pos = accumulated_code.find("\n---EXEC---\n")) !=
                       std::string::npos) {
                    std::string code = accumulated_code.substr(0, exec_pos);
                    accumulated_code = accumulated_code.substr(exec_pos + 11);

                    std::string response;
                    try {
                        if (execute_callback_) {
                            response = execute_callback_(code);
                        }
                        else {
                            auto [success, error] =
                                python::execute_with_error(code);
                            python::flush_python_output();
                            response =
                                success ? "OK\n" : "ERROR: " + error + "\n";
                        }
                    }
                    catch (const std::exception& e) {
                        spdlog::error("Python execution error: {}", e.what());
                        response = "ERROR: " + std::string(e.what()) + "\n";
                    }

                    co_await asio::async_write(
                        socket, asio::buffer(response), asio::use_awaitable);
                }
            }
        }
        catch (const std::system_error& e) {
            if (e.code() != asio::error::eof &&
                e.code() != asio::error::connection_reset) {
                spdlog::error("Session error: {}", e.what());
            }
            else {
                spdlog::info("Client disconnected");
            }
        }
        catch (const std::exception& e) {
            spdlog::error("Session error: {}", e.what());
        }

        PyGILState_Release(gstate);

        std::error_code ec;
        socket.close(ec);
    }

    int port_;
    std::atomic<bool> running_;
    asio::io_context io_context_;
    asio::ip::tcp::acceptor acceptor_;
    std::thread thread_;
    ExecuteCallback execute_callback_;
};

PythonTcpServer::PythonTcpServer(int port) : impl_(std::make_unique<Impl>(port))
{
}

PythonTcpServer::~PythonTcpServer() = default;

void PythonTcpServer::set_execute_callback(ExecuteCallback callback)
{
    impl_->set_execute_callback(std::move(callback));
}

void PythonTcpServer::start()
{
    impl_->start();
}

void PythonTcpServer::stop()
{
    impl_->stop();
}

bool PythonTcpServer::is_running() const
{
    return impl_->is_running();
}

int PythonTcpServer::get_port() const
{
    return impl_->get_port();
}

std::shared_ptr<PythonTcpServer> create_python_tcp_server(int port)
{
    return std::make_shared<PythonTcpServer>(port);
}

}  // namespace python

RUZINO_NAMESPACE_CLOSE_SCOPE
