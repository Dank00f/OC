#include <iostream>
int main() {
    std::cout << "Hello, world!\n";
#if defined(_WIN32)
    std::cout << "Platform: Windows\n";
#elif defined(__linux__)
    std::cout << "Platform: Linux\n";
#endif
    return 0;
}
