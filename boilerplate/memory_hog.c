#include <stdlib.h>
#include <string.h>

int main() {
    while (1) {
        void *p = malloc(10*1024*1024);
        memset(p, 0, 10*1024*1024);
    }
}
