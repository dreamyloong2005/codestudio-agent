import Foundation
import Virtualization

// Capability query only: no VM configuration, guest, disk, or network is created.
print("{\"supported\":\(VZVirtualMachine.isSupported),\"minimum_cpus\":\(VZVirtualMachineConfiguration.minimumAllowedCPUCount),\"maximum_cpus\":\(VZVirtualMachineConfiguration.maximumAllowedCPUCount),\"minimum_memory_bytes\":\(VZVirtualMachineConfiguration.minimumAllowedMemorySize),\"maximum_memory_bytes\":\(VZVirtualMachineConfiguration.maximumAllowedMemorySize)}")
