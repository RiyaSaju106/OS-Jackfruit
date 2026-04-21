#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    int mb = 50; // default
    if (argc > 1) {
        mb = atoi(argv[1]);
    }

    printf("Allocating %d MB...\n", mb);

    size_t size = mb * 1024 * 1024;
    char *mem = malloc(size);

    if (!mem) {
        perror("malloc failed");
        return 1;
    }

    // Touch memory to ensure RSS grows
    for (size_t i = 0; i < size; i += 4096) {
        mem[i] = 1;
    }

    printf("Holding memory...\n");
    sleep(30); // keep process alive

    free(mem);
    return 0;
}
