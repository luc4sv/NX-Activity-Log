#include <fstream>
#include "nx/User.hpp"

namespace NX {
    User::User(AccountUid ID){
        this->ID_ = ID;

        // Set values in case of an error
        this->username_ = "";
        this->ptr = nullptr;
        this->size = 0;

        // Get name and icon
        AccountProfile profile;
        AccountProfileBase profile_base;

        Result rc = accountGetProfile(&profile, this->ID_);
        if (R_SUCCEEDED(rc)){
            // Get username
            rc = accountProfileGet(&profile, NULL, &profile_base);
            if (R_SUCCEEDED(rc)){
                this->username_ = profile_base.nickname;
            }

            u32 tmp;
            // Get image size and allocate memory
            rc = accountProfileGetImageSize(&profile, &this->size);
            if (R_SUCCEEDED(rc)) {
                this->ptr = (u8 *) malloc(this->size);

                // Get image
                rc = accountProfileLoadImage(&profile, this->ptr, this->size, &tmp);
            }

            // Close profile
            accountProfileClose(&profile);
        }
    }

    User::User(AccountUid ID, const std::string & name) {
        this->ID_ = ID;
        this->username_ = name;
        this->ptr = nullptr;
        this->size = 0;

        // Load placeholder image
        std::ifstream file("romfs:/icon/no_icon.jpg", std::ios::binary);
        file.unsetf(std::ios::skipws);
        file.seekg(0, std::ios::end);
        std::streamoff len = file.tellg();
        if (len > 0) {
            this->ptr = static_cast<u8 *>(malloc(len));
            file.seekg(0, std::ios::beg);
            file.read(reinterpret_cast<char *>(this->ptr), len);
            this->size = static_cast<u32>(len);
        }
    }

    AccountUid User::ID() {
        return this->ID_;
    }

    std::string User::username() {
        return this->username_;
    }

    u8 * User::imgPtr() {
        return this->ptr;
    }

    u32 User::imgSize() {
        return this->size;
    }

    User::~User() {
        // Free memory used by image texture
        if (this->ptr != nullptr) {
            free(this->ptr);
        }
    }
};