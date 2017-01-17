#ifndef LOG_H
#define LOG_H

#include <ostream>
#include <cstring>

void print(std::ostream& os, const char* fmt) {
    assert(!strchr(fmt, '%') && "Not enough arguments to print");
    os << fmt << std::endl;
}

template <typename T, typename... Args>
void print(std::ostream& os, const char* fmt, const T& t, Args... args) {
    auto ptr = strchr(fmt, '%');
    while (ptr && ptr[1] == '%') ptr = strchr(ptr + 2, '%');
    assert(ptr && "Too many arguments to print");
    os.write(fmt, ptr - fmt);
    os << t;
    print(os, ptr + 1, args...);
}

template <typename... Args>
void error(Args... args) {
    print(std::cerr, args...);
    abort();
}

template <typename... Args>
void info(Args... args) {
    print(std::cout, args...);
}

template <typename... Args>
void debug(Args... args) {
#ifndef NDEBUG
    print(std::cout, args...);
#endif
}

#endif