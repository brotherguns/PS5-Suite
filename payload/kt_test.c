#include <stdio.h>
#include <unistd.h>
#include <ps5/kernel.h>

int main() {
    pid_t me = getpid();
    printf("authid = %lx\n", (unsigned long)kernel_get_ucred_authid(me));
    printf("rootvnode = %lx\n", (unsigned long)kernel_get_root_vnode());
    uint8_t caps[16];
    int r = kernel_get_ucred_caps(me, caps);
    printf("caps rc = %d, caps[0]=%02x\n", r, caps[0]);
    return 0;
}
