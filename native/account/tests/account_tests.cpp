#include "account/account.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace native::account;
namespace {
void require(bool valid, const char* reason) { if (!valid) throw std::runtime_error(reason); }
template<class Callback> void rejects(Callback callback) {
    try { callback(); } catch (const ApiError& error) { require(error.status == 400, "unexpected validation status"); return; }
    throw std::runtime_error("invalid input was accepted");
}
}
int main(int argc, char**) {
    try {
        if (argc > 1) {
            // 仅固定合成测试密码，供 Go bcrypt 回归读取，不接收真实凭据。
            std::cout << Json{{"password", "联调-password-123!"}, {"hash", hash_password("联调-password-123!")}}.dump() << '\n';
            return 0;
        }
        require(validated_name("角色Alpha") == "角色Alpha", "unicode name changed");
        require(validated_name("\xE3\x80\x80角色\xC2\xA0", true) == "角色", "legacy unicode trim changed");
        rejects([] { validated_name("角色 "); });
        rejects([] { validated_name("\xC2\xA0角色"); });
        rejects([] { validated_name(std::string(25, 'x')); });
        rejects([] { validated_name("\xC0\xAF"); });
        rejects([] { validated_name("\xED\xA0\x80"); });
        rejects([] { validated_name("x\xC2\x85y"); });
        rejects([] { validate_password("1234567"); });
        rejects([] { validate_password(std::string(73, 'x')); });
        rejects([] { validate_password(std::string("12345678\0", 9)); });
        rejects([] { validate_password("12345678\xC2\x80"); });
        validate_password(std::string(72, 'x'));
        require(hash_token("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "token SHA-256 changed");
        require(equal_secret("key", "key") && !equal_secret("key", "Key") && !equal_secret("key", "key!"), "secret compare failed");
        // 以下 hash 由仓库依赖 golang.org/x/crypto/bcrypt cost10 生成。
        require(verify_password("$2a$10$oUN8qo7QRJ4Mp/DNocTFyeOlBD7F9FbxlfLImoFp91gaoGuNeZAA2", "password-123!"), "Go ASCII bcrypt rejected");
        require(verify_password("$2a$10$5cAfhnaXy5oiH5LG6mKdOuWxwHHEi/ZtbjFYAj2SJyRJ4f/3J600G", "联调-password-123!"), "Go UTF8 bcrypt rejected");
        std::string maximum;
        for (int n = 0; n < 36; ++n) maximum += "é";
        require(maximum.size() == 72 && verify_password("$2a$10$k9dGh/yVk8YCmLHtV5U/DezgDYtgH.V51Y6Da9qxxgpObqHqaINz.", maximum), "Go 72-byte bcrypt rejected");
        auto one = hash_password("联调-password-123!"), two = hash_password("联调-password-123!");
        require(one != two && one.substr(0, 7) == "$2b$10$", "bcrypt salt or cost incorrect");
        require(verify_password(one, "联调-password-123!") && !verify_password(one, "wrong-password!"), "bcrypt compare incorrect");
        rejects([] { parse_request("{\"name\":\"one\",\"name\":\"two\"}"); });
        rejects([] { parse_request("{} {}"); });
        rejects([] { parse_request("[]"); });
        rejects([] { parse_request("{\"x\":" + std::string(24, '[') + "0" + std::string(24, ']') + "}"); });
        std::cout << "Account UTF-8, Go bcrypt compatibility, random salt and parser invariants passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
