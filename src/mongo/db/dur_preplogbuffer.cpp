// @file dur_preplogbuffer.cpp

/**
*    Copyright (C) 2009 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

/*
     PREPLOGBUFFER
       we will build an output buffer ourself and then use O_DIRECT
       we could be in read lock for this
       for very large objects write directly to redo log in situ?
     @see https://docs.google.com/drawings/edit?id=1TklsmZzm7ohIZkwgeK6rMvsdaR13KjtJYMsfLr175Zc
*/

#include "mongo/pch.h"

#include "mongo/db/dur.h"
#include "mongo/db/dur_commitjob.h"
#include "mongo/db/dur_journal.h"
#include "mongo/db/dur_journalimpl.h"
#include "mongo/db/dur_stats.h"
#include "mongo/server.h"
#include "mongo/util/alignedbuilder.h"
#include "mongo/util/mongoutils/hash.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/timer.h"

using namespace mongoutils;

namespace mongo {
    namespace dur {

        extern Journal j;

        RelativePath local = RelativePath::fromRelativePath("local");

        static DurableMappedFile* findMMF_inlock(void *ptr, size_t &ofs) {
            DurableMappedFile *f = privateViews.find_inlock(ptr, ofs);
            if( f == 0 ) {
                error() << "findMMF_inlock failed " << privateViews.numberOfViews_inlock() << endl;
                printStackTrace(); // we want a stack trace and the assert below didn't print a trace once in the real world - not sure why
                stringstream ss;
                ss << "view pointer cannot be resolved " << hex << (size_t) ptr;
                journalingFailure(ss.str().c_str()); // asserts, which then abends
            }
            return f;
        }

        /** put the basic write operation into the buffer (bb) to be journaled */
        static void prepBasicWrite_inlock(AlignedBuilder&bb, const WriteIntent *i, RelativePath& lastDbPath) {
            size_t ofs = 1;
            DurableMappedFile *mmf = findMMF_inlock(i->start(), /*out*/ofs);

            if( unlikely(!mmf->willNeedRemap()) ) {
                // tag this mmf as needed a remap of its private view later.
                // usually it will already be dirty/already set, so we do the if above first
                // to avoid possibility of cpu cache line contention
                mmf->willNeedRemap() = true;
            }

            // since we have already looked up the mmf, we go ahead and remember the write view location
            // so we don't have to find the DurableMappedFile again later in WRITETODATAFILES()
            // 
            // this was for WRITETODATAFILES_Impl2 so commented out now
            //
            /*
            dassert( i->w_ptr == 0 );
            i->w_ptr = ((char*)mmf->view_write()) + ofs;
            */

            JEntry e;
            e.len = min(i->length(), (unsigned)(mmf->length() - ofs)); //don't write past end of file
            verify( ofs <= 0x80000000 );
            e.ofs = (unsigned) ofs;
            e.setFileNo( mmf->fileSuffixNo() );
            if( mmf->relativePath() == local ) {
                e.setLocalDbContextBit();
            }
            else if( mmf->relativePath() != lastDbPath ) {
                lastDbPath = mmf->relativePath();
                JDbContext c;
                bb.appendStruct(c);
                bb.appendStr(lastDbPath.toString());
            }
            bb.appendStruct(e);
#if defined(_EXPERIMENTAL)
            i->ofsInJournalBuffer = bb.len();
#endif
            bb.appendBuf(i->start(), e.len);

            if (unlikely(e.len != (unsigned)i->length())) {
                log() << "journal info splitting prepBasicWrite at boundary" << endl;

                // This only happens if we write to the last byte in a file and
                // the fist byte in another file that is mapped adjacently. I
                // think most OSs leave at least a one page gap between
                // mappings, but better to be safe.

                WriteIntent next ((char*)i->start() + e.len, i->length() - e.len);
                prepBasicWrite_inlock(bb, &next, lastDbPath);
            }
        }

        void assertNothingSpooled();

        /** basic write ops / write intents.  note there is no particular order to these : if we have
            two writes to the same location during the group commit interval, it is likely
            (although not assured) that it is journaled here once.
        */
        static void prepBasicWrites(AlignedBuilder& bb) {
            scoped_lock lk(privateViews._mutex());

            // each time events switch to a different database we journal a JDbContext
            // switches will be rare as we sort by memory location first and we batch commit.
            RelativePath lastDbPath;

            assertNothingSpooled();
            const vector<WriteIntent>& _intents = commitJob.getIntentsSorted();
            verify( !_intents.empty() ); // did you call releasingWriteLock()?

            WriteIntent last;
            for( vector<WriteIntent>::const_iterator i = _intents.begin(); i != _intents.end(); i++ ) { 
                if( i->start() < last.end() ) { 
                    // overlaps
                    last.absorb(*i);
                }
                else { 
                    // discontinuous
                    if( i != _intents.begin() )
                        prepBasicWrite_inlock(bb, &last, lastDbPath);
                    last = *i;
                }
            }
            prepBasicWrite_inlock(bb, &last, lastDbPath);
        }

        static void resetLogBuffer(/*out*/JSectHeader& h, AlignedBuilder& bb) {
            bb.reset();

            h.setSectionLen(0xffffffff);  // total length, will fill in later
            h.seqNumber = getLastDataFileFlushTime();
            h.fileId = j.curFileId();
        }

        /** we will build an output buffer ourself and then use O_DIRECT
            we could be in read lock for this
            caller handles locking
            @return partially populated sectheader and _ab set
        */
        static void _PREPLOGBUFFER(JSectHeader& h, AlignedBuilder& bb) {
            verify(storageGlobalParams.dur);
            assertLockedForCommitting();

            resetLogBuffer(h, bb); // adds JSectHeader

            // ops other than basic writes (DurOp's)
            {
                for( vector< shared_ptr<DurOp> >::iterator i = commitJob.ops().begin(); i != commitJob.ops().end(); ++i ) {
                    (*i)->serialize(bb);
                }
            }

            prepBasicWrites(bb);

            return;
        }
        void PREPLOGBUFFER(/*out*/ JSectHeader& h, AlignedBuilder& ab) {
            assertLockedForCommitting();
            Timer t;
            j.assureLogFileOpen(); // so fileId is set
            _PREPLOGBUFFER(h, ab);
            stats.curr->_prepLogBufferMicros += t.micros();
        }

    }
}
