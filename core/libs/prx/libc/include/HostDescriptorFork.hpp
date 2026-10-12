#pragma once

extern "C" {

int RegisterHostDescriptorFork_nid_no_patch();
void BeginHostDescriptorOperation_nid_no_patch();
void EndHostDescriptorOperation_nid_no_patch() noexcept;

}

class HostDescriptorOperation {
public:
    HostDescriptorOperation() { BeginHostDescriptorOperation_nid_no_patch(); }
    ~HostDescriptorOperation() { EndHostDescriptorOperation_nid_no_patch(); }
    HostDescriptorOperation(const HostDescriptorOperation&) = delete;
    HostDescriptorOperation& operator=(const HostDescriptorOperation&) = delete;
};
