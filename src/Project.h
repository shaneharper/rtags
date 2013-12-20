/* This file is part of RTags.

RTags is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

RTags is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with RTags.  If not, see <http://www.gnu.org/licenses/>. */

#ifndef Project_h
#define Project_h

#include "CursorInfo.h"
#include <rct/Path.h>
#include <rct/LinkedList.h>
#include "RTags.h"
#include "RTagsClang.h"
#include "Match.h"
#include <rct/Timer.h>
#include <rct/RegExp.h>
#include <rct/FileSystemWatcher.h>
#include "IndexerJob.h"
#include <mutex>
#include <memory>

class FileManager;
class IndexerJob;
class RestoreThread;
class Connection;
class Project : public std::enable_shared_from_this<Project>
{
public:
    Project(const Path &path);
    ~Project();
    enum State {
        Unloaded,
        Inited,
        Loading,
        Loaded
    };
    State state() const { return mState; }
    void init();

    enum FileManagerMode {
        FileManager_Asynchronous,
        FileManager_Synchronous
    };
    void load(FileManagerMode mode = FileManager_Asynchronous);
    void unload();

    std::shared_ptr<FileManager> fileManager;

    Path path() const { return mPath; }

    bool match(const Match &match, bool *indexed = 0) const;

    const SymbolMap &symbols() const { return mSymbols; }
    SymbolMap &symbols() { return mSymbols; }

    const SymbolNameMap &symbolNames() const { return mSymbolNames; }
    SymbolNameMap &symbolNames() { return mSymbolNames; }

    Set<Location> locations(const String &symbolName, uint32_t fileId = 0) const;
    SymbolMap symbols(uint32_t fileId) const;
    enum SortFlag {
        Sort_None = 0x0,
        Sort_DeclarationOnly = 0x1,
        Sort_Reverse = 0x2
    };
    List<RTags::SortedCursor> sort(const Set<Location> &locations, unsigned int flags = Sort_None) const;

    const FilesMap &files() const { return mFiles; }
    FilesMap &files() { return mFiles; }

    const UsrMap &usrs() const { return mUsr; }
    UsrMap &usrs() { return mUsr; }

    const Set<uint32_t> &suspendedFiles() const;
    bool toggleSuspendFile(uint32_t file);
    bool isSuspended(uint32_t file) const;
    void clearSuspendedFiles();

    bool isIndexed(uint32_t fileId) const;

    void dump(const Source &source, Connection *conn);
    void index(const Source &args, const std::shared_ptr<Cpp> &cpp, uint32_t indexerJobFlags);

    List<Source> sources(uint32_t fileId) const;
    bool hasSource(const Source &source) const;
    enum DependencyMode {
        DependsOnArg,
        ArgDependsOn // slow
    };
    Set<uint32_t> dependencies(uint32_t fileId, DependencyMode mode) const;
    bool visitFile(uint32_t fileId, uint64_t id);
    String fixIts(uint32_t fileId) const;
    int reindex(const Match &match);
    int remove(const Match &match);
    void onJobFinished(const std::shared_ptr<IndexData> &job);
    SourceMap sources() const { return mSources; }
    DependencyMap dependencies() const { return mDependencies; }
    Set<Path> watchedPaths() const { return mWatchedPaths; }
    void onTimerFired(Timer* event);
    bool isIndexing() const { return !mJobs.isEmpty(); }
    void dirty(const Path &);
    String dumpJobs() const;
    Hash<Path, uint32_t> visitedFiles() const
    {
        Hash<Path, uint32_t> ret;
        for (Set<uint32_t>::const_iterator it = mVisitedFiles.begin(); it != mVisitedFiles.end(); ++it) {
            ret[Location::path(*it)] = *it;
        }

        return ret;
    }
private:
    void restore(RestoreThread *thread);
    void watch(const Path &file);
    void reloadFileManager();
    void addDependencies(const DependencyMap &hash, Set<uint32_t> &newFiles);
    void addFixIts(const DependencyMap &dependencies, const FixItMap &fixIts);
    void syncDB(int *dirtyTime, int *syncTime);
    void startDirtyJobs(const Set<uint32_t> &files);
    bool save();
    void sync();

    const Path mPath;
    State mState;

    SymbolMap mSymbols;
    SymbolNameMap mSymbolNames;
    UsrMap mUsr;
    FilesMap mFiles;

    Set<uint32_t> mVisitedFiles;

    int mJobCounter;

    struct JobData {
        JobData()
            : pendingFlags(0), crashCount(0)
        {}
        Source pendingSource;
        uint32_t pendingFlags;
        std::shared_ptr<Cpp> pendingCpp;
        int crashCount;
        std::shared_ptr<IndexerJob> job;
    };

    // key'ed on Source::key()
    Hash<uint64_t, JobData> mJobs;
    Hash<uint64_t, Connection*> mDumps;
    Hash<uint64_t, std::shared_ptr<IndexData> > mPendingData; // ### this could go into JobData

    Timer mSyncTimer;
    StopWatch mTimer;
    FileSystemWatcher mWatcher;
    DependencyMap mDependencies;
    SourceMap mSources;
    Set<Path> mWatchedPaths;
    FixItMap mFixIts;

    Set<uint32_t> mPendingDirtyFiles;
    Set<uint32_t> mSuspendedFiles;

    friend class RestoreThread;
};

inline bool Project::visitFile(uint32_t visitFileId, uint64_t key)
{
    if (mVisitedFiles.insert(visitFileId)) {
        if (key) {
            assert(mJobs.contains(key));
            JobData &data = mJobs[key];
            assert(data.job);
            data.job->visited.insert(visitFileId);
        }
        return true;
    }
    return false;
}

#endif
