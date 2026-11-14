/*
 * Functional test for the kernelbase CPU sets patch.
 *
 * Creates named threads, applies CPU set / EcoQoS / affinity requests the way a
 * hybrid-aware game would, prints Wine's view of each thread's affinity and then
 * idles so the host-side affinity (/proc/<pid>/task/<tid>/status) can be checked.
 *
 * Build: x86_64-w64-mingw32-gcc -O2 -o test_cpusets.exe test_cpusets.c
 */
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>

typedef BOOL (WINAPI *pSetThreadSelectedCpuSets)(HANDLE, const ULONG *, ULONG);
typedef BOOL (WINAPI *pSetProcessDefaultCpuSets)(HANDLE, const ULONG *, ULONG);
typedef BOOL (WINAPI *pGetSystemCpuSetInformation)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
typedef BOOL (WINAPI *pSetThreadInformation)(HANDLE, int, void *, DWORD);
typedef HRESULT (WINAPI *pSetThreadDescription)(HANDLE, PCWSTR);
typedef NTSTATUS (WINAPI *pNtQueryInformationThread)(HANDLE, int, void *, ULONG, ULONG *);

static pSetThreadSelectedCpuSets SetThreadSelectedCpuSets_;
static pSetProcessDefaultCpuSets SetProcessDefaultCpuSets_;
static pSetThreadInformation SetThreadInformation_;
static pSetThreadDescription SetThreadDescription_;
static pNtQueryInformationThread NtQueryInformationThread_;

static ULONG pcore_id, ecore_id, ids_count;
static int have_p, have_e;

static DWORD WINAPI idle_thread( void *arg )
{
    SetThreadDescription_( GetCurrentThread(), (const WCHAR *)arg );
    for (;;) Sleep( 1000 );
    return 0;
}

static ULONG_PTR wine_affinity( HANDLE thread );

static DWORD WINAPI quick_thread( void *arg )
{
    return 0;
}

static void run_and_close( HANDLE h )
{
    ResumeThread( h );
    WaitForSingleObject( h, INFINITE );
    CloseHandle( h );
}

/* thread ids are recycled: a new thread must not inherit the CPU sets of an exited one */
static void test_tid_reuse(void)
{
    DWORD first, tid;
    HANDLE h;
    int i;

    SetProcessDefaultCpuSets_( GetCurrentProcess(), &ecore_id, 1 );
    h = CreateThread( NULL, 0, quick_thread, NULL, CREATE_SUSPENDED, &first );
    SetThreadSelectedCpuSets_( h, &pcore_id, 1 );
    run_and_close( h );
    for (i = 0; i < 5000; i++)
    {
        h = CreateThread( NULL, 0, quick_thread, NULL, CREATE_SUSPENDED, &tid );
        if (tid == first)
        {
            printf( "tid reuse: id %#lx came back after %d threads, affinity %#llx (must be the process default)\n",
                    tid, i + 1, (unsigned long long)wine_affinity( h ) );
            run_and_close( h );
            break;
        }
        run_and_close( h );
    }
    if (i == 5000) printf( "tid reuse: id never recycled\n" );
    SetProcessDefaultCpuSets_( GetCurrentProcess(), NULL, 0 );
}

static HANDLE spawn( const WCHAR *name )
{
    HANDLE h = CreateThread( NULL, 0, idle_thread, (void *)name, 0, NULL );
    Sleep( 50 );
    return h;
}

static ULONG_PTR wine_affinity( HANDLE thread )
{
    ULONG_PTR mask = 0;
    NtQueryInformationThread_( thread, 4 /* ThreadAffinityMask */, &mask, sizeof(mask), NULL );
    return mask;
}

