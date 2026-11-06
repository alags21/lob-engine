#include <windows.h>
#include <powrprof.h>
#include <cstdio>
#include <vector>
#pragma comment(lib, "powrprof.lib")
struct PPI { ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState; };
int main() {
  ULONG len = 0;
  GetSystemCpuSetInformation(nullptr, 0, &len, GetCurrentProcess(), 0);
  std::vector<char> buf(len);
  GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf.data(), len, &len, GetCurrentProcess(), 0);
  SYSTEM_INFO si; GetSystemInfo(&si);
  std::vector<PPI> ppi(si.dwNumberOfProcessors);
  CallNtPowerInformation(ProcessorInformation, nullptr, 0, ppi.data(), (ULONG)(ppi.size() * sizeof(PPI)));
  for (char* p = buf.data(); p < buf.data() + len;) {
    auto* e = (SYSTEM_CPU_SET_INFORMATION*)p;
    if (e->Type == CpuSetInformation) {
      auto& c = e->CpuSet;
      unsigned idx = c.LogicalProcessorIndex;
      std::printf("logical %2u core %2u effclass %u maxmhz %lu curmhz %lu\n", idx, c.CoreIndex, c.EfficiencyClass, idx < ppi.size() ? ppi[idx].MaxMhz : 0, idx < ppi.size() ? ppi[idx].CurrentMhz : 0);
    }
    p += e->Size;
  }
}
