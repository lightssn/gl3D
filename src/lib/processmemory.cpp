#include "processmemory.h"
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <vector>
#endif
ProcessMemory processMemory() {
    ProcessMemory result;
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if(!K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),sizeof(counters))) return result;
    result.valid=true; result.workingSet=qint64(counters.WorkingSetSize); result.privateCommit=qint64(counters.PrivateUsage);
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    // Shared pages are excluded from Task Manager's private working-set column.
    size_t entries=size_t(counters.WorkingSetSize/system.dwPageSize)+8192;
    for(int attempt=0;attempt<3;++attempt) {
        std::vector<ULONG_PTR> buffer(entries+1);
        if(K32QueryWorkingSet(GetCurrentProcess(),buffer.data(),DWORD(buffer.size()*sizeof(ULONG_PTR)))) {
            const auto* working=reinterpret_cast<const PSAPI_WORKING_SET_INFORMATION*>(buffer.data());
            for(ULONG_PTR i=0;i<working->NumberOfEntries;++i) if(!working->WorkingSetInfo[i].Shared) result.privateWorkingSet+=system.dwPageSize;
            result.privateWorkingSetValid=true; break;
        }
        if(GetLastError()!=ERROR_BAD_LENGTH) break;
        entries*=2;
    }
#endif
    return result;
}
