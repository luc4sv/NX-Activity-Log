#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include "nlohmann/json.hpp"
#include "nx/PlayData.hpp"
#include "utils/NX.hpp"
#include "utils/Time.hpp"

// Maximum number of entries to process in one iteration
#define MAX_PROCESS_ENTRIES 32768

namespace NX {
    std::vector<PD_Session> PlayData::getPDSessions(TitleID titleID, AccountUid userID, u64 start_ts, u64 end_ts, bool allowUnassigned) {
        // Break each "session" apart and keep if matching titleID and userID
        std::vector<PD_Session> sessions;
        size_t a = 0;
        while (a < this->events.size()) {
            if (this->events[a]->eventType == Applet_Launch) {
                // Find end of session (or start of next session if switch crashed before exit)
                size_t s = a;
                bool time_c = false;
                bool titleID_c = false;
                if (titleID == 0 || this->events[a]->titleID == titleID) {
                    titleID_c = true;
                }
                bool userID_c = false;
                // True if any account event is found within this session
                bool hasAccount = false;
                // Sessions with no account event are only accepted for applications (not system applets)
                const bool unassignedOK = allowUnassigned && !this->isDeletedUser(userID) && this->accountTitles.count(this->events[s]->titleID) == 0 && this->isUnassignedEligible(this->events[s]->titleID);

                a++;
                bool end = false;
                while (a < this->events.size() && !end) {
                    // Check if every event is in the required range
                    if (this->events[a]->clockTimestamp >= start_ts && this->events[a]->clockTimestamp <= end_ts) {
                        time_c = true;
                    }
                    // Also check if there is an event prior to event and after
                    if (a > s) {
                        if (this->events[a]->clockTimestamp > end_ts && this->events[a-1]->clockTimestamp < start_ts) {
                            time_c = true;
                        }
                    }

                    switch (this->events[a]->eventType) {
                        // Check userID whenever account event encountered
                        case Account_Active:
                        case Account_Inactive:
                            hasAccount = true;
                            if (this->events[a]->userID == userID) {
                                userID_c = true;
                            }
                            break;

                        // Exit indicates end of session
                        case Applet_Exit:
                            if (time_c && titleID_c && (userID_c || (unassignedOK && !hasAccount))) {
                                struct PD_Session st;
                                st.index = s;
                                st.num = a - s + 1;
                                sessions.emplace_back(st);
                            }
                            end = true;
                            break;

                        // Encountering another launch also indicates end of session (due to crash)
                        case Applet_Launch:
                            if (time_c && titleID_c && (userID_c || (unassignedOK && !hasAccount))) {
                                struct PD_Session st;
                                st.index = s;
                                st.num = a - s;
                                sessions.emplace_back(st);
                            }
                            end = true;
                            a--;
                            break;

                        default:
                            break;
                    }
                    a++;
                }
            } else {
                a++;
            }
        }

        return sessions;
    }

    RecentPlayStatistics * PlayData::countPlaytime(std::vector<PD_Session> sessions, u64 start_ts, u64 end_ts) {
        // Count playtime + launches
        RecentPlayStatistics * stats = new RecentPlayStatistics;
        stats->titleID = 0;
        stats->playtime = 0;
        stats->launches = 0;

        if (sessions.size() == 0) {
            return stats;
        }

        // Iterate over valid sessions to calculate statistics
        for (size_t i = 0; i < sessions.size(); i++) {
            stats->launches++;

            u64 last_ts = 0;
            u64 last_clock = 0;
            bool in_before = false;
            bool done = false;
            for (size_t j = sessions[i].index; j < sessions[i].index + sessions[i].num; j++) {
                if (done) {
                    break;
                }

                switch (this->events[j]->eventType) {
                    case Applet_Launch:
                        // Skip to first in focus event
                        while (j+1 < this->events.size()) {
                            if (this->events[j+1]->eventType != Applet_InFocus) {
                                j++;
                            } else {
                                break;
                            }
                        }
                    case Applet_Exit:
                    case Account_Active:
                    case Account_Inactive:
                        break;

                    case Applet_InFocus:
                        last_ts = this->events[j]->steadyTimestamp;
                        last_clock = this->events[j]->clockTimestamp;
                        in_before = false;
                        if (this->events[j]->clockTimestamp < start_ts) {
                            last_clock = start_ts;
                            in_before = true;
                        } else if (this->events[j]->clockTimestamp >= end_ts) {
                            done = true;
                        }
                        break;

                    case Applet_OutFocus:
                        if (this->events[j]->clockTimestamp >= end_ts) {
                            stats->playtime += (end_ts - last_clock);
                        } else if (this->events[j]->clockTimestamp >= start_ts) {
                            if (in_before) {
                                stats->playtime += (this->events[j]->clockTimestamp - last_clock);
                            } else {
                                stats->playtime += (this->events[j]->steadyTimestamp - last_ts);
                            }
                        }

                        // Move to last out focus (I don't know why the log has multiple)
                        while (j+1 < this->events.size()) {
                            if (this->events[j+1]->eventType == Applet_OutFocus) {
                                j++;
                            } else {
                                break;
                            }
                        }
                        break;
                }
            }
        }

        return stats;
    }

