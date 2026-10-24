#ifndef PLAYDATA_HPP
#define PLAYDATA_HPP

#include "nx/Title.hpp"
#include "Types.hpp"
#include <vector>
#include <future>
#include <functional>
#include <atomic>
#include <map>
#include <set>
#include <limits>

namespace NX {
    // Progress callback function type
    typedef std::function<void(int, int)> ProgressCallback;

    // Enumeration for event type in PlayEvent struct
    enum PlayEventType {
        PlayEvent_Applet,       // PlayEvent contains applet event
        PlayEvent_Account       // PlayEvent contains account event
    };

    // Enumeration for applet/account event type in PlayEvent struct
    enum EventType {
        Applet_Launch,          // Applet launched
        Applet_Exit,            // Applet quit
        Applet_InFocus,         // Applet gained focus
        Applet_OutFocus,        // Applet lost focus
        Account_Active,         // Account selected
        Account_Inactive        // Account closed?
    };

    // PlayEvents are parsed PdmPlayEvents containing only necessary information
    struct PlayEvent {
        PlayEventType type;        // Type of PlayEvent
        AccountUid userID;         // UserID
        TitleID titleID;           // TitleID
        EventType eventType;       // See EventType enum
        u64 clockTimestamp;        // Time of event
        u64 steadyTimestamp;       // Steady timestamp (used for calculating duration)
    };

    // A PlaySession represents a session of play for a game. It contains the start
    // and end timestamps and playtime for convenience. Note that (end-start) != playtime
    // due to time when the game may not have been focussed.
    struct PlaySession {
        u64 playtime;           // Total playtime in seconds
        u64 startTimestamp;     // Time of launch
        u64 endTimestamp;       // Time of exit
    };

    // PdmPlayStatistics but only the necessary things
    struct PlayStatistics {
        TitleID titleID;        // TitleID of these stats
        u64 firstPlayed;        // Timestamp of first launch
        u64 lastPlayed;         // Timestamp of last play (exit)
        u64 playtime;           // Total playtime in seconds
        u32 launches;           // Total launches
    };

    // RecentPlayStatistics struct is similar to PdmPlayStatistics but
    // only contains recent values
    struct RecentPlayStatistics {
        TitleID titleID;        // TitleID of these statistics
        u64 playtime;           // Total playtime in seconds
        u32 launches;           // Total launches
    };

    // Struct used for analyzing/splitting up play sessions
    struct PD_Session {
        size_t index;   // Index of first (launch) event
        size_t num;     // Number of events for this session
    };

    // Pair of play events and summaries
    typedef std::pair<std::vector<PlayEvent *>, std::vector<PlayStatistics *>> PlayEventsAndSummaries;

    // PlayData stores PlayEvents which are created from processing PlayEvent.dat using pdm.
    // The data can then be queried across a period of time, with the summation of these
    // statistics being returned. It also offers wrappers for some pdm functions.
    class PlayData {
        private:
            // Vector containing created PlayEvents
            // Should be in chronological order (ie. as read from pdm)
            std::vector<PlayEvent *> events;

            // Vector containing all read summaries
            // Not in any specific order
            std::vector<PlayStatistics *> summaries;

            // Vector of titles in imported data
            std::vector<std::pair<u64, std::string>> importTitles;

            // Timestamp indicating when summaries were imported
            // Used to "merge" with system stats
            uint64_t importTimestamp;

            // Async threads for reading data
            std::future<PlayEventsAndSummaries> pdmThread;
            std::future<PlayEventsAndSummaries> impThread;

            // Progress tracking
            std::atomic<int> currentProgress;
            std::atomic<int> maxProgress;

            // Progress callback function
            ProgressCallback progressCallback;

            // Whether sessions launched without any account (homebrew forwarders,
            // emulators, ports...) are treated as belonging to every user
            bool includeUnassigned_;

            // IDs of installed applications. Only these are ever accepted as "unassigned"
            // so that system applets/hbloader/etc. are not mistaken for games
            std::set<TitleID> installedIDs;

