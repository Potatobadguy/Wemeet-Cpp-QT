#include "meeting_repository.h"
#include "logger.h"
#include <cstdio>

namespace wemeet {

std::optional<uint64_t> MeetingRepository::create(
        uint64_t creator_id, const std::string& room_id,
        const std::string& title, int max_participants) {

    auto guard = pool_->acquire();
    if (!guard.conn) return std::nullopt;

    char query[1024];
    snprintf(query, sizeof(query),
        "INSERT INTO meetings (room_id, creator_id, title, max_participants) "
        "VALUES ('%s', %lu, '%s', %d)",
        room_id.c_str(), creator_id, title.c_str(), max_participants);

    if (mysql_query(guard.conn, query)) {
        LOG_ERROR("create meeting failed: %s", mysql_error(guard.conn));
        return std::nullopt;
    }

    uint64_t id = mysql_insert_id(guard.conn);
    LOG_INFO("Meeting created: id=%lu, room=%s", id, room_id.c_str());
    return id;
}

bool MeetingRepository::end_meeting(const std::string& room_id) {
    auto guard = pool_->acquire();
    if (!guard.conn) return false;

    char query[512];
    snprintf(query, sizeof(query),
        "UPDATE meetings SET end_time = NOW(), status = 1 WHERE room_id = '%s'",
        room_id.c_str());

    if (mysql_query(guard.conn, query)) {
        LOG_ERROR("end_meeting failed: %s", mysql_error(guard.conn));
        return false;
    }
    return mysql_affected_rows(guard.conn) > 0;
}

std::optional<MeetingRecord> MeetingRepository::find_by_room_id(
        const std::string& room_id) {

    auto guard = pool_->acquire();
    if (!guard.conn) return std::nullopt;

    char query[512];
    snprintf(query, sizeof(query),
        "SELECT id, room_id, creator_id, title, "
        "UNIX_TIMESTAMP(start_time), UNIX_TIMESTAMP(COALESCE(end_time, NOW())), "
        "max_participants, COALESCE(record_path,''), status "
        "FROM meetings WHERE room_id = '%s' LIMIT 1",
        room_id.c_str());

    if (mysql_query(guard.conn, query)) return std::nullopt;

    MYSQL_RES* result = mysql_store_result(guard.conn);
    if (!result) return std::nullopt;

    MYSQL_ROW row = mysql_fetch_row(result);
    if (!row) { mysql_free_result(result); return std::nullopt; }

    MeetingRecord m;
    m.id               = row[0] ? std::stoull(row[0]) : 0;
    m.room_id          = row[1] ? row[1] : "";
    m.creator_id       = row[2] ? std::stoull(row[2]) : 0;
    m.title            = row[3] ? row[3] : "";
    m.start_time       = row[4] ? std::stoll(row[4]) : 0;
    m.end_time         = row[5] ? std::stoll(row[5]) : 0;
    m.max_participants = row[6] ? std::stoi(row[6]) : 50;
    m.record_path      = row[7] ? row[7] : "";
    m.status           = row[8] ? std::stoi(row[8]) : 0;

    mysql_free_result(result);
    return m;
}

std::vector<MeetingRecord> MeetingRepository::get_user_history(
        uint64_t user_id, int offset, int limit) {

    auto guard = pool_->acquire();
    if (!guard.conn) return {};

    char query[1024];
    snprintf(query, sizeof(query),
        "SELECT id, room_id, creator_id, title, "
        "UNIX_TIMESTAMP(start_time), UNIX_TIMESTAMP(COALESCE(end_time, NOW())), "
        "max_participants, COALESCE(record_path,''), status "
        "FROM meetings WHERE creator_id = %lu "
        "ORDER BY start_time DESC LIMIT %d OFFSET %d",
        user_id, limit, offset);

    if (mysql_query(guard.conn, query)) return {};

    MYSQL_RES* res = mysql_store_result(guard.conn);
    if (!res) return {};

    std::vector<MeetingRecord> meetings;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        MeetingRecord m;
        m.id               = row[0] ? std::stoull(row[0]) : 0;
        m.room_id          = row[1] ? row[1] : "";
        m.creator_id       = row[2] ? std::stoull(row[2]) : 0;
        m.title            = row[3] ? row[3] : "";
        m.start_time       = row[4] ? std::stoll(row[4]) : 0;
        m.end_time         = row[5] ? std::stoll(row[5]) : 0;
        m.max_participants = row[6] ? std::stoi(row[6]) : 50;
        m.record_path      = row[7] ? row[7] : "";
        m.status           = row[8] ? std::stoi(row[8]) : 0;
        meetings.push_back(m);
    }
    mysql_free_result(res);
    return meetings;
}

} // namespace wemeet
