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

#include "Server.h"

#include "CompletionThread.h"
#include "CompileMessage.h"
#include "CreateOutputMessage.h"
#include "CursorInfoJob.h"
#include "DependenciesJob.h"
#include "VisitFileResponseMessage.h"
#include "PreprocessJob.h"
#include "Filter.h"
#include "FindFileJob.h"
#include "FindSymbolsJob.h"
#include "FollowLocationJob.h"
#include "IndexerJob.h"
#include "Source.h"
#include "Cpp.h"
#if defined(HAVE_CXCOMPILATIONDATABASE)
#  include <clang-c/CXCompilationDatabase.h>
#endif
#include "ListSymbolsJob.h"
#include "LogObject.h"
#include "Match.h"
#include "Preprocessor.h"
#include "Project.h"
#include "QueryMessage.h"
#include "VisitFileMessage.h"
#include "IndexerMessage.h"
#include "JobRequestMessage.h"
#include "JobResponseMessage.h"
#include "MulticastForwardMessage.h"
#include "RTags.h"
#include "ReferencesJob.h"
#include "StatusJob.h"
#include <clang-c/Index.h>
#include <rct/Connection.h>
#include <rct/EventLoop.h>
#include <rct/SocketClient.h>
#include <rct/Log.h>
#include <rct/Message.h>
#include <rct/Messages.h>
#include <rct/Path.h>
#include <rct/Process.h>
#include <rct/Rct.h>
#include <rct/RegExp.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <limits>

static const bool debugMulti = getenv("RDM_DEBUG_MULTI");

Server *Server::sInstance = 0;
Server::Server()
    : mVerbose(false), mCurrentFileId(0), mThreadPool(0), mRemotePending(0), mCompletionThread(0)
{
    Messages::registerMessage<JobRequestMessage>();
    Messages::registerMessage<JobResponseMessage>();

    assert(!sInstance);
    sInstance = this;

    mUnloadTimer.timeout().connect(std::bind(&Server::onUnload, this));
    mRescheduleTimer.timeout().connect(std::bind(&Server::onReschedule, this));
    mReconnectForwardsTimer.timeout().connect(std::bind(&Server::reconnectForwards, this));
}

Server::~Server()
{
    if (mCompletionThread) {
        mCompletionThread->stop();
        mCompletionThread->join();
        delete mCompletionThread;
        mCompletionThread = 0;
    }

    clear();
    assert(sInstance == this);
    sInstance = 0;
    Messages::cleanup();
}

void Server::clear()
{
    stopServers();
    delete mThreadPool; // wait first?
    mThreadPool = 0;
}

bool Server::init(const Options &options)
{
    RTags::initMessages();

    mOptions = options;
    mRescheduleTimer.restart(mOptions.rescheduleTimeout);
    Path clangPath = Path::resolved(CLANG_INCLUDEPATH);
    mOptions.includePaths.append(clangPath);
#ifdef OS_Darwin
    if (clangPath.exists()) {
        clangPath += "../../../c++/v1/";
        clangPath.resolve();
        if (clangPath.isDir())
            mOptions.includePaths.append(clangPath);
        // this seems to be the only way we get things like cstdint
    }
#endif

    if (options.options & UnlimitedErrors)
        mOptions.defaultArguments << "-ferror-limit=0";
    if (options.options & Wall)
        mOptions.defaultArguments << "-Wall";
    if (options.options & SpellChecking)
        mOptions.defaultArguments << "-fspell-checking";
    Log l(Error);
    l << "Running with" << mOptions.jobCount << "jobs, using args:"
      << String::join(mOptions.defaultArguments, ' ') << '\n';
    if (mOptions.tcpPort || mOptions.multicastPort) {
        if (mOptions.tcpPort)
            l << "tcp-port:" << mOptions.tcpPort;
        if (mOptions.multicastPort)
            l << "multicast-port:" << mOptions.multicastPort;
        l << '\n';
    }
    l << "includepaths" << String::join(mOptions.includePaths, ' ');

    if (mOptions.options & ClearProjects) {
        clearProjects();
    }

    for (int i=0; i<10; ++i) {
        mUnixServer.reset(new SocketServer);
        if (mUnixServer->listen(mOptions.socketFile)) {
            break;
        }
        mUnixServer.reset();
        if (!i) {
            enum { Timeout = 1000 };
            Connection connection;
            if (connection.connectUnix(mOptions.socketFile, Timeout)) {
                connection.send(QueryMessage(QueryMessage::Shutdown));
                connection.disconnected().connect(std::bind([](){ EventLoop::eventLoop()->quit(); }));
                connection.finished().connect(std::bind([](){ EventLoop::eventLoop()->quit(); }));
                EventLoop::eventLoop()->exec(Timeout);
            }
        }
        sleep(1);
        Path::rm(mOptions.socketFile);
    }
    if (!mUnixServer) {
        error("Unable to listen on %s", mOptions.socketFile.constData());
        return false;
    }

    restoreFileIds();
    mUnixServer->newConnection().connect(std::bind(&Server::onNewConnection, this, std::placeholders::_1));
    reloadProjects();
    if (!(mOptions.options & NoStartupCurrentProject)) {
        Path current = Path(mOptions.dataDir + ".currentProject").readAll(1024);
        if (current.size() > 1) {
            current.chop(1);
            if (!setCurrentProject(current)) {
                error() << "Can't restore project" << current;
                unlink((mOptions.dataDir + ".currentProject").constData());
            }
        }
    }

    if (!mOptions.multicastAddress.isEmpty()) {
        mMulticastSocket.reset(new SocketClient);
        if (!mMulticastSocket->bind(mOptions.multicastPort)) {
            error() << "Can't bind to multicast port" << mOptions.multicastPort;
            return false;
        }
        if (!mMulticastSocket->addMembership(mOptions.multicastAddress)) {
            error() << "Can't add membership" << mOptions.multicastAddress;
            return false;
        }
        mMulticastSocket->setMulticastLoop(false);
        if (mOptions.multicastTTL)
            mMulticastSocket->setMulticastTTL(mOptions.multicastTTL);
        mMulticastSocket->readyReadFrom().connect(std::bind(&Server::onMulticastReadyRead, this,
                                                            std::placeholders::_1,
                                                            std::placeholders::_2,
                                                            std::placeholders::_3,
                                                            std::placeholders::_4));
    }

    for (auto it = mOptions.multicastForwards.cbegin(); it != mOptions.multicastForwards.cend(); ++it) {
        connectMulticastForward(*it);
    }

    if (mOptions.tcpPort) {
        mTcpServer.reset(new SocketServer);
        if (!mTcpServer->listen(mOptions.tcpPort)) {
            error() << "Unable to listen on port" << mOptions.tcpPort;
            return false;
        }

        mTcpServer->newConnection().connect(std::bind(&Server::onNewConnection, this, std::placeholders::_1));
    }
    mThreadPool = new ThreadPool(mOptions.jobCount);
    return true;
}

std::shared_ptr<Project> Server::addProject(const Path &path) // lock always held
{
    std::shared_ptr<Project> &project = mProjects[path];
    if (!project) {
        project.reset(new Project(path));
        return project;
    }
    return std::shared_ptr<Project>();
}

