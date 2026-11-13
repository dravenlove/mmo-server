#include "gateway/runtime.hpp"
#include <iostream>
int main() {
    try { return mmo::gateway::Runtime(mmo::gateway::Config::environment()).run(); }
    catch (const std::exception& error) {
        // 配置错误不打印环境变量或任何请求正文。
        std::cerr << "native-gateway: " << error.what() << '\n';
        return 1;
    }
}
