#pragma once

namespace mongo {

    const int EXCLUSIVE = 0x10000;
    const int MASK      = 0x0ffff;

    /** The purpose of this class is to create a fast-path when shared locks are 
        very common and exclusive locks are rare.  Greedy semantic is maintained.

        Note this is likely not useful or slower or harmful if you exclusively lock 
        often.
    */
    class OptimisticShareableLock : boost::noncopyable {
        boost::mutex _m;
        boost::condition _doneShared;
        AtomicUInt _x;
        SimpleRWLock _r;
        void decrement() {
            unsigned res = --_x;
            if( res && (res&MASK) == 0 ) {
                // writer(s) awaits, and we were the last reader.  wake them.
                _doneShared.notify_all();
            }        
        }
    public:
        class Exclusive : boost::noncopyable {
            OptimisticShareableLock& _l;
        public:
            Exclusive(OptimisticShareableLock& l) : _l(l) {
                unsigned res = l._x.signedAdd(EXCLUSIVE);
                if( res & MASK ) { 
                    boost::mutex::scoped_lock lk(l._m);
                    while( 1 ) { 
                        if( (l._x.get() & MASK) == 0 )
                            break;
                        l._doneShared.wait(l._m);
                    }
                }
                l._r.lock();
            }
            ~Exclusive() { 
                _l._x.signedAdd(-EXCLUSIVE);
                _l._r.unlock();
            }
        };
        class Shared : boost::noncopyable {
            OptimisticShareableLock& _l;
            bool _quick;
        public:
            Shared(OptimisticShareableLock& l) : _l(l) {
                unsigned res = ++_l._x;
                _quick = res < EXCLUSIVE;
                if( !_quick ) {
                    // old fashioned approach
                    _l.decrement();       // get out of the way
                    _l._r.lock_shared();
                }
            }
            ~Shared() {
                if( _quick )
                    _l.decrement();
            }
        };
    };

}