    PlayEventsAndSummaries PlayData::readPlayDataFromPdm() {
        Result rc;
        PlayEventsAndSummaries ret;

        // Position of first event to read
        s32 offset = 0;
        // Total events read in iteration
        s32 playedTotal = 1;

        // Array to store read events
        PdmPlayEvent *pEvents = new PdmPlayEvent[MAX_PROCESS_ENTRIES];

        // Read all events
        while (playedTotal > 0) {
            memset(pEvents, 0, MAX_PROCESS_ENTRIES * sizeof(PdmPlayEvent));
            rc = pdmqryQueryPlayEvent(offset, pEvents, MAX_PROCESS_ENTRIES, &playedTotal);
            if (R_FAILED(rc) || playedTotal == 0) {
                break;
            }

            // Set next read position to next event
            offset += playedTotal;

            // Process read events
            for (s32 i = 0; i < playedTotal; i++) {
                PlayEvent *event;
                TitleID tmpID;

                // Populate PlayEvent based on event type
                switch (pEvents[i].play_event_type) {
                    case PdmPlayEventType_Account:
                        // Ignore this event if type is 2
                        if (pEvents[i].event_data.account.type == 2) {
                            continue;
                        }

                        event = new PlayEvent;
                        event->type = PlayEvent_Account;

                        // UserID words are wrong way around (why Nintendo?)
                        event->userID.uid[0] = pEvents[i].event_data.account.uid[0];
                        event->userID.uid[0] = (event->userID.uid[0] << 32) | pEvents[i].event_data.account.uid[1];
                        event->userID.uid[1] = (event->userID.uid[1] << 32) | pEvents[i].event_data.account.uid[2];
                        event->userID.uid[1] = (event->userID.uid[1] << 32) | pEvents[i].event_data.account.uid[3];

                        // Set account event type
                        switch (pEvents[i].event_data.account.type) {
                            case 0:
                                event->eventType = Account_Active;
                                break;
                            case 1:
                                event->eventType = Account_Inactive;
                                break;
                        }
                        break;
                    case PdmPlayEventType_Applet:
                        // Ignore this event based on log policy
                        if (pEvents[i].event_data.applet.log_policy != PdmPlayLogPolicy_All) {
                            continue;
                        }

                        // Ignore event with titleID 0
                        tmpID = pEvents[i].event_data.applet.program_id[0];
                        tmpID = (tmpID << 32) | pEvents[i].event_data.applet.program_id[1];
                        if (tmpID == 0) {
                            continue;
                        }

                        event = new PlayEvent;
                        event->type = PlayEvent_Applet;

                        // Join two halves of title ID
                        event->titleID = tmpID;

                        // Set applet event type
                        switch (pEvents[i].event_data.applet.event_type) {
                            case PdmAppletEventType_Launch:
                                event->eventType = Applet_Launch;
                                break;
                            case PdmAppletEventType_Exit:
                            case PdmAppletEventType_Exit5:
                            case PdmAppletEventType_Exit6:
                                event->eventType = Applet_Exit;
                                break;
                            case PdmAppletEventType_InFocus:
                                event->eventType = Applet_InFocus;
                                break;
                            case PdmAppletEventType_OutOfFocus:
                            case PdmAppletEventType_OutOfFocus4:
                                event->eventType = Applet_OutFocus;
                                break;
                        }
                        break;
                    // Do nothing for other event types
                    default:
                        continue;
                        break;
                }

                // Set timestamps
                event->clockTimestamp = pEvents[i].timestamp_user;
                event->steadyTimestamp = pEvents[i].timestamp_steady;

                // Add PlayEvent to vector
                ret.first.emplace_back(event);
            }
        }

        // Free memory allocated to array
        delete[] pEvents;

        return ret;
    }

