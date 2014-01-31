#pragma once

namespace mongo {
    class SimpleRWLock;
    struct MyState;
    namespace HLM { 
        typedef unsigned long long PageId;
        class LockAll { 
            bool _already;
            MyState *_s;
        public:
            LockAll();
            ~LockAll();
            static bool already();
        };
        class LockMid { 
            unsigned _a;
            bool _exclusive, topAlready;
            SimpleRWLock *middle;
        public:
            LockMid(unsigned a, bool exclusive); // use false for exclusive.  exclusive=true is for internal impl
            ~LockMid();
            static bool already(unsigned a);
            bool lockedMiddle() const { return middle != 0; }
            unsigned a() const { return _a; }
        };
        class Lock { 
            LockMid lkMid;
            PageId _b;
            bool _already;
        public:
            Lock(unsigned a, PageId b);
            ~Lock();
        };
        // declare our intention to use granular locking (PageMgr) within the scoped 
        // code block.  takes out a shared lock on a and then use PageMgr to lock 
        // anything touched thereunder (whether you are writing OR reading).
        class Granular { 
            LockMid _lk;
            bool _old;
        public:
            Granular(unsigned a);
            ~Granular();
        };
    }
    namespace PageMgr { 
        // lock a "page" or thing (could be a record).  
        // at moment a is database ordinal and b is from DiskLoc, but this will evolve.
        // the intent here is to keep unrelated db-concepts (DiskLoc, Database) out of 
        // this module thus the genericism.
        void lock(unsigned a, HLM::PageId b);

        void unlockIfUntagged(unsigned a, HLM::PageId b);

        /** release all our locks for our thread */
        void unlockAll();

        void tag(HLM::PageId b, int tagval);

        void assertTagged(HLM::PageId b);

        //template <typename F>
        void unlockAllExcept(int tagval);

        /** Selective release some locks with following methods.  example:
            
            ...code...
            unsigned e = PageMgr::epoch();
            doSomething();
            PageMgr::pop(e)

            When you call pop you are saying "i am done with those pages / locks".  So be very 
            sure you don't have dangling references.

            Note if before epoch() earlier code already locked a page, it will still be locked 
            after the pop(), even if locked again in doSomething().  So you don't need to reason
            about any code outside of the contained block in question.
         */
        unsigned epoch();
        unsigned pop(unsigned whence);
    }
}
