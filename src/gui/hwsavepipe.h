#ifndef HWSAVEPIPE_H
#define HWSAVEPIPE_H

#include <cstddef>
#include <string>
#include <sys/types.h>

// One whole command line to a nonblocking FIFO. No buffering and no retry of a
// partially delivered line. This is the transport primitive of the save flow.
namespace HwSavePipe {
enum Status { Sent, NotSent, Uncertain, Invalid };
struct Result {
    Status status;
    int error; // errno for NotSent; zero otherwise
};
typedef ssize_t (*Writer)(int, const void*, size_t, void*);

Result send_line(int fd, const std::string& line);
// Only request/reply commands need a notification selector. Preparation lines
// are deliberately left unchanged, including at the 512-byte minimum limit.
// An "@N hwslot" line is returned only when N is this node (else empty).
std::string with_notification(unsigned notify, const std::string& line);
Result send_line_with(int fd, const std::string& line, Writer writer, void* context);

// Percent-encode *every* UTF-8 byte, including ASCII letters. An empty or
// malformed UTF-8 name has no encoding. The caller then checks line length.
bool encode_name(const std::string& utf8, std::string& encoded);
}

#endif
