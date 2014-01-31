#pragma once

#include "diskloc.h"
#include "concurrency_research.h"
#include "client.h"
#include "database.h"

namespace mongo { 

    namespace RecLocker {
        inline size_t quantize(void *mmf_ptr) { 
            /** to be made smarter later.  
                we should quantize some so that we aren't doing a gigantic 
                number of microlocks.  however not every smart yet.
                8192 to avoid any weirdness with deadlocks intra-btree bucket.
                (caveat: v0 indexes are not 8kb aligned)
                also this isn't great as for example two different namespacedetails
                might be adjacent.  so more to do later.
            */
            // note we divide rather than just masking the low bits as the lock 
            // code might hash the 'b' parameter we send it so just clearing all
            // the low bits is likely bad idea
            return ((size_t) mmf_ptr) / 8192;
        }
        inline unsigned tempHash(const StringData& dbOrNs) { 
            return StringData::Hasher()(nsToDatabaseSubstring(dbOrNs));
        }
        inline void lock(const StringData& ns, void *mmf_ptr) {
            PageMgr::lock(tempHash(ns),quantize(mmf_ptr));
        }
        inline void lock(void *mmf_ptr) {
            Client&c = cc();
            Database *db = c.database();
            PageMgr::lock(db->ordinal, quantize(mmf_ptr));
        }
        inline void tag(void *p) { 
            PageMgr::tag(quantize(p), 1);
        }
        inline void assertTagged(void *p) { 
            PageMgr::assertTagged(quantize(p));
        }
        /**inline void unlockAllExcept(DiskLoc loc) { 
            unsigned b = loc.a() << 20 | loc.getOfs() >> 12;
            PageMgr::unlockAll( cc().database()->ordinal, b );
        }*/
        void unlockNonTagged();
        // Scoped lock of a pointer / page -- recursion is ok
        class Scoped : HLM::Lock {
        public:
            Scoped(void *mmf_ptr) : 
              HLM::Lock( cc().database()->ordinal, quantize(mmf_ptr) ) { }
        };
    }
    class GranularForDB : public HLM::Granular { 
    public:
        GranularForDB(const StringData& dbOrNs) : 
          HLM::Granular( RecLocker::tempHash(dbOrNs) ) { }
    };

}
