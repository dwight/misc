#include "rec_locker.h"
#include "concurrency_research.h"

namespace mongo { 

    /**
    class KeepMutated { 
    public:
        bool operator() (HLM::PageId p) { 
            // todo
            // nto sure best way to do given that we are locking a little different than the write intent address???
            n
            return false;
        }

    };*/

    void RecLocker::unlockNonTagged() { 
        PageMgr::unlockAllExcept(1);
    }

}