    PlayEventsAndSummaries PlayData::readPlayDataFromImport() {
        PlayEventsAndSummaries ret;

        // Abort if no file
        if (!std::filesystem::exists("/switch/NX-Activity-Log/importedData.json")) {
            return ret;
        }

        // Read in file
        std::ifstream file("/switch/NX-Activity-Log/importedData.json");
        nlohmann::json json = nlohmann::json::parse(file);
        if (json["users"] == nullptr || json["importTimestamp"] == nullptr) {
            return ret;
        }

        this->importTimestamp = json["importTimestamp"];

        // Iterate over each user
        for (nlohmann::json user : json["users"]) {
            if (user["titles"] == nullptr) {
                continue;
            }

            // Iterate over each title
            for (nlohmann::json title : user["titles"]) {
                if (title["id"] == 0) {
                    continue;
                }

                bool hasEntry = false;

                // Create events for title
                if (title["events"] != nullptr) {
                    for (nlohmann::json event : title["events"]) {
                        if (event["clockTimestamp"] != nullptr && event["steadyTimestamp"] != nullptr && event["type"] != nullptr) {
                            EventType type = static_cast<EventType>(event["type"]);

                            PlayEvent *evt = new PlayEvent;
                            evt->type = (type == Account_Active || type == Account_Inactive ? PlayEvent_Account : PlayEvent_Applet);
                            evt->userID = {user["id"][0], user["id"][1]};
                            evt->titleID = title["id"];
                            evt->eventType = type;
                            evt->clockTimestamp = event["clockTimestamp"];
                            evt->steadyTimestamp = event["steadyTimestamp"];

                            hasEntry = true;
                            ret.first.emplace_back(evt);
                        }
                    }
                }

                // Create summary for title
                if (title["summary"] != nullptr) {
                    nlohmann::json summary = title["summary"];
                    if (summary["firstPlayed"] != nullptr && summary["lastPlayed"] != nullptr && summary["playtime"] != nullptr && summary["launches"] != nullptr) {
                        PlayStatistics * stats = new PlayStatistics;
                        stats->titleID = title["id"];
                        stats->firstPlayed = summary["firstPlayed"];
                        stats->lastPlayed = summary["lastPlayed"];
                        stats->playtime = summary["playtime"];
                        stats->launches = summary["launches"];

                        hasEntry = true;
                        ret.second.emplace_back(stats);
                    }
                }

                // Store data if entry added
                if (hasEntry) {
                    if (std::find_if(this->importTitles.begin(), this->importTitles.end(), [title](std::pair<u64, std::string> entry) { return (title["id"] == entry.first); }) == this->importTitles.end()) {
                        if (title["id"] != 0) {
                            this->importTitles.emplace_back(std::make_pair(title["id"], title["name"]));
                        }
                    }
                }
            }
        }
        return ret;
    }

    PlayData::PlayData() : includeUnassigned_(false), currentProgress(0), maxProgress(1), progressCallback(nullptr) {
        // Read in all data simultaneously
        this->pdmThread = std::async(std::launch::async, [this]() -> PlayEventsAndSummaries {
            return this->readPlayDataFromPdm();
        });
        this->impThread = std::async(std::launch::async, [this]() -> PlayEventsAndSummaries {
            return this->readPlayDataFromImport();
        });
    }

