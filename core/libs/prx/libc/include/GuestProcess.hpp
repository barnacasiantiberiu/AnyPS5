#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_GUESTPROCESS_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_GUESTPROCESS_HPP

struct GuestForkHooks {
    // Prepare runs in increasing priority; parent/child restore in reverse.
    // The callback whose prepare returns false or throws is also restored, so
    // callbacks must track acquired resources and tolerate partial preparation.
    // Parent restoration completes before a child may return to guest code.
    // Callbacks may not reenter GuestFork or mutate the hook registry.
    unsigned priority;
    bool (*prepare)(void* context);
    bool (*parent)(void* context, bool childCreated) noexcept;
    bool (*child)(void* context) noexcept;
    void* context;
};

extern "C" {

int GuestRegisterForkHooks_nid_no_patch(const GuestForkHooks* hooks);
int GuestFork_nid_no_patch();
int GuestWaitPid_nid_no_patch(int pid, int* status, int options);
int GuestSetSid_nid_no_patch();
int GuestGetSid_nid_no_patch(int pid);
int GuestGetProcessGroup_nid_no_patch(int pid);
int GuestGetParentPid_nid_no_patch();

}

#endif