int main( int argc, char **argv )
{
    HMODULE kb = GetModuleHandleA( "kernelbase.dll" ), nt = GetModuleHandleA( "ntdll.dll" );
    pGetSystemCpuSetInformation GetSystemCpuSetInformation_;
    SYSTEM_CPU_SET_INFORMATION *info, *p;
    ULONG len = 0, eco_state[3] = { 1, 1, 1 };  /* Version, ControlMask=EXECUTION_SPEED, StateMask=EXECUTION_SPEED */
    HANDLE a, b, c, d, e, f, g;
    int hold = argc > 1 ? atoi( argv[1] ) : 6;

    GetSystemCpuSetInformation_ = (void *)GetProcAddress( kb, "GetSystemCpuSetInformation" );
    SetThreadSelectedCpuSets_ = (void *)GetProcAddress( kb, "SetThreadSelectedCpuSets" );
    SetProcessDefaultCpuSets_ = (void *)GetProcAddress( kb, "SetProcessDefaultCpuSets" );
    SetThreadInformation_ = (void *)GetProcAddress( kb, "SetThreadInformation" );
    SetThreadDescription_ = (void *)GetProcAddress( kb, "SetThreadDescription" );
    NtQueryInformationThread_ = (void *)GetProcAddress( nt, "NtQueryInformationThread" );

    GetSystemCpuSetInformation_( NULL, 0, &len, GetCurrentProcess(), 0 );
    info = malloc( len );
    if (!GetSystemCpuSetInformation_( info, len, &len, GetCurrentProcess(), 0 )) return 1;
    for (p = info; (char *)p < (char *)info + len; p = (void *)((char *)p + p->Size))
    {
        printf( "cpuset id %#lx cpu %2u core %2u class %u\n", p->CpuSet.Id, p->CpuSet.LogicalProcessorIndex,
                p->CpuSet.CoreIndex, p->CpuSet.EfficiencyClass );
        if (p->CpuSet.EfficiencyClass && !have_p) { pcore_id = p->CpuSet.Id; have_p = 1; }
        if (!p->CpuSet.EfficiencyClass && !have_e) { ecore_id = p->CpuSet.Id; have_e = 1; }
        ids_count++;
    }
    if (!have_p) { pcore_id = info[0].CpuSet.Id; }

    a = spawn( L"t-pcore" );
    SetThreadSelectedCpuSets_( a, &pcore_id, 1 );

    b = spawn( L"t-ecore" );
    SetThreadSelectedCpuSets_( b, &ecore_id, 1 );

    c = spawn( L"t-ecoqos" );
    {
        ULONG normal_state[3] = { 1, 1, 0 };  /* explicitly "not throttled" */
        HANDLE h = spawn( L"t-ecoqos-off" );
        BOOL ret = SetThreadInformation_( c, 3 /* ThreadPowerThrottling */, eco_state, sizeof(eco_state) );
        printf( "ecoqos: ret %d affinity %#llx", ret, (unsigned long long)wine_affinity( c ) );
        SetThreadInformation_( h, 3, eco_state, sizeof(eco_state) );
        SetThreadInformation_( h, 3, normal_state, sizeof(normal_state) );
        printf( "; on then off: %#llx\n", (unsigned long long)wine_affinity( h ) );
    }

    d = spawn( L"t-cleared" );
    SetThreadSelectedCpuSets_( d, &pcore_id, 1 );
    SetThreadSelectedCpuSets_( d, NULL, 0 );

    e = spawn( L"t-affinity" );
    SetThreadAffinityMask( e, 0x3 );
    SetThreadSelectedCpuSets_( e, &ecore_id, 1 );   /* conflicts: affinity must win */

    f = spawn( L"t-default" );
    SetProcessDefaultCpuSets_( GetCurrentProcess(), &ecore_id, 1 );
    g = spawn( L"t-newdefault" );

    printf( "wine view: pcore=%#llx ecore=%#llx ecoqos=%#llx cleared=%#llx affinity=%#llx default=%#llx newdefault=%#llx\n",
            (unsigned long long)wine_affinity( a ), (unsigned long long)wine_affinity( b ), (unsigned long long)wine_affinity( c ), (unsigned long long)wine_affinity( d ),
            (unsigned long long)wine_affinity( e ), (unsigned long long)wine_affinity( f ), (unsigned long long)wine_affinity( g ) );
    printf( "READY pid %lu\n", GetCurrentProcessId() );
    fflush( stdout );
    Sleep( hold * 1000 );

    /* clearing the process default must give everything back */
    SetProcessDefaultCpuSets_( GetCurrentProcess(), NULL, 0 );
    printf( "after clearing default: default=%#llx newdefault=%#llx\n", (unsigned long long)wine_affinity( f ), (unsigned long long)wine_affinity( g ) );
    test_tid_reuse();
    fflush( stdout );
    return 0;
}