    void PlayData::initPlayEvents() {
        // Update progress
        PlayEventsAndSummaries pdmData = this->pdmThread.get();
        if (this->progressCallback) {
            this->currentProgress.store(this->currentProgress.load() + 1);
            this->progressCallback(this->currentProgress.load(), this->maxProgress.load());
        }

        PlayEventsAndSummaries impData = this->impThread.get();
        if (this->progressCallback) {
            this->currentProgress.store(this->currentProgress.load() + 1);
            this->progressCallback(this->currentProgress.load(), this->maxProgress.load());
        }

        this->summaries = impData.second;
        this->events = pdmData.first;
        for (PlayEvent * event : impData.first) {
            std::vector<PlayEvent *>::iterator it = std::find_if(pdmData.first.begin(), pdmData.first.end(), [event](PlayEvent * pot) {
                return (event->type == pot->type && event->titleID == pot->titleID &&
                        event->eventType == pot->eventType && event->clockTimestamp == pot->clockTimestamp && event->steadyTimestamp == pot->steadyTimestamp);
            });

            // Copy event or delete if duplicate
            if (it == pdmData.first.end()) {
                this->events.emplace_back(event);
            } else {
                delete event;
            }
        }
        if (this->progressCallback) {
            this->currentProgress.store(this->currentProgress.load() + 1);
            this->progressCallback(this->currentProgress.load(), this->maxProgress.load());
        }

        pdmData.first.clear();
        impData.first.clear();
    }

    bool PlayData::isDeletedUser(AccountUid uid) const {
        return this->deletedUsers.count(std::make_pair(uid.uid[0], uid.uid[1])) > 0;
    }

    std::vector<AccountUid> PlayData::findDeletedUsers(const std::vector<AccountUid> & current) {
        std::vector<AccountUid> found;
        this->deletedUsers.clear();
        for (PlayEvent * e : this->events) {
            if (e->type != PlayEvent_Account || e->eventType != Account_Active) {
                continue;
            }
            // Ignore the empty account
            if (e->userID.uid[0] == 0 && e->userID.uid[1] == 0) {
                continue;
            }

            auto key = std::make_pair(e->userID.uid[0], e->userID.uid[1]);
            bool exists = std::find_if(current.begin(), current.end(), [&key](AccountUid u) {
                return (u.uid[0] == key.first && u.uid[1] == key.second);
            }) != current.end();
            if (!exists && this->deletedUsers.insert(key).second) {
                found.push_back(e->userID);
            }
        }
        return found;
    }

    std::vector<TitleID> PlayData::getTitleIDsForUser(AccountUid userID) {
        std::set<TitleID> ids;
        for (const PD_Session & s : this->getPDSessions(0, userID, 0, std::numeric_limits<u64>::max(), false)) {
            ids.insert(this->events[s.index]->titleID);
        }
        return std::vector<TitleID>(ids.begin(), ids.end());
    }

    bool PlayData::isUnassignedEligible(TitleID id) const {
        // First ID used by user applications (Super Mario Odyssey); everything below belongs to the system
        constexpr TitleID firstApplicationID = 0x0100000000010000ULL;
        return (id >= firstApplicationID || this->installedIDs.count(id) > 0);
    }