int Server::reloadProjects()
{
    mProjects.clear(); // ### could keep the ones that persist somehow
    List<Path> projects = mOptions.dataDir.files(Path::File);
    const Path home = Path::home();
    for (int i=0; i<projects.size(); ++i) {
        Path file = projects.at(i);
        Path p = file.mid(mOptions.dataDir.size());
        RTags::decodePath(p);
        if (p.isDir()) {
            bool remove = false;
            if (FILE *f = fopen(file.constData(), "r")) {
                Deserializer in(f);
                int version;
                in >> version;

                if (version == Server::DatabaseVersion) {
                    int fs;
                    in >> fs;
                    if (fs != Rct::fileSize(f)) {
                        error("%s seems to be corrupted, refusing to restore. Removing.",
                              file.constData());
                        remove = true;
                    } else {
                        addProject(p);
                    }
                } else {
                    remove = true;
                    error() << file << "has wrong format. Got" << version << "expected" << Server::DatabaseVersion << "Removing";
                }
                fclose(f);
            }
            if (remove) {
                Path::rm(file);
            }
        }
    }
    return mProjects.size();
}

void Server::onNewConnection(SocketServer *server)
{
    while (true) {
        SocketClient::SharedPtr client = server->nextConnection();
        if (!client)
            break;
        Connection *conn = new Connection(client);
        conn->newMessage().connect(std::bind(&Server::onNewMessage, this, std::placeholders::_1, std::placeholders::_2));
        conn->disconnected().connect(std::bind(&Server::onConnectionDisconnected, this, std::placeholders::_1));

        if (debugMulti) {
            String ip;
            uint16_t port;
            if (conn->client()->peer(&ip, &port))
                error() << "Got connection from" << String::format<64>("%s:%d", ip.constData(), port);
        }
    }
}

void Server::onConnectionDisconnected(Connection *o)
{
    o->disconnected().disconnect();
    EventLoop::deleteLater(o);
    for (auto it = mMulticastForwards.begin(); it != mMulticastForwards.end(); ++it) {
        if (it->second.connection == o) {
            it->second.connection = 0;
            ++it->second.failures;
            warning() << "Disconnected from host:"
                      << String::format<64>("%s:%d", it->first.first.constData(), it->first.second)
                      << it->second.failures;
            EventLoop::eventLoop()->callLater(std::bind(&Server::reconnectForwards, this));
            break;
        }
    }
    mPendingJobRequests.remove(o);
}

void Server::onNewMessage(Message *message, Connection *connection)
{
    if (mOptions.unloadTimer)
        mUnloadTimer.restart(mOptions.unloadTimer * 1000 * 60, Timer::SingleShot);

    ClientMessage *m = static_cast<ClientMessage*>(message);

    switch (message->messageId()) {
    case CompileMessage::MessageId:
        handleCompileMessage(static_cast<CompileMessage&>(*m), connection);
        break;
    case QueryMessage::MessageId:
        error() << m->raw();
        handleQueryMessage(static_cast<const QueryMessage&>(*m), connection);
        break;
    case MulticastForwardMessage::MessageId:
        handleMulticastForwardMessage(static_cast<const MulticastForwardMessage&>(*m), connection);
        break;
    case IndexerMessage::MessageId:
        handleIndexerMessage(static_cast<const IndexerMessage&>(*m), connection);
        break;
    case CreateOutputMessage::MessageId:
        error() << m->raw();
        handleCreateOutputMessage(static_cast<const CreateOutputMessage&>(*m), connection);
        break;
    case VisitFileMessage::MessageId:
        handleVisitFileMessage(static_cast<const VisitFileMessage&>(*m), connection);
        break;
    case ResponseMessage::MessageId:
    case FinishMessage::MessageId:
    case VisitFileResponseMessage::MessageId:
        error() << getpid() << "Unexpected message" << static_cast<int>(message->messageId());
        // assert(0);
        connection->finish();
        break;
    case JobRequestMessage::MessageId:
        handleJobRequestMessage(static_cast<const JobRequestMessage&>(*m), connection);
        break;
    case JobResponseMessage::MessageId:
        handleJobResponseMessage(static_cast<const JobResponseMessage&>(*m), connection);
        break;
    default:
        error("Unknown message: %d", message->messageId());
        connection->finish();
        break;
    }
    if (mOptions.options & NoFileManagerWatch) {
        std::shared_ptr<Project> project = currentProject();
        if (project && project->fileManager && (Rct::monoMs() - project->fileManager->lastReloadTime()) > 60000)
            project->fileManager->reload(FileManager::Asynchronous);
    }
}

void Server::index(const String &arguments, const Path &pwd, const List<String> &withProjects)
{
    Path unresolvedPath;
    Source source = Source::parse(arguments, pwd, &unresolvedPath);
    if (!source.isIndexable())
        return;
    Path project = findProject(source.sourceFile(), unresolvedPath, withProjects);
    if (!shouldIndex(source, project))
        return;

    preprocess(std::move(source), std::move(project), IndexerJob::None);
}

void Server::preprocess(Source &&source, Path &&project, uint32_t flags)
{
    std::shared_ptr<PreprocessJob> job(new PreprocessJob(std::move(source), std::move(project), flags));
    mPendingPreprocessJobs.append(job);
    startPreprocessJobs();
}

void Server::handleCompileMessage(CompileMessage &message, Connection *conn)
{
    conn->close();
    index(message.arguments(), message.workingDirectory(), message.projects());
}

void Server::handleCreateOutputMessage(const CreateOutputMessage &message, Connection *conn)
{
    new LogObject(conn, message.level());
}

void Server::handleIndexerMessage(const IndexerMessage &message, Connection *conn)
{
    std::shared_ptr<IndexData> indexData = message.data();
    // error() << "Got indexer message" << message.project() << Location::path(indexData->fileId);
    assert(indexData);
    auto it = mProcessingJobs.find(indexData->jobId);
    if (debugMulti)
        error() << "got indexer message for job" << Location::path(indexData->fileId()) << indexData->jobId;
    if (it == mProcessingJobs.end()) {
        // job already processed
        if (debugMulti)
            error() << "already got a response for" << indexData->jobId;
        return;
    }
    assert(!(it->second.flags & IndexData::FromRemote));
    assert(it->second.flags & (IndexerJob::Running|IndexerJob::Aborted|IndexerJob::Crashed));
    if (!(it->second->flags == (IndexerJob::Aborted|IndexerJob::Crashed))) {
        assert(it->second->flags & IndexerJob::Running);
        it->second->flags |= IndexerJob::Complete;
        it->second->flags &= ~IndexerJob::Running;
    }
    mProcessingJobs.erase(it);
    std::shared_ptr<Project> project = mProjects.value(message.project());
    if (!project) {
        error() << "Can't find project root for this IndexerMessage" << message.project() << Location::path(indexData->fileId());
        return;
    }
    String ip;
    uint16_t port;
    if (conn->client()->peer(&ip, &port))
        indexData->message << String::format<64>(" from %s", ip.constData());

    project->onJobFinished(indexData);
    conn->finish();
    startPreprocessJobs();
}

