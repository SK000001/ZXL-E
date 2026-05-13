#include <cstdio>
#include <cuda_runtime.h>
__global__ void k(int *x) { *x = 42; }
int main() {
    int dev_count = 0;
    cudaGetDeviceCount(&dev_count);
    printf("cuda devices: %d\n", dev_count);
    if (dev_count == 0) return 1;
    cudaDeviceProp p;
    cudaGetDeviceProperties(&p, 0);
    printf("dev0: %s  sm_%d%d  vram=%zu MiB\n", p.name, p.major, p.minor, p.totalGlobalMem >> 20);
    int *d, h = 0;
    cudaMalloc(&d, sizeof(int));
    k<<<1,1>>>(d);
    cudaMemcpy(&h, d, sizeof(int), cudaMemcpyDeviceToHost);
    cudaFree(d);
    printf("kernel returned: %d\n", h);
    return h == 42 ? 0 : 1;
}