    void PlayData::writeDiagnostics(const std::vector<AccountUid> & allUsers, const std::map<TitleID, std::string> & names, const std::string & path) {
        // Deleted profiles are listed in the app too but are not real users of the console
        std::vector<AccountUid> users;
        for (AccountUid u : allUsers) {
            if (!this->isDeletedUser(u)) {
                users.push_back(u);
            }
        }

        auto uidStr = [](AccountUid uid) {
            char buf[40];
            snprintf(buf, sizeof(buf), "%016llX%016llX", static_cast<unsigned long long>(uid.uid[0]), static_cast<unsigned long long>(uid.uid[1]));
            return std::string(buf);
        };
        auto dateStr = [](u64 ts) {
            if (ts == 0) {
                return std::string("-");
            }
            std::time_t t = static_cast<std::time_t>(ts);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d", std::gmtime(&t));
            return std::string(buf);
        };
        auto nameOf = [&names](TitleID id) {
            auto it = names.find(id);
            return (it != names.end() ? it->second : std::string("?"));
        };
        auto isCurrent = [&users](AccountUid uid) {
            for (AccountUid u : users) {
                if (u.uid[0] == uid.uid[0] && u.uid[1] == uid.uid[1]) {
                    return true;
                }
            }
            return false;
        };

        std::ofstream out(path, std::ios::trunc);
        if (!out.good()) {
            return;
        }

        const u64 MAX = std::numeric_limits<u64>::max();
        out << "NX Activity Log diagnostics\n";
        out << "Events: " << this->events.size() << "\n";
        out << "Include unassigned: " << (this->includeUnassigned_ ? "yes" : "no") << "\n\n";

        // Accounts found in the events
        std::map<std::string, std::pair<AccountUid, size_t>> accounts;
        for (PlayEvent * e : this->events) {
            if (e->type == PlayEvent_Account && e->eventType == Account_Active) {
                std::string key = uidStr(e->userID);
                auto it = accounts.find(key);
                if (it == accounts.end()) {
                    accounts[key] = std::make_pair(e->userID, 1);
                } else {
                    it->second.second++;
                }
            }
        }
        out << "== Accounts found in account events ==\n";
        for (const auto & kv : accounts) {
            AccountUid uid = kv.second.first;
            std::vector<PD_Session> sessions = this->getPDSessions(0, uid, 0, MAX, false);
            RecentPlayStatistics * rps = this->countPlaytime(sessions, 0, MAX);
            out << kv.first << "  " << (isCurrent(uid) ? "EXISTS" : "NOT ON CONSOLE (deleted?)")
                << "  login events: " << kv.second.second << "  sessions: " << sessions.size() << "  playtime: " << (rps->playtime / 3600) << "h " << ((rps->playtime / 60) % 60) << "m\n";
            delete rps;
        }
        out << "\nCurrent users on console: " << users.size() << "\n";
        for (AccountUid u : users) {
            out << uidStr(u) << (accounts.count(uidStr(u)) ? "" : "  (no login events)") << "\n";
        }

        // Sessions without any account event, per title
        struct Info { size_t sessions = 0; u64 first = 0; u64 last = 0; };
        std::map<TitleID, Info> perTitle;
        AccountUid none = {};
        std::vector<PD_Session> unassigned = this->getPDSessions(0, none, 0, MAX, true);
        for (const PD_Session & s : unassigned) {
            Info & info = perTitle[this->events[s.index]->titleID];
            info.sessions++;
            for (size_t j = s.index; j < s.index + s.num; j++) {
                u64 ts = this->events[j]->clockTimestamp;
                if (this->events[j]->eventType == Applet_Launch && (info.first == 0 || ts < info.first)) {
                    info.first = ts;
                }
                if (ts > info.last) {
                    info.last = ts;
                }
            }
        }
        out << "\n== Titles with sessions that contain no account event ==\n";
        for (const auto & kv : perTitle) {
            std::vector<PD_Session> sessions = this->getPDSessions(kv.first, none, 0, MAX, true);
            RecentPlayStatistics * rps = this->countPlaytime(sessions, 0, MAX);
            char id[24];
            snprintf(id, sizeof(id), "%016llX", static_cast<unsigned long long>(kv.first));
            out << id << "  " << nameOf(kv.first) << "  sessions: " << kv.second.sessions << "  playtime: " << (rps->playtime / 3600) << "h " << ((rps->playtime / 60) % 60) << "m"
                << "  first: " << dateStr(kv.second.first) << "  last: " << dateStr(kv.second.last) << "\n";
            delete rps;
        }
    }

    void PlayData::setIncludeUnassigned(bool b) {
        this->includeUnassigned_ = b;
    }

    bool PlayData::includeUnassigned() const {
        return this->includeUnassigned_;
    }

    void PlayData::setInstalledTitleIDs(const std::vector<TitleID> & ids) {
        this->installedIDs = std::set<TitleID>(ids.begin(), ids.end());
    }

    std::vector<TitleID> PlayData::getUnassignedTitleIDs() {
        // Find the titles that are played with an account (by any account, including deleted ones)
        this->accountTitles.clear();
        std::set<std::pair<u64, u64>> seen;
        for (PlayEvent * e : this->events) {
            if (e->type == PlayEvent_Account && e->eventType == Account_Active && seen.insert(std::make_pair(e->userID.uid[0], e->userID.uid[1])).second) {
                for (const PD_Session & s : this->getPDSessions(0, e->userID, 0, std::numeric_limits<u64>::max(), false)) {
                    this->accountTitles.insert(this->events[s.index]->titleID);
                }
            }
        }

        // Look at every session of every title: the "user" is an all-zero uid which never
        // matches a real account, so only sessions without any account event are returned
        AccountUid none = {};
        std::vector<PD_Session> sessions = this->getPDSessions(0, none, 0, std::numeric_limits<u64>::max(), true);

        this->unassignedTitles.clear();
        for (const PD_Session & session : sessions) {
            this->unassignedTitles.insert(this->events[session.index]->titleID);
        }
        return std::vector<TitleID>(this->unassignedTitles.begin(), this->unassignedTitles.end());
    }