void Server::handleQueryMessage(const QueryMessage &message, Connection *conn)
{
    conn->setSilent(message.flags() & QueryMessage::Silent);
    updateProject(message.projects(), message.flags());

    switch (message.type()) {
    case QueryMessage::Invalid:
        assert(0);
        break;
    case QueryMessage::Sources:
        sources(message, conn);
        break;
    case QueryMessage::SendDiagnostics:
        sendDiagnostics(message, conn);
        break;
    case QueryMessage::CodeCompleteAt:
    case QueryMessage::PrepareCodeCompleteAt:
        codeCompleteAt(message, conn);
        break;
    case QueryMessage::SuspendFile:
        suspendFile(message, conn);
        break;
    case QueryMessage::IsIndexing:
        isIndexing(message, conn);
        break;
    case QueryMessage::RemoveFile:
        removeFile(message, conn);
        break;
    case QueryMessage::JobCount:
        jobCount(message, conn);
        break;
    case QueryMessage::FixIts:
        fixIts(message, conn);
        break;
    case QueryMessage::FindFile:
        findFile(message, conn);
        break;
    case QueryMessage::DumpFile:
        dumpFile(message, conn);
        break;
    case QueryMessage::Dependencies:
        dependencies(message, conn);
        break;
    case QueryMessage::DeleteProject:
        removeProject(message, conn);
        break;
    case QueryMessage::UnloadProject:
        removeProject(message, conn);
        break;
    case QueryMessage::ReloadProjects:
        reloadProjects(message, conn);
        break;
    case QueryMessage::Project:
        project(message, conn);
        break;
#if defined(HAVE_CXCOMPILATIONDATABASE) && CLANG_VERSION_MINOR >= 3
    case QueryMessage::LoadCompilationDatabase:
        loadCompilationDatabase(message, conn);
        break;
#endif
    case QueryMessage::Reindex: {
        reindex(message, conn);
        break; }
    case QueryMessage::ClearProjects:
        clearProjects(message, conn);
        break;
    case QueryMessage::CursorInfo:
        cursorInfo(message, conn);
        break;
    case QueryMessage::Shutdown:
        shutdown(message, conn);
        break;
    case QueryMessage::FollowLocation:
        followLocation(message, conn);
        break;
    case QueryMessage::ReferencesLocation:
        referencesForLocation(message, conn);
        break;
    case QueryMessage::ReferencesName:
        referencesForName(message, conn);
        break;
    case QueryMessage::ListSymbols:
        listSymbols(message, conn);
        break;
    case QueryMessage::FindSymbols:
        findSymbols(message, conn);
        break;
    case QueryMessage::Status:
        status(message, conn);
        break;
    case QueryMessage::IsIndexed:
        isIndexed(message, conn);
        break;
    case QueryMessage::HasFileManager:
        hasFileManager(message, conn);
        break;
    case QueryMessage::PreprocessFile:
        preprocessFile(message, conn);
        break;
    case QueryMessage::ReloadFileManager:
        reloadFileManager(message, conn);
        break;
    case QueryMessage::MulticastForward:
    case QueryMessage::RemoveMulticastForward:
        handleMulticastForward(message, conn);
        break;
    }
}

void Server::followLocation(const QueryMessage &query, Connection *conn)
{
    const Location loc = query.location();
    if (loc.isNull()) {
        conn->write("Not indexed");
        conn->finish();
        return;
    }
    std::shared_ptr<Project> project = updateProjectForLocation(loc.path());
    if (!project) {
        error("No project");
        conn->finish();
        return;
    } else if (project->state() != Project::Loaded) {
        conn->write("Project loading");
        conn->finish();
        return;
    }

    FollowLocationJob job(loc, query, project);
    job.run(conn);
    conn->finish();
}

void Server::isIndexing(const QueryMessage &, Connection *conn)
{
    for (ProjectsMap::const_iterator it = mProjects.begin(); it != mProjects.end(); ++it) {
        if (it->second->isIndexing()) {
            conn->write("1");
            conn->finish();
            return;
        }
    }
    conn->write("0");
    conn->finish();
}

void Server::removeFile(const QueryMessage &query, Connection *conn)
{
    // Path path = query.path();
    const Match match = query.match();
    std::shared_ptr<Project> project = updateProjectForLocation(match);
    if (!project)
        project = currentProject();

    if (!project) {
        error("No project");
        conn->finish();
        return;
    } else if (project->state() != Project::Loaded) {
        conn->write("Project loading");
        conn->finish();
        return;
    }

    const int count = project->remove(match);
    // error() << count << query.query();
    if (count) {
        conn->write<128>("Removed %d files", count);
    } else {
        conn->write("No matches");
    }
    conn->finish();
}

void Server::findFile(const QueryMessage &query, Connection *conn)
{
    std::shared_ptr<Project> project = currentProject();
    if (!project || project->state() == Project::Unloaded) {
        error("No project");
        conn->finish();
        return;
    }

    FindFileJob job(query, project);
    job.run(conn);
    conn->finish();
}

void Server::dumpFile(const QueryMessage &query, Connection *conn)
{
    const uint32_t fileId = Location::fileId(query.query());
    if (!fileId) {
        conn->write<256>("%s is not indexed", query.query().constData());
        conn->finish();
        return;
    }

    std::shared_ptr<Project> project = updateProjectForLocation(Location::path(fileId));
    if (!project || project->state() != Project::Loaded) {
        conn->write<256>("%s is not indexed", query.query().constData());
        conn->finish();
        return;
    }

    const Source source = project->sources(fileId).value(query.buildIndex());
    if (!source.isNull()) {
        project->dump(source, conn);
    } else {
        conn->write<256>("%s build: %d not found", query.query().constData(), query.buildIndex());
        conn->finish();
    }
}

void Server::cursorInfo(const QueryMessage &query, Connection *conn)
{
    const Location loc = query.location();
    if (loc.isNull()) {
        conn->finish();
        return;
    }
    std::shared_ptr<Project> project = updateProjectForLocation(loc.path());

    if (!project) {
        conn->finish();
    } else if (project->state() != Project::Loaded) {
        conn->write("Project loading");
        conn->finish();
    } else {
        CursorInfoJob job(loc, query, project);
        job.run(conn);
        conn->finish();
    }
}

void Server::dependencies(const QueryMessage &query, Connection *conn)
{
    const Path path = query.query();
    std::shared_ptr<Project> project = updateProjectForLocation(path);
    if (!project) {
        conn->finish();
        return;
    } else if (project->state() != Project::Loaded) {
        conn->write("Project loading");
        conn->finish();
        return;
    }

    DependenciesJob job(query, project);
    job.run(conn);
    conn->finish();
}

void Server::fixIts(const QueryMessage &query, Connection *conn)
{
    std::shared_ptr<Project> project = updateProjectForLocation(query.match());
    if (project && project->state() == Project::Loaded) {
        String out = project->fixIts(Location::fileId(query.query()));
        if (!out.isEmpty())
            conn->write(out);
    }
    conn->finish();
}

void Server::referencesForLocation(const QueryMessage &query, Connection *conn)
{
    const Location loc = query.location();
    if (loc.isNull()) {
        conn->write("Not indexed");
        conn->finish();
        return;
    }
    std::shared_ptr<Project> project = updateProjectForLocation(loc.path());

    if (!project) {
        error("No project");
        conn->finish();
        return;
    } else if (project->state() != Project::Loaded) {
        conn->write("Project loading");
        conn->finish();
        return;
    }

    ReferencesJob job(loc, query, project);
    job.run(conn);
    conn->finish();
}

void Server::referencesForName(const QueryMessage& query, Connection *conn)
{
    const String name = query.query();

    std::shared_ptr<Project> project = currentProject();

    if (!project) {
        error("No project");
        conn->finish();
        return;
    } else if (project->state() != Project::Loaded) {
        conn->write("Project loading");
        conn->finish();
        return;
    }

    ReferencesJob job(name, query, project);
    job.run(conn);
    conn->finish();
}

