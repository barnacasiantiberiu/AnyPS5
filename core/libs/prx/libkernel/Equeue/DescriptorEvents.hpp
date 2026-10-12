#ifndef CORE_LIBS_PRX_LIBKERNEL_EQUEUE_DESCRIPTOREVENTS_HPP
#define CORE_LIBS_PRX_LIBKERNEL_EQUEUE_DESCRIPTOREVENTS_HPP

#include "prx/libkernel/Equeue/Equeue.hpp"

int PrepareDescriptorEvent_nid_no_patch(KernelEqueueEvent& event, std::size_t lowWatermark);
bool IsKqueueDescriptor_nid_no_patch(int descriptor);
int PollKqueueDescriptor_nid_no_patch(int descriptor, int events);
void DuplicateKqueueDescriptor_nid_no_patch(int source, int target);

#endif