    void PlayData::setProgressCallback(ProgressCallback callback) {
        this->progressCallback = callback;
    }

    int PlayData::getCurrentProgress() const {
        return this->currentProgress.load();
    }

    void PlayData::setCurrentProgress(int current) {
        this->currentProgress.store(current);
    }

    int PlayData::getMaxProgress() const {
        return this->maxProgress.load();
    }

    void PlayData::setMaxProgress(int max) {
        this->maxProgress.store(max);
    }

    std::vector<Title *> PlayData::getMissingTitles(std::vector<Title *> passed) {
        // Iterate over events and summaries, creating title objects for
        // titles not present in passed vector
        std::vector <Title *> missing;

        for (std::pair<u64, std::string> title : this->importTitles) {
            std::vector<Title *>::iterator it = std::find_if(passed.begin(), passed.end(), [title](Title * t) {
                return (title.first == t->titleID());
            });
            if (it == passed.end()) {
                missing.emplace_back(new Title(title.first, title.second));
            }
        }

        return missing;
    }

    std::vector<PlayEvent> PlayData::getPlayEvents(u64 start, u64 end, TitleID titleID, AccountUid userID) {
        std::vector<PlayEvent> events;

        // Get a vector of PD_Sessions -> should only be one if the provided timestamps are correct
        std::vector<PD_Session> sessions = this->getPDSessions(titleID, userID, start, end, this->includeUnassigned_);
        for (size_t i = 0; i < sessions.size(); i++) {
            for (size_t j = sessions[i].index; j < sessions[i].index + sessions[i].num; j++) {
                // Ignore repeated OutFocus events
                if (this->events[j]->eventType == Applet_OutFocus && this->events[j-1]->eventType == Applet_OutFocus) {
                    continue;
                }
                events.emplace_back(*this->events[j]);
            }
        }

        return events;
    }

    std::vector<PlaySession> PlayData::getPlaySessionsForUser(TitleID titleID, AccountUid userID) {
        // Get list of pd_sessions
        struct tm end = Utils::Time::getTmForCurrentTime();
        std::vector<PD_Session> pd_sessions = this->getPDSessions(titleID, userID, 0, std::mktime(&end), this->includeUnassigned_);

        // Create PlaySessions for each PD_Session
        std::vector<PlaySession> sessions;
        for (size_t i = 0; i < pd_sessions.size(); i++) {
            struct PlaySession p;
            p.startTimestamp = 0;
            p.endTimestamp = 0;

            // Get start and end timestamps
            for (size_t j = pd_sessions[i].index; j < pd_sessions[i].index + pd_sessions[i].num; j++) {
                switch (this->events[j]->eventType) {
                    // Ignore account events
                    case Account_Active:
                    case Account_Inactive:
                    case Applet_InFocus:
                        break;

                    // Set start/end timestamps
                    case Applet_Launch:
                        p.startTimestamp = this->events[j]->clockTimestamp;
                        break;

                    case Applet_Exit:
                        p.endTimestamp = this->events[j]->clockTimestamp;
                        break;


                    case Applet_OutFocus:
                        p.endTimestamp = this->events[j]->clockTimestamp;   // In case of a firmware crash there is no Applet_Exit event logged

                        // Move to last out focus (I don't know why the log has multiple)
                        while (j+1 < this->events.size()) {
                            if (this->events[j+1]->eventType == Applet_OutFocus) {
                                j++;
                            } else {
                                break;
                            }
                        }
                        break;
                }
            }

            // Get playtime
            RecentPlayStatistics * rps = countPlaytime(pd_sessions, p.startTimestamp, p.endTimestamp);
            p.playtime = rps->playtime;
            delete rps;

            sessions.emplace_back(p);
        }

        return sessions;
    }