void Server::findSymbols(const QueryMessage &query, Connection *conn)
{
    const String partial = query.query();

    std::shared_ptr<Project> project = currentProject();

    if (!project) {
        error("No project");
        conn->finish();
        return;
    } else if (project->state() != Project::Loaded) {
        conn->write("Project loading");
        conn->finish();
        return;
    }

    FindSymbolsJob job(query, project);
    job.run(conn);
    conn->finish();
}

void Server::listSymbols(const QueryMessage &query, Connection *conn)
{
    const String partial = query.query();

    std::shared_ptr<Project> project = currentProject();
    if (!project) {
        error("No project");
        conn->finish();
        return;
    }

    ListSymbolsJob job(query, project);
    job.run(conn);
    conn->finish();
}

void Server::status(const QueryMessage &query, Connection *conn)
{
    std::shared_ptr<Project> project = currentProject();

    if (!project) {
        error("No project");
        conn->finish();
        return;
    } else if (project->state() != Project::Loaded) {
        conn->write("Project loading");
        conn->finish();
        return;
    }

    conn->client()->setWriteMode(SocketClient::Synchronous);

    StatusJob job(query, project);
    job.run(conn);
    conn->finish();
}

void Server::isIndexed(const QueryMessage &query, Connection *conn)
{
    int ret = 0;
    const Match match = query.match();
    std::shared_ptr<Project> project = updateProjectForLocation(match);
    if (project) {
        bool indexed = false;
        if (project->match(match, &indexed))
            ret = indexed ? 1 : 2;
    }

    error("=> %d", ret);
    conn->write<16>("%d", ret);
    conn->finish();
}

void Server::reloadFileManager(const QueryMessage &, Connection *conn)
{
    std::shared_ptr<Project> project = currentProject();
    if (project) {
        conn->write<512>("Reloading files for %s", project->path().constData());
        conn->finish();
        project->fileManager->reload(FileManager::Asynchronous);
    } else {
        conn->write("No current project");
        conn->finish();
    }
}

void Server::hasFileManager(const QueryMessage &query, Connection *conn)
{
    const Path path = query.query();
    std::shared_ptr<Project> project = updateProjectForLocation(path);
    if (project && project->fileManager && (project->fileManager->contains(path) || project->match(query.match()))) {
        error("=> 1");
        conn->write("1");
    } else {
        error("=> 0");
        conn->write("0");
    }
    conn->finish();
}

void Server::preprocessFile(const QueryMessage &query, Connection *conn)
{
    const Path path = query.query();
    std::shared_ptr<Project> project = updateProjectForLocation(path);
    if (!project) {
        conn->write("No project");
        conn->finish();
        return;
    } else if (project->state() != Project::Loaded) {
        conn->write("Project loading");
        conn->finish();
        return;
    }

    const uint32_t fileId = Location::fileId(path);
    const Source source = project->sources(fileId).value(query.buildIndex());
    if (!source.isValid()) {
        conn->write<256>("%s build: %d not found", query.query().constData(), query.buildIndex());
    } else {
        Preprocessor pre(source, conn);
        pre.preprocess();
    }
    conn->finish();
}

void Server::clearProjects()
{
    for (ProjectsMap::const_iterator it = mProjects.begin(); it != mProjects.end(); ++it)
        it->second->unload();
    Rct::removeDirectory(mOptions.dataDir);
    mCurrentProject.reset();
    unlink((mOptions.dataDir + ".currentProject").constData());
    mProjects.clear();
}

void Server::reindex(const QueryMessage &query, Connection *conn)
{
    Match match = query.match();
    std::shared_ptr<Project> project = updateProjectForLocation(match);
    if (!project) {
        project = currentProject();
        if (!project) {
            error("No project");
            conn->finish();
            return;
        } else if (project->state() != Project::Loaded) {
            conn->write("Project loading");
            conn->finish();
            return;
        }
    }

    const int count = project->reindex(match);
    // error() << count << query.query();
    if (count) {
        conn->write<128>("Dirtied %d files", count);
    } else {
        conn->write("No matches");
    }
    conn->finish();
}

bool Server::shouldIndex(const Source &source, const Path &srcRoot) const
{
    if (srcRoot.isEmpty())
        return false;
    assert(source.isIndexable());
    if (mOptions.ignoredCompilers.contains(source.compiler()))
        return false;

    const Path sourceFile = source.sourceFile();

    if (Filter::filter(sourceFile, mOptions.excludeFilters) == Filter::Filtered)
        return false;

    std::shared_ptr<Project> project = mProjects.value(srcRoot);
    return !project || !project->hasSource(source);
}

Path Server::findProject(const Path &path, const Path &unresolvedPath, const List<String> &withProjects) const
{
    std::shared_ptr<Project> current = mCurrentProject.lock();
    if (current && (current->match(unresolvedPath) || (path != unresolvedPath && current->match(path))))
        return current->path();

    for (auto it = mProjects.begin(); it != mProjects.end(); ++it) {
        if (it->second->match(unresolvedPath) || (path != unresolvedPath && it->second->match(path)))
            return it->first;
    }

    for (auto it = mProjects.begin(); it != mProjects.end(); ++it) {
        for (auto p = withProjects.begin(); p != withProjects.end(); ++p) {
            if (it->second->match(*p))
                return it->first;
        }
    }

    const Path root = RTags::findProjectRoot(unresolvedPath, RTags::SourceRoot);
    if (root.isEmpty() && path != unresolvedPath)
        return RTags::findProjectRoot(path, RTags::SourceRoot);
    return root;
}

void Server::index(const Source &source, const std::shared_ptr<Cpp> &cpp, const Path &srcRoot, uint32_t flags)
{
    std::shared_ptr<Project> project = mProjects.value(srcRoot);
    if (!project) {
        project = addProject(srcRoot);
        assert(project);
    }
    project->load();

    if (!mCurrentProject.lock()) {
        mCurrentProject = project;
        setupCurrentProjectFile(project);
    }
    assert(project);
    project->index(source, cpp, flags);
    startNextJob();
}

std::shared_ptr<Project> Server::setCurrentProject(const Path &path, unsigned int queryFlags) // lock always held
{
    ProjectsMap::iterator it = mProjects.find(path);
    if (it != mProjects.end()) {
        setCurrentProject(it->second, queryFlags);
        return it->second;
    }
    return std::shared_ptr<Project>();
}

void Server::setupCurrentProjectFile(const std::shared_ptr<Project> &project)
{
    if (project) {
        Path::mkdir(mOptions.dataDir);
        FILE *f = fopen((mOptions.dataDir + ".currentProject").constData(), "w");
        if (f) {
            if (!fwrite(project->path().constData(), project->path().size(), 1, f) || !fwrite("\n", 1, 1, f)) {
                error() << "error writing to" << (mOptions.dataDir + ".currentProject");
                fclose(f);
                unlink((mOptions.dataDir + ".currentProject").constData());
            } else {
                fclose(f);
            }
        } else {
            error() << "error opening" << (mOptions.dataDir + ".currentProject") << "for write";
        }
    } else {
        Path::rm(mOptions.dataDir + ".currentProject");
    }
}

std::shared_ptr<Project> Server::setCurrentProject(const std::shared_ptr<Project> &project, unsigned int queryFlags)
{
    std::shared_ptr<Project> old = mCurrentProject.lock();
    if (project && project != old) {
        if (old && old->fileManager)
            old->fileManager->clearFileSystemWatcher();
        mCurrentProject = project;
        setupCurrentProjectFile(project);

        Project::FileManagerMode mode = Project::FileManager_Asynchronous;
        if (queryFlags & QueryMessage::WaitForLoadProject)
            mode = Project::FileManager_Synchronous;
        switch (project->state()) {
        case Project::Loaded:
        case Project::Inited:
            project->fileManager->reload(FileManager::Asynchronous);
            break;
        default:
            break;
        }
        project->load(mode);
        return project;
    }
    return std::shared_ptr<Project>();
}

