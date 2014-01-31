// @file concurrency_research.cpp

#include <vector>
#include <set>
#include <boost/noncopyable.hpp>
#include <boost/thread/condition.hpp>
#include "bson/util/atomic_int.h"
#include "util/concurrency/threadlocal.h"
#include "util/concurrency/mutex.h"
#include "util/concurrency/rwlock.h"
#include "util/concurrency/optimistic_rwlock.h"
#include "util/concurrency/spin_lock.h"
#include "concurrency_research.h"
#include "server.h"

using namespace std;
using namespace mongo;

/** Heirarchical lock manager

    Resources can be locked in a hierarchical fashion.
*/

using HLM::PageId;

namespace mongo { 
    struct MyState { // (TLS)
        MyState() : top(0), granular(false) { }
        int top;
        bool granular;
        map<unsigned,int> mids;
        map<PageId,int> pages;
        //vector<PageId> pages_seq;
    };
    TSP_DECLARE(MyState, myState);
    TSP_DEFINE(MyState, myState)
}

namespace {

#if defined(_WIN32)
    typedef DWORD pthread_t;
    pthread_t pthread_self() { return GetCurrentThreadId(); }
#endif

    // presumably this gets smarter later to avoid "performance collisions" too often
    // 
    // the "who" field below is to support recursion -- a simple list of what is locked 
    // in TLS for every thread would be big and messy.
    // 
    class LeafLockCache {
        enum { P = 1021 };
        struct Lock {
            Lock() : m("LeafLockCache"), who(0), n(0) { }
            SimpleMutex m;
            pthread_t who;
            int n;
        } locks[P];
        Lock& hash(PageId x) { return locks[x % P]; }
    public:
        /** @return true if already locked by us */
        void lock(PageId x) { 
            log() << " lock " << hex << x << endl;
            pthread_t me = pthread_self();  dassert( me != 0 );            
            Lock& k = hash(x);
            if( k.who == me ) {
                k.n++;
            }
            else {
                k.m.lock();
                k.who = me;
            }
        }
        void unlock(PageId x) {
            log() << " unlock " << hex << x << endl;
            Lock& k = hash(x);
            if( k.n ) { 
                k.n--;
                dassert( pthread_self() == k.me );
                return;
            }
            k.who = 0;
            k.m.unlock();
        }
    } llCache;
    SimpleRWLock &top = *(new SimpleRWLock());
    const int Q = 3;
    struct {
        SpinLock m;
        map<unsigned,SimpleRWLock*> map;
    } mids[Q];
    SimpleRWLock* getMiddle(unsigned id) { 
        unsigned i = id % Q;
        scoped_spinlock lk( mids[i].m );
        SimpleRWLock *&p = mids[i].map[id];
        if( p == 0 )
            p = new SimpleRWLock("mid");
        return p;
    }
    void lock(unsigned a) {
        getMiddle(a)->lock();
    }
    /*
    void lock(unsigned a, PageId b) {
        top.lock_shared();
        getMiddle(a)->lock_shared();
        log() << "lck " << hex << b << " a:" << a << endl;
        llCache.lock(b);
    }*/
}

namespace mongo {

    // recursive implementation
    namespace HLM {

        const int EXCLUSIVE = -1;
        const int SHARED = 1;

        bool LockAll::already() {
            return myState.getMake()->top == EXCLUSIVE;
        }
        LockAll::LockAll() { 
            //log() << "LockAll" << endl;
            _s = myState.getMake();
            fassert(1, _s->top <= 0); 
            _already = _s->top != 0;
            if( !_already ) { 
                _s->top = EXCLUSIVE;
                top.lock();
            }
            //log() << "LockAll done" << endl;
        }
        LockAll::~LockAll() { 
            //log() << "~LockAll" << endl;
            if( !_already ) {
                fassert(1, _s->top == EXCLUSIVE);
                _s->top = 0;
                top.unlock();
            }
            //log() << "~LockAll done" << endl;
        }
        bool LockMid::already(unsigned a) { 
            MyState *s = myState.getMake();
            return s->mids[a] != 0;
        }
        LockMid::LockMid(unsigned a, bool exclusive) : _a(a), _exclusive(exclusive) {
            log() << "LockMid " << a << ' ' << exclusive << endl;
            middle = 0;
            MyState *s = myState.getMake();
            if( s->top == EXCLUSIVE ) { 
                topAlready = true;
                return;
            }
            topAlready = s->top != 0;
            bool midAlready = s->mids[a] != 0;
            if( !topAlready ) { 
                s->top = SHARED;
                top.lock_shared();
            }
            if( !midAlready ) {
                s->mids[a] = exclusive ? EXCLUSIVE : SHARED;
                // given the spinlock to find the middle RWLock pointer, we remember it 
                // so that we don't have to do that again in ~LockMid().
                middle = getMiddle(a);
                if( exclusive )
                    middle->lock();
                else
                    middle->lock_shared();
            }
            log() << "LockMid done" << endl;
        }
        LockMid::~LockMid() {
            log() << "~LockMid " << topAlready << ' ' << (void*)middle << endl;
            MyState *s = myState.get();
            if( lockedMiddle() ) {
                if( _exclusive )
                    middle->unlock();
                else
                    middle->unlock_shared();
                s->mids[_a] = false; // could also do mids.clear(a) but will that cause a lot of heap thrash?
            }
            if( !topAlready ) {
                verify( s->top == 1 );
                s->top = 0;
                top.unlock_shared();
            }
            log() << "~LockMid done" << endl;
        }
        Lock::Lock(unsigned a, PageId b) :
            lkMid(a, false), _b(b)
        { 
            PageMgr::lock(a,_b);
        }
        Lock::~Lock() {
            PageMgr::unlockIfUntagged(lkMid.a(),_b);
        }

