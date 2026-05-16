//
//  base64 encoding and decoding with C++.
//  Version: 2.rc.09 (release candidate)
//

#ifndef BASE64_H_C0CE2A47_D10E_42C9_A27C_C883944E704A
#define BASE64_H_C0CE2A47_D10E_42C9_A27C_C883944E704A

#include <string>

#if __cplusplus >= 201703L
#include <string_view>
#endif  // __cplusplus >= 201703L

// 该头文件声明了 Base64 编码和解码的函数接口，
// 用于将二进制数据转换为可打印的 ASCII 字符串。

// 标准 Base64 编码：将字符串编码为 Base64
// 参数 s: 要编码的输入字符串
// 参数 url: 是否使用 URL 安全的字符集（默认 false）
// 返回: Base64 编码后的字符串
std::string base64_encode     (std::string const& s, bool url = false);

// PEM 格式 Base64 编码：用于证书等，每行 64 字符
// 参数 s: 要编码的输入字符串
// 返回: PEM 格式的 Base64 字符串
std::string base64_encode_pem (std::string const& s);

// MIME 格式 Base64 编码：用于邮件附件等，每行 76 字符
// 参数 s: 要编码的输入字符串
// 返回: MIME 格式的 Base64 字符串
std::string base64_encode_mime(std::string const& s);

// Base64 解码：将 Base64 字符串解码为原始数据
// 参数 s: 要解码的 Base64 字符串
// 参数 remove_linebreaks: 是否移除输入中的换行符（默认 false）
// 返回: 解码后的原始字符串
std::string base64_decode(std::string const& s, bool remove_linebreaks = false);

// 编码字节数组：将二进制数据编码为 Base64
// 参数: 二进制数据指针
// 参数 len: 数据长度
// 参数 url: 是否使用 URL 安全的字符集
// 返回: Base64 编码后的字符串
std::string base64_encode(unsigned char const*, size_t len, bool url = false);

#if __cplusplus >= 201703L
//
// Interface with std::string_view rather than const std::string&
// Requires C++17
// Provided by Yannic Bonenberger (https://github.com/Yannic)
//

// 以下是 C++17 版本的函数，使用 string_view 提高效率，避免拷贝

// 标准 Base64 编码（string_view 版本）
std::string base64_encode     (std::string_view s, bool url = false);

// PEM 格式编码（string_view 版本）
std::string base64_encode_pem (std::string_view s);

// MIME 格式编码（string_view 版本）
std::string base64_encode_mime(std::string_view s);

// Base64 解码（string_view 版本）
std::string base64_decode(std::string_view s, bool remove_linebreaks = false);
#endif  // __cplusplus >= 201703L

#endif /* BASE64_H_C0CE2A47_D10E_42C9_A27C_C883944E704A */