std::shared_ptr<Project> Server::updateProjectForLocation(const Match &match)
{
    std::shared_ptr<Project> cur = currentProject();
    // give current a chance first to avoid switching project when using system headers etc
    if (cur && cur->match(match))
        return cur;

    for (ProjectsMap::const_iterator it = mProjects.begin(); it != mProjects.end(); ++it) {
        if (it->second != cur && it->second->match(match)) {
            return setCurrentProject(it->second->path());
        }
    }
    return std::shared_ptr<Project>();
}

void Server::removeProject(const QueryMessage &query, Connection *conn)
{
    const bool unload = query.type() == QueryMessage::UnloadProject;

    const Match match = query.match();
    ProjectsMap::iterator it = mProjects.begin();
    while (it != mProjects.end()) {
        ProjectsMap::iterator cur = it++;
        if (cur->second->match(match)) {
            if (mCurrentProject.lock() == cur->second) {
                mCurrentProject.reset();
                setupCurrentProjectFile(std::shared_ptr<Project>());
                unlink((mOptions.dataDir + ".currentProject").constData());
            }
            cur->second->unload();
            Path path = cur->first;
            conn->write<128>("%s project: %s", unload ? "Unloaded" : "Deleted", path.constData());
            if (!unload) {
                RTags::encodePath(path);
                Path::rm(mOptions.dataDir + path);
                mProjects.erase(cur);
            }
        }
    }
    conn->finish();
}

void Server::reloadProjects(const QueryMessage &query, Connection *conn)
{
    const int old = mProjects.size();
    const int cur = reloadProjects();
    conn->write<128>("Changed from %d to %d projects", old, cur);
    conn->finish();
}

bool Server::selectProject(const Match &match, Connection *conn, unsigned int queryFlags)
{
    std::shared_ptr<Project> selected;
    bool error = false;
    for (ProjectsMap::const_iterator it = mProjects.begin(); it != mProjects.end(); ++it) {
        if (it->second->match(match)) {
            if (error) {
                if (conn)
                    conn->write(it->first);
            } else if (selected) {
                error = true;
                if (conn) {
                    conn->write<128>("Multiple matches for %s", match.pattern().constData());
                    conn->write(selected->path());
                    conn->write(it->first);
                }
                selected.reset();
            } else {
                selected = it->second;
                const Path p = match.pattern();
                if (p.isFile()) {
                    // ### this needs to deal with dependencies for headers
                    mCurrentFileId = Location::fileId(p);
                } else {
                    mCurrentFileId = 0;
                }
            }
        }
    }
    if (selected) {
        if (setCurrentProject(selected, queryFlags) && conn)
            conn->write<128>("Selected project: %s for %s", selected->path().constData(), match.pattern().constData());
        return true;
    } else if (!error && conn) {
        conn->write<128>("No matches for %s", match.pattern().constData());
    }
    return false;
}

bool Server::updateProject(const List<String> &projects, unsigned int queryFlags)
{
    for (int i=0; i<projects.size(); ++i) {
        if (selectProject(projects.at(i), 0, queryFlags))
            return true;
    }
    return false;
}

void Server::project(const QueryMessage &query, Connection *conn)
{
    if (query.query().isEmpty()) {
        const std::shared_ptr<Project> current = mCurrentProject.lock();
        const char *states[] = { "(unloaded)", "(inited)", "(loading)", "(loaded)" };
        for (ProjectsMap::const_iterator it = mProjects.begin(); it != mProjects.end(); ++it) {
            conn->write<128>("%s %s%s",
                             it->first.constData(),
                             states[it->second->state()],
                             it->second == current ? " <=" : "");
        }
    } else {
        Path selected;
        bool error = false;
        const Match match = query.match();
        const ProjectsMap::const_iterator it = mProjects.find(match.pattern());
        bool ok = false;
        unsigned long long index = query.query().toULongLong(&ok);
        if (it != mProjects.end()) {
            selected = it->first;
        } else {
            for (ProjectsMap::const_iterator it = mProjects.begin(); it != mProjects.end(); ++it) {
                assert(it->second);
                if (ok) {
                    if (!index) {
                        selected = it->first;
                    } else {
                        --index;
                    }
                }
                if (it->second->match(match)) {
                    if (error) {
                        conn->write(it->first);
                    } else if (!selected.isEmpty()) {
                        error = true;
                        conn->write<128>("Multiple matches for %s", match.pattern().constData());
                        conn->write(selected);
                        conn->write(it->first);
                        selected.clear();
                    } else {
                        selected = it->first;
                    }
                }
            }
        }
        if (!selected.isEmpty()) {
            std::shared_ptr<Project> current = mCurrentProject.lock();
            if (!current || selected != current->path()) {
                setCurrentProject(selected);
                conn->write<128>("Selected project: %s for %s", selected.constData(), match.pattern().constData());
            }
        } else if (!error) {
            conn->write<128>("No matches for %s", match.pattern().constData());
        }
    }
    conn->finish();
}

void Server::jobCount(const QueryMessage &query, Connection *conn)
{
    if (query.query().isEmpty()) {
        conn->write<128>("Running with %d jobs", mOptions.jobCount);
    } else {
        const int jobCount = query.query().toLongLong();
        if (jobCount <= 0 || jobCount > 100) {
            conn->write<128>("Invalid job count %s (%d)", query.query().constData(), jobCount);
        } else {
            mOptions.jobCount = jobCount;
            mThreadPool->setConcurrentJobs(jobCount);
            conn->write<128>("Changed jobs to %d", jobCount);
        }
    }
    conn->finish();
}

void Server::sendDiagnostics(const QueryMessage &query, Connection *conn)
{
    if (testLog(RTags::CompilationErrorXml))
        logDirect(RTags::CompilationErrorXml, query.query());
    conn->finish();
}

void Server::clearProjects(const QueryMessage &query, Connection *conn)
{
    clearProjects();
    conn->write("Cleared projects");
    conn->finish();
}

#if defined(HAVE_CXCOMPILATIONDATABASE) && CLANG_VERSION_MINOR >= 3
void Server::loadCompilationDatabase(const QueryMessage &query, Connection *conn)
{
    const Path path = query.query();
    // ### this will ignore the actual file name, not sure how to fix that
    CXCompilationDatabase_Error err;
    CXCompilationDatabase db = clang_CompilationDatabase_fromDirectory(path.constData(), &err);
    if (err != CXCompilationDatabase_NoError) {
        conn->write("Can't load compilation database");
        conn->finish();
        return;
    }
    CXCompileCommands cmds = clang_CompilationDatabase_getAllCompileCommands(db);
    const unsigned int sz = clang_CompileCommands_getSize(cmds);
    for (unsigned int i = 0; i < sz; ++i) {
        CXCompileCommand cmd = clang_CompileCommands_getCommand(cmds, i);
        String args;
        CXString str = clang_CompileCommand_getDirectory(cmd);
        Path dir = clang_getCString(str);
        clang_disposeString(str);
        const unsigned int num = clang_CompileCommand_getNumArgs(cmd);
        for (unsigned int j = 0; j < num; ++j) {
            str = clang_CompileCommand_getArg(cmd, j);
            args += clang_getCString(str);
            clang_disposeString(str);
            if (j < num - 1)
                args += " ";
        }

        index(args, dir, query.projects());
    }
    clang_CompileCommands_dispose(cmds);
    clang_CompilationDatabase_dispose(db);
    conn->write("Compilation database loaded");
    conn->finish();
}
#endif

