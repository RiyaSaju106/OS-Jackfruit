#include <stdio.h>
#include <unistd.h>

int main() {
    printf("Starting IO pulse...\n");

    for (int i = 0; i < 20; i++) {
        printf("Pulse %d\n", i);
        fflush(stdout);
        sleep(1);
    }

    printf("Finished IO pulse\n");
    return 0;
}

