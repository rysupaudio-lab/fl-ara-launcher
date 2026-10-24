#include "FLNativeMixBoundary.h"
#include <cstdio>
#include <cstdlib>
#include <cerrno>
using namespace rysup::fl_ara;
static void need(bool okay,const char* text){if(!okay){std::fprintf(stderr,"FAIL: %s\n",text);std::exit(1);}}
int main(){
    pthread_mutexattr_t attributes;need(pthread_mutexattr_init(&attributes)==0,"attributes");
    need(pthread_mutexattr_settype(&attributes,PTHREAD_MUTEX_RECURSIVE)==0,"recursive type");
    pthread_mutex_t mutex;need(pthread_mutex_init(&mutex,&attributes)==0,"mutex init");
    need(pthread_mutexattr_destroy(&attributes)==0,"attributes destroy");
    const auto free1=detail::probeInitializedMutexFromOwnedWorker(&mutex);
    need(free1.available()&&free1.tryLockResult==0&&free1.unlockResult==0,"free mutex available and released");
    need(pthread_mutex_lock(&mutex)==0,"main outer lock1");
    const auto held1=detail::probeInitializedMutexFromOwnedWorker(&mutex);
    need(!held1.available()&&held1.reason==NativeMixBoundaryReason::Contended&&held1.tryLockResult==EBUSY,"worker refuses recursive main outer lock");
    need(pthread_mutex_lock(&mutex)==0,"main recursive lock2");
    need(!detail::probeInitializedMutexFromOwnedWorker(&mutex).available(),"worker refuses recursive depth2");
    need(pthread_mutex_unlock(&mutex)==0,"main releases inner only");
    need(!detail::probeInitializedMutexFromOwnedWorker(&mutex).available(),"inner release does not falsely prove no outer lock");
    need(pthread_mutex_unlock(&mutex)==0,"main releases final outer");
    const auto free2=detail::probeInitializedMutexFromOwnedWorker(&mutex);
    need(free2.available()&&free2.tryLockResult==0&&free2.unlockResult==0,"fully released mutex available and released");
    need(pthread_mutex_destroy(&mutex)==0,"worker left no lock held");
    std::puts("PASS: free, main-held recursive1/2, inner-only release and full release; no DAW loaded");
}
