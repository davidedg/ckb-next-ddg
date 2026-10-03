#include "hwsavepipe.h"

#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <unistd.h>

namespace HwSavePipe {
namespace {
ssize_t native_write(int fd, const void* bytes, size_t length, void*) {
    return write(fd, bytes, length);
}
bool valid_utf8(const std::string& s) {
    for(size_t i = 0; i < s.size();) {
        const unsigned char a = static_cast<unsigned char>(s[i]);
        unsigned count, code, minimum;
        if(a < 0x80) { count = 1; code = a; minimum = 0; }
        else if((a & 0xe0) == 0xc0) { count = 2; code = a & 0x1f; minimum = 0x80; }
        else if((a & 0xf0) == 0xe0) { count = 3; code = a & 0x0f; minimum = 0x800; }
        else if((a & 0xf8) == 0xf0) { count = 4; code = a & 0x07; minimum = 0x10000; }
        else return false;
        if(count > s.size() - i) return false;
        for(unsigned j = 1; j < count; ++j) {
            unsigned char b = static_cast<unsigned char>(s[i + j]);
            if((b & 0xc0) != 0x80) return false;
            code = (code << 6) | (b & 0x3f);
        }
        if(code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
        i += count;
    }
    return true;
}
}

bool encode_name(const std::string& utf8, std::string& encoded) {
    encoded.clear();
    if(utf8.empty() || !valid_utf8(utf8) || utf8.size() > encoded.max_size() / 3) return false;
    static const char hex[] = "0123456789ABCDEF";
    encoded.reserve(utf8.size() * 3);
    for(unsigned char c : utf8) {
        encoded += '%';
        encoded += hex[c >> 4];
        encoded += hex[c & 15];
    }
    return true;
}

Result send_line_with(int fd, const std::string& line, Writer writer, void* context) {
    if(fd < 0 || !writer || line.empty() || line.back() != '\n' ||
       line.find('\n') != line.size() - 1 || line.find('\0') != std::string::npos)
        return {Invalid, 0};
    errno = 0;
    long limit = fpathconf(fd, _PC_PIPE_BUF);
    if(limit < 1 || line.size() > static_cast<size_t>(limit)) return {Invalid, errno};
    int flags = fcntl(fd, F_GETFL);
    if(flags < 0 || !(flags & O_NONBLOCK)) return {Invalid, flags < 0 ? errno : 0};
    ssize_t n = writer(fd, line.data(), line.size(), context);
    if(n == static_cast<ssize_t>(line.size())) return {Sent, 0};
    if(n > 0) return {Uncertain, 0};
    return {NotSent, n < 0 ? errno : 0};
}

Result send_line(int fd, const std::string& line) {
    return send_line_with(fd, line, native_write, nullptr);
}

std::string with_notification(unsigned notify, const std::string& line) {
    if(notify == 0 || notify > 9 || line.empty()) return std::string();
    if(line[0] == '@'){
        // Cache batches already own their selector. A line of a hwslot transaction names its owner: only this node's goes out
        const size_t space = line.find(' ');
        if(space != std::string::npos && line.compare(space, 8, " hwslot ") == 0 &&
           line.compare(0, space, "@" + std::to_string(notify)) != 0)
            return std::string();
        return line;
    }
    for(unsigned mode = 1; mode <= 3; ++mode){
        const std::string prefix = "mode " + std::to_string(mode);
        if(line == prefix + " get :hwsavecheck\n" || line == prefix + " hwsave\n")
            return "@" + std::to_string(notify) + " " + line;
    }
    return line;
}
}