            // Whether a title may be shown for sessions without an account: either it is installed,
            // or its ID is in the range used by user applications (so titles that have since been
            // uninstalled are kept, while system applets/hbloader, which sit below that range, are not)
            bool isUnassignedEligible(TitleID) const;

            // Accounts that appear in the events but no longer exist on the console (as 16 bytes: both words)
            std::set<std::pair<u64, u64>> deletedUsers;

            // IDs of titles that have at least one session with an account. Such titles are real games, so
            // any session of theirs without an account (e.g. leaving the user selection) isn't "unassigned"
            std::set<TitleID> accountTitles;

            bool isDeletedUser(AccountUid) const;

            // IDs of installed applications that have at least one session without an account
            std::set<TitleID> unassignedTitles;

            // Return vector of PD_Sessions for given title/user IDs + time range
            // Give a titleID of zero to include all titles
            // The last parameter allows sessions with no account event at all to be accepted
            std::vector<PD_Session> getPDSessions(TitleID, AccountUid, u64, u64, bool);

            // Counts playtime and launches for the given sessions
            // TitleID in returned struct is zero
            RecentPlayStatistics * countPlaytime(std::vector<PD_Session>, u64, u64);

            // Reads and parses data using pdm
            PlayEventsAndSummaries readPlayDataFromPdm();

            // Reads and parses data from imported file
            PlayEventsAndSummaries readPlayDataFromImport();

        public:
            // The constructor prepares + creates PlayEvents
            PlayData();

            // The destructor deletes PlayEvents (frees memory)
            ~PlayData();

            void setProgressCallback(ProgressCallback callback);

            // Gets current progress of data initialization
            int getCurrentProgress() const;
            int getMaxProgress() const;
            void setCurrentProgress(int current);
            void setMaxProgress(int max);

            // Merges the data within the two passed vectors by only keeping unique PlayEvents
            void initPlayEvents();

            // Enable/disable treating sessions without any account as belonging to every user
            void setIncludeUnassigned(bool);
            bool includeUnassigned() const;

            // Returns the accounts found in the events which are not in the given list (deleted profiles).
            // These are remembered: sessions without an account are never attributed to them
            std::vector<AccountUid> findDeletedUsers(const std::vector<AccountUid> &);

            // Returns the IDs of all titles with sessions belonging to the given account
            std::vector<TitleID> getTitleIDsForUser(AccountUid);

            // Sets the IDs of installed applications (call before getUnassignedTitleIDs())
            void setInstalledTitleIDs(const std::vector<TitleID> &);

            // Writes a plain text report of how sessions are being attributed (which accounts appear in the
            // events, whether they still exist, and which titles have sessions without any account)
            // to the given path. Used to investigate wrong playtime; the names map is only for readability
            void writeDiagnostics(const std::vector<AccountUid> &, const std::map<TitleID, std::string> &, const std::string &);

            // Scans the events and returns the IDs of installed applications that have sessions
            // without any account. Also remembers them for use in getStatisticsForUser()
            std::vector<TitleID> getUnassignedTitleIDs();

            // Returns title objects that are not present in the given vector
            std::vector<Title *> getMissingTitles(std::vector<Title *>);

            // Returns vector containing PlayEvents between the given times
            // Start time, end time, titleID, userID
            std::vector<PlayEvent> getPlayEvents(u64, u64, TitleID, AccountUid);

            // Returns all play sessions for the given title ID and user ID
            std::vector<PlaySession> getPlaySessionsForUser(TitleID, AccountUid);

            // Returns a RecentPlayStatistics for the given time range and user ID for all Title IDs
            RecentPlayStatistics * getRecentStatisticsForUser(u64, u64, AccountUid);

            // Returns a RecentPlayStatistics for the given time range, user ID and title ID
            RecentPlayStatistics * getRecentStatisticsForTitleAndUser(TitleID, u64, u64, AccountUid);

            // Returns a PlayStatistics for the given titleID and userID
            PlayStatistics * getStatisticsForUser(TitleID, AccountUid);
    };
};

#endif
