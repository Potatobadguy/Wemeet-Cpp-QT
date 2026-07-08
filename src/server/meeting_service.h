#pragma once
#include "connection_pool.h"
#include "meeting_repository.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <shared_mutex>
#include <memory>
#include <cstdint>

namespace wemeet {

/**
 * @brief 会议服务 — 房间状态管理
 */
struct RoomParticipant {
    uint64_t user_id;
    std::string nickname;
    bool audio_on = true;
    bool video_on = true;
    bool is_host  = false;
};

struct RoomInfo {
    std::string room_id;
    std::string title;
    uint64_t    host_id;
    int         max_participants;
    std::vector<RoomParticipant> participants;
    int64_t     created_at;
};

class MeetingService {
public:
    explicit MeetingService(ConnectionPool* db_pool)
        : meeting_repo_(db_pool) {}

    // ── 会议操作 ─────────────────────────────────────────
    struct CreateResult {
        bool   success;
        std::string room_id;
        std::string error;
    };

    CreateResult create_meeting(uint64_t host_id,
                                 const std::string& title,
                                 int max_participants = 50);

    struct JoinResult {
        bool   success;
        std::string error;
        std::vector<RoomParticipant> participants;
    };

    JoinResult join_meeting(uint64_t user_id,
                             const std::string& nickname,
                             const std::string& room_id);

    void leave_meeting(uint64_t user_id, const std::string& room_id);
    void end_meeting(const std::string& room_id, uint64_t host_id);

    // ── 查询 ─────────────────────────────────────────────
    std::shared_ptr<RoomInfo> get_room(const std::string& room_id);

    // 广播给房间内其他人
    void broadcast_to_room(const std::string& room_id, uint64_t exclude_user,
                            const std::function<void(uint64_t)>& sender);

private:
    std::string generate_room_id();

    MeetingRepository meeting_repo_;

    mutable std::shared_mutex rooms_mutex_;
    std::unordered_map<std::string, std::shared_ptr<RoomInfo>> rooms_;
};

} // namespace wemeet