    RecentPlayStatistics * PlayData::getRecentStatisticsForUser(u64 start_ts, u64 end_ts, AccountUid userID) {
        return this->countPlaytime(this->getPDSessions(0, userID, start_ts, end_ts, this->includeUnassigned_), start_ts, end_ts);
    }

    RecentPlayStatistics * PlayData::getRecentStatisticsForTitleAndUser(TitleID titleID, u64 start_ts, u64 end_ts, AccountUid userID) {
        RecentPlayStatistics * s = this->countPlaytime(this->getPDSessions(titleID, userID, start_ts, end_ts, this->includeUnassigned_), start_ts, end_ts);
        s->titleID = titleID;
        return s;
    }

    PlayStatistics * PlayData::getStatisticsForUser(TitleID titleID, AccountUid userID) {
        // For titles launched without an account, and for deleted accounts, the system has no meaningful per-account
        // statistics (it can return unrelated values for them), so these are worked out from the play events only
        const bool eventsOnly = (this->includeUnassigned_ && this->unassignedTitles.count(titleID) > 0) || this->isDeletedUser(userID);

        // Zeroed as the query leaves it untouched on failure
        PdmPlayStatistics tmp = {};
        if (!eventsOnly) {
            pdmqryQueryPlayStatisticsByApplicationIdAndUserAccountId(titleID, userID, false, &tmp);
        }
        // Value-initialised: the dates must be zero when neither pdm nor the summaries have any
        PlayStatistics * stats = new PlayStatistics();
        stats->titleID = titleID;
        if (tmp.first_timestamp_user != 0 && tmp.last_timestamp_user != 0) {
            stats->firstPlayed = tmp.first_timestamp_user;
            stats->lastPlayed = tmp.last_timestamp_user;
        } else {
            auto it = std::find_if(this->summaries.begin(), this->summaries.end(), [titleID](auto s){ return (s->titleID == titleID); });
            if (it != this->summaries.end()) {
                stats->firstPlayed = (*it)->firstPlayed;
                stats->lastPlayed = (*it)->lastPlayed;
            }
        }

        stats->playtime = tmp.playtime / 1000 / 1000 / 1000; //the unit of playtime in PdmPlayStatistics is ns
        stats->launches = tmp.total_launches;

        // The system only tracks statistics per account, so sessions without an account are invisible
        // to the query above. Fill in what is missing using the sessions found in the play events.
        if (eventsOnly) {
            std::vector<PD_Session> sessions = this->getPDSessions(titleID, userID, 0, std::numeric_limits<u64>::max(), true);
            u64 first = 0;
            u64 last = 0;
            // Ignore timestamps that can't be real (before 2000 or in the future)
            const u64 minTs = 946684800;
            const u64 maxTs = static_cast<u64>(std::time(nullptr)) + 86400;
            for (const PD_Session & session : sessions) {
                for (size_t j = session.index; j < session.index + session.num; j++) {
                    u64 ts = this->events[j]->clockTimestamp;
                    if (ts < minTs || ts > maxTs) {
                        continue;
                    }
                    if (this->events[j]->eventType == Applet_Launch && (first == 0 || ts < first)) {
                        first = ts;
                    }
                    if ((this->events[j]->eventType == Applet_Exit || this->events[j]->eventType == Applet_OutFocus) && ts > last) {
                        last = ts;
                    }
                }
            }

            if (first != 0 && (stats->firstPlayed == 0 || first < stats->firstPlayed)) {
                stats->firstPlayed = first;
            }
            if (last > stats->lastPlayed) {
                stats->lastPlayed = last;
            }
            if (stats->launches == 0) {
                stats->launches = sessions.size();
            }
            if (stats->playtime == 0 && !sessions.empty()) {
                RecentPlayStatistics * rps = this->countPlaytime(sessions, 0, std::numeric_limits<u64>::max());
                stats->playtime = rps->playtime;
                delete rps;
            }
        }
        return stats;
    }

    PlayData::~PlayData() {
        for (PlayEvent * event : this->events) {
            delete event;
        }
        for (PlayStatistics * stats : this->summaries) {
            delete stats;
        }
    }
};
