/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * Copyright the Collabora Online contributors.
 *
 * SPDX-License-Identifier: MPL-2.0
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <common/ConfigUtil.hpp>
#include <common/FileUtil.hpp>
#include <common/Util.hpp>
#include <net/WebSocketHandler.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <utility>

#include <signal.h>

#include <Poco/Path.h>
#include <Poco/Util/AbstractConfiguration.h>
#include <Poco/Util/OptionSet.h>
#include <Poco/Util/ServerApplication.h>

class ChildProcess;
class TraceFileWriter;
class DocumentBroker;
class ClipboardCache;
class FileServerRequestHandler;

std::shared_ptr<ChildProcess> getNewChild_Blocks(SocketPoll &destPoll, unsigned mobileAppDocId);

// A WSProcess object in the WSD process represents a descendant process, either the direct child
// process ForKit or a grandchild Kit process, with which the WSD process communicates through a
// WebSocket.
class WSProcess
{
public:
    /// @param pid is the process ID.
    /// @param socket is the underlying Socket to the process.
    WSProcess(const std::string& name,
              const pid_t pid,
              const std::shared_ptr<StreamSocket>& socket,
              std::shared_ptr<WebSocketHandler> handler) :

        _name(name),
        _pid(pid),
        _ws(std::move(handler)),
        _socket(socket)
    {
        LOG_INF(_name << " ctor [" << _pid << "].");
    }

    WSProcess(WSProcess&& other) = delete;

    const WSProcess& operator=(WSProcess&& other) = delete;

    virtual ~WSProcess()
    {
        LOG_DBG('~' << _name << " dtor [" << _pid << "].");

        if (_pid <= 0)
            return;

        terminate();

        // No need for the socket anymore.
        _ws.reset();
        _socket.reset();
    }

    /// Let the child close a nice way.
    void close()
    {
        if (_pid < 0)
            return;

        try
        {
            LOG_DBG("Closing ChildProcess [" << _pid << "].");

            requestTermination();

            // Shutdown the socket.
            if (_ws)
                _ws->shutdown();
        }
        catch (const std::exception& ex)
        {
            LOG_ERR("Error while closing child process: " << ex.what());
        }

        _pid = -1; // Detach from child.
    }

    /// Request graceful termination.
    void requestTermination()
    {
        // Request the child to exit
        if (isAlive())
        {
            LOG_DBG("Stopping ChildProcess [" << _pid << "] by sending 'exit' command");
            sendTextFrame("exit", /*flush=*/true);
        }
    }

    /// Kill or abandon the child.
    void terminate()
    {
        if (_pid < 0)
            return;

#if !MOBILEAPP
        if (::kill(_pid, 0) == 0)
        {
            LOG_INF("Killing child [" << _pid << "].");
#if CODE_COVERAGE || VALGRIND_COOLFORKIT
            constexpr auto signal = SIGTERM;
#else
            constexpr auto signal = SIGKILL;
#endif
            if (!SigUtil::killChild(_pid, signal))
            {
                LOG_ERR("Cannot terminate lokit [" << _pid << "]. Abandoning.");
            }
        }
#else
        // What to do? Throw some unique exception that the outermost call in the thread catches and
        // exits from the thread?
#endif
        _pid = -1;
    }

    pid_t getPid() const { return _pid; }

    /// Send a text payload to the child-process WS.
    bool sendTextFrame(const std::string& data, bool flush = false)
    {
        return sendFrame(data, false, flush);
    }

