#include "CharacterRules.h"

#include <cstring>

CCharacterRules::DeleteState CCharacterRules::ClassifyDelete(bool hasDeletedAt, int64_t elapsedSec, int graceDays)
{
    if (!hasDeletedAt)
        return DeleteState::Alive;
    return (elapsedSec >= int64_t{ graceDays } * SECONDS_PER_DAY) ? DeleteState::Expired : DeleteState::Pending;
}

int32_t CCharacterRules::GraceLeftSec(int64_t elapsedSec, int graceDays)
{
    const int64_t left = int64_t{ graceDays } * SECONDS_PER_DAY - elapsedSec;
    return left >= 1 ? static_cast<int32_t>(left) : 1;
}

CCharacterRules::DuplicateKey CCharacterRules::ClassifyDuplicate(const char* errorText)
{
    if (errorText == nullptr)
        return DuplicateKey::Unknown;
    if (std::strstr(errorText, "uq_roster_name") != nullptr)
        return DuplicateKey::Name;
    if (std::strstr(errorText, "uq_roster_slot") != nullptr)
        return DuplicateKey::Slot;
    return DuplicateKey::Unknown;
}

const char* CCharacterRules::ResultName(CharLoginResult result)
{
    switch (result)
    {
    case CharLoginResult::OK:          return "ok";
    case CharLoginResult::BAD_TOKEN:   return "bad_token";
    case CharLoginResult::EXPIRED:     return "expired";
    case CharLoginResult::SERVER_DOWN: return "server_down";
    case CharLoginResult::BUSY:        return "busy";
    case CharLoginResult::DB_FAIL:     return "db_fail";
    case CharLoginResult::COUNT:       break;
    }
    return "unknown";
}

const char* CCharacterRules::ResultName(CharCreateResult result)
{
    switch (result)
    {
    case CharCreateResult::OK:         return "ok";
    case CharCreateResult::TAKEN:      return "taken";
    case CharCreateResult::BAD_FORM:   return "bad_form";
    case CharCreateResult::BAD_JOB:    return "bad_job";
    case CharCreateResult::BAD_LOOK:   return "bad_look";
    case CharCreateResult::SLOT_TAKEN: return "slot_taken";
    case CharCreateResult::BUSY:       return "busy";
    case CharCreateResult::DB_FAIL:    return "db_fail";
    case CharCreateResult::COUNT:      break;
    }
    return "unknown";
}

const char* CCharacterRules::ResultName(CharDeleteResult result)
{
    switch (result)
    {
    case CharDeleteResult::OK:       return "ok";
    case CharDeleteResult::NOT_MINE: return "not_mine";
    case CharDeleteResult::IN_GAME:  return "in_game";
    case CharDeleteResult::BUSY:     return "busy";
    case CharDeleteResult::DB_FAIL:  return "db_fail";
    case CharDeleteResult::COUNT:    break;
    }
    return "unknown";
}

const char* CCharacterRules::ResultName(CharRestoreResult result)
{
    switch (result)
    {
    case CharRestoreResult::OK:       return "ok";
    case CharRestoreResult::NOT_MINE: return "not_mine";
    case CharRestoreResult::EXPIRED:  return "expired";
    case CharRestoreResult::BUSY:     return "busy";
    case CharRestoreResult::DB_FAIL:  return "db_fail";
    case CharRestoreResult::COUNT:    break;
    }
    return "unknown";
}

const char* CCharacterRules::ResultName(CharSelectResult result)
{
    switch (result)
    {
    case CharSelectResult::OK:        return "ok";
    case CharSelectResult::NOT_MINE:  return "not_mine";
    case CharSelectResult::DELETED:   return "deleted";
    case CharSelectResult::ZONE_DOWN: return "zone_down";
    case CharSelectResult::ZONE_FULL: return "zone_full";
    case CharSelectResult::BUSY:      return "busy";
    case CharSelectResult::DB_FAIL:   return "db_fail";
    case CharSelectResult::COUNT:     break;
    }
    return "unknown";
}