void Server::shutdown(const QueryMessage &query, Connection *conn)
{
    for (Hash<Path, std::shared_ptr<Project> >::const_iterator it = mProjects.begin(); it != mProjects.end(); ++it) {
        if (it->second)
            it->second->unload();
    }
    EventLoop::eventLoop()->quit();
    conn->write("Shutting down");
    conn->finish();
}

void Server::sources(const QueryMessage &query, Connection *conn)
{
    const Path path = query.query();
    if (path.isFile()) {
        std::shared_ptr<Project> project = updateProjectForLocation(path);
        if (project) {
            if (project->state() != Project::Loaded) {
                conn->write("Project loading");
            } else {
                const uint32_t fileId = Location::fileId(path);
                if (fileId) {
                    const List<Source> sources = project->sources(fileId);
                    for (List<Source>::const_iterator it = sources.begin(); it != sources.end(); ++it) {
                        if (query.flags() & QueryMessage::CompilationFlagsOnly) {
                            conn->write<128>("%s: %s",
                                             it->sourceFile().constData(),
                                             String::join(it->toCommandLine(0), ' ').constData());
                        } else {
                            conn->write(it->toString());
                        }
                    }
                }
            }
            conn->finish();
            return;
        }
    }

    if (std::shared_ptr<Project> project = currentProject()) {
        const Match match = query.match();
        if (project->state() != Project::Loaded) {
            conn->write("Project loading");
        } else {
            const SourceMap infos = project->sources();
            for (SourceMap::const_iterator it = infos.begin(); it != infos.end(); ++it) {
                if (match.isEmpty() || match.match(it->second.sourceFile())) {
                    if (query.flags() & QueryMessage::CompilationFlagsOnly) {
                        conn->write<128>("%s: %s",
                                         it->second.sourceFile().constData(),
                                         String::join(it->second.toCommandLine(0), ' ').constData());
                    } else {
                        conn->write(it->second.toString());
                    }
                }
            }
        }
    } else {
        conn->write("No project");
    }
    conn->finish();
}

void Server::suspendFile(const QueryMessage &query, Connection *conn)
{
    std::shared_ptr<Project> project;
    const Match match = query.match();
    if (match.isEmpty() || match.pattern() == "clear") {
        project = currentProject();
    } else {
        project = updateProjectForLocation(match);
    }
    if (!project) {
        conn->write("No project");
    } else if (project->state() != Project::Loaded) {
        conn->write("Project loading");
    } else {
        if (match.isEmpty()) {
            const Set<uint32_t> suspendedFiles = project->suspendedFiles();
            if (suspendedFiles.isEmpty()) {
                conn->write<512>("No files suspended for project %s", project->path().constData());
            } else {
                for (Set<uint32_t>::const_iterator it = suspendedFiles.begin(); it != suspendedFiles.end(); ++it)
                    conn->write<512>("%s is suspended", Location::path(*it).constData());
            }
        } else {
            const Path p = query.match().pattern();
            if (p == "clear") {
                project->clearSuspendedFiles();
                conn->write<512>("No files are suspended");
            } else if (!p.isFile()) {
                conn->write<512>("%s doesn't seem to exist", p.constData());
            } else {
                const uint32_t fileId = Location::insertFile(p);
                conn->write<512>("%s is no%s suspended", p.constData(),
                                 project->toggleSuspendFile(fileId) ? "w" : " longer");
            }
        }
    }
    conn->finish();
}

void Server::handleJobRequestMessage(const JobRequestMessage &message, Connection *conn)
{
    if (debugMulti)
        error() << "got a request for" << message.numJobs() << "jobs";
    int cnt = message.numJobs();
    auto it = mPending.begin();
    while (it != mPending.end()) {
        std::shared_ptr<IndexerJob>& job = *it;
        if (!(job->flags & IndexerJob::FromRemote)) {
            assert(job->flags & (IndexerJob::Running|IndexerJob::Complete));
            if (debugMulti)
                error() << "sending job for" << job->sourceFile << conn->client()->peerName();
            job->started = Rct::monoMs();
            job->flags |= (IndexerJob::Running|IndexerJob::Remote);
            mProcessingJobs[job->id] = job;
            conn->send(JobResponseMessage(job, mOptions.tcpPort));
            it = mPending.erase(it);
            if (!--cnt)
                break;
            continue;
        }
        ++it;
    }
    conn->finish();
}

void Server::handleJobResponseMessage(const JobResponseMessage &message, Connection *conn)
{
    std::shared_ptr<IndexerJob> job(new IndexerJob);
    message.toIndexerJob(job, conn);
    if (debugMulti) {
        error() << "got indexer job for" << job->destination << ":" << job->port
                << "with preprocessed" << job->cpp->preprocessed.size()
                << job->sourceFile;
    }
    assert(job->type == IndexerJob::Remote);
    assert(job->state == IndexerJob::Pending);
    addJob(job);
}

void Server::handleVisitFileMessage(const VisitFileMessage &message, Connection *conn)
{
    uint32_t fileId = 0;
    bool visit = false;

    std::shared_ptr<Project> project = mProjects.value(message.project());
    Path resolved;
    if (project) {
        resolved = message.file().resolved();
        fileId = Location::insertFile(resolved);
        visit = project->visitFile(fileId, message.key());
    }
    VisitFileResponseMessage msg(fileId, resolved, visit);
    conn->send(msg);
}

void Server::restoreFileIds()
{
    const Path p = mOptions.dataDir + "fileids";
    bool clear = true;
    FILE *f = fopen(p.constData(), "r");
    if (f) {
        Hash<Path, uint32_t> pathsToIds;
        Deserializer in(f);
        int version;
        in >> version;
        if (version == DatabaseVersion) {
            int size;
            in >> size;
            if (size != Rct::fileSize(f)) {
                error("Refusing to load corrupted file %s", p.constData());
            } else {
                in >> pathsToIds;
                clear = false;
                Location::init(pathsToIds);
            }
            fclose(f);
        } else {
            error("%s has the wrong format. Got %d, expected %d. Can't restore anything",
                  p.constData(), version, Server::DatabaseVersion);
        }
    }
    if (clear)
        clearProjects();
}

bool Server::saveFileIds() const
{
    if (!Path::mkdir(mOptions.dataDir)) {
        error("Can't create directory [%s]", mOptions.dataDir.constData());
        return false;
    }
    const Path p = mOptions.dataDir + "fileids";
    FILE *f = fopen(p.constData(), "w");
    if (!f) {
        error("Can't open file %s", p.constData());
        return false;
    }
    const Hash<Path, uint32_t> pathsToIds = Location::pathsToIds();
    Serializer out(f);
    out << static_cast<int>(DatabaseVersion);
    const int pos = ftell(f);
    out << static_cast<int>(0) << pathsToIds;
    const int size = ftell(f);
    fseek(f, pos, SEEK_SET);
    out << size;
    fclose(f);
    return true;
}

void Server::onUnload()
{
    std::shared_ptr<Project> cur = mCurrentProject.lock();
    for (ProjectsMap::const_iterator it = mProjects.begin(); it != mProjects.end(); ++it) {
        if (it->second->state() != Project::Unloaded && it->second != cur && !it->second->isIndexing()) {
            it->second->unload();
        }
    }
}

