#include <stdio.h>

int main(int argc, char **argv) {
    (void)argv;
    printf("ext4-dos host inspector\n");
    if (argc < 2) {
        fprintf(stderr, "usage: host_cli <ext4-image>\n");
        return 1;
    }
    return 0;
}
