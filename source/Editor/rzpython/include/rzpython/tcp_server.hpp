#pragma once

#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "api.h"

RUZINO_NAMESPACE_OPEN_SCOPE

namespace python {

class RZPYTHON_API PythonTcpServer {
   public:
    using ExecuteCallback = std::function<std::string(const std::string& code)>;

    PythonTcpServer(int port = 5555);
    ~PythonTcpServer();

    void set_execute_callback(ExecuteCallback callback);
    void start();
    void stop();
    bool is_running() const;
    int get_port() const;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

RZPYTHON_API std::shared_ptr<PythonTcpServer> create_python_tcp_server(
    int port = 5555);

}  // namespace python

RUZINO_NAMESPACE_CLOSE_SCOPE