void Server::onReschedule()
{
    const uint64_t now = Rct::monoMs();
    auto it = mProcessingJobs.begin();
    while (it != mProcessingJobs.end()) {
        const std::shared_ptr<IndexerJob>& job = it->second;
        assert(job->state != IndexerJob::Complete);
        if (!job->started) {
            // local job, no need to reschedule
            ++it;
            continue;
        }
        if (static_cast<int>(now - job->started) >= mOptions.rescheduleTimeout) {
            // this might never happen, reschedule this job
            // don't take it out of the mProcessingJobs list since the result might come back still
            if (debugMulti)
                error() << "rescheduling job" << job->sourceFile << job->id;
            mPending.push_back(job);
            startNextJob();
        }
        ++it;
    }
}

void Server::onMulticastReadyRead(const SocketClient::SharedPtr &socket,
                                  const String &ip,
                                  uint16_t port,
                                  Buffer &&in)
{
    const Buffer buffer = std::forward<Buffer>(in);
    handleMulticastData(ip, port, buffer.data(), buffer.size(), 0);
}

/*
  We always give atleast one job to the "process pool" but otherwise active
  threadpool jobs take precedence over rp's
*/
inline int Server::availableJobSlots(JobSlotsMode mode) const
{
    const int count = std::max(mOptions.jobCount - mThreadPool->busyThreads(), 1);
    if (mode == Local)
        return count;
    int ret = mLocalJobs.size();
    for (auto it : mPendingJobRequests) {
        ret += it.second;
    }
    return std::max(0, count - ret);
}

void Server::handleMulticastData(const String &ip, uint16_t port,
                                 const unsigned char *data, int size, Connection *source)
{
    if (!mMulticastForwards.isEmpty()) {
        const MulticastForwardMessage msg(ip, port, String(reinterpret_cast<const char*>(data), size));
        for (auto it = mMulticastForwards.begin(); it != mMulticastForwards.end(); ++it) {
            if (it->second.connection && it->second.connection != source && !it->second.connection->send(msg)) {
                error() << "Unable to forward to"
                        << String::format<64>("%s:%d",
                                              it->first.first.constData(),
                                              it->first.second);
            }
        }
    }
    uint16_t jobs = 0;
    uint16_t tcpPort = 0;

    while (size >= 5) {
        if (*data != 'j') {
            Log log(Error);
            log << "Got unexpected header in data from" << ip << *data;
            return;
        }
        jobs = *reinterpret_cast<const uint16_t*>(data + 1);
        tcpPort = *reinterpret_cast<const uint16_t*>(data + 3);
        if (debugMulti)
            error() << ip << "has" << jobs << "jobs" << "on port" << tcpPort;
        data += 5;
        size -= 5;
    }

    if (size > 0) {
        Log log(Error);
        log << "Got unexpected data from" << ip << size;
        while (size) {
            printf("0x%x", *(data++));
            --size;
        }
        printf("\n");
        return;
    }
    if (jobs && tcpPort) {
        const int maxJobs = std::min<int>(jobs, availableJobSlots(Remote));
        if (debugMulti)
            error("available jobs %d available %d local %d pending %d processcount %d",
                  jobs, availableJobSlots(Remote), mLocalJobs.size(), mPendingJobRequests.size(),
                  mOptions.jobCount);
        if (maxJobs > 0)
            fetchRemoteJobs(ip, tcpPort, maxJobs);
    }
}

void Server::fetchRemoteJobs(const String& ip, uint16_t port, uint16_t jobs)
{
    if (debugMulti)
        error() << "connecting to" << ip << port;
    Connection* conn = new Connection;
    if (!conn->connectTcp(ip, port)) {
        delete conn;
        return;
    }
    if (debugMulti)
        error() << "asking for" << jobs << "jobs";
    conn->newMessage().connect(std::bind(&Server::onNewMessage, this, std::placeholders::_1, std::placeholders::_2));
    conn->disconnected().connect(std::bind(&Server::onConnectionDisconnected, this, std::placeholders::_1));
    conn->finished().connect(std::bind([this, conn]() { mPendingJobRequests.remove(conn); conn->close(); EventLoop::deleteLater(conn); }));
    conn->send(JobRequestMessage(jobs));
    assert(!mPendingJobRequests.contains(conn));
    mPendingJobRequests[conn] = jobs;
}

void Server::addJob(const std::shared_ptr<IndexerJob> &job)
{
    assert(job);
    mPending.push_back(job);
    if (job->flags & IndexerJob::FromRemote)
        ++mRemotePending;
    startNextJob();
}

void Server::startNextJob()
{
    while (!mPending.isEmpty() && mLocalJobs.size() < availableJobSlots(Local)) {
        std::shared_ptr<IndexerJob> job = mPending.first();
        assert(job);
        assert(!(job->state & IndexerJob::Complete|IndexerJob::Running));
        if (!(job->flags & IndexerJob::FromRemote) || project(job->project)) {
            if (!(job->flags & IndexerJob::FromRemote))
                mProcessingJobs[job->id] = job;
            if (job->launchProcess()) {
                if (debugMulti)
                    error() << "started job locally for" << job->sourceFile << job->id;
                mLocalJobs[job->process] = std::make_pair(job, Rct::monoMs());
                job->process->finished().connect(std::bind(&Server::onLocalJobFinished, this,
                                                           std::placeholders::_1));
            } else {
                EventLoop::eventLoop()->callLater(std::bind(&Server::onLocalJobFinished, this, std::placeholders::_1), job->process);
            }
        }
        mPending.pop_front();
    }

    if (mRemotePending >= static_cast<unsigned int>(mPending.size()) || mPending.empty())
        return;
    const uint16_t count = static_cast<uint16_t>(mPending.size() - mRemotePending);
    const uint16_t tcpPort = mOptions.tcpPort;
    unsigned char buf[5];
    buf[0] = 'j';
    memcpy(buf + 1, &count, sizeof(count));
    memcpy(buf + 3, &tcpPort, sizeof(tcpPort));
    if (debugMulti)
        error() << "announcing" << mPending.size() - mRemotePending << "jobs";
    mMulticastSocket->writeTo(mOptions.multicastAddress, mOptions.multicastPort, buf, sizeof(buf));
    if (!mMulticastForwards.isEmpty()) {
        const MulticastForwardMessage msg(String(), 0, String(reinterpret_cast<const char*>(buf), sizeof(buf)));
        for (auto it = mMulticastForwards.begin(); it != mMulticastForwards.end(); ++it) {
            if (it->second.connection && !it->second.connection->send(msg)) {
                error() << "Unable to forward to"
                        << String::format<64>("%s:%d",
                                              it->first.first.constData(),
                                              it->first.second);
            } else if (debugMulti) {
                error() << "forwarding jobs announcement" << mPending.size() - mRemotePending << "jobs";
            }
        }
    }

}

