// 固定合成测试夹具：核对 C++ libcrypt 生成的 bcrypt 可被原 Go Auth 验证。
package main

import (
	"encoding/json"
	"os"

	"golang.org/x/crypto/bcrypt"
)

func main() {
	var sample struct{ Password, Hash string }
	if err := json.NewDecoder(os.Stdin).Decode(&sample); err != nil {
		panic("invalid synthetic fixture")
	}
	if sample.Password != "联调-password-123!" {
		panic("only synthetic fixture is accepted")
	}
	if bcrypt.CompareHashAndPassword([]byte(sample.Hash), []byte(sample.Password)) != nil {
		panic("Go rejected C++ bcrypt hash")
	}
	println("Go accepted C++ bcrypt hash")
}
