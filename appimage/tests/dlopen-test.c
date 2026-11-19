/* dlopen-test LIB...: loads each Vulkan layer library the way the Vulkan loader does (dlopen with
 * RTLD_NOW, so every imported symbol must resolve against this system's libraries), negotiates the
 * layer interface (the layers accept only a process whose command line contains SC_FG_MATCH /
 * SC_PROBE_MATCH, set here to this program's name), asks for vkCreateInstance and unloads it again.
 * Prints one line per library; exit 0 when all of them pass. No Vulkan driver or loader is needed.
 * Built by appimage/verify-layer.sh in the build container (glibc 2.35). */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vk_layer.h>

int main(int argc, char **argv)
{
    int failed = 0;
    setenv("SC_FG_MATCH", "dlopen-test", 1);
    setenv("SC_PROBE_MATCH", "dlopen-test", 1);
    for (int i = 1; i < argc; i++) {
        void *lib = dlopen(argv[i], RTLD_NOW | RTLD_LOCAL);
        if (!lib) {
            printf("FAIL %s: %s\n", argv[i], dlerror());
            failed = 1;
            continue;
        }
        PFN_vkNegotiateLoaderLayerInterfaceVersion negotiate =
            (PFN_vkNegotiateLoaderLayerInterfaceVersion)dlsym(lib, "vkNegotiateLoaderLayerInterfaceVersion");
        VkNegotiateLayerInterface v = {LAYER_NEGOTIATE_INTERFACE_STRUCT, NULL, 2, NULL, NULL, NULL};
        VkResult r = negotiate ? negotiate(&v) : VK_ERROR_INITIALIZATION_FAILED;
        PFN_vkVoidFunction create = r == VK_SUCCESS && v.pfnGetInstanceProcAddr
                                        ? v.pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance") : NULL;
        int ok = negotiate && r == VK_SUCCESS && v.loaderLayerInterfaceVersion == 2 && create && v.pfnGetDeviceProcAddr;
        printf("%s %s: negotiate=%d interface=%u gipa(vkCreateInstance)=%s\n", ok ? "OK  " : "FAIL", argv[i], (int)r,
               v.loaderLayerInterfaceVersion, create ? "set" : "null");
        if (dlclose(lib)) {
            printf("FAIL %s: dlclose: %s\n", argv[i], dlerror());
            ok = 0;
        }
        failed |= !ok;
    }
    return failed;
}
