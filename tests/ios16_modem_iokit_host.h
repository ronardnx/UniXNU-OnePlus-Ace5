/* Host OS substrate for the real kernel service and broker translation units.
 * It supplies locks, allocation and worker threads, not modem business logic. */
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#define ARM64_BOARD_CONFIG_SWEET 1
#define OSDeclareDefaultStructors(x)
#define OSDefineMetaClassAndStructors(x,y)
#define THREAD_UNINT 0
#define KERN_SUCCESS 0
struct OSDictionary {};
struct IOService {
    std::atomic<unsigned> references{1};
    virtual ~IOService()=default;
    virtual bool init(OSDictionary * =nullptr){return true;}
    virtual bool start(IOService *){return true;}
    virtual void stop(IOService *){}
    virtual void free(){}
    void retain(){references++;}
    void release(){assert(references>1);references--;}
    void registerService(){}
};
struct IOLock {std::mutex mutex;std::condition_variable cv;};
struct IOSimpleLock {std::mutex mutex;};
using IOInterruptState=unsigned;
static IOSimpleLock *IOSimpleLockAlloc(){return new IOSimpleLock;}
static void IOSimpleLockFree(IOSimpleLock *lock){delete lock;}
static IOInterruptState IOSimpleLockLockDisableInterrupt(IOSimpleLock *lock){lock->mutex.lock();return 0;}
static void IOSimpleLockUnlockEnableInterrupt(IOSimpleLock *lock,IOInterruptState){lock->mutex.unlock();}
static IOLock *IOLockAlloc(){return new IOLock;}
static void IOLockFree(IOLock *p){delete p;}
static void IOLockLock(IOLock *p){p->mutex.lock();}
static void IOLockUnlock(IOLock *p){p->mutex.unlock();}
static void IOLockSleep(IOLock *p,void *,int) {
    std::unique_lock<std::mutex> lock(p->mutex,std::adopt_lock);p->cv.wait(lock);lock.release();
}
static void IOLockWakeup(IOLock *p,void *,bool){p->cv.notify_all();}
static void *IOMalloc(size_t n){return std::malloc(n);}
static void IOFree(void *p,size_t){std::free(p);}
static void IOSleep(unsigned ms){std::this_thread::sleep_for(std::chrono::milliseconds(ms));}
static void PE_parse_boot_argn(const char *name,int *value,size_t) {
    *value=!std::strcmp(name,"giulia_modem") || !std::strcmp(name,"giulia_debug");
}
static uint64_t mach_absolute_time() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void absolutetime_to_nanoseconds(uint64_t value,uint64_t *out){*out=value;}
using wait_result_t=int;
using thread_t=std::thread *;
static int kernel_thread_start(void(*fn)(void *,wait_result_t),void *arg,thread_t *out) {
    *out=new std::thread([=]{fn(arg,0);});return KERN_SUCCESS;
}
static void thread_deallocate(thread_t thread){thread->detach();delete thread;}
static void *current_thread(){return nullptr;}
static void thread_terminate(void *){}
static std::mutex modemLogLock;
static std::vector<std::string> modemLogs;
static void IOLog(const char *format,...) {
    char line[1024];va_list args;va_start(args,format);std::vsnprintf(line,sizeof(line),format,args);va_end(args);
    std::lock_guard<std::mutex> lock(modemLogLock);modemLogs.emplace_back(line);
}
