#pragma once
#include "gateway/protocol.hpp"
#include <memory>
namespace mmo::gateway {
class Runtime {
public:
    explicit Runtime(Config config);
    ~Runtime();
    int run();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
