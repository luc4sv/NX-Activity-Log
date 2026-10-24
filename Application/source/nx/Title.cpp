#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include "nx/Title.hpp"

namespace NX {
    static const char * CACHE_DIR = "/config/NX-Activity-Log/cache";

    static std::string cachePath(TitleID id, const char * ext) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%s/%016" PRIX64 ".%s", CACHE_DIR, static_cast<uint64_t>(id), ext);
        return std::string(buf);
    }

    void Title::saveCache(const std::string & name, const u8 * icon, u32 iconSize) {
        // Only written once per title to keep startup fast
        std::error_code ec;
        if (std::filesystem::exists(cachePath(this->titleID_, "name"), ec) && std::filesystem::exists(cachePath(this->titleID_, "icon"), ec)) {
            return;
        }
        std::filesystem::create_directories(CACHE_DIR, ec);

        std::ofstream nameFile(cachePath(this->titleID_, "name"), std::ios::binary | std::ios::trunc);
        if (nameFile.good()) {
            nameFile << name;
        }
        std::ofstream iconFile(cachePath(this->titleID_, "icon"), std::ios::binary | std::ios::trunc);
        if (iconFile.good()) {
            iconFile.write(reinterpret_cast<const char *>(icon), iconSize);
        }
    }

    bool Title::loadCache() {
        std::ifstream nameFile(cachePath(this->titleID_, "name"), std::ios::binary);
        std::ifstream iconFile(cachePath(this->titleID_, "icon"), std::ios::binary | std::ios::ate);
        if (!nameFile.good() || !iconFile.good()) {
            return false;
        }

        std::string name((std::istreambuf_iterator<char>(nameFile)), std::istreambuf_iterator<char>());
        std::streamoff iconSize = iconFile.tellg();
        if (name.empty() || iconSize <= 0) {
            return false;
        }

        this->ptr = static_cast<u8 *>(malloc(iconSize));
        if (this->ptr == nullptr) {
            return false;
        }
        iconFile.seekg(0, std::ios::beg);
        iconFile.read(reinterpret_cast<char *>(this->ptr), iconSize);
        this->size = static_cast<u32>(iconSize);
        this->name_ = name;
        return true;
    }

    void Title::loadNoIcon() {
        std::ifstream file("romfs:/icon/no_icon.jpg", std::ios::binary);
        file.unsetf(std::ios::skipws);
        file.seekg(0, std::ios::end);
        this->size = file.tellg();
        file.seekg(0, std::ios::beg);

        this->ptr = static_cast<u8 *>(malloc(this->size));
        u8 byte;
        size_t pos = 0;
        while (file >> byte && pos < this->size) {
            this->ptr[pos] = byte;
            pos++;
        }
    }

    Title::Title(TitleID titleID, bool installed) {
        this->titleID_ = titleID;
        this->is_installed = installed;
        this->name_ = "";
        this->ptr = nullptr;
        this->size = 0;

        // Get name and icon
        NsApplicationControlData data;
        NacpLanguageEntry * lang = nullptr;
        size_t nacp_size;
        Result rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, this->titleID_, &data, sizeof(NsApplicationControlData), &nacp_size);
        if (R_SUCCEEDED(rc)){
            // Get name
            rc = nacpGetLanguageEntry(&data.nacp, &lang);
            if (R_SUCCEEDED(rc)){
                this->name_ = std::string(lang->name);
            }

            // Get icon
            this->size = nacp_size - sizeof(data.nacp);
            this->ptr = (u8 *) malloc(this->size);
            memcpy(this->ptr, data.icon, this->size);

            // Remember them for when the title is no longer installed
            if (!this->name_.empty() && this->ptr != nullptr && this->size > 0) {
                this->saveCache(this->name_, this->ptr, this->size);
            }
        } else {
            // Not installed (anymore): use what was saved earlier
            this->loadCache();
        }

        // Still nothing: show the ID rather than a blank entry, and a placeholder icon
        if (this->name_.empty()) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%016" PRIX64, static_cast<uint64_t>(this->titleID_));
            this->name_ = std::string(buf);
        }
        if (this->ptr == nullptr) {
            this->loadNoIcon();
        }
    }

    Title::Title(const TitleID titleID, const std::string & name) {
        this->titleID_ = titleID;
        this->is_installed = false;
        this->name_ = name;
        this->ptr = nullptr;
        this->size = 0;

        // Load in image
        this->loadNoIcon();
    }

    TitleID Title::titleID() {
        return this->titleID_;
    }

    bool Title::isInstalled() {
        return this->is_installed;
    }

    std::string Title::name() {
        return this->name_;
    }

    u8 * Title::imgPtr() {
        return this->ptr;
    }

    u32 Title::imgSize() {
        return this->size;
    }

    Title::~Title() {
        if (this->ptr != nullptr) {
            free(this->ptr);
        }
    }
};