        void test() { 
            {
                HLM::LockAll lk;
            }
            {
                HLM::LockMid lk(3,true);
            }
            {
                HLM::LockMid lk(3,true);
                HLM::LockMid lk2(4,true);
            }
            {
                HLM::Lock lk(3,4);
            }
            {
                HLM::Lock lk(3,4);
                HLM::Lock lk2(2,4);
                HLM::Lock lk3(3,9);
                HLM::Lock lk4(3,9);
            }
            {
                HLM::LockAll lk;
                HLM::LockMid lk2(3,true);
                HLM::Lock lk3(3,9);
            }
            {
                HLM::LockMid lk2(3,true);
                HLM::Lock lk3(3,9);
            }
        }

        struct Test { 
            Test() { 
                //test(); 
                //test(); 
                //test(); 
           } 
        } ttttst;

        Granular::Granular(unsigned a) : _lk(a,false) {
            MyState *s = myState.get();
            _old = s->granular;
            s->granular = _lk.lockedMiddle();
            log() << "Granular " << s->granular << endl;
        }
        Granular::~Granular() {
            MyState *s = myState.get();
            s->granular = _old;
            if( !s->pages.empty() ) { 
                log() << "~Granular: w/unlockAll() " << _old << endl;
                if( !_old ) { 
                    PageMgr::unlockAll();
                }
            }
            else { 
                log() << "~Granular " << _old << endl;
            }
        } 

   } // namespace HLM

    namespace PageMgr { 

        /*unsigned epoch() {
            return myState.get()->pages_seq.size();
        }*/
        /*
        unsigned pop(unsigned whence) {
            MyState *s = myState.get();
            vector<PageId>& v = s->pages_seq;
            for( unsigned i = whence; i < v.size(); i++ ) { 
                llCache.unlock(v[i]);
                s->pages.erase(v[i]);
            }
            v.erase(v.begin()+whence, v.end());
            dassert( v.size() == s->pages.size() );
            return v.size();
        }*/

        // tag a locked item as desired.  Note that pageid can be greater 
        // than the one locked and it is assumed they are one contiguous 
        // "record" in that case; e.g. if you locked a record at address 
        // 0x02020000, and then tag 0x02020010, it will "tag" 0x02020000
        // for you if that is the closest locked address/page
        void tag(PageId b, int x) { 
            log() << "tag " << hex << b << endl;
            MyState *s = myState.getMake();
            if( s->granular ) {
                map<PageId,int>::iterator i = s->pages.upper_bound(b);
                if( i == s->pages.begin() ) {
                    log() << "ERROR PageMgr::tag no page before " << hex << b << endl;
                    fassert(1, false);
                }
                --i;
                fassert( 1 , i->first <= b );
                i->second = x;
            }
        }

        void assertTagged(PageId b) {
            log() << "atg " << hex << b << endl;
            MyState *s = myState.getMake();
            if( s->granular ) {
                map<PageId,int>::iterator i = s->pages.upper_bound(b);
                fassert(1, i != s->pages.begin());
                --i;
                fassert(1, i->second == 1);
            }
        }

        void unlockIfUntagged(unsigned a, HLM::PageId b) { 
            MyState *s = myState.getMake();
            if( s->granular ) {
                map<PageId,int>::iterator i = s->pages.find(b);
                fassert(1, i != s->pages.end() );
                if( i->second == 0 ) {
                    s->pages.erase(i);
                    llCache.unlock(b);
                }
            }
        }

        // you MUST lock the middle layer ('a') yourself previously.
        void lock(unsigned a, PageId b) {   
            log() << "lck " << hex << b << endl;
            MyState *s = myState.getMake();
            if( s->granular ) {
                map<PageId,int>::iterator i = s->pages.find(b);
                if( i != s->pages.end() )
                    return;
                s->pages.insert(i, pair<PageId,int>(b,0));
                //s->pages_seq.push_back(b);
                llCache.lock(b);
            }
        }

        /*
        template <typename Keep>
        void unlockAllExcept(Keep keep) {
            MyState *s = myState.getMake();

            set<PageId> p;
            s->pages_seq.clear();

            for( map<PageId,int>::iterator i = s->pages.begin(); i != s->pages.end(); i++ ) { 
                if( keep(*i) ) {
                    p.insert(*i);
                    s->pages_seq.push_back(*i);
                }
                else { 
                    llCache.unlock(*i);
                }
            }
            // our presumption is that a minority of pages stay locked, 
            // thus written this way
            s->pages.swap(p); 
        }*/        

        void unlockAllExcept(int tagval_tokeep) {
            MyState *s = myState.getMake();

            map<PageId,int> p;
            //s->pages_seq.clear();

            for( map<PageId,int>::iterator i = s->pages.begin(); i != s->pages.end(); i++ ) { 
                if( i->second == tagval_tokeep ) {
                    p.insert(*i);
                    //s->pages_seq.push_back(i->first);
                }
                else { 
                    llCache.unlock(i->first);
                }
            }
            // our presumption is that a minority of pages stay locked, 
            // thus written this way
            s->pages.swap(p); 
        }

        void unlockAll() {
            //ARE WE REINDEXING ALL KEYS ON AN UPDATE??? that would be bad.
            //    IS TWO PHASE STILL AROUND?

            MyState *s = myState.getMake();
            for( map<PageId,int>::iterator i = s->pages.begin(); i != s->pages.end(); i++ ) { 
                llCache.unlock(i->first);
            }
            s->pages.clear();
            //s->pages_seq.clear();
        }
    
    }

} // namespace mongo