    /// Send a payload to the child-process WS.
    bool sendFrame(const std::string& data, bool binary = false, bool flush = false)
    {
        try
        {
            if (_ws)
            {
                LOG_TRC("Send to " << _name << " message: ["
                                   << COOLProtocol::getAbbreviatedMessage(data) << ']');
                _ws->sendMessage(data.c_str(), data.size(),
                                 (binary ? WSOpCode::Binary : WSOpCode::Text), flush);
                return true;
            }
        }
        catch (const std::exception& exc)
        {
            LOG_ERR("Failed to send " << _name << " [" << _pid << "] data [" <<
                    COOLProtocol::getAbbreviatedMessage(data) << "] due to: " << exc.what());
            throw;
        }

        LOG_WRN("No socket to " << _name << " to send [" << COOLProtocol::getAbbreviatedMessage(data) << ']');
        return false;
    }

    /// Check whether this child is alive and socket not in error.
    /// Note: zombies will show as alive, and sockets have waiting
    /// time after the other end-point closes. So this isn't accurate.
    virtual bool isAlive() const
    {
#if !MOBILEAPP
        try
        {
            return _pid > 1 && _ws && ::kill(_pid, 0) == 0;
        }
        catch (const std::exception&)
        {
        }

        return false;
#else
        return _pid > 1;
#endif
    }

protected:
    std::shared_ptr<WebSocketHandler> getWSHandler() const { return _ws; }
    std::shared_ptr<StreamSocket> getSocket() const { return _socket.lock(); };

private:
    std::string _name;
    std::atomic<pid_t> _pid; ///< The process-id, which can be access from different threads.
    std::shared_ptr<WebSocketHandler> _ws;  // FIXME: should be weak ? ...
    std::weak_ptr<StreamSocket> _socket;
};

#if !MOBILEAPP

class ForKitProcWSHandler final : public WebSocketHandler
{
public:
    template <typename T>
    ForKitProcWSHandler(const std::weak_ptr<StreamSocket>& socket, const T& request)
        : WebSocketHandler(socket.lock(), request)
    {
    }

    virtual void handleMessage(const std::vector<char>& data) override;
};

class ForKitProcess final : public WSProcess
{
public:
    template <typename T>
    ForKitProcess(int pid, std::shared_ptr<StreamSocket>& socket, const T& request)
        : WSProcess("ForKit", pid, socket, std::make_shared<ForKitProcWSHandler>(socket, request))
    {
        socket->setHandler(getWSHandler());
    }
};

#endif