void Server::onLocalJobFinished(Process *process)
{
    auto it = mLocalJobs.find(process);
    assert(it != mLocalJobs.end());
    std::shared_ptr<IndexerJob> &job = it->second.first;
    if (debugMulti)
        error() << "job finished" << job->flags << process->errorString() << process->readAllStdErr();
    if (job->flags & IndexerJob::FromRemote) {
        --mRemotePending;
        error() << "Built remote job" << job->sourceFile.toTilde() << "for"
                << job->destination
                << "in" << (Rct::monoMs() - it->second.second) << "ms";
    }
    job->flags &= ~IndexerJob::Running;
    if (!(job->flags & IndexerJob::Aborted) && (process->returnCode() != 0 || !process->errorString().isEmpty())) {
        job->flags |= IndexerJob::Crashed;
        std::shared_ptr<Project> proj = project(job->project);
        if (proj && proj->state() == Project::Loaded) {
            std::shared_ptr<IndexData> data(new IndexData(job->flags));
            data->key = job->source.key();
            EventLoop::SharedPtr loop = EventLoop::eventLoop();
            assert(loop);
            loop->callLater([proj, data]() { proj->onJobFinished(data); });
        }
    }
    job->process = 0;
    mProcessingJobs.erase(job->id);
    mLocalJobs.erase(it);
    EventLoop::deleteLater(process);
    startNextJob();
    startPreprocessJobs();
}

void Server::handleMulticastForward(const QueryMessage &message, Connection *conn)
{
    const String query = message.query();
    if (query.isEmpty()) {
        reconnectForwards();
        assert(message.type() == QueryMessage::MulticastForward);
        for (auto it = mMulticastForwards.begin(); it != mMulticastForwards.end(); ++it) {
            conn->write<64>("%s:%d %s", it->first.first.constData(), it->first.second,
                            (it->second.connection && it->second.connection->isConnected() ? "connected" : "not connected"));
        }
    } else if (message.type() == QueryMessage::MulticastForward) {
        const std::pair<String, uint16_t> host = RTags::parseHost(query.constData());
        assert(!host.first.isEmpty());
        if (mMulticastForwards.value(host).connection) {
            conn->write<64>("Already connected to host %s:%d", host.first.constData(), host.second);
        } else if (connectMulticastForward(host)) {
            conn->write<64>("Connecting to host %s:%d", host.first.constData(), host.second);
        } else {
            conn->write<64>("Failed to connect to host %s:%d", host.first.constData(), host.second);
        }
    } else {
        assert(message.type() == QueryMessage::RemoveMulticastForward);
        const std::pair<String, uint16_t> host = RTags::parseHost(query.constData());
        assert(!host.first.isEmpty());
        bool ok;
        Connection *c = mMulticastForwards.take(host, &ok).connection;
        if (ok) {
            if (c) {
                conn->write<64>("Disconnecting forward to %s:%d", host.first.constData(), host.second);
                c->finish();
            } else {
                conn->write<64>("Removed forward to %s:%d", host.first.constData(), host.second);
            }
        } else {
            conn->write<64>("No forward to %s:%d", host.first.constData(), host.second);
        }
    }
    conn->finish();
}
bool Server::connectMulticastForward(const std::pair<String, uint16_t> &host)
{
    Forward &forward = mMulticastForwards[host];
    if (forward.connection)
        return true;
    forward.connection = new Connection;
    forward.lastAttempt = Rct::monoMs();
    forward.connection->newMessage().connect(std::bind(&Server::onNewMessage, this, std::placeholders::_1, std::placeholders::_2));
    if (!forward.connection->connectTcp(host.first, host.second)) {
        error() << "Can't connect to multicast forwarding address"
                << String::format<128>("%s:%d", host.first.constData(), host.second);
        delete forward.connection;
        forward.connection = 0;
        ++forward.failures;
        reconnectForwards();
        return false;
    }
    forward.connection->connected().connect(std::bind([this, host]() {
                auto it = mMulticastForwards.find(host);
                if (it != mMulticastForwards.end())
                    it->second.failures = 0;
                error() << "Connected to forwarding address"
                        << String::format<128>("%s:%d", host.first.constData(), host.second); }));
    forward.connection->disconnected().connect(std::bind(&Server::onConnectionDisconnected, this, std::placeholders::_1));
    return true;
}

void Server::handleMulticastForwardMessage(const MulticastForwardMessage &message, Connection *conn)
{
    const String data = message.message();
    String ip = message.ip();
    uint16_t port = message.port();
    assert(ip.isEmpty() == (port == 0));
    if (!port && !conn->client()->peer(&ip, &port)) {
        error() << "Unable to get peer from socket";
        return;
    }
    if (debugMulti)
        error() << "Received forwarded message from" << ip;
    handleMulticastData(ip, port, reinterpret_cast<const unsigned char*>(data.constData()), data.size(), conn);
    // conn->finish(); // ### should I finish this?
}

void Server::stopServers()
{
    Path::rm(mOptions.socketFile);
    mUnixServer.reset();
    mTcpServer.reset();
    mProjects.clear();
}

static inline uint64_t connectTime(uint64_t lastAttempt, int failures)
{
    uint64_t wait = 0;
    if (failures) {
        wait = 1000;
        for (int i=1; i<failures; ++i) {
            wait *= 2;
        }
    }
    return lastAttempt + wait;
}

void Server::reconnectForwards()
{
    const uint64_t now = Rct::monoMs();
    uint64_t least = std::numeric_limits<uint64_t>::max();
    List<std::pair<String, uint16_t> > hosts;
    for (auto it = mMulticastForwards.begin(); it != mMulticastForwards.end(); ++it) {
        if (!it->second.connection) {
            const uint64_t time = connectTime(it->second.lastAttempt, it->second.failures);
            if (time <= now) {
                hosts.append(it->first);
            } else {
                least = std::min(least, time - now);
            }
        }
    }
    if (least != std::numeric_limits<uint64_t>::max()) {
        mReconnectForwardsTimer.restart(least, Timer::SingleShot);
    } else {
        mReconnectForwardsTimer.stop();
    }

    for (auto it = hosts.begin(); it != hosts.end(); ++it) {
        connectMulticastForward(*it); // extra lookup
    }
}

void Server::codeCompleteAt(const QueryMessage &query, Connection *conn)
{
    const String q = query.query();
    Deserializer deserializer(q);
    Path path;
    int line, column;
    deserializer >> path >> line >> column;
    std::shared_ptr<Project> project = updateProjectForLocation(path);
    if (!project) {
        conn->write<128>("No project found for %s", path.constData());
        conn->finish();
        return;
    }
    const uint32_t fileId = Location::insertFile(path);
    const Source source = project->sources(fileId).value(query.buildIndex());
    if (source.isNull()) {
        conn->write<128>("No source found for %s", path.constData());
        conn->finish();
        return;
    }
    if (!mCompletionThread) {
        mCompletionThread = new CompletionThread(10); // ### need setting
        mCompletionThread->start();
    }

    const Location loc(fileId, line, column);
    unsigned int flags = CompletionThread::None;
    if (query.type() == QueryMessage::PrepareCodeCompleteAt)
        flags |= CompletionThread::Refresh;
    if (query.flags() & QueryMessage::ElispList)
        flags |= CompletionThread::Elisp;
    mCompletionThread->completeAt(source, loc, flags, query.unsavedFiles().value(path));
    conn->finish();
    error() << "Got completion" << query.type() << path << line << column;
}

int Server::startPreprocessJobs()
{
    int ret = 0;
    int size = mPending.size() + mThreadPool->backlockSize() + mThreadPool->busyThreads() + mProcessingJobs.size();
    enum { MaxPending = 50 };
    while (size < MaxPending && !mPendingPreprocessJobs.isEmpty()) {
        ++ret;
        ++size;
        std::shared_ptr<PreprocessJob> job = mPendingPreprocessJobs.front();
        mPendingPreprocessJobs.pop_front();
        mThreadPool->start(job);
    }
    return ret;
}