/// The Server class which is responsible for all
/// external interactions.
class COOLWSD final : public Poco::Util::ServerApplication,
                      public UnitWSDInterface
{
public:
    COOLWSD();
    ~COOLWSD();

    // An Application is a singleton anyway,
    // so just keep these as statics.
    static std::atomic<uint64_t> NextConnectionId;
    static unsigned int NumPreSpawnedChildren;
#if !MOBILEAPP
    static bool NoCapsForKit;
    static bool NoSeccomp;
    static bool AdminEnabled;
    static bool UnattendedRun; ///< True when run from an unattended test, not interactive.
    static bool SignalParent;
    static bool UseEnvVarOptions;
    static std::string RouteToken;
#if ENABLE_DEBUG
    static bool SingleKit;
    static bool ForceCaching;
#endif
    static std::shared_ptr<ForKitProcess> ForKitProc;
    static std::atomic<int> ForKitProcId;
#endif
    static std::string UserInterface;
    static std::string ConfigFile;
    static std::string ConfigDir;
    static std::string SysTemplate;
    static std::string LoTemplate;
    static std::string CleanupChildRoot;
    static std::string ChildRoot;
    static std::string ServerName;
    static std::string FileServerRoot;
    static std::string ServiceRoot; ///< There are installations that need prefixing every page with some path.
    static std::string TmpFontDir;
    static std::string LOKitVersion;
    static bool EnableTraceEventLogging;
    static bool EnableAccessibility;
    static bool EnableMountNamespaces;
    static FILE *TraceEventFile;
    static void writeTraceEventRecording(const char *data, std::size_t nbytes);
    static void writeTraceEventRecording(const std::string &recording);
    static std::string LogLevel;
    static std::string LogLevelStartup;
    static std::string LogDisabledAreas;
    static std::string LogToken;
    static std::string MostVerboseLogLevelSettableFromClient;
    static std::string LeastVerboseLogLevelSettableFromClient;
    static bool AnonymizeUserData;
    static bool CheckCoolUser;
    static bool CleanupOnly;
    static bool IsProxyPrefixEnabled;
    static std::atomic<unsigned> NumConnections;
    static std::unique_ptr<TraceFileWriter> TraceDumper;
    static bool IndirectionServerEnabled;
    static bool GeolocationSetup;
#if !MOBILEAPP
    static std::unique_ptr<ClipboardCache> SavedClipboards;

    /// The file request handler used for file-serving.
    static std::unique_ptr<FileServerRequestHandler> FileRequestHandler;

    /// The WASM support/activation state.
    enum class WASMActivationState
    {
        Disabled,
        Enabled
#if ENABLE_DEBUG
        ,
        Forced ///< When Forced, only WASM is served.
#endif
    };
    static WASMActivationState WASMState;

    /// Tracks the URIs that are switching to Disconnected (WASM) Mode.
    /// The time is when the switch request was made. We expire the request after a certain
    /// time, in case the user fails to load WASM, it will revert to Collaborative mode.
    static std::unordered_map<std::string, std::chrono::steady_clock::time_point> Uri2WasmModeMap;
#endif

    static std::unordered_set<std::string> EditFileExtensions;
    static unsigned MaxConnections;
    static unsigned MaxDocuments;
    static std::string HardwareResourceWarning;
    static std::string OverrideWatermark;
    static std::set<const Poco::Util::AbstractConfiguration*> PluginConfigurations;
    static std::chrono::steady_clock::time_point StartTime;
    static std::string BuyProductUrl;
    static std::string LatestVersion;
    static std::mutex FetchUpdateMutex;
    static bool IsBindMountingEnabled;
    static std::mutex RemoteConfigMutex;
#if MOBILEAPP
#ifndef IOS
    /// This is used to be able to wait until the lokit main thread has finished (and it is safe to load a new document).
    static std::mutex lokit_main_mutex;
#endif
#endif

    /// For testing only [!]
    static int getClientPortNumber();
    /// For testing only [!] DocumentBrokers are mostly single-threaded with their own thread
    static std::vector<std::shared_ptr<DocumentBroker>> getBrokersTestOnly();

    // Return a map for fast searches. Used in testing and in admin for cleanup
    static std::set<pid_t> getKitPids();
    static std::set<pid_t> getSpareKitPids();
    static std::set<pid_t> getDocKitPids();

    static std::string GetConnectionId()
    {
        return Util::encodeId(NextConnectionId++, 3);
    }

    static const std::string& getHardwareResourceWarning()
    {
        return HardwareResourceWarning;
    }

    static bool isSSLTermination() { return ConfigUtil::isSSLTermination(); }

    static std::shared_ptr<TerminatingPoll> getWebServerPoll();

    /// Return true if extension is marked as view action in discovery.xml.
    static bool IsViewFileExtension(const std::string& extension)
    {
        std::string lowerCaseExtension = extension;
        std::transform(lowerCaseExtension.begin(), lowerCaseExtension.end(), lowerCaseExtension.begin(), ::tolower);
        if constexpr (Util::isMobileApp())
        {
            if (lowerCaseExtension == "pdf")
                return true; // true for only pdf - it is not editable
            return false; // mark everything else editable on mobile
        }
        return EditFileExtensions.find(lowerCaseExtension) == EditFileExtensions.end();
    }

    /// Trace a new session and take a snapshot of the file.
    static void dumpNewSessionTrace(const std::string& id, const std::string& sessionId, const std::string& uri, const std::string& path);

    /// Trace the end of a session.
    static void dumpEndSessionTrace(const std::string& id, const std::string& sessionId, const std::string& uri);

    static void dumpEventTrace(const std::string& id, const std::string& sessionId, const std::string& data);

    static void dumpIncomingTrace(const std::string& id, const std::string& sessionId, const std::string& data);

    static void dumpOutgoingTrace(const std::string& id, const std::string& sessionId, const std::string& data);

    /// Waits on Forkit and reaps if it dies, then restores.
    /// Return true if wait succeeds.
    static bool checkAndRestoreForKit();

    /// Creates a new instance of Forkit.
    /// Return true when successful.
    static bool createForKit();

    /// Sends a message to ForKit through PrisonerPoll.
    static void sendMessageToForKit(const std::string& message);

    /// Terminates spare kits that aren't assigned a document yet.
    static void requestTerminateSpareKits();

    /// Checks forkit (and respawns), rebalances
    /// child kit processes and cleans up DocBrokers.
    static void doHousekeeping();

    static void checkDiskSpaceAndWarnClients(const bool cacheLastCheck);

    static void checkSessionLimitsAndWarnClients();

    /// Close document with @docKey and a @message
    static void closeDocument(const std::string& docKey, const std::string& message);

    /// Autosave a given document (currently only called from Admin).
    static void autoSave(const std::string& docKey);

    /// Sets the log level of current kits.
    static void setLogLevelsOfKits(const std::string& level);

    /// Anonymize the basename of filenames, preserving the path and extension.
    static std::string anonymizeUrl(const std::string& url)
    {
        return FileUtil::anonymizeUrl(url);
    }

    /// Anonymize user names and IDs.
    /// Will use the Obfuscated User ID if one is provided via WOPI.
    static std::string anonymizeUsername(const std::string& username)
    {
        return FileUtil::anonymizeUsername(username);
    }
    static void alertAllUsersInternal(const std::string& msg);
    static void alertUserInternal(const std::string& dockey, const std::string& msg);
    static void setMigrationMsgReceived(const std::string& docKey);
    static void setAllMigrationMsgReceived();

#if ENABLE_DEBUG
    /// get correct server URL with protocol + port number for this running server
    static std::string getServerURL();
#endif

protected:
    void initialize(Poco::Util::Application& self) override
    {
        try
        {
            innerInitialize(self);
        }
        catch (const Poco::Exception& ex)
        {
            LOG_FTL("Failed to initialize COOLWSD: "
                    << ex.displayText()
                    << (ex.nested() ? " (" + ex.nested()->displayText() + ')' : ""));
            throw; // Nothing further to do.
        }
        catch (const std::exception& ex)
        {
            LOG_FTL("Failed to initialize COOLWSD: " << ex.what());
            throw; // Nothing further to do.
        }
    }

    void defineOptions(Poco::Util::OptionSet& options) override;
    void handleOption(const std::string& name, const std::string& value) override;
    int main(const std::vector<std::string>& args) override;

    /// Handle various global static destructors.
    static void cleanup();

private:
#if !MOBILEAPP
    void processFetchUpdate(SocketPoll& poll);
    static void setupChildRoot(const bool UseMountNamespaces);
    void initializeEnvOptions();
#endif // !MOBILEAPP

    void initializeSSL();
    void displayHelp();

    /// The actual initialize implementation.
    void innerInitialize(Application& self);

    /// The actual main implementation.
    int innerMain();

    static void appendAllowedHostsFrom(Poco::Util::LayeredConfiguration& conf, const std::string& root, std::vector<std::string>& allowed);
    static void appendAllowedAliasGroups(Poco::Util::LayeredConfiguration& conf, std::vector<std::string>& allowed);

private:
    /// UnitWSDInterface
    virtual std::string getJailRoot(int pid) override;

    /// Settings passed from the command-line to override those in the config file.
    std::map<std::string, std::string> _overrideSettings;

#if MOBILEAPP
public:
    static int prisonerServerSocketFD;
#endif
};

void setKitInProcess();

int createForkit(const std::string& forKitPath, const StringVector& args);

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
